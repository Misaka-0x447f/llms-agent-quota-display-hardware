import assert from "node:assert/strict";
import test from "node:test";

import { CliError } from "../src/errors.js";
import { parseUtcOffset } from "../src/cli.js";

test("parses positive and negative UTC offsets", () => {
  assert.equal(parseUtcOffset("+08:00"), 480);
  assert.equal(parseUtcOffset("-05:30"), -330);
});

test("rejects invalid UTC offsets", () => {
  assert.throws(() => parseUtcOffset("+15:00"), CliError);
  assert.throws(() => parseUtcOffset("08:60"), CliError);
  assert.throws(() => parseUtcOffset("UTC+8"), CliError);
});
