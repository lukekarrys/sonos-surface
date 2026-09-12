import { mkdirSync } from "node:fs";
import { join } from "node:path";
import { ROOT, LIBS, sourceFiles, run } from "./common.ts";
export const includes = [
  join(ROOT, "libraries/SurfaceCore/src"),
  join(ROOT, "libraries/SurfaceSonos/src"),
  join(LIBS, "SurfaceJson/src"),
  join(LIBS, "SurfaceXml/src"),
  join(LIBS, "SurfaceSml/src"),
];
export const deviceInclude = join(ROOT, "libraries/SurfaceDevice/src");
export const coreSources = () => sourceFiles(includes[0], /\.cpp$/);
export const sharedSources = () => [
  ...coreSources(),
  ...sourceFiles(includes[1], /\.cpp$/),
  join(includes[3], "tinyxml2.cpp"),
];
export interface HostTarget {
  name: string;
  sources: string[];
  includes: string[];
  sanitize?: boolean;
  options?: string[];
  linkOptions?: string[];
}
export function hostCompileArguments(target: HostTarget): string[] {
  return [
    "clang++",
    "-std=c++17",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-g",
    ...(target.sanitize !== false
      ? ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
      : []),
    ...target.includes.map((path) => `-I${path}`),
    ...(target.options ?? []),
  ];
}
export function hostCompilationDatabase(targets: HostTarget[]) {
  return targets.flatMap((target) =>
    target.sources.map((file) => ({
      directory: ROOT,
      file,
      arguments: [...hostCompileArguments(target), "-c", file],
    })),
  );
}
export async function compileHost(target: HostTarget) {
  const build = join(ROOT, ".build/host");
  mkdirSync(build, { recursive: true });
  const binary = join(build, target.name);
  const [compiler, ...args] = hostCompileArguments(target);
  await run(compiler, [
    ...args,
    ...target.sources,
    ...(target.linkOptions ?? []),
    "-o",
    binary,
  ]);
  return binary;
}
