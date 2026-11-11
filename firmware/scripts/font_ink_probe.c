// 量出屏幕上实际用到的字形在基线上下各占几行，用来定 1px loading 线的位置。
//
// 这套量法是「界面上不能压字」这条硬性要求的证据链：main.cpp 里的 static_assert
// 用到的 kTextMaxTopRows / kTextMaxBottomRows 就是这里的输出。
//
// 用法（在 firmware/ 目录下，u8g2 已在 .pio/libdeps 里）：
//   SRC=.pio/libdeps/esp32c3_supermini/U8g2/src/clib
//   cc -I"$SRC" -o /tmp/font_ink_probe scripts/font_ink_probe.c $(ls "$SRC"/*.c)
//   /tmp/font_ink_probe
//
// 只读、离线，不需要接设备。输出形如：
//   [6d]           baseline=20  ink  9..21  top=-11 bottom=+1
//   deepseek       baseline=36  ink 29..37  top=-7 bottom=+1

#include <stdio.h>
#include <string.h>

#include "u8g2.h"

static uint8_t buffer[1024];

static uint8_t discardingByte(u8x8_t *u8x8, uint8_t msg, uint8_t argInt, void *argPtr) {
  (void)u8x8;
  (void)msg;
  (void)argInt;
  (void)argPtr;
  return 1;
}

// 把这些字符串按 firmware 实际用的字体与基线画进缓冲，回读 ink 的行范围。
static void measure(const uint8_t *font, const char *text, int baseline) {
  u8g2_t display;
  memset(&display, 0, sizeof(display));
  u8g2_Setup_ssd1315_i2c_128x64_noname_f(&display, U8G2_R0, discardingByte, discardingByte);
  u8g2_SetFont(&display, font);
  u8g2_ClearBuffer(&display);
  u8g2_DrawStr(&display, 0, baseline, text);

  const uint8_t *pixels = u8g2_GetBufferPtr(&display);
  int firstRow = 64;
  int lastRow = -1;
  for (int row = 0; row < 64; ++row) {
    for (int column = 0; column < 128; ++column) {
      if ((pixels[(row / 8) * 128 + column] & (1u << (row % 8))) == 0) continue;
      if (row < firstRow) firstRow = row;
      if (row > lastRow) lastRow = row;
    }
  }
  printf("%-14s baseline=%2d  ink %2d..%2d  top=%2d bottom=%+d\n", text, baseline, firstRow,
         lastRow, firstRow - baseline, lastRow - baseline);
}

int main(void) {
  // 第一行：Claude 7 天窗口（7×14）
  measure(u8g2_font_7x14_tf, "[6d]", 20);
  measure(u8g2_font_7x14_tf, "D-4h", 20);
  measure(u8g2_font_7x14_tf, "27%", 20);
  // 第二、三行：平台名（6×12）与金额（7×14 / 5×8）
  measure(u8g2_font_6x12_tf, "deepseek", 36);
  measure(u8g2_font_6x12_tf, "openrouter", 52);
  measure(u8g2_font_6x12_tf, "<10m", 36);
  measure(u8g2_font_7x14_tf, "12.34", 36);
  measure(u8g2_font_5x8_tf, "12.34+1.5", 36);
  return 0;
}
