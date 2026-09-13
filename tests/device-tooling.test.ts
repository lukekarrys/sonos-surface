import { test } from "node:test";
import assert from "node:assert/strict";
import {
  captureSummary,
  espressifVendorId,
  identifyPorts,
  monitor,
} from "../scripts/device.ts";
import type { CapturedLine } from "../scripts/device.ts";
import { usb, usbRequest } from "../scripts/usb.ts";
import {
  dragPoints,
  injectReply,
  touchCommands,
  uiAction,
  releaseRequired,
} from "../scripts/ui.ts";
import type { DevicePort } from "../scripts/serial-device.ts";

const SECRET = "https://music.apple.com/us/album/private-1234";
// Injected ports only: host tooling tests never open a real serial device.
class Port implements DevicePort {
  writes: string[] = [];
  lines: string[] = [];
  closed = false;
  reply: (command: string) => string[] = () => [];
  constructor(lines: string[] = []) {
    this.lines = lines;
  }
  async readLine() {
    return this.lines.shift() ?? "";
  }
  async clear() {
    this.lines = [];
  }
  async write(command: string) {
    this.writes.push(command);
    this.lines.push(...this.reply(command));
  }
  async close() {
    this.closed = true;
  }
}
// Replies in the device's own formats: the ui-state line, and one delivery
// line per queued sample, preceded by inject chatter that is not a delivery.
const uiPort = (state: { held?: boolean; cancelled?: boolean } = {}) => {
  const port = new Port();
  port.reply = (command) => {
    if (command === "ui-state\n")
      return [`[10] ui-state ${JSON.stringify({ touch: state })}`];
    const touch = /^ui-touch (\d+) (\d+) 1\n$/.exec(command);
    return [
      "[11] [ui] inject gesture expired; hardware sampling resumed",
      touch
        ? `[11] [ui] inject touch x=${touch[1]} y=${touch[2]} fingers=1 hit=3 volume-preview=-1 seek-preview=-1`
        : "[11] [ui] inject release",
    ];
  };
  return port;
};

test("usb returns one reply, skips background lines, and bounds the wait", async () => {
  const port = new Port();
  port.reply = () => [
    "[10] heartbeat wifi=3 busy=0 heap=1 inactivity=42",
    "[11] worker Idle -> Running id=4 deadline=9",
    "[11] Sonos 192.0.2.1 urn:schemas-upnp-org:service:AVTransport:1#Play",
    "[11] GetZoneGroupState http=200 ms=90",
    "[11] request=2 status=succeeded Observed requested result refresh-error=",
    "[11] [ui] frame screen=0 draw=16 flush=44 total=60",
    '[12] lifecycles {"worker":{}}',
  ];
  const printed: string[] = [];
  assert.equal(
    await usbRequest(port, "lifecycle-status", {
      print: (l) => printed.push(l),
    }),
    '[12] lifecycles {"worker":{}}',
  );
  assert.deepEqual(port.writes, ["lifecycle-status\n"]);
  assert.equal(printed.length, 7);
  port.reply = () => [
    "[10] heartbeat wifi=3 busy=0 heap=1",
    "[11] ui-state {}",
  ];
  assert.equal(
    await usbRequest(port, "ui-state", {
      expect: "ui-state ",
      print: () => {},
    }),
    "[11] ui-state {}",
  );
  // Only heartbeats and transitions arrive: the command produced no reply.
  port.reply = () => [
    "[10] heartbeat wifi=3 busy=0 heap=1 inactivity=42",
    "[11] worker Running -> Idle id=4 outcome=Success",
  ];
  await assert.rejects(
    usbRequest(port, `preview ${SECRET}`, {
      seconds: 0.05,
      print: () => {},
    }),
    (error) =>
      error instanceof Error &&
      /not observed/.test(error.message) &&
      !error.message.includes(SECRET),
  );
  // The line bound ends a capture that never matches an explicit token.
  port.reply = () => ["[10] frame one", "[11] frame two", "[12] expected"];
  await assert.rejects(
    usbRequest(port, "ui-state", {
      expect: "expected",
      lines: 2,
      print: () => {},
    }),
    /not observed/,
  );
});

test("usb fails on device rejections and refuses credential and reboot commands", async () => {
  const port = new Port();
  for (const rejection of [
    "UI_INJECT_INVALID: use ui-touch X Y",
    "UI_INJECT_FULL: 32 samples already queued",
    "CALIBRATION_INVALID (adapter must be ready)",
    "REBOOT_BUSY",
  ]) {
    port.reply = () => [`[10] ${rejection}`];
    await assert.rejects(
      usbRequest(port, `ui-touch ${SECRET}`, { print: () => {} }),
      (error) =>
        error instanceof Error &&
        /rejected/.test(error.message) &&
        !error.message.includes(SECRET),
    );
  }
  const opened: string[] = [];
  const open = async (name: string) => {
    opened.push(name);
    return port;
  };
  for (const command of [
    `config {"wifi_password":"${SECRET}"}`,
    "config",
    "read-only false",
    "read-only",
  ])
    await assert.rejects(
      usb(["--port", "fake", "--command", command], open),
      (error) =>
        error instanceof Error &&
        /node --run configure/.test(error.message) &&
        !error.message.includes(SECRET),
    );
  assert.deepEqual(opened, []);
  port.reply = () => ["[10] device-config {}"];
  await usb(["--port", "fake", "--command", "config-status"], open);
  assert.deepEqual(opened, ["fake"]);
  assert.ok(port.closed);
});

