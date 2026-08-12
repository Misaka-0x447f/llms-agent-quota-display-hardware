import assert from "node:assert/strict";
import test from "node:test";

import {
  AUTHORIZATION_URL,
  CLIENT_ID,
  REDIRECT_URI,
  TOKEN_URL,
  accountIdFromIdToken,
  authorizationCodeFromCallback,
  createAuthorizationRequest,
  exchangeAuthorizationCode,
  parseOAuthResponse,
  type JsonObject,
  type OAuthRequest,
} from "../src/oauth.js";

function jwt(payload: JsonObject): string {
  return `header.${Buffer.from(JSON.stringify(payload)).toString("base64url")}.signature`;
}

test("non-JSON pending response is accepted", () => {
  assert.deepEqual(parseOAuthResponse(403, "authorization_pending"), {});
});

test("extracts nested ChatGPT account ID", () => {
  const token = jwt({
    "https://api.openai.com/auth": { chatgpt_account_id: "acct-123" },
  });
  assert.equal(accountIdFromIdToken(token), "acct-123");
});

test("authorization URL uses the CLIProxyAPI browser callback flow", () => {
  const authorization = createAuthorizationRequest();
  const parsed = new URL(authorization.authorizationUrl);
  assert.equal(parsed.origin + parsed.pathname, AUTHORIZATION_URL);
  assert.equal(parsed.searchParams.get("client_id"), CLIENT_ID);
  assert.equal(parsed.searchParams.get("redirect_uri"), REDIRECT_URI);
  assert.equal(parsed.searchParams.get("response_type"), "code");
  assert.equal(parsed.searchParams.get("code_challenge_method"), "S256");
  assert.equal(parsed.searchParams.get("state"), authorization.state);
  assert.ok(parsed.searchParams.get("code_challenge"));
  assert.notEqual(parsed.searchParams.get("code_challenge"), authorization.codeVerifier);
});

test("validates the OAuth callback state", () => {
  assert.equal(
    authorizationCodeFromCallback(
      `${REDIRECT_URI}?code=auth-code&state=state-123`,
      "state-123",
    ),
    "auth-code",
  );
  assert.throws(
    () => authorizationCodeFromCallback(`${REDIRECT_URI}?code=auth-code&state=wrong`, "state-123"),
    /state/,
  );
});

test("exchanges the browser callback code for tokens", async () => {
  const calls: Array<[string, JsonObject, boolean]> = [];
  const expectedResponse: JsonObject = {
    access_token: "access",
    refresh_token: "refresh",
    id_token: jwt({
      "https://api.openai.com/auth": { chatgpt_account_id: "acct-123" },
    }),
    expires_in: 3600,
  };
  const responses: Array<[number, JsonObject]> = [[200, expectedResponse]];
  const request: OAuthRequest = async (url, payload, options) => {
    calls.push([url, payload, options?.form ?? false]);
    const response = responses.shift();
    assert.ok(response);
    return response;
  };
  const tokens = await exchangeAuthorizationCode("auth-code", "verifier", {
    request,
    now: () => 1000,
  });

  assert.equal(tokens.accountId, "acct-123");
  assert.equal(tokens.expiresAt, 4600);
  assert.equal(tokens.diagnostics.bodyByteSource, "解析后紧凑 JSON");
  assert.equal(tokens.diagnostics.bodyBytes, Buffer.byteLength(JSON.stringify(expectedResponse)));
  assert.deepEqual(calls, [[
    TOKEN_URL,
    {
      grant_type: "authorization_code",
      client_id: CLIENT_ID,
      code: "auth-code",
      redirect_uri: REDIRECT_URI,
      code_verifier: "verifier",
    },
    true,
  ]]);
});
