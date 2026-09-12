Add DEVICE TOOLING FOR AUTONOMOUS ON-DEVICE VERIFICATION.

Goal: an agent with both boards attached can identify which serial port is which target,
send one USB command and capture its reply without an interactive terminal, drive the
ws-1.8 UI without a finger, and read UI state back. Nothing here changes product
behavior; every later UI prompt depends on it.

Do NOT change Sonos, policy, intent, sleep, or read_only semantics.
Do NOT make injected input count as local activity (USB commands do not today).
Do NOT add a new dependency.
Do NOT add historical checkpoint documentation.

Read first: AGENTS.md; README.md (Commands and diagnostics; Boot recovery and
per-device calibration; the serial helper rules); scripts/serial-device.ts (openPort,
readyPort, waitReady, request; the no-DTR/RTS rule); scripts/device.ts (monitor, ports,
flash, the build option pattern for --touch-diagnostic); scripts/configure.ts and
scripts/calibrate.ts (existing request() consumers); libraries/SurfaceDevice/src/
Runtime.cpp handle() (USB command dispatch, boardCommand hook, activity rules in loop());
Waveshare.cpp boardCommand (ui-screen, display-edge, peripherals-retry) and pollInput
(touch sampling, `held`, activity); Stick.cpp boardCommand; DevicePower.h;
tests/tooling.test.ts, tests/config-profile.test.ts, tests/discover.test.ts (host tools
are tested with injected fakes, never real ports); tests/waveshare_ui_test.cpp.

==================================================
1. `board` USB COMMAND (BOTH TARGETS)
==================================================

Add a read-only USB command `board` that prints one line:

  board target=ws-1.8 adapter="Waveshare V2 CO5300/CST820" ready=1

  board target=stick-s3 adapter="M5StickS3 ID26" ready=1

using the hardware target id from the build define and the adapter notice the board
already produces at boot. No side effects, no Sonos work, no activity, safe while busy.

Add it to the `Commands:` help line and README's USB command table.

==================================================
2. PORT IDENTIFICATION
==================================================

Add `node --run ports -- --identify`.

For each Espressif port (vendorId 303a): open it with the existing quiet serial helper,
wait for application readiness with a short bound (about 10 s), send `board`, and print
the target beside the port. A port that does not answer is reported as
"no application response (asleep, busy booting, or not sonos-surface)". Never send
anything before readiness, never send credentials, and never write to a port that is not
an Espressif device.

Two identical models are still flashed by explicit --port; identification only removes
the guessing.

Add `--identify` to the ports help text and README's flash section.

==================================================
3. NON-INTERACTIVE USB COMMAND TOOL
==================================================

Add `node --run usb -- --port PORT --command "lifecycle-status" [--expect "lifecycles "]
[--seconds N] [--lines M]`.

Behavior:

- wait for readiness (readyPort), clear, send the command once, then print device lines
  until a line contains --expect (default: the first line that is not a heartbeat and
  not an echoed transition log, within --seconds, default 10) or M lines were printed
- exit 0 only when the expected line was seen; nonzero on timeout, serial failure, or a
  known rejection token for that command (reuse the tokens configure.ts already knows)
- refuse `config ...` and `read-only ...` through this tool and point at configure/flash;
  they carry credentials or reboot the device and have their own verified flows
- print only device text; never log the command arguments in errors (values may be
  private)

Implement it in scripts/usb.ts with the request logic factored so tests can inject a fake
DevicePort exactly as tests/config-profile.test.ts injects `ready`.

==================================================
4. INPUT INJECTION ON ws-1.8 (NORMAL BUILD ONLY)
==================================================

Add development-only USB commands, compiled out of the touch-diagnostic build:

  ui-touch X Y [fingers]     one sample in calibrated screen coordinates
                             (0..367, 0..447), fingers defaults to 1
  ui-touch release           one release sample
  ui-button boot             one debounced BOOT short-press event

Semantics:

- injected samples enter the SAME path as hardware samples after calibration
  (today: ui.touch / requestQueue in pollInput; later: the LVGL indev). They bypass the
  affine calibration because they are already screen coordinates, and nothing else.
- samples are queued (bounded, 32 entries; overflow rejects with an error line) and
  consumed one per touch poll (30 ms) in place of the hardware sample, so a host-side
  drag is a sequence of ui-touch commands. A physical finger detected during injection
  cancels the queue and logs it.
- `ui-button boot` enters whatever action the BOOT release will map to. Until
  todo/3-lvgl.md and todo/4-ws-1.8-multiscreen.md assign one, it logs
  `[ui] inject button=boot (no action)`.
- injected input NEVER sets LocalActivity and never postpones sleep; keep the USB rule
  from docs/product.md intact.
- injected input never bypasses admission, read_only, or policy: it is exactly a finger.
  The committed default profile has read_only=false, so injected taps on transport,
  volume, seek, and queue controls mutate the configured room for real; AGENTS.md
  permits that on configured rooms, and the owner mutes the amplifier during loops.
  Use a read_only=true profile deliberately when a test would otherwise be disruptive.
- log every injection loudly: `[ui] inject touch x=.. y=.. fingers=..` and the resulting
  hit, mirroring the existing `[touch] raw=.. mapped=..` line.
- `held` (release required after boot/recovery) applies to injected samples too; the
  tool sends a release first when needed.

