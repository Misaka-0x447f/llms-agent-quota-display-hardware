import assert from "node:assert/strict";
import test from "node:test";

import { CliError } from "../src/errors.js";
import { MAX_API_KEYS, parseApiKeys, parseUtcOffset, run } from "../src/cli.js";

test("parses positive and negative UTC offsets", () => {
  assert.equal(parseUtcOffset("+08:00"), 480);
  assert.equal(parseUtcOffset("-05:30"), -330);
});

test("rejects invalid UTC offsets", () => {
  assert.throws(() => parseUtcOffset("+15:00"), CliError);
  assert.throws(() => parseUtcOffset("08:60"), CliError);
  assert.throws(() => parseUtcOffset("UTC+8"), CliError);
});


// 抓取频率固定在固件里（Claude 10 分钟、余额 50 秒），CLI 不再提供间隔参数。
test("update no longer accepts refresh interval options", async () => {
  await assert.rejects(run(["update", "--interval", "5"]), /interval/);
  await assert.rejects(run(["update", "--interval-seconds", "50"]), /interval/);
});

test("accepts repeated, comma separated and newline separated API keys", () => {
  assert.deepEqual(parseApiKeys(["k1", "k2"], "--deepseek-key"), ["k1", "k2"]);
  assert.deepEqual(parseApiKeys(["k1,k2\nk3"], "--deepseek-key"), ["k1", "k2", "k3"]);
  assert.deepEqual(parseApiKeys([" k1 ", "k1"], "--deepseek-key"), ["k1"]);
  assert.deepEqual(parseApiKeys(undefined, "--openrouter-key"), []);
});

test("validates each API key list", () => {
  assert.throws(() => parseApiKeys([""], "--deepseek-key"), /不能为空/);
  assert.throws(() => parseApiKeys([" , "], "--openrouter-key"), /不能为空/);
  assert.throws(
    () => parseApiKeys(["a", "b", "c", "d", "e"], "--deepseek-key"),
    /最多支持 4 个/,
  );
  assert.equal(MAX_API_KEYS, 4);
  assert.throws(() => parseApiKeys(["k".repeat(129)], "--deepseek-key"), /128/);
  assert.doesNotThrow(() => parseApiKeys(["k".repeat(128)], "--deepseek-key"));
});

test("validates keys before resolving a serial port", async () => {
  await assert.rejects(
    run(["update", "--deepseek-key", "a,b,c,d,e"]),
    /最多支持 4 个/,
  );
});
