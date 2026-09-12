GOAL: MAKE THE RUNTIME SELF-HEALING AND HOST-TESTABLE

Convert the current implicit runtime lifecycle logic into explicit, independently
testable finite-state machines where that materially improves correctness.

This is a long-running autonomous reliability milestone.

The end goal is that normal recoverable failures in:

- Wi-Fi
- Sonos discovery
- Sonos HTTP
- worker/job execution
- subscription lifecycle
- topology refresh

recover automatically without requiring:

- a refresh button
- a reboot
- serial interaction
- carefully timed human acceptance steps

The owner should eventually be able to use the devices normally and report bugs,
while agents reproduce/fix lifecycle failures through host-side deterministic tests.

You may work for hours.

Prefer host-side tests and simulation first.
Use physical devices only after host gates pass and only if they are available.

Do NOT stop after converting one small machine if the approach proves successful.
Continue through the other appropriate lifecycle domains according to the gates below.

Do NOT create one giant global state machine.
Do NOT nest machines.
Do NOT model UI state in these FSMs.
Do NOT migrate the Observed/Pending/Interaction UI model into FSMs.
Do NOT reuse the old arduino-mkr-iot-carrier-sonos StateMachine implementation.
Do NOT add historical checkpoint documentation.

==================================================
0. READ FIRST
==================================================

Read:

- AGENTS.md
- current Runtime.cpp
- worker/job submission
- Wi-Fi setup/reconnect logic
- Sonos discovery/session handling
- current topology subscription logic
- AppState/application request execution
- current tests
- current docs describing runtime/network behavior

Understand the exact existing behavior before changing architecture.

==================================================
1. FIRST DECISION: FSM LIBRARY EVALUATION
==================================================

Evaluate Boost.Ext SML as the first-choice FSM library.

Reasons to evaluate it:

- header-only
- explicit transition tables
- guards/actions
- dependency injection
- logging
- host-testability
- testing facilities that can force/inspect machine state

Pin one known-good version if adopted.

Do NOT add:

- Boost distribution as a large dependency
- Boost.MSM
- TinyFSM simultaneously
- another competing FSM library

If SML proves unsuitable for this repo because of:

- toolchain incompatibility
- unacceptable diagnostics
- code-size/resource impact
- Arduino incompatibility
- poor test ergonomics

then stop the SML-specific path and implement the SAME architecture with small explicit
enum/event machines.

Do not abandon the reliability goal merely because one library is rejected.

==================================================
2. ARCHITECTURAL RULE
==================================================

State machines decide WHAT SHOULD HAPPEN.

Injected effects perform real platform work.

Machines must not directly own:

- WiFi.begin
- HTTPClient
- FreeRTOS queue calls
- sleep
- Serial
- Sonos SOAP

Conceptually:

  event
    ->
  pure/mostly-pure state transition
    ->
  emitted/injected effect
    ->
  platform adapter performs side effect
    ->
  result comes back as another event

Firmware uses real effects.

Host tests use fakes.

==================================================
3. MACHINE BOUNDARIES
==================================================

Use separate machines/lifecycles for separate concerns.

Candidate domains:

A. Worker/job lifecycle

B. Wi-Fi lifecycle

C. Sonos availability/discovery/session lifecycle

D. Subscription lifecycle

Do NOT combine these into one Cartesian-product machine.

Communication between machines happens through explicit events.

Example:

  WiFi Online
      ->
  SonosHealth receives NetworkAvailable

  WiFi Offline
      ->
  SonosHealth receives NetworkUnavailable

No nested FSM ownership.

==================================================
4. GATE 1: WORKER / BUSY LIFECYCLE
==================================================

Start here because current global `busy` behavior can wedge the device.

Replace manually maintained durable busy state with an explicit bounded job lifecycle.

Conceptually:

  Idle
  Running

Add Recovering only if real behavior requires it.

Running state must carry/associate:

- job identity
- start time
- deadline

Events should cover at least:

- Submit
- Success
- Failure
- DeadlineExpired
- NetworkUnavailable if relevant
- WorkerUnavailable
- Shutdown

Hard invariant:

EVERY accepted job eventually exits Running by:

- success
- failure
- timeout
- cancellation/shutdown

There must be no permanent Busy condition.

`busy` may remain as a DERIVED compatibility/readout value:

  busy = workerState != Idle

It must no longer be the source of truth.

==================================================
5. WORKER JOB IDENTITY
==================================================

Every accepted job gets a unique monotonic identity.

Late completion from an old job must never affect a newer one.