test("port identification answers, reports silence, and never writes elsewhere", async () => {
  const answering = new Port(["[1] heartbeat wifi=3 busy=0 heap=1"]);
  answering.reply = () => [
    '[2] board target=ws-1.8 adapter="Waveshare V2 CO5300/CST820" ready=1',
  ];
  const silent = new Port();
  const skipped = new Port(["[1] heartbeat wifi=3 busy=0 heap=1"]);
  const ports: Record<string, Port> = {
    "/dev/cu.usbmodem101": answering,
    "/dev/cu.usbmodem102": silent,
    "/dev/cu.Bluetooth": skipped,
  };
  const opened: string[] = [];
  const printed: string[] = [];
  await identifyPorts(
    async () => [
      {
        path: "/dev/cu.usbmodem101",
        vendorId: espressifVendorId.toUpperCase(),
      },
      { path: "/dev/cu.usbmodem102", vendorId: espressifVendorId },
      { path: "/dev/cu.Bluetooth", vendorId: "05ac" },
      { path: "/dev/cu.unknown" },
    ],
    async (path) => {
      opened.push(path);
      return ports[path];
    },
    (line) => printed.push(line),
    0.05,
  );
  assert.deepEqual(printed, [
    '/dev/cu.usbmodem101 target=ws-1.8 adapter="Waveshare V2 CO5300/CST820" ready=1',
    "/dev/cu.usbmodem102 no application response (asleep, busy booting, or not sonos-surface)",
    "/dev/cu.Bluetooth skipped (not an Espressif device)",
    "/dev/cu.unknown skipped (not an Espressif device)",
  ]);
  assert.deepEqual(opened, ["/dev/cu.usbmodem101", "/dev/cu.usbmodem102"]);
  assert.deepEqual(answering.writes, ["board\n"]);
  assert.deepEqual(silent.writes, []);
  assert.deepEqual(skipped.writes, []);
  assert.ok(answering.closed && silent.closed);
});

test("ui drags step through the screen in order and release first only when required", async () => {
  const points = dragPoints([52, 351], [316, 351], 5);
  assert.deepEqual(points, [
    [52, 351],
    [118, 351],
    [184, 351],
    [250, 351],
    [316, 351],
  ]);
  assert.deepEqual(dragPoints([32, 28], [332, 428], 2), [
    [32, 28],
    [332, 428],
  ]);
  for (const steps of [1, 32, 2.5])
    assert.throws(() => dragPoints([0, 0], [10, 10], steps), /--steps/);
  assert.deepEqual(touchCommands([[1, 2]]), [
    "ui-touch 1 2 1",
    "ui-touch release",
  ]);
  assert.equal(injectReply("ui-touch 1 2 1"), "[ui] inject touch x=1 y=2 ");
  assert.equal(injectReply("ui-touch release"), "[ui] inject release");
  const options = { steps: 3, intervalMs: 0, tailSeconds: 0, seconds: 0.05 };
  const free = uiPort();
  await uiAction(free, ["tap", "184", "286"], options, () => {});
  assert.deepEqual(free.writes, [
    "ui-state\n",
    "ui-touch 184 286 1\n",
    "ui-touch release\n",
  ]);
  for (const state of [{ held: true }, { cancelled: true }]) {
    const port = uiPort(state);
    await uiAction(
      port,
      ["drag", "52", "351", "316", "351"],
      options,
      () => {},
    );
    assert.deepEqual(port.writes, [
      "ui-state\n",
      "ui-touch release\n",
      "ui-touch 52 351 1\n",
      "ui-touch 184 351 1\n",
      "ui-touch 316 351 1\n",
      "ui-touch release\n",
    ]);
    assert.equal(await releaseRequired(port, () => {}), true);
  }
  // A sample a physical finger cancelled was never delivered, so the tap
  // fails instead of reporting the discarded contact as done.
  const cancelled = new Port();
  cancelled.reply = (command) =>
    command === "ui-state\n"
      ? ["[10] ui-state {}"]
      : ["[11] [ui] inject cancelled=physical-touch"];
  await assert.rejects(
    uiAction(cancelled, ["tap", "184", "286"], options, () => {}),
    /not observed/,
  );
  assert.deepEqual(cancelled.writes, ["ui-state\n", "ui-touch 184 286 1\n"]);
  const navigation = new Port();
  navigation.reply = (command) => [
    command === "ui-state\n"
      ? "[10] ui-state {}"
      : command === "ui-button boot\n"
        ? "[10] [ui] inject button=boot (no action)"
        : "[10] [ui] navigation screen=2 (no playback intent)",
  ];
  await uiAction(navigation, ["screen", "queue"], options, () => {});
  await uiAction(navigation, ["button", "boot"], options, () => {});
  await uiAction(navigation, ["state"], options, () => {});
  assert.deepEqual(navigation.writes, [
    "ui-screen queue\n",
    "ui-button boot\n",
    "ui-state\n",
  ]);
  for (const argv of [
    ["tap", "184"],
    ["tap", "368", "286"],
    ["tap", "184", "448"],
    ["tap", "-1", "286"],
    ["drag", "1", "2", "3"],
    ["screen", "other"],
    ["button", "power"],
    ["state", "extra"],
    ["swipe", "1", "2"],
  ])
    await assert.rejects(uiAction(uiPort(), argv, options, () => {}));
});

