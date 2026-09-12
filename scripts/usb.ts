import { cli, main, numberOption, required, stringOption } from "./common.ts";
import { backgroundLine, readyPort, rejectionTokens } from "./serial-device.ts";
import type { DevicePort } from "./serial-device.ts";

// Credential-carrying and rebooting commands keep their own verified flows.
export const refusedCommands = ["config", "read-only"];

export interface RequestOptions {
  expect?: string;
  seconds?: number;
  lines?: number;
  print?: (line: string) => void;
}
// One command, one bounded reply. Errors never repeat the command or its
// arguments: an intent, a URL, or a room name may be private.
export async function usbRequest(
  port: DevicePort,
  command: string,
  options: RequestOptions = {},
): Promise<string> {
  const { expect, seconds = 10, lines = 40, print = console.log } = options;
  await port.clear();
  await port.write(`${command}\n`);
  const deadline = performance.now() + seconds * 1000;
  let printed = 0;
  while (performance.now() < deadline && printed < lines) {
    const line = await port.readLine(
      Math.min(500, Math.max(1, deadline - performance.now())),
    );
    if (!line) continue;
    print(line);
    printed++;
    if (rejectionTokens.some((token) => line.includes(token)))
      throw new Error("Device rejected the command (arguments omitted)");
    if (expect === undefined ? !backgroundLine(line) : line.includes(expect))
      return line;
  }
  throw new Error(
    "Expected device reply not observed within the bound (arguments omitted)",
  );
}
export function refused(command: string) {
  return refusedCommands.includes(command.split(" ")[0]);
}
export async function usb(argv = process.argv.slice(2), open = readyPort) {
  const { values, positionals } = cli(
    ["port", "command", "expect", "seconds", "lines"],
    [],
    argv,
  );
  if (values.help)
    return console.log(
      'node --run usb -- --port PORT --command "lifecycle-status" [--expect "lifecycles "] [--seconds N] [--lines M]',
    );
  if (positionals.length) throw new Error("Unexpected positional arguments");
  const name = required(values.port, "port");
  const command = required(values.command, "command");
  if (refused(command))
    throw new Error(
      "config and read-only are not available here: use node --run configure or node --run flash, which carry credentials and verify the reboot",
    );
  const seconds = numberOption(values.seconds, 10, 1, 600);
  const lines = numberOption(values.lines, 40, 1, 1000);
  const port = await open(name);
  try {
    await usbRequest(port, command, {
      expect: stringOption(values.expect),
      seconds,
      lines,
    });
  } finally {
    await port.close();
  }
}
if (import.meta.main) await main(() => usb());
