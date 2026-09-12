AUTONOMOUS GOAL MODE

Carry this LVGL evaluation as far as possible without owner interaction.

You may:

- research the pinned/current vendor integration
- add/pin LVGL
- update deterministic setup/build tooling
- build the display/touch adapter
- create the playground
- add host tests where meaningful
- build all firmware targets
- flash the attached ws-1.8 board once it is identified through the device tooling
  (node --run ports -- --identify, or the boot banner); never flash the Stick for this
  milestone
- drive the playground over USB (ui-touch / ui-nav injection) and assert its logs
- collect serial/resource diagnostics
- iterate on integration/build/runtime faults

Do not stop merely because subjective physical acceptance remains.

If attached hardware is available, get the experiment fully flashed, verified over USB,
and ready for the owner to simply use.

The owner should ideally only need to answer:

  "Does dragging/tapping/switching screens feel good?"

not perform an ordered debugging script.

If no hardware is available or physical touch cannot be evaluated autonomously, stop only
after every non-subjective gate is green and report the smallest remaining physical
evaluation.

----------------

Evaluate and prototype LVGL as the UI FOUNDATION for sonos-surface before we continue
with the planned multi-screen, optimistic-state, Sonos-subscription, and grouping UI
milestones.

This is an architectural evaluation milestone.

The goal is NOT to redesign the product UI yet.

The goal is to determine whether LVGL should own:
- widget interaction
- touch hit-testing
- dragging
- scrolling
- screen switching
- local animations
- invalidation/redraw
- basic layout

while the application continues to own:
- Sonos state
- Observed/Pending/Interaction semantics
- MusicIntent
- policy
- grouping
- networking
- device config
- read_only
- sleep

Do NOT implement grouping in this milestone.
Do NOT add AVTransport/RenderingControl event subscriptions; the existing topology
subscription lifecycle stays as it is.
Do NOT implement the full new UI state architecture in this milestone.
Do NOT add SquareLine Studio yet.
Do NOT add Brookesia.
Do NOT use Boost.Ext SML or any other FSM library for UI state (SML stays for the
runtime lifecycles only).
Do NOT redesign the entire current UI.
Do NOT change the stick-s3 UI or link LVGL into the stick-s3 build.
Do NOT add historical checkpoint documentation.

==================================================
0. CURRENT REPOSITORY FACTS
==================================================

Prerequisites: todo/1-device-tooling.md (non-interactive USB command tool, port
identification, ui-touch / ui-nav injection, ui-state) and todo/2-user-input-priority.md
(user input preempts automatic reads; `busy` means a user job is running) are complete.
If either is not, do it first.

Read first: AGENTS.md; docs/waveshare-frontend.md; docs/hardware.md (Waveshare rendering
and calibration; resource and performance constraints; inactivity and buttons);
libraries/SurfaceDevice/src/Waveshare.cpp, WaveshareUi.h, WaveshareDrawing.h,
TouchCoordinates.h, DevicePower.h, WaveshareArtwork.cpp; Runtime.cpp (loop(),
boardPoll/boardRender cadence, stateMutex); scripts/common.ts, scripts/device.ts,
scripts/hardware-targets.ts, scripts/setup.ts; tests/waveshare_ui_test.cpp and
tests/touch_test.cpp.

Facts that shape this milestone (verify, do not re-derive):

- Target is ws-1.8 only. stick-s3 keeps M5GFX, must build unchanged, and must not link
  LVGL. Guarded includes already work: Arduino_GFX is only included under
  SURFACE_WAVESHARE_1_8 and the Stick build is unaffected.
