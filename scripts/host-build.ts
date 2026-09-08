import { mkdirSync } from "node:fs";
import { join } from "node:path";
import { ROOT, LIBS, sourceFiles, run } from "./common.ts";
export const includes = [
  join(ROOT, "libraries/SurfaceCore/src"),
  join(ROOT, "libraries/SurfaceSonos/src"),
  join(LIBS, "SurfaceJson/src"),
  join(LIBS, "SurfaceXml/src"),
];
export const deviceInclude = join(ROOT, "libraries/SurfaceDevice/src");
export const coreSources = () => sourceFiles(includes[0], /\.cpp$/);
export const sharedSources = () => [
  ...coreSources(),
  ...sourceFiles(includes[1], /\.cpp$/),
  join(includes[3], "tinyxml2.cpp"),
];
export async function compileHost(
  name: string,
  sources: string[],
  paths: string[],
  sanitize = true,
  extra: string[] = [],
) {
  const build = join(ROOT, ".build/host");
  mkdirSync(build, { recursive: true });
  const binary = join(build, name);
  await run("clang++", [
    "-std=c++17",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-g",
    ...(sanitize
      ? ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
      : []),
    ...paths.map((path) => `-I${path}`),
    ...sources,
    ...extra,
    "-o",
    binary,
  ]);
  return binary;
}
