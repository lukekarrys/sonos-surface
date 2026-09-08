# sonos-surface

Physical Apple Music/Sonos controls for M5StickS3 + ST25R3916 NFC and Waveshare
ESP32-S3-Touch-AMOLED-1.8. Both boards use the same portable C++ application and
direct Sonos adapter; no server is required.

The current system reads NFC cards, selects configured rooms, controls playback,
and observes metadata/modes/volume/timing. Waveshare adds its accepted touch UI,
background artwork, and bounded queue browsing/selection. Source policy is shared:
new albums play in order once; playlists preserve shuffle and repeat off; tracks
repeat off unless their room overrides it. NFC writing is specified, not implemented.

## Current contracts

| Document | Owns |
| --- | --- |
| [Product](docs/product.md) | Scope, architecture, family workflows, future boundaries |
| [Intent and cards](docs/intent.md) | Schema, source/mode invariants, NFC encoding, future simple writer |
| [Room configuration and policy](docs/policy.md) | Authoritative room object, identity, defaults/overrides, precedence/provenance, migration |
| [Planner/executor](docs/planner-executor.md) | Frozen acceptance, ordering, preservation, dispatch safeguards, failures/reconciliation |
| [Sonos capabilities](docs/sonos-capabilities.md) | Observations, bounded queues, seek/selection, Apple/Sonos mapping |
| [Waveshare frontend](docs/waveshare-frontend.md) | Accepted 1.8-inch interaction, rendering and artwork worker |
| [Hardware](docs/hardware.md) | Board wiring/revisions, calibration, durable measured evidence, unresolved bugs |

## Setup on macOS

Requirements: Xcode Command Line Tools (`clang++`), Python 3.9+, Git, curl, and
Arduino CLI (tested with 1.1.1). Install missing tools with `xcode-select --install`
and `brew install arduino-cli`, then:

```sh
python3 scripts/setup.py
python3 scripts/test.py
python3 scripts/device.py build stick
python3 scripts/device.py build waveshare
python3 -m venv .deps/venv
.deps/venv/bin/python -m pip install pyserial==3.5
```

Setup pins Arduino-ESP32 3.3.11 and library versions in `scripts/setup.py`.
Dependencies/builds live in ignored `.deps`/`.build`; Arduino uses its normal board
package cache. Update dependency pins and setup instructions together. For portable
sanitizer tests alone, run `setup.py --host-only`, then `test.py`; tests never
contact Sonos. VS Code tasks expose the same build/test commands.

## Configuration

Discover names, display IDs, UUIDs, and state without playback changes:

```sh
python3 scripts/discover.py
python3 scripts/probe.py --ip SPEAKER_IP --uid RINCON_SPEAKER_ID
```

`probe.py` uses the shared C++ adapter and permanently blocks mutations.
`discover.py --ip SPEAKER_IP` works without multicast; an absent multicast reply
does not imply the speaker is offline. Allow local network access if macOS prompts.

For a new device, copy `config.example.json` into `.local/config.json` (use a
separate `.local/waveshare-config.json` for Waveshare). Never overwrite a working
private config with the example. Set Wi-Fi credentials, optional playable
`source_url`, and intended room policies. Apple Music must already work in Sonos;
`apple_region: "52231"` is a Sonos service descriptor, not a storefront country code.

```json
{
  "read_only": true,
  "rooms": {
    "office": {},
    "living-room": {"playlist": {"shuffle": true}},
    "bedroom": {}
  }
}
```

`rooms` keys are both the allowlist and home for room-specific exceptions. Empty
values allow a room with shared defaults. Only current, uniquely resolved,
independently eligible rooms are selectable. New discovered rooms are not enrolled;
missing/ambiguous/invalid entries warn. Renames require editing the display ID.
See [policy](docs/policy.md) for the exact identity, defaults, and override contract.

`read_only=true` blocks all Sonos mutations at HTTP dispatch. False permits
requested effects in configured/selectable rooms with identity/topology safeguards.
Room configuration controls availability, not permission. Missing mode defaults
true and missing rooms selects nothing. Normal firmware has no room-specific
mutation build flags. Keep autonomous testing read-only or use pure `preview`.