Host convenience in scripts/ui.ts, built on the usb tool:

  node --run ui -- --port PORT tap X Y
  node --run ui -- --port PORT drag X1 Y1 X2 Y2 [--steps N] [--interval-ms 30]
  node --run ui -- --port PORT release
  node --run ui -- --port PORT button boot
  node --run ui -- --port PORT screen now|rooms|queue
  node --run ui -- --port PORT state

Each subcommand prints the device's `[ui]` lines for that action and exits nonzero if
the device rejected it.

==================================================
5. `ui-state` USB COMMAND (ws-1.8)
==================================================

Print one JSON line with the current UI model: screen and sub-view, touching/contact
control, active previews, toast, busy/online/readOnly, observed transport/position/
duration/volume, and the last frame's draw/flush/total times and poll-gap maximum.

Later prompts extend this line (pending/interaction/visible fields, subscription health);
design it as a nlohmann::json object built incrementally (docs/hardware.md explains the
main-loop stack limit for large initializers).

==================================================
6. BOUNDED LOG CAPTURE
==================================================

`node --run monitor -- TARGET --port PORT --seconds N` exists. Add `--until "TOKEN"` so a
capture ends early when a line contains TOKEN (still bounded by --seconds); add `--stats`
so the capture ends with one summary line (heartbeat count, `busy=1` ratio, worker
Idle/Running transition count, median and maximum job duration from those transitions,
and the maximum `[button] max-poll-gap-ms`); and make the non-TTY path usable: when
stdin is not a TTY, monitor reads no commands and exits at the bound instead of
waiting. todo/2-user-input-priority.md measures its before/after with `--stats`.

==================================================
7. TESTS
==================================================

Host (node --test, injected fakes only):

- usb: success token, timeout, rejection token, refused commands, no argument leakage in
  errors
- ports --identify: answering and non-answering fake ports, non-Espressif ports skipped
- ui: drag step generation and ordering; release-first when held
- monitor --until and --stats (summary computed from a fixture log)

Portable C++ (sanitized):

- the injected-sample queue: bounded, FIFO, cancelled by a physical finger, consumed one
  per poll, release semantics, and `held` handling, as a small header separate from
  Arduino code
- `ui-state` JSON shape from a WaveshareUi fixture

Build: both targets and the ws-1.8 touch-diagnostic variant build with --warnings more;
the diagnostic variant must not contain the injection commands.

==================================================
8. DOCUMENTATION
==================================================

Update README.md: USB command table (board, ui-touch, ui-button, ui-state), the ports and
monitor usage lines, and a short "Autonomous device verification" paragraph under
Commands and diagnostics describing usb/ui/ports --identify and the rule that injected
input is not activity and not a mutation bypass. Update docs/waveshare-frontend.md's
diagnostics paragraph. Do not add history.

==================================================
9. GREEN BASELINE
==================================================

  node --run format
  node --run format:cpp
  node --run check
  node --run check:full

Keep VS Code diagnostics green; regenerate node --run cpp:configure if host targets were
added.

==================================================
10. AUTONOMOUS DEVICE VERIFICATION
==================================================

With both boards attached (they are today: two Espressif ports):

1. `node --run ports -- --identify` names one stick-s3 and one ws-1.8.
2. Flash both (without --config, preserving configuration).
3. `node --run usb -- --port WS --command lifecycle-status` returns the JSON line.
4. `node --run ui -- --port WS screen queue`, then `state`, shows the Queue screen.
5. `node --run ui -- --port WS tap 184 286` on Now Playing logs the Play/Pause release
   action, one transport intent, and the resulting job; the configured room pauses or
   plays. Tap again to restore it.
6. `node --run ui -- --port WS drag 52 351 316 351 --steps 20` logs a volume preview
   following the drag and one absolute volume intent on release; read the prior volume
   from `ui-state` first and restore it afterward with a second drag or a USB intent.
7. `node --run monitor -- ws-1.8 --port WS --seconds 30 --until heartbeat` exits early.
8. `node --run monitor -- stick-s3 --port STICK --seconds 120 --stats` ends with the
   summary line (heartbeat count, busy ratio, job transitions and durations, poll gap);
   its numbers are the baseline todo/2-user-input-priority.md compares against.
9. Confirm from the heartbeat that inactivity was not reset by any of the above (use a
   short sleep timeout in a throwaway profile, then restore).

==================================================
11. COMPLETION
==================================================

Stop when:

1. `board` answers on both targets
2. `ports --identify` maps both attached boards
3. `usb` sends one command and returns its reply non-interactively with exit codes
4. `ui-touch`, `ui-button`, `ui-state`, and the `ui` host tool work on ws-1.8 and are
   absent from the touch-diagnostic build
5. injected input records no activity and bypasses nothing
6. monitor has --until and a non-TTY mode
7. host and portable tests cover the above
8. README and frontend docs describe the commands
9. all checks/builds pass and section 10 was run and logged

Then commit the finished work: todo/README.md "How to run" rules, a `todo 1:`
subject, the README status cell set to `implemented (<hash>)`, only on a green
baseline, never a push. The prompt is not complete with uncommitted work.

Then report only:

- the exact commands added and their exit-code contract
- the injection queue semantics
- anything the later UI prompts must know to use them

Do not begin the LVGL milestone automatically.
