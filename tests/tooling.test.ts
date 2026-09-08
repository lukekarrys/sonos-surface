import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, writeFileSync, readFileSync, rmSync } from "node:fs";
import { join } from "node:path";
import { ROOT, run, sourceFiles } from "../scripts/common.ts";
import { clangFormat } from "../scripts/format-cpp.ts";
import { compileArguments, editorDatabase, device } from "../scripts/device.ts";
import {
  hardwareTargets,
  hardwareTargetId,
  buildPath,
  fqbn,
} from "../scripts/hardware-targets.ts";
import { checkTasks } from "../scripts/check.ts";

test("process arguments remain literal and failures/signals propagate", async () => {
  const args = [
    "space value",
    "`echo unexpected`",
    "$(echo unexpected)",
    'quote"value',
    "line\nvalue",
  ];
  const output = await run(
    process.execPath,
    ["-e", "console.log(JSON.stringify(process.argv.slice(1)))", ...args],
    { capture: true },
  );
  assert.deepEqual(JSON.parse(output), args);
  await assert.rejects(
    run(process.execPath, ["-e", "process.exit(9)"], { capture: true }),
    /exit 9/,
  );
  await assert.rejects(
    run(process.execPath, ["-e", 'process.kill(process.pid,"SIGTERM")'], {
      capture: true,
    }),
    /SIGTERM/,
  );
  await assert.rejects(
    run("/nonexistent/surface-tool", [], { capture: true }),
    /Cannot start/,
  );
});
test("both formatter check modes reject broken fixtures without modifying them", async (t) => {
  const root = mkdtempSync(join(ROOT, ".build/format-fixture-"));
  t.after(() => rmSync(root, { recursive: true, force: true }));
  const ts = join(root, "fixture.ts");
  const cpp = join(root, "fixture.cpp");
  writeFileSync(ts, "const  value={a:1,b:2}\n");
  writeFileSync(cpp, "int main(){return 0;}\n");
  const prettier = join(ROOT, "node_modules/prettier/bin/prettier.cjs");
  const before = [readFileSync(ts, "utf8"), readFileSync(cpp, "utf8")];
  await assert.rejects(
    run(
      process.execPath,
      [prettier, "--ignore-path", "/dev/null", "--check", ts],
      { capture: true },
    ),
  );
  await assert.rejects(
    run(clangFormat(), ["--dry-run", "--Werror", cpp], { capture: true }),
  );
  assert.deepEqual(
    [readFileSync(ts, "utf8"), readFileSync(cpp, "utf8")],
    before,
  );
  await run(
    process.execPath,
    [prettier, "--ignore-path", "/dev/null", "--write", ts],
    { capture: true },
  );
  await run(clangFormat(), ["-i", cpp], { capture: true });
  await run(
    process.execPath,
    [prettier, "--ignore-path", "/dev/null", "--check", ts],
    { capture: true },
  );
  await run(clangFormat(), ["--dry-run", "--Werror", cpp], { capture: true });
});
test("editor target generation uses the normal compiler options", () => {
  for (const { id, define } of Object.values(hardwareTargets)) {
    const normal = compileArguments(id);
    const editor = compileArguments(id, false, true);
    assert.deepEqual(
      editor.filter((arg) => arg !== "--only-compilation-database"),
      normal,
    );
    assert.ok(normal.includes("more"));
    assert.ok(normal.some((arg) => arg.includes(`-D${define}`)));
  }
});

test("hardware registry accepts exactly the current model IDs and rejects aliases", async () => {
  assert.deepEqual(Object.keys(hardwareTargets), ["stick-s3", "ws-1.8"]);
  for (const id of Object.keys(hardwareTargets))
    assert.equal(hardwareTargetId(id), id);
  for (const id of [
    "stick",
    "waveshare",
    "",
    "toString",
    "__proto__",
    "ws-1.8-unit2",
  ]) {
    assert.throws(() => hardwareTargetId(id), /Hardware target must be/);
    for (const action of [
      "build",
      "flash",
      "cpp:configure",
      "monitor",
      "reset",
      "reboot",
    ])
      await assert.rejects(
        device([action, id, "--port", "fake"]),
        /Hardware target must be/,
      );
  }
  for (const id of Object.keys(hardwareTargets))
    await assert.rejects(device(["flash", id]), /--port/);
});

test("hardware targets retain their FQBN facts and isolated build outputs", () => {
  const common =
    "esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi,UploadSpeed=460800,";
  assert.equal(
    fqbn("stick-s3"),
    common + "FlashSize=8M,PartitionScheme=default_8MB",
  );
  assert.equal(
    fqbn("ws-1.8"),
    common + "FlashSize=16M,PartitionScheme=app3M_fat9M_16MB",
  );
  assert.equal(buildPath("stick-s3"), join(ROOT, ".build/stick-s3-runtime"));
  assert.equal(buildPath("ws-1.8"), join(ROOT, ".build/ws-1.8-runtime"));
  assert.equal(buildPath("ws-1.8", true), join(ROOT, ".build/ws-1.8-touch"));
  assert.throws(
    () => compileArguments("stick-s3", true),
    /no touch diagnostic/,
  );
  assert.ok(
    compileArguments("ws-1.8", true).some((arg) =>
      arg.includes("-DSURFACE_TOUCH_DIAGNOSTIC=1"),
    ),
  );
});

test("editor databases validate the exact model define and retain firmware mapping", () => {
  const sources = sourceFiles(join(ROOT, "libraries"), /\.cpp$/);
  for (const { id, define } of Object.values(hardwareTargets)) {
    const files = [
      ...sources,
      join(buildPath(id), "sketch/sonos_surface.ino.cpp"),
    ];
    const entries = (flag: string) =>
      files.map((file) => ({
        directory: ROOT,
        file,
        arguments: [
          "xtensa-esp32s3-elf-g++",
          flag,
          "-I/cores/esp32",
          "-ISurfaceJson",
          file,
        ],
      }));
    const database = editorDatabase(entries(`-D${define}`), id);
    for (const source of [
      ...sources,
      join(ROOT, "firmware/sonos_surface/sonos_surface.ino"),
    ])
      assert.ok(
        database
          .find((entry) => entry.file === source)
          ?.arguments.includes(`-D${define}`),
      );
    for (const wrong of [
      "SURFACE_STICK",
      "SURFACE_WAVESHARE",
      `${define}_OTHER`,
      ...Object.values(hardwareTargets)
        .filter((target) => target.id !== id)
        .map((target) => target.define),
    ])
      assert.throws(
        () => editorDatabase(entries(`-D${wrong}`), id),
        /incomplete ESP32 compiler context/,
      );
  }
});

test("full checks build every current target through the matching package task", () => {
  const { scripts } = JSON.parse(
    readFileSync(join(ROOT, "package.json"), "utf8"),
  ) as { scripts: Record<string, string> };
  assert.equal(scripts["check:full"], "node scripts/check.ts --full");
  assert.deepEqual(checkTasks(true).slice(checkTasks().length), [
    "build:stick-s3",
    "build:ws-1.8",
  ]);
  for (const { id } of Object.values(hardwareTargets))
    for (const action of ["build", "flash"])
      assert.equal(
        scripts[`${action}:${id}`],
        `node scripts/device.ts ${action} ${id}`,
      );
  for (const alias of ["stick", "waveshare"])
    for (const action of ["build", "flash"])
      assert.equal(scripts[`${action}:${alias}`], undefined);
});
