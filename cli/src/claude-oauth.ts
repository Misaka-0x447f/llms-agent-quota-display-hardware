import { createHash, randomBytes } from "node:crypto";

import { OAuthError } from "./errors.js";
import {
  receiveAuthorizationCode,
  requestOAuth,
  responseDiagnostics,
  type AuthorizationRequest,
  type BrowserLoginDependencies,
  type Tokens,
} from "./oauth.js";

// Public Claude Code client ID, matching CLIProxyAPI's Claude PKCE login flow.
export const CLAUDE_CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
export const CLAUDE_AUTHORIZATION_URL = "https://claude.ai/oauth/authorize";
export const CLAUDE_TOKEN_URL = "https://platform.claude.com/v1/oauth/token";
export const CLAUDE_REDIRECT_URI = "http://localhost:54545/callback";
export const CLAUDE_SCOPE =
  "user:profile user:inference user:sessions:claude_code user:mcp_servers user:file_upload";

export function createClaudeAuthorizationRequest(): AuthorizationRequest {
  const codeVerifier = randomBytes(64).toString("base64url");
  const state = randomBytes(32).toString("base64url");
  const params = new URLSearchParams({
    code: "true",
    client_id: CLAUDE_CLIENT_ID,
    response_type: "code",
    redirect_uri: CLAUDE_REDIRECT_URI,
    scope: CLAUDE_SCOPE,
    code_challenge: createHash("sha256").update(codeVerifier).digest("base64url"),
    code_challenge_method: "S256",
    state,
  });
  return { authorizationUrl: `${CLAUDE_AUTHORIZATION_URL}?${params}`, codeVerifier, state };
}

export async function exchangeClaudeAuthorizationCode(
  code: string,
  authorization: Pick<AuthorizationRequest, "codeVerifier" | "state">,
  dependencies: BrowserLoginDependencies = {},
): Promise<Tokens> {
  const request = dependencies.request ?? requestOAuth;
  const now = dependencies.now ?? (() => Date.now() / 1000);
  let bodyBytes: number | undefined;
  const [status, body] = await request(CLAUDE_TOKEN_URL, {
    grant_type: "authorization_code",
    code,
    redirect_uri: CLAUDE_REDIRECT_URI,
    client_id: CLAUDE_CLIENT_ID,
    code_verifier: authorization.codeVerifier,
    state: authorization.state,
  }, {
    timeoutMs: 60_000,
    onResponseBody: (bytes) => { bodyBytes = bytes; },
  });
  if (status !== 200) throw new OAuthError(`Claude OAuth 授权码交换失败（HTTP ${status}）`);
  const accessToken = typeof body.access_token === "string" ? body.access_token.trim() : "";
  const refreshToken = typeof body.refresh_token === "string" ? body.refresh_token.trim() : "";
  const expiresIn = body.expires_in;
  if (!accessToken || !refreshToken || typeof expiresIn !== "number" ||
      !Number.isSafeInteger(expiresIn) || expiresIn <= 0 || expiresIn > 365 * 86400) {
    throw new OAuthError("Claude OAuth 响应缺少有效的访问令牌、刷新令牌或过期时间");
  }
  if (typeof body.scope === "string" && !body.scope.split(/\s+/).includes("user:profile")) {
    throw new OAuthError("Claude OAuth 授权缺少读取用量所需的 user:profile 权限");
  }
  // Unlike Codex, Claude usage does not need an account ID header or an ID token.
  return {
    accessToken, refreshToken, accountId: "", expiresAt: Math.trunc(now()) + expiresIn,
    diagnostics: responseDiagnostics(body, bodyBytes),
  };
}

export async function claudeBrowserLogin(
  onAuthorizationUrl: (url: string) => void | Promise<void>,
  dependencies: BrowserLoginDependencies = {},
): Promise<Tokens> {
  const authorization = createClaudeAuthorizationRequest();
  const code = await receiveAuthorizationCode(authorization, CLAUDE_REDIRECT_URI, onAuthorizationUrl);
  return exchangeClaudeAuthorizationCode(code, authorization, dependencies);
}
