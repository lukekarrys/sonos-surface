# Vertical slice evidence and experiments

## Implemented checkpoint scope

Current contract (2026-09-06): persisted device `read_only=true` blocks all Sonos
mutations at HTTP dispatch. With it false, the selected configured eligible room
may receive requested effects. Room/policy config uses current display IDs,
resolved to internal UUIDs. Autonomous testing keeps read-only enabled.
Earlier build-mode references below record historical images and measurements;
they are superseded by the runtime model and are not current setup instructions.

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

## Hardware hardening pass: 2026-09-05

This section supersedes earlier current-status/recovery checkpoints above; those
entries remain historical observations. No product feature or Sonos operation
was added. Both connected boards are left on **read-only** hardening firmware;
their household configuration is retained. Playback binaries remain separate.

### Stick: remove the misidentification path

**Source evidence:** pinned M5GFX 0.2.28 `init_impl()` consults its
`M5GFX/AUTODETECT` NVS hint and still calls `autodetect()`. A compiled
`M5GFX_BOARD` is also only a hint. Stick detection tests pull-ups on GPIO47/48
and reads PM1 before constructing the panel. Pinned M5Unified 0.2.21
`begin()` passes that display board through `_check_boardtype()` before using
`fallback_board`, which applies only if the result remains unknown. The
embedded-PSRAM S3 branch can select StampS3Mini (155) when its probes find no
identifying peripheral. Its pin map and absent display explain 2/1 and 0×0.
Thus an unavailable identifying peripheral can become a wrong hardware identity.
The exact electrical reason the original PM1/detection probe missed is unproven.
PM1 idle sleep/power state across reset is a plausible contributor: the pinned
M5GFX source explicitly disables PM1 idle sleep, but only after its identity read.

**Deliberate change:** `StickDisplay.h` supplies board 26 and a fixed ST7789
panel using the pinned vendor dimensions, offsets, SPI pins, LCD power sequence,
and backlight settings. Its explicit panel overload bypasses detection and its
cache. PM1 idle sleep is disabled before the other LCD power writes, whose
success is logged. M5Unified still owns power/buttons, with unused mic/speaker,
IMU/RTC and external display discovery disabled. Grove I2C is explicitly SDA9 /
SCL10 at 100 kHz with a 50 ms transaction timeout. No dependency pin changed.
A failed display or NFC initialization logs and retries every five seconds.
Unit registration is retained across NFC retries; failed NFC never gates Wi-Fi
or the Sonos worker. Display readiness includes initialization status and dimensions.

**Measured:** read-only flash/hash verification and six successive `reboot`
commands passed in one open serial session. Each reported board 26,
240×135, display-ready=1, Grove power=1, SDA9/SCL10, I2C=1, NFC ready=1,
Wi-Fi and verified Office identity/current state. Idle heap was approximately
212 KB. Logs: `.local/stick-hardening-flash.log`, `.local/stick-six-boots.log`.
This removes the avoidable runtime identity failure; six warm boots are not a
cold-boot reliability statistic. Visible pixels, physical NFC reads, and a
Grove-disconnect/reconnect test still need the short owner checklist.

### Waveshare: distinguish ROM handoff, USB reset, and peripheral readiness

**Source/image evidence:** `esptool image-info` reports **0x403c88b8 as the
second-stage bootloader entry point** in our pinned 19,968-byte bootloader image.
The historical ROM `entry` line therefore does not show that Arduino setup ran.
A missing application log also cannot distinguish a silent USB console from a
stalled bootloader/early startup. Recovery of the same flashed image by power
cycling argues against consistently broken application contents. The historical
stall's exact reset/power/early-boot cause remains unproven. Application retry
logic cannot recover a CPU that has not reached the application.

**Measured USB tooling defect:** even presetting pyserial 3.5 DTR and RTS false
caused fresh `USB_UART_CHIP_RESET` boots when opening the Waveshare port. Its
POSIX `open()` explicitly applies both states with modem-line ioctls. The shared
`serial_device.py` helper now suppresses those writes. A following monitor open
started with uptime 50,000 ms and continued through state reads, rather than
rebooting. Configuration and calibration use the same helper. This observation
is specific to this Mac/native USB setup, not a promise about every host driver.
Logs: `.local/waveshare-hardening-retry.log`, `.local/waveshare-quiet-open.log`.

