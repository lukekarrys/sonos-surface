import { existsSync, readFileSync, writeFileSync, renameSync } from "node:fs";
import { join, basename, resolve } from "node:path";
import { createInterface } from "node:readline";
import {
  CONFIG,
  CORE_VERSION,
  ROOT,
  cli,
  main,
  numberOption,
  required,
  run,
  sourceFiles,
  stringOption,
} from "./common.ts";
import { loadProfile } from "./config-profile.ts";
import type { Profile } from "./config-profile.ts";
import { configureDevice } from "./configure.ts";
import { listPorts, openPort, readyPort, request } from "./serial-device.ts";

export type Board = "stick" | "waveshare";
export function boardName(value: string): Board {
  if (value !== "stick" && value !== "waveshare")
    throw new Error("Device must be stick or waveshare");
  return value;
}
export function buildPath(board: Board, touch = false) {
  return join(ROOT, ".build", `${board}-${touch ? "touch" : "runtime"}`);
}
export function fqbn(board: Board) {
  return (
    "esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi,UploadSpeed=460800," +
    (board === "stick"
      ? "FlashSize=8M,PartitionScheme=default_8MB"
      : "FlashSize=16M,PartitionScheme=app3M_fat9M_16MB")
  );
}
const sketch = join(ROOT, "firmware/sonos_surface");
export function compileArguments(
  board: Board,
  touch = false,
  database = false,
) {
  return [
    "--config-file",
    CONFIG,
    "compile",
    "--fqbn",
    fqbn(board),
    "--libraries",
    join(ROOT, "libraries"),
    "--build-path",
    buildPath(board, touch),
    "--warnings",
    "more",
    "--build-property",
    `compiler.cpp.extra_flags=-std=gnu++17 -DSURFACE_${board.toUpperCase()} -DSURFACE_TOUCH_DIAGNOSTIC=${Number(touch)}`,
    "--build-property",
    'compiler.cpp.flags=-MMD -c "@{compiler.sdk.path}/flags/cpp_flags" {compiler.warning_flags} {compiler.optimization_flags} {compiler.common_werror_flags} -std=gnu++17',
    "--build-property",
    `tools.gen_esp32part.cmd="${process.execPath}" "${join(ROOT, "scripts/partitions.ts")}"`,
    ...(database ? ["--only-compilation-database"] : []),
    sketch,
  ];
}
export async function build(board: Board, touch = false) {
  let diagnostics = "";
  await run("arduino-cli", compileArguments(board, touch), {
    onOutput: (text) => {
      diagnostics += text;
    },
  });
  if (
    diagnostics
      .split("\n")
      .some(
        (line) =>
          (line.includes(`${ROOT}/libraries/`) ||
            line.includes(`${ROOT}/firmware/`) ||
            line.includes(`${buildPath(board, touch)}/sketch/`)) &&
          line.includes("warning:"),
      )
  )
    throw new Error(
      "Project-owned compiler warnings must be fixed before the build is green",
    );
}
export interface CompileCommand {
  directory: string;
  file: string;
  arguments: string[];
}
// GCC response files use whitespace and quoted/escaped arguments, not shell evaluation.
export function responseArguments(text: string): string[] {
  const result: string[] = [];
  let word = "",
    quote = "",
    escaped = false,
    active = false;
  for (const char of text) {
    if (escaped) {
      word += char;
      escaped = false;
      active = true;
    } else if (char === "\\") escaped = true;
    else if (quote) {
      if (char === quote) quote = "";
      else word += char;
    } else if (char === '"' || char === "'") {
      quote = char;
      active = true;
    } else if (/\s/.test(char)) {
      if (active) result.push(word);
      word = "";
      active = false;
    } else {
      word += char;
      active = true;
    }
  }
  if (quote || escaped) throw new Error("Malformed compiler response file");
  if (active) result.push(word);
  return result;
}
export function editorArguments(
  args: string[],
  directory: string,
  depth = 0,
): string[] {
  if (depth > 8) throw new Error("Recursive compiler response file");
  const expanded = args.flatMap((arg) =>
    arg.startsWith("@")
      ? editorArguments(
          responseArguments(
            readFileSync(resolve(directory, arg.slice(1)), "utf8"),
          ),
          directory,
          depth + 1,
        )
      : [arg],
  );
  // cpptools does not interpret GCC's prefix-relative include switches. Preserve
  // their exact search directories as ordinary -I flags in the editor database.
  if (depth) return expanded;
  let prefix = "";
  const result: string[] = [];
  for (let i = 0; i < expanded.length; i++) {
    const arg = expanded[i];
    if (arg === "-iprefix") prefix = expanded[++i];
    else if (arg === "-iwithprefixbefore")
      result.push(`-I${resolve(directory, prefix + expanded[++i])}`);
    else result.push(arg);
  }
  return result;
}
export function editorDatabase(
  entries: CompileCommand[],
  board: Board,
): CompileCommand[] {
  entries = entries.map((entry) => ({
    ...entry,
    arguments: editorArguments(entry.arguments, entry.directory),
  }));
  const owned = sourceFiles(join(ROOT, "libraries"), /\.cpp$/);
  const mapped = entries.map((entry) => {
    const source =
      owned.find((path) =>
        entry.file.includes(
          `/libraries/${path.slice(join(ROOT, "libraries").length + 1).replace("/src/", "/")}`,
        ),
      ) ?? owned.find((path) => entry.file === path);
    if (!source) return entry;
    return {
      ...entry,
      file: source,
      arguments: entry.arguments.map((arg) =>
        arg === entry.file ? source : arg,
      ),
    };
  });
  const firmware = entries.find((entry) =>
    entry.file.endsWith("/sketch/sonos_surface.ino.cpp"),
  );
  if (!firmware)
    throw new Error("Compilation database lacks firmware translation unit");
  const ino = join(sketch, "sonos_surface.ino");
  mapped.push({
    ...firmware,
    file: ino,
    arguments: [
      firmware.arguments[0],
      "-x",
      "c++",
      "-include",
      "Arduino.h",
      ...firmware.arguments
        .slice(1)
        .map((arg) => (arg === firmware.file ? ino : arg)),
    ],
  });
  for (const source of owned) {
    const entry = mapped.find((command) => command.file === source);
    if (!entry)
      throw new Error(`Compilation database lacks ${basename(source)}`);
    const flags = entry.arguments.join(" ");
    if (
      !entry.arguments[0].includes("xtensa-esp32s3-elf-g++") ||
      !flags.includes(`-DSURFACE_${board.toUpperCase()}`) ||
      !flags.includes("/cores/esp32") ||
      !flags.includes("SurfaceJson")
    )
      throw new Error(
        "Compilation database has incomplete ESP32 compiler context",
      );
  }
  return mapped;
}
export async function configureCpp(board: Board) {
  let diagnostics = "";
  await run("arduino-cli", compileArguments(board, false, true), {
    onOutput: (text) => {
      diagnostics += text;
    },
  });
  // Arduino CLI can exit zero after failed dependency discovery in database mode.
  if (/error occurred detecting libraries|fatal error:/i.test(diagnostics))
    throw new Error(
      "Arduino library discovery failed; editor database was not replaced",
    );
  const entries = JSON.parse(
    readFileSync(join(buildPath(board), "compile_commands.json"), "utf8"),
  ) as CompileCommand[];
  const database = editorDatabase(entries, board);
  const target = join(ROOT, ".build/compile_commands.json");
  writeFileSync(`${target}.tmp`, JSON.stringify(database, null, 2) + "\n");
  renameSync(`${target}.tmp`, target);
  console.log(
    `C++ editor target: ${board}. Verified owned library and firmware entries in .build/compile_commands.json.`,
  );
}
function hardwareDirectory(board: Board, touch = false): string {
  const options = JSON.parse(
    readFileSync(join(buildPath(board, touch), "build.options.json"), "utf8"),
  ) as { hardwareFolders: string; customBuildProperties: string };
  if (
    !options.customBuildProperties
      .split(/[ ,]+/)
      .includes(`-DSURFACE_${board.toUpperCase()}`)
  )
    throw new Error("Built image board differs; rebuild before flashing");
  return options.hardwareFolders.split(",")[0];
}
export async function upload(board: Board, port: string, touch = false) {
  if (!existsSync(join(buildPath(board, touch), "sonos_surface.ino.bin")))
    throw new Error(`Build first: node --run build:${board}`);
  const platform = join(
    hardwareDirectory(board, touch),
    `esp32/hardware/esp32/${CORE_VERSION}/platform.txt`,
  );
  const prefix = "tools.esptool_py.upload.pattern_args=";
  const recipe = readFileSync(platform, "utf8")
    .split(/\r?\n/)
    .find((line) => line.startsWith(prefix));
  if (!recipe?.includes("--after hard-reset"))
    throw new Error(
      "Pinned upload recipe changed; inspect reset behavior before flashing",
    );
  await run("arduino-cli", [
    "--config-file",
    CONFIG,
    "upload",
    "--fqbn",
    fqbn(board),
    "--port",
    port,
    "--input-dir",
    buildPath(board, touch),
    "--upload-property",
    recipe
      .replace(prefix, "upload.pattern_args=")
      .replace("--after hard-reset", "--after watchdog-reset"),
    "--upload-property",
    "upload.speed=115200",
    "--upload-property",
    'upload.pattern="{path}/{cmd}" {upload.pattern_args}',
    sketch,
  ]);
}
export interface FlashOperations {
  build: typeof build;
  upload: typeof upload;
  ready: (port: string) => Promise<void>;
  configure: (port: string, profile: Profile) => Promise<void>;
}
const operations: FlashOperations = {
  build,
  upload,
  async ready(name) {
    const port = await readyPort(name);
    await port.close();
  },
  configure: configureDevice,
};
export async function flash(
  board: Board,
  port: string,
  config?: string,
  envFile?: string,
  touch = false,
  ops = operations,
) {
  if (envFile && !config)
    throw new Error("--env-file requires --config when flashing");
  // Resolve before builds, USB access, or writes. Never place credentials in process args.
  const profile = config ? loadProfile(config, envFile) : undefined;
  await ops.build(board, touch);
  let uploadError: unknown;
  try {
    await ops.upload(board, port, touch);
  } catch (error) {
    uploadError = error;
  }
  if (uploadError)
    console.error(
      "Upload failed; checking application separately. Readiness cannot certify a new image.",
    );
  await ops.ready(port);
  if (uploadError) throw uploadError;
  console.log(
    "Application ready (peripheral readiness is reported separately).",
  );
  if (profile) await ops.configure(port, profile);
}
export async function monitor(name: string, seconds: number) {
  const port = await openPort(name);
  let stopped = false;
  let writeError: unknown;
  const stop = () => {
    stopped = true;
  };
  process.on("SIGINT", stop);
  process.on("SIGTERM", stop);
  const input = process.stdin.isTTY
    ? createInterface({ input: process.stdin })
    : undefined;
  let writes = Promise.resolve();
  input?.on("line", (line) => {
    writes = writes
      .then(() => port.write(`${line}\n`))
      .catch((error) => {
        writeError = error;
        stopped = true;
      });
  });
  try {
    const deadline = seconds ? performance.now() + seconds * 1000 : Infinity;
    while (!stopped && performance.now() < deadline) {
      const line = await port.readLine();
      if (line) console.log(line);
    }
    await writes;
    if (writeError) throw writeError;
  } finally {
    input?.close();
    process.stdin.pause();
    process.off("SIGINT", stop);
    process.off("SIGTERM", stop);
    await port.close();
  }
}
export async function device(argv = process.argv.slice(2)) {
  const { values, positionals } = cli(
    ["port", "device", "seconds", "config", "env-file"],
    ["download-mode", "touch-diagnostic"],
    argv,
  );
  if (values.help)
    return console.log(
      "node --run build:stick|build:waveshare|flash:stick|flash:waveshare -- [--port PORT] [--config PATH] [--env-file PATH] [--touch-diagnostic]\nnode --run cpp:configure -- [stick|waveshare]\nnode --run monitor|reset|reboot -- --device stick|waveshare --port PORT [--seconds N] [--download-mode]\nnode --run ports",
    );
  const [action, positionalBoard, ...extra] = positionals;
  if (
    ![
      "build",
      "flash",
      "cpp:configure",
      "monitor",
      "reset",
      "reboot",
      "ports",
    ].includes(action)
  )
    throw new Error("Unknown device action");
  if (extra.length || (positionalBoard && values.device))
    throw new Error("Unexpected or duplicate device argument");
  if (action === "ports") {
    console.log(JSON.stringify(await listPorts(), null, 2));
    return;
  }
  const board = boardName(
    positionalBoard ??
      stringOption(values.device) ??
      (action === "cpp:configure" ? "stick" : ""),
  );
  const touch = Boolean(values["touch-diagnostic"]);
  if (touch && (board !== "waveshare" || !["build", "flash"].includes(action)))
    throw new Error("--touch-diagnostic requires a Waveshare build or flash");
  if (values["download-mode"] && action !== "reset")
    throw new Error("--download-mode is only for reset");
  if ((values.config || values["env-file"]) && action !== "flash")
    throw new Error("--config and --env-file are only for flash");
  if (values.seconds !== undefined && action !== "monitor")
    throw new Error("--seconds is only for monitor");
  if (action === "build") return build(board, touch);
  if (action === "cpp:configure") return configureCpp(board);
  const port = required(values.port, "port");
  if (action === "flash")
    return flash(
      board,
      port,
      stringOption(values.config),
      stringOption(values["env-file"]),
      touch,
    );
  if (action === "monitor")
    return monitor(port, numberOption(values.seconds, 0, 0, 86400));
  if (action === "reset") {
    // Recovery must work without a successful build or any build-directory state.
    const details = JSON.parse(
      await run(
        "arduino-cli",
        [
          "--config-file",
          CONFIG,
          "board",
          "details",
          "--fqbn",
          fqbn(board),
          "--format",
          "json",
        ],
        { capture: true },
      ),
    ) as { build_properties: string[] };
    const prefix = "runtime.tools.esptool_py.path=";
    const toolPath = details.build_properties
      .find((property) => property.startsWith(prefix))
      ?.slice(prefix.length);
    if (!toolPath)
      throw new Error("Pinned esptool is unavailable; run node --run setup");
    const esptool = join(toolPath, "esptool");
    let resetError: unknown;
    try {
      await run(esptool, [
        "--chip",
        "esp32s3",
        "--port",
        port,
        "--before",
        values["download-mode"] ? "no-reset" : "default-reset",
        "--after",
        "watchdog-reset",
        "--connect-attempts",
        "2",
        "chip-id",
      ]);
    } catch (error) {
      resetError = error;
    }
    await operations.ready(port);
    if (resetError) throw resetError;
    return;
  }
  const connection = await readyPort(port);
  try {
    await request(connection, "reboot", "REBOOTING", ["REBOOT_BUSY"], 5);
  } finally {
    await connection.close();
  }
  const rebooted = await readyPort(port);
  await rebooted.close();
  console.log("Application ready.");
}
if (import.meta.main) await main(() => device());