Deployment guidance: configure Sonos's own per-room maximum-volume setting as the
hard safety limit. The owner's 1.8-inch slider is experimental; the shared logical
volume range remains 0–100, and a future larger kids-room UI is expected to use
+/- buttons. This project does not add a software volume limiter.

For existing devices, follow the [one-time migration](docs/policy.md#replacement-acceptance-and-migration):
rooms arrays and either old playlist-shuffle key now reject. Preserve the current
mode and room set, move existing playlist overrides beneath their room entry,
flash current firmware, and upload the converted file. No room gets repeat-one
unless deliberately configured. UUIDs do not belong in human config.

## Flash and configure

Connect the intended board, close other serial monitors, and enumerate ports:

```sh
arduino-cli board list
.deps/venv/bin/python scripts/device.py flash stick --port STICK_PORT
.deps/venv/bin/python scripts/configure.py --port STICK_PORT --file .local/config.json
.deps/venv/bin/python scripts/device.py monitor stick --port STICK_PORT
```

For Waveshare, after building its normal image:

```sh
.deps/venv/bin/python scripts/device.py flash waveshare --port WAVESHARE_PORT
.deps/venv/bin/python scripts/configure.py --port WAVESHARE_PORT --file .local/waveshare-config.json
.deps/venv/bin/python scripts/device.py monitor waveshare --port WAVESHARE_PORT
```

Ports can change; identify the board before flashing. Flash verifies hashes, uses
watchdog reset at 115200 baud, then checks application READY or an idle heartbeat.
Application readiness does not by itself prove working peripherals or visible pixels.

Config upload validates before replacement, advances the local revision, and
reboots. Busy/invalid updates reject. Household JSON persists in `surface/config`;
preferred display ID in `surface/preferred-id`; calibration separately in
`surface/touch`. Upload does not erase calibration. Credentials are not printed
or compiled into firmware; prototype NVS is not encrypted. Keep `.local` private.

Boot reads existing Sonos state without input. Expect `SONOS_MODE`, `device-config`,
room resolution, and state logs. Playback polls nominally every ten seconds;
topology events invalidate discovery. Input during worker activity rejects as busy.
An interactive monitor forwards newline-terminated commands; Ctrl-C closes it.

## Commands and diagnostics

| USB command | Effect |
| --- | --- |
| `config-status` | Print mode, room exceptions, policy revision; no credentials |
| `rooms`, `status` | Refresh discovery/selected-room observations |
| `room-next`, `room-select DISPLAY_ID` | Select configured eligible room and read it; no playback effect |
| `preview URL`, `preview {v1 JSON}` | Parse, validate, resolve policy, print plan only; never submit or contact Sonos |
| `queue [start,count]` | Read a bounded selected-room page; count 1–20 |
| `play`, `pause`, `toggle`, `next`, `previous` | Submit one transport request |
| `source`, bare Apple URL, v1 JSON | Submit configured source or supplied intent |
| `read-only true`, `read-only false` | Persist mode and reboot; update the private config to match |
| `config {JSON}` | Replace full device config and reboot; prefer the private-file uploader |
| `reboot` | Reboot an idle application |

Example pure policy check (catalog ID is illustrative):

```text
preview https://music.apple.com/us/album/example/12345
```

Expect shuffle=false/repeat=off with `source-default:album` in an ordinary room.
A playlist preview shows the selected room's shuffle exception or preservation,
and repeat=off. `policy` diagnostics carry independent `{value, origin}` entries
for shuffle and repeat; null/preserve differs from false/off. Invalid sourced
combinations print PREVIEW_INVALID and never create a worker job.

Advanced intent example, sent directly to submit or after `preview ` to inspect:

```json
{"format":"sonos-surface","version":1,"intent":{"volume":{"delta":-5},"repeat":"off"}}
```

See [intent](docs/intent.md) for source/mode restrictions and volume/seek/queue
capabilities. Accepted requests freeze target UUID and resolved policy/revision.
Changing selection/config never retargets accepted work. A read-only request may
succeed without a write if already satisfied; otherwise READ_ONLY_BLOCKED identifies
the first required effect. Uncertain effects are never automatically retried.

M5 A single-click refreshes; A double-click cycles rooms; B toggles using a fresh
read of the bound room. NFC reads existing Text/URI/empty-type URL cards and v1
JSON Text cards without writing. One held presentation submits once; retapping
requires removal. Unsupported formats report errors instead of guessing payloads.

Waveshare uses release-to-submit transport/mode/volume/seek controls and four-item
queue pages. USB `ui-screen now|rooms|queue` navigates its normal screens without
touch or playback. Hardware diagnostics include `touch-calibration`,
`touch-calibration {JSON}`, `peripherals-retry`, `display-edge N [R]`, and
`display-edge off`. See the [frontend contract](docs/waveshare-frontend.md).

Logs stream over USB only; there is no stored history after unplugging. For a
bounded capture use `device.py monitor BOARD --port PORT --seconds 30`, redirecting
output to a private `.local` log. Live laptop capture is needed for battery tests.

## Boot recovery and per-device calibration

The serial helper suppresses pyserial DTR/RTS writes that caused native USB resets
on this Mac. Other monitors may reset on open. For an idle app use `reboot`; for
a stalled interface use the bounded watchdog-reset tool without flash writes:

```sh
.deps/venv/bin/python scripts/device.py reboot waveshare --port BOARD_PORT
.deps/venv/bin/python scripts/device.py reset waveshare --port BOARD_PORT
```

If serial already reports `waiting for download`, use `reset ... --download-mode`.
A ROM entry line does not prove application startup. If bounded reset cannot
connect, physical power recovery may be required; see [hardware](docs/hardware.md#usb-boot-power-and-networking-limits).
Do not erase NVS or repeatedly flash to diagnose a pre-application stall.

Stick should report ID26, 240×135, Grove9/10, display-ready and NFC-ready. Missing
NFC retries while Wi-Fi/Sonos continue. Waveshare should identify V1 SH8601/FT3168
or V2 CO5300/CST820, 368×448, and its 329,728-byte PSRAM canvas. Peripheral failures
retry; `peripherals-retry` exercises that path. QSPI allocation failure needs reboot.

Calibration is per-unit, controller-specific affine JSON. Missing/invalid data
warns and uses identity; valid household config uploads do not touch it. Query:

```sh
.deps/venv/bin/python scripts/calibrate.py --port BOARD_PORT
```

To calibrate another unit, keep runtime read_only true and flash raw diagnostics:

```sh
python3 scripts/device.py build waveshare --touch-diagnostic
.deps/venv/bin/python scripts/device.py flash waveshare --touch-diagnostic --port BOARD_PORT
.deps/venv/bin/python scripts/device.py monitor waveshare --port BOARD_PORT > .local/touch-samples.log
```

Tap and release each of six white plus centers; stop capture after Done. Diagnostics
stay raw and disable touch actions; USB commands still use the normal runtime gate.
Fit, review the residual/JSON, upload, and verify retention:

```sh
python3 scripts/calibrate.py --samples .local/touch-samples.log --controller 0x15 --output .local/touch.json
.deps/venv/bin/python scripts/calibrate.py --port BOARD_PORT --file .local/touch.json
.deps/venv/bin/python scripts/device.py reboot waveshare --port BOARD_PORT
.deps/venv/bin/python scripts/calibrate.py --port BOARD_PORT
```

Use 0x38 for V1, 0x15 for V2. Fit rejects incomplete/moving/poorly spread runs;
it does not write a device. Restore the normal build and verify fresh center taps.
An identity reset uses the correct controller with this JSON:

```json
{"version":1,"controller":21,"x_scale":1,"x_offset":0,"y_scale":1,"y_offset":0}
```

The model, bounds, and measured unit/edge limitations live in hardware. Never copy
another unit's fit merely because its controller matches.

## Repository structure

- `libraries/SurfaceCore/src/`: portable intent, policy, planning/execution, AppState.
- `libraries/SurfaceSonos/src/`: SOAP/Apple metadata behind `LocalHttp`.
- `libraries/SurfaceDevice/src/`: ESP32 runtime, config parser, board adapters.
- `firmware/sonos_surface/`: shared Arduino entry point.
- `tests/`, `scripts/`: portable/protocol fixtures and setup/device tooling.

Keep network/hardware SDKs out of portable layers and generated/private files
untracked. NFC writing, a writer server, 4.3C, voice, and broader UI work remain
separate future tasks.
