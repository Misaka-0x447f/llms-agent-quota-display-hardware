import { createHash, randomBytes } from "node:crypto";
import { createServer } from "node:http";
import { createInterface } from "node:readline";

import { EnvHttpProxyAgent, request as httpRequest } from "undici";

import { OAuthError } from "./errors.js";

export const CLIENT_ID = "app_EMoamEEZ73f0CkXaXp7hrann";
export const AUTHORIZATION_URL = "https://auth.openai.com/oauth/authorize";
export const TOKEN_URL = "https://auth.openai.com/oauth/token";
export const REDIRECT_URI = "http://localhost:1455/auth/callback";
const CALLBACK_HOST = "localhost";
const CALLBACK_PORT = 1455;
const CALLBACK_TIMEOUT_MS = 15 * 60 * 1000;

export interface Tokens {
  accessToken: string;
  refreshToken: string;
  accountId: string;
  expiresAt: number;
  diagnostics: OAuthResponseDiagnostics;
}

export interface OAuthResponseFieldDiagnostic {
  name: string;
  serializedBytes: number;
  displayValue: string | number | boolean | null;
}

export interface OAuthResponseDiagnostics {
  /** 原始 HTTP 响应体的 UTF-8 字节数；注入测试请求时退化为紧凑 JSON 字节数。 */
  bodyBytes: number;
  bodyByteSource: "HTTP 响应体" | "解析后紧凑 JSON";
  fields: OAuthResponseFieldDiagnostic[];
}

export type JsonObject = Record<string, unknown>;
export interface RequestOptions {
  form?: boolean;
  timeoutMs?: number;
  /** 仅用于本地诊断，不记录或输出响应内容。 */
  onResponseBody?: (utf8Bytes: number) => void;
}
export type OAuthRequest = (
  url: string,
  payload: JsonObject,
  options?: RequestOptions,
) => Promise<[number, JsonObject]>;

const proxyAgent = new EnvHttpProxyAgent();

