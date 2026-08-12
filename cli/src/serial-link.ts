import { randomUUID } from "node:crypto";

import { Presets, SingleBar } from "cli-progress";
import { SerialPort } from "serialport";

import { DeviceError } from "./errors.js";
import type { JsonObject } from "./oauth.js";

const BAUD_RATE = 115_200;
const RETRY_INTERVAL_MS = 1_000;
// ESP32-C3 USB CDC 的 Arduino 接收缓冲很小。大行一次写入会在 loop() 消费前溢出，
// 造成 JSON 被截断；分段并留出消费时间。
const WRITE_CHUNK_BYTES = 64;
const WRITE_CHUNK_DELAY_MS = 4;

type ListedPort = Awaited<ReturnType<typeof SerialPort.list>>[number];

function delay(milliseconds: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

function isEspressifPort(port: ListedPort): boolean {
  if (port.vendorId?.toLowerCase() === "303a") return true;
  const description = [
    port.manufacturer,
    port.pnpId,
    port.serialNumber,
    port.productId,
  ]
    .filter(Boolean)
    .join(" ")
    .toLowerCase();
  return /espressif|usb jtag|esp32/.test(description);
}

function describePort(port: ListedPort): string {
  const label = [port.manufacturer, port.pnpId].filter(Boolean).join(" ");
  return label ? `${port.path}（${label}）` : port.path;
}

export function selectAutoPort(ports: ListedPort[]): string {
  const espressifPorts = ports.filter(isEspressifPort);
  const candidates = espressifPorts.length > 0 ? espressifPorts : ports.length === 1 ? ports : [];
  if (candidates.length === 1) return candidates[0].path;

  const found = ports.length > 0 ? `检测到：${ports.map(describePort).join("；")}` : "未检测到串口";
  if (candidates.length > 1) {
    throw new DeviceError(`发现多个 Espressif 串口，请用 --port 指定。${found}`);
  }
  throw new DeviceError(`未自动发现 Espressif 串口；请连接设备，或用 --port 指定。${found}`);
}

export async function resolvePort(explicitPort?: string): Promise<string> {
  if (explicitPort?.trim()) return explicitPort;
  try {
    return selectAutoPort(await SerialPort.list());
  } catch (error) {
    if (error instanceof DeviceError) throw error;
    const detail = error instanceof Error ? error.message : String(error);
    throw new DeviceError(`枚举串口失败：${detail}`, { cause: error });
  }
}

function openPort(port: SerialPort): Promise<void> {
  return new Promise((resolve, reject) => {
    port.open((error) => (error ? reject(error) : resolve()));
  });
}

export function isUnsupportedControlLineError(error: unknown): boolean {
  if (!(error instanceof Error)) return false;
  return /operation not supported|not supported, cannot set|ENOTSUP/i.test(error.message);
}

function setIdleControlLines(port: SerialPort): Promise<void> {
  return new Promise((resolve, reject) => {
    port.set({ dtr: false, rts: false }, (error) => {
      if (!error || isUnsupportedControlLineError(error)) resolve();
      else reject(error);
    });
  });
}

function flushInput(port: SerialPort): Promise<void> {
  return new Promise((resolve, reject) => {
    port.flush((error) => (error ? reject(error) : resolve()));
  });
}

function write(port: SerialPort, data: Buffer): Promise<void> {
  return new Promise((resolve, reject) => {
    port.write(data, (writeError) => {
      if (writeError) return reject(writeError);
      port.drain((drainError) => (drainError ? reject(drainError) : resolve()));
    });
  });
}

async function writeFramed(
  port: SerialPort,
  data: Buffer,
  onProgress?: (bytesSent: number) => void,
): Promise<void> {
  let bytesSent = 0;
  for (let offset = 0; offset < data.length; offset += WRITE_CHUNK_BYTES) {
    const chunk = data.subarray(offset, offset + WRITE_CHUNK_BYTES);
    await write(port, chunk);
    bytesSent += chunk.length;
    onProgress?.(bytesSent);
    if (offset + WRITE_CHUNK_BYTES < data.length) await delay(WRITE_CHUNK_DELAY_MS);
  }
}

function createSendProgress(totalBytes: number): SingleBar | undefined {
  if (totalBytes <= WRITE_CHUNK_BYTES || !process.stderr.isTTY) return undefined;
  const bar = new SingleBar(
    {
      format: "发送 [{bar}] {percentage}% | {value}/{total} B",
      clearOnComplete: true,
      hideCursor: true,
    },
    Presets.shades_classic,
  );
  bar.start(totalBytes, 0);
  return bar;
}

function closePort(port: SerialPort): Promise<void> {
  if (!port.isOpen) return Promise.resolve();
  return new Promise((resolve) => port.close(() => resolve()));
}

export async function requestDevice(
  path: string,
  payload: JsonObject,
  timeoutMs = 12_000,
): Promise<JsonObject> {
  const requestId = randomUUID().replaceAll("-", "").slice(0, 12);
  const encoded = Buffer.from(
    `${JSON.stringify({ request_id: requestId, ...payload })}\n`,
    "utf8",
  );
  const port = new SerialPort({ path, baudRate: BAUD_RATE, autoOpen: false });

  try {
    await openPort(port);
    // Native USB DTR/RTS also control reset and the ROM boot strap. Release both
    // immediately, then tolerate any reset by retrying the same request ID.
    await setIdleControlLines(port);
    await delay(250);
    await flushInput(port);

    return await new Promise<JsonObject>((resolve, reject) => {
      let receiveBuffer = "";
      let sending = false;
      let settled = false;
      let sendCount = 0;
      let retryTimer: NodeJS.Timeout | undefined;
      let timeoutTimer: NodeJS.Timeout | undefined;
      const progress = createSendProgress(encoded.length);

      const finish = (error?: Error, response?: JsonObject) => {
        if (settled) return;
        settled = true;
        if (retryTimer) clearInterval(retryTimer);
        if (timeoutTimer) clearTimeout(timeoutTimer);
        progress?.stop();
        port.off("data", onData);
        port.off("error", onError);
        if (error) reject(error);
        else resolve(response ?? {});
      };

      const send = async () => {
        if (sending || settled) return;
        sending = true;
        try {
          sendCount += 1;
          progress?.update(0);
          await writeFramed(port, encoded, (bytesSent) => progress?.update(bytesSent));
        } catch (error) {
          const detail = error instanceof Error ? error.message : String(error);
          finish(new DeviceError(`无法访问串口 ${path}：${detail}`, { cause: error }));
        } finally {
          sending = false;
        }
      };

      const onData = (chunk: Buffer) => {
        receiveBuffer += chunk.toString("utf8");
        for (;;) {
          const newline = receiveBuffer.indexOf("\n");
          if (newline < 0) break;
          const line = receiveBuffer.slice(0, newline).trim();
          receiveBuffer = receiveBuffer.slice(newline + 1);
          if (!line) continue;
          let response: unknown;
          try {
            response = JSON.parse(line);
          } catch {
            continue;
          }
          if (
            response !== null &&
            typeof response === "object" &&
            !Array.isArray(response) &&
            (response as JsonObject).request_id === requestId
          ) {
            const object = response as JsonObject;
            if (object.ok !== true) {
              const errorMessage = String(object.error ?? "设备拒绝了请求");
              finish(
                new DeviceError(
                  errorMessage === "未知命令"
                    ? "设备固件不支持该命令，请先烧录最新固件"
                    : errorMessage,
                ),
              );
            } else {
              finish(undefined, object);
            }
          }
        }
      };

      const onError = (error: Error) =>
        finish(new DeviceError(`无法访问串口 ${path}：${error.message}`, { cause: error }));
      port.on("data", onData);
      port.on("error", onError);

      retryTimer = setInterval(() => void send(), RETRY_INTERVAL_MS);
      timeoutTimer = setTimeout(
        () =>
          finish(
            new DeviceError(
              "等待设备响应超时；请确认设备已正常启动而不是停在 Boot 模式" +
                `（命令：${String(payload.cmd ?? "未知")}；请求 ID：${requestId}；` +
                `有效载荷：${encoded.length} B；已发送：${sendCount} 次）`,
            ),
          ),
        timeoutMs,
      );
      void send();
    });
  } catch (error) {
    if (error instanceof DeviceError) throw error;
    const detail = error instanceof Error ? error.message : String(error);
    throw new DeviceError(`无法访问串口 ${path}：${detail}`, { cause: error });
  } finally {
    await closePort(port);
  }
}
