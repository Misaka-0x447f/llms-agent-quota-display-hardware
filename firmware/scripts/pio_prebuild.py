"""PlatformIO pre-build hook for keeping the generated UI glyphs in sync."""

Import("env")  # type: ignore[name-defined]  # Provided by PlatformIO/SCons.

from pathlib import Path
import sys

scripts_dir = Path(env.subst("$PROJECT_DIR")) / "scripts"  # type: ignore[name-defined]
sys.path.insert(0, str(scripts_dir))

from sync_ui_glyphs import FontError, sync  # noqa: E402

try:
    count, bitmap_bytes = sync(scripts_dir.parent, check=False)
except FontError as error:
    print(f"字形同步失败：{error}", file=sys.stderr)
    env.Exit(1)  # type: ignore[name-defined]

print(f"已同步 {count} 个中文字形（位图 {bitmap_bytes} B）")
