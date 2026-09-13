import { setTimeout as delay } from "node:timers/promises";
import { cli, main, numberOption, required } from "./common.ts";
import { readyPort } from "./serial-device.ts";
import type { DevicePort } from "./serial-device.ts";
import { usbRequest, usbTail } from "./usb.ts";

export type Point = [number, number];
// Injected samples are consumed one per 30 ms touch poll, so a drag is a
// sequence of commands; the device queue bounds how many may be in flight.
export const maxDragSteps = 31;
export function dragPoints(from: Point, to: Point, steps: number): Point[] {
  if (!Number.isInteger(steps) || steps < 2 || steps > maxDragSteps)
    throw new Error(`--steps must be an integer 2..${maxDragSteps}`);
  return Array.from({ length: steps }, (_, index) => {
    const progress = index / (steps - 1);
    return [
      Math.round(from[0] + (to[0] - from[0]) * progress),
      Math.round(from[1] + (to[1] - from[1]) * progress),
    ] as Point;
  });
}
export function touchCommands(points: Point[]): string[] {
  return [
    ...points.map(([x, y]) => `ui-touch ${x} ${y} 1`),
    "ui-touch release",
  ];
}
export function coordinate(value: string | undefined, maximum: number): number {
  if (!/^\d{1,3}$/.test(value ?? "") || Number(value) > maximum)
    throw new Error(`Screen coordinates are 0..367 by 0..447`);
  return Number(value);
}
function point(values: string[], index: number): Point {
  return [coordinate(values[index], 367), coordinate(values[index + 1], 447)];
}
// A screen that still requires a release swallows the first injected contact
// exactly as it swallows a finger held through boot, recovery, or navigation.
export async function releaseRequired(
  port: DevicePort,
  print?: (line: string) => void,
) {
  const line = await usbRequest(port, "ui-state", {
    expect: "ui-state ",
    print,
  });
  const state: unknown = JSON.parse(
    line.slice(line.indexOf("ui-state ") + "ui-state ".length),
  );
  const touch =
    typeof state === "object" && state && "touch" in state
      ? (state as { touch: { held?: boolean; cancelled?: boolean } }).touch
      : {};
  return Boolean(touch.held || touch.cancelled);
}
export async function uiAction(
  port: DevicePort,
  positionals: string[],
  options: { steps: number; intervalMs: number; tailSeconds: number },
  print: (line: string) => void = console.log,
) {
  const [action, ...rest] = positionals;
  const inject = async (commands: string[]) => {
    for (const [index, command] of commands.entries()) {
      if (index) await delay(options.intervalMs);
      await usbRequest(port, command, { expect: "[ui] inject ", print });
    }
    // The release action and the job it admits arrive after the last reply.
    await usbTail(port, options.tailSeconds, print);
  };
  if (action === "state") {
    if (rest.length) throw new Error("state takes no arguments");
    await usbRequest(port, "ui-state", { expect: "ui-state ", print });
    return;
  }
  if (action === "screen") {
    if (!["now", "rooms", "queue"].includes(rest[0]) || rest.length !== 1)
      throw new Error("screen takes now, rooms, or queue");
    await usbRequest(port, `ui-screen ${rest[0]}`, {
      expect: "[ui] navigation ",
      print,
    });
    return;
  }
  if (action === "button") {
    if (rest[0] !== "boot" || rest.length !== 1)
      throw new Error("button takes boot");
    await usbRequest(port, "ui-button boot", {
      expect: "[ui] inject button=",
      print,
    });
    return;
  }
  if (action === "release") {
    if (rest.length) throw new Error("release takes no arguments");
    return inject(["ui-touch release"]);
  }
  if (action === "tap" || action === "drag") {
    if (rest.length !== (action === "tap" ? 2 : 4))
      throw new Error(
        action === "tap" ? "tap takes X Y" : "drag takes X1 Y1 X2 Y2",
      );
    const points =
      action === "tap"
        ? [point(rest, 0)]
        : dragPoints(point(rest, 0), point(rest, 2), options.steps);
    const first = (await releaseRequired(port, print))
      ? ["ui-touch release"]
      : [];
    return inject([...first, ...touchCommands(points)]);
  }
  throw new Error("Use tap, drag, release, button, screen, or state");
}
export async function ui(argv = process.argv.slice(2), open = readyPort) {
  const { values, positionals } = cli(
    ["port", "steps", "interval-ms", "tail-seconds"],
    [],
    argv,
  );
  if (values.help)
    return console.log(
      "node --run ui -- --port PORT tap X Y\nnode --run ui -- --port PORT drag X1 Y1 X2 Y2 [--steps N] [--interval-ms 30]\nnode --run ui -- --port PORT release\nnode --run ui -- --port PORT button boot\nnode --run ui -- --port PORT screen now|rooms|queue\nnode --run ui -- --port PORT state\nTouch actions print what follows for [--tail-seconds 4].",
    );
  const name = required(values.port, "port");
  const options = {
    steps: numberOption(values.steps, 10, 2, maxDragSteps),
    intervalMs: numberOption(values["interval-ms"], 30, 0, 1000),
    tailSeconds: numberOption(values["tail-seconds"], 4, 0, 60),
  };
  const port = await open(name);
  try {
    await uiAction(port, positionals, options);
  } finally {
    await port.close();
  }
}
if (import.meta.main) await main(() => ui());