- Current rendering: WaveshareDrawing composes every frame into a 368x448 RGB565 PSRAM
  canvas (329,728 bytes) and Waveshare.cpp flushes one full frame, measured at 43-63 ms
  and blocking the main task. docs/hardware.md records why: thin primitives disappeared
  when sent as small CO5300 address windows (Arduino_GFX issue #780). The pinned
  Arduino_CO5300::writeAddrWindow performs no even-alignment of x/y/w/h.
- Touch: Waveshare.cpp polls the CST820/FT3168 every 30 ms on the main task and maps
  raw samples through the saved per-unit calibration (waveshareTouchPoint). The `held`
  flag requires a release after boot/recovery/calibration before touch is accepted.
- Main loop order: coordinator service -> boardPoll -> power -> Wi-Fi -> serial -> input
  dispatch -> render (every 100 ms when dirty) -> vTaskDelay(1). The Sonos worker task
  and the artwork task never touch the display; sharedState is copied under stateMutex
  and rendered from the copy.
- Power: a raw finger sample sets LocalActivity::Touch; BOOT low sets
  LocalActivity::Button. Nothing else counts. LVGL must not add an activity channel.
- Dependencies: pinned Arduino libraries live in scripts/common.ts `libraries` and are
  installed by node --run setup. The Arduino index offers lvgl 9.5.0 (9.2.0 through 9.5.0
  are available). Vendor evidence is already cloned at
  .local/power-research/waveshare/examples/arduino-v2: LVGL 8.4.0 with Arduino_GFX 1.6.4,
  a 1/10-screen partial draw buffer, and draw16bitRGBBitmap per flushed area on this same
  CO5300 board, plus the vendor lv_conf.h. That demo is evidence that partial windows
  can render on this panel; it is not a template.
- Build toggles: --touch-diagnostic in scripts/device.ts maps to
  -DSURFACE_TOUCH_DIAGNOSTIC and a separate .build/ws-1.8-touch directory
  (hardware-targets.ts `touchDiagnostic`). Model the LVGL playground toggle on it.
- Editor database: node --run cpp:configure validates that every owned translation unit
  has complete ESP32 context. It must be regenerated and still pass after adding LVGL.
- Flash: the ws-1.8 image is 1.84 MB of a 3 MB app partition. Internal SRAM is the
  scarce resource, not flash.
- Runtime lifecycles (worker, Wi-Fi, Sonos health, topology subscription) are Boost.Ext
  SML machines composed by RuntimeCoordinator. They are unrelated to this milestone.

==================================================
1. RESEARCH CURRENT LVGL FIT
==================================================

Confirm the best CURRENT LVGL integration path for the repository/toolchain.

Prefer LVGL 9.x unless the existing board/vendor integration strongly requires another
version.

Determine:

- how LVGL should be added/pinned in the current Arduino CLI build
- whether the current display adapters can provide the required flush interface
- whether the current touch adapters can provide the required input interface
- memory implications
- framebuffer strategy
- PSRAM requirements
- tick/task integration
- whether current Waveshare vendor libraries already include or assume LVGL pieces
- whether adding LVGL creates conflicting display ownership with existing code

Known answers to start from (verify them; do not spend time re-deriving them):

- Add/pin: `lvgl@<exact 9.x version>` in scripts/common.ts `libraries`; node --run setup
  installs it into .deps like every other pinned library.
- Config: the Arduino LVGL package looks for lv_conf.h beside the lvgl library folder,
  or with -DLV_CONF_INCLUDE_SIMPLE it includes "lv_conf.h" from the include path, or
  with -DLV_CONF_PATH=<absolute path>. Prefer LV_CONF_INCLUDE_SIMPLE with a repo-owned
  libraries/SurfaceDevice/src/lv_conf.h (that directory is already on the include path
  of every build) added through compiler.cpp.extra_flags in scripts/device.ts. Do not
  copy the vendor lv_conf.h and do not write generated files into .deps.
- Flush interface: Arduino_GFX draw16bitRGBBitmap(x, y, pixels, w, h) on the panel for
  partial areas, or writes into the existing PSRAM canvas followed by its full-frame
  flush. Both are already available in Waveshare.cpp.
- Input interface: the existing 30 ms calibrated sample. An LVGL indev read callback
  returns the last sample and pressed state; LVGL's default refresh/indev period is
  33 ms, so align the two.
- The pinned vendor libraries in this repo assume nothing about LVGL; the vendor demo
  drives LVGL through Arduino_GFX the same way this project draws today.

Do not blindly copy vendor demo architecture.

Use vendor examples as evidence, but fit LVGL into this project's architecture.

==================================================
2. TARGET OF THE EXPERIMENT
==================================================

Build a small LVGL-backed PLAYGROUND that proves the UI layer itself.

The experiment should exercise:

- screen creation/switching
- large button hit targets
- small button hit targets
- slider dragging
- continuous finger tracking
- scrolling or another gesture-owned widget interaction
- simple label/text update
- local animation
- touch release/cancel behavior

Optionally include a small canvas/drawing surface if cheap.

The experiment should use real touch hardware and real rendering.

It should NOT depend on Sonos behavior beyond maybe displaying current observed values.

The playground build still runs the full runtime (Wi-Fi, worker, topology subscription,
artwork) so memory and timing measurements are realistic. Do not measure LVGL on a
stripped-down firmware.

==================================================
3. KEEP APP STATE OUTSIDE LVGL
==================================================

Do NOT make LVGL the application's authoritative state store.

Do NOT immediately adopt lv_subject_t / observer bindings as the main application state
architecture.

Treat LVGL as the presentation/input engine.

Conceptually:

  application state
      ->
  derived UI values
      ->
  LVGL widgets

and:

  LVGL events
      ->
  application actions / local interaction state

The future Observed/Pending/Interaction/ViewModel architecture should remain possible.

Avoid introducing architecture where LVGL widget values themselves become authoritative
Sonos/application state.

==================================================
4. DISPLAY ADAPTER BOUNDARY
==================================================

Preserve hardware-specific display details in the existing adapter layer.

LVGL should not need to know:

- panel revision quirks
- QSPI pin details
- calibration details
- PMIC behavior
- touch controller revision

The hardware adapter should expose the display/touch primitives LVGL needs.

Do not leak LVGL assumptions back into shared Sonos/core libraries.

==================================================
5. TOUCH ADAPTER BOUNDARY
==================================================

Feed LVGL calibrated touch coordinates from the current touch adapter.

Do not bypass current calibration.

Verify:

- press
- move
- release
- rapid taps
- small targets
- dragging
- edge touches

If LVGL expects a different event cadence/model than the current adapter provides,
make the smallest clean adapter change.

Do not duplicate calibration in LVGL.

Injected samples from `ui-touch` (todo/1-device-tooling.md) arrive already in screen
coordinates and must enter the same LVGL indev path as hardware samples so USB
verification exercises the real pipeline.

==================================================
6. FRAMEBUFFER / FLUSH STRATEGY
==================================================

Evaluate the most appropriate LVGL render strategy for current hardware.

Compare at least conceptually:

- partial draw buffers
- larger draw buffers
- full-frame buffering where current hardware already does that successfully

Use actual memory constraints.

Do not assume full-screen double buffering is necessary.

Do not prematurely optimize.

Known constraint that must be measured, not assumed:

The current UI uses a full-frame flush because small CO5300 address windows dropped thin
primitives (docs/hardware.md). LVGL's default PARTIAL render mode flushes small areas.

Required experiments, in order:

1. PARTIAL mode with an internal-SRAM draw buffer (for example 368 x 40 x 2 bytes).
   Check whether thin lines, 1 px borders, slider tracks, and text render intact across
   the whole panel, including odd x offsets and odd widths.
2. If not, register an LV_EVENT_INVALIDATE_AREA handler on the display that rounds
   invalidated areas to even x/y and even w/h (the driver does no alignment). Re-check.
3. If still not, use DIRECT or FULL render mode into the existing PSRAM canvas and keep
   the full-frame flush. Measure the cost.

Whichever mode is chosen, record the durable finding (what fails, what works, and why)
in docs/hardware.md under the Waveshare rendering section, and measure touch-to-flush
latency and main-loop poll gap under each mode you try.

The success criterion is:

- responsive touch
- stable rendering
- no obvious tearing/flicker regression
- acceptable memory use

==================================================
7. PERFORMANCE INSTRUMENTATION
==================================================

Instrument enough to measure:

- LVGL task/tick cadence
- average/representative flush time
- touch-to-visible-response latency
- slider drag responsiveness
- free heap / PSRAM before and during UI
- dropped/late frames if observable

Concretely, log with millis() timestamps:

- touch sample time -> lv_timer_handler start -> flush complete (touch-to-visible)
- flush duration and the flushed area size
- maximum main-loop poll gap under a continuous drag (touch starvation)
- free internal heap and free PSRAM at boot, during a drag, and during an artwork
  download
- LVGL memory pool usage (lv_mem_monitor) when the built-in allocator is used

Do not build a permanent profiling subsystem.

Use lightweight development diagnostics, in the style of the existing `[ui] frame` and
`[artwork]` lines.

==================================================
8. INPUT RESPONSIVENESS
==================================================

The key physical test is whether LVGL solves the problems the current handwritten UI
would otherwise need to solve manually.

Validate:

- finger can drag slider smoothly
- slider does not jump erratically
- touch targets can be significantly smaller than current conservative handmade ones
- rapid repeated button taps resolve correctly
- moving finger across control remains captured appropriately
- releasing outside control behaves sensibly
- touch input is not blocked by ordinary rendering

Note that today's full-frame flush blocks the main task for 43-63 ms per frame; judge
"not blocked by rendering" against the render mode chosen in section 6, and report the
measured poll gap.

If LVGL performs poorly because of integration choices, investigate the integration
before concluding LVGL itself is unsuitable.

==================================================
9. SCREEN SWITCHING
==================================================

Prototype two or three simple LVGL screens.

Use an explicit physical-button action to switch between them.

The physical button is BOOT (GPIO0). Today Waveshare.cpp only reads it as activity
(digitalRead(0) == LOW) with no edge detection. Implement, in the adapter, so the
multi-screen milestone can reuse it:

- action on release (edge-triggered), not on press
- debounce of at least 30 ms
- holds longer than 1 s perform no action
- the press that woke the device from deep sleep is consumed: BOOT is the EXT0 wake
  source and is still low during boot, so require a release before the first navigation
  press is accepted (Stick.cpp's releaseAfterBoot is the same pattern)
- press/hold still counts as local activity exactly as today

PWR is PMIC-owned and a long press powers the board off; leave it unused.

Expose the same action over USB as `ui-nav next` (todo/1-device-tooling.md) so screen
switching can be exercised without a finger. USB navigation does not count as activity.

Do NOT implement global swipe navigation.

The purpose is to prove:

- screen lifecycle
- hidden screen does not receive touch
- active touch is safely cancelled/released during switch
- screen switching is cheap/reliable

Do not yet create the final Now Playing / Grouping / Playground screen architecture.

This is only a UI-engine proof.

==================================================
10. SMALL-TARGET TEST
==================================================

Create a dedicated test surface with several target sizes.

For example:

- large (44 px)
- medium (32 px)
- deliberately small (24 px)

Measure/observe whether the calibrated touch + LVGL hit testing can reliably operate the
smaller controls.

This is important because the stock/demo UI demonstrated that dense controls can work
better than the current handmade UI suggests.

Do not treat tiny controls as a product requirement.

This is a capability test.

==================================================
11. SLIDER TEST
==================================================

Create a slider that:

- follows the finger continuously
- updates a nearby numeric label
- supports dragging from any reasonable point
- does not trigger network/Sonos activity

This should be entirely local.

Use it to validate:

- input event flow
- visual response latency
- touch capture
- redraw behavior

Do not implement seek semantics yet.

==================================================
12. OPTIONAL CANVAS TEST
==================================================

If low effort, add a tiny drawing/canvas test:

  finger down/move
      ->
  draw line following touch

This is not a product feature.

It is useful because it closely reproduces the sort of rich vendor demo behavior that
motivated this evaluation.

If it adds significant work, skip it.

==================================================
13. CURRENT UI COEXISTENCE
==================================================

Do NOT delete the current accepted UI during evaluation.

Preferred approach:

- retain current production/prototype UI path
- add an LVGL playground path or temporary compile/runtime selection
- prove LVGL physically
- decide afterward whether to replace the current rendering layer

Concretely: add a --lvgl-playground option to build/flash in scripts/device.ts and
scripts/hardware-targets.ts, mapping to -DSURFACE_LVGL_PLAYGROUND=1 and a separate
.build/ws-1.8-lvgl directory, exactly like --touch-diagnostic. The normal ws-1.8 build
and the stick-s3 build must be unaffected. node --run check:full must build the
playground variant as well (or gain an explicit task for it that check:full runs).

Avoid maintaining two permanent UI frameworks.

A temporary evaluation toggle is acceptable.

Do not document both as long-term supported architectures.

==================================================
14. DEPENDENCY DISCIPLINE
==================================================

If adding LVGL:

- pin a specific version
- make installation deterministic through current setup tooling
- keep dependency ownership explicit
- update build/config tooling cleanly

Do not add:

- SquareLine Studio generated code
- Brookesia
- another display framework
- another touch framework
- unrelated UI dependencies

One experiment, one UI library.

==================================================
15. LVGL CONFIG
==================================================

Create the smallest project-owned LVGL configuration necessary.

Keep options intentional.

Avoid copying a huge vendor lv_conf.h full of unrelated enabled features.

Enable only what the experiment/current project needs.

Start from the LVGL 9 lv_conf_template.h: keep LV_COLOR_DEPTH 16; enable only the
widgets, fonts, and features the playground uses; choose the memory allocator
deliberately (built-in pool in internal SRAM versus the C library allocator backed by
heap_caps) and record the choice; disable examples, demos, and file systems; keep
LV_USE_LOG available behind a build-time switch for diagnostics.

Document only durable non-obvious configuration choices.

==================================================
16. SCREEN/UI CODE ORGANIZATION
==================================================

Keep LVGL-specific code separate from shared core/application logic.

A reasonable conceptual boundary is:

  SurfaceCore / SurfaceSonos
      no LVGL

  SurfaceDevice / frontend adapter
      LVGL integration

  UI/screens
      LVGL widgets + translation from app state

Exact folders/names may follow current repo structure.

Do not introduce LVGL headers into policy/intent/planner code.

==================================================
17. TESTABILITY
==================================================

Do not make portable application tests require a physical display.

Keep:

- Sonos logic
- policy
- state derivation
- intent logic

testable independently of LVGL.

For this milestone, add lightweight tests for any new pure adapter/state logic where
practical (for example the BOOT debounce/edge logic, the injected-sample queue, and any
area-rounding helper).

Do not attempt to unit-test the entire LVGL rendering engine.

Optional, only if cheap: a headless host target that compiles LVGL with a dummy flush
callback and a scripted indev, so screen lifecycle and hit tests can run on the host in
later milestones. Do not add it to node --run check unless it compiles in well under 30
seconds; otherwise add a separate task and mention it in the report.

==================================================
18. EXISTING POWER MODEL
==================================================

LVGL ticks/tasks must not accidentally count as local activity.

UI redraws/animations do NOT reset inactivity.

Only existing physical/local-user interaction rules do.

The raw finger sample in Waveshare.cpp already produces LocalActivity::Touch; keep that
as the only touch activity channel. lv_timer_handler, animations, redraws, and injected
USB input touch nothing in DevicePower.

Before sleep:

- stop LVGL/display activity cleanly as needed (stop calling lv_timer_handler before the
  panel is powered down; lv_deinit is not required)
- preserve existing hardware shutdown semantics

On wake:

- initialize UI fresh

Do not persist LVGL screen/widget state as authoritative application state.

==================================================
19. EXISTING THREADING MODEL
==================================================

Do not let LVGL introduce long blocking work into UI callbacks.

LVGL event handlers must not:

- perform synchronous Sonos HTTP
- download artwork
- wait for network operations

They should enqueue/request application actions through existing paths.

Likewise background workers must not directly manipulate LVGL objects from unsafe threads
if LVGL requires single-threaded ownership.

The rule for this project:

  The Arduino main task owns LVGL. lv_init, lv_timer_handler, the flush callback, the
  indev callback, and every widget create/update/delete run only from Waveshare.cpp's
  poll/render path. The Sonos worker and artwork tasks publish into AppState and the
  artwork buffers under their existing locks; the main task copies the snapshot and
  updates widgets from the copy. No lv_* call from any other task, ever.

State this rule in the adapter header and in docs if LVGL is adopted.

==================================================
20. COMPARE AGAINST HAND-ROLLED UI
==================================================

At completion, explicitly evaluate whether LVGL materially improves:

- touch reliability
- slider dragging
- small-target interaction
- screen management
- redraw/invalidation
- code complexity
- future maintainability

Also identify costs:

- flash size
- RAM/PSRAM
- integration complexity
- build time
- debugging complexity

Do not declare LVGL successful merely because it compiles.

The decision should be based on physical usability and architectural fit.

==================================================
21. STATE MACHINE LIBRARIES
==================================================

Boost.Ext SML is already pinned and used for the runtime lifecycle machines (worker,
Wi-Fi, Sonos health, topology subscription). Do not use it, or any other FSM library, for
UI/screen state in this milestone.

Do not add TinyFSM, Boost.MSM, or another FSM framework.

Use explicit enums/structs where the playground needs state.

After UI architecture is established, small UI lifecycle machines can be evaluated
separately if repeated patterns justify it.

Do not reuse the old arduino-mkr-iot-carrier-sonos StateMachine implementation.

==================================================
22. SQUARELINE
==================================================

Do NOT add SquareLine Studio/generated UI yet.

Direct LVGL code is preferred for this experiment because we need to understand:

- runtime semantics
- event handling
- source control shape
- application-state integration

After LVGL itself is accepted, visual code generation can be evaluated separately for
larger polished layouts.

==================================================
23. DOCUMENTATION
==================================================

Add a concise current UI architecture note only if the evaluation results in a durable
decision.

Before the experiment is accepted, do not rewrite the repo as though LVGL is already
the permanent architecture.

The measured CO5300 rendering finding from section 6 is durable regardless of the
decision; record it in docs/hardware.md.

If LVGL is accepted, update docs to state clearly:

- LVGL owns widgets/input/rendering
- application owns authoritative state
- hardware adapters own display/touch details
- the main task owns every LVGL call

Do not add implementation diary/history.

==================================================
24. GREEN BASELINE
==================================================

Run current canonical:

  node --run format
  node --run format:cpp
  node --run check
  node --run check:full

Keep VS Code diagnostics green (regenerate node --run cpp:configure -- ws-1.8 and confirm
its validation passes with LVGL present).

Ensure adding LVGL does not break non-UI hardware target builds. The playground variant
must also build with --warnings more and no owned-code warnings.

==================================================
25. AUTONOMOUS DEVICE VERIFICATION
==================================================

Before asking for physical acceptance, prove the integration over USB using the device
tooling (node --run usb, ui-touch, ui-nav, ui-state, monitor):

1. Identify the ws-1.8 port; flash the playground variant; wait for READY. Keep sleep
   disabled for the session (sleep_timeout_seconds: 0 through configure) and restore the
   intended profile afterward.
2. Inject taps at the centers of the large/medium/small targets; assert the LVGL click
   log line for each and count misses per size.
3. Inject a drag across the slider (press, N moves at 30 ms, release); assert the slider
   value follows and the label updates; log touch-to-flush latency per move.
4. Inject a press, then ui-nav next, then a release; assert the old screen's release
   never fires and the new screen receives no phantom press or release.
5. Inject rapid double and triple taps; assert one click per tap.
6. Inject edge coordinates (x = 0/367, y = 0/447) and out-of-range values; assert no
   crash and no off-screen hit.
7. Read heap/PSRAM from the heartbeat before and after a five-minute injected workload
   (taps, drags, screen switches); assert no downward trend.
8. Leave the board flashed with the playground and include the injected results and
   measured numbers in the report.

Injected input never counts as local activity and never bypasses admission: it is the
same path a finger takes.

==================================================
26. PHYSICAL ACCEPTANCE
==================================================

When the LVGL playground is ready and section 25 has passed, stop and give me a compact
physical test.

I want to evaluate:

1. button hit reliability
2. small target reliability
3. slider drag feel
4. finger tracking
5. screen switching
6. visual latency
7. flicker/tearing
8. memory stability

If canvas test exists:
9. draw continuously with finger

Also report measured resource usage.

Do not proceed automatically to replacing the current UI.

==================================================
27. DECISION OUTPUT
==================================================

After physical testing, make an explicit recommendation:

A. ADOPT LVGL

B. DO NOT ADOPT LVGL

C. ADOPT WITH A SPECIFIC LIMITATION

Base the recommendation on:

- physical interaction quality
- architectural fit
- resource cost
- maintainability

Use these measurable gates. The thresholds are proposals: report the measured numbers
either way, and adjust a threshold only with evidence stated in the report.

- touch-to-flush latency during a drag: median <= 60 ms, p95 <= 120 ms
- injected taps: 20/20 on 44 px targets, >= 18/20 on 32 px; report the 24 px rate
- maximum main-loop poll gap under a continuous drag <= 100 ms
- free internal heap floor during a drag plus an artwork download >= 50 KB
- ws-1.8 image <= 2.6 MB of the 3 MB app partition
- no thin-primitive loss with the chosen render mode
- build time delta for node --run check:full reported

The owner then judges only feel, flicker, and appearance.

If accepted, the next planned milestone will be the multi-screen shell built ON TOP OF
LVGL.

If rejected, the next planned milestone will use the existing rendering/touch
architecture.

==================================================
28. COMPLETION
==================================================

Stop when:

1. LVGL is deterministically integrated enough to test
2. real calibrated touch feeds LVGL
3. buttons work
4. small-target test works
5. slider drag works
6. screen switching works
7. local animation/rendering works
8. application/network logic remains outside LVGL
9. sleep semantics remain intact
10. resource/performance costs are measured
11. the USB-injected verification in section 25 passed and is logged
12. physical evaluation can make a clear adopt/reject decision
13. the durable CO5300/LVGL rendering finding is recorded in docs/hardware.md
14. all checks/builds pass

Then commit the finished work: todo/README.md "How to run" rules, a `todo 3:`
subject, the README status cell set to `implemented (<hash>)`, only on a green
baseline, never a push. The prompt is not complete with uncommitted work.

Then report only:

- LVGL version/integration mechanism
- render-buffer strategy and the CO5300 finding
- LVGL ownership/threading rule
- measured RAM/PSRAM/flash impact and the section 27 gate numbers
- injected and physical interaction results
- recommendation: adopt / reject / adopt-with-limitations

Then, as the final step of this milestone, rewrite todo/4-ws-1.8-multiscreen.md to the
chosen track: keep the sections marked for that track, delete the other track's sections
and the TRACK SELECTION preamble, and fold in anything this evaluation learned (button
mechanics, render mode, threading rule). Note in the report any other todo prompt that
must change.

Do not begin the multi-screen milestone automatically.
