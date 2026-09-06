# sonos-surface

Physical Apple Music/Sonos controls for M5StickS3 + ST25R3916 NFC and Waveshare
ESP32-S3-Touch-AMOLED-1.8. Both boards compile the same portable C++ application
and direct Sonos adapter. No Node server is required.

**Current milestone:** M5StickS3 discovers independent Sonos rooms, selects a room
with A double-click, and resolves the same NFC intent against that room's UUID.
The shared core supports source selection, play/pause/next/previous, absolute and
relative volume, shuffle, repeat, absolute seek, normalized playback metadata,
and bounded queue reading/selection. Runtime `read_only` defaults true.
See the [shared capability contract](docs/sonos-capabilities.md).
See the [Stick manual test](docs/stick-milestone-test.md) and latest
[hardware evidence](docs/hardware.md) for measured versus pending behavior.

Household configuration persists independently of per-device touch calibration.
The Waveshare fit is validated for this unit's current buttons, not all V2 units
or screen edges. Diagnostics continue to report raw coordinates. Stick startup
now uses explicit target wiring; failed peripherals retry independently of Sonos.
Native USB reset/power-cycle limitations remain; see the hardening evidence below.
See [hardware findings and limitations](docs/hardware.md). The product contracts
remain in [docs/product.md](docs/product.md).

Device `read_only=true` blocks all Sonos mutations at HTTP dispatch while retaining
parsing, policy, plans, and state reads. Set it false deliberately to control a
selected configured eligible room. `rooms` is the device's selectable-room universe
for both reading and controlling. Both controls persist in NVS; no rebuild is needed.

