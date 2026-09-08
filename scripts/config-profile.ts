import { readFileSync } from "node:fs";
import { join } from "node:path";
import { ROOT } from "./common.ts";

export const DEFAULT_CONFIG = join(ROOT, "config/default.json");
export const DEFAULT_ENV = join(ROOT, ".env");
export class ProfileError extends Error {}
export type Json =
  null | boolean | number | string | Json[] | { [key: string]: Json };
export interface Profile {
  config: Record<string, Json>;
  payload: string;
}
export function object(value: unknown): value is Record<string, Json> {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}
function read(path: string, label: string): string {
  try {
    return new TextDecoder("utf-8", { fatal: true }).decode(readFileSync(path));
  } catch {
    throw new ProfileError(`Cannot read ${label} (values omitted)`);
  }
}
export function loadEnv(path?: string): Record<string, string> {
  let text: string;
  try {
    text = new TextDecoder("utf-8", { fatal: true }).decode(
      readFileSync(path ?? DEFAULT_ENV),
    );
  } catch (error) {
    if (
      path === undefined &&
      (error as NodeJS.ErrnoException).code === "ENOENT"
    )
      return {};
    throw new ProfileError("Cannot read environment file");
  }
  const values: Record<string, string> = Object.create(null);
  for (let line of text.split(/\r?\n/)) {
    line = line.trim();
    if (!line || line.startsWith("#")) continue;
    const assignment = /^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$/.exec(line);
    if (!assignment || Object.hasOwn(values, assignment[1]))
      throw new ProfileError("Invalid or duplicate environment assignment");
    let value = assignment[2].trim();
    if (value.startsWith('"') || value.startsWith("'")) {
      if (value.length < 2 || value.at(-1) !== value[0])
        throw new ProfileError("Unclosed environment value quote");
      value = value.slice(1, -1);
    }
    values[assignment[1]] = value;
  }
  return values;
}
export function parseJson(text: string): Json {
  try {
    // Inspect container depth and decoded object keys before JSON.parse erases duplicates.
    const containers: (Set<string> | null)[] = [];
    const tokens = text.matchAll(/"(?:[^"\\]|\\[\s\S])*"|[{}\[\]]/g);
    for (const token of tokens) {
      const value = token[0];
      if (value === "{" || value === "[") {
        containers.push(value === "{" ? new Set() : null);
        if (containers.length > 8) throw new Error();
      } else if (value === "}" || value === "]") containers.pop();
      else if (/^\s*:/.test(text.slice(token.index + value.length))) {
        const keys = containers.at(-1);
        const key: string = JSON.parse(value);
        if (!keys || keys.has(key)) throw new Error();
        keys.add(key);
      }
    }
    return JSON.parse(text, (_key, value: Json) => {
      if (typeof value === "number" && !Number.isFinite(value))
        throw new Error();
      return value;
    }) as Json;
  } catch {
    throw new ProfileError(
      "Invalid, duplicate, or excessively nested configuration JSON (values omitted)",
    );
  }
}
export function expand(value: Json, environment: NodeJS.ProcessEnv): Json {
  if (Array.isArray(value))
    return value.map((item) => expand(item, environment));
  if (object(value))
    return Object.fromEntries(
      Object.entries(value).map(([key, item]) => {
        if (key.includes("${"))
          throw new ProfileError(
            "Placeholders are allowed only in JSON string values",
          );
        return [key, expand(item, environment)];
      }),
    );
  if (typeof value !== "string") return value;
  const result = value.replace(
    /\$\{([A-Za-z_][A-Za-z0-9_]*)\}/g,
    (_match, name: string) => {
      if (!Object.hasOwn(environment, name) || environment[name] === undefined)
        throw new ProfileError(
          "Missing referenced environment variable (values omitted)",
        );
      return environment[name];
    },
  );
  if (result.includes("${"))
    throw new ProfileError("Unresolved or unsupported environment placeholder");
  if (!result.isWellFormed())
    throw new ProfileError("Invalid Unicode in configuration");
  return result;
}
export function loadProfile(
  path = DEFAULT_CONFIG,
  envFile?: string,
  environ = process.env,
): Profile {
  const config = expand(parseJson(read(path, "configuration profile")), {
    ...loadEnv(envFile),
    ...environ,
  });
  if (!object(config))
    throw new ProfileError("Configuration must be a JSON object");
  // Firmware owns room and policy validation; these are only cheap framing checks.
  for (const [key, value] of Object.entries(config)) {
    const valid = ["wifi_ssid", "wifi_password", "apple_region"].includes(key)
      ? typeof value === "string"
      : key === "read_only"
        ? typeof value === "boolean"
        : key === "rooms"
          ? object(value)
          : false;
    if (!valid)
      throw new ProfileError(
        "Unknown configuration field or invalid top-level type",
      );
  }
  const payload = JSON.stringify(config);
  if (Buffer.byteLength(payload) > 4088)
    throw new ProfileError("Resolved configuration exceeds 4088 UTF-8 bytes");
  return { config, payload };
}
