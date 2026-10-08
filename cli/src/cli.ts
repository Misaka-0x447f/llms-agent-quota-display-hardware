#!/usr/bin/env -S node --import tsx

import { parseArgs } from "node:util";

import open from "open";

import { AbortError, CliError, DeviceError, OAuthError } from "./errors.js";
import { claudeBrowserLogin } from "./claude-oauth.js";
import { browserLogin } from "./oauth.js";
import { readHidden } from "./prompt.js";
import { requestDevice, resolvePort } from "./serial-link.js";

type Command = "update" | "login" | "list" | "fetchNow" | "erase";

const HELP = `ESP32-C3 Claude 额度屏配置工具（5 小时 / 7 天）

用法：
  codex-quota-device update    [选项]
  codex-quota-device login     [选项]
  codex-quota-device list
  codex-quota-device fetchNow
  codex-quota-device erase

update 选项（至少指定一个；未指定的参数保持设备原值不变）：
  --ssid SSID           更新 Wi-Fi 名称（改 SSID 而未给 --password 时会交互提示密码）
  --password PASSWORD   更新 Wi-Fi 密码
  --deepseek-key KEY    保留旧 DeepSeek 配置（本版本不采集或显示；两个 key 成对更新）
  --openrouter-key KEY  保留旧 OpenRouter 配置（本版本不采集或显示）
  --interval MIN        更新刷新间隔 1～1440 分钟
  --utc-offset +HH:MM   更新时区偏移
  --port PORT           手动指定串口；默认自动发现 Espressif 设备

login 选项：
  --provider NAME       claude（默认）；codex 仅保留旧凭据配置，屏幕只显示 Claude
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

function parseInterval(value: string): number {
  if (!/^\d+$/.test(value)) throw new CliError("刷新间隔必须是整数分钟");
  const interval = Number(value);
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
      "deepseek-key": { type: "string" },
      "openrouter-key": { type: "string" },
      interval: { type: "string" },
      "utc-offset": { type: "string" },
    },
  });
  const port = await resolvePort(values.port);

  const payload: Record<string, unknown> = { cmd: "configure" };

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

  if (values.interval !== undefined) {
    payload.refresh_minutes = parseInterval(values.interval);
  }

  if (values["utc-offset"] !== undefined) {
    payload.utc_offset_minutes = parseUtcOffset(values["utc-offset"]);
  }

  // 两个 key 成对更新：只给一个时交互补齐另一个。
  if (values["deepseek-key"] !== undefined || values["openrouter-key"] !== undefined) {
    const deepseekKey = (
      values["deepseek-key"] ?? (await readHidden("DeepSeek API Key：", "--deepseek-key"))
    ).trim();
    const openrouterKey = (
      values["openrouter-key"] ?? (await readHidden("OpenRouter API Key：", "--openrouter-key"))
    ).trim();
    if (!deepseekKey || !openrouterKey) {
      throw new CliError("DeepSeek 和 OpenRouter 的 API Key 不能为空");
    }
    payload.deepseek_key = deepseekKey;
    payload.openrouter_key = openrouterKey;
  }

  const updatedFields = Object.keys(payload).filter((key) => key !== "cmd");
  if (updatedFields.length === 0) {
    throw new CliError(
      "请至少指定一个要更新的参数：--ssid / --password / --deepseek-key / --openrouter-key / --interval / --utc-offset",
    );
  }

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
  const port = await resolvePort(values.port);
  if (values.provider !== "claude" && values.provider !== "codex") {
    throw new CliError("--provider 只接受 claude 或 codex");
  }
  const isClaude = values.provider === "claude";
  if (isClaude) {
    const status = await requestDevice(port, { cmd: "status" }, 20_000);
    if (status.usage_provider !== "claude") {
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
  console.log(`${isClaude ? "Claude" : "Codex"} 登录凭据已写入设备：${String(response.account_id ?? "已更新")}`);
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
