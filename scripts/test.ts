import { existsSync } from "node:fs";
import { join } from "node:path";
import { ROOT, LIBS, DEPS, main, run, sourceFiles } from "./common.ts";
import {
  compileHost,
  includes,
  deviceInclude,
  coreSources,
  sharedSources,
} from "./host-build.ts";

await main(async () => {
  await run(process.execPath, [
    "--test",
    ...sourceFiles(join(ROOT, "tests"), /\.test\.ts$/),
  ]);
  const buttons = existsSync(
    join(LIBS, "M5Unified/src/utility/Button_Class.hpp"),
  )
    ? join(LIBS, "M5Unified/src")
    : join(DEPS, "host/M5Buttons/src");
  const targets: [string, string[], string[]][] = [
    ["core_test", sharedSources(), includes],
    ["touch_test", [], [deviceInclude, includes[2]]],
    [
      "stick_button_test",
      [join(buttons, "utility/Button_Class.cpp")],
      [deviceInclude, includes[0], buttons],
    ],
    ["waveshare_ui_test", coreSources(), [deviceInclude, ...includes]],
    ["artwork_test", [], [deviceInclude, includes[0]]],
  ];
  for (const [name, sources, paths] of targets) {
    const binary = await compileHost(
      name,
      [...sources, join(ROOT, `tests/${name}.cpp`)],
      paths,
    );
    await run(binary);
  }
});
