#!/usr/bin/env python3
"""Extract the Chinese glyphs used by the firmware from U8g2's WQY12 font."""

from __future__ import annotations

import argparse
import ast
from pathlib import Path
import re
import sys


FONT_SYMBOL = "u8g2_font_wqy12_t_gb2312"
FONT_DATA_HEADER_SIZE = 23
GLYPH_WIDTH = 12
GLYPH_HEIGHT = 12
GLYPH_BASELINE = 11
STRING_LITERAL = re.compile(r'"(?:\\.|[^"\\])*"', re.DOTALL)
CJK = re.compile(r"[\u3400-\u9fff]")


class FontError(RuntimeError):
    pass


class BitReader:
    def __init__(self, data: bytes, byte_offset: int) -> None:
        self.data = data
        self.bit_offset = byte_offset * 8

    def unsigned(self, count: int) -> int:
        value = 0
        for shift in range(count):
            byte = self.data[self.bit_offset // 8]
            value |= ((byte >> (self.bit_offset % 8)) & 1) << shift
            self.bit_offset += 1
        return value

    def signed(self, count: int) -> int:
        return self.unsigned(count) - (1 << (count - 1))


class U8g2Font:
    def __init__(self, data: bytes) -> None:
        if len(data) < FONT_DATA_HEADER_SIZE:
            raise FontError("U8g2 字体数据不完整")
        self.data = data
        self.bits_per_0 = data[2]
        self.bits_per_1 = data[3]
        self.bits_per_width = data[4]
        self.bits_per_height = data[5]
        self.bits_per_x = data[6]
        self.bits_per_y = data[7]
        self.bits_per_delta_x = data[8]
        self.unicode_start = FONT_DATA_HEADER_SIZE + self._word(21)

    def _word(self, offset: int) -> int:
        return (self.data[offset] << 8) | self.data[offset + 1]

    def _glyph_data_offset(self, codepoint: int) -> int:
        lookup = self.unicode_start
        glyph = self.unicode_start
        while True:
            glyph += self._word(lookup)
            last_codepoint = self._word(lookup + 2)
            lookup += 4
            if last_codepoint >= codepoint:
                break
            if lookup + 3 >= len(self.data):
                raise FontError(f"U+{codepoint:04X} 不在字体索引中")

        while glyph + 2 < len(self.data):
            current = self._word(glyph)
            if current == 0:
                break
            if current == codepoint:
                return glyph + 3
            glyph += self.data[glyph + 2]
        raise FontError(f"U+{codepoint:04X} 不在 {FONT_SYMBOL} 中")

    def render_xbm(self, codepoint: int) -> bytes:
        reader = BitReader(self.data, self._glyph_data_offset(codepoint))
        width = reader.unsigned(self.bits_per_width)
        height = reader.unsigned(self.bits_per_height)
        offset_x = reader.signed(self.bits_per_x)
        offset_y = reader.signed(self.bits_per_y)
        reader.signed(self.bits_per_delta_x)

        local_pixels: set[tuple[int, int]] = set()
        x = 0
        y = 0

        def consume_run(length: int, foreground: bool) -> None:
            nonlocal x, y
            remaining = length
            while True:
                to_edge = width - x
                current = min(remaining, to_edge)
                if foreground:
                    for pixel_x in range(x, x + current):
                        local_pixels.add((pixel_x, y))
                if remaining < to_edge:
                    x += remaining
                    return
                remaining -= to_edge
                x = 0
                y += 1
                if remaining == 0:
                    return

        while y < height:
            zeroes = reader.unsigned(self.bits_per_0)
            ones = reader.unsigned(self.bits_per_1)
            while True:
                consume_run(zeroes, False)
                consume_run(ones, True)
                if reader.unsigned(1) == 0:
                    break

        top = GLYPH_BASELINE - height - offset_y
        packed = bytearray((GLYPH_WIDTH + 7) // 8 * GLYPH_HEIGHT)
        for local_x, local_y in local_pixels:
            cell_x = offset_x + local_x
            cell_y = top + local_y
            if not (0 <= cell_x < GLYPH_WIDTH and 0 <= cell_y < GLYPH_HEIGHT):
                raise FontError(
                    f"U+{codepoint:04X} 超出 {GLYPH_WIDTH}x{GLYPH_HEIGHT} 字框"
                )
            packed[cell_y * 2 + cell_x // 8] |= 1 << (cell_x % 8)
        return bytes(packed)


def find_u8g2_fonts_source(firmware_dir: Path) -> Path:
    matches = sorted(
        firmware_dir.glob(".pio/libdeps/*/U8g2/src/clib/u8g2_fonts.c")
    )
    if not matches:
        raise FontError(
            "找不到 U8g2 字体源；请先在 firmware 目录执行 `pio pkg install`"
        )
    return matches[0]


def load_font_data(source: Path) -> bytes:
    text = source.read_text(encoding="latin1")
    declaration = f"const uint8_t {FONT_SYMBOL}["
    start = text.find(declaration)
    if start < 0:
        raise FontError(f"{source} 中找不到 {FONT_SYMBOL}")
    start = text.find("=", start) + 1
    end = text.find("\n#endif", start)
    if end < 0:
        raise FontError(f"无法确定 {FONT_SYMBOL} 的结尾")
    fragments = STRING_LITERAL.findall(text[start:end])
    try:
        return b"".join(ast.literal_eval("b" + item) for item in fragments)
    except (SyntaxError, ValueError) as error:
        raise FontError(f"无法解析 {FONT_SYMBOL}：{error}") from error


def collect_chinese_characters(source_files: list[Path]) -> list[str]:
    characters: set[str] = set()
    for source in source_files:
        text = source.read_text(encoding="utf-8")
        for literal in STRING_LITERAL.findall(text):
            characters.update(CJK.findall(literal))
    return sorted(characters, key=ord)


def format_bitmap(bitmap: bytes) -> str:
    rows = []
    for offset in range(0, len(bitmap), 12):
        values = ", ".join(f"0x{value:02x}" for value in bitmap[offset : offset + 12])
        rows.append(f"        {values},")
    return "\n".join(rows)


def make_header(font: U8g2Font, characters: list[str], sources: list[Path]) -> str:
    source_list = ", ".join(path.name for path in sources)
    glyphs = []
    for character in characters:
        codepoint = ord(character)
        bitmap = format_bitmap(font.render_xbm(codepoint))
        glyphs.append(
            f"    // {character} U+{codepoint:04X}\n"
            f"    {{0x{codepoint:04x}, {{\n{bitmap}\n    }}}},"
        )

    return f"""#pragma once

// Generated by scripts/sync_ui_glyphs.py from {source_list}.
// Source font: U8g2 {FONT_SYMBOL}. Do not edit this file manually.

#include <Arduino.h>

namespace ui_font {{

constexpr uint8_t kGlyphWidth = {GLYPH_WIDTH};
constexpr uint8_t kGlyphHeight = {GLYPH_HEIGHT};

struct Glyph {{
  uint16_t codepoint;
  uint8_t bitmap[24];
}};

static const Glyph kGlyphs[] PROGMEM = {{
{chr(10).join(glyphs)}
}};

inline const uint8_t* find(uint16_t codepoint) {{
  for (const auto& glyph : kGlyphs) {{
    if (glyph.codepoint == codepoint) return glyph.bitmap;
  }}
  return nullptr;
}}

}}  // namespace ui_font
"""


def sync(firmware_dir: Path, check: bool) -> tuple[int, int]:
    source_files = [firmware_dir / "src" / "main.cpp"]
    characters = collect_chinese_characters(source_files)
    if not characters:
        raise FontError("固件源码中没有找到中文字符串")

    font_source = find_u8g2_fonts_source(firmware_dir)
    font = U8g2Font(load_font_data(font_source))
    generated = make_header(font, characters, source_files)
    output = firmware_dir / "src" / "ui_glyphs.h"
    current = output.read_text(encoding="utf-8") if output.exists() else ""

    if check:
        if current != generated:
            raise FontError("ui_glyphs.h 未同步；请运行 scripts/sync_ui_glyphs.py")
    elif current != generated:
        output.write_text(generated, encoding="utf-8")

    return len(characters), len(characters) * 24


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check", action="store_true", help="只检查生成文件，不进行修改"
    )
    args = parser.parse_args()
    firmware_dir = Path(__file__).resolve().parents[1]
    try:
        count, bitmap_bytes = sync(firmware_dir, args.check)
    except FontError as error:
        print(f"字形同步失败：{error}", file=sys.stderr)
        return 1
    action = "已校验" if args.check else "已同步"
    print(f"{action} {count} 个中文字形（位图 {bitmap_bytes} B）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
