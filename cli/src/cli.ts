#!/usr/bin/env -S node --import tsx

import { parseArgs } from "node:util";

import open from "open";

import { AbortError, CliError, DeviceError, OAuthError } from "./errors.js";
import { claudeBrowserLogin } from "./claude-oauth.js";
import { browserLogin } from "./oauth.js";
import { readHidden } from "./prompt.js";
import { requestDevice, resolvePort } from "./serial-link.js";

type Command = "update" | "login" | "list" | "fetchNow" | "erase";

const HELP = `ESP32-C3 额度屏配置工具

用法：
  codex-quota-device update    [选项]
  codex-quota-device login     [选项]
  codex-quota-device list
  codex-quota-device fetchNow
  codex-quota-device erase

update 选项（至少指定一个；未指定的参数保持设备原值不变）：
  --ssid SSID           更新 Wi-Fi 名称（改 SSID 而未给 --password 时会交互提示密码）
  --password PASSWORD   更新 Wi-Fi 密码
  --deepseek-key KEY     更新 DeepSeek 余额 key；可重复或用逗号分隔，最多 4 个
  --openrouter-key KEY   更新 OpenRouter 余额 key；可重复或用逗号分隔，最多 4 个
  --utc-offset +HH:MM   更新时区偏移
  --rows "chatgpt:7d,deepseek,openrouter"
                        配置屏幕显示哪些行、顺序、窗口和刷新间隔（最多 3 行）：
                        来源可以是 chatgpt / claude / deepseek / openrouter，
                        chatgpt、claude 可加 :7d 或 :5h；余额来源不写窗口。
                        每行可用 @间隔 覆盖默认节奏（10s～24h，如 @30s / @5m，
                        默认窗口 10m、余额 50s）。没列出的来源不再抓取，
                        但凭据保留在设备里。
  --port PORT           手动指定串口；默认自动发现 Espressif 设备

login 选项：
  --provider NAME       claude（默认）/ chatgpt（别名 codex）
  --port PORT          手动指定串口；默认自动发现 Espressif 设备
  --no-browser          不自动打开浏览器

list / fetchNow / erase 也接受可选的 --port PORT。
`;

export function parseUtcOffset(value: string): number {
  const match = /^([+-]?)(\d{1,2}):(\d{2})$/.exec(value);
  if (!match) throw new CliError("时区格式应为 +08:00 或 -05:30");
  const sign = match[1] === "-" ? -1 : 1;
  const hours = Number(match[2]);
  const minutes = Number(match[3]);
  const result = sign * (hours * 60 + minutes);
  if (hours > 14 || minutes > 59 || result < -14 * 60 || result > 14 * 60) {
    throw new CliError("时区偏移超出有效范围");
  }
  return result;
}

function requireString(value: string | undefined, option: string): string {
  if (!value?.trim()) throw new CliError(`缺少必要参数 ${option}`);
  return value;
}

export const MAX_API_KEYS = 4;
const MAX_API_KEY_LENGTH = 128;

export const MAX_ROWS = 3;
const ROW_SOURCES = ["chatgpt", "claude", "deepseek", "openrouter"];
const WINDOW_SOURCES = ["chatgpt", "claude"];
const ROW_WINDOWS = ["7d", "5h"];
const MIN_REFRESH_SECONDS = 10;
const MAX_REFRESH_SECONDS = 24 * 60 * 60;

// 每行刷新间隔：整数 + s/m/h，例如 30s、10m、1h；范围 10 秒～24 小时。
export function parseRefreshInterval(value: string): number {
  const match = /^(\d+)([smh])$/.exec(value);
  if (!match) throw new CliError(`刷新间隔格式应为 30s / 10m / 1h：${value}`);
  const scale = match[2] === "s" ? 1 : match[2] === "m" ? 60 : 3600;
  const seconds = Number(match[1]) * scale;
  if (seconds < MIN_REFRESH_SECONDS || seconds > MAX_REFRESH_SECONDS) {
    throw new CliError(`刷新间隔需在 10 秒到 24 小时之间：${value}`);
  }
  return seconds;
}