Explicitly test:

  Job A starts
  Job A times out
  worker returns Idle

  Job B starts

  late Success(A) arrives

Expected:

  Job B remains Running
  A's stale result is ignored/logged
  B is not completed/corrupted

Do the same for stale failure callbacks.

==================================================
6. WORKER DEADLINES
==================================================

Every running job must have a bounded deadline.

Do not rely only on lower-level HTTP timeouts.

The worker lifecycle itself owns the maximum accepted-operation lifetime.

On deadline:

- mark the job failed/timed out
- invalidate/discard affected transient session state where appropriate
- return worker machine to Idle
- schedule authoritative reconciliation
- allow later input

Do not automatically replay Sonos mutation commands after uncertain timeout.

At-most-once behavior remains preferred for mutations.

==================================================
7. HOST-TEST WORKER BEFORE CONTINUING
==================================================

Before converting another subsystem, establish strong host coverage.

At minimum:

- submit while idle
- submit while running
- success
- explicit failure
- deadline
- wrong job completion ID
- late completion after timeout
- network disappears during job
- worker unavailable
- shutdown
- subsequent job succeeds after prior failure
- thousands of sequential jobs do not wedge

Then add deterministic pseudo-random event testing.

==================================================
8. MODEL-LEVEL CHAOS TESTING
==================================================

Add a seeded deterministic event-sequence harness.

It should be able to run large numbers of transitions quickly on host.

Target at least:

  100,000 event steps

where practical.

Generate combinations of:

- submits
- successes
- failures
- deadlines
- stale completions
- network up/down
- recovery events
- shutdown/restart-model events

After EVERY step assert invariants.

Failure output must include:

- RNG seed
- minimal/current event history
- current state
- relevant context

so Codex can reproduce it exactly.

Keep this fast enough to run in normal CI/check where practical, or provide a heavier
`stress` task if needed.

==================================================
9. GATE 1 DECISION
==================================================

After worker conversion:

If the FSM/library approach is:

- readable
- testable
- deterministic
- builds cleanly
- materially safer than the previous boolean lifecycle

continue autonomously.

If not:

- revert/adjust architecture before proceeding
- do not spread a poor abstraction across the repo

==================================================
10. GATE 2: WI-FI LIFECYCLE
==================================================

Once Gate 1 passes, model Wi-Fi explicitly.

Candidate states:

  Unconfigured
  Connecting
  Online
  Backoff

Add another state only if a real semantic distinction requires it.

Candidate events:

  ConfigAvailable
  ConfigRemoved
  Connected
  Disconnected
  ConnectTimeout
  RetryDue
  Shutdown

Do not rely solely on Arduino `setAutoReconnect(true)` as the application lifecycle.

The application should know whether it is:

- trying to connect
- online
- intentionally waiting before retry
- unconfigured

==================================================
11. WI-FI EFFECTS
==================================================

Inject effects such as:

  beginConnect()
  disconnect()

Firmware implementation talks to Arduino WiFi.

Host fake records effect calls and injects connection events.

Timers/deadlines must use injected/controlled monotonic time in tests.

==================================================
12. WI-FI RECOVERY POLICY
==================================================

Implement bounded retry/backoff.

Requirements:

- connection cannot remain Connecting forever
- loss of connection causes deterministic recovery
- repeated failures do not tight-loop
- recovery requires no human input
- successful connection resets appropriate backoff state
- config removal returns to Unconfigured cleanly

Prefer simple bounded/exponential backoff with a sane maximum.

Avoid random jitter unless it solves a real household problem.

==================================================
13. WI-FI TEST MATRIX
==================================================

Host-test:

- boot with no config
- config appears
- immediate connect
- connect timeout
- retry
- repeated failure
- eventual success
- online disconnect
- reconnect
- config removed during connect
- config removed while online
- shutdown in every state
- stale Connected event during Backoff
- stale Disconnected event while already offline

Chaos-test Wi-Fi transitions too.

==================================================
14. GATE 3: SONOS HEALTH / DISCOVERY
==================================================

Once Wi-Fi lifecycle is solid, model Sonos availability separately.

Candidate conceptual states:

  Offline
  Discovering
  Ready
  Backoff

Possibly Degraded if and only if it has useful distinct semantics.

Events:

  NetworkAvailable
  NetworkUnavailable
  DiscoverySucceeded
  DiscoveryFailed
  RetryDue
  SessionFailure
  Shutdown

Do not model playback state here.

This machine represents whether authoritative Sonos access/topology is currently usable.

