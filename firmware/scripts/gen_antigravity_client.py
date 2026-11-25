"""Generate firmware/src/antigravity_client.h from the local (untracked) client config.

Antigravity's Google OAuth client id/secret are treated as secrets by GitHub push
protection, so they never live in the repository. The CLI keeps them in
cli/antigravity-client.json (gitignored) or in ANTIGRAVITY_CLIENT_ID /
ANTIGRAVITY_CLIENT_SECRET. The firmware needs the same pair compiled in, so the
build reads it from the same place and writes a header.

Missing configuration is not a hard error: the header gets empty strings and the
device reports "Antigravity 客户端未配置" instead of pretending to have a client.
"""

from __future__ import annotations

import json
import os
from pathlib import Path

HEADER_NAME = "antigravity_client.h"

HEADER_TEMPLATE = """// 由 firmware/scripts/gen_antigravity_client.py 生成，不要手改。
// 值来自 cli/antigravity-client.json（gitignore）或 ANTIGRAVITY_CLIENT_ID /
// ANTIGRAVITY_CLIENT_SECRET 环境变量；不写进仓库。
#pragma once

namespace antigravity_client {{
constexpr char kClientId[] = "{client_id}";
constexpr char kClientSecret[] = "{client_secret}";
}}  // namespace antigravity_client
"""


def _escape(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"')


def resolve_client(repo_root: Path) -> tuple[str, str, str]:
    """Return (client_id, client_secret, source)."""
    env_id = os.environ.get("ANTIGRAVITY_CLIENT_ID", "").strip()
    env_secret = os.environ.get("ANTIGRAVITY_CLIENT_SECRET", "").strip()
    if env_id and env_secret:
        return env_id, env_secret, "环境变量"
    config_path = repo_root / "cli" / "antigravity-client.json"
    if config_path.is_file():
        try:
            data = json.loads(config_path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            print(f"读取 {config_path} 失败：{error}")
            return "", "", "无"
        client_id = str(data.get("clientId", "")).strip()
        client_secret = str(data.get("clientSecret", "")).strip()
        if client_id and client_secret:
            return client_id, client_secret, str(config_path)
    return "", "", "无"


def sync(project_dir: Path) -> tuple[bool, str]:
    """Write firmware/src/antigravity_client.h. Returns (configured, source)."""
    repo_root = project_dir.parent
    src_dir = project_dir / "src"
    src_dir.mkdir(parents=True, exist_ok=True)
    client_id, client_secret, source = resolve_client(repo_root)
    header = HEADER_TEMPLATE.format(
        client_id=_escape(client_id), client_secret=_escape(client_secret)
    )
    target = src_dir / HEADER_NAME
    # 只有内容变化时才写，避免每次构建都触发重编译。
    if not target.is_file() or target.read_text(encoding="utf-8") != header:
        target.write_text(header, encoding="utf-8")
    return bool(client_id and client_secret), source


if __name__ == "__main__":
    import sys

    project = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
    configured, source = sync(project)
    print(f"Antigravity 客户端：{'已配置' if configured else '未配置'}（{source}）")
