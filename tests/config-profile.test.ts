import { test } from "node:test";
import assert from "node:assert/strict";
import {
  mkdtempSync,
  writeFileSync,
  readFileSync,
  readdirSync,
  rmSync,
  copyFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { configure, configureDevice } from "../scripts/configure.ts";
import {
  DEFAULT_CONFIG,
  loadEnv,
  loadProfile,
  ProfileError,
  parseJson,
} from "../scripts/config-profile.ts";
import { hardwareTargets } from "../scripts/hardware-targets.ts";
import { flash } from "../scripts/device.ts";
import type { FlashOperations } from "../scripts/device.ts";
import type { DevicePort } from "../scripts/serial-device.ts";
import {
  request,
  readyPort,
  serialOptions,
  waitReady,
} from "../scripts/serial-device.ts";
const SECRET = 'secret with spaces "quotes" \\slashes # $()';
function fixture(t: { after: (fn: () => void) => void }) {
  const root = mkdtempSync(join(tmpdir(), "surface-profile-"));
  t.after(() => rmSync(root, { recursive: true, force: true }));
  const path = join(root, "arbitrary label-2.json"),
    env = join(root, ".env");
  const config = {
    wifi_ssid: "${WIFI_SSID}",
    wifi_password: "${WIFI_PASSWORD}",
    read_only: true,
    rooms: { office: {} },
  };
  writeFileSync(env, `WIFI_SSID=House Wi-Fi\nWIFI_PASSWORD='${SECRET}'\n`);
  writeFileSync(path, JSON.stringify(config));
  return {
    root,
    path,
    env,
    config,
    load: (environment = {}) => loadProfile(path, env, environment),
  };
}
test("profiles, arbitrary external paths, filename independence and environment precedence", async (t) => {
  const f = fixture(t);
  const first = f.load();
  assert.equal(first.config.wifi_password, SECRET);
  assert.deepEqual(JSON.parse(first.payload), first.config);
  const other = join(f.root, "unrelated-room.json");
  copyFileSync(f.path, other);
  assert.deepEqual(first, loadProfile(other, f.env, {}));
  assert.equal(
    f.load({ WIFI_SSID: "Process", WIFI_PASSWORD: "" }).config.wifi_password,
    "",
  );
  assert.equal(f.load({ WIFI_SSID: "Process" }).config.wifi_ssid, "Process");
  assert.equal(DEFAULT_CONFIG.endsWith("/config/default.json"), true);
  assert.equal(loadProfile(undefined, f.env, {}).config.read_only, true);
  assert.equal(
    loadProfile(join(DEFAULT_CONFIG, "../luke.json"), f.env, {}).config
      .read_only,
    false,
  );
  await configure(
    ["--port", "fake", "--config", other, "--env-file", f.env],
    async (port, profile) => {
      assert.equal(port, "fake");
      assert.deepEqual(profile, loadProfile(other, f.env));
    },
  );
  await configure(
    ["--port", "fake", "--env-file", f.env],
    async (_port, profile) => {
      assert.deepEqual(profile, loadProfile(DEFAULT_CONFIG, f.env));
    },
  );
});
test("literal env grammar, missing files, single-pass substitution and no evaluation", (t) => {
  const f = fixture(t);
  writeFileSync(
    f.env,
    "# comment\n\n A = ' spaces = # remain ' \nB=literal\\n$HOME\nC=\n",
  );
  assert.deepEqual(
    { ...loadEnv(f.env) },
    { A: " spaces = # remain ", B: "literal\\n$HOME", C: "" },
  );
  for (const text of ["A=x\nA=y", "export A=x", "A", 'A="unclosed', SECRET]) {
    writeFileSync(f.env, text);
    assert.throws(
      () => loadEnv(f.env),
      (error) =>
        error instanceof ProfileError && !error.message.includes(SECRET),
    );
  }
  assert.throws(() => loadEnv(join(f.root, "absent")));
  writeFileSync(f.env, "WIFI_SSID=House\nWIFI_PASSWORD=x");
  for (const value of [
    "${WIFI_SSID:-default}",
    "${}",
    "${WIFI_SSID",
    "${OTHER}",
  ]) {
    writeFileSync(f.path, JSON.stringify({ ...f.config, wifi_ssid: value }));
    assert.throws(() => f.load());
  }
  writeFileSync(
    f.path,
    JSON.stringify({
      ...f.config,
      wifi_ssid: "${PREFIX} ${WIFI_SSID}/${PREFIX}",
    }),
  );
  assert.equal(f.load({ PREFIX: "The" }).config.wifi_ssid, "The House/The");
  writeFileSync(f.path, JSON.stringify(f.config));
  assert.throws(() => f.load({ WIFI_SSID: "${WIFI_PASSWORD}" }));
  const literal = "$(touch should-not-exist) `echo no` $HOME";
  assert.equal(f.load({ WIFI_SSID: literal }).config.wifi_ssid, literal);
});
test("JSON structure, duplicates, nesting, Unicode byte limits and injection rejection", (t) => {
  const f = fixture(t);
  for (const text of [
    '{"wifi_password":"' + SECRET + '"',
    '{"rooms":${WIFI_SSID}}',
    '{"rooms":{},"rooms":{}}',
    '{"rooms":{"a":{},"\\u0061":{}}}',
    '{"read_only":NaN}',
    '{"rooms":{"x":1e999}}',
  ]) {
    writeFileSync(f.path, text);
    assert.throws(
      () => f.load(),
      (error) =>
        error instanceof ProfileError && !error.message.includes(SECRET),
    );
  }
  for (const value of [
    [],
    { rooms: [] },
    { read_only: "false" },
    { wifi_password: 2 },
    { unexpected: true },
    { "${WIFI_SSID}": "value" },
    { rooms: { x: [[[[[[[{}]]]]]]] } },
  ]) {
    writeFileSync(f.path, JSON.stringify(value));
    assert.throws(() => f.load());
  }
  writeFileSync(f.path, JSON.stringify(f.config));
  assert.throws(() => f.load({ WIFI_PASSWORD: "é".repeat(2100) }));
  const profile = f.load({ WIFI_PASSWORD: '\", "read_only": false, "x": "' });
  assert.equal(JSON.parse(profile.payload).read_only, true);
  assert.deepEqual(
    parseJson('{"rooms":{"__proto__":{}}}'),
    JSON.parse('{"rooms":{"__proto__":{}}}'),
  );
});
class Port implements DevicePort {
  writes: string[] = [];
  lines: string[] = [];
  closed = false;
  state: unknown;
  constructor(state: unknown) {
    this.state = state;
  }
  async readLine() {
    return this.lines.shift() ?? "";
  }
  async clear() {
    this.lines = [];
  }
  async write(command: string) {
    this.writes.push(command);
    this.lines = [
      command === "config-status\n"
        ? `device-config ${JSON.stringify(this.state)}`
        : "CONFIG_SAVED rebooting",
    ];
  }
  async close() {
    this.closed = true;
  }
}
test("configuration verifies new revision, gate and normalized policies after reboot without exposing secrets", async (t) => {
  const f = fixture(t);
  const profile = f.load();
  profile.config.rooms = { office: { album: {} } };
  profile.payload = JSON.stringify(profile.config);
  const before = { policyRevision: 7, read_only: false, rooms: {} };
  const after = { policyRevision: 8, read_only: true, rooms: { office: {} } };
  const snapshot = () =>
    readdirSync(f.root).map((file) => [
      file,
      readFileSync(join(f.root, file), "utf8"),
    ]);
  const files = snapshot();
  for (const result of [
    after,
    { ...after, policyRevision: 7 },
    { ...after, rooms: {} },
    { ...after, read_only: false },
    null,
  ]) {
    const ports = [new Port(before), new Port(result)];
    let index = 0;
    const action = configureDevice("fake", profile, async () => ports[index++]);
    if (result === after) await action;
    else await assert.rejects(action);
    assert.deepEqual(ports[0].writes, [
      "config-status\n",
      `config ${profile.payload}\n`,
    ]);
    assert.deepEqual(ports[1].writes, ["config-status\n"]);
    assert.ok(ports.every((port) => port.closed));
  }
  await assert.rejects(
    configureDevice("fake", profile, async () => {
      throw new Error(SECRET);
    }),
    (error) => error instanceof ProfileError && !error.message.includes(SECRET),
  );
  const rejected = new Port(before);
  rejected.readLine = async () => `CONFIG_INVALID ${SECRET}`;
  await assert.rejects(
    request(rejected, profile.payload, "CONFIG_SAVED", ["CONFIG_INVALID"]),
    (error) => error instanceof Error && !error.message.includes(SECRET),
  );
  assert.deepEqual(snapshot(), files);
});
test("flash preflight, preservation, readiness, failed build/upload ordering", async (t) => {
  const f = fixture(t);
  const events: string[] = [];
  const ops: FlashOperations = {
    async build() {
      events.push("build");
    },
    async upload() {
      events.push("flash");
    },
    async ready() {
      events.push("ready");
    },
    async configure(_port, profile) {
      events.push("configure");
      assert.equal(profile.config.read_only, true);
    },
  };
  await flash("stick-s3", "fake", f.path, f.env, false, ops);
  assert.deepEqual(events.splice(0), ["build", "flash", "ready", "configure"]);
  await flash("stick-s3", "fake", undefined, undefined, false, ops);
  assert.deepEqual(events.splice(0), ["build", "flash", "ready"]);
  await assert.rejects(
    flash("stick-s3", "fake", undefined, undefined, false, {
      ...ops,
      async build() {
        events.push("build");
        throw new Error();
      },
    }),
  );
  assert.deepEqual(events.splice(0), ["build"]);
  await assert.rejects(
    flash("stick-s3", "fake", f.path, f.env, false, {
      ...ops,
      async build() {
        events.push("build");
        throw new Error();
      },
    }),
  );
  assert.deepEqual(events.splice(0), ["build"]);
  await assert.rejects(
    flash("stick-s3", "fake", f.path, f.env, false, {
      ...ops,
      async upload() {
        events.push("flash");
        throw new Error();
      },
    }),
  );
  assert.deepEqual(events.splice(0), ["build", "flash", "ready"]);
  writeFileSync(f.path, "{invalid");
  await assert.rejects(flash("stick-s3", "fake", f.path, f.env, false, ops));
  assert.deepEqual(events, []);
  let touched = false;
  await assert.rejects(
    configure(["--port", "fake", "--config", f.path], async () => {
      touched = true;
    }),
  );
  assert.equal(touched, false);
  writeFileSync(
    f.path,
    '{"wifi_password":"${SURFACE_MISSING_SECRET_FIXTURE}"}',
  );
  await assert.rejects(flash("stick-s3", "fake", f.path, f.env, false, ops));
  assert.deepEqual(events, []);
});
test("flash resolves the profile before building and applies that captured profile", async (t) => {
  const f = fixture(t);
  const expected = loadProfile(f.path, f.env);
  const events: string[] = [];
  await flash("ws-1.8", "fake", f.path, f.env, true, {
    async build(board, touch) {
      assert.equal(board, "ws-1.8");
      assert.equal(touch, true);
      events.push("build");
      // Changing the file after preflight must not change the submitted profile.
      writeFileSync(f.path, "{invalid");
    },
    async upload(board, port, touch) {
      assert.deepEqual([board, port, touch], ["ws-1.8", "fake", true]);
      events.push("flash");
    },
    async ready() {
      events.push("ready");
    },
    async configure(_port, profile) {
      assert.deepEqual(profile, expected);
      events.push("configure");
    },
  });
  assert.deepEqual(events, ["build", "flash", "ready", "configure"]);
});
test("serial readiness requires application evidence and reconnect never resends", async () => {
  assert.equal(serialOptions.hupcl, false);
  assert.equal(serialOptions.rtscts, false);
  const first = new Port({}),
    second = new Port({});
  first.readLine = async () => {
    throw new Error("disconnected");
  };
  second.lines = [
    "entry 0x403c88b8",
    "heartbeat wifi=3 busy=1",
    "heartbeat wifi=3 busy=0",
  ];
  let opened = 0;
  assert.equal(
    await readyPort("fake", async () => (++opened === 1 ? first : second), 1),
    second,
  );
  assert.ok(first.closed);
  assert.deepEqual(first.writes, []);
  assert.deepEqual(second.writes, []);
  await assert.rejects(waitReady(new Port({}), 0.001));
});

test("flash model, arbitrary profile filename and explicit USB port remain independent", async (t) => {
  const f = fixture(t);
  const expected = loadProfile(f.path, f.env);
  const paths = [
    f.path,
    join(f.root, "kids-room-2.json"),
    join(f.root, "stick.json"),
  ];
  for (const path of paths.slice(1)) copyFileSync(f.path, path);
  for (const { id } of Object.values(hardwareTargets)) {
    for (const path of paths) {
      for (const port of ["/dev/cu.usbmodem101", "/dev/cu.usbmodem102"]) {
        const events: unknown[] = [];
        await flash(id, port, path, f.env, false, {
          async build(target, touch) {
            events.push(["build", target, touch]);
          },
          async upload(target, selectedPort, touch) {
            events.push(["upload", target, selectedPort, touch]);
          },
          async ready(selectedPort) {
            events.push(["ready", selectedPort]);
          },
          async configure(selectedPort, profile) {
            assert.deepEqual(profile, expected);
            events.push(["configure", selectedPort]);
          },
        });
        assert.deepEqual(events, [
          ["build", id, false],
          ["upload", id, port, false],
          ["ready", port],
          ["configure", port],
        ]);
      }
    }
  }
});
