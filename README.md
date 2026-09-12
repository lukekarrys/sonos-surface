# sonos-surface

Physical Apple Music/Sonos controls for M5StickS3 + ST25R3916 NFC and Waveshare ESP32-S3-Touch-AMOLED-1.8. Both boards use the same portable C++ application and direct Sonos adapter; no server is required.

The current system reads NFC cards, selects configured rooms, controls playback, and observes metadata/modes/volume/timing. Waveshare adds its accepted touch UI, background artwork, and bounded queue browsing/selection. Source policy is shared: new albums play in order once; playlists preserve shuffle and repeat off; tracks repeat off. Persisted device and room policy can override these code-owned defaults. Stick also hosts a local phone-friendly NFC writer with explicit arming and semantic read-back verification.

## Current contracts

| Document | Owns |
| --- | --- |
| [Product](docs/product.md) | Scope, architecture, family workflows, future boundaries |
| [Intent and cards](docs/intent.md) | Schema, source/mode invariants, NFC encoding, simple writer |
| [Local card writer](docs/tag-writer.md) | Phone workflow, transient arming, editing, capacity, LAN API |
| [Room configuration and policy](docs/policy.md) | Authoritative room object, identity, defaults/overrides, precedence/provenance |
| [Planner/executor](docs/planner-executor.md) | Frozen acceptance, ordering, preservation, dispatch safeguards, failures/reconciliation |
| [Sonos capabilities](docs/sonos-capabilities.md) | Observations, bounded queues, seek/selection, Apple/Sonos mapping |
| [Waveshare frontend](docs/waveshare-frontend.md) | Accepted 1.8-inch interaction, rendering and artwork worker |
| [Hardware](docs/hardware.md) | Board wiring/revisions, calibration, durable measured evidence, unresolved bugs |

## Setup and checks

Use Node 24.12 or later within Node 24, npm, the macOS command-line developer tools (clang++ and libcurl), Arduino CLI **1.1.1**, and clang-format **19**. Node executes TypeScript directly; TypeScript checks types without emitting code. The npm runtime constraint rejects unsupported Node versions during installation.