function isObject(value: unknown): value is JsonObject {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

export function parseOAuthResponse(status: number, body: string): JsonObject {
  let parsed: unknown;
  try {
    parsed = JSON.parse(body);
  } catch (error) {
    if (status < 200 || status >= 300) return {};
    throw new OAuthError(`OAuth 服务返回了无法解析的数据（HTTP ${status}）`, {
      cause: error,
    });
  }
  if (!isObject(parsed)) {
    throw new OAuthError(`OAuth 服务返回格式异常（HTTP ${status}）`);
  }
  return parsed;
}

export async function requestOAuth(
  url: string,
  payload: JsonObject,
  options: RequestOptions = {},
): Promise<[number, JsonObject]> {
  const form = options.form ?? false;
  const encoded = form
    ? new URLSearchParams(
        Object.entries(payload).map(([key, value]) => [key, String(value)]),
      ).toString()
    : JSON.stringify(payload);
  try {
    const response = await httpRequest(url, {
      method: "POST",
      dispatcher: proxyAgent,
      headers: {
        accept: "application/json",
        "content-type": form
          ? "application/x-www-form-urlencoded"
          : "application/json",
      },
      body: encoded,
      headersTimeout: options.timeoutMs ?? 30_000,
      bodyTimeout: options.timeoutMs ?? 30_000,
    });
    const body = await response.body.text();
    const parsed = parseOAuthResponse(response.statusCode, body);
    options.onResponseBody?.(Buffer.byteLength(body, "utf8"));
    return [response.statusCode, parsed];
  } catch (error) {
    if (error instanceof OAuthError) throw error;
    const detail = error instanceof Error ? error.message : String(error);
    throw new OAuthError(`OAuth 网络请求失败：${detail}`, { cause: error });
  }
}

function jwtPayload(token: string): JsonObject {
  try {
    const segment = token.split(".")[1];
    if (!segment) throw new Error("missing payload");
    const parsed: unknown = JSON.parse(Buffer.from(segment, "base64url").toString("utf8"));
    if (!isObject(parsed)) throw new Error("payload is not an object");
    return parsed;
  } catch (error) {
    throw new OAuthError("ID token 格式异常，无法取得账号 ID", { cause: error });
  }
}

export function accountIdFromIdToken(idToken: string): string {
  const payload = jwtPayload(idToken);
  const nested = payload["https://api.openai.com/auth"];
  const source = isObject(nested) ? nested : payload;
  for (const key of ["chatgpt_account_id", "chatgptAccountId", "account_id", "accountId"]) {
    const value = source[key];
    if (typeof value === "string" && value.trim()) return value.trim();
  }
  throw new OAuthError("ID token 中没有 ChatGPT account ID");
}

export interface AuthorizationRequest {
  authorizationUrl: string;
  codeVerifier: string;
  state: string;
}

export function createAuthorizationRequest(): AuthorizationRequest {
  const codeVerifier = randomBytes(64).toString("base64url");
  const codeChallenge = createHash("sha256").update(codeVerifier).digest("base64url");
  const state = randomBytes(32).toString("base64url");
  const params = new URLSearchParams({
    client_id: CLIENT_ID,
    response_type: "code",
    redirect_uri: REDIRECT_URI,
    scope: "openid email profile offline_access",
    state,
    code_challenge: codeChallenge,
    code_challenge_method: "S256",
    prompt: "login",
    id_token_add_organizations: "true",
    codex_cli_simplified_flow: "true",
  });
  return {
    authorizationUrl: `${AUTHORIZATION_URL}?${params}`,
    codeVerifier,
    state,
  };
}

export function authorizationCodeFromCallback(callbackUrl: string, expectedState: string): string {
  let parsed: URL;
  try {
    parsed = new URL(callbackUrl);
  } catch (error) {
    throw new OAuthError("回调 URL 格式无效", { cause: error });
  }
  if (parsed.pathname !== "/auth/callback") {
    throw new OAuthError("回调 URL 路径无效");
  }
  const providerError = parsed.searchParams.get("error");
  if (providerError) throw new OAuthError(`OAuth 授权失败：${providerError}`);
  const code = parsed.searchParams.get("code")?.trim();
  if (!code) throw new OAuthError("回调 URL 中没有授权码");
  if (parsed.searchParams.get("state") !== expectedState) {
    throw new OAuthError("OAuth state 校验失败");
  }
  return code;
}

export interface BrowserLoginDependencies {
  request?: OAuthRequest;
  now?: () => number;
}

function diagnosticValue(name: string, value: unknown): string | number | boolean | null {
  if (name === "expires_in" && typeof value === "number") return value;
  if (name === "token_type" && typeof value === "string") return value;
  if (typeof value === "boolean" || value === null) return value;
  const serialized = JSON.stringify(value);
  const bytes = Buffer.byteLength(serialized === undefined ? String(value) : serialized, "utf8");
  return `[已隐藏，${bytes} B]`;
}

function responseDiagnostics(
  response: JsonObject,
  responseBodyBytes: number | undefined,
): OAuthResponseDiagnostics {
  return {
    bodyBytes: responseBodyBytes ?? Buffer.byteLength(JSON.stringify(response), "utf8"),
    bodyByteSource: responseBodyBytes === undefined ? "解析后紧凑 JSON" : "HTTP 响应体",
    fields: Object.entries(response).map(([name, value]) => {
      const serialized = JSON.stringify(value);
      return {
        name,
        serializedBytes: Buffer.byteLength(serialized === undefined ? String(value) : serialized, "utf8"),
        displayValue: diagnosticValue(name, value),
      };
    }),
  };
}

export async function exchangeAuthorizationCode(
  authorizationCode: string,
  codeVerifier: string,
  dependencies: BrowserLoginDependencies = {},
): Promise<Tokens> {
  const request = dependencies.request ?? requestOAuth;
  const now = dependencies.now ?? (() => Date.now() / 1000);
  let responseBodyBytes: number | undefined;
  const [tokenStatus, tokenResponse] = await request(
    TOKEN_URL,
    {
      grant_type: "authorization_code",
      client_id: CLIENT_ID,
      code: authorizationCode,
      redirect_uri: REDIRECT_URI,
      code_verifier: codeVerifier,
    },
    {
      form: true,
      onResponseBody: (bytes) => {
        responseBodyBytes = bytes;
      },
    },
  );
  if (tokenStatus !== 200) {
    throw new OAuthError(`交换 OAuth token 失败（HTTP ${tokenStatus}）`);
  }

  const accessToken = String(tokenResponse.access_token ?? "").trim();
  const refreshToken = String(tokenResponse.refresh_token ?? "").trim();
  const idToken = String(tokenResponse.id_token ?? "").trim();
  const expiresIn = Number(tokenResponse.expires_in ?? 0);
  if (
    !accessToken ||
    !refreshToken ||
    !idToken ||
    !Number.isInteger(expiresIn) ||
    expiresIn <= 0
  ) {
    throw new OAuthError("OAuth token 响应缺少必要字段");
  }

  return {
    accessToken,
    refreshToken,
    accountId: accountIdFromIdToken(idToken),
    expiresAt: Math.trunc(now()) + expiresIn,
    diagnostics: responseDiagnostics(tokenResponse, responseBodyBytes),
  };
}

interface CallbackListener {
  wait: Promise<string>;
  close: () => Promise<void>;
}

async function startCallbackListener(expectedState: string): Promise<CallbackListener> {
  let settle: ((code: string) => void) | undefined;
  let fail: ((error: Error) => void) | undefined;
  const wait = new Promise<string>((resolve, reject) => {
    settle = resolve;
    fail = reject;
  });
  const server = createServer((request, response) => {
    const callbackUrl = new URL(request.url ?? "/", REDIRECT_URI).toString();
    try {
      const code = authorizationCodeFromCallback(callbackUrl, expectedState);
      response.writeHead(200, { "content-type": "text/html; charset=utf-8" });
      response.end("<p>Codex 登录完成，可以关闭此页面。</p>");
      settle?.(code);
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      response.writeHead(400, { "content-type": "text/plain; charset=utf-8" });
      response.end(message);
      fail?.(error instanceof Error ? error : new OAuthError(message));
    }
  });
  const timeout = setTimeout(
    () => fail?.(new OAuthError("等待 OAuth 回调超时（15 分钟）")),
    CALLBACK_TIMEOUT_MS,
  );
  try {
    await new Promise<void>((resolve, reject) => {
      server.once("error", reject);
      server.listen(CALLBACK_PORT, CALLBACK_HOST, () => {
        server.off("error", reject);
        resolve();
      });
    });
  } catch (error) {
    clearTimeout(timeout);
    const detail = error instanceof Error ? error.message : String(error);
    throw new OAuthError(`无法监听 localhost:${CALLBACK_PORT} OAuth 回调端口：${detail}`, {
      cause: error,
    });
  }
  return {
    wait,
    close: async () => {
      clearTimeout(timeout);
      if (!server.listening) return;
      await new Promise<void>((resolve) => server.close(() => resolve()));
    },
  };
}

function waitForPastedCallback(expectedState: string): {
  wait: Promise<string>;
  close: () => void;
} | undefined {
  if (!process.stdin.isTTY) return undefined;
  const prompt = createInterface({ input: process.stdin, output: process.stdout });
  console.log("若浏览器未自动回跳，可随时粘贴完整 callback URL 并回车：");
  let resolve: ((code: string) => void) | undefined;
  let reject: ((error: Error) => void) | undefined;
  const wait = new Promise<string>((onResolve, onReject) => {
    resolve = onResolve;
    reject = onReject;
  });
  prompt.on("line", (line) => {
    const input = line.trim();
    if (!input) return;
    try {
      resolve?.(authorizationCodeFromCallback(input, expectedState));
    } catch (error) {
      reject?.(error instanceof Error ? error : new OAuthError(String(error)));
    }
  });
  return { wait, close: () => prompt.close() };
}

export async function browserLogin(
  onAuthorizationUrl: (url: string) => void | Promise<void>,
  dependencies: BrowserLoginDependencies = {},
): Promise<Tokens> {
  const authorization = createAuthorizationRequest();
  const listener = await startCallbackListener(authorization.state);
  const pasted = waitForPastedCallback(authorization.state);
  try {
    await onAuthorizationUrl(authorization.authorizationUrl);
    const code = await Promise.race([listener.wait, ...(pasted ? [pasted.wait] : [])]);
    return await exchangeAuthorizationCode(code, authorization.codeVerifier, dependencies);
  } finally {
    pasted?.close();
    await listener.close();
  }
}
