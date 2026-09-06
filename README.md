# sonos-surface

Physical Apple Music/Sonos controls for M5StickS3 + ST25R3916 NFC and Waveshare
ESP32-S3-Touch-AMOLED-1.8. Both boards compile the same portable C++ application
and direct Sonos adapter. No Node server is required.

**Current status:** the first two-board vertical slice is demonstrated on real
hardware. M5StickS3 legacy NFC album/playlist/station playback is owner-confirmed,
including switching sources. Waveshare touch reaches the same application/Sonos
core; the owner confirmed center-button Pause/Play works after buffered rendering
and a measured coordinate correction. Both boards independently fetch existing
Office playback. All 122 Mac core checks and the touch mapping checks pass.

Both boards retain playback-enabled Office configuration. The Waveshare correction
is validated for this unit's current buttons, not all V2 units or screen edges.
Its read-only diagnostic continues to report raw coordinates. Intermittent boot
and peripheral initialization failures have recovery procedures; their causes
remain unresolved.
See [hardware findings and limitations](docs/hardware.md). The product contracts
remain in [docs/product.md](docs/product.md).

**Current testing authorization:** real Sonos mutations are permitted only in
**Office**. Verify its room name and stable UUID before playback testing; all
other rooms remain read-only. Default firmware builds block every mutating Sonos
request before HTTP dispatch, even if a card/button requests playback. Default boot logs show
`SONOS_MODE=READ_ONLY`. Read-only and playback-enabled binaries use separate
build directories so a default flash cannot select a playback build.

