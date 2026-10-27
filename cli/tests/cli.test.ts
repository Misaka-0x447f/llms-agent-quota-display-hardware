import assert from "node:assert/strict";
import test from "node:test";

import { CliError } from "../src/errors.js";
import { parseRefreshInterval, parseUtcOffset, run } from "../src/cli.js";

test("parses positive and negative UTC offsets", () => {
  assert.equal(parseUtcOffset("+08:00"), 480);
  assert.equal(parseUtcOffset("-05:30"), -330);
});

test("rejects invalid UTC offsets", () => {
  assert.throws(() => parseUtcOffset("+15:00"), CliError);
  assert.throws(() => parseUtcOffset("08:60"), CliError);
  assert.throws(() => parseUtcOffset("UTC+8"), CliError);
});


test("uses seconds in the device payload and accepts both interval boundaries", () => {
  assert.deepEqual(parseRefreshInterval(undefined, "50"), { refresh_seconds: 50 });
  assert.deepEqual(parseRefreshInterval(undefined, "86400"), { refresh_seconds: 86400 });
});

test("preserves the legacy minutes payload and leaves unspecified intervals unchanged", () => {
  assert.deepEqual(parseRefreshInterval("1", undefined), { refresh_minutes: 1 });
  assert.deepEqual(parseRefreshInterval("1440", undefined), { refresh_minutes: 1440 });
  assert.deepEqual(parseRefreshInterval(undefined, undefined), {});
});

test("rejects seconds outside the range or not expressed as an integer", () => {
  for (const value of ["49", "86401", "0", "-50", "50.5", "5e1", "", "NaN", "Infinity"]) {
    assert.throws(() => parseRefreshInterval(undefined, value), CliError, value);
  }
});

test("retains validation of legacy minute intervals", () => {
  for (const value of ["0", "1441", "1.5", "", "NaN"]) {
    assert.throws(() => parseRefreshInterval(value, undefined), CliError, value);
  }
});

test("rejects conflicting interval options before resolving a serial port", async () => {
  assert.throws(() => parseRefreshInterval("1", "50"), /不能同时指定/);
  await assert.rejects(
    run(["update", "--interval", "1", "--interval-seconds", "50"]),
    /不能同时指定/,
  );
});

test("validates the seconds option before resolving a serial port", async () => {
  await assert.rejects(run(["update", "--interval-seconds", "49"]), /50～86400 秒/);
});
