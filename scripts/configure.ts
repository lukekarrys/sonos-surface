import { isDeepStrictEqual } from "node:util";
import { cli, main, required, stringOption } from "./common.ts";
import { loadProfile, object, ProfileError } from "./config-profile.ts";
import type { Profile } from "./config-profile.ts";
import { configRejectionTokens, readyPort, request } from "./serial-device.ts";
import type { DevicePort } from "./serial-device.ts";

async function status(port: DevicePort) {
  const result: unknown = JSON.parse(
    await request(port, "config-status", "device-config "),
  );
  if (
    !object(result) ||
    !Number.isSafeInteger(result.policyRevision) ||
    typeof result.read_only !== "boolean" ||
    !Number.isInteger(result.sleep_timeout_seconds) ||
    !object(result.rooms) ||
    !object(result.policy)
  )
    throw new Error();
  return result;
}
export async function configureDevice(
  name: string,
  profile: Profile,
  ready = readyPort,
) {
  try {
    const port = await ready(name);
    let before;
    try {
      before = await status(port);
      await request(
        port,
        `config ${profile.payload}`,
        "CONFIG_SAVED",
        configRejectionTokens,
      );
    } finally {
      await port.close();
    }
    // CONFIG_SAVED precedes reboot; require a fresh readiness event and solicited status.
    const rebooted = await ready(name);
    let after;
    try {
      after = await status(rebooted);
    } finally {
      await rebooted.close();
    }
    const rooms = profile.config.rooms ?? {};
    if (!object(rooms)) throw new Error();
    const normalizePolicy = (policies: unknown) => {
      if (!object(policies)) throw new Error();
      return Object.fromEntries(
        Object.entries(policies).filter(
          ([, fields]) => object(fields) && Object.keys(fields).length,
        ),
      );
    };
    const expected = Object.fromEntries(
      Object.entries(rooms).map(([room, policies]) => [
        room,
        normalizePolicy(policies),
      ]),
    );
    if (
      after.policyRevision !== Number(before.policyRevision) + 1 ||
      after.read_only !== (profile.config.read_only ?? true) ||
      after.sleep_timeout_seconds !==
        (profile.config.sleep_timeout_seconds ?? 300) ||
      !isDeepStrictEqual(after.rooms, expected) ||
      !isDeepStrictEqual(
        after.policy,
        normalizePolicy(profile.config.policy ?? {}),
      )
    )
      throw new ProfileError(
        "Device configuration status does not match submitted profile",
      );
  } catch (error) {
    if (error instanceof ProfileError) throw error;
    // Never expose transport errors or device text: either could include credentials.
    throw new ProfileError(
      "Configuration transfer or status verification failed (values omitted); inspect device before retrying",
    );
  }
  console.log(
    "Configuration saved; revision, read-only mode, sleep timeout, and device/room policies verified.",
  );
}
export async function configure(
  argv = process.argv.slice(2),
  send = configureDevice,
) {
  const { values, positionals } = cli(["port", "config", "env-file"], [], argv);
  if (values.help)
    return console.log(
      "node --run configure -- --port PORT [--config PATH] [--env-file PATH]",
    );
  if (positionals.length) throw new Error("Unexpected positional arguments");
  const port = required(values.port, "port");
  const profile = loadProfile(
    stringOption(values.config),
    stringOption(values["env-file"]),
  );
  await send(port, profile);
}
if (import.meta.main) await main(() => configure());
