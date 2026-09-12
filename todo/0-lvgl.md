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
- flash an attached supported development device if safely identifiable
- collect serial/resource diagnostics
- iterate on integration/build/runtime faults

Do not stop merely because subjective physical acceptance remains.

If attached hardware is available, get the experiment fully flashed and ready for the
owner to simply use.

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
Do NOT implement Sonos event subscriptions in this milestone.
Do NOT implement the full new UI state architecture in this milestone.
Do NOT add SquareLine Studio yet.
Do NOT add Brookesia.
Do NOT add a generic FSM library.
Do NOT redesign the entire current UI.
Do NOT add historical checkpoint documentation.

Read the current rendering/touch architecture, current hardware adapters, Waveshare
frontend docs, current portable tests, and AGENTS.md first.

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

Do not build a permanent profiling subsystem.

Use lightweight development diagnostics.

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

If LVGL performs poorly because of integration choices, investigate the integration
before concluding LVGL itself is unsuitable.

==================================================
9. SCREEN SWITCHING
==================================================

Prototype two or three simple LVGL screens.

Use an explicit physical-button action to switch between them.

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

- large
- medium
- deliberately small

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
practical.

Do not attempt to unit-test the entire LVGL rendering engine.

==================================================
18. EXISTING POWER MODEL
==================================================

LVGL ticks/tasks must not accidentally count as local activity.

UI redraws/animations do NOT reset inactivity.

Only existing physical/local-user interaction rules do.

Before sleep:

- stop LVGL/display activity cleanly as needed
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

Establish one clear rule for which task/thread owns LVGL calls.

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

Do NOT add a state-machine library in this milestone.

The current UI evaluation should not depend on:

- TinyFSM
- Boost.SML
- Boost.MSM
- another FSM framework

Use existing explicit state where needed.

After UI architecture is established, small lifecycle machines can be evaluated
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

If LVGL is accepted, update docs to state clearly:

- LVGL owns widgets/input/rendering
- application owns authoritative state
- hardware adapters own display/touch details

Do not add implementation diary/history.

==================================================
24. GREEN BASELINE
==================================================

Run current canonical:

  node --run format
  node --run format:cpp
  node --run check
  node --run check:full

Keep VS Code diagnostics green.

Ensure adding LVGL does not break non-UI hardware target builds.

==================================================
25. PHYSICAL ACCEPTANCE
==================================================

When the LVGL playground is ready, stop and give me a compact physical test.

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
26. DECISION OUTPUT
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

If accepted, the next planned milestone will be the multi-screen shell built ON TOP OF
LVGL.

If rejected, the next planned milestone will use the existing rendering/touch
architecture.

==================================================
27. COMPLETION
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
11. physical evaluation can make a clear adopt/reject decision
12. all checks/builds pass

Then report only:

- LVGL version/integration mechanism
- render-buffer strategy
- LVGL ownership/threading rule
- measured RAM/PSRAM/flash impact
- physical interaction results
- recommendation: adopt / reject / adopt-with-limitations
- exact changes needed to the next multi-screen prompt if LVGL is adopted

Do not begin the multi-screen milestone automatically.