**Workflow change and limits:** Arduino CLI 1.1.1 requires overriding the promoted
`upload.pattern_args` property; overriding the original tool-prefixed property
did not change its RTS reset. The corrected recipe uses watchdog reset, consistent
with [Espressif's native USB recovery guidance](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/esptool/advanced-options.html).
At upload baud 460800, one Waveshare run verified all four flash images, then
esptool 5.3.1 raised macOS `OSError: [Errno 83] Device error` in special-baud
reconfiguration during watchdog-reset teardown. A quiet monitor showed the
application was running. At 115200 the next upload/reset completed cleanly;
115200 is now the upload override. Stick also completed watchdog-reset upload.
Logs: `.local/waveshare-watchdog-flash.log`, `.local/waveshare-watchdog-boot.log`,
`.local/waveshare-115200-flash.log`, `.local/waveshare-final-hardening-flash.log`.

Flash/reset now checks READY or an idle heartbeat after the tool finishes,
including inspecting boot after a tool error. A tool error still returns failure:
an old running application cannot certify a new upload. The `[boot] application
reached reset-reason=...` marker precedes board initialization. `reboot` requires
an idle application; `reset` uses bounded esptool connection plus watchdog reset
without flash writes. `--download-mode` skips the pre-reset only when ROM download
mode was already observed. There is no automatic erase/reflash recovery.

**Application peripheral change:** the initial hardening flash reproduced
`Waveshare expander 0x20 missing`, but still reached READY and started Wi-Fi. The
following monitor open reset and recovered it, so that particular log does **not**
prove automatic retry recovery. Failed adapter initialization now restarts I2C
and repeats the vendor expander pulse and bounded touch readiness probe every
five seconds. Ten consecutive touch read failures enter the same path and require
release before another action. Display rendering remains available during initial
read failures. Initial display selection still depends on touch revision evidence:
FT3168=V1, CST820=V2; no panel revision is guessed if neither responds.
A direct peripheral-reset test caught a stale-address bug in the first retry
implementation: retaining the old address skipped readiness polling after reset,
so CST820 mode writes repeatedly failed. Clearing that readiness before each
probe fixed the test. Even during that failure, Wi-Fi and Sonos reads continued.
The successful QSPI bus and allocated panel/canvas are reused. The pinned QSPI
`begin()` aborts on an already installed bus, so calling it again is explicitly
prevented; a failed first QSPI allocation requires reboot while USB/networking
remain available. `peripherals-retry` exercises the adapter recovery path without
restarting Wi-Fi or Sonos. These are local peripheral delays, never Sonos ordering.

### Durable calibration and configuration boundaries

**Deliberate contract:** the existing Preferences/NVS namespace **`surface`** now
contains two independent strings: household JSON at **`config`**, and Waveshare
adapter JSON at **`touch`**. Household replacement/copying must not erase or copy a
physical unit's correction, so a separate key is safer than adding calibration
to the existing replacement-only household object. This reuses Preferences and
the same namespace, not a parallel persistence framework. Neither SurfaceCore nor
MusicIntent/cards/policies know about calibration.

Version 1 stores `controller`, `x_scale`, `x_offset`, `y_scale`, `y_offset`, and
`version: 1`, using the measured independent-axis affine model. The correction is
selected only for the matching controller. Missing/invalid/version-mismatched
NVS data logs and uses identity. Wrong or invalid updates preserve the old value.
Finite positive scales 0.5–1.5 and offsets ±112 are conservative supported model
bounds, not measured physical limits. Raw and mapped off-screen values reject;
no edge clamping can turn bad samples into button hits. Diagnostics stay raw.
Saving calibration requires release before the next normal control contact.

The existing `.local/logs/waveshare-calibration.log` fits with maximum training
residual **8.9623 pixels**, reproducing the documented correction. The reviewed
fit is in `.local/waveshare-touch.json`; no per-unit coefficients remain in
firmware defaults. Host tests retain the original samples as regression evidence.
Another unit uses the same read-only six-target diagnostic, then
`scripts/calibrate.py --samples LOG --controller 0x15 --output JSON` (0x38 for V1),
reviews the fit, and uploads with `--file JSON --port PORT`. The tool rejects
incomplete, moving, poorly spread and high-residual runs. Query with `--port PORT`
and verify after reboot; fresh physical taps provide independent acceptance.
Exact commands and identity-reset JSON are in the [README](../README.md#boot-recovery-and-per-device-calibration).

### Final automated validation

- All **122** portable core behavioral checks and the expanded touch calibration
  schema/fallback/round-trip tests pass with address/undefined behavior sanitizers.
  Python fitting tests reproduce the measured samples and reject incomplete or
  moving runs. Python syntax and `git diff --check` pass. SurfaceCore/SurfaceSonos
  code and intent/playback fixtures are unchanged.
- All five build variants pass: Stick read-only/playback, Waveshare read-only/
  playback/diagnostic. Diagnostic + mutation flags reject before building. Only
  read-only images were flashed in this pass; no Sonos mutation was issued.
- The measured Waveshare calibration was saved, queried, and restored through
  **six consecutive software reboots**, each with a successful independent Office
  state read. A `version:99` update was rejected and the prior JSON remained exact.
  NVS survived subsequent reflashing of the recovery correction. Logs:
  `.local/waveshare-six-boots.log`, `.local/waveshare-final-hardening-flash.log`.
- The corrected Waveshare `peripherals-retry` passed **three** successive resets
  with calibration reload, framebuffer rendering and continued Wi-Fi/Office reads,
  without CPU reboot. Idle heap stayed approximately 215 KB. This exercises the
  retry branch; it does not identify the electrical cause of an absent expander.
  Log: `.local/waveshare-peripheral-recovery.log`.
- Two quiet serial opens on each board preserved monotonic uptime: Stick
  210064 → 215074 ms; Waveshare 75000 → 80000 ms. Neither logged a new ROM or
  application boot. Log: `.local/quiet-reopens.log`.
- Stick's bounded esptool `reset` command also passed, with correct board/display/
  Grove/NFC initialization and READY. Log: `.local/stick-reset-tool.log`.

### Physical validation status and manual recovery

- September 6 follow-ups below confirm visible screens on both boards, Stick
  NFC reads/removal detection, boot without Grove with ongoing networking, and
  automatic NFC recovery after reconnecting Grove. The intermittent NFC
  preparation failure remains unresolved; phase logging is installed.
- Waveshare calibration survived the owner's power-button restart and a fresh
  Refresh tap hit correctly. Earlier Play/Pause hits and the visible boundary
  are recorded below; all three controls were not retested after this particular
  restart. Calibration edges remain unvalidated.
- Both boards resumed visible operation and state reads after owner-performed
  power-button sequences; Stick also read its NFC card afterward. Where
  convenient, boot without the configured AP and restore it to verify
  reconnection/state refresh. The September 6 outage test below verifies
  automatic reconnection after loss of an established connection on both boards;
  boot with the AP already absent has not been physically exercised.
- If a port shows only ROM output and bounded `reset` cannot connect, use physical
  recovery: Stick side reset; Waveshare full power cycle, including battery power
  if present. Use BOOT during Waveshare power-on only if download mode is needed.
  Preserve NVS; no erase was needed in this pass. A QSPI allocation failure requires
  a deliberate reboot. None of these limitations should be presented as fixed by
  application retries.


### Physical follow-up: screen visibility and calibrated hits

- The owner completed Play → Pause → Refresh center taps and reported that the
  screens look right, except clipped bottom button corners and the top title
  line on Waveshare. This confirms visible rendering while identifying a remaining
  layout defect; it does not measure the panel's exact visible corner radius.
- The read-only capture matched all requested inputs: Play raw (97,447) → mapped
  (109,415); Pause (306,447) → (274,415); Refresh (295,363) → (266,342).
  Play was blocked at the HTTP boundary. Pause found the speaker already paused
  and satisfied the request without a mutation. Refresh fetched existing state.
  Log: `.local/waveshare-physical-touch-check.log`.
- The controller layout now trials a 32px outer inset: title at (32,32), buttons
  140×60 at x=32/196 and y=284/356. Drawing and hit testing share those dimensions.
  The notice moves below the title. This is a conservative layout correction,
  not a new calibration or a universal panel specification. Saved calibration
  and raw six-target diagnostic positions remain unchanged. Physical visibility
  and fresh center taps of the moved controls require owner confirmation.
- The inset read-only image compiled, flashed with verification, and reached
  application readiness with an independent Office state fetch. All 122 core
  checks and calibration tests still pass. Capture for the next physical check:
  `.local/waveshare-inset-touch-check.log`; build/flash logs use
  `.local/waveshare-inset-{build,flash}.log`.

- The owner confirmed the 32px-inset title and buttons are fully visible, with
  a photo showing spare horizontal space and the second notice line wrapping
  back to x=0. This is text-layout behavior, not evidence for a different touch
  transform. The owner also reports completing the moved-button taps; the
  follow-up capture ended with USB `Device not configured` and contains no
  corresponding Play/Pause/Refresh sequence, so
  those taps are owner-reported rather than serial-verified.
- Added temporary `display-edge N` (0..64 pixels) / `display-edge off` USB
  diagnostics to measure visible panel bounds. A one-pixel white rectangular
  outline is drawn in the full-frame PSRAM canvas, with its inset labeled at
  screen center. It holds until another command; touch actions are suppressed
  while visible. Nothing is persisted or applied to calibration. Start at 32px
  and move outward only after owner feedback, distinguishing straight edges
  from corner clipping. The normal controller returns with `display-edge off`.
- The outline read-only build compiled and flashed with verification; all core
  and calibration tests pass. Waveshare re-enumerated to `/dev/cu.usbmodem1101`;
  USB serial `28:84:85:3B:7F:A0` confirms the same board. This explains the old-port
  capture failure without identifying why USB disconnected. Initial outline
  command is `display-edge 32`; capture: `.local/waveshare-edge-check.log`.
- The owner confirmed the complete 32px outline, including all four corners,
  is visible. The next held outline is 16px inward; its visibility is pending
  owner feedback. No calibration or persisted layout values changed.

- At 16px inset the owner reports approximately 1–2 pixels clipped at the square
  corners, with spare space along all four straight edges. This establishes that
  one rectangular inset wastes usable space; it does not yet measure a radius.
- Extended the temporary outline command to `display-edge INSET [RADIUS]`:
  inset 0..64px, radius 0..120px (omitted means square corners). Both values are
  labeled, held for inspection, and never persisted. The next trial keeps the
  16px inset and rounds corners with radius 8px, isolating curvature from edge
  position. A uniform circular corner remains a model to test, not an assumed
  exact description of the physical mask.
- The rounded-outline read-only build compiled and flashed with verification,
  then reached an idle Wi-Fi heartbeat. Trial command: `display-edge 16 8`.
  Build/flash evidence: `.local/waveshare-rounded-edge-{build,flash}.log`.
- The owner confirmed all four rounded corners at inset 16px / radius 8px
  are fully visible. Next trial: inset 8px / radius 8px, changing only the
  edge position and checking straight-edge visibility separately from clipping
  at the corners.
- At inset 8px / radius 8px, the owner reports that all straight edges remain
  visible but the corners are clipped. Keep the measured straight-edge inset
  fixed for the next trial and increase only the corner radius to 32px:
  `display-edge 8 32`.

- At the owner's request, the edge diagnostic now fills the entire addressable
  panel dark blue with the same white outline. This makes the visible panel mask
  distinguishable from unused black background. The comparison remains at inset
  8px / radius 32px; no new estimate of corner clipping has been supplied yet.
- With the blue background at inset 8px / radius 32px, the owner sees a few
  pixels of blue beyond all four straight edges. Corners may be clipped by
  approximately 1px or may only look thinner from circle rasterization; that
  distinction is unresolved. Next trial changes only inset to 4px, retaining
  radius 32px, to locate the straight-edge limit before refining corner radius.
- At inset 4px / radius 32px, the owner reports that the straight edges look
  aligned with the visible boundary, with no blue outside; more of the corners
  are clipped. Treat 4px as an approximate owner-observed straight-edge inset,
  not a pixel-exact panel specification. The next trial holds inset 4px and
  increases radius to 48px: `display-edge 4 48`.
- At inset 4px / radius 48px, the owner reports that the outline looks aligned
  and supplied a photo showing an apparently continuous border around all four
  corners. Camera glow/blur prevents a pixel-exact boundary judgment. At the
  owner's suggestion, begin a ±2px comparison: first inset 2px / radius 48px,
  held for inspection. The 4px / 48px setting remains the current best observed
  fit until these comparisons provide better evidence.
- At inset 2px / radius 48px, the owner reports corner clipping. Straight-edge
  visibility was not separately reported for that trial. The next comparison
  uses inset 6px / radius 48px, on the inward side of the 4px candidate fit.
- At inset 6px / radius 48px, the owner reports a very slight blue margin,
  estimated at 1–2px, and supplied a photo showing a continuous outline. Together
  with clipping at 2px and apparent alignment at 4px, this brackets the visible
  boundary near inset 4px and supports 6px as a small inward safety margin for
  this rounded outline. These remain visual estimates, not exact mask geometry.
- Next corner comparison returns to the 4px candidate inset and reduces radius
  from 48px to 46px. This tests a slightly squarer corner against the previously
  observed 4px / 48px fit: `display-edge 4 46`.
- At inset 4px / radius 46px, the owner reports slightly thinner corners,
  estimating clipping at roughly half the line width. This favors the previously
  observed 48px radius over 46px for the boundary fit. Final comparison:
  inset 4px / radius 50px, checking whether the larger radius clears the corners
  with a small margin. Subpixel estimates describe perceived line thickness,
  not measured fractional-pixel panel geometry.
- At inset 4px / radius 50px, the owner reports no blue gap and apparently
  uniform corners. This is the best observed visible-boundary fit from this
  sequence for this physical unit. It is an approximate visual estimate, not
  a universal Waveshare specification or a reason to change touch calibration.
  The one-pixel line and photos do not establish subpixel mask geometry.
- Boundary diagnostic is complete. Restore the existing, owner-confirmed inset
  controller with `display-edge off`; its rectangular content still needs space
  inside the curved boundary. No layout expansion or calibration update is made
  from the boundary estimate during this validation step. Continue the pending
  NFC presentation and peripheral/cold-boot/reconnection checks.

### Stick physical follow-up: 2026-09-06

- The owner reports that the Stick screen stays readable, with some light
  flickering. Whether the flicker happens only during text changes or also on
  a static screen is not yet established. The existing renderer clears the
  panel before drawing changed content directly; this could explain an update
  flash, but has not been established as the cause of the reported flicker.
- The bounded `.local/stick-nfc-presentation-check.log` contains ongoing state
  reads/heartbeats but no NFC detections or reboot markers. It does not verify
  the requested hold/remove/retap sequence. Keep that check pending rather than
  inferring success from screen readability.
- The owner clarified that the boards sat overnight and that flicker occurs
  only when text changes, probably during a redraw. This is consistent with
  the direct clear-and-redraw renderer; it is not an observed idle flicker or
  boot failure. Start a fresh 30-minute capture without rebooting the Stick:
  `.local/stick-nfc-morning-check.log`. The earlier bounded capture cannot
  establish what happened after it ended.
- Fresh quiet serial capture begins at uptime 43,772,253ms (about 12h09m), with
  Wi-Fi connected, worker idle, and successful Office state reads (HTTP 200,
  22–33ms in the first sample). Current status retains a previous read-only
  rejection; it does not reconstruct the missed card presentations. This
  confirms the Stick is responsive after the overnight interval without a
  capture-induced reboot. Fresh hold/remove/retap evidence remains pending.


### Intermittent NFC preparation failure

- Fresh morning capture confirms three presentations of the same NTAG213
  playlist card: first and third read/decoded successfully (83ms and 82ms for
  NDEF validation/read), while the second returned the combined
  `NFC identify/reactivate failed` error. All three were followed by removal
  events. Only the two successful reads submitted intents; both reached the
  read-only HTTP guard, which blocked Stop. No Sonos mutation was issued.
  Evidence: `.local/stick-nfc-morning-check.log`.
- Pinned `NFCLayerA::identify()` calls its own reactivation/probes and then
  deactivates the card before returning. The adapter's subsequent `reactivate()`
  is therefore required. The combined short-circuit error cannot establish
  which stage failed, and a successful retap establishes recovery, not cause.
- Split preparation logs into timed `phase=identify` and `phase=reactivate`
  results and distinct screen errors. Preserve the existing once-per-presentation
  latch and remove/retap recovery while capturing which phase fails. No guessed
  RF delay, automatic retry, card write, or Sonos behavior change is introduced.
  A fresh physical sequence is required to narrow the failure further.
- The phase-logging read-only image compiled and flashed with verification,
  then resumed successful state reads and idle heartbeats. Fresh physical
  capture: `.local/stick-nfc-phase-check.log` (30-minute window). Build/flash
  logs: `.local/stick-nfc-phase-{build,flash}.log`.
- The owner completed the next sequence. The phase capture records six
  successful presentations of the same card, each followed by a removal event:
  identification 50–52ms, reactivation 27ms, and valid NDEF reads 83–85ms.
  Each presentation submitted one intent; all six were blocked at Stop by the
  read-only guard, with no Sonos mutation. Exact three-second hold durations
  were not established. The intermittent preparation failure did not recur in
  this sequence; the additional logging is instrumentation, not a confirmed
  fix. Leave it in place to distinguish phases if the error returns.

### Stick boot without NFC: 2026-09-06

- With the owner confirming the Grove NFC cable unplugged and USB connected,
  issued the USB `reboot` command. The application reached startup with board
  ID 26, display 240x135 / ready=1, Grove power=1, SDA=9 / SCL=10, I2C=1,
  and NFC ready=0. Serial explicitly reported the missing NFC cable and retry.
  Evidence: `.local/stick-no-nfc-reboot.log`.
- The disconnected NFC initialization retried every five seconds without
  blocking Wi-Fi or Office state reads. At uptime 22 seconds all four state
  SOAP calls returned HTTP 200 (21–33ms), reporting paused playback; idle
  heartbeats showed Wi-Fi connected and heap 212028. No Sonos mutations were
  sent. Evidence: `.local/stick-no-nfc-check.log`.
- The owner confirmed the screen remains readable with NFC disconnected.
  After reconnecting the Grove cable without rebooting, NFC initialization
  recovered at uptime 90.674 seconds. The next card presentation identified
  successfully in 51ms, reactivated in 27ms, and read valid NDEF in 83ms.
  Exactly one intent was submitted, Stop was blocked by the read-only guard,
  and card removal was detected. Office state reads continued before and after
  recovery with monotonic uptime. This verifies recovery from NFC absent at
  boot for this physical test; it does not resolve the separate intermittent
  card preparation failure.

### Waveshare battery-backed power-cycle check: 2026-09-06

- USB removal left the owner's board running on its battery. The owner held
  both buttons until the screen went dark, then reconnected USB; normal startup
  has not yet been confirmed. USB enumerated at `/dev/cu.usbmodem1101`, but a
  quiet 12-second capture returned no serial output
  (`.local/waveshare-power-check.log`). This alone does not establish download
  mode or a complete power-off.
- The [vendor button FAQ](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8)
  specifies a PWR click to turn on and a six-second PWR hold to turn off.
  For recovery, hold PWR alone for six seconds, release, then click it again.
  Leave BOOT released during normal startup: holding BOOT at power-on selects
  download mode.
- The owner subsequently confirmed the screen was back on. A read-only USB
  calibration query returned the exact saved controller-21/version-1 fit,
  including all four coefficients, without rewriting NVS. At uptime 52 seconds
  the application verified Office identity and all four state SOAP calls
  returned HTTP 200 (22–32ms), reporting paused playback. Idle heartbeat heap
  was 215096 with Wi-Fi connected. Evidence:
  `.local/waveshare-power-calibration.log` and
  `.local/waveshare-after-power-check.log`.
- This verifies application recovery and calibration retention after the
  owner's battery-backed power-button sequence without reflash, erase, or
  software reset from the host. The transition itself was not captured, so it
  does not establish the earlier dark-screen cause or exact reset reason.
- The owner's subsequent Refresh tap logged raw (274,347), mapped (249,328),
  hit=Refresh. It triggered a fresh Office identity/state read at uptime
  98.454 seconds; all four state SOAP calls returned HTTP 200 (31–52ms).
  This confirms the saved calibration and Refresh control work after this
  restart. No Sonos mutation was sent.

### Stick power-button restart check: 2026-09-06

- The owner completed the requested USB-disconnected off/on sequence with NFC
  attached and confirmed the normal screen returned. The previous serial
  capture ended with a ROM reset banner and USB `Device not configured` error
  during the transition. Reopened the enumerated Stick port without issuing
  a software reset, reflash, or erase.
- At uptime 32 seconds, the new capture verified Office identity and all four
  state SOAP calls returned HTTP 200 (32–34ms), reporting paused playback.
  An idle heartbeat showed Wi-Fi connected and heap 212124. Evidence:
  `.local/stick-after-power-check.log`. This confirms application/network
  recovery alongside the owner-confirmed visible screen. The final boot's
  board/display/Grove initialization banner was not captured, so do not infer
  new numerical board-ID or pin measurements from this capture.
- The owner's subsequent card presentation passed identification (50ms),
  reactivation (27ms), and valid NDEF reading (83ms). It submitted one intent,
  Stop was blocked by the read-only guard, and removal was detected. Office
  state reads continued afterward. This completes the NFC functional check
  after the requested power-button restart, with no Sonos mutation.

### Wi-Fi outage and recovery: 2026-09-06

- Captured both USB ports in detached local processes while the owner disabled
  and restored the configured Wi-Fi. Stick logged disconnected at uptime
  469506ms and connected at 760386ms; Waveshare logged disconnected at 733764ms
  and connected at 1024425ms. Both spent approximately 291 seconds disconnected,
  maintained idle heartbeats, and retried Wi-Fi every 30 seconds. Neither
  recording contains an application reboot. Evidence:
  `.local/stick-wifi-outage.log` and `.local/waveshare-wifi-outage.log`.
- Both automatically resumed complete Office identity/state reads: first full
  snapshot 1179ms after Stick's connected marker, 809ms after Waveshare's.
  These intervals measure from device association, not the unrecorded time the
  owner restored the AP. Subsequent reads continued; Stick recovered from an
  additional transient GetMediaInfo failure as well. No Sonos mutation was sent.
- The outage exposed a separate stale-message defect: `Application::refresh()`
  overwrote command `detail` on a read failure and retained that message after
  successful reads. Added an observation-only `AppState.refreshError`, cleared
  by successful refresh/verification/reconciliation, while preserving command
  status/detail and the uncertainty recovery guard. Both adapters display the
  current refresh error when present and return to command detail on recovery;
  serial logs distinguish the two. No command retry or replay was added.
- Host regression checks cover repeated failures and recovery with idle,
  successful, rejected, and uncertain command outcomes, retaining the previous
  snapshot on partial read failure and never replaying mutations. All 126 core
  checks plus touch/calibration checks pass. Initial boot without the AP remains a
  separate untested physical condition.
- All five current-source build variants passed: Stick/Waveshare read-only,
  Stick/Waveshare playback-enabled, and Waveshare touch diagnostic. Flashed only
  the two read-only images with hash verification and successful watchdog reset.
  Both resumed verified Office state reads with empty `refresh-error`; Waveshare
  logged a fresh-state render. Its calibration query returned the exact saved
  fit after the final flash. Evidence: `.local/wifi-recovery-tests.log`,
  `.local/wifi-recovery-*-build.log`, `.local/wifi-recovery-*-flash.log`, and
  `.local/wifi-recovery-calibration.log`.
- The recovery-message correction was tested with host failure/recovery fixtures;
  the physical AP outage was not repeated after this correction. Network retry,
  NFC/touch mapping, and Sonos operation ordering were unchanged by this follow-up.
  Stopped both detached outage captures and their temporary idle-sleep prevention
  after preserving the evidence.

## Stick room/policy milestone: 2026-09-06

This section supersedes the initial slice's capability limits above. It does not
retroactively change the earlier hardware observations.

### Earlier implementation contracts (superseded by runtime room model below)

- Live SSDP/bootstrap discovery feeds a bounded independent-player list from
  ZoneGroupTopology. UUID is identity; names and addresses are live observations.
  Grouped, bonded, invisible, and unverifiable targets are unavailable. The
  controller never substitutes a coordinator or changes grouping.
- Stick A double-click cycles current eligible UUIDs; A single-click refreshes;
  B pauses. USB `room-next` uses the same selection path. Selection only reads
  Sonos and saves a preferred UUID in `surface/preferred-room`. Boot restores it
  when eligible, otherwise reports a deterministic fallback. Invalidation after
  boot retains the selected UUID and blocks new effects until explicit selection
  or eligibility recovery. Accepted work freezes target and policy before worker
  execution; busy inputs are rejected. Per-target uncertainty survives cycling.
- `playlist_shuffle_room` remains in USB/NVS runtime configuration. The owner's
  Living Room choice was saved by stable UUID without compiling policy into the
  image. Album shuffle=false remains household-wide; explicit values win.
- Shared intent/planning/Sonos support volume set/delta, repeat off/all/one,
  shuffle, play/pause/next/previous, and source transport preservation. Relative
  volume freezes a fresh absolute target; native skips are never automatically
  retried. The existing album/playlist/track/station NFC decoding paths remain.
- This historical image used a compiled mutation guard. The runtime device mode
  and configured room resolution below replace that implementation.
- Topology subscriptions on port 1401 invalidate the list, with full refreshes
  at boot/reconnect, notification receipt, and ten-second polling. The publisher's
  granted lease bounds renewal. State reads remain polling-based. Group/identity
  rechecks reduce but cannot atomically prevent races with external controllers.

### Measured device evidence

- Portable tests pass 212 behavioral checks under address/undefined behavior
  sanitizers, plus the existing touch and calibration suites. Fixtures cover
  identity/name/IP changes, grouping/unavailability/reappearance, preference and
  fallback, policy target binding, explicit false, mode omission, source transport
  preservation, relative volume freezing, and uncertain native skip dispatch.
- Stick and Waveshare read-only builds passed; the explicitly Office-bound Stick
  playback variant also compiled. Only the Stick read-only image was flashed.
  No Waveshare UI or physical hardware work was performed for this milestone.
- Stick flash/hash verification and watchdog application readiness passed. With
  `sonos_ip` empty, the device logged **SSDP discovered players=4** at uptime
  3457ms, a topology subscription HTTP 200 at 3638ms, and selected Office by UUID
  at 3641ms. A topology event triggered a new snapshot at 3886ms. This establishes
  bootstrap discovery without a configured IP or room list, plus initial event
  delivery. Renewal under an actual topology change/outage remains unmeasured.
  Evidence: `.local/stick-milestone-final-flash.log`.
- USB cycling read Office → Living Room → Bedroom → Ellie's Room → Office.
  Office retained PAUSED_PLAYBACK, NORMAL, track 20, and the same title and volume
  before/after. Living Room, Bedroom, and Ellie's Room returned their own stopped
  state and volumes. The empty titles matched their zero track observations.
  This establishes device selection/state isolation via USB, not physical
  double-click ergonomics or audible playback in another room.
- The identical album input derived false in Office and Living Room. The same
  known playlist URL preserved shuffle in Office and selected
  `playlist-room-shuffle` in Living Room. Explicit false selected explicit
  provenance in both. Next and combined mode/relative-volume requests in Living
  Room reached the read-only guard. No mutating Sonos HTTP action was dispatched.
  Evidence: `.local/stick-milestone-final-check.log`.
- The first diagnostic pass used an empty configured Source URL for playlist
  fixtures; those cases were invalid and were repeated using the known legacy
  playlist URL. It also exposed truncation of long USB policy lines with a zero
  TX timeout. Added a bounded 20ms TX timeout for diagnostic backpressure; this
  does not introduce Sonos operation ordering sleeps.

### Pending physical evidence

Fresh physical NFC presentations on the final image are now verified below.
Exact hold durations and visible double-click/display behavior still need owner
confirmation. Earlier audible NFC playback evidence remains historical. Native skip behavior,
volume/repeat changes, source + pause, and source-only transport preservation
have protocol-fixture coverage but have not been mutated on real Sonos in this
milestone. Office's observed volume was 100; choose a comfortable level before
any audible playback test. Use the current runtime mode and room configuration below.
Real grouping changes, IP/room renaming, subscription renewal/outage recovery,
and fallback with an unavailable preferred room remain physical follow-ups.
See the [manual checkpoint](stick-milestone-test.md).

- The final read-only diagnostic image flashed with verified hash and reached
  an idle heartbeat. The host successfully parsed both complete accepted-intent
  JSON lines: Living Room playlist input absent → resolved true / room policy,
  and explicit false → resolved false / explicit. The HTTP guard blocked queue
  clearing in both cases. Cycling then returned to Office. This verifies the
  bounded USB diagnostic correction for the tested payloads, not every maximum
  size payload. Evidence: `.local/stick-milestone-diagnostics-{flash,check}.log`.


### Physical album/playlist policy matrix

- The owner supplied four accepted-request/guard pairs, independently matched in
  `.local/stick-milestone-physical-check-2.log`: Office playlist at 5177847ms,
  Office album at 5194521ms, Living Room playlist at 5204983ms, and Living Room
  album at 5209825ms. The same physical legacy NTAG213 playlist card was used in
  both rooms, and likewise the same album card. Identification/reactivation and
  valid NDEF reads succeeded for each, with removal events after the presentations.
- Office playlist: omitted shuffle remained absent/preserved. Living Room
  playlist: omitted shuffle resolved true from `playlist-room-shuffle`. Album
  shuffle resolved false from `albums-in-order` in both rooms. Each request
  retained its selected stable UUID, and volume/repeat remained preserved fields.
  This proves the physical NFC-to-runtime-policy path on the final image.
- Office blocked Stop; Living Room was already stopped, so its Stop operation
  needed no write and the first blocked effect was RemoveAllTracksFromQueue.
  Neither room received a mutating HTTP request from these tests. These are
  policy-resolution results, not observed changes to Sonos shuffle or evidence
  that the whole playback plan completed.
- An earlier playlist presentation at 5172802ms coincided with an active state
  refresh and has no accepted-request line, consistent with the documented busy
  rejection contract. Its later removal/retap produced the accepted Office
  request above. Exact hold timing, explicit-shuffle card input, and audible
  preservation/control behavior remain separate physical checks.

### Earlier multiple playlist-room rules (wire identity superseded below)

- At the time of this measurement, the canonical runtime setting was
  `playlist_shuffle_rooms`, a map of stable Sonos UUIDs to boolean defaults.
  Multiple rooms may independently derive true or false; omitted UUIDs preserve
  shuffle. Explicit input and the household album default retain precedence.
- The legacy singular string remains readable from existing NVS/imports as one
  UUID mapped to true (or an empty map for the empty string). Supplying both keys
  rejects. Values must be booleans and keys must be stable RINCON identities;
  malformed/oversized maps reject without replacing the saved configuration.
- All 247 portable behavioral checks and the existing touch/calibration checks
  pass. Added multi-room true/false/preserve, explicit override, frozen resolution,
  legacy conversion, and atomic invalid-config fixtures. Stick and Waveshare
  read-only builds passed. Build evidence:
  `.local/stick-policy-map-build.log`, `.local/waveshare-policy-map-build.log`.
- Flashed the updated Stick read-only image with hash verification and application
  readiness. Before replacing NVS config, the old singular setting still derived
  playlist shuffle=true in Living Room. Uploaded the plural map retaining only
  that existing rule; after the configuration reboot, the same URL again resolved
  true with room-policy provenance. Both requests were blocked before queue
  clearing. No Sonos mutations were dispatched and no additional room's policy
  was enabled. Evidence: `.local/stick-policy-map-flash.log`,
  `.local/stick-policy-map-legacy-check.log`, `.local/stick-policy-map-check.log`.
  Multiple simultaneous entries are covered by host fixtures; only the owner's
  existing Living Room rule is configured on the physical device.

## Runtime room/configuration model: 2026-09-06

### Deliberate contracts

- Replaced normal build-mode authorization with persisted `read_only` (default
  true) and an independent `rooms` display-ID list (default empty). The final
  `GuardedHttp::request` boundary blocks every non-read action before dispatch
  while read-only, including unknown future actions. Normal builds have no
  room-specific compile flags. A private/protected dispatch implementation avoids
  bypass through the concrete ESP transport API; the destination UUID is still
  independently verified for permitted writes.
- Both allowlist and playlist policy resolve current display IDs against the
  entire discovered snapshot. Current names, canonical IDs, UUIDs, eligibility,
  filtered selection, resolution warnings, and accepted policy/plans are logged.
  Missing/invalid/colliding IDs never bind or fall back to historical UUIDs.
  Valid rooms keep working; unavailable entries stay configured and reappear.
- Selection sorts by case-insensitive name then UUID; A double-click and USB
  `room-next` share the same path. Preferences now persist as display IDs in
  `surface/preferred-id`. Accepted work retains its UUID and resolved policy even
  when later selection, topology, or config changes. Config replacement increments
  a local NVS revision and reboots; busy updates reject. `read-only true/false`
  persists mode without a rebuild. Only shuffle provenance is modeled.

### Measured software and Stick evidence

- **324 portable behavioral checks** pass with address/undefined sanitizers,
  plus touch/calibration suites. Coverage includes canonical IDs and invalid names,
  collisions even with an ineligible duplicate, filtered cycling, group/missing
  recovery, rename and replacement-UUID binding, malformed config warnings,
  resolved policy maps, explicit precedence, and the actual shared dispatch gate.
  All existing NFC parsing and shared Sonos protocol checks pass. The station
  fixture now decodes XML ampersands in received URI arguments; no station
  transport behavior was changed. Evidence: `.local/room-model-tests.log`.
- Both normal runtime board builds passed (Stick 1,548,571 bytes; Waveshare
  1,334,835 bytes). No Waveshare flash, calibration, or UI work was performed.
  Evidence: `.local/room-model-{stick,waveshare}-build.log`.
- First Stick flash/hash verification and application readiness passed. Its
  pre-migration NVS had no room list and an old UUID playlist key: the new image
  stayed `READ_ONLY`, selected nothing, and logged `ROOM CONFIG ERROR: set rooms`
  plus `ROOM ID INVALID` for that key. This directly verifies safe legacy behavior,
  rather than silently converting a UUID preference or discovering selectable rooms.
  Evidence: `.local/room-model-stick-flash.log`.
- The owner chose **Office, Living Room, Bedroom**. Kitchen is awaiting replacement
  hardware; Ellie's Room is deliberately excluded. Saved private/device config
  with `rooms: ["office", "living-room", "bedroom"]`, `read_only: true`, and the
  existing `playlist_shuffle_rooms: {"living-room": true}`. Normal config contains
  no Sonos UUID or IP. A private pre-migration backup is retained. Evidence:
  `.local/room-model-configure.log` and `device-config` in the check log.
- Full topology discovered `bedroom`, `ellies-room`, `living-room`, and `office`,
  with corresponding internal UUIDs. USB cycling completed two rounds of
  **Bedroom → Living Room → Office → Bedroom**; only those three ever became
  selectable. Each selected room returned its independent playback/volume state.
  Cycling did not dispatch any mutating action.
- The USB matrix accepted 15 inputs across the three rooms: album, playlist,
  explicit-false playlist, relative volume + repeat, and Next in each. Every input
  logged its display ID, frozen UUID, policy revision 2, and static operation plan;
  all 15 hit the read-only HTTP boundary. Albums derived false throughout. Playlist
  shuffle derived true only in Living Room; Bedroom/Office preserved. Explicit
  false won in all rooms. No mutating SOAP dispatch appeared in the capture.
  Evidence: `.local/room-model-check.log`; the local checker also asserted the
  allowlist/mode before sending any intents and audited all HTTP action logs.
- USB `read-only true` saved and rebooted without a build, retained all three
  display IDs and the policy map, and restored Office from its saved display ID.
  `SONOS_MODE=READ_ONLY` and independent Office state reads resumed after boot.
  The test never set mode false. Office remained paused, NORMAL, volume 100,
  track 20; choose a comfortable physical output level before any audible test.

- Final reviewed Stick image flashed with hash verification and application
  readiness. A subsequent USB smoke check confirmed the persisted three display
  IDs, `read_only=true`, Living Room policy, restored Office selection/state, and
  an accepted Office playlist plan with preserve provenance blocked at Stop.
  Evidence: `.local/room-model-stick-final-flash.log` and
  `.local/room-model-final-smoke.log`. The Stick remains in read-only mode; no
  monitor process is left holding its USB port.

### Remaining physical checkpoint

The new mode is included in the Stick display notice, but visible readability,
physical A double-click reliability, and fresh NFC taps on this room-model image
still require owner observation. Earlier NFC hardware evidence remains historical;
USB policy checks are not physical card evidence. Intentional playback/volume/
mode tests must begin only after the owner deliberately sets `read_only=false`.
See the updated [three-room physical procedure](stick-milestone-test.md).

The existing executor counts completed no-op operations: a stopped room can report
`partial` when Stop required no write and the guard blocks the subsequent queue
clear. The detail explicitly says `READ_ONLY_BLOCKED`; this is not evidence of a
partial Sonos mutation. No effect occurred in these read-only checks. Real rename,
grouping, and event-renewal/outage cases remain fixture-tested rather than newly
measured on the household. No next feature milestone has started.

## Stick double-click follow-up: 2026-09-06

- The owner reported double-clicking alternated the display between Busy and
  Refreshing rooms/state instead of completing the intended room switch. No
  physical edge trace was captured from that image, so the exact missed/rejected
  gesture remains unproven.
- Code inspection found two concrete issues: manual work notices never expired,
  and every background refresh replaced the notice; the worker also held the
  UI's state mutex across serial diagnostics and an NVS preference save. USB
  backpressure could therefore delay the main task's button sampling. M5's pinned
  driver decides single/double counts after its 500ms release window; both press
  and release edges must be sampled for correct classification.
- Moved worker logging and preference writes outside the short shared-state lock.
  Background refreshes no longer replace notices, and completed manual work/busy
  notices return to Ready. Busy input still rejects without queued replay; its
  accepted/rejected route is now explicit in serial. NFC polls defer while an A
  gesture is pending, preserving the NFC presentation/removal latch on resumption.
  Button edge/count and maximum polling-gap logs distinguish missed gestures
  from a correct double-click rejected during actual work. The click thresholds,
  Sonos requests, room configuration, and read-only boundary are unchanged.
- All 324 core behavioral checks, touch/calibration checks, and new Stick button
  checks passed. Timestamped raw edges drive the actual M5Unified 0.2.21 button
  implementation and adapter mapping: double-clicks at several spacings emit one
  RoomNext and zero Refresh events; single-click, hold, B pause, and NFC deferral
  also pass. Host-only setup now downloads just the pinned portable button sources
  and license; that isolated build/test also passed. Evidence:
  `.local/stick-button-tests.log`, `.local/stick-button-host-setup.log`.
- Both board builds passed. Verified saved `read_only=true` before flashing only
  the Stick, then observed flash hash verification and application readiness.
  Startup logged a 35ms maximum button polling gap. Evidence:
  `.local/stick-button-before.log`, `.local/stick-button-build.log`,
  `.local/stick-button-waveshare-build.log`, `.local/stick-button-flash.log`.
- Physical gesture validation was pending at flash time; the owner-confirmed
  retest below now establishes successful cycling. The original failure had no
  edge trace, so its exact cause cannot be established retrospectively.
- USB runtime follow-up passed: cycled the configured three-room list, submitted
  two immediate RoomNext inputs and observed exactly one accepted/one busy
  rejection, then restored the original Office preference. No mutating SOAP
  action was dispatched. Captured maximum button-poll gaps were 21–25ms during
  these reads. Evidence: `.local/stick-button-check.log`. A bounded 30-minute
  physical capture was opened at `.local/stick-button-physical.log` for the retest.

- **Owner-confirmed physical retest:** the owner reported “that worked.” The
  corresponding capture shows three A double-clicks, each with two complete
  press/release pairs and `decided clicks=2`, accepted at uptime 906636ms,
  910124ms, and 913492ms. Selection progressed **Office → Bedroom → Living Room
  → Office**, followed by independent successful state reads for each room.
  Sample gaps at those button edges were 1–2ms. No single-click refresh or busy
  rejection accompanied those three gestures, and no mutating SOAP request was
  dispatched in that sequence. Evidence: `.local/stick-button-physical.log`.
  This validates the corrected physical cycling path on the current image;
  fresh NFC/policy and deliberate playback checks remain separate checkpoints.

### Current-image physical NFC policy matrix

- The owner completed the read-only card sequence. The same capture contains
  five accepted physical presentations with successful NFC preparation/decoding
  and removal events: Office playlist at 1003497ms and 1050822ms, Living Room
  playlist at 1062562ms, Living Room album at 1073216ms, and Office album at
  1080772ms. The two Office playlist presentations were separate removal/retap
  cycles. Evidence: `.local/stick-button-physical.log`.
- All requests used human display IDs in their accepted diagnostics and froze
  the corresponding internal UUID plus policy revision 3. Office playlist
  preserved omitted shuffle; Living Room playlist derived true from
  `playlist-room-shuffle`; albums derived false from `albums-in-order` in both
  rooms. Volume and repeat remained preserved. Each accepted input logged its
  operation plan and reached `READ_ONLY_BLOCKED` at the first necessary write.
- Office blocked Stop; the already-stopped Living Room blocked queue clearing.
  Five guard dispatch blocks matched the five accepted presentations, with no
  mutating SOAP HTTP action dispatched anywhere in this sequence. The owner
  reported completion; logs establish physical NFC-to-policy/guard behavior,
  not audible playback or observed Sonos shuffle changes.
- This completes the current-image omitted-shuffle NFC policy matrix alongside
  the confirmed physical room cycling. Explicit-shuffle cards, intentional live
  playback/volume/mode behavior, and preservation during actual source changes
  remain separate physical checks. Runtime read-only mode remains enabled.

### Owner-enabled control mode

- After the physical read-only matrix passed, the owner explicitly requested
  `read_only=false`. Sent the runtime USB setting command and observed
  `CONFIG_SAVED`, reboot into `SONOS_MODE=CONTROL`, and a post-boot config query
  with false plus the same three display IDs and Living Room playlist rule.
  Updated `.local/config.json` to match. No rebuild or flash was required.
  Evidence: `.local/stick-enable-control.log`.
- Office restored as the selected room and remained PAUSED_PLAYBACK, NORMAL,
  volume 100, track 20 at the post-boot read. No playback test command was sent
  during this configuration change. Restarted a bounded 30-minute serial capture
  at `.local/stick-control-physical.log` for subsequent owner-driven tests.

### Root player identity guard correction

- The owner's first CONTROL-mode album and playlist taps were accepted for
  Office at uptime 176568ms and 186423ms, then rejected before Stop dispatch with
  `Destination UUID differs from accepted target`. Both the topology resolution
  and adapter preflight had identified Office correctly. No mutation was sent
  by either failed request. Evidence: `.local/stick-control-physical.log`.
- A read-only GET of Office's device description confirmed a root ZonePlayer UDN
  plus embedded MediaServer (`_MS`) and MediaRenderer (`_MR`) UDNs. The ESP guard's
  recursive walk overwrote the root UUID with the last embedded UUID, creating
  a false mismatch. Its earlier host fixtures lacked embedded devices and tested
  only the mode/room gate, so they did not expose this separate ESP-only parser.
- Replaced that walk with one portable root-ZonePlayer parser shared by adapter
  identity reads and the final HTTP guard. It selects unique direct children of
  the root device, supports XML name prefixes, rejects malformed/missing/duplicate
  identity fields, and ignores embedded identities. The actual guard now performs
  its fresh UUID read/comparison in the portable layer before calling ESP HTTP
  dispatch. True destination mismatches still block and log expected/actual UUIDs;
  mutable room names do not determine permission or retarget accepted work.
- All **334 behavioral checks**, touch/calibration, and button suites pass with
  sanitizers. Protocol fixtures now include root ZonePlayer plus embedded `_MS`
  and `_MR` devices. The shared gate is exercised with matching root identity,
  renamed room label, empty/wrong/embedded expected UUIDs, and read-only mode.
  Parser fixtures cover namespace prefixes and invalid/duplicate root fields.
  Evidence: `.local/stick-identity-tests.log`.
- Office's actual captured XML was also passed through the same parser/guard in
  a host-only program with simulated dispatch: the root matched; an expected
  `_MR` UUID, read-only mode, and malformed identity each blocked before simulated
  mutation. No sockets or live Sonos writes were used by this check. Evidence:
  `.local/office-device-description.xml`, `.local/office-identity-check.log`.
- Both board builds passed; flashed only the Stick with hash verification and
  application readiness. Fresh Office reads succeeded with the corrected shared
  parser and still reported paused playback. A post-flash config query verified
  `read_only=false`, the same three display IDs, and Living Room playlist policy.
  Evidence: `.local/stick-identity-build.log`,
  `.local/stick-identity-waveshare-build.log`, `.local/stick-identity-flash.log`.
  A 30-minute physical capture is running at `.local/stick-identity-physical.log`.
  No live mutation was sent autonomously; the next owner card tap must confirm
  actual playback beyond this corrected identity boundary.


## Unplugged testing and Stick B toggle

- The owner reported playback worked after the root-identity correction, then
  walked around with the Stick unplugged and encountered busy/topology-refresh
  glitches. There is no stored device log history: diagnostics go to USB serial;
  NVS holds configuration/preference/calibration only. The owner explicitly chose
  future laptop/live capture instead of adding on-device logging. No persistent
  logging feature was added.
- The existing capture ends after an accepted Office playlist and a successful
  Stop HTTP 200, followed by `Brownout detector was triggered`, a ROM reset banner,
  and host USB `Device not configured`. This proves the corrected guard allowed
  the root player write and that a power dip/reset occurred near disconnection;
  it does not record the later walk-around or establish the cause of those
  glitches. Full playback success in that interval is owner-reported rather
  than captured terminal verification. Evidence: `.local/stick-identity-physical.log`.
- At the owner's request, B now toggles play/pause instead of always pausing.
  Gesture admission freezes the selected UUID and policy context; a fresh read
  normalizes playing to Pause and paused/stopped to Play. Invalid/transitioning/
  no-media state rejects. The resolved explicit command follows the existing
  application/planner/guard, preserving queue/source, volume, and modes. Busy
  behavior is unchanged; USB `toggle` exposes the same path for diagnostics.
- All **347 behavioral checks**, touch/calibration, and button tests pass with
  sanitizers. New cases cover playback changing after the cached screen snapshot,
  play/pause directions, stopped state, bound-target mismatch, unknown states,
  read-only/unconfigured blocking, preservation, and uncertainty preventing a
  second mutation. The M5 raw-edge test now verifies one Toggle event per B click.
  Evidence: `.local/stick-toggle-tests.log`. Physical B pause/resume on the new
  image remains pending; no autonomous playback test was requested or performed.
- On reconnection before flashing, current RAM status showed request 13 succeeded
  and Office paused on album track 1. This is a last-result/current-state snapshot,
  not a retained timeline of the unplugged glitches. Evidence:
  `.local/stick-toggle-before.log`.
- Stick and Waveshare builds passed; only the Stick was flashed, with hash
  verification and application readiness. Post-flash read-only inspection
  confirmed the existing CONTROL setting and three configured room IDs. Evidence:
  `.local/stick-toggle-build.log`, `.local/stick-toggle-waveshare-build.log`,
  `.local/stick-toggle-flash.log`. A bounded 30-minute USB capture was started at
  `.local/stick-toggle-physical.log` for an owner B pause/resume retest. The host
  sent only a configuration query, with no toggle or other playback test command.
