import { isIPv4 } from "node:net";
import {
  cli,
  main,
  numberOption,
  required,
  run,
  stringOption,
} from "./common.ts";
import { compileHost } from "./host-build.ts";
import { hostProbeTarget } from "./host-test-targets.ts";
export async function buildProbe() {
  return compileHost(hostProbeTarget());
}
export async function probeAddress(
  binary: string,
  ip: string,
  args = ["", "0", "2", "1", "10000"],
  capture = false,
) {
  if (!isIPv4(ip)) throw new Error("Speaker IP must be an IPv4 address");
  return run(binary, [ip, ...args], { capture });
}
if (import.meta.main)
  await main(async () => {
    const { values, positionals } = cli([
      "ip",
      "uid",
      "queue-start",
      "queue-count",
      "samples",
      "interval-ms",
    ]);
    if (values.help)
      return console.log(
        "node --run probe -- --ip IPV4 [--uid UUID] [--queue-start N] [--queue-count 1..20] [--samples 1..360] [--interval-ms 0..60000]",
      );
    if (positionals.length) throw new Error("Unexpected positional arguments");
    const ip = required(values.ip, "ip");
    if (!isIPv4(ip)) throw new Error("Speaker IP must be an IPv4 address");
    const numbers = [
      numberOption(values["queue-start"], 0, 0, 4294967295),
      numberOption(values["queue-count"], 2, 1, 20),
      numberOption(values.samples, 1, 1, 360),
      numberOption(values["interval-ms"], 10000, 0, 60000),
    ];
    if (!numbers.every(Number.isInteger))
      throw new Error("Probe numeric options must be integers");
    await probeAddress(await buildProbe(), ip, [
      stringOption(values.uid) ?? "",
      ...numbers.map(String),
    ]);
  });
