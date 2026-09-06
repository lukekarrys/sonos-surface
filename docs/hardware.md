# Vertical slice evidence and experiments

## Implemented checkpoint scope

Current owner instruction (2026-09-05): real Sonos mutations are authorized only
for **Office**, after verifying its room name and stable UUID. All other rooms
remain read-only. Firmware defaults to a strict read-only HTTP action allowlist; commands
such as Play, Stop, SetPlayMode, ClearQueue, and AddSource are blocked before
network dispatch. The Mac probe uses the same allowlist. A separate explicit
build/flash flag can enable an Office playback test with its verified target
configuration; the flag itself does not enforce a room-name allowlist.
M5/Waveshare flashing and non-Sonos testing are authorized.

Both board builds share `SurfaceCore` and `SurfaceSonos`; hardware/network SDKs
are confined to `SurfaceDevice`. A serial plan encodes predecessor dependencies.
One worker processes each submitted request; input while busy is rejected rather
than queued/replayed. UI/NFC polling continues on the Arduino task. Configuration
is stored over USB in NVS; the local web editor is deferred.

Supported input is source + explicit play, play/pause, and shuffle. Source with
omitted transport and source + pause deliberately reject until preservation
behavior is validated. Repeat **requests** reject, but current repeat is read
and preserved when setting the combined Sonos play mode. Two required policies
are implemented; arbitrary policy configuration is deferred.

The current adapter uses 3-second connect and 8-second HTTP timeouts, a
60-second dispatch budget, 10-second readiness/verification windows, and
200-ms condition-poll intervals. In-flight HTTP can extend a window by its
timeout. These are experimental bounds to tune from logs, not intent semantics.
There are no diagnostic inter-command sleeps or automatic mutation retries.

Queue replacement currently uses Stop → ClearQueue → AddSource → SelectQueue →
ApplyMode → Play. Clear waits for zero queue entries; add requires positive
`NumTracksAdded` and a nonempty queue. Verification checks playing, selected
queue, preserved/requested mode, and mute. This does not prove catalog identity
if another controller replaces the queue concurrently. Source metadata follows
the reference mapping; it does not pre-validate Apple entitlement/availability,
so a rejected insert can follow a successful destructive clear.

Identity and group topology are read before mutations; grouped or unverifiable
targets reject. An ambiguous mutation result blocks subsequent mutations. Full
request deduplication, automatic recovery, event subscriptions, global ordering,
and cancellation are not implemented. Current-state refresh uses polling at
boot/reconnect, every ten seconds, and after execution. These are explicit
milestone limits, not replacements for the final planner/executor contract.

## Observed evidence: 2026-09-04

- Mac portable tests pass with address/undefined behavior sanitizers.
- Both target firmware builds pass with the pinned Arduino stack (approximately
  1.47 MB for StickS3 and 1.24 MB for Waveshare), including default read-only
  builds. The current Mac suite has 63 checks, including the mutation allowlist.
- USB identified the connected board as `StickS3_UiFlow2_` (VID:PID
  `303A:832B`) on `/dev/cu.usbmodem2101`. Ran
  `python3 scripts/device.py flash stick --port /dev/cu.usbmodem2101` with the
  read-only image. esptool 5.3.1 failed at `Connecting...` with
  `Could not configure port: (6, 'Device not configured')`. No flash write was
  reported. The USB/serial device disappeared and did not return during a
  bounded 20-second watch or subsequent enumeration. This is an observed
  connection/reset failure, not evidence of a firmware boot failure.
- No firmware was successfully flashed in this first attempt. The following
  day's download-mode recovery resolved this blocker.
- Read-only SSDP discovery reached four household Sonos speakers and fetched
  their existing playback state; all reported stopped during this observation.
- The **same C++ DirectSonos adapter** used in firmware fetched identity,
  transport, position, play mode, and media state from the Office speaker.
  SOAP reads returned HTTP 200 in approximately 16–20 ms. Group topology
  preflight also passed. No playback or queue mutation was sent.
- NFC reading, visible displays/touch, Wi-Fi on each board, source playback, and
  the firmware polling path remain **unvalidated on hardware** until checkpoints.

## Observed evidence: 2026-09-05

- After the owner entered download mode, USB enumerated as `USB JTAG/serial
  debug unit`, VID:PID `303A:1001`, on `/dev/cu.usbmodem2101`.
