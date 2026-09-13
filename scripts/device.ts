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
import { hostCompilationDatabase } from "./host-build.ts";
import type { HostTarget } from "./host-build.ts";
import { hostTestTargets, hostProbeTarget } from "./host-test-targets.ts";
import { configureDevice } from "./configure.ts";
import {
  deviceMessage,
  listPorts,
  openPort,
  readyPort,
  request,
  waitReady,
} from "./serial-device.ts";
import type { DevicePort } from "./serial-device.ts";

import {
  hardwareTargets,
  hardwareTargetId,
  buildPath,
  fqbn,
} from "./hardware-targets.ts";
import type { HardwareTargetId } from "./hardware-targets.ts";

const sketch = join(ROOT, "firmware/sonos_surface");
export function compileArguments(
  targetId: HardwareTargetId,
  touch = false,
  database = false,
) {
  return [
    "--config-file",
    CONFIG,
    "compile",
    "--fqbn",
    fqbn(targetId),
    "--libraries",
    join(ROOT, "libraries"),
    "--build-path",
    buildPath(targetId, touch),
    "--warnings",
    "more",
    "--build-property",
    `compiler.cpp.extra_flags=-std=gnu++17 -D${hardwareTargets[targetId].define} -DSURFACE_TOUCH_DIAGNOSTIC=${Number(touch)}`,
    "--build-property",
    'compiler.cpp.flags=-MMD -c "@{compiler.sdk.path}/flags/cpp_flags" {compiler.warning_flags} {compiler.optimization_flags} {compiler.common_werror_flags} -std=gnu++17',
    "--build-property",
    `tools.gen_esp32part.cmd="${process.execPath}" "${join(ROOT, "scripts/partitions.ts")}"`,
    ...(database ? ["--only-compilation-database"] : []),
    sketch,
  ];
}
export async function build(targetId: HardwareTargetId, touch = false) {
  let diagnostics = "";
  await run("arduino-cli", compileArguments(targetId, touch), {
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
            line.includes(`${buildPath(targetId, touch)}/sketch/`)) &&
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
  targetId: HardwareTargetId,
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
  for (const source of [...owned, ino, firmware.file]) {
    const entry = mapped.find((command) => command.file === source);
    if (!entry)
      throw new Error(`Compilation database lacks ${basename(source)}`);
    const flags = entry.arguments.join(" ");
    if (
      !entry.arguments[0].includes("xtensa-esp32s3-elf-g++") ||
      !entry.arguments.includes(`-D${hardwareTargets[targetId].define}`) ||
      Object.values(hardwareTargets).some(
        (target) =>
          target.id !== targetId &&
          entry.arguments.includes(`-D${target.define}`),
      ) ||
      !flags.includes("/cores/esp32") ||
      !flags.includes("SurfaceJson")
    )
      throw new Error(
        "Compilation database has incomplete ESP32 compiler context",
      );
  }
  return mapped;
}
export function mergeEditorDatabase(
  embedded: CompileCommand[],
  host: CompileCommand[],
): CompileCommand[] {
  const merged = new Map<string, CompileCommand>();
  const isTest = (file: string) => file.startsWith(join(ROOT, "tests") + "/");
  for (const entry of embedded) {
    const file = resolve(entry.directory, entry.file);
    if (!isTest(file) && !merged.has(file))
      merged.set(file, { ...entry, file });
  }
  for (const entry of host) {
    const file = resolve(entry.directory, entry.file);
    const previous = merged.get(file);
    if (
      previous &&
      isTest(file) &&
      JSON.stringify(previous.arguments) !== JSON.stringify(entry.arguments)
    )
      throw new Error(`Conflicting host compiler contexts for ${file}`);
    if (!previous) merged.set(file, { ...entry, file });
  }
  return [...merged.values()];
}
export function validateHostEditorDatabase(
  database: CompileCommand[],
  targets: HostTarget[],
  sources = sourceFiles(join(ROOT, "tests"), /\.cpp$/),
) {
  const expected = hostCompilationDatabase(targets);
  for (const source of sources) {
    const entries = database.filter((entry) => entry.file === source);
    const context = expected.find((entry) => entry.file === source);
    if (entries.length !== 1 || !context)
      throw new Error(
        `Compilation database lacks unique host test context for ${source}`,
      );
    const entry = entries[0];
    if (
      basename(entry.arguments[0]) !== "clang++" ||
      !entry.arguments.includes("-std=c++17") ||
      entry.arguments.some((arg) =>
        /xtensa|SURFACE_STICK_S3|SURFACE_WAVESHARE_1_8|Arduino\.h|\/cores\/esp32|esp32-arduino-libs|\/esp32\/hardware\//.test(
          arg,
        ),
      ) ||
      entry.directory !== context.directory ||
      JSON.stringify(entry.arguments) !== JSON.stringify(context.arguments)
    )
      throw new Error(
        `Compilation database has incomplete host compiler context for ${source}`,
      );
  }
}
export async function configureCpp(targetId: HardwareTargetId) {
  let diagnostics = "";
  await run("arduino-cli", compileArguments(targetId, false, true), {
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
    readFileSync(join(buildPath(targetId), "compile_commands.json"), "utf8"),
  ) as CompileCommand[];
  const targets = [...hostTestTargets(), hostProbeTarget()];
  const database = mergeEditorDatabase(
    editorDatabase(entries, targetId),
    hostCompilationDatabase(targets),
  );
  validateHostEditorDatabase(database, targets);
  const target = join(ROOT, ".build/compile_commands.json");
  writeFileSync(`${target}.tmp`, JSON.stringify(database, null, 2) + "\n");
  renameSync(`${target}.tmp`, target);
  console.log(
    `C++ editor target: ${targetId}. Verified firmware, owned libraries, and all host test translation units in .build/compile_commands.json.`,
  );
}
function hardwareDirectory(targetId: HardwareTargetId, touch = false): string {
  const options = JSON.parse(
    readFileSync(
      join(buildPath(targetId, touch), "build.options.json"),
      "utf8",
    ),
  ) as { hardwareFolders: string; customBuildProperties: string };
  if (
    !options.customBuildProperties
      .split(/[ ,]+/)
      .includes(`-D${hardwareTargets[targetId].define}`)
  )
    throw new Error(
      "Built image hardware target differs; rebuild before flashing",
    );
  return options.hardwareFolders.split(",")[0];
}
export async function upload(
  targetId: HardwareTargetId,
  port: string,
  touch = false,
) {
  if (!existsSync(join(buildPath(targetId, touch), "sonos_surface.ino.bin")))
    throw new Error(`Build first: node --run build:${targetId}`);
  const platform = join(
    hardwareDirectory(targetId, touch),
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
    fqbn(targetId),
    "--port",
    port,
    "--input-dir",
    buildPath(targetId, touch),
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
  targetId: HardwareTargetId,
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
  await ops.build(targetId, touch);
  let uploadError: unknown;
  try {
    await ops.upload(targetId, port, touch);
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
export const espressifVendorId = "303a";
// Identification only removes guessing: two identical models are still flashed
// by explicit --port. Nothing is written before readiness, and nothing but the
// read-only board command is ever written at all.
export async function identifyPorts(
  list: () => Promise<{ path: string; vendorId?: string }[]> = listPorts,
  open: (path: string) => Promise<DevicePort> = openPort,
  print: (line: string) => void = console.log,
  seconds = 10,
) {
  for (const info of await list()) {
    if (info.vendorId?.toLowerCase() !== espressifVendorId) {
      print(`${info.path} skipped (not an Espressif device)`);
      continue;
    }
    let port: DevicePort | undefined;
    let answer =
      "no application response (asleep, busy booting, or not sonos-surface)";
    try {
      port = await open(info.path);
      await waitReady(port, seconds);
      answer = (await request(port, "board", "board ", [], 5)).trim();
    } catch {
      // A silent port is reported, never reset or retried.
    } finally {
      await port?.close();
    }
    print(`${info.path} ${answer}`);
  }
}
export interface CapturedLine {
  at: number;
  line: string;
}
export const summaryRelevant = (line: string) => {
  const text = deviceMessage(line);
  return (
    text.startsWith("heartbeat ") ||
    text.startsWith("worker ") ||
    text.includes("max-poll-gap-ms=") ||
    text.includes("poll-gap-max=")
  );
};
// Job durations come from the device's own log timestamps, which a buffered
// serial backlog cannot distort; host arrival time is the fallback for a line
// that carries none.
export function captureSummary(entries: CapturedLine[]): string {
  let heartbeats = 0,
    busy = 0,
    transitions = 0,
    buttonGap = 0,
    uiGap = 0;
  const started = new Map<string, number>();
  const durations: number[] = [];
  // A capture can begin with buffered backlog, so counts are only readable
  // beside the device time the capture actually spans.
  let first: number | undefined,
    last = 0;
  for (const { at, line } of entries) {
    const stamp = /^\[(\d+)\] /.exec(line);
    const time = stamp ? Number(stamp[1]) : at;
    if (stamp) {
      first ??= time;
      last = time;
    }
    const text = deviceMessage(line);
    if (text.startsWith("heartbeat ")) {
      heartbeats++;
      if (text.includes("busy=1")) busy++;
      continue;
    }
    const running = /^worker Idle -> Running id=(\d+)/.exec(text);
    if (running) {
      transitions++;
      started.set(running[1], time);
      continue;
    }
    const idle = /^worker Running -> Idle id=(\d+)/.exec(text);
    if (idle) {
      transitions++;
      const start = started.get(idle[1]);
      if (start !== undefined) {
        // A host fallback time is fractional; report whole milliseconds.
        durations.push(Math.round(time - start));
        started.delete(idle[1]);
      }
      continue;
    }
    const button = /\[button\] max-poll-gap-ms=(\d+)/.exec(text);
    if (button) buttonGap = Math.max(buttonGap, Number(button[1]));
    const frame = /poll-gap-max=(\d+)/.exec(text);
    if (frame) uiGap = Math.max(uiGap, Number(frame[1]));
  }
  const sorted = [...durations].sort((a, b) => a - b);
  const middle = sorted.length >> 1;
  const median = !sorted.length
    ? 0
    : sorted.length % 2
      ? sorted[middle]
      : (sorted[middle - 1] + sorted[middle]) / 2;
  return [
    `capture span-ms=${last - (first ?? 0)}`,
    `heartbeats=${heartbeats}`,
    `busy-ratio=${(heartbeats ? busy / heartbeats : 0).toFixed(3)}`,
    `worker-transitions=${transitions}`,
    `jobs=${sorted.length}`,
    `job-ms-median=${Math.round(median)}`,
    `job-ms-max=${sorted.length ? sorted[sorted.length - 1] : 0}`,
    `button-poll-gap-ms-max=${buttonGap}`,
    `ui-poll-gap-ms-max=${uiGap}`,
  ].join(" ");
}
export async function monitor(
  name: string,
  seconds: number,
  options: { until?: string; stats?: boolean } = {},
  open: (path: string) => Promise<DevicePort> = openPort,
  print: (line: string) => void = console.log,
) {
  // An unattended capture must always end by itself.
  if (!process.stdin.isTTY && !seconds)
    throw new Error(
      "--seconds is required when stdin is not a terminal; monitor reads no commands there",
    );
  const port = await open(name);
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
  const entries: CapturedLine[] = [];
  try {
    // A measurement starts from live output: an unread port holds a backlog
    // whose device time would otherwise be counted as part of this capture.
    if (options.stats) {
      let discarded = 0;
      const drained = performance.now() + 2000;
      while (performance.now() < drained && (await port.readLine(20)))
        discarded++;
      print(`capture start: discarded ${discarded} buffered lines`);
    }
    const deadline = seconds ? performance.now() + seconds * 1000 : Infinity;
    while (!stopped && performance.now() < deadline) {
      const line = await port.readLine();
      if (!line) continue;
      print(line);
      if (options.stats && summaryRelevant(line))
        entries.push({ at: performance.now(), line });
      if (options.until && line.includes(options.until)) break;
    }
    if (options.stats) print(captureSummary(entries));
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
    ["port", "seconds", "config", "env-file", "until"],
    ["download-mode", "touch-diagnostic", "stats", "identify"],
    argv,
  );
  if (values.help)
    return console.log(
      `node --run build:TARGET|flash:TARGET -- [--port PORT] [--config PATH] [--env-file PATH] [--touch-diagnostic]\nnode --run cpp:configure -- [TARGET]\nnode --run monitor -- TARGET --port PORT [--seconds N] [--until TOKEN] [--stats]\nnode --run reset|reboot -- TARGET --port PORT [--download-mode]\nnode --run ports -- [--identify]\nHardware targets: ${Object.keys(hardwareTargets).join(", ")}`,
    );
  const [action, positionalTarget, ...extra] = positionals;
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
  if (extra.length) throw new Error("Unexpected hardware target argument");
  if (action === "ports") {
    if (positionalTarget)
      throw new Error("ports does not take a hardware target");
    if (values.identify) return identifyPorts();
    console.log(JSON.stringify(await listPorts(), null, 2));
    return;
  }
  if (values.identify) throw new Error("--identify is only for ports");
  const targetId = hardwareTargetId(
    positionalTarget ?? (action === "cpp:configure" ? "stick-s3" : ""),
  );
  const touch = Boolean(values["touch-diagnostic"]);
  if (
    touch &&
    (!hardwareTargets[targetId].touchDiagnostic ||
      !["build", "flash"].includes(action))
  )
    throw new Error(
      "--touch-diagnostic requires a supported hardware target build or flash",
    );
  if (values["download-mode"] && action !== "reset")
    throw new Error("--download-mode is only for reset");
  if ((values.config || values["env-file"]) && action !== "flash")
    throw new Error("--config and --env-file are only for flash");
  if (
    (values.seconds !== undefined ||
      values.until !== undefined ||
      values.stats) &&
    action !== "monitor"
  )
    throw new Error("--seconds, --until, and --stats are only for monitor");
  if (action === "build") return build(targetId, touch);
  if (action === "cpp:configure") return configureCpp(targetId);
  const port = required(values.port, "port");
  if (action === "flash")
    return flash(
      targetId,
      port,
      stringOption(values.config),
      stringOption(values["env-file"]),
      touch,
    );
  if (action === "monitor")
    return monitor(port, numberOption(values.seconds, 0, 0, 86400), {
      until: stringOption(values.until),
      stats: Boolean(values.stats),
    });
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
          fqbn(targetId),
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
