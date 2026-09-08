import { readFileSync, writeFileSync } from "node:fs";
import { cli, main, numberOption, required, stringOption } from "./common.ts";
import { readyPort, request } from "./serial-device.ts";

function fitAxis(raw: number[], expected: number[]): [number, number] {
  const mean = (values: number[]) =>
    values.reduce((a, b) => a + b, 0) / values.length;
  const mr = mean(raw),
    me = mean(expected);
  const variance = raw.reduce((sum, x) => sum + (x - mr) ** 2, 0);
  if (variance < 100)
    throw new Error("Samples do not span enough of the screen");
  const scale =
    raw.reduce((sum, x, i) => sum + (x - mr) * (expected[i] - me), 0) /
    variance;
  return [scale, me - scale * mr];
}
export function fitSamples(text: string, controller: number) {
  if (![21, 56].includes(controller))
    throw new Error("Controller must be 0x15 or 0x38");
  const pattern =
    /\[calibration\] target=(\d+) expected=(\d+),(\d+) first=(\d+),(\d+) last=(\d+),(\d+) release=(\d+),(\d+)/g;
  let rows: number[][] = [];
  for (const match of text.matchAll(pattern)) {
    const [target, x, y, rx, ry, lx, ly, ex, ey] = match.slice(1).map(Number);
    if (target < 1 || target > 6) throw new Error("Unknown target number");
    if (target === 1) rows = [];
    if (target !== rows.length + 1)
      throw new Error("Expected an ordered six-target run");
    if (
      rx >= 368 ||
      ry >= 448 ||
      Math.max(
        Math.abs(rx - lx),
        Math.abs(ry - ly),
        Math.abs(rx - ex),
        Math.abs(ry - ey),
      ) > 16
    )
      throw new Error("Invalid or moving contact; repeat diagnostic");
    if (
      x !== [92, 276][(target - 1) % 2] ||
      y !== [140, 270, 406][Math.floor((target - 1) / 2)]
    )
      throw new Error("Unexpected diagnostic target");
    rows.push([x, y, rx, ry]);
  }
  if (rows.length !== 6)
    throw new Error("Need a complete six-target diagnostic run");
  const [xs, xo] = fitAxis(
    rows.map((r) => r[2]),
    rows.map((r) => r[0]),
  );
  const [ys, yo] = fitAxis(
    rows.map((r) => r[3]),
    rows.map((r) => r[1]),
  );
  if (
    xs < 0.5 ||
    xs > 1.5 ||
    ys < 0.5 ||
    ys > 1.5 ||
    Math.abs(xo) > 112 ||
    Math.abs(yo) > 112
  )
    throw new Error("Fit outside supported correction bounds");
  const residual = Math.max(
    ...rows.map(([x, y, rx, ry]) =>
      Math.max(Math.abs(xs * rx + xo - x), Math.abs(ys * ry + yo - y)),
    ),
  );
  if (residual > 16)
    throw new Error(
      `Fit residual ${residual.toFixed(1)}px exceeds 16px; inspect/repeat samples`,
    );
  return {
    calibration: {
      version: 1,
      controller,
      x_scale: xs,
      x_offset: xo,
      y_scale: ys,
      y_offset: yo,
    },
    residual,
  };
}
if (import.meta.main)
  await main(async () => {
    const { values, positionals } = cli([
      "samples",
      "controller",
      "output",
      "file",
      "port",
    ]);
    if (values.help)
      return console.log(
        "node --run calibrate -- --samples LOG [--controller 0x15|0x38] [--output JSON]\nnode --run calibrate -- --port PORT [--file JSON]",
      );
    if (positionals.length) throw new Error("Unexpected positional arguments");
    if (values.samples) {
      if (values.file || values.port)
        throw new Error(
          "Fit samples separately; review JSON before --file --port",
        );
      const { calibration, residual } = fitSamples(
        readFileSync(required(values.samples, "samples"), "utf8"),
        numberOption(values.controller, 21, 21, 56),
      );
      const payload = JSON.stringify(calibration, null, 2) + "\n";
      console.log(
        payload +
          `Max training residual: ${residual.toFixed(2)}px (fresh taps still required)`,
      );
      if (values.output)
        writeFileSync(required(values.output, "output"), payload);
    } else {
      if (values.output || values.controller)
        throw new Error("--output and --controller require --samples");
      const name = required(values.port, "port");
      const file = stringOption(values.file);
      const payload = file
        ? JSON.stringify(JSON.parse(readFileSync(file, "utf8")))
        : undefined;
      if (payload && Buffer.byteLength(payload) > 1024)
        throw new Error("Calibration too large");
      const port = await readyPort(name);
      try {
        const result = await request(
          port,
          "touch-calibration" + (payload ? ` ${payload}` : ""),
          payload ? "CALIBRATION_SAVED" : "CALIBRATION ",
          ["CALIBRATION_INVALID", "CALIBRATION_SAVE_FAILED"],
        );
        console.log(payload ? "Calibration saved." : result);
      } finally {
        await port.close();
      }
    }
  });
