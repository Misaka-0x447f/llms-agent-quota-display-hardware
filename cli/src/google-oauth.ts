import { createHash, randomBytes } from "node:crypto";

import type { GoogleClient } from "./antigravity-client.js";
import { OAuthError } from "./errors.js";
import {
  receiveAuthorizationCode,
  requestOAuth,
  responseDiagnostics,
  type AuthorizationRequest,
  type BrowserLoginDependencies,
  type Tokens,
} from "./oauth.js";

// Antigravity 用的 Google installed-app 客户端由本地配置提供（见
// antigravity-client.ts）：client id / secret 不进仓库。真正代表用户身份的是
// 设备里那份 refresh token。
export const GOOGLE_AUTHORIZATION_URL = "https://accounts.google.com/o/oauth2/v2/auth";
export const GOOGLE_TOKEN_URL = "https://oauth2.googleapis.com/token";
export const GOOGLE_REDIRECT_URI = "http://localhost:54545/callback";
// `aicode` 是 Antigravity 自己的 scope：只有带它的令牌，Cloud Code 后端才会把这个
// 客户端当成 Antigravity（额度摘要里才会出现 gemini-* 组）。只申请 cloud-platform
// 的令牌会被当成旧的 Gemini Code Assist 客户端，loadCodeAssist 直接回
// `ineligibleTiers: UNSUPPORTED_CLIENT`，配额里只有 3p-*（Claude/GPT）组。
// openid/email 只用来在 `list` 里显示登录的账号。
export const GOOGLE_SCOPE =
  "openid email https://www.googleapis.com/auth/aicode " +
  "https://www.googleapis.com/auth/cloud-platform";

// Google 的 installed-app 客户端只能走回环重定向：设备码端点对这类客户端
// 直接返回 "Invalid client type"（只有 TVs and Limited Input devices 能用）。
export function createGoogleAuthorizationRequest(client: GoogleClient): AuthorizationRequest {
  // PKCE（S256）：回环端口或粘贴回调被截获时，没有本次登录独有的 verifier
  // 就换不到令牌。随软件发布的 client_secret 不能替代这一步。
  const codeVerifier = randomBytes(64).toString("base64url");
  const state = randomBytes(32).toString("base64url");
  const params = new URLSearchParams({
    client_id: client.id,
    response_type: "code",
    redirect_uri: GOOGLE_REDIRECT_URI,
    scope: GOOGLE_SCOPE,
    // 必须同时给 access_type=offline + prompt=consent，否则不会再发 refresh_token。
    access_type: "offline",
    prompt: "consent",
    code_challenge: createHash("sha256").update(codeVerifier).digest("base64url"),
    code_challenge_method: "S256",
    state,
  });
  return {
    authorizationUrl: `${GOOGLE_AUTHORIZATION_URL}?${params}`,
    codeVerifier,
    state,
  };
}

function emailFromIdToken(idToken: string): string {
  if (!idToken) return "";
  const segment = idToken.split(".")[1];
  if (!segment) return "";
  try {
    const payload: unknown = JSON.parse(Buffer.from(segment, "base64url").toString("utf8"));
    if (payload === null || typeof payload !== "object") return "";
    const email = (payload as Record<string, unknown>).email;
    return typeof email === "string" ? email.trim() : "";
  } catch {
    // 邮箱只用于展示；解不出来不算登录失败。
    return "";
  }
}

export async function exchangeGoogleAuthorizationCode(
  code: string,
  authorization: Pick<AuthorizationRequest, "codeVerifier">,
  client: GoogleClient,
  dependencies: BrowserLoginDependencies = {},
): Promise<Tokens> {
  const request = dependencies.request ?? requestOAuth;
  const now = dependencies.now ?? (() => Date.now() / 1000);
  let bodyBytes: number | undefined;
  const [status, body] = await request(
    GOOGLE_TOKEN_URL,
    {
      grant_type: "authorization_code",
      code,
      client_id: client.id,
      client_secret: client.secret,
      redirect_uri: GOOGLE_REDIRECT_URI,
      code_verifier: authorization.codeVerifier,
    },
    {
      form: true,
      timeoutMs: 60_000,
      onResponseBody: (bytes) => {
        bodyBytes = bytes;
      },
    },
  );
  if (status !== 200) {
    // Google 的错误体可能带账号细节，只报状态码。
    throw new OAuthError(`Google OAuth 授权码交换失败（HTTP ${status}）`);
  }
  const accessToken = typeof body.access_token === "string" ? body.access_token.trim() : "";
  const refreshToken = typeof body.refresh_token === "string" ? body.refresh_token.trim() : "";
  const expiresIn = body.expires_in;
  if (
    !accessToken ||
    !refreshToken ||
    typeof expiresIn !== "number" ||
    !Number.isSafeInteger(expiresIn) ||
    expiresIn <= 0 ||
    expiresIn > 365 * 86400
  ) {
    throw new OAuthError(
      "Google OAuth 响应缺少访问令牌、刷新令牌或过期时间（离线访问需要 access_type=offline）",
    );
  }
  return {
    accessToken,
    refreshToken,
    accountId: emailFromIdToken(typeof body.id_token === "string" ? body.id_token : ""),
    expiresAt: Math.trunc(now()) + expiresIn,
    diagnostics: responseDiagnostics(body, bodyBytes),
  };
}

export async function googleBrowserLogin(
  onAuthorizationUrl: (url: string) => void | Promise<void>,
  client: GoogleClient,
  dependencies: BrowserLoginDependencies = {},
): Promise<Tokens> {
  const authorization = createGoogleAuthorizationRequest(client);
  const code = await receiveAuthorizationCode(
    authorization,
    GOOGLE_REDIRECT_URI,
    onAuthorizationUrl,
  );
  return exchangeGoogleAuthorizationCode(code, authorization, client, dependencies);
}
