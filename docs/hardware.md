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
| Waveshare bus/reset | I2C SDA15/SCL14, 100 kHz, 50-ms timeout; XCA9554 at 0x20: output 0 LCD reset, 1 DSI panel power enable, 2 touch reset; QSPI CS12/SCK11/D0–D3=4/5/6/7 | Vendor schematic/pinmap; V2 initialization measured |

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

## Inactivity power and physical buttons

The [product inactivity contract](product.md#inactivity-and-physical-wake) applies on battery and USB. Both adapters select ESP32-S3 deep sleep with one explicit RTC EXT0 button wake source and no timer. Wake reboots/reconciles; no application state is retained. The electrical paths below are established by schematics and vendor drivers. An I2C shutdown failure is logged as `peripherals-off=0`; the CPU still sleeps with Wi-Fi off, but peripheral savings cannot be assumed.

### stick-s3: M5StickS3

Physical identification (portrait orientation, USB below the front key):

```text
Front face                  Other physical controls
+----------------------+    Side key = KEY2 / M5 BtnB
|       display        |    Black side button beside STICK S3 marking
|                      |      = PWR_BTN (PM1 power/reset/download)
|  blue front key      |
+-------- USB ---------+
Front key = KEY1 / M5 BtnA / GPIO11 = automatic-sleep wake source
```

The front/side application keys are identified by position; `KEY1/KEY2` are schematic names and `BtnA/BtnB` are library names, not assumed enclosure labels. They connect GPIO11/12 to ground, each with a 10 kOhm pull-up to retained 3V3_L2. GPIO11 supports S3 RTC wake. The black power/reset button is connected to PM1, not an ESP GPIO. [M5 schematic, sheets 1–3](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1207/K150_Stick_S3_PRJ_V0.6_20251111_2025_11_17_16_10_24.pdf), [vendor button instructions](https://docs.m5stack.com/en/core/StickS3).

| State | Blue front key (KEY1) | Side application key (KEY2) | Black side power/reset button (PM1-owned) |
| --- | --- | --- | --- |
| Active, normal | Single click: refresh; double click: next configured eligible room; hold: no application action. Press/hold resets inactivity. | Click: play/pause toggle through normal policy/gate. Press/hold resets inactivity. | Single click: reset; double click: board power-off; long press: download mode. |
| Active, worker busy | Activity still counts; new refresh/room actions reject as busy. | Activity still counts; toggle rejects as busy. | Same PM1 controls. |
| Entering sleep | Application actions ignored; a key still down when EXT0 is armed can immediately wake the CPU. | No application action; not a wake source. | PM1 controls remain available. |
| Sleeping (automatic deep sleep) | Press: wake into fresh boot. | No effect. | Single click: fresh reset/boot; double/long retain PM1 power-off/download behavior. |
| Booting/waking | Initial held key/click window is consumed until release; release before another gesture. | Initial held key/click window is consumed until release. | Repeated/long presses can power off or enter download mode; release for normal boot. |
| Board powered off through PM1 | No effect. | No effect. | Single click: power on. |

Shutdown sends ST7789 sleep, turns off backlight, and removes PM1 L3B LCD/codec/microphone power and Grove ST25R3916/IR power. BMI270 sensors are disabled with advanced power save enabled; L1 remains supplied because its I2C interface shares L2 pull-ups without the codec's isolation circuit (schematic sheet 3). This avoids relying on an unpowered sensor tolerating a live bus. The L0-powered amplifier's shutdown input is driven low. SPI/Grove outputs are held low to limit back-powering. L0 PM1/charger, L1 suspended IMU, and L2 ESP/button pull-ups remain supplied; the CPU deep sleeps and Wi-Fi is off. PM1 timers/watchdog/external GPIO wake are disabled. M5Unified exposes `Power.powerOff()` and sleep helpers, but its PM1 power-off admits VIN insertion wake, so inactivity uses the explicit GPIO11 path instead of the shared PM1 IRQ or board power-off. [M5 power network and helpers](https://docs.m5stack.com/en/arduino/m5sticks3/m5pm1), [PM1 USB/shutdown example](https://github.com/m5stack/M5PM1/blob/main/examples/usb_interrupt_sleep/usb_interrupt_sleep.ino).

[BMI270 suspend registers and I2C timing](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmi270-ds000.pdf) define the IMU shutdown sequence.

### ws-1.8: Waveshare ESP32-S3-Touch-AMOLED-1.8

```text
Front view: display facing you, USB-C on the right

+-----------------------+
|                       +-- [BOOT]  upper button; sleep wake
|                       |
|       [display]       +-- [USB-C]
|                       |
|                       +-- [PWR]   lower button; PMIC power
+-----------------------+

Physical labels   Electrical destination
[BOOT] ---------> ESP32 GPIO0, active low  <-- automatic-sleep wake source
[PWR] ----------> AXP2101 PWRON
                   + inverted sense -> XCA9554 input 4 (not RTC wake)
```

Positions follow the vendor's [labeled board photo](https://docs.waveshare.com/assets/images/ESP32-S3-Touch-AMOLED-1.8-intro-5084bc71922dfe8f2f963511b719e001.webp), viewed from the display side rather than the rear PCB side.

BOOT has a 10 kOhm pull-up to DCDC1/VCC3V3. PWR is PMIC-owned; its sensed copy and AXP IRQ terminate at expander inputs 4/5. The vendor publishes a shared board schematic; V2's CO5300/CST820 path is selected by the existing touch probe. [Board schematic](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8/ESP32-S3-Touch-AMOLED-1.8.pdf), [original and V2 vendor examples](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8/tree/main/examples).

| State | BOOT | PWR (AXP2101-owned) |
| --- | --- | --- |
| Active, normal or busy | Unused for application actions; press/hold resets inactivity. | Short press: no application action; sensed press resets inactivity. Hold about six seconds: board power-off. |
| Active, raw touch calibration or display-edge diagnostic | Same button behavior; touch playback is suppressed by the existing diagnostic mode. | Same PMIC behavior; no diagnostic-specific action. |
| Entering sleep | Application activity is no longer accepted; low at EXT0 arming can immediately wake. | No application action; PMIC long-press power-off remains available. |
| Sleeping (automatic deep sleep) | Press: fresh boot/reconciliation. | Short press: no CPU wake. Long press: PMIC power-off; release, then press to power on. |
| Booting/waking | Release promptly. Holding during power-on/reset selects ROM download mode; normal application gives this button no action. | Release after power-on; a prolonged hold can power off again. |
| Board powered off through PMIC | No effect alone; keep released when powering on. | Press to power on. |

Shutdown sends DISPOFF/SLPIN through the pinned CO5300/SH8601 driver and lowers expander DSI power enable. It requests CST820 vendor sleep (`E5=3`) or FT3168 hibernate (`A5=3`), powers down QMI8658 accel/gyro, resets the unused ES8311 to power-down defaults, disables ALDO1 analog audio/mic supply, and holds amplifier enable low. PMIC ADC/watchdog work stops. DCDC1 also supplies ESP, touch, IMU, and codec digital power, so it stays on; the RTC/PMIC/charging circuits remain supplied. There is no NFC hardware on this target. CPU deep sleep stops tasks and Wi-Fi is off; touch/RTC/IMU/PMIC interrupts are not CPU wake sources.

AXP2101 software power-off (`REG10[0]`) can remove DCDC1 even with USB supplied: the schematic has no USB bypass around that regulator. However, its power-on sources include factory-customized VBUS/battery transitions, charging recovery, and IRQ. The published register interface does not establish a runtime way to restrict those sources to PWR on this exact board. Deep sleep with BOOT is selected to enforce physical-button wake without relying on those PMIC settings. This is a documented design constraint, not a claim that PMIC shutdown was measured to restart immediately. [AXP2101 power-on/off contract, section 6.5.4](https://files.waveshare.com/wiki/common/X-power-AXP2101_SWcharge_V1.0.pdf).

### Supply and measurement limits

Neither target branches on USB presence when entering automatic sleep. USB may continue charging, and USB data becomes unavailable while the ESP sleeps. A complete supply loss/brownout or an external hardware reset can still cause a fresh boot; firmware cannot suppress those electrical resets. Manual PMIC power-off is distinct from automatic deep sleep and may admit supply-triggered power-on. No battery-current savings are inferred from a dark display. Existing PMIC readings do not measure whole-board deep-sleep current while the CPU is off: use an external meter in series with the battery, compare awake and settled asleep current, and separately measure USB input with charging/full-battery state accounted for.

## USB, boot, power, and networking limits

Short inactivity timeouts can disconnect native USB before an upload starts: `flash:TARGET` builds first, then uploads, and only afterward applies any `--config` profile. For development, send an ordinary profile with `sleep_timeout_seconds: 0` using `configure` while the application is awake; restore the intended timeout afterward. If the application cannot stay reachable, enter the board's manual ROM download mode before flashing. The application inactivity timer does not run in ROM download mode or during the firmware transfer; it resumes when the application boots with its saved configuration.

Native USB needs the repository's quiet serial helper: explicit DTR/RTS writes on open caused resets even when both states were preset false. Avoiding those ioctls preserved uptime across reopen tests on this Mac. The serialport wrapper disables hangup-on-close and never explicitly sets the modem lines. Other host drivers or monitors may behave differently. USB RX is 8 KiB; its former 256-byte default lost configuration commands. TX timeout is bounded at 20 ms, and serial logging must stay outside the UI state mutex to preserve button sampling.

Flash uses watchdog reset at 115200 baud and verifies application READY/idle heartbeat separately from flash hash. A ROM `entry 0x403c88b8` line is the second-stage bootloader handoff, not proof of Arduino startup. An occasional ROM-only or later silent serial stall remains unexplained; silence cannot separate console failure from CPU/boot failure. Bounded reset has recovered some cases; physical power cycling has recovered the unchanged image in others. Application peripheral retries cannot repair a CPU that never reached the application.

Waveshare may stay powered by its battery after USB removal. PWR held alone for six seconds turns off; release and click PWR to turn on. BOOT must be released for normal startup; holding BOOT during power-on selects download mode. Stick has a side reset button. Preserve NVS; no erase is needed as routine recovery.

Both boards recovered without reboot from a measured approximately 291-second Wi-Fi outage, retried every 30 seconds, and resumed independent Sonos reads. Startup with the AP already absent is implemented but not physically exercised. An unplugged Stick capture showed brownout near USB disconnection; the cause of later owner-reported battery/busy/topology glitches remains unrecorded. There is no persistent device log history; use live laptop capture for those observations.

Topology listener startup must wait for a Wi-Fi connection. Opening its socket while Wi-Fi is uninitialized can cause an ESP32 network semaphore assertion. Manual discovery also rejects before opening UDP while disconnected. This assertion is distinct from the unresolved native USB/early-boot stalls above.

## Sonos evidence and open validation

The owner confirmed NFC album/playlist/personal-station playback and physical Stick room cycling. Both boards independently read configured-room state. Waveshare source/pause/play also completed through USB; Play acknowledgment often preceded observed PLAYING by about two seconds. This supports condition polling, not a new fixed delay. URI mapping findings live in [capabilities](sonos-capabilities.md#apple-source-mapping-and-playback-evidence).

Paused seek and selection of the third existing queue item are owner-confirmed; playing/stopped variants, single-track playback mapping, full repeat/shuffle mutation combinations, real rename/group changes, and topology subscription outage/renewal remain primarily fixture-tested. The frontend's broad acceptance is not a claim that every control mutation was traced on hardware.
