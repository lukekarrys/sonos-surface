import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, writeFileSync, readFileSync, rmSync } from "node:fs";
import { join } from "node:path";
import { ROOT, run } from "../scripts/common.ts";
import { clangFormat } from "../scripts/format-cpp.ts";
import { compileArguments } from "../scripts/device.ts";

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
  for (const board of ["stick", "waveshare"] as const) {
    const normal = compileArguments(board);
    const editor = compileArguments(board, false, true);
    assert.deepEqual(
      editor.filter((arg) => arg !== "--only-compilation-database"),
      normal,
    );
    assert.ok(normal.includes("more"));
    assert.ok(
      normal.some((arg) => arg.includes(`-DSURFACE_${board.toUpperCase()}`)),
    );
  }
});
