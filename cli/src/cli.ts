#!/usr/bin/env -S node --import tsx

import { parseArgs } from "node:util";

import open from "open";

import { AbortError, CliError, DeviceError, OAuthError } from "./errors.js";
import { browserLogin } from "./oauth.js";
import { readHidden } from "./prompt.js";
import { requestDevice, resolvePort } from "./serial-link.js";

type Command = "set" | "login" | "list" | "fetchNow" | "erase";

const HELP = `ESP32-C3 Codex 额度屏配置工具

用法：
  codex-quota-device set      --ssid SSID [选项]
  codex-quota-device login    [选项]
  codex-quota-device list
  codex-quota-device fetchNow
  codex-quota-device erase

set 选项：
  --port PORT          手动指定串口；默认自动发现 Espressif 设备
  --password PASSWORD   省略时安全交互输入
  --interval MIN        刷新间隔 1～1440 分钟，默认 5
  --utc-offset +HH:MM   默认使用电脑当前时区

login 选项：
  --port PORT          手动指定串口；默认自动发现 Espressif 设备
  --no-browser          不自动打开浏览器

list / fetchNow / erase 也接受可选的 --port PORT。
`;

function defaultUtcOffsetMinutes(): number {
  return -new Date().getTimezoneOffset();
}

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

function formatUtcOffset(minutes: number): string {
  const sign = minutes < 0 ? "-" : "+";
  const absolute = Math.abs(minutes);
  return `${sign}${String(Math.floor(absolute / 60)).padStart(2, "0")}:${String(
    absolute % 60,
  ).padStart(2, "0")}`;
}

function parseInterval(value: string | undefined): number {
  const text = value ?? "5";
  if (!/^\d+$/.test(text)) throw new CliError("刷新间隔必须是整数分钟");
  const interval = Number(text);
  if (interval < 1 || interval > 1440) {
    throw new CliError("刷新间隔必须在 1～1440 分钟之间");
  }
  return interval;
}

function requireString(value: string | undefined, option: string): string {
  if (!value?.trim()) throw new CliError(`缺少必要参数 ${option}`);
  return value;
}

function redactSensitiveText(text: string): string {
  return text
    .replace(
      /((?:access_token|refresh_token|id_token|wifi_password|code_verifier|code)=)[^\s&"']+/gi,
      "$1[已隐藏]",
    )
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

async function setDevice(args: string[]): Promise<void> {
  const { values } = parseArgs({
    args,
    strict: true,
    allowPositionals: false,
    options: {
      port: { type: "string" },
      ssid: { type: "string" },
      password: { type: "string" },
      interval: { type: "string", default: "5" },
      "utc-offset": {
        type: "string",
        default: formatUtcOffset(defaultUtcOffsetMinutes()),
      },
    },
  });
  const port = await resolvePort(values.port);
  const ssid = requireString(values.ssid, "--ssid");
  const interval = parseInterval(values.interval);
  const utcOffset = parseUtcOffset(values["utc-offset"]);
  const password = values.password ?? (await readHidden("Wi-Fi 密码："));

  console.log("正在写入设备配置（不会触发 OAuth）……");
  const response = await requestDevice(
    port,
    {
      cmd: "configure",
      wifi_ssid: ssid,
      wifi_password: password,
      refresh_minutes: interval,
      utc_offset_minutes: utcOffset,
    },
    20_000,
  );
  console.log(`配置完成：刷新间隔 ${String(response.refresh_minutes ?? interval)} 分钟`);
}

async function loginDevice(args: string[]): Promise<void> {
  const { values } = parseArgs({
    args,
    strict: true,
    allowPositionals: false,
    options: {
      port: { type: "string" },
      "no-browser": { type: "boolean", default: false },
    },
  });
  const port = await resolvePort(values.port);
  const tokens = await browserLogin(async (url) => {
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
    cmd: "login",
    access_token: tokens.accessToken,
    refresh_token: tokens.refreshToken,
    account_id: tokens.accountId,
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
  console.log(`登录凭据已写入设备：${String(response.account_id ?? "已更新")}`);
}

async function simpleCommand(command: Exclude<Command, "set" | "login">, args: string[]) {
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
  if (!["set", "login", "list", "fetchNow", "erase"].includes(rawCommand)) {
    throw new CliError(`未知命令：${rawCommand}\n\n${HELP}`);
  }
  const command = rawCommand as Command;
  if (command === "set") await setDevice(args);
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