- The default read-only StickS3 image flashed successfully: esptool wrote
  1,466,944 application bytes and verified the hash. The default RTS reset left
  the chip in ROM download mode. The pinned esptool `--before no-reset --after
  watchdog-reset chip-id` command exited that mode; see the exact command in
  [README](../README.md#first-checkpoint-m5sticks3-nfc-read).
- Boot logs confirmed `SPI_FAST_FLASH_BOOT`, M5 board ID 26, display 240×135,
  Grove SDA 9/SCL 10, and `NFC ready: tap card`. Available PSRAM was 8,388,608
  bytes. Serial heartbeats continued with stable free heap around 270 KB.
  This verifies initialization, not visible pixels or a successful card read.
- Boot reported `SONOS_MODE=READ_ONLY` and no Wi-Fi configuration. A v1 play
  intent sent over USB reached application preflight and failed with
  `Configure sonos_uid before controlling a speaker`, without a network request.
  Opening the USB serial port also produced a fresh boot on this setup.
- The shared C++ read-only probe reverified Office identity and ungrouped
  topology. Existing playback was `PLAYING`, mode `NORMAL`; transport, track,
  settings, and media reads returned HTTP 200 in 16–20 ms (topology 21 ms).
  This proves state can be fetched without initiating playback from our code.
  No real Sonos mutation has been sent during these tests.
- Captured boot and USB-input evidence is in ignored `.local/logs/` files.

### URI compatibility and Wi-Fi checkpoint

- Added URI decoding alongside Text cards; all 83 Mac behavioral checks pass,
  and both firmware builds pass. Flashed the updated default read-only StickS3
  image and verified its hash. No card writes or real Sonos mutations were sent.
- Uploaded the owner's private Wi-Fi configuration with `scripts/configure.py`;
  the board acknowledged `CONFIG_SAVED`. On reboot it connected to Wi-Fi and
  independently fetched Office's current `PLAYING` state and track metadata.
  Repeated ten-second refreshes succeeded; SOAP reads took approximately 21–44 ms.
  Evidence: ignored `.local/logs/stick-uri-wifi.log`.
- That reboot also exposed an unresolved initialization failure: M5Unified
  reported board ID 155 (`M5StampS3Mini`), display `0x0`, and Grove SDA 2/SCL 1,
  followed by NFC initialization failure. These differ from the previously
  verified StickS3 ID 26, 240×135 display, and SDA 9/SCL 10. The generic runtime
  `display initialized` line is not evidence of success when dimensions are zero.
  Inspection of pinned M5Unified shows `fallback_board` only applies to an
  unknown ID, so it does not override this incorrect identification. Root cause
  remains unverified; do not assume the Grove cable changed.
- A subsequent USB-open reset printed only the ROM boot sequence during a
  six-second capture. Both esptool default-reset and explicit `--before usb-reset`
  attempts failed with `No serial data received`; no additional writes occurred.
  Next action requires one physical side-button reset and inspection of boot
  logs/display, then a repeat tap of the URI card once NFC is ready.

Do not infer old-speaker mutation latency or successful playback from these
read-only measurements. Record card type/capacity, board revision, operation
timings, observed outcomes, and corrections here after each physical checkpoint.
Keep credentials and household configuration in ignored `.local` files.

### Follow-up card diagnosis

- A later serial capture confirmed normal StickS3 initialization again (ID 26,
  240×135, Grove 9/10), NFC ready, Wi-Fi connected, and Office state refreshes.
  This shows recovery occurred; it does not establish the reboot failure's cause.
- The owner then presented an NTAG213 with 144 user bytes. NDEF reading returned
  success/valid in 73 ms, but the vendor decoder returned one empty record:
  `TNF=0 type= payload-bytes=0`. The prior generic Text/URI error was misleading.
  Evidence is in ignored `.local/logs/stick-record-diagnostic.log`.
- The adapter now labels this result `Empty NDEF record: no music URL` and logs
  16 bytes from NTAG2 user page 4 via read-only `read16`, to compare the on-card
  TLV/header with the decoded result. Other rejected types show TNF/type on screen.
  Actual card contents versus a decoding issue remains unresolved until that raw
  read is captured; do not rewrite the card or infer a URL from leftover bytes.
- The diagnostic StickS3 build flashed and hash verification passed. Boot again
  confirmed the correct display/Grove setup, NFC ready, and Office reads. No card
  was detected during this second capture (`.local/logs/stick-record-raw.log`),
  so the raw-page comparison still requires a physical tap.

## Source evidence and experiments still needed

### Apple Music station support

- The owner supplied a legacy empty-type record containing a 92-byte personal
  station URL (`/us/station/luke-karrys-station/ra.u-...`). Read-only Office
  favorites contained this exact station, providing authoritative household URI
  and DIDL evidence: `x-sonosapi-radio:radio%3a{stationId}?sid=204&flags=44&sn=1`,
  item prefix `000c002c`, and an audioBroadcast class. The inspected Node reference
  revision did not cover stations; source construction is based on this speaker
  evidence instead.
- The adapter uses that URI without an account-specific `sn` parameter, the
  existing configured Apple descriptor, `parentID=-1`, and audioBroadcast class.
  This combination succeeded on Office; account serial discovery was unnecessary
  for this household test. Personal stations still require account access.
- Station intents normalize through the same Text/URI/empty-type card paths.
  Their plan is SelectStation → Play, with no queue clear/add/select or SetPlayMode.
  Stations receive no album/playlist policy. Explicit station shuffle and standalone
  shuffle against a current radio source reject before mutation. Verification
  requires the requested station URI, PLAYING, and preserved mute.
- All 122 portable checks pass, including standard/legacy station payloads,
  unsupported combinations, real-adapter SOAP sequencing with a fake transport,
  and rejecting PLAYING on a different station. Both target builds pass. The
  playback-enabled StickS3 build flashed, booted, and fetched Office state.
- A USB submission of the exact personal station URL succeeded in **4,116 ms**
  (2,805 → 6,921 ms since boot). SetAVTransportURI took 104 ms; Play took 1,343 ms.
  Verification observed TRANSITIONING before PLAYING on `I Walked`. The exact
  station URI and preserved mute passed verification, and later polling remained
  PLAYING. Log: ignored `.local/logs/office-station-playback.log`.
- Before/after read-only queue snapshots both contained 25 entries with identical
  metadata SHA-256. No queue, volume, mute, or mode mutation was dispatched in the
  station test. The owner subsequently confirmed the physical station card worked.
  Other station/account variants and transitions from shuffled queues remain unvalidated.

### First Office mutation experiment

- Reverified Office UUID and ungrouped topology before the test. This target is
  a Sonos Port (S23). Read-only `GetVolume` returned 100 and `GetOutputFixed`
  returned 1; fixed line output explains the volume value. Neither setting changed.
- Flashed the explicit playback build, then submitted the card's Waxahatchee
  playlist URL over USB to the ESP32 application. Queue clear succeeded in 32 ms
  and Browse observed zero entries. AddURIToQueue failed in 482 ms with HTTP 500,
  Sonos 804. Execution stopped, reconciled `STOPPED`, and reported `partial`;
  no Play was dispatched. Log: ignored `.local/logs/office-first-playback.log`.
- The URL resolves to an Apple Music playlist; metadata matches the reference
  library. A bounded Office-only diagnostic rechecked identity, topology, and
  empty queue, then changed only the container URI's separator from literal `:`
  to `%3a`. The insertion succeeded with `NumTracksAdded=27`, `NewQueueLength=27`.
  It did not issue Play. This measures a URI-encoding requirement, not a need for
  an inter-command delay or a different account token.
- Shared album/playlist URI construction now encodes that separator. This
  correction is also supported by [SoCo's Apple Music share-link code](https://github.com/SoCo/SoCo/blob/master/soco/plugins/sharelink.py).
  Track URI syntax is unchanged and remains unvalidated. No automatic mutation
  retry was added.
- The corrected playback-enabled StickS3 build flashed and booted successfully.
  A fresh USB URL submission through the shared application completed in
  **3,698 ms** (request start 2,378 ms, success 6,076 ms after boot). Queue clear
  took 33 ms, insert 391 ms, select queue 33 ms, set mode 26 ms, and Play 566 ms.
  Browse confirmed 0 then 27 queue entries. These are single-run observations,
  not latency guarantees.
- Play's HTTP 200 preceded `PLAYING` by about two seconds: verification initially
  observed `TRANSITIONING`, then `PLAYING` with `Right Back to It (feat. MJ
  Lenderman)`. The request succeeded with mode `NORMAL`, policy provenance
  `preserve`, selected target queue, and preserved mute. Periodic state refresh
  continued afterward. No diagnostic sleep was needed; readiness polling supplied
  the necessary synchronization. Log: `.local/logs/office-corrected-playback.log`.
- All 107 host checks and both builds pass after the URI correction. Office's
  reported volume remained 100; no volume/output-mode command was issued.
  This proves direct ESP32 source playback via USB. The owner must still confirm
  audible output and repeat the physical NFC tap with this playback-enabled image.
  At that point album playback, standalone pause, and Waveshare touch were still
  hardware-unvalidated; subsequent owner evidence is recorded below.

### Owner-confirmed M5 NFC playback

- The owner confirmed the physical NFC playback test succeeded, then tested an
  additional legacy album card and switched between the album and playlist cards.
  This establishes the M5 NFC → intent → shared application → direct Office
  playback path for both source kinds without rewriting those cards.
- This is owner-reported functional evidence, not a captured timing trace for
  each tap. The additional album's URL/record encoding and shuffle transitions
  were not logged in this test; do not infer coverage of every card encoding,
  held-card suppression, or explicit-shuffle override from successful switching.
- The next milestone checkpoint is Waveshare display/touch and independent state
  fetching, followed by Office control through the same application. Standalone
  pause and the intermittent StickS3 reboot/autodetection issue still need testing.

- The owner confirmed the empty-type legacy card now displays `NFC parsed;
  submitted`, followed by Sonos commands being blocked by the read-only guard.
  This validates physical NFC input through parsing and application submission;
  it is not yet evidence of successful source playback. Office identity and
  ungrouped topology were reverified before preparing a playback-enabled build.

- The owner's NFC Tools screenshots identify another actual household encoding:
  well-known TNF `0x01`, an empty type, and 94 payload bytes starting `68 74 74 70
  73 3a 2f 2f` (raw `https://`, with no Text status/language or URI prefix byte).
  The URL is a Waxahatchee Essentials Apple Music playlist. The StickS3's newer
  `TNF=1 type=` report agrees with that representation. This is distinct from
  the earlier TNF=0/zero-payload capture; their relationship remains unknown.
  A narrow portable record dispatcher now accepts the demonstrated empty-type
  URL encoding; it rejects JSON, unsupported types/TNFs, and malformed standard
  Text/URI records. The exact screenshot URL is a regression fixture. No card
  migration, format/write operation, or broad payload guessing is introduced.
  All 106 portable checks and both target builds pass. The updated read-only
  StickS3 firmware flashed with hash verification; successful parsing of the
  physical empty-type card still requires a tap on this build.

- The owner observed a visible `need Text, not URI` error when presenting an
  existing card. This corrects the initial Text-only assumption: some household
  cards use well-known URI records. The reader now accepts `T` and `U`, expands
  full/HTTPS-prefixed URI payloads, and applies the existing Apple URL validator.
  Prefix codes follow [Nordic's URI RTD documentation](https://nrfconnectdocs.nordicsemi.com/ncs-bm/latest/nrf-bm/doxygen/html/group__nfc__uri__rec.html).
  Portable tests cover both encodings, album/playlist/track execution equivalence,
  malformed UTF-8, invalid prefixes/URLs, and expanded size limits. A successful
  read/parse of the same physical URI card still needs a repeat tap after flashing.

- [Reference routes at 6644198](https://github.com/lukekarrys/node-sonos-http-api/blob/664419878228cbd66dd36956e5c03fc68a09f587/src/device.ts)
  normalize share URLs and enqueue `apple:kind:id`. Its lockfile pins
  `@svrooij/sonos` **2.6.0-beta.11**; inspected the exact npm package's
  `lib/helpers/metadata-helper.js`. Album/playlist URI prefixes are `1004206c`
  and `1006206c`, service ID 204, with default descriptor region 52231.
  Validate actual playback/account compatibility before changing this mapping.
- [M5Unit-NFC source](https://github.com/m5stack/M5Unit-NFC/tree/93745b547364f310cd64b5155a870103a7800a5d)
  supplies NFC-A NDEF reads. Polling uses WUPA rather than REQA alone so a halted,
  held tag does not appear removed. Three missed polls release the presentation
  latch; verify this against real card positioning/RF noise. No writer functions
  are called. Other NFC protocols are deferred.
- [StickS3 documentation](https://docs.m5stack.com/en/core/StickS3) and the pinned
  M5Unified driver provide Grove power and board-aware I2C setup. Validate
  detection, power, SDA/SCL, and display orientation on the physical StickS3.
- [Waveshare reference examples](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8/tree/7ab8f957e22ea1ab811256359f4eddcaaf49ee91/examples)
  distinguish V1 SH8601/FT3168 and V2 CO5300/CST820. The adapter probes the touch
  address and selects the corresponding display, using the vendor pinmap/reset
  sequence. Its 20-ms reset pulse is an electrical hardware requirement, not
  Sonos ordering. Verify revision detection, touch coordinates, and held-touch
  suppression before relying on either variant.

### Waveshare USB bring-up and current recovery blocker

- Identified the new board at `/dev/cu.usbmodem1101`, USB serial/MAC
  `28:84:85:3B:7F:A0`, distinct from StickS3 at `/dev/cu.usbmodem2101`.
  esptool confirmed ESP32-S3 rev 0.2, 16 MB flash and 8 MB embedded PSRAM.
  Read-only firmware flashed with hash verification and booted successfully.
- Detected V2 CO5300/CST820: 368×448 display, touch address `0x15`, SDA15/SCL14.
  Added explicit driver dimensions, readiness, touch-read, and render logs, plus
  guards against polling/rendering after board initialization failure. The first
  five-byte touch register read succeeded with zero fingers. Rendering returned
  successfully; this does not prove visible pixels or physical touch mapping.
- Configuration initially received no acknowledgment. The pinned native USB
  driver's 256-byte RX buffer was smaller than the 306-byte configuration command
  containing a Source URL. Increased it to 8 KiB before Serial.begin and made the
  uploader wait for READY or an idle heartbeat before transmitting. Configuration
  then saved successfully, and the restarted board loaded the saved settings.
  Private `.local/waveshare-config.json` retains the existing Wi-Fi/Office settings
  and supplies the known playlist for Source; no credentials are committed.
- Read-only firmware joined Wi-Fi and independently fetched verified Office state:
  `PLAYING`, mode `NORMAL`, title `The Twist`. Display logging transitioned from
  stale/unknown to `playback=PLAYING stale=0`. A later ten-second refresh succeeded;
  SOAP reads took 21–32 ms in that sample. Heap remained approximately 216 KB.
  Evidence: `.local/logs/waveshare-wifi-display.log` (ignored). No input initiated
  that playback, so this proves the second board's independent shared-state path.
- Updated Waveshare read-only/playback builds and StickS3 playback build compile.
  The latest portable suite remains 122 passing checks; no core behavior changed
  during this bring-up. Python syntax and whitespace checks pass. The shared USB
  buffer change is built for StickS3 but has not been flashed there yet.
- The Waveshare playback image flashed with hash verification, but subsequent
  monitoring produced only ROM boot output (`USB_UART_CHIP_RESET`, ending at
  `entry 0x403c88b8`). No application READY/identity logs followed in the bounded
  observation. The planned USB Source → Pause → Play test required those signals
  and **sent no commands**. Log: `.local/logs/waveshare-office-playback.log`.
- Two bounded esptool recovery attempts, normal reset and `--before usb-reset`,
  both with `--after watchdog-reset --connect-attempts 2 chip-id`, failed with
  `No serial data received`. No erase or subsequent flash write was attempted.
  Root cause is unknown; successful upload alone does not establish application
  boot, and the difference between reset behavior and image behavior is unresolved.

### Waveshare recovery and direct Office playback

- The owner unplugged/reconnected Waveshare USB. The unchanged playback image
  then booted successfully, including V2 driver initialization, 8 KiB USB RX,
  Wi-Fi connection, and `SONOS_MODE=PLAYBACK_ENABLED`. Independent reads observed
  Office already playing `Fire` before any test submission. Log:
  `.local/logs/waveshare-replug-boot.log`. No reflash or configuration change was
  necessary. This establishes a recovery, not the cause of the earlier boot stall.
- A subsequent serial open also booted successfully. A bounded USB test waited
  for playback mode, verified Office UUID/room, and an idle application before
  submitting `source`, then `pause`, then `play`. Each next submission required
  success and an idle heartbeat. Firmware performed its normal group preflights.
  USB commands use the same `handle`/application path as the touch adapter;
  this exercise does not establish physical touch detection or coordinate mapping.
- Source replaced the queue with 27 tracks and reached verified PLAYING on
  `Right Back to It (feat. MJ Lenderman)` in **3,944 ms** (2,527 → 6,471).
  AddURIToQueue took 366 ms and Play 560 ms; readiness polling observed
  TRANSITIONING before PLAYING. Existing mode NORMAL and mute were preserved.
- Pause reached verified PAUSED_PLAYBACK in **626 ms** (10,005 → 10,631), with
  its SOAP command taking 65 ms. Resume reached verified PLAYING in **2,124 ms**
  (15,006 → 17,130), despite Play acknowledgment taking only 52 ms. These single
  observations reinforce waiting for state rather than treating HTTP 200 as
  playback readiness. No retries or diagnostic delays were introduced.
- Renderer logs showed pending/succeeded and PLAYING/PAUSED_PLAYBACK updates.
  The script ended with `WAVESHARE_SOURCE_PAUSE_PLAY_PASSED` and Office playing.
  No volume, mute, output configuration, or other-room mutation was issued.
  Log: `.local/logs/waveshare-office-playback.log` (replaces the earlier failed
  attempt's ROM-only capture; that failure is recorded in the preceding section).

### Waveshare touch targeting investigation

- The owner reports difficulty hitting visible buttons; swiping triggers unclear
  actions. Captured raw coordinates span the screen, including several y=447
  edge readings. Source submissions occurred at (139,342) and (16,352), and
  Pause at (189,392); the shared application completed these operations. This
  proves touch input can reach Sonos, but not that targeting is correct or usable.
  Evidence: `.local/logs/waveshare-touch-checkpoint.log`.
- Found a definite hitbox defect: the gap between drawn button columns was
  included in hit testing. Drawing and hit testing now use the same four button
  rectangles; gaps no longer dispatch actions. This alone does not explain the
  owner's full report. No coordinate scaling/mirroring has been guessed.
- The pinned vendor V2 example uses the same 368×448 display constructor, raw
  touch coordinate register layout, and periodic interrupt mode. It reads via
  interrupt notifications; our adapter polls finger count and latches the first
  coordinate until release. Coordinate alignment, stale first samples, release
  reporting, and missed short taps remain hypotheses requiring physical evidence.
- Added a separate `--touch-diagnostic` Waveshare build: the HTTP boundary stays
  read-only, all touch actions are disabled, and the display draws a yellow
  crosshair at reported coordinates. Serial logs raw bytes, finger count, event
  bits, and hit name during motion/release. It provides four numbered targets
  at (40,140), (328,140), (40,270), and (328,270). This is a diagnostic adapter
  screen; it does not change intent or Sonos semantics.
- Diagnostic build compiled, flashed with hash verification, and booted with
  `DIAGNOSTIC: coordinates only; touch actions disabled` and `SONOS_MODE=READ_ONLY`.
  The incompatible diagnostic/playback flag combination rejects before building;
  Python syntax and whitespace checks pass. Serial capture is written to ignored
  `.local/logs/waveshare-touch-diagnostic.log`. No extra Sonos mutations were sent
  during this investigation. The Waveshare now holds the diagnostic image;
  StickS3 firmware and both saved configurations were left unchanged.

Immediate checkpoint: in the diagnostic screen, tap the centers of targets
1 → 2 → 3 → 4, lifting between each, then slowly drag across the screen. Report
whether the yellow crosshair follows the finger or is offset/mirrored/stuck.
Capture raw coordinates before selecting a correction. Both boards have exercised
the same application/Sonos core; reliable Waveshare touch targeting still gates
the two-board milestone.

- Diagnostic follow-up: the owner tapped the numbered targets and dragged, but
  reports no visible yellow crosshair. The numbers intentionally do not align
  with the separate Play/Pause button labels. Serial captured touch-down,
  movement (event=2), and release (event=1, fingers=0), including a continuous
  drag spanning approximately x=46–346 and y=47–423. Thus input is arriving and
  is not permanently latched. Exact tap-to-target correspondence and actual
  drawing placement remain unconfirmed; request a screen photo before guessing
  calibration or claiming the crosshair was visibly rendered.

### Missing thin display primitives

- The owner's screen photo shows size-2 text and target numbers, but none of
  the target circles, thin button borders, or yellow crosshair. This provides
  evidence of a rendering problem independent of touch calibration. The library's
  [CO5300 issue #780](https://github.com/moononournation/Arduino_GFX/issues/780)
  reports missing single-pixel/thin primitives on the same controller. The pinned
  driver writes these as small panel address windows; larger glyph blocks render.
  This is a matching upstream symptom, not yet proof of every targeting cause.
- Waveshare now composes both normal UI and diagnostics in an RGB565 PSRAM
  canvas (368×448×2 = 329,728 bytes) and flushes the whole frame. Small primitives
  become pixels inside that full image instead of individual panel transactions.
  Canvas allocation failure stops board readiness with an explicit error.
  Diagnostic logs measure flush time and check the cursor's framebuffer color;
  these checks still cannot prove the physical pixels are visible.
- USB re-enumerated as `/dev/cu.usbmodem101`; serial `28:84:85:3B:7F:A0` confirms
  it is the same Waveshare. README commands use this port. Touch coordinates,
  stored configuration, intent semantics, and Sonos execution are unchanged.
- The canvas diagnostic compiled and flashed with hash verification. The first
  boot reported `Waveshare expander 0x20 missing`, before panel/canvas allocation.
  Closing serial and running the following software watchdog reset recovered
  initialization without physical interaction or another flash:

  ```sh
  ~/Library/Arduino15/packages/esp32/tools/esptool_py/5.3.1/esptool --chip esp32s3 --port /dev/cu.usbmodem101 --after watchdog-reset --connect-attempts 2 chip-id
  ```

  The next boot reported `adapter ready=1`, allocated the PSRAM canvas, and
  completed full-frame transfers in 44 ms. The cursor color check returned
  `yellow-in-buffer`. Firmware remained `SONOS_MODE=READ_ONLY`; no mutations
  were issued. Log: `.local/logs/waveshare-canvas-diagnostic.log`. The transient
  expander failure's cause remains unknown; do not claim the reset issue fixed.

- The owner confirmed the crosshairs, button outlines, and plus signs beside
  targets 1–4 are now visible. This validates the buffered drawing correction
  on the physical V2 panel. The capture also shows continuous movement and
  release samples, with framebuffer flushes taking 43–45 ms. Accurate visual
  alignment between finger and yellow marker has not yet been owner-confirmed.

- The owner confirms the marker follows the finger during dragging, but notes
  a possible small downward offset on taps. Recent short contacts show release
  coordinates unchanged or 1–3 pixels upward from their first reported sample;
  this does not explain a consistent downward release shift. There is no measured
  intended-contact reference sufficient to calibrate an offset. Retain raw
  coordinates and test usability with the now-visible button rectangles.
- A fresh read-only probe verified Office UUID, ungrouped topology, and STOPPED
  state before restoring playback-enabled firmware. No automatic playback test
  is needed here; the outstanding evidence is physical center-button targeting.
- The normal playback-enabled Waveshare build with buffered rendering compiled,
  flashed with hash verification, and booted with `adapter ready=1` and
  `SONOS_MODE=PLAYBACK_ENABLED`. No recovery reset was needed on this upload.
  Physical button capture: `.local/logs/waveshare-buttons-canvas.log`. The
  diagnostic image has been replaced; touches now control configured Office.

Next physical checkpoint: in the normal controller, tap the center of Play
(bottom left), wait for `PLAYING`/`succeeded`, then tap the center of Pause
(bottom right). Capture named button hits and results; report whether each
center tap consistently activates the intended button. A possible small tap
offset remains an observation, not a confirmed calibration defect.

### Repeatable center-button misses

- The owner reports center taps missing while taps near the button top work.
  Normal controller logs show successful Play/Pause requests, but other contacts
  at y=438–447 miss the bottom row's y=376–435 bounds. This supports the reported
  usability problem; intended-contact coordinates are still needed to distinguish
  scale, offset, and contact-position effects. One logged hit also occurred during
  a periodic refresh, so busy rejection can independently explain an ignored tap.
- At the owner's request, restore the existing buffered, read-only crosshair
  diagnostic for offset feedback. No guessed correction or hitbox expansion is
  applied. The same four plus targets remain; the owner can describe the marker's
  displacement relative to a deliberate tap at a known target center.

Current checkpoint: tap the plus center beside target 1, lift, and report where
the yellow marker lands relative to it. Capture `.local/logs/waveshare-offset.log`.
Sonos mutations remain disabled in this diagnostic image.

- The owner can place taps close to diagnostic targets 1–4, but continues to
  miss Play/Pause and asks whether the defect is confined to the bottom. Those
  targets cover only y=140 and y=270; the bottom button centers are y=406.
  Bottom contacts repeatedly reach y=438–447, while other contacts reach near
  the target rows. This is compatible with increasing/nonlinear error or an
  edge/contact effect, but the logs alone do not locate the physical finger.
  Use the current read-only diagnostic to drag from target 4 down through Pause
  and observe where the marker starts separating from the finger. No new flash
  or calibration change is needed for that test.
- Two subsequent downward drags progressed continuously through the reported
  Pause region and ended at (340,436) and (336,434). In both, release preserved
  the final in-contact coordinate exactly; there was no release-time jump in
  the captured data. The owner's phrase about the final marker position needs
  clarification before interpreting its visual displacement. Do not attribute
  this observation to release handling or apply a calibration from it alone.
- The owner clarified that the marker ends at the bottom of Pause, not back at
  target 4. With a reported physical endpoint at Pause's center (y=406), the
  traces suggest approximately 28–30 pixels of excess reported y near that row.
  This is an approximate owner-described reference, not enough to establish a
  transform or whether distortion is linear across the screen.
- The read-only diagnostic now presents one white plus at a time at
  (92,140), (276,140), (92,270), (276,270), (92,406), and (276,406). Each release
  logs the expected point and first/last/release raw readings, then advances.
  Movement over 16 pixels or multiple fingers retries that target. A pre-existing
  held finger at boot is ignored until release. No coefficients or config are
  changed; these samples are for review. The three rows distinguish a fixed
  offset from increasing or nonlinear error without conflating labels and targets.

Current checkpoint: tap the center of each white plus once, lifting between
targets, until the screen says Done. Capture `.local/logs/waveshare-calibration.log`
and compare expected/actual positions before implementing a correction.
The six-target build compiled, flashed with hash verification, and booted with
`adapter ready=1` and `SONOS_MODE=READ_ONLY`; the serial capture is active.

### Six-target measurements and provisional V2 correction

The completed run recorded identical first/last/release positions for all six taps:

| Target | Display x,y | Raw x,y |
| --- | --- | --- |
| 1 | 92,140 | 87,120 |
| 2 | 276,140 | 307,136 |
| 3 | 92,270 | 69,289 |
| 4 | 276,270 | 304,279 |
| 5 | 92,406 | 72,441 |
| 6 | 276,406 | 312,427 |

Independent least-squares fits for each axis give
`x = round(0.792093109 * rawX + 32.050138643)` and
`y = round(0.866487799 * rawY + 27.650440782)`. Both axes need correction in this
sample; a fixed downward offset alone cannot explain the pattern. Residuals on
the fitted samples are at most 9 pixels per axis after rounding. The bottom
samples map to (89,410) and (279,398), inside the original Play/Pause rectangles.
These are training residuals, not independent accuracy measurements.

`TouchCoordinates.h` keeps this provisional mapping in the board adapter, applied
only to V2 normal controls. V1 remains identity, diagnostics remain raw, and
out-of-range raw reports reject before mapping/hit testing. Normal logs show both
raw and mapped coordinates. No hitboxes, MusicIntent, policies, or Sonos behavior
change. Host checks reproduce the six samples, verify V1 identity, bottom-button
inclusion, and invalid-input rejection. Fresh physical center taps must validate
the fit before declaring targeting fixed.

The coefficients are measured for the owner's current unit, not a CST820 hardware
specification. They are compiled into this prototype's V2 adapter; another V2
unit requires validation before reuse. Per-device calibration storage is deferred.
The fit is measured only across the sampled area. Mapping nominal raw boundaries
insets the reachable coordinates to approximately x=32–323 and y=28–415; edge
controls and gestures are not validated and may require a broader model/data.
Do not expand the model until a real use case or new samples justify it.

Next checkpoint: restore the playback controller for verified Office, tap the
center of Pause then Play after success, and compare fresh raw/mapped hit logs
with intended buttons. Capture `.local/logs/waveshare-calibrated-buttons.log`.
All 122 core behavioral checks and the new touch mapping checks passed on Mac
with address/undefined behavior sanitizers. The corrected playback build compiled,
flashed with hash verification, and booted with V2 fit logging, `adapter ready=1`,
and `SONOS_MODE=PLAYBACK_ENABLED`. A fresh read-only probe verified Office's UUID
and ungrouped topology before flashing. No automatic mutations were sent; the
remaining check is the owner's new button taps.

### Owner-confirmed calibrated Waveshare buttons

- In response to the fresh center-button Pause/Play test, the owner confirmed
  "yeah that works". This independently validates usability of the fitted
  coordinates for those controls on this unit. The available calibrated-button
  log contains boot/state reads but no new touch/result trace for that reported
  pair; treat the acceptance as owner-reported evidence, not measured latency.
- Together with earlier captured Waveshare touch → Source/Pause execution,
  USB Source/Pause/Play verification, independent state reads on both boards,
  and owner-confirmed M5 NFC album/playlist/station playback, this establishes
  the first two-board vertical slice. Both use the shared intent/application/
  Sonos path. No production-readiness claim follows from this milestone.
- Keep the measured fit's unit/edge limitations, unresolved boot/peripheral
  failures, and deferred event/recovery features explicit. Recommended next work:
  reliable boot/peripheral initialization and per-device calibration storage,
  before adding UI features or assuming another V2 board shares this fit.