Install the developer tools with `xcode-select --install` if needed. Arduino CLI 1.1.1 is available from [Arduino's releases](https://github.com/arduino/arduino-cli/releases/tag/v1.1.1). clang-format is an external tool, installed separately from npm; on macOS, `brew install llvm@19` supplies the version 19 formatter used by this baseline. A standalone `clang-format` 19 installation also works. Keep the formatter major version consistent when updating the baseline.

New-machine workflow, after installing those external tools:

```sh
git clone https://github.com/lukekarrys/sonos-surface.git
cd sonos-surface
npm install
cp .env.example .env
# Edit .env with this computer's Wi-Fi credentials.
node --run setup
node --run cpp:configure
node --run check
```

npm installs TypeScript, Node type definitions, Prettier, and serialport. Setup installs Arduino-ESP32 3.3.11 and the exact library versions in `scripts/common.ts`, plus nlohmann/json 3.12.0, tinyxml2 11.0.0, and [Boost.Ext SML 1.2.0](https://github.com/boost-ext/sml/releases/tag/v1.2.0). SML is installed as the single upstream header named `surface_sml.hpp` and its license in `SurfaceSml`; the flat header name supports Arduino library discovery, and no Boost distribution is required. The host partition hook emits the current boards’ ESP-IDF partition tables with MD5 checksums; uploads use the toolchain’s self-contained esptool executable directly. No separate interpreter is required. Dependencies/builds live in ignored `.deps`/`.build`; Arduino uses its board package cache. For portable tests only, use `node --run setup -- --host-only`.

| Task | Purpose |
| --- | --- |
| `node --run check` | Typecheck, Prettier check, clang-format check, host fixtures and C++ tests under address/undefined-behavior sanitizers; host warnings are errors |
| `node --run check:full` | Everything in check, then `stick-s3` and `ws-1.8` firmware builds with Arduino `--warnings more` |
| `node --run test` | Host TypeScript fixtures and sanitizer-backed portable C++ tests; no Sonos or physical devices |
| `node --run stress -- --seed 12345` | Reproducible lifecycle fault testing; optional `--steps`, three default seeds when omitted; see [runtime lifecycles](docs/runtime-lifecycles.md) |
| `node --run typecheck` | Static TypeScript checking with no emit |
| `node --run format`, `node --run format:check` | Write/check Prettier formatting for supported text formats |
| `node --run format:cpp`, `node --run format:cpp:check` | Write/check clang-format for owned C++, headers, and sketch |
| `node --run build:stick-s3`, `node --run build:ws-1.8` | Build the selected firmware |

Prettier owns all repository Markdown with `proseWrap: "never"`, so paragraphs are not hard wrapped. Formatting excludes generated, private, and vendor trees. Check tasks never rewrite files. Both firmware targets must build without warnings in owned code. Pinned M5Unified 0.2.21 and M5GFX 0.2.28 emit unused-function/unused-variable warnings in their own sources under `more`; these remain visible and do not fail the owned-code warning check. All commands live in package.json and run through `node --run`; VS Code is optional.

## Editor setup

Install the recommended [TypeScript 7 extension](https://marketplace.visualstudio.com/items?itemName=TypeScriptTeam.native-preview). Workspace settings enable `js/ts.experimental.useTsgo` and point `js/ts.tsdk.path` to `./node_modules/typescript`, so VS Code can use the same TypeScript 7 package as `node --run typecheck`. Accept **Allow** when the extension asks to use the workspace SDK, or run **TypeScript: Select TypeScript Version** and choose the workspace version. This selection is stored by VS Code, separately from repository settings. Use a current VS Code release (the extension requires 1.126 or later).

### C++ editor target

Install the recommended Microsoft C/C++ extension. Run:

```sh
node --run cpp:configure
node --run cpp:configure -- stick-s3
node --run cpp:configure -- ws-1.8
```

Each command configures one active embedded target and always includes the actual host clang++ context for every C++ translation unit in `tests/`, including the read-only probe. `stick-s3` is the default. Switching to `ws-1.8` changes only the embedded context; host tests keep their own C++17 flags and includes. Test execution and database generation consume the same typed target descriptions in `scripts/host-test-targets.ts` and compiler arguments in `scripts/host-build.ts`. Test headers inherit their including host translation unit's context; no separate tests configuration is needed.

The generator maps Arduino's copied library sources and generated sketch back to the owned files, expands GCC response files, and converts prefix-relative includes to equivalent absolute paths for Microsoft C/C++. Shared library implementations prefer the selected embedded context. Firmware and complete host-test coverage are validated before atomically replacing `.build/compile_commands.json`, the canonical path consumed by VS Code. Regenerate after adding test targets or changing toolchain pins or compiler options; quick checks do not regenerate it.

## Configuration

Discover names, display IDs, UUIDs, and state without playback changes:

```sh
node --run discover
node --run probe -- --ip SPEAKER_IP --uid RINCON_SPEAKER_ID
```

`node --run probe` uses the shared C++ adapter and permanently blocks mutations. Discovery requires multicast replies and a readable Sonos topology. If discovery fails, the controller reports an error and blocks playback. Allow local network access if macOS prompts. `node --run probe -- --ip` is a separate read-only diagnostic.

Environment profiles live in `config/`: `default.json` is the default profile and `luke.json` describes the shared household environment. Each profile's `read_only` field controls Sonos mutation permission. Either profile can configure either hardware target. Speaker addresses come from discovery.

To define another environment, copy a profile to any filename. Filenames are arbitrary labels and have no runtime semantics. Only `rooms` inside the JSON determines Sonos targeting. Profiles use the current device config shape:

```json
{
  "wifi_ssid": "${WIFI_SSID}",
  "wifi_password": "${WIFI_PASSWORD}",
  "apple_region": "52231",
  "read_only": true,
  "sleep_timeout_seconds": 300,
  "policy": { "playlist": { "shuffle": true } },
  "rooms": {
    "office": {},
    "living-room": { "playlist": { "shuffle": false } },
    "bedroom": {}
  }
}
```

Commit profiles with placeholders for credentials. Keep the repo-root `.env` local and ignored by Git; `.env.example` lists the Wi-Fi variable names. On another computer, clone the repo, create `.env`, and run the setup commands above. Keep credentials out of profiles, command-line arguments, and logs. Apple Music must already work in Sonos; `apple_region: "52231"` is a Sonos service descriptor, not a storefront country code.

The host loader reads `.env` by default; `--env-file PATH` selects another file. Process environment values override file values, including explicitly empty values. A missing default `.env` is allowed when the process environment supplies every referenced variable; an explicitly selected missing file rejects.

Environment files use one `KEY=VALUE` per line. Blank lines and lines beginning with `#` are ignored. Surrounding whitespace is trimmed; matching single or double quotes preserve whitespace inside a value. Quotes are stripped, and their contents are literal: no escape processing, inline comments, shell execution, or expansion inside the environment file. Values can contain spaces, `=`, `#`, and dollar signs.

Only `${NAME}` placeholders in JSON string values are expanded, once, on the host. Multiple placeholders in one string are allowed. Quotes and backslashes in values are JSON-escaped safely; variables cannot inject JSON fields or change field types. Missing variables, unresolved `${...}`, default expressions, and placeholders in object keys reject. The device receives ordinary JSON and knows nothing about environment files or variable names. Resolved JSON stays in memory and is never written to a file by these tools.

Host checks reject invalid/duplicate JSON, unknown or wrongly typed top-level fields, excessive nesting, and payloads exceeding 4,088 UTF-8 bytes after expansion. These checks run before serial access or flashing. Firmware remains authoritative for full device/room-policy and configuration validation.

`rooms` keys are both the allowlist and home for room-specific exceptions. Empty values allow a room with device policy and code-owned source defaults. Optional top-level `policy` uses the same source-policy schema and applies only to configured rooms; it cannot enroll rooms. Each field resolves explicit intent → room policy → device policy → code-owned source default → absent/preserve. Only current, uniquely resolved, independently eligible rooms are selectable. New discovered rooms are not enrolled; missing/ambiguous/invalid entries fail resolution. Valid configured rooms stay usable; if none resolves to an eligible target, playback is blocked. Renames require editing the display ID. See [policy](docs/policy.md) for the exact identity, defaults, and override contract.

`read_only=true` blocks all Sonos mutations at HTTP dispatch. False permits requested effects in configured/selectable rooms with identity/topology safeguards. Room configuration controls availability, not permission. Missing mode defaults true and missing rooms selects nothing. Normal firmware has no room-specific mutation build flags. Autonomous agents may mutate Sonos only in rooms listed in the `rooms` allowlist of the configuration flashed to the device under test (`config/default.json` unless another profile was flashed; read it from that device's `config-status`), from host tooling or through that device; keep changes bounded and reversible, restore the previous state, never touch other rooms, and never turn an existing `read_only=true` flag false to complete a test. Pure `preview` remains available for policy checks without any Sonos contact.

`sleep_timeout_seconds` defaults to 300; zero disables automatic sleep. Physical buttons, touch, newly presented NFC cards, and deliberate Stick writer interactions reset inactivity. Armed writer operations postpone sleep within their timeout. USB, passive writer status polling, and background Sonos/network work do not. Sleep also applies while charging; wake with the Stick's front key or Waveshare's BOOT button. See [power and button states](docs/hardware.md#inactivity-power-and-physical-buttons). Disable sleep through ordinary configuration when a long USB development session is needed.

Deployment guidance: configure Sonos's own per-room maximum-volume setting as the hard safety limit. The owner's 1.8-inch slider is experimental; the shared logical volume range remains 0–100, and a future larger kids-room UI is expected to use +/- buttons. This project does not add a software volume limiter.

## Flash and configure

Hardware target = hardware model (`stick-s3` for M5StickS3, `ws-1.8` for Waveshare ESP32-S3-Touch-AMOLED-1.8); `--config` = runtime/device profile; `--port` = physical connected unit. Profile filenames such as `config/luke.json`, `config/kids-room.json`, or `config/kids-room-2.json` are arbitrary and do not select hardware, USB devices, or Sonos rooms. The same profile can be used with either target. Build facts live in the typed registry in `scripts/hardware-targets.ts`; generated output uses `.build/stick-s3-runtime`, `.build/ws-1.8-runtime`, and `.build/ws-1.8-touch`.

Connect the intended units, close other serial monitors, and enumerate ports:

```sh
node --run ports
node --run flash:stick-s3 -- --port STICK_PORT --config config/luke.json
node --run flash:ws-1.8 -- --port WAVESHARE_PORT --config config/luke.json
```

`ports` lists serial paths and available USB metadata, including manufacturer and serial number. Choose each port explicitly; tooling does not match ports to targets or profiles. Two identical models use the same target and are flashed sequentially, for example:

```sh
node --run flash:ws-1.8 -- --port /dev/cu.usbmodem101 --config config/foo.json
node --run flash:ws-1.8 -- --port /dev/cu.usbmodem102 --config config/bar.json
```

`flash --config PATH` resolves and checks the profile first, builds the selected firmware, flashes, waits for application readiness, uploads the resolved profile, then queries and verifies config status after reboot. Any profile path is accepted, including paths outside `config/`. `--env-file PATH` works with either command:

```sh
node --run flash:stick-s3 -- --port STICK_PORT --config config/test-environment.json --env-file /path/to/secrets.env
```

Configure an already running device without flashing:

```sh
node --run configure -- --port STICK_PORT
node --run configure -- --port WAVESHARE_PORT --config config/luke.json
node --run configure -- --port STICK_PORT --config /path/to/profile.json --env-file /path/to/secrets.env
```

`node --run configure` defaults to repo-root `config/default.json`, which is read-only. The default environment-file path is also relative to the repository, independent of the working directory. Explicit relative paths resolve from the working directory.

**Flash always builds the requested current firmware before uploading and waiting for application readiness. Without `--config`, it preserves the current device configuration.** When `--config` is supplied, profile/env preflight runs before the build or any USB access, and the profile is applied and verified after readiness. To inspect the application afterward, use `node --run monitor -- TARGET --port PORT`.

Ports can change; identify the connected unit before flashing. Flash verifies hashes, uses watchdog reset at 115200 baud, then checks application READY or an idle heartbeat. Application readiness does not by itself prove working peripherals or visible pixels. A failed build/flash never proceeds to profile upload.

Config upload validates before replacement, advances the local revision, and reboots. Busy/invalid updates reject. The host verifies the new revision, read-only mode, sleep timeout, and room exceptions through a fresh `config-status` query; that response does not expose Wi-Fi credentials, so credentials are not read back for comparison. Household JSON persists in `surface/config`; preferred display ID in `surface/preferred-id`; calibration separately in `surface/touch`. Each boot clears a saved room preference outside the configured `rooms` allowlist, while retaining preferences for configured rooms that are temporarily unavailable. Upload does not erase calibration. Credentials are not printed or compiled into firmware; prototype NVS is not encrypted. `.local/` holds private logs and temporary captures.

Boot reads existing Sonos state without input. Expect `SONOS_MODE`, `device-config`, room resolution, and state logs. Playback polls nominally every ten seconds; topology events invalidate discovery. Input during worker activity rejects as busy. An interactive monitor forwards newline-terminated commands; Ctrl-C closes it.

## Local NFC writer

On an awake Stick, open **`http://STICK_IP/`** in Safari on the same LAN, or use Apple Music **Share → Make Sonos Card** with the [iOS/iPadOS Shortcut](docs/tag-writer.md#iosipados-share-sheet-shortcut) to open a populated draft. The current numeric IP appears on the Stick display and in serial `[writer] URL=...` output; the router's DHCP client list also supplies it. The Shortcut has one address constant; a DHCP reservation can keep it stable. Shared drafts are unarmed, live in RAM for up to ten minutes, and do not prevent sleep. Physical-button wake is required if the Stick is asleep. No separate server or phone app is needed. The page is available only on `stick-s3`.

Paste an Apple Music share URL, select source-valid shuffle/repeat options, and tap **Write Card**. `Default` omits the field; transport is always Play. Present a blank writable card within 60 seconds, keep it still until **WRITE OK**, then remove it. Use **Read / Edit Card** before replacing a nonempty music card; the read presentation never plays. Hidden supported fields and optional metadata survive edits. See [writer details and capacity](docs/tag-writer.md).

`read_only` gates Sonos, so intentional NFC writing works in either mode without changing it. A later normal tap follows the usual Sonos gate and selected-room policy. The writer automatically uses raw URLs for defaults, compact `ss1` for explicit modes, and structured JSON for advanced fields/metadata. NTAG213 writing/read-back and the Safari/Shortcut workflows are verified. The complete payload must fit the tag's inspected capacity; see [formats, capacity, and hardware limits](docs/tag-writer.md#capacity).

Writer assets live in `libraries/SurfaceDevice/src/writer/index.html`. After editing, run `node --run format`, `node --run writer:page`, and `node --run format:cpp`. `check` verifies that the checked-in embedded header matches the HTML source.

## Commands and diagnostics

| USB command | Effect |
| --- | --- |
| `config-status` | Print mode, sleep timeout, device policy, room exceptions, policy revision; no credentials |
| `rooms`, `status` | Refresh discovery/selected-room observations |
| `room-next`, `room-select DISPLAY_ID` | Select configured eligible room and read it; no playback effect |
| `preview URL`, `preview {v1 JSON}` | Parse, validate, resolve policy, print plan only; never submit or contact Sonos |
| `queue [start,count]` | Read a bounded selected-room page; count 1–20 |
| `play`, `pause`, `toggle`, `next`, `previous` | Submit one transport request |
| Bare Apple URL, v1 JSON | Submit the supplied intent |
| `read-only true`, `read-only false` | Persist mode and reboot; update the committed profile to match |
| `config {JSON}` | Replace full device config and reboot; prefer the profile uploader |
| `reboot` | Reboot an idle application |

Example pure policy check (catalog ID is illustrative):

```text
preview https://music.apple.com/us/album/example/12345
```

Without device/room overrides, expect album shuffle=false/repeat=off with `source-default:album`; playlists preserve shuffle and resolve repeat=off. Configured overrides show `device-policy` or `room-policy:<display-id>` origins. `policy` diagnostics carry independent `{value, origin}` entries for shuffle and repeat; null/preserve differs from false/off. Invalid sourced combinations print PREVIEW_INVALID and never create a worker job.

Advanced intent example, sent directly to submit or after `preview ` to inspect:

```json
{
  "format": "sonos-surface",
  "version": 1,
  "intent": { "volume": { "delta": -5 }, "repeat": "off" }
}
```

See [intent](docs/intent.md) for source/mode restrictions and volume/seek/queue capabilities. Accepted requests freeze target UUID and resolved policy/revision. Changing selection/config never retargets accepted work. A read-only request may succeed without a write if already satisfied; otherwise READ_ONLY_BLOCKED identifies the first required effect. Uncertain effects are never automatically retried.

M5 A single-click refreshes; A double-click cycles rooms; B toggles using a fresh read of the bound room. Normal NFC reading accepts Text/URI/empty-type URL cards, compact `ss1` Text cards, and v1 JSON Text cards without writing. One held presentation submits once; retapping requires removal. Unsupported formats report errors instead of guessing payloads.

Waveshare uses release-to-submit transport/mode/volume/seek controls and four-item queue pages. USB `ui-screen now|rooms|queue` navigates its normal screens without touch or playback. Hardware diagnostics include `touch-calibration`, `touch-calibration {JSON}`, `peripherals-retry`, `display-edge N [R]`, and `display-edge off`. See the [frontend contract](docs/waveshare-frontend.md).

Logs stream over USB only; there is no stored history after unplugging. For a bounded capture use `node --run monitor -- TARGET --port PORT --seconds 30`, redirecting output to a private `.local` log. Live laptop capture is needed for battery tests.

## Boot recovery and per-device calibration

The serial helper avoids explicit DTR/RTS writes and disables hangup-on-close through serialport; control-line changes have caused native USB resets on this Mac. Other monitors may reset on open. For an idle app use `reboot`; for a stalled interface use the bounded watchdog-reset tool without flash writes:

```sh
node --run reboot -- ws-1.8 --port BOARD_PORT
node --run reset -- ws-1.8 --port BOARD_PORT
```

If serial already reports `waiting for download`, use `reset ... --download-mode`. A ROM entry line does not prove application startup. If bounded reset cannot connect, physical power recovery may be required; see [hardware](docs/hardware.md#usb-boot-power-and-networking-limits). Do not erase NVS or repeatedly flash to diagnose a pre-application stall.

Stick should report ID26, 240×135, Grove9/10, display-ready and NFC-ready. Missing NFC retries while Wi-Fi/Sonos continue. Waveshare should identify V1 SH8601/FT3168 or V2 CO5300/CST820, 368×448, and its 329,728-byte PSRAM canvas. Peripheral failures retry; `peripherals-retry` exercises that path. QSPI allocation failure needs reboot.

Calibration is per-unit, controller-specific affine JSON. Missing/invalid data warns and uses identity; valid household config uploads do not touch it. Query:

```sh
node --run calibrate -- --port BOARD_PORT
```

To calibrate another unit, keep runtime read_only true and flash raw diagnostics:

```sh
node --run build:ws-1.8 -- --touch-diagnostic
node --run flash:ws-1.8 -- --touch-diagnostic --port BOARD_PORT
node --run monitor -- ws-1.8 --port BOARD_PORT > .local/touch-samples.log
```

Tap and release each of six white plus centers; stop capture after Done. Diagnostics stay raw and disable touch actions; USB commands still use the normal runtime gate. Fit, review the residual/JSON, upload, and verify retention:

```sh
node --run calibrate -- --samples .local/touch-samples.log --controller 0x15 --output .local/touch.json
node --run calibrate -- --port BOARD_PORT --file .local/touch.json
node --run reboot -- ws-1.8 --port BOARD_PORT
node --run calibrate -- --port BOARD_PORT
```

Use 0x38 for V1, 0x15 for V2. Fit rejects incomplete/moving/poorly spread runs; it does not write a device. Restore the normal build and verify fresh center taps. An identity reset uses the correct controller with this JSON:

```json
{
  "version": 1,
  "controller": 21,
  "x_scale": 1,
  "x_offset": 0,
  "y_scale": 1,
  "y_offset": 0
}
```

The model, bounds, and measured unit/edge limitations live in hardware. Never copy another unit's fit merely because its controller matches.

## Repository structure

- `libraries/SurfaceCore/src/`: portable intent, policy, planning/execution, AppState.
- `libraries/SurfaceSonos/src/`: SOAP/Apple metadata behind `LocalHttp`.
- `libraries/SurfaceDevice/src/`: ESP32 runtime, config parser, board adapters.
- `firmware/sonos_surface/`: shared Arduino entry point.
- `config/`: committed environment profiles; `.env.example`: local secret-file template.
- `tests/`, `scripts/`: portable/protocol fixtures and setup/device tooling.

Keep network/hardware SDKs out of portable layers and generated/private files untracked. 4.3C, voice, and broader UI work remain separate future tasks.