==================================================
15. PRESERVE STALE OBSERVATIONS DURING OUTAGE
==================================================

Change the current destructive failure behavior where appropriate.

A transient discovery/network failure should not immediately erase all useful previously
observed room/application state.

Prefer:

  last observed state retained
  +
  freshness/availability marked stale/unavailable

rather than:

  rooms = {}
  selected room erased
  UI collapses to empty

Do NOT use stale observations as authority for mutations.

Stale state may remain visible for continuity.

When Sonos recovers:

  fresh discovery/state replaces stale observations.

==================================================
16. SONOS RECOVERY
==================================================

Requirements:

- failed discovery schedules bounded retry
- stale discovery host can be discarded
- speaker reboot should recover
- selected room temporarily unavailable should recover automatically
- successful discovery refreshes authoritative topology
- recovery schedules authoritative state read
- no human refresh button required

Do not continuously hammer SSDP.

==================================================
17. SESSION FAILURE
==================================================

If a Sonos session/request repeatedly fails:

- mark/invalidate appropriate session
- do not wedge global worker
- trigger rediscovery/reconciliation according to health state
- preserve at-most-once mutation semantics

A failed request must never permanently poison future requests.

==================================================
18. GATE 4: SUBSCRIPTION LIFECYCLE
==================================================

Only after the earlier machines are solid, convert/clean up topology subscription
lifecycle if it benefits from the same pattern.

Candidate states:

  Inactive
  Subscribing
  Healthy
  Renewing
  Backoff

Events:

  NetworkAvailable
  NetworkUnavailable
  SubscribeSucceeded
  SubscribeFailed
  RenewalDue
  RenewalSucceeded
  RenewalFailed
  NotifyReceived
  LeaseExpired
  Shutdown

Do not nest this under SonosHealth.

==================================================
19. SUBSCRIPTION SELF-HEALING
==================================================

Requirements:

- stale SID cannot wedge subscription
- renewal failure causes retry
- Wi-Fi loss discards subscription assumptions
- reconnect establishes fresh subscription
- subscription failure degrades to polling/reconciliation
- events never become required for basic functionality

==================================================
20. NO GLOBAL FSM
==================================================

Explicitly reject designs like:

  ConnectedReadyIdleSubscribed
  ConnectedReadyBusySubscribed
  ConnectedDiscoveringBusy...
  
Do not encode orthogonal domains into one state enum.

Each machine must remain comprehensible independently.

==================================================
21. LOGGING / INTROSPECTION
==================================================

Every lifecycle should provide a compact snapshot suitable for:

- serial diagnostics
- tests
- future debug UI

For example:

Worker:
  state
  jobId
  startedAt
  deadline
  lastOutcome

WiFi:
  state
  since
  retryAt
  attempts
  lastError

Sonos:
  state
  since
  retryAt
  lastSuccessfulDiscovery
  lastError

Subscription:
  state
  SID-present boolean
  renewAt
  lastNotifyAt
  lastError

Do not leak secrets.

==================================================
22. TRANSITION LOGGING
==================================================

Log state transitions, not every poll.

Example:

  wifi Connecting -> Backoff reason=timeout
  wifi Backoff -> Connecting attempt=3

  worker Idle -> Running id=42 kind=refresh
  worker Running -> Idle id=42 outcome=timeout

  sonos Ready -> Backoff reason=request-failure

This should make field bug reports diagnosable without producing overwhelming logs.

==================================================
23. PROPERTY / INVARIANT TESTS
==================================================

Encode invariants centrally.

At minimum:

WORKER

- Idle owns no job
- Running owns exactly one job
- Running always has deadline
- timed-out job cannot complete a later job
- every accepted job has at most one terminal outcome

WIFI

- Online implies network-ready observation
- Connecting has a deadline
- Backoff has retry time
- Unconfigured never calls beginConnect spontaneously

SONOS

- Ready requires network available
- mutations never execute while Sonos health says unusable
- recovery never replays uncertain prior mutations

SUBSCRIPTIONS

- Healthy requires SID/lease
- network loss makes prior subscription unusable
- stale NOTIFY cannot resurrect expired subscription

==================================================
24. FAULT-INJECTION TEST HARNESS
==================================================

Create fake effects capable of simulating:

- connection never succeeds
- delayed connection
- connection drops
- HTTP timeout
- HTTP explicit failure
- SSDP returns nothing
- SSDP recovers
- speaker address changes
- session fails
- late callback
- subscription renewal fails
- stale SID notify
- queue read failure