// 与固件 row_config.h 同一套语法：逗号分隔、顺序即行序、chatgpt/claude 可带窗口。
// 在电脑侧先校验，避免把明显写错的配置下发到设备。
export function parseRows(value: string): string {
  const tokens = value.split(",").map((token) => token.trim());
  if (tokens.some((token) => token.length === 0)) {
    throw new CliError("--rows 不能有空项，例如 chatgpt:7d,deepseek,openrouter");
  }
  if (tokens.length > MAX_ROWS) throw new CliError(`--rows 最多 ${MAX_ROWS} 行`);
  const normalized = tokens.map((token) => {
    const atIndex = token.indexOf("@");
    const base = atIndex < 0 ? token : token.slice(0, atIndex);
    const interval = atIndex < 0 ? undefined : token.slice(atIndex + 1);
    if (interval !== undefined) parseRefreshInterval(interval);
    const parts = base.split(":");
    if (parts.length > 2) throw new CliError(`--rows 项格式错误：${token}`);
    const [name, window] = parts;
    if (!ROW_SOURCES.includes(name)) {
      throw new CliError(`--rows 里有未知来源：${name}`);
    }
    if (window !== undefined) {
      if (!WINDOW_SOURCES.includes(name)) {
        throw new CliError(`${name} 不接受窗口后缀`);
      }
      if (!ROW_WINDOWS.includes(window)) {
        throw new CliError(`窗口只接受 7d 或 5h：${window}`);
      }
    }
    const description = window === undefined ? name : `${name}:${window}`;
    return interval === undefined ? description : `${description}@${interval}`;
  });
  return normalized.join(",");
}

// 同一平台最多 4 个 key：可重复传入，也可用逗号或换行分隔；重复 key 去重。
export function parseApiKeys(values: string[] | undefined, option: string): string[] {
  if (values === undefined) return [];
  const keys = values
    .flatMap((value) => value.split(/[\n\r,]+/))
    .map((value) => value.trim())
    .filter((value) => value.length > 0);
  const unique = [...new Set(keys)];
  if (unique.length === 0) throw new CliError(`${option} 不能为空`);
  if (unique.length > MAX_API_KEYS) {
    throw new CliError(`${option} 最多支持 ${MAX_API_KEYS} 个 key`);
  }
  if (unique.some((key) => key.length > MAX_API_KEY_LENGTH)) {
    throw new CliError(`${option} 的单个 key 不能超过 ${MAX_API_KEY_LENGTH} 个字符`);
  }
  return unique;
}

