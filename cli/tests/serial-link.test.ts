import assert from "node:assert/strict";
import test from "node:test";

import { DeviceError } from "../src/errors.js";
import { isUnsupportedControlLineError, selectAutoPort } from "../src/serial-link.js";

test("ignores only unsupported USB control-line operations", () => {
  assert.equal(
    isUnsupportedControlLineError(new Error("Operation not supported, cannot set")),
    true,
  );
  assert.equal(isUnsupportedControlLineError(new Error("ENOTSUP")), true);
  assert.equal(isUnsupportedControlLineError(new Error("Permission denied")), false);
  assert.equal(isUnsupportedControlLineError("not supported"), false);
});

test("auto-selects the Espressif serial device", () => {
  const ports = [
    { path: "/dev/ttyUSB0", vendorId: "1a86", manufacturer: "QinHeng" },
    { path: "/dev/ttyACM0", vendorId: "303A", manufacturer: "Espressif" },
  ] as Parameters<typeof selectAutoPort>[0];
  assert.equal(selectAutoPort(ports), "/dev/ttyACM0");
});

test("requires --port when automatic discovery is ambiguous", () => {
  const ports = [
    { path: "/dev/ttyACM0", vendorId: "303a", manufacturer: "Espressif" },
    { path: "/dev/ttyACM1", vendorId: "303a", manufacturer: "Espressif" },
  ] as Parameters<typeof selectAutoPort>[0];
  assert.throws(() => selectAutoPort(ports), DeviceError);
});
