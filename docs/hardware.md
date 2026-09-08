# Hardware facts, evidence, and limits

This document retains durable board observations and unresolved physical issues. Contracts live in [product](product.md), [intent](intent.md), [policy](policy.md), [execution](planner-executor.md), and [capabilities](sonos-capabilities.md). [README](../README.md) owns setup, flashing, USB recovery, and calibration commands. Private logs remain in ignored `.local`; Git history retains prior experiments.

## Supported boards and wiring

| Board | Driver/wiring facts | Evidence |
| --- | --- | --- |
| M5StickS3 | Board ID 26; ST7789, 240×135 landscape; 8 MB PSRAM | Physical display/NFC playback and repeated warm startup confirmed |
| Stick LCD | SPI3 MOSI39, SCLK40, DC45, CS41, RESET21; backlight38; 135×240 panel offsets 52/40 | Explicit pinned M5GFX wiring in `StickDisplay.h` |
| Stick power/Grove | PM1 at 0x6e on I2C1 SDA47/SCL48; Grove SDA9/SCL10 at 100 kHz, 50-ms timeout, Grove power enabled | Boot and disconnected/reconnected NFC observed |
| M5 NFC Unit | ST25R3916, pinned M5Unit-NFC NFC-A implementation | Household NTAG213, 144 user bytes; album/playlist/station cards read |
| Waveshare AMOLED 1.8 V2 | CO5300 / CST820 at 0x15; 368×448; ESP32-S3 rev 0.2; 16 MB flash, 8 MB PSRAM | Owner's physical board; UI and artwork accepted |
| Waveshare AMOLED 1.8 V1 | SH8601 / FT3168 at 0x38; 368×448 | Vendor driver path exists; no physical V1 validation |
| Waveshare bus/reset | I2C SDA15/SCL14, 100 kHz, 50-ms timeout; XCA9554 at 0x20 resets outputs 0–2; QSPI CS12/SCK11/D0–D3=4/5/6/7 | Vendor pinmap in adapter; V2 initialization measured |

Stick startup supplies an explicit board and panel, bypassing M5GFX autodetection and its `M5GFX/AUTODETECT` NVS cache. Detection hints previously misidentified the unit as StampS3Mini (155), giving 0×0 display and Grove 2/1. PM1 idle sleep is disabled before LCD power writes. The electrical cause of failed identification is unproven; explicit wiring removes that avoidable runtime decision.

Waveshare probes touch to choose its revision; no response means no guessed panel. The expander's 20-ms reset pulse and bounded readiness probe are electrical adapter requirements. Successful panel/QSPI/canvas allocations are reused: pinned QSPI begin cannot safely run twice. A failed initial QSPI allocation requires reboot. Other initialization failures retry every five seconds; ten consecutive touch read failures trigger peripheral recovery and require release before further actions. USB `peripherals-retry` has recovered the V2 adapter without CPU/network restart.

## NFC reading and limits

