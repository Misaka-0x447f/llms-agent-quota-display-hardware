import { readFileSync } from "node:fs";
import { homedir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

import { CliError } from "./errors.js";

// Antigravity 用的是 Google 的 installed-app 客户端（公开客户端，但值本身仍被
// GitHub push protection 当成 secret）。为了不把 client id / secret 提交进仓库，
// 这里在运行时从下面两处之一读取：
//
//   1. 环境变量 ANTIGRAVITY_CLIENT_ID / ANTIGRAVITY_CLIENT_SECRET
//   2. 本地配置文件 cli/antigravity-client.json（.gitignore 里）
//
// 文件格式见 antigravity-client.example.json（只有占位符）。
//
// 这份值只用于把 Google 账号的授权结果换成令牌；真正的凭据（refresh token）在
// 设备 NVS 里，不写进电脑文件。
export const ANTIGRAVITY_CLIENT_FILE = fileURLToPath(
  new URL("../antigravity-client.json", import.meta.url),
);

export interface GoogleClient {
  id: string;
  secret: string;
}

function fromEnvironment(): GoogleClient | undefined {
  const id = process.env.ANTIGRAVITY_CLIENT_ID?.trim();
  const secret = process.env.ANTIGRAVITY_CLIENT_SECRET?.trim();
  if (id && secret) return { id, secret };
  return undefined;
}

function fromFile(path: string): GoogleClient | undefined {
  let raw: string;
  try {
    raw = readFileSync(path, "utf8");
  } catch {
    return undefined;
  }
  let parsed: unknown;
  try {
    parsed = JSON.parse(raw);
  } catch {
    throw new CliError(`${path} 不是合法 JSON`);
  }
  if (parsed === null || typeof parsed !== "object") return undefined;
  const record = parsed as Record<string, unknown>;
  const id = typeof record.clientId === "string" ? record.clientId.trim() : "";
  const secret = typeof record.clientSecret === "string" ? record.clientSecret.trim() : "";
  if (!id || !secret) return undefined;
  return { id, secret };
}

// 找不到时给出一条可执行的提示，而不是在授权到一半才失败。
export function loadGoogleClient(): GoogleClient {
  const fromEnv = fromEnvironment();
  if (fromEnv) return fromEnv;
  const fromDisk = fromFile(ANTIGRAVITY_CLIENT_FILE);
  if (fromDisk) return fromDisk;
  throw new CliError(
    "缺少 Antigravity 的 Google 客户端配置。请任选一种方式：\n" +
      `  1) 复制 cli/antigravity-client.example.json 到 ${ANTIGRAVITY_CLIENT_FILE} 并填入 clientId/clientSecret；\n` +
      "  2) 设置环境变量 ANTIGRAVITY_CLIENT_ID 与 ANTIGRAVITY_CLIENT_SECRET。\n" +
      `参考目录：${join(homedir(), "git", "llms-agent-quota-display-hardware")}`,
  );
}
