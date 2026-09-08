import { SerialPort } from "serialport";
import { setTimeout as delay } from "node:timers/promises";
import { StringDecoder } from "node:string_decoder";

export interface DevicePort {
  readLine(timeoutMs?: number): Promise<string>;
  write(text: string): Promise<void>;
  clear(): Promise<void>;
  close(): Promise<void>;
}
export const serialOptions = {
  baudRate: 115200,
  autoOpen: false,
  rtscts: false,
  hupcl: false,
} as const;
// The POSIX binding opens/configures termios without issuing modem-line set ioctls.
// Never call set(): native USB DTR/RTS transitions can reset these boards.
export async function openPort(path: string): Promise<DevicePort> {
  const port = new SerialPort({ path, ...serialOptions });
  let failure = false;
  let pending = "";
  const decoder = new StringDecoder("utf8");
  const lines: string[] = [];
  let wake: (() => void) | undefined;
  port.on("error", () => {
    failure = true;
    wake?.();
  });
  port.on("close", () => {
    failure = true;
    wake?.();
  });
  port.on("data", (chunk: Buffer) => {
    pending += decoder.write(chunk);
    const split = pending.split("\n");
    pending = split.pop()!;
    lines.push(...split.map((line) => line.replace(/\r$/, "")));
    if (lines.length > 1024 || pending.length > 65536) {
      failure = true;
    }
    wake?.();
  });
  await new Promise<void>((resolve, reject) =>
    port.open((error) =>
      error ? reject(new Error("Device port unavailable")) : resolve(),
    ),
  );
  const closed = () => {
    if (failure) throw new Error("Device serial connection closed or failed");
  };
  return {
    async readLine(timeoutMs = 500) {
      closed();
      if (!lines.length)
        await new Promise<void>((resolve) => {
          const timer = setTimeout(() => {
            wake = undefined;
            resolve();
          }, timeoutMs);
          wake = () => {
            clearTimeout(timer);
            wake = undefined;
            resolve();
          };
        });
      closed();
      return lines.shift() ?? "";
    },
    async write(text) {
      closed();
      await new Promise<void>((resolve, reject) => {
        const timer = setTimeout(
          () =>
            reject(
              new Error(
                "Serial write timed out; inspect device before retrying",
              ),
            ),
          5000,
        );
        port.write(text, "utf8", (error) => {
          clearTimeout(timer);
          if (error) reject(new Error("Serial write failed (values omitted)"));
          else resolve();
        });
      });
    },
    async clear() {
      closed();
      await new Promise<void>((resolve, reject) =>
        port.flush((error) =>
          error ? reject(new Error("Serial flush failed")) : resolve(),
        ),
      );
      lines.length = 0;
      pending = "";
    },
    async close() {
      if (port.isOpen)
        await new Promise<void>((resolve) => port.close(() => resolve()));
    },
  };
}
export const listPorts = () => SerialPort.list();
export async function waitReady(port: DevicePort, seconds = 20, echo = false) {
  const deadline = performance.now() + seconds * 1000;
  while (performance.now() < deadline) {
    const line = await port.readLine(
      Math.min(500, Math.max(1, deadline - performance.now())),
    );
    if (echo && line) console.log(line);
    if (
      line.includes("READY: NFC/touch;") ||
      (line.includes("heartbeat ") && line.includes("busy=0"))
    )
      return;
  }
  throw new Error(
    "Application readiness not observed. Inspect serial/reset; a verified flash is not a verified boot.",
  );
}
export async function readyPort(
  name: string,
  open = openPort,
  seconds = 30,
): Promise<DevicePort> {
  const deadline = performance.now() + seconds * 1000;
  while (performance.now() < deadline) {
    let port: DevicePort | undefined;
    try {
      port = await open(name);
      await waitReady(
        port,
        Math.max(0.001, (deadline - performance.now()) / 1000),
      );
      return port;
    } catch {
      await port?.close();
      if (performance.now() >= deadline) break;
      await delay(250);
    }
  }
  throw new Error(
    "Device unavailable or application readiness not observed; inspect serial/reset before retrying.",
  );
}
export async function request(
  port: DevicePort,
  command: string,
  success: string,
  failures: string[] = [],
  seconds = 10,
) {
  await port.clear();
  await port.write(`${command}\n`);
  const deadline = performance.now() + seconds * 1000;
  while (performance.now() < deadline) {
    const line = await port.readLine();
    if (failures.some((token) => line.includes(token)))
      throw new Error(
        "Device rejected request or failed to save it (values omitted)",
      );
    if (line.includes(success))
      return line.slice(line.indexOf(success) + success.length);
  }
  throw new Error(
    "Device acknowledgment not received; inspect device before retrying.",
  );
}