The reader accepts one Text, URI, or observed TNF=1/empty-type raw URL record under the [card contract](intent.md#versioning-optional-data-and-nfc-encoding). The empty type is an actual household encoding, not permission to guess malformed Text/URI payloads. NTAG213's measured 144-byte capacity is relevant to the future writer; 4,096-byte parser capacity does not imply that cards can store 4 KB.

Polling uses WUPA so halted held tags do not falsely appear removed. Three missed polls release the presentation latch; one presentation submits once, and removal/ retap permits another. Identify deactivates the tag, so explicit reactivation before reading is required. Successful measured preparations took about 50–52 ms to identify, 27 ms to reactivate, and 73–85 ms for NDEF reads. These are samples, not deadlines.

An intermittent identify/reactivate failure recovered on retap; phase logging now distinguishes stages, but its cause remains unresolved. No guessed RF delay or automatic playback retry was added. NFC absent at boot does not block display, Wi-Fi, or Sonos; reconnecting Grove recovered reading without reboot in a physical test. NFC polling defers while a Stick A gesture is pending to preserve button sampling. M5Unified uses its 500-ms click decision window.

Writing/formatting is not implemented. Actual writable tag/capacity handling, interrupted-write behavior, and verified readback still need a focused writer hardware slice. NFC-B/F/V and multi-record formats remain unsupported.

## Waveshare rendering and calibration

Thin primitives disappeared when sent as small CO5300 address windows. Rendering all pixels in a 368×448 RGB565 PSRAM canvas (**329,728 bytes**) and flushing one full frame corrected the visible lines/crosshairs on the owner's V2 board. Keep this workaround unless a measured replacement proves it unnecessary.

A visual mask experiment found approximately inset 4 px/radius 50 px aligned with this unit's rounded visible boundary; it is not pixel-exact or a universal panel specification. The accepted UI fits within that mask. Stick has a small redraw flicker when text changes, consistent with direct clear/redraw; no idle flicker was reported.

Calibration belongs solely to the adapter: NVS namespace `surface`, key `touch`, separate from household `config`. Replacing/copying household config must never copy or erase a unit's correction. V1 calibration JSON contains controller, version, x/y scale, and x/y offset. The independent-axis affine model rounds mapped coordinates. Positive scales 0.5–1.5 and offsets ±112 are supported model bounds; missing/invalid/version/controller mismatch falls back to identity and warns. Invalid updates retain the old fit. Raw/mapped off-screen reports reject instead of clamping into buttons. Diagnostics always remain raw.

The owner's six-target V2 fit has maximum training residual **8.9623 px**. It maps nominal raw bounds to approximately x=32–323/y=28–415, so edges/full-screen accuracy are unvalidated. Fit values live on the unit, not firmware defaults. A different unit requires its own raw six-target run and independent center-tap validation; training residual is not an independent accuracy measurement.

Calibrated Pause/Play center taps, a Refresh tap after a power-button restart, the current frontend, artwork appearance, and artwork following an external album change are owner-confirmed. Calibration survived repeated software reboots, reflashes, peripheral recovery, and a battery-backed power-button restart.

## Resource and performance constraints

The [frontend artwork contract](waveshare-frontend.md#album-artwork) owns exact worker limits: one in-flight generation, PSRAM buffers, bounded baseline JPEG, and only the current thumbnail retained. Late/changed-room images are discarded; artwork runs separately from the Sonos worker and UI locks. No shared layer fetches images, and raw touch diagnostics start no artwork worker.

Measured V2 examples, not worst-case guarantees:

- Full-frame flush about 43–63 ms; typical complete UI frames 57–86 ms. Full flush is the main regular blocking work; a deliberate peripheral reset produced a 448-ms polling gap. Short-tap and larger-image latency are not proven by averages.
- A 400×400, 64,468-byte speaker JPEG took 704–858 ms to download and 191–195 ms to decode/fit. Working free PSRAM at buffer overlap was about 7.75 MB; an 8,192-byte retained cover used about 8,452 allocator/runtime bytes. No cumulative PSRAM loss appeared across that small room-switch sample; this was not a stress test.
- Room navigation rendered during a download; switching rooms cancelled stale generation processing and restored the no-art baseline. Returning published only the new generation's cover.
- Four-item queue pages took about 78–133 ms excluding preceding discovery/state reads. Shared SOAP bodies are capped at 64 KiB; queue pages at 20 items, with four requested by Waveshare. Unusually large metadata may require smaller pages.

## USB, boot, power, and networking limits

Native USB needs the repository's quiet serial helper: explicit DTR/RTS writes on open caused resets even when both states were preset false. Avoiding those ioctls preserved uptime across reopen tests on this Mac. The serialport wrapper disables hangup-on-close and never explicitly sets the modem lines. Other host drivers or monitors may behave differently. USB RX is 8 KiB; its former 256-byte default lost configuration commands. TX timeout is bounded at 20 ms, and serial logging must stay outside the UI state mutex to preserve button sampling.

Flash uses watchdog reset at 115200 baud and verifies application READY/idle heartbeat separately from flash hash. A ROM `entry 0x403c88b8` line is the second-stage bootloader handoff, not proof of Arduino startup. An occasional ROM-only or later silent serial stall remains unexplained; silence cannot separate console failure from CPU/boot failure. Bounded reset has recovered some cases; physical power cycling has recovered the unchanged image in others. Application peripheral retries cannot repair a CPU that never reached the application.

Waveshare may stay powered by its battery after USB removal. PWR held alone for six seconds turns off; release and click PWR to turn on. BOOT must be released for normal startup; holding BOOT during power-on selects download mode. Stick has a side reset button. Preserve NVS; no erase is needed as routine recovery.

Both boards recovered without reboot from a measured approximately 291-second Wi-Fi outage, retried every 30 seconds, and resumed independent Sonos reads. Startup with the AP already absent is implemented but not physically exercised. An unplugged Stick capture showed brownout near USB disconnection; the cause of later owner-reported battery/busy/topology glitches remains unrecorded. There is no persistent device log history; use live laptop capture for those observations.

Topology listener startup must wait for a Wi-Fi connection. Opening its socket while Wi-Fi is uninitialized can cause an ESP32 network semaphore assertion. Manual discovery also rejects before opening UDP while disconnected. This assertion is distinct from the unresolved native USB/early-boot stalls above.

## Sonos evidence and open validation

The owner confirmed NFC album/playlist/personal-station playback and physical Stick room cycling. Both boards independently read configured-room state. Waveshare source/pause/play also completed through USB; Play acknowledgment often preceded observed PLAYING by about two seconds. This supports condition polling, not a new fixed delay. URI mapping findings live in [capabilities](sonos-capabilities.md#apple-source-mapping-and-playback-evidence).

Paused seek and selection of the third existing queue item are owner-confirmed; playing/stopped variants, single-track playback mapping, full repeat/shuffle mutation combinations, real rename/group changes, and topology subscription outage/renewal remain primarily fixture-tested. The frontend's broad acceptance is not a claim that every control mutation was traced on hardware.
