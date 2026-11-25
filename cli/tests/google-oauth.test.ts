import assert from "node:assert/strict";
import test from "node:test";

import { CliError } from "../src/errors.js";
import { parseRows, run } from "../src/cli.js";
import {
  GOOGLE_AUTHORIZATION_URL,
  GOOGLE_REDIRECT_URI,
  GOOGLE_SCOPE,
  GOOGLE_TOKEN_URL,
  createGoogleAuthorizationRequest,
  exchangeGoogleAuthorizationCode,
} from "../src/google-oauth.js";
import type { GoogleClient } from "../src/antigravity-client.js";

// 测试用假客户端：真实值只存在于本地配置（cli/antigravity-client.json，未入库）。
const TEST_CLIENT: GoogleClient = {
  id: "test-client.apps.googleusercontent.com",
  secret: "test-client-secret",
};
import { createHash } from "node:crypto";

import type { JsonObject, OAuthRequest } from "../src/oauth.js";

function jwt(payload: JsonObject): string {
  return `header.${Buffer.from(JSON.stringify(payload)).toString("base64url")}.signature`;
}

// --provider 校验在解析串口之前：写错 provider 时应先看到参数错误。
test("login rejects unknown providers before touching the serial port", async () => {
  await assert.rejects(
    () => run(["login", "--provider", "gemini"]),
    /--provider/,
  );
});

// Antigravity 只有每周额度：CLI 侧就必须拒绝 :5h，不能下发到设备再报错。
test("antigravity rows accept only the weekly window", () => {
  assert.equal(parseRows("antigravity:7d,deepseek"), "antigravity:7d,deepseek");
  assert.equal(parseRows("antigravity,deepseek"), "antigravity,deepseek");
  assert.throws(() => parseRows("antigravity:5h"), CliError);
  // chatgpt / claude 的 5h 不受影响。
  assert.equal(parseRows("chatgpt:5h,claude:5h"), "chatgpt:5h,claude:5h");
});

// 授权链接走回环重定向（设备码对 installed-app 客户端不可用），并且必须带
// offline + consent，否则 Google 不会再发 refresh_token。
test("google authorization URL requests offline access on the loopback redirect", () => {
  const authorization = createGoogleAuthorizationRequest(TEST_CLIENT);
  const parsed = new URL(authorization.authorizationUrl);
  assert.equal(parsed.origin + parsed.pathname, GOOGLE_AUTHORIZATION_URL);
  assert.equal(parsed.searchParams.get("client_id"), TEST_CLIENT.id);
  assert.equal(parsed.searchParams.get("redirect_uri"), GOOGLE_REDIRECT_URI);
  assert.equal(parsed.searchParams.get("response_type"), "code");
  assert.equal(parsed.searchParams.get("scope"), GOOGLE_SCOPE);
  // 必须带 aicode：少了它，Cloud Code 会把令牌当成旧的 Gemini Code Assist 客户端。
  assert.match(GOOGLE_SCOPE, /auth\/aicode/);
  assert.equal(parsed.searchParams.get("access_type"), "offline");
  assert.equal(parsed.searchParams.get("prompt"), "consent");
  assert.equal(parsed.searchParams.get("state"), authorization.state);
  assert.ok(parsed.searchParams.get("state"));
  // PKCE：challenge 必须是本次 verifier 的 S256，且不能把 verifier 自己发出去。
  assert.equal(parsed.searchParams.get("code_challenge_method"), "S256");
  assert.equal(
    parsed.searchParams.get("code_challenge"),
    createHash("sha256").update(authorization.codeVerifier).digest("base64url"),
  );
  assert.ok(authorization.codeVerifier.length > 0);
  assert.notEqual(parsed.searchParams.get("code_challenge"), authorization.codeVerifier);
});

// 每次登录的 verifier 必须不同，否则 PKCE 只剩形式。
test("google authorization requests use a fresh PKCE verifier", () => {
  const first = createGoogleAuthorizationRequest(TEST_CLIENT);
  const second = createGoogleAuthorizationRequest(TEST_CLIENT);
  assert.notEqual(first.codeVerifier, second.codeVerifier);
  assert.notEqual(first.state, second.state);
});

test("google token exchange sends the client pair and reports the account email", async () => {
  const calls: Array<[string, JsonObject, boolean]> = [];
  const response: JsonObject = {
    access_token: "ya29.access",
    refresh_token: "1//refresh",
    id_token: jwt({ email: "someone@example.com" }),
    expires_in: 3599,
  };
  const request: OAuthRequest = async (url, payload, options) => {
    calls.push([url, payload, options?.form ?? false]);
    return [200, response];
  };
  const tokens = await exchangeGoogleAuthorizationCode(
    "auth-code",
    { codeVerifier: "verifier-123" },
    TEST_CLIENT,
    { request, now: () => 1000 },
  );
  assert.equal(tokens.accessToken, "ya29.access");
  assert.equal(tokens.refreshToken, "1//refresh");
  assert.equal(tokens.accountId, "someone@example.com");
  assert.equal(tokens.expiresAt, 4599);
  assert.equal(calls.length, 1);
  assert.equal(calls[0][0], GOOGLE_TOKEN_URL);
  assert.equal(calls[0][2], true);
  assert.deepEqual(calls[0][1], {
    grant_type: "authorization_code",
    code: "auth-code",
    client_id: TEST_CLIENT.id,
    client_secret: TEST_CLIENT.secret,
    redirect_uri: GOOGLE_REDIRECT_URI,
    code_verifier: "verifier-123",
  });
});

// 没有 refresh_token 说明授权页没给 offline access：必须当作失败，
// 否则设备拿不到能续期的凭据，几小时后就会「登录失效」。
test("google token exchange requires a refresh token", async () => {
  const request: OAuthRequest = async () => [
    200,
    { access_token: "ya29.access", expires_in: 3599 },
  ];
  await assert.rejects(
    () =>
      exchangeGoogleAuthorizationCode("auth-code", { codeVerifier: "v" }, TEST_CLIENT, {
        request,
      }),
    /刷新令牌/,
  );
});

test("google token exchange reports only the status on failure", async () => {
  const request: OAuthRequest = async () => [
    400,
    { error: "invalid_grant", error_description: "account detail" },
  ];
  await assert.rejects(
    () =>
      exchangeGoogleAuthorizationCode("auth-code", { codeVerifier: "v" }, TEST_CLIENT, {
        request,
      }),
    (error: unknown) => {
      assert.ok(error instanceof Error);
      assert.match(error.message, /HTTP 400/);
      assert.doesNotMatch(error.message, /account detail/);
      return true;
    },
  );
});
