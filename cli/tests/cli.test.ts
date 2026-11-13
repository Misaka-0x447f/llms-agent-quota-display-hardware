import assert from "node:assert/strict";
import test from "node:test";

import { CliError } from "../src/errors.js";
import { MAX_API_KEYS, MAX_ROWS, parseApiKeys, parseRefreshInterval, parseRows, parseUtcOffset, run } from "../src/cli.js";

test("parses positive and negative UTC offsets", () => {
  assert.equal(parseUtcOffset("+08:00"), 480);
  assert.equal(parseUtcOffset("-05:30"), -330);
});

test("rejects invalid UTC offsets", () => {
  assert.throws(() => parseUtcOffset("+15:00"), CliError);
  assert.throws(() => parseUtcOffset("08:60"), CliError);
  assert.throws(() => parseUtcOffset("UTC+8"), CliError);
});


// 刷新间隔随行配置走（--rows 的 @interval），CLI 不再提供旧的全局间隔参数。
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

// 屏幕行配置：与固件同一套语法，电脑侧先拦掉明显写错的写法。
test("normalizes rows config", () => {
  assert.equal(parseRows("chatgpt:7d,deepseek,openrouter"), "chatgpt:7d,deepseek,openrouter");
  assert.equal(parseRows(" chatgpt , deepseek "), "chatgpt,deepseek");
  assert.equal(parseRows("chatgpt:5h,chatgpt:7d,openrouter"), "chatgpt:5h,chatgpt:7d,openrouter");
  assert.equal(MAX_ROWS, 3);
});

test("rejects malformed rows config", () => {
  const invalid = [
    "",
    ",",
    "chatgpt,",
    ",chatgpt",
    "chatgpt,,deepseek",
    "chatgpt:",
    "chatgpt:1d",
    "chatgpt:7d:5h",
    "deepseek:7d",
    "openrouter:5h",
    "unknown",
    "chatgpt,deepseek,openrouter,claude",
  ];
  for (const value of invalid) {
    assert.throws(() => parseRows(value), CliError, value);
  }
});

test("validates rows before resolving a serial port", async () => {
  await assert.rejects(run(["update", "--rows", "deepseek:7d"]), /窗口/);
  await assert.rejects(run(["update", "--rows", ""]), /空项/);
});

// 每行刷新间隔：默认窗口 10m、余额 50s，可用 @ 覆盖。
test("normalizes per-row refresh intervals", () => {
  assert.equal(parseRefreshInterval("30s"), 30);
  assert.equal(parseRefreshInterval("10m"), 600);
  assert.equal(parseRefreshInterval("1h"), 3600);
  assert.equal(parseRefreshInterval("10s"), 10);
  assert.equal(parseRefreshInterval("24h"), 86400);
  assert.equal(parseRows("chatgpt:7d@5m,deepseek@30s"), "chatgpt:7d@5m,deepseek@30s");
  assert.equal(parseRows("chatgpt, deepseek"), "chatgpt,deepseek");
  assert.equal(parseRows("chatgpt:7d@10s,deepseek@24h"), "chatgpt:7d@10s,deepseek@24h");
});

test("rejects out-of-range or malformed refresh intervals", () => {
  const invalid = ["", "5", "5x", "0s", "9s", "25h", "-5m", "5m30s", "1.5m"];
  for (const value of invalid) {
    assert.throws(() => parseRefreshInterval(value), CliError, value);
  }
  assert.throws(() => parseRows("chatgpt:7d@9s"), CliError);
  assert.throws(() => parseRows("deepseek@5m@30s"), CliError);
});

test("validates row intervals before resolving a serial port", async () => {
  await assert.rejects(run(["update", "--rows", "chatgpt:7d@9s"]), /10 秒/);
});
