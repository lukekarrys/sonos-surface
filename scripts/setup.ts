import {
  mkdirSync,
  existsSync,
  writeFileSync,
  renameSync,
  rmSync,
} from "node:fs";
import { dirname, join } from "node:path";
import {
  CONFIG,
  CORE_VERSION,
  CLI_VERSION,
  DEPS,
  LIBS,
  M5_VERSION,
  libraries,
  cli,
  main,
  run,
} from "./common.ts";
import { clangFormat } from "./format-cpp.ts";

async function download(url: string, path: string) {
  if (existsSync(path)) return;
  mkdirSync(dirname(path), { recursive: true });
  const temporary = `${path}.download`;
  try {
    const response = await fetch(url, { signal: AbortSignal.timeout(60000) });
    if (!response.ok)
      throw new Error(`Download failed: HTTP ${response.status}`);
    writeFileSync(temporary, Buffer.from(await response.arrayBuffer()));
    renameSync(temporary, path);
  } finally {
    rmSync(temporary, { force: true });
  }
}
export async function setup(hostOnly = false) {
  await run("clang++", ["--version"], { capture: true });
  console.log(
    (await run(clangFormat(), ["--version"], { capture: true })).trim(),
  );
  mkdirSync(LIBS, { recursive: true });
  writeFileSync(
    CONFIG,
    `directories:\n  user: ${JSON.stringify(join(DEPS, "arduino"))}\n`,
  );
  for (const [name, version] of [
    ["SurfaceJson", "3.12.0"],
    ["SurfaceXml", "11.0.0"],
  ]) {
    const lib = join(LIBS, name);
    mkdirSync(lib, { recursive: true });
    writeFileSync(
      join(lib, "library.properties"),
      `name=${name}\nversion=${version}\nauthor=Upstream contributors\nmaintainer=sonos-surface\nsentence=Portable upstream dependency\nparagraph=See upstream license\ncategory=Other\narchitectures=*\n`,
    );
  }
  await download(
    "https://raw.githubusercontent.com/nlohmann/json/v3.12.0/single_include/nlohmann/json.hpp",
    join(LIBS, "SurfaceJson/src/surface_json.hpp"),
  );
  await download(
    "https://raw.githubusercontent.com/nlohmann/json/v3.12.0/LICENSE.MIT",
    join(LIBS, "SurfaceJson/LICENSE"),
  );
  for (const file of ["tinyxml2.h", "tinyxml2.cpp", "LICENSE.txt"])
    await download(
      `https://raw.githubusercontent.com/leethomason/tinyxml2/11.0.0/${file}`,
      join(LIBS, "SurfaceXml", file === "LICENSE.txt" ? file : `src/${file}`),
    );
  if (hostOnly) {
    for (const file of [
      "src/utility/Button_Class.hpp",
      "src/utility/Button_Class.cpp",
      "LICENSE",
    ])
      await download(
        `https://raw.githubusercontent.com/m5stack/M5Unified/${M5_VERSION}/${file}`,
        join(DEPS, "host/M5Buttons", file),
      );
  } else {
    const version = await run("arduino-cli", ["version"], { capture: true });
    if (!version.includes(`Version: ${CLI_VERSION} `))
      throw new Error(
        `Arduino CLI ${CLI_VERSION} is required; install it from Arduino CLI releases.`,
      );
    const base = ["--config-file", CONFIG];
    const index = [
      "--additional-urls",
      "https://espressif.github.io/arduino-esp32/package_esp32_index.json",
    ];
    await run("arduino-cli", [...base, "core", "update-index", ...index]);
    await run("arduino-cli", [
      ...base,
      "core",
      "install",
      `esp32:esp32@${CORE_VERSION}`,
      ...index,
    ]);
    await run("arduino-cli", [...base, "lib", "update-index"]);
    await run("arduino-cli", [
      ...base,
      "lib",
      "install",
      "--no-deps",
      ...libraries,
    ]);
  }
  console.log(
    "Dependencies ready. Select the editor target with node --run cpp:configure.",
  );
}
if (import.meta.main)
  await main(async () => {
    const { values, positionals } = cli([], ["host-only"]);
    if (values.help) return console.log("node --run setup -- [--host-only]");
    if (positionals.length) throw new Error("Unexpected positional arguments");
    await setup(Boolean(values["host-only"]));
  });