**Recommended next work:** make boot/peripheral initialization reliable and
store calibration per device before expanding the UI or adding another unit.
See the [Waveshare commands](#waveshare-checkpoint).

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
python3 scripts/device.py flash stick --port /dev/cu.usbmodem2101
python3 scripts/device.py monitor stick --port /dev/cu.usbmodem2101
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

If boot instead reports M5 ID 155, display `0x0`, and Grove pins 2/1, board
autodetection has failed; the expected StickS3 values are ID 26, 240×135, and
pins 9/10. This has occurred after configuration/reboot and is unresolved.
Single-click the side reset button and capture the new boot log; do not treat
the generic `display initialized` line as proof that the screen initialized.

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
... Configure sonos_uid before controlling a speaker
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

Edit `.local/config.json`: set 2.4 GHz Wi-Fi credentials, `sonos_ip`, the exact
discovered `sonos_uid`, and a known playable public Apple Music album/playlist
`source_url`. Leave `playlist_shuffle_room` empty unless enabling playlist
shuffle for that exact speaker ID. `apple_region: "52231"` matches the reference
default; it is a Sonos service identifier, not an Apple storefront country code.
Apple Music must already work in the household's Sonos app.

Close the monitor, send the file, then reopen it:

```sh
.deps/venv/bin/python scripts/configure.py --port /dev/cu.usbmodem2101 --file .local/config.json
python3 scripts/device.py monitor stick --port /dev/cu.usbmodem2101
```

Configuration persists in device NVS and triggers a reboot; credentials are not
printed or compiled into firmware. This prototype does not encrypt NVS. Do not
commit `.local` files or share their contents in logs. Reconfiguration replaces
all settings and requires no rebuild.
The uploader waits for firmware readiness before sending. The shared USB receive
buffer is 8 KiB: the previous 256-byte default dropped longer configurations,
including one with a Source URL. Expect `[usb] RX buffer=8192 bytes` on updated builds.

On boot, expect Wi-Fi IP, speaker ID, SOAP timing logs, and an observed playback
state even without presenting a card. State refreshes every ten seconds and
after commands. Play music externally and confirm the display updates. Sonos
subscriptions are deferred until initial playback works.

Playback testing is currently authorized for **Office only**. Configure its
verified UUID and IP before building/flashing a separate playback-enabled image.
The build flag enables mutations; it does not itself enforce a room-name allowlist.
The adapter verifies the configured UUID and rejects grouped targets.

```sh
python3 scripts/device.py build stick --allow-sonos-mutations
python3 scripts/device.py flash stick --allow-sonos-mutations --port BOARD_PORT
```

Default builds will report `Read-only firmware: command not sent` instead.
At a suitable existing Office speaker volume,
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
python3 scripts/device.py flash waveshare --touch-diagnostic --port /dev/cu.usbmodem101
python3 scripts/device.py monitor waveshare --port /dev/cu.usbmodem101
```

This separate `.build/waveshare-touch` image shows one target at a time, raw
coordinates, and a yellow crosshair while touching. Touch
actions are disabled and Sonos is read-only; the flag cannot combine with
`--allow-sonos-mutations`. Tap the white plus center and lift; it advances through
six targets at x=92/276 and y=140/270/406. Drags over 16 raw pixels request a retry.
Capture `[calibration]` logs with expected, first-contact, last-contact, and release
coordinates. No calibration is saved/applied; reboot to repeat the sequence.
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
python3 scripts/device.py flash waveshare --port /dev/cu.usbmodem101
.deps/venv/bin/python scripts/configure.py --port /dev/cu.usbmodem101 --file .local/waveshare-config.json
python3 scripts/device.py monitor waveshare --port /dev/cu.usbmodem101
```

These commands produce read-only firmware. For authorized Office playback:

```sh
python3 scripts/device.py build waveshare --allow-sonos-mutations
python3 scripts/device.py flash waveshare --allow-sonos-mutations --port /dev/cu.usbmodem101
python3 scripts/device.py monitor waveshare --port /dev/cu.usbmodem101
```

**Observed recovery:** after flashing the playback image, monitoring once stopped
at ROM `entry 0x403c88b8`, before application logs. Normal and USB-reset esptool
connection attempts failed. Unplugging/reconnecting Waveshare USB recovered the
same image without reflashing; application boot and Office playback then passed.
If this recurs, try that power cycle first; if battery-powered, USB removal alone
will not guarantee a cold boot.
If recovery still requires download mode, the vendor specifies holding **BOOT**
during power-on; this board has PWR/BOOT, not the StickS3's reset button.
See [vendor controls](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.8).
No flash erase was needed. The underlying reset/boot issue remains unresolved.

Expected application logs include `Waveshare V2 CO5300/CST820`,
`display=368x448 touch=0x15`, `adapter ready=1`, Wi-Fi connection, verified Office
identity, and an observed title/state. Read-only boot and recurring state reads
already passed; visible pixels and physical touches still require confirmation.
The adapter probes FT3168 at `0x38` (V1/SH8601) or CST820 at `0x15` (V2/CO5300).
Confirm the serial revision matches the physical label. The screen has Source,
Refresh, Play, and Pause buttons. Source submits the configured Apple Music URL
through the same application path as NFC. Tap once per action and report touch
coordinates, the operation log, visible state, and audible result. Begin with
Refresh after boot recovery; test Source/Play/Pause only after confirming Office
identity and `SONOS_MODE=PLAYBACK_ENABLED` in serial.

## Supported commands and structure

Supported: Apple album/playlist/track/station URLs, legacy URL → source + play, v1 JSON
source + explicit play, standalone play/pause, and explicit shuffle (including
false). Source-only/source+pause, next, volume, repeat requests, unknown intent
keys, and required extensions fail before mutation. Existing repeat is preserved
when applying shuffle. Required policies derive album shuffle=false and playlist
shuffle=true only for the configured room; explicit shuffle always wins.
Station URLs select a direct radio source and preserve the stored queue. They
receive no shuffle policy; explicit station + shuffle (even false) fails before
mutation. Station sources also require play in this slice. Personal stations
depend on access through the household's Sonos Apple Music account.

USB monitor commands (newline terminated): `status`, `source`, `play`, `pause`,
a bare Apple URL, or a full v1 JSON document. M5 button A refreshes; B pauses.
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

No generic workflow engine, queue editor, artwork, volume control, voice, or
full retry/reconciliation framework is included in this slice.
