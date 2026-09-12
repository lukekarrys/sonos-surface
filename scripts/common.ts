import { spawn } from "node:child_process";
import { readdirSync } from "node:fs";
import { resolve, join, delimiter } from "node:path";
import { parseArgs } from "node:util";

export const ROOT = resolve(import.meta.dirname, "..");
export const DEPS = join(ROOT, ".deps");
export const LIBS = join(DEPS, "arduino/libraries");
export const CONFIG = join(DEPS, "arduino-cli.yaml");
export const CORE_VERSION = "3.3.11";
export const CLI_VERSION = "1.1.1";
export const M5_VERSION = "0.2.21";
export const SML_VERSION = "1.2.0";
export const libraries = [
  "M5GFX@0.2.28",
  `M5Unified@${M5_VERSION}`,
  "M5Utility@0.2.0",
  "M5HAL@0.1.2",
  "M5UnitUnified@0.5.5",
  "M5Unit-NFC@0.1.0",
  "GFX Library for Arduino@1.6.7",
];

export interface RunOptions {
  capture?: boolean;
  onOutput?: (text: string) => void;
  cwd?: string;
  env?: NodeJS.ProcessEnv;
}
// Arguments are intentionally omitted from errors: callers may handle private data.
export async function run(
  command: string,
  args: string[] = [],
  options: RunOptions = {},
): Promise<string> {
  return new Promise((resolveResult, reject) => {
    const child = spawn(command, args, {
      cwd: options.cwd ?? ROOT,
      env: options.env ?? process.env,
      shell: false,
      stdio:
        options.capture || options.onOutput
          ? ["ignore", "pipe", "pipe"]
          : "inherit",
    });
    let output = "";
    child.stdout?.on("data", (chunk: Buffer) => {
      output += chunk.toString();
      options.onOutput?.(chunk.toString());
      if (!options.capture) process.stdout.write(chunk);
    });
    child.stderr?.on("data", (chunk: Buffer) => {
      output += chunk.toString();
      options.onOutput?.(chunk.toString());
      if (!options.capture) process.stderr.write(chunk);
    });
    let interrupted: NodeJS.Signals | undefined;
    const interrupt = (signal: NodeJS.Signals) => {
      interrupted = signal;
      child.kill(signal);
    };
    const sigint = () => interrupt("SIGINT");
    const sigterm = () => interrupt("SIGTERM");
    process.on("SIGINT", sigint);
    process.on("SIGTERM", sigterm);
    const cleanup = () => {
      process.off("SIGINT", sigint);
      process.off("SIGTERM", sigterm);
    };
    child.on("error", () => {
      cleanup();
      reject(
        new Error(`Cannot start ${command}; check installation and PATH.`),
      );
    });
    child.on("close", (code, signal) => {
      cleanup();
      if (interrupted) process.exitCode = interrupted === "SIGINT" ? 130 : 143;
      if (code !== 0 || signal || interrupted)
        reject(
          new Error(
            `${command} failed (${signal ?? interrupted ?? `exit ${code}`}).`,
          ),
        );
      else resolveResult(output);
    });
  });
}
export function sourceFiles(
  directory: string,
  extensions = /\.(cpp|h|ino)$/,
): string[] {
  return readdirSync(directory, { withFileTypes: true })
    .flatMap((entry) => {
      const path = join(directory, entry.name);
      return entry.isDirectory()
        ? sourceFiles(path, extensions)
        : extensions.test(entry.name)
          ? [path]
          : [];
    })
    .sort();
}
export function cli(
  strings: string[],
  booleans: string[] = [],
  argv = process.argv.slice(2),
) {
  return parseArgs({
    args: argv,
    allowPositionals: true,
    options: {
      ...Object.fromEntries(
        strings.map((key) => [key, { type: "string" as const }]),
      ),
      ...Object.fromEntries(
        [...booleans, "help"].map((key) => [key, { type: "boolean" as const }]),
      ),
    },
  });
}
export function stringOption(
  value: string | boolean | undefined,
): string | undefined {
  return typeof value === "string" ? value : undefined;
}
export function required(
  value: string | boolean | undefined,
  name: string,
): string {
  if (typeof value !== "string" || !value)
    throw new Error(`--${name} is required`);
  return value;
}
export function numberOption(
  value: string | boolean | undefined,
  fallback: number,
  min: number,
  max: number,
): number {
  const number = value === undefined ? fallback : Number(value);
  if (!Number.isFinite(number) || number < min || number > max)
    throw new Error("Numeric option outside supported bounds");
  return number;
}
export async function main(action: () => Promise<unknown> | unknown) {
  try {
    await action();
  } catch (error) {
    console.error(error instanceof Error ? error.message : "Command failed");
    process.exitCode ||= 1;
  }
}
export const executablePaths = (process.env.PATH ?? "").split(delimiter);
