import { existsSync } from "node:fs";
import { spawnSync } from "node:child_process";
import { join } from "node:path";
import {
  ROOT,
  cli,
  executablePaths,
  main,
  run,
  sourceFiles,
} from "./common.ts";

export function clangFormat(): string {
  const candidates = [
    ...executablePaths.map((path) => join(path, "clang-format")),
    "/opt/homebrew/opt/llvm@19/bin/clang-format",
    "/usr/local/opt/llvm@19/bin/clang-format",
    "/opt/homebrew/opt/llvm/bin/clang-format",
    "/usr/local/opt/llvm/bin/clang-format",
  ];
  const found = candidates.find((path) => {
    if (!existsSync(path)) return false;
    const version = spawnSync(path, ["--version"], {
      encoding: "utf8",
      shell: false,
    });
    return (
      version.status === 0 && /clang-format version 19\./.test(version.stdout)
    );
  });
  if (!found)
    throw new Error(
      "clang-format 19 is required. On macOS: brew install llvm@19",
    );
  return found;
}
export async function formatCpp(
  check = false,
  files = ["libraries", "firmware", "tests"].flatMap((dir) =>
    sourceFiles(join(ROOT, dir)),
  ),
) {
  await run(clangFormat(), [
    ...(check ? ["--dry-run", "--Werror"] : ["-i"]),
    ...files,
  ]);
}
if (import.meta.main)
  await main(async () => {
    const { values, positionals } = cli([], ["check"]);
    if (values.help) return console.log("node --run format:cpp[:check]");
    if (positionals.length) throw new Error("Unexpected positional arguments");
    await formatCpp(Boolean(values.check));
  });
