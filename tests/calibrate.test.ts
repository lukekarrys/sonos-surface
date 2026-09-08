import { test } from "node:test";
import assert from "node:assert/strict";
import { fitSamples } from "../scripts/calibrate.ts";
test("measured fit and malformed/incomplete calibration runs", () => {
  const raw = [
    [87, 120],
    [307, 136],
    [69, 289],
    [304, 279],
    [72, 441],
    [312, 427],
  ];
  const lines = raw.map(
    ([rx, ry], i) =>
      `[calibration] target=${i + 1} expected=${[92, 276][i % 2]},${[140, 270, 406][Math.floor(i / 2)]} first=${rx},${ry} last=${rx},${ry} release=${rx},${ry}`,
  );
  const text = lines.join("\n");
  const { calibration, residual } = fitSamples(text, 21);
  assert.ok(Math.abs(calibration.x_scale - 0.792093109) < 1e-9);
  assert.ok(Math.abs(calibration.y_offset - 27.650440782) < 1e-9);
  assert.ok(residual < 10);
  for (const bad of [
    lines.slice(0, 5).join("\n"),
    lines.slice(1).join("\n"),
    text + "\n" + lines[0],
    text.replace("first=87,120", "first=0,0"),
  ])
    assert.throws(() => fitSamples(bad, 21));
});
