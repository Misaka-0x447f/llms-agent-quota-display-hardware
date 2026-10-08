import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import test from "node:test";

import {
  CLAUDE_CLIENT_ID, CLAUDE_AUTHORIZATION_URL, CLAUDE_REDIRECT_URI, CLAUDE_TOKEN_URL,
  createClaudeAuthorizationRequest, exchangeClaudeAuthorizationCode,
} from "../src/claude-oauth.js";
import { authorizationCodeFromCallback, parseOAuthResponse, type JsonObject, type OAuthRequest } from "../src/oauth.js";

// All credentials in these fixtures are synthetic test data.
const authorization = { codeVerifier: "test-verifier", state: "test-state" };
const valid: JsonObject = {
  access_token: "test-access", refresh_token: "test-refresh", expires_in: 28800,
  scope: "user:profile user:inference", account: { uuid: "test-account", email: "test@example.com" },
};

test("Claude login uses a fresh PKCE verifier and a separate callback", () => {
  const first = createClaudeAuthorizationRequest();
  const second = createClaudeAuthorizationRequest();
  assert.notEqual(first.codeVerifier, second.codeVerifier);
  assert.notEqual(first.state, second.state);
  const url = new URL(first.authorizationUrl);
  assert.equal(url.origin + url.pathname, CLAUDE_AUTHORIZATION_URL);
  assert.equal(url.searchParams.get("client_id"), CLAUDE_CLIENT_ID);
  assert.equal(url.searchParams.get("redirect_uri"), CLAUDE_REDIRECT_URI);
  assert.equal(url.searchParams.get("code_challenge"), createHash("sha256").update(first.codeVerifier).digest("base64url"));
  assert.ok(url.searchParams.get("scope")?.split(" ").includes("user:profile"));
  assert.equal(url.searchParams.get("state"), first.state);
});

test("Claude callback validates state, origin, path and errors", () => {
  assert.equal(authorizationCodeFromCallback(`${CLAUDE_REDIRECT_URI}?code=test-code&state=test-state`, "test-state", CLAUDE_REDIRECT_URI), "test-code");
  for (const url of [
    `${CLAUDE_REDIRECT_URI}?code=test-code&state=wrong`,
    "http://localhost:1455/auth/callback?code=test-code&state=test-state",
    "https://untrusted.example/callback?code=test-code&state=test-state",
    `${CLAUDE_REDIRECT_URI}?state=test-state`,
    `${CLAUDE_REDIRECT_URI}?error=test-private-message&state=test-state`,
  ]) assert.throws(() => authorizationCodeFromCallback(url, "test-state", CLAUDE_REDIRECT_URI));
});

test("Claude exchanges code with state and PKCE without requiring Codex ID token", async () => {
  const calls: unknown[] = [];
  const request: OAuthRequest = async (url, payload, options) => {
    calls.push([url, payload]);
    assert.equal(options?.form ?? false, false);
    options?.onResponseBody?.(512);
    return [200, valid];
  };
  const tokens = await exchangeClaudeAuthorizationCode("test-code", authorization, { request, now: () => 1000 });
  assert.equal(tokens.expiresAt, 29800);
  assert.equal(tokens.accountId, "");
  assert.equal(tokens.diagnostics.bodyBytes, 512);
  assert.deepEqual(calls, [[CLAUDE_TOKEN_URL, {
    grant_type: "authorization_code", code: "test-code", redirect_uri: CLAUDE_REDIRECT_URI,
    client_id: CLAUDE_CLIENT_ID, code_verifier: "test-verifier", state: "test-state",
  }]]);
  const diagnostics = JSON.stringify(tokens.diagnostics);
  for (const secret of ["test-access", "test-refresh", "test-account", "test@example.com"]) {
    assert.ok(!diagnostics.includes(secret));
  }
});

test("rejects malformed credentials, expiry and insufficient scopes", async () => {
  for (const fields of [
    { access_token: {} }, { access_token: "" }, { refresh_token: "" },
    { expires_in: "3600" }, { expires_in: -1 }, { expires_in: 0.5 },
    { expires_in: Number.POSITIVE_INFINITY }, { scope: "user:inference" },
  ]) {
    await assert.rejects(exchangeClaudeAuthorizationCode("test-code", authorization, {
      request: async () => [200, { ...valid, ...fields }],
    }));
  }
});

test("OAuth rejection and malformed JSON do not expose response secrets", async () => {
  await assert.rejects(exchangeClaudeAuthorizationCode("test-code", authorization, {
    request: async () => [403, { error: "test-private-response", access_token: "test-access" }],
  }), (error: Error) => !String(error.stack).includes("test-private") && !String(error.stack).includes("test-access"));
  assert.throws(() => parseOAuthResponse(200, '{"access_token":"test-private-fragment"'),
    (error: Error) => error.cause === undefined && !String(error.stack).includes("test-private"));
});

test("callback listener ignores unrelated requests, receives code and releases its port", async () => {
  const { createServer } = await import("node:net");
  const { receiveAuthorizationCode } = await import("../src/oauth.js");
  const probe = createServer();
  await new Promise<void>((resolve) => probe.listen(0, "127.0.0.1", resolve));
  const address = probe.address();
  assert.ok(address && typeof address === "object");
  const port = address.port;
  await new Promise<void>((resolve) => probe.close(() => resolve()));
  const redirect = `http://127.0.0.1:${port}/callback`;
  const result = await receiveAuthorizationCode({
    ...authorization, authorizationUrl: "https://example.com/test-authorization",
  }, redirect, async () => {
    const invalid = await fetch(`${redirect}?code=test-code&state=wrong`);
    assert.equal(invalid.status, 400);
    await invalid.text();
    const valid = await fetch(`${redirect}?code=test-code&state=test-state`);
    assert.equal(valid.status, 200);
    await valid.text();
  });
  assert.equal(result, "test-code");
  await new Promise<void>((resolve, reject) => {
    probe.once("error", reject);
    probe.listen(port, "127.0.0.1", resolve);
  });
  await new Promise<void>((resolve) => probe.close(() => resolve()));
});