test("bounded captures stop at a token and summarize worker, busy and poll gaps", async (t) => {
  Object.defineProperty(process.stdin, "isTTY", {
    value: false,
    configurable: true,
  });
  await assert.rejects(
    monitor("fake", 0, {}, async () => new Port()),
    /--seconds is required/,
  );
  const port = new Port([
    "[1] heartbeat wifi=3 busy=0 heap=1",
    "[2] worker Idle -> Running id=1 deadline=5",
    "[3] heartbeat wifi=3 busy=1 heap=1",
    "[4] worker Running -> Idle id=1 outcome=Success",
    "[5] after the token",
  ]);
  const printed: string[] = [];
  await monitor(
    "fake",
    5,
    { until: "worker Running -> Idle" },
    async () => port,
    (line) => printed.push(line),
  );
  assert.deepEqual(printed, [
    "[1] heartbeat wifi=3 busy=0 heap=1",
    "[2] worker Idle -> Running id=1 deadline=5",
    "[3] heartbeat wifi=3 busy=1 heap=1",
    "[4] worker Running -> Idle id=1 outcome=Success",
  ]);
  assert.ok(port.closed);
  // A stats capture discards the backlog first and measures live output only.
  const stats = new Port([
    "[1000] heartbeat wifi=3 busy=1 heap=1 inactivity=1",
  ]);
  const lines: string[] = [];
  await monitor(
    "fake",
    0.2,
    { stats: true },
    async () => stats,
    (line) => lines.push(line),
  );
  assert.deepEqual(lines, [
    "capture start: discarded 1 buffered lines",
    "capture span-ms=0 heartbeats=0 busy-ratio=0.000 worker-transitions=0 jobs=0 " +
      "job-ms-median=0 job-ms-max=0 button-poll-gap-ms-max=0 ui-poll-gap-ms-max=0",
  ]);
  t.diagnostic(lines.at(-1)!);
  // Durations come from the device timestamps, so a buffered backlog arriving
  // in one burst still reports the real job times.
  const fixture: CapturedLine[] = [
    { at: 0, line: "[1000] heartbeat wifi=3 busy=0 heap=1 inactivity=1" },
    { at: 0, line: "[1010] worker Idle -> Running id=1 deadline=5" },
    { at: 0, line: "[1020] heartbeat wifi=3 busy=1 heap=1 inactivity=1" },
    { at: 0, line: "[1110] worker Running -> Idle id=1 outcome=Success" },
    { at: 0, line: "[1120] worker Idle -> Running id=2 deadline=5" },
    { at: 0, line: "[1130] worker stale-result id=1" },
    { at: 0, line: "[1320] worker Running -> Idle id=2 outcome=Timeout" },
    { at: 0, line: "[1330] worker Idle -> Running id=3 deadline=5" },
    { at: 0, line: "[1630] worker Running -> Idle id=3 outcome=Success" },
    { at: 0, line: "[1700] heartbeat wifi=3 busy=1 heap=1 inactivity=1" },
    { at: 0, line: "[button] max-poll-gap-ms=31" },
    { at: 0, line: "[button] max-poll-gap-ms=12" },
    { at: 0, line: "[ui] frame screen=0 poll-gap-max=44 heap=1" },
    { at: 0, line: "[1999] worker Running -> Idle id=9 outcome=Success" },
  ];
  assert.equal(
    captureSummary(fixture),
    "capture span-ms=999 heartbeats=3 busy-ratio=0.667 worker-transitions=7 jobs=3 " +
      "job-ms-median=200 job-ms-max=300 button-poll-gap-ms-max=31 ui-poll-gap-ms-max=44",
  );
  // Without a device timestamp, host arrival time still bounds the job.
  assert.match(
    captureSummary([
      { at: 1000, line: "worker Idle -> Running id=1 deadline=5" },
      { at: 1450.6, line: "worker Running -> Idle id=1 outcome=Success" },
    ]),
    /jobs=1 job-ms-median=451 job-ms-max=451/,
  );
  assert.equal(
    captureSummary([]),
    "capture span-ms=0 heartbeats=0 busy-ratio=0.000 worker-transitions=0 jobs=0 " +
      "job-ms-median=0 job-ms-max=0 button-poll-gap-ms-max=0 ui-poll-gap-ms-max=0",
  );
});