See [boot recovery and calibration](#boot-recovery-and-per-device-calibration) for the hardware-hardening workflow.

## Setup on macOS

Requirements: Xcode Command Line Tools (`clang++`), Python 3.9+, Git, curl, and
Arduino CLI (tested with 1.1.1). If missing:

```sh
xcode-select --install
brew install arduino-cli
```

From this repository:

```sh
python3 scripts/setup.py
python3 scripts/test.py
python3 scripts/device.py build stick
python3 scripts/device.py build waveshare
```

Setup pins Arduino-ESP32 3.3.11 and library versions in `scripts/setup.py`.
Libraries live in ignored `.deps/arduino/libraries`; board packages use Arduino's
normal cache. Builds live in `.build`. First setup downloads a large ESP32
toolchain. VS Code tasks provide the same test/build commands.

For portable tests alone, use `python3 scripts/setup.py --host-only` followed by
`python3 scripts/test.py`. Tests use the Mac compiler with address/undefined
behavior sanitizers; they do not contact Sonos.

USB flash verification, reset, monitor, and configuration tools use pyserial:

```sh
python3 -m venv .deps/venv
.deps/venv/bin/python -m pip install pyserial==3.5
```

## First checkpoint: M5StickS3 NFC read

Connect **M5StickS3** by USB and the **ST25R3916 Unit NFC** to its Grove port.
No Wi-Fi, speaker, or Apple account configuration is needed for this checkpoint.
Use a known existing music card; the firmware never writes or formats it.
Existing Text or URI records, and legacy TNF=1 records with an empty type and
raw Apple Music URL payload, are supported as
source + play, with normal shuffle policies. They coexist with v1 JSON Text
cards; there is no need to rewrite your collection for testing or initial use.

```sh
arduino-cli board list
python3 scripts/device.py build stick
.deps/venv/bin/python scripts/device.py flash stick --port /dev/cu.usbmodem2101
.deps/venv/bin/python scripts/device.py monitor stick --port /dev/cu.usbmodem2101
```

Replace the port with the one listed for your board. Close other serial monitors
before flashing. If upload cannot connect, use the board's download mode and
list ports again; do not erase flash as a first troubleshooting step.

**Verified on the connected StickS3:** flash/hash verification, firmware boot,
240×135 display-driver initialization, NFC initialization, and USB intent input.
Legacy album, playlist, and personal-station card playback are now owner-confirmed.
The steps below remain useful for bringing up another device.

For the initial UiFlow2 firmware, upload required holding the **side reset button
until the green LED flashed**, then releasing and retrying upload. After a
successful upload, the board remained in download mode (`waiting for download`).
This software watchdog reset started the flashed application without another
button press (close the monitor first):

```sh
~/Library/Arduino15/packages/esp32/tools/esptool_py/5.3.1/esptool --chip esp32s3 --port /dev/cu.usbmodem2101 --before no-reset --after watchdog-reset chip-id
```

Use this recovery command only when serial confirms the board is still in
download mode. See [recorded findings](docs/hardware.md).

Current Stick builds supply an explicit StickS3 panel/board configuration and
Grove SDA9/SCL10. They bypass both M5GFX autodetection and its NVS detection cache.
Expect ID 26, 240×135, `display-ready=1`, Grove power=1, I2C=1, and NFC ready=1.
ID 155/0×0/2,1 identifies the old autodetection path; check the image before
assuming a cable fault. Display and NFC failures are logged separately and
retried every five seconds while USB/Wi-Fi/Sonos continue.

Expected screen: `sonos-surface / NFC`, `WiFi offline`, and `NFC ready: tap card`.
Serial should include Grove power/I2C initialization and periodic heartbeats.
Tap a card, hold it there for two seconds, remove it, then tap again. Expect:

```text
[nfc] detected uid=...
[nfc] type=... user-bytes=...
[nfc] read=1 valid=1 ms=...
[nfc] TNF=1 type=T payload-bytes=...
[nfc] payload=https://music.apple.com/...
NFC parsed ...
... Selected room unavailable; refresh targets
[nfc] removed; ready for next presentation
```

The configuration error is expected on an unconfigured board; reading/parsing
is the purpose of this checkpoint. A held card should submit only once; a new
tap should submit again. Send back the boot log through both taps and a brief
screen description. URI cards should log `type=U` and `URI prefix=0x00` or
`0x04` before the decoded URL. Empty-type legacy cards log
`legacy raw URL (empty type)` before the URL. For another record type or a non-NFC-A tag,
report the record/type/error; do not rewrite the card to hide a compatibility issue.

## Configure direct playback after the NFC checkpoint

Discover speaker identities and current state without changing playback:

```sh
python3 scripts/discover.py
python3 scripts/probe.py --ip SPEAKER_IP --uid RINCON_SPEAKER_ID
```

`probe.py` uses the same C++ `DirectSonos` adapter as both boards and refuses all
mutating HTTP actions. `discover.py --ip SPEAKER_IP` works without multicast.
Allow local network access if macOS prompts. A missing multicast reply does not
mean a speaker is offline.

Create a private configuration:

```sh
mkdir -p .local
cp config.example.json .local/config.json
python3 -m venv .deps/venv
.deps/venv/bin/python -m pip install pyserial==3.5
```

Edit `.local/config.json`: set Wi-Fi credentials, optional playable `source_url`,
`read_only`, and the explicit `rooms` list. Use current human room display IDs:
`Office` → `office`, `Living Room` → `living-room`, `Kids' Room` → `kids-room`.
Whitespace, hyphens, and underscores collapse to hyphens; ASCII punctuation is
stripped. Non-ASCII names and empty IDs need repair, not guessed transliteration.
See the [complete room contract](docs/product.md#stick-room-and-policy-milestone).

`playlist_shuffle_rooms` maps display IDs to boolean playlist defaults:

```json
{
  "read_only": true,
  "rooms": ["office", "living-room", "kitchen", "bedroom"],
  "playlist_shuffle_rooms": {"bedroom": true}
}
```

Discovery resolves config IDs into UUIDs internally. Only configured, uniquely
resolved, independently eligible rooms are selectable; new household rooms never
join automatically. Missing/ambiguous/invalid IDs stay configured with serial and
display warnings; valid rooms continue. Renames require updating configuration.
A missing room reappears when eligible again. Policy entries do not enroll rooms.
Albums derive shuffle=false everywhere; explicit shuffle always wins. Unlisted
playlist rooms preserve shuffle; a false entry derives false.

Upload config to persist and reboot. `sonos_ip` may remain as an optional SSDP
bootstrap hint; `sonos_uid` is ignored for targeting. Old singular
`playlist_shuffle_room` loads as one true entry, but UUID keys generate repair
warnings and must be replaced with current display IDs. An omitted `rooms` list
selects nothing; missing `read_only` defaults true. Old UUID preferences are ignored;
new room switches persist a display ID in `surface/preferred-id`.

`apple_region: "52231"` matches the reference
default; it is a Sonos service identifier, not an Apple storefront country code.
Apple Music must already work in the household's Sonos app.

Close the monitor, send the file, then reopen it:

```sh
.deps/venv/bin/python scripts/configure.py --port /dev/cu.usbmodem2101 --file .local/config.json
.deps/venv/bin/python scripts/device.py monitor stick --port /dev/cu.usbmodem2101
```

Configuration persists in device NVS and triggers a reboot; credentials are not
printed or compiled into firmware. This prototype does not encrypt NVS. Do not
commit `.local` files or share their contents in logs. Reconfiguration replaces
household settings, preserves the separate device calibration key, and requires no rebuild.
The uploader waits for firmware readiness before sending. The shared USB receive
buffer is 8 KiB: the previous 256-byte default dropped longer configurations,
including one with a Source URL. Expect `[usb] RX buffer=8192 bytes` on updated builds.

On boot, expect Wi-Fi IP, speaker ID, SOAP timing logs, and an observed playback
state even without presenting a card. State refreshes every ten seconds and
after commands. Play music externally and confirm the display updates. Topology notifications trigger fresh discovery; playback state uses ten-second polling.

Use `config-status` in the USB monitor to inspect room IDs, policy, and mode without
credentials. Send `read-only true` to enter testing mode, or deliberately send
`read-only false` to enable requested effects in configured eligible rooms. Both
commands persist and reboot; no build/flash flags are needed. Update your private
config file too so a later full upload preserves your intended mode.

Read-only requests report `READ_ONLY_BLOCKED: device read-only mode; command not sent`
after logging accepted policy and planned effects. At a comfortable selected-room
volume with read-only deliberately disabled,
tap the music card: the request replaces
the target queue and starts it. Volume is preserved. Expect pending operation
names, queue-count observations, then a verified result. Send the operation log
and whether the correct source actually played. A failure after clearing may
leave the queue empty. There are no automatic mutation retries; uncertain
results block further mutations until inspection and a deliberate restart/retry.

## Waveshare checkpoint

To return to the read-only touch diagnostic when investigating targeting:

```sh
python3 scripts/device.py build waveshare --touch-diagnostic
.deps/venv/bin/python scripts/device.py flash waveshare --touch-diagnostic --port /dev/cu.usbmodem101
.deps/venv/bin/python scripts/device.py monitor waveshare --port /dev/cu.usbmodem101
```

This separate `.build/waveshare-touch` image shows one target at a time, raw
coordinates, and a yellow crosshair while touching. Touch
actions are disabled. Set runtime `read_only=true` before diagnostics; USB intents
still use the shared runtime guard. Tap the white plus center and lift; it advances through
six targets at x=92/276 and y=140/270/406. Drags over 16 raw pixels request a retry.
Capture `[calibration]` logs with expected, first-contact, last-contact, and release
coordinates. The diagnostic does not apply calibration or save samples automatically; reboot to repeat.
Use `scripts/calibrate.py` below to fit and persist the captured samples.
Drawing uses a 329,728-byte PSRAM canvas and one full-frame transfer
to avoid missing thin primitives on CO5300. Expect `[display] PSRAM canvas=329728
bytes; full-frame flush`. Normal builds below restore the controller UI after diagnosis.

The connected Waveshare is `/dev/cu.usbmodem101`, USB serial
`28:84:85:3B:7F:A0`; the StickS3 is `/dev/cu.usbmodem2101`. Recheck enumeration if
ports change. A private `.local/waveshare-config.json` has been prepared and saved
to the Waveshare with Wi-Fi, Office identity, and the known playable playlist for
the Source button. For another setup, copy `.local/config.json` to that file and
set `source_url`; never overwrite working private configuration with the example.

```sh
python3 scripts/device.py build waveshare
arduino-cli board list
.deps/venv/bin/python scripts/device.py flash waveshare --port /dev/cu.usbmodem101
.deps/venv/bin/python scripts/configure.py --port /dev/cu.usbmodem101 --file .local/waveshare-config.json
.deps/venv/bin/python scripts/device.py monitor waveshare --port /dev/cu.usbmodem101
```

These commands use the same runtime room/mode configuration as Stick. Ensure the
Waveshare config has `read_only=true` and an explicit room list during migration.

**Observed recovery:** after flashing the playback image, monitoring once stopped
at ROM `entry 0x403c88b8`, before application logs. Normal and USB-reset esptool
connection attempts failed. Unplugging/reconnecting Waveshare USB recovered the
same image without reflashing; application boot and Office playback then passed.
If this recurs, try that power cycle first; if battery-powered, USB removal alone
will not guarantee a cold boot.
If recovery still requires download mode, the vendor specifies holding **BOOT**
during power-on; this board has PWR/BOOT, not the StickS3's reset button.
See [vendor controls](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.8).
No flash erase was needed. The earlier ROM-only symptom remains unexplained; the current workflow uses a watchdog
reset and verifies application readiness after upload. A cold power cycle remains
the fallback if serial/download-mode recovery cannot reach the chip.

Expected application logs include `Waveshare V2 CO5300/CST820`,
`display=368x448 touch=0x15`, `adapter ready=1`, Wi-Fi connection, verified Office
identity, and an observed title/state. Buffered rendering and calibrated center-button touches
were owner-confirmed before this pass; repeat the short physical checklist after hardening.
The adapter probes FT3168 at `0x38` (V1/SH8601) or CST820 at `0x15` (V2/CO5300).
Confirm the serial revision matches the physical label. The screen has Source,
Refresh, Play, and Pause buttons. Source submits the configured Apple Music URL
through the same application path as NFC. Tap once per action and report touch
coordinates, the operation log, visible state, and audible result. Begin with
Refresh after boot recovery; test Source/Play/Pause only after confirming the
selected configured room and `SONOS_MODE=CONTROL` in serial.

## Boot recovery and per-device calibration

Flash now uses the pinned Arduino upload recipe with `--after watchdog-reset`,
at 115200 upload baud, then waits for READY or an idle heartbeat. Hash verification proves flash contents;
`Application ready` proves the runtime is responding, even if a peripheral reports
failure. The monitor/configuration tools suppress pyserial's DTR/RTS writes; repeated
opens have preserved uptime on this Mac. Opening a different monitor can still reset USB.
In an interactive terminal the monitor forwards typed, newline-terminated USB
commands. Use these explicit commands with either board name and its current port:

```sh
.deps/venv/bin/python scripts/device.py reboot waveshare --port /dev/cu.usbmodem101
.deps/venv/bin/python scripts/device.py reset waveshare --port /dev/cu.usbmodem101
.deps/venv/bin/python scripts/device.py monitor waveshare --port /dev/cu.usbmodem101 --seconds 20
```

`reboot` requires a running, idle application. `reset` uses a bounded esptool
connection and watchdog reset, with no flash write. If serial already says
`waiting for download`, use `reset ... --download-mode`. Close all other monitors.
If only ROM `entry ...` appears and reset cannot connect, physically power-cycle;
USB removal is insufficient if a battery still powers the board. Waveshare uses
PWR/BOOT: hold PWR alone for six seconds to turn off, release, then click PWR to
turn on. Leave BOOT released for normal startup; holding it during power-on
selects download mode ([vendor controls](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8)).
Stick has a side reset button. Re-enumerate ports before retrying. Do not
erase NVS or repeatedly flash to recover a pre-application stall.

The earliest application marker is `[boot] application reached reset-reason=...`.
A ROM entry line is the handoff to the second-stage bootloader, not proof that
Arduino setup ran. Expander/touch/NFC failures after that marker are application
peripheral failures. They log and retry every five seconds; successful networking
and the Sonos worker continue. Waveshare needs a touch-controller response to
choose V1 vs V2 display hardware, so initial display setup waits for that evidence.
The Waveshare USB command `peripherals-retry` exercises that reset/recovery path
without rebooting Wi-Fi/Sonos. The QSPI bus is initialized once and reused; a failed
initial QSPI allocation needs a reboot (the pinned driver cannot safely begin twice).
One macOS esptool watchdog teardown error occurred after verified flash at 460800;
115200 completed cleanly. If the tool reports failure, boot is still inspected,
but the command returns failure so a running old image cannot certify a new upload.
Wi-Fi retries every 30 seconds, including an unavailable AP at startup; Sonos
keeps bounded reads and ten-second refreshes. No playback is replayed at reboot.
Both boards recovered automatically from a measured 291-second Wi-Fi outage;
boot with the AP already absent remains untested. Observation errors now clear
after successful reads while preserving the last command outcome. Intermittent
NFC identification/reactivation failure remains under diagnosis; removal and
retap recovered it in the observed cases.

Calibration is adapter-owned JSON in the existing NVS **namespace `surface`, key
`touch`**. Household JSON stays at **`surface/config`**. Separate keys deliberately
prevent household replacement/copying from erasing or transferring a physical
unit's correction. Compiled target wiring stays in the board adapters; cards,
MusicIntent, policies, and SurfaceCore never contain calibration.

The v1 model is axis-aligned affine, matching the recorded experiment:
`x = round(x_scale * rawX + x_offset)` and likewise y. `controller` is decimal
21 (`0x15`, CST820 V2) or 56 (`0x38`, FT3168 V1). Example identity calibration:

```json
{"version":1,"controller":21,"x_scale":1,"x_offset":0,"y_scale":1,"y_offset":0}
```

Missing, malformed, unsupported-version, out-of-bounds, or wrong-controller data
falls back to identity, with an explicit serial warning. Invalid updates preserve
the previous value. Positive scales must be 0.5–1.5, offsets within ±112 pixels;
these are conservative support bounds for this model, not measured sensor limits.
Raw and mapped out-of-screen coordinates reject rather than clamping to buttons.
Saving applies immediately and requires release before another control tap.
Diagnostics remain raw even with saved calibration. To restore identity, upload
the example above with the correct controller ID.

For another physical unit, flash the read-only diagnostic and capture a complete
six-target run (tap each white plus center and lift). Then fit, inspect the JSON
and training residual, upload, and verify after reboot:

```sh
.deps/venv/bin/python scripts/device.py monitor waveshare --port BOARD_PORT > .local/touch-samples.log
python3 scripts/calibrate.py --samples .local/touch-samples.log --controller 0x15 --output .local/touch.json
.deps/venv/bin/python scripts/calibrate.py --port BOARD_PORT --file .local/touch.json
.deps/venv/bin/python scripts/device.py reboot waveshare --port BOARD_PORT
.deps/venv/bin/python scripts/calibrate.py --port BOARD_PORT
```

Stop the capture with Ctrl-C after Done. The fitter rejects incomplete, moving,
poorly spread, or high-residual runs; it never writes a device during fitting.
Restore a normal **read-only** build and confirm fresh center taps from raw/mapped
hit logs before enabling Office playback. No source edits/rebuilds are needed to
change calibration itself. A new model needs a new version; v1 is not a full-screen
accuracy claim.

## Supported commands and structure

Supported: Apple album/playlist/track/station URLs; legacy URL → source + play;
v1 JSON source with explicit play/pause or omitted transport; standalone
play/pause/next/previous; `volume: {"set": 25}` / `{"delta": -5}`;
`shuffle: true/false`; `repeat: "off"/"all"/"one"`;
`seek: {"positionMs": 125000}`; `queueIndex: 2` (third active-queue item).

Omission preserves source/queue, volume, repeat, shuffle (unless policy derives
it), and playing versus non-playing transport during source replacement.
Relative volume reads a fresh baseline, clamps to 0–100, and freezes one absolute
target. Mode writes preserve the omitted shuffle/repeat component. Next/previous
cannot combine with source or mode fields and are never automatically retried.
Stations keep their existing direct-radio behavior; explicit station mode fields
reject. Seek/queue selection preserve transport and cannot combine with source,
next/previous, or each other. Queue selection requires active queue playback.
Unknown fields and required extensions reject before mutation.

USB `queue [start,count]` reads a selected-room queue page, count 1–20.
For example, `queue [0,2]` reads the first two entries. The same read-only
Mac probe accepts `--queue-start`, `--queue-count`, `--samples`, and `--interval-ms`.
Metadata includes album, normalized artwork URL, millisecond position/duration,
queue index/total, mute, and normalized modes/source. Artwork is never downloaded.

USB commands (newline terminated): `rooms`, `room-next`, `status`, `source`,
`play`, `pause`, `toggle`, `next`, `previous`, `reboot`, a bare Apple URL, or v1 intent JSON.
Settings controls use v1 JSON, for example:

```json
{"format":"sonos-surface","version":1,"intent":{"volume":{"delta":-5},"repeat":"off"}}
```

M5 A single-click refreshes, A double-click cycles configured eligible rooms,
and B toggles play/pause from a fresh read of the room bound at the press.
Unknown/transitioning playback rejects; toggle never changes the source, queue,
volume, or mode. Cycling only changes targeting/state. Rooms sort by case-insensitive
human name, then UUID. Boot restores the preferred display ID when available;
otherwise a visible warning accompanies the first valid configured fallback.
Selection invalidation uses the same fallback; accepted work remains bound to its
original UUID and can fail but never follows that fallback.
Input while the worker is busy is rejected; there is no hidden request queue.
Accepted work freezes target and policy before the worker begins its reads.


Button regression tests use the pinned M5Unified 0.2.21 button state machine.
`setup.py --host-only` downloads just its portable source/license, without the
Arduino SDK. The Stick defers NFC polling while an A gesture is pending and logs
button edges, decided click counts, and maximum polling gaps. Completed manual
work notices clear to Ready; background refreshes do not replace the idle notice.

The existing Waveshare USB hardware diagnostics remain available:
`touch-calibration`, `touch-calibration {JSON}`, `peripherals-retry`,
`display-edge N [R]`, and `display-edge off`.
NFC supports one UTF-8 NDEF Text record, one Apple Music URI record (full URL
or HTTPS prefix compression), or one legacy TNF=1/empty-type raw URL record on
an NFC-A tag supported by the vendor library.
NFC-B/F/V, multiple records, Smart Posters, and writer mode are not implemented.
Unsupported optional extensions are ignored for execution;
there is no editor to round-trip them yet.

- `libraries/SurfaceCore/src/`: intent, policy, serial planner/executor, AppState.
- `libraries/SurfaceSonos/src/`: Apple metadata, SOAP, readiness/verification;
  network-independent `LocalHttp` boundary.
- `libraries/SurfaceDevice/src/`: ESP32 HTTP/Wi-Fi/NVS/worker runtime and separate
  Stick/Waveshare adapters.
- `firmware/sonos_surface/`: shared Arduino entry point.
- `tests/`: portable behavioral tests and read-only real-Sonos diagnostic.
- `scripts/`: pinned setup, tests, build/flash/monitor, discovery/configuration.

No generic workflow engine, queue editor, artwork rendering/downloading, voice, or
full retry/reconciliation framework is included in this slice.

Diagnostics stream over USB serial; the device has no stored log history to
retrieve after unplugging. Use a laptop and live `device.py monitor` capture for
walk-around testing. NVS stores configuration, preference, and calibration, not
a history of requests or topology events.