Agents should be able to reproduce lifecycle bugs entirely on host.

==================================================
25. TEST COMMANDS
==================================================

Integrate with current Node/package workflow.

Prefer:

  node --run check

to include deterministic ordinary lifecycle tests.

Add something like:

  node --run stress

for heavier chaos/property tests if 100k+ event runs make the normal check too slow.

`stress` must use fixed/default seeds plus allow:

  --seed <value>

to reproduce a failure.

Do not introduce Python.

==================================================
26. PHYSICAL DEVICE USE — OPTIONAL LATE GATE
==================================================

If both devices are physically attached and tooling can identify them safely:

After ALL host gates pass, you may:

- build
- flash
- monitor
- exercise normal Sonos reads
- perform controlled Sonos transport mutations

The owner may leave sleep disabled for development.

Do not alter persistent config unless required and explicitly safe under current repo
rules.

Do not perform destructive unrelated household operations.

==================================================
27. PHYSICAL CHAOS — ONLY IF AUTOMATABLE
==================================================

If it can be done without human timing, autonomously test:

- restart firmware
- reconnect Wi-Fi
- repeated refresh
- repeated transport operations
- Sonos request failure/recovery where controllable
- subscriptions over an extended period

Do not stop to ask the owner to:

- unplug router at a specific second
- reboot speakers manually
- watch serial interactively
- perform repeated ordered scenarios

Those behaviors should be host-simulated instead.

==================================================
28. WATCHDOG / LAST RESORT
==================================================

Do not use reboot as ordinary recovery.

The architecture should recover through lifecycle transitions.

A hardware/software watchdog may remain appropriate for true task deadlock/crash, but it
must not mask logical state-machine failures.

Do not make:

  "stuck -> reboot"

the normal design.

==================================================
29. NO UI REDESIGN
==================================================

This milestone is runtime reliability.

Do not implement:

- LVGL
- multi-screen UI
- optimistic UI model
- grouping UI
- visual redesign

Existing UI may gain clearer connectivity/error status only if required to expose the
new lifecycle honestly.

==================================================
30. DOCUMENTATION
==================================================

Document durable runtime architecture only after the pattern is accepted.

Describe:

- independent lifecycle machines
- bounded work
- automatic recovery
- host-side fault injection
- no manual refresh requirement

Do not add:

- migration history
- overnight-run diary
- acceptance chronology
- transient seeds/logs

==================================================
31. GREEN BASELINE
==================================================

After each major gate:

  node --run format
  node --run format:cpp
  node --run check

At final completion:

  node --run check:full
  node --run stress

Keep VS Code diagnostics green.

==================================================
32. AUTONOMOUS PROGRESSION RULE
==================================================

Continue from one gate to the next WITHOUT asking for human confirmation when:

- architecture remains simpler/clearer
- host tests are green
- invariants hold
- firmware builds
- no durable product semantics need owner choice

Stop and report rather than guessing if:

- a product behavior decision is genuinely ambiguous
- adoption would require combining lifecycle machines
- SML creates unacceptable toolchain/resource consequences
- a required operation cannot be safely modeled/tested without destructive hardware
- a change would alter card/policy/grouping semantics

Do not stop merely because physical acceptance has not yet happened.

==================================================
33. SUCCESS CRITERIA
==================================================

This milestone is complete when:

1. global manually-maintained busy state is no longer the authoritative lifecycle
2. every accepted worker job is bounded and terminal
3. stale completion cannot corrupt newer jobs
4. Wi-Fi lifecycle is explicit and self-healing
5. Sonos discovery/session health is explicit and self-healing
6. subscription lifecycle is explicit/self-healing where adopted
7. transient outages preserve stale displayable state instead of unnecessarily erasing it
8. mutations never execute against known-stale/unavailable authority
9. no recoverable failure requires manual refresh
10. lifecycle behavior is host-testable
11. deterministic chaos/fault-injection tests exist
12. agents can reproduce failures from seeds/event sequences
13. no giant/nested FSM was created
14. firmware builds remain green
15. normal existing functionality remains intact

==================================================
34. FINAL REPORT
==================================================

At the end report:

- whether SML was adopted and why
- machines created
- transition-table summary for each
- effects interfaces created
- previous busy implementation removed/reduced
- retry/backoff/deadline policies
- stale-state behavior during outage
- host test count/coverage
- chaos-test command and seeds used
- any physical testing performed
- any remaining case that still genuinely requires human/device validation

Do not begin LVGL work automatically.