function redactSensitiveText(text: string): string {
  return text
    .replace(
      /((?:access_token|refresh_token|id_token|wifi_password|code_verifier|code)=)[^\s&"']+/gi,
      "$1[已隐藏]",
    )
    .replace(/sk-ant-[A-Za-z0-9_-]+/g, "[Claude 凭据已隐藏]")
    .replace(/eyJ[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+/g, "[JWT 已隐藏]");
}

function printError(error: unknown): void {
  const primary = error instanceof Error ? error.message : String(error);
  console.error(`错误：${redactSensitiveText(primary)}`);

  const seen = new Set<unknown>();
  let current: unknown = error;
  const details: string[] = [];
  while (current instanceof Error && !seen.has(current)) {
    seen.add(current);
    if (current.stack) details.push(redactSensitiveText(current.stack));
    current = current.cause;
  }
  if (details.length > 0) console.error(`详细错误：\n${details.join("\n由以下原因引起：\n")}`);
}

async function updateDevice(args: string[]): Promise<void> {
  const { values } = parseArgs({
    args,
    strict: true,
    allowPositionals: false,
    options: {
      port: { type: "string" },
      ssid: { type: "string" },
      password: { type: "string" },
      "deepseek-key": { type: "string", multiple: true },
      "openrouter-key": { type: "string", multiple: true },
      "utc-offset": { type: "string" },
      rows: { type: "string" },
    },
  });
  const payload: Record<string, unknown> = { cmd: "configure" };

  // 两个平台可以独立更新；同一平台的多个 key 以换行分隔下放设备。
  const deepseekKeys = parseApiKeys(values["deepseek-key"], "--deepseek-key");
  const openrouterKeys = parseApiKeys(values["openrouter-key"], "--openrouter-key");
  if (deepseekKeys.length > 0) payload.deepseek_key = deepseekKeys.join("\n");
  if (openrouterKeys.length > 0) payload.openrouter_key = openrouterKeys.join("\n");

  // 纯参数校验全部放在解析串口之前：写错 rows/时区/密钥时应先看到参数错误，
  // 而不是「未发现串口」。
  if (values["utc-offset"] !== undefined) {
    payload.utc_offset_minutes = parseUtcOffset(values["utc-offset"]);
  }

  if (values.rows !== undefined) {
    payload.rows = parseRows(values.rows);
  }

  // Wi-Fi：给了 --ssid 或 --password 才更新；改 SSID 而未给密码时交互补齐。
  if (values.ssid !== undefined || values.password !== undefined) {
    if (values.ssid !== undefined) {
      payload.wifi_ssid = requireString(values.ssid, "--ssid");
    }
    payload.wifi_password =
      values.password !== undefined
        ? values.password
        : await readHidden("Wi-Fi 密码：", "--password");
  }

  const updatedFields = Object.keys(payload).filter((key) => key !== "cmd");
  if (updatedFields.length === 0) {
    throw new CliError(
      "请至少指定一个要更新的参数：--ssid / --password / --deepseek-key / --openrouter-key / --utc-offset / --rows",
    );
  }

  const port = await resolvePort(values.port);

  console.log("正在写入设备配置（不会触发 OAuth）……");
  await requestDevice(port, payload, 20_000);
  console.log(`更新完成（${updatedFields.join("、")}）`);
}

async function loginDevice(args: string[]): Promise<void> {
  const { values } = parseArgs({
    args,
    strict: true,
    allowPositionals: false,
    options: {
      port: { type: "string" },
      "no-browser": { type: "boolean", default: false },
      provider: { type: "string", default: "claude" },
    },
  });
  if (!["claude", "codex", "chatgpt"].includes(values.provider)) {
    throw new CliError("--provider 只接受 claude 或 chatgpt（兼容别名 codex）");
  }
  const isClaude = values.provider === "claude";
  const port = await resolvePort(values.port);
  if (isClaude) {
    const status = await requestDevice(port, { cmd: "status" }, 20_000);
    if (status.usage_provider !== "claude" && status.claude_supported !== true) {
      throw new DeviceError("请先烧录支持 Claude 的固件，再运行 login");
    }
  }
  const login = isClaude ? claudeBrowserLogin : browserLogin;
  const tokens = await login(async (url) => {
    console.log(`正在打开 OAuth 授权页：${url}`);
    if (!values["no-browser"]) {
      try {
        await open(url);
      } catch (error) {
        const detail = error instanceof Error ? error.message : String(error);
        console.error(`无法自动打开浏览器：${detail}`);
      }
    }
    console.log("等待浏览器登录完成并回调 CLI……");
  });
  const loginPayload = {
    cmd: isClaude ? "claude_login" : "login",
    access_token: tokens.accessToken,
    refresh_token: tokens.refreshToken,
    ...(isClaude ? {} : { account_id: tokens.accountId }),
    expires_at: tokens.expiresAt,
  };
  const redactedAuthenticationBody = Object.fromEntries(
    tokens.diagnostics.fields.map(({ name, displayValue }) => [name, displayValue]),
  );
  const serialRequestBytes = Buffer.byteLength(
    `${JSON.stringify({ request_id: "000000000000", ...loginPayload })}\n`,
    "utf8",
  );
  console.log(
    `OAuth 鉴权响应体（敏感字段已隐藏；${tokens.diagnostics.bodyByteSource} ` +
      `${tokens.diagnostics.bodyBytes} B）：`,
  );
  console.log(JSON.stringify(redactedAuthenticationBody, null, 2));
  console.log(
    "字段大小（JSON 序列化后）：" +
      tokens.diagnostics.fields
        .map(({ name, serializedBytes }) => `${name}=${serializedBytes} B`)
        .join("；"),
  );
  console.log(`将下放的串口 login JSON：${serialRequestBytes} B（含 request_id 和换行符）`);
  console.log(
    "OAuth 登录成功，正在下放凭据（token 不会写入电脑文件）……",
  );
  const response = await requestDevice(
    port,
    loginPayload,
    20_000,
  );
  console.log(`${isClaude ? "Claude" : "ChatGPT"} 登录凭据已写入设备：${String(response.account_id ?? "已更新")}`);
}

async function simpleCommand(command: Exclude<Command, "update" | "login">, args: string[]) {
  const { values } = parseArgs({
    args,
    strict: true,
    allowPositionals: false,
    options: { port: { type: "string" } },
  });
  const port = await resolvePort(values.port);
  // CLI 名称可以保持面向用户的语义，串口协议则继续使用固件已有的命令名。
  const protocolCommand =
    command === "list" ? "status" : command === "fetchNow" ? "refresh" : command;
  const response = await requestDevice(port, { cmd: protocolCommand }, 20_000);
  const { request_id: _requestId, ok: _ok, ...safe } = response;
  console.log(JSON.stringify(safe, null, 2));
}

export async function run(argv = process.argv.slice(2)): Promise<void> {
  const [rawCommand, ...args] = argv;
  if (!rawCommand || rawCommand === "--help" || rawCommand === "-h") {
    console.log(HELP);
    return;
  }
  if (!["update", "login", "list", "fetchNow", "erase"].includes(rawCommand)) {
    throw new CliError(`未知命令：${rawCommand}\n\n${HELP}`);
  }
  const command = rawCommand as Command;
  if (command === "update") await updateDevice(args);
  else if (command === "login") await loginDevice(args);
  else await simpleCommand(command, args);
}

if (import.meta.url === `file://${process.argv[1]}`) {
  run().catch((error: unknown) => {
    if (error instanceof AbortError) {
      console.error("\n已取消。");
      process.exitCode = 130;
      return;
    }
    if (
      error instanceof OAuthError ||
      error instanceof DeviceError ||
      error instanceof CliError
    ) {
      printError(error);
      process.exitCode = 1;
      return;
    }
    printError(error);
    process.exitCode = 1;
  });
}
