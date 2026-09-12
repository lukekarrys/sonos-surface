Make USER INPUT TAKE PRIORITY OVER AUTOMATIC BACKGROUND WORK, and cut the cost of an
automatic poll.

This is a runtime reliability milestone on top of the completed lifecycle machines
(WorkerLifecycle, WifiLifecycle, SonosHealth, SubscriptionLifecycle, RuntimeCoordinator).
It changes admission and the automatic poll. It does not change what a user action does
once it is admitted.

AUTONOMOUS GOAL MODE

Work host-first (fault harness, chaos, invariants), then measure on the attached boards
with the tooling from todo/1-device-tooling.md. AGENTS.md permits Sonos mutations on the
rooms configured on the device under test. Do not stop for the owner until section 9 has
run and its numbers are in the report.

Do NOT add a second Sonos worker task or concurrent Sonos requests.
Do NOT change at-most-once mutation semantics, uncertainty/recovery, or read_only.
Do NOT change the executor's own preflight and verification reads (prepare/verify).
Do NOT queue a user action behind another user action; that stays a visible busy
rejection.
Do NOT change transport settings (timeouts, retries, Wi-Fi power save) before the
measurements in sections 6 and 7 exist; then change them only with that evidence.
Do NOT add UI screens, LVGL, event subscriptions, or the pending/view model here.
Do NOT add historical checkpoint documentation.

Read first: AGENTS.md; docs/runtime-lifecycles.md; docs/planner-executor.md (admission
and frozen work; observation and reconciliation); docs/hardware.md (resource and
performance constraints; USB limits); README.md (Commands and diagnostics);
libraries/SurfaceDevice/src/WorkerLifecycle.h, RuntimeCoordinator.h (admission,
enqueueJob, bindDiscovery, discoveryResult, finishJob, applyFacts, automaticJobDue),
RuntimeAdmission.h, DeadlineClient.h, JobNetworkClient.h, Runtime.cpp (EspHttp::dispatch
and its `allowed` closure, discoverAddresses, the worker loop, submit/selectRoom/
submitToggle, loop()); libraries/SurfaceCore/src/SurfaceCore.cpp (Application::
readObservation, submit, discardCancelledResult); libraries/SurfaceSonos/src/
SurfaceSonos.cpp (identity, discover, refresh, reconcile, ungrouped, prepare, verify,
GuardedHttp::request); libraries/SurfaceDevice/src/WaveshareUi.h (mutation() and
fresh()), Stick.cpp (the Busy label); tests/runtime_fault_test.cpp (RuntimeFixture,
busyWinsOverDiscoveryRecovery, the chaos loop and its liveness window),
tests/worker_lifecycle_test.cpp, tests/sonos_lifecycle_test.cpp.

==================================================
0. MEASURED FACTS
==================================================

From .local/fsm-device/regression-live-stick.log (22 automatic polls) and the current
source. Verify against the tree; do not re-derive from scratch.

- An automatic poll job makes 13 HTTP requests: on the SSDP-chosen discovery host,
  GET /xml/device_description.xml and GetZoneGroupState; on the selected room,
  GET /xml/device_description.xml, GetTransportInfo, GetPositionInfo,
  GetTransportSettings, GetMediaInfo, GetVolume, GetMute, Browse, GetPositionInfo,
  GetMediaInfo, and a second GetZoneGroupState (ungrouped() inside
  DirectSonos::reconcile).
- Job duration: about 0.95 s median, 3.68 s worst. The slow polls are the discovery
  host: discoverAddresses() collects hosts in a std::set<std::string>, so the
  lexicographically smallest IP (Kid's Room, 192.168.4.98) becomes the topology source,
  and its GetZoneGroupState took 2.6-2.8 s twice, versus about 100 ms from the selected
  room.
- With a 10 s cadence the worker is Running 9-13 % of the time, and every user action in
  that window is rejected: RuntimeCoordinator::admission returns "Busy; input ignored"
  whenever the worker is Running, for any job. WaveshareUi::mutation() also rejects on
  context.busy, and the Stick shows "Busy".
- This contract predates the lifecycle work (the old std::atomic busy did the same). The
  lifecycle work added one request per poll (ungrouped in reconcile), immediate recovery
  reads after failures, and rejection during Sonos Backoff.
- A user mutation costs about 25 requests (pickup topology 2, prepare's refresh and
  ungrouped 11, GuardedHttp identity GET 1, the action 1, verify's refresh 10); Stick's
  toggle adds a refresh first (about 35). At 60-100 ms per request a pause completes in
  2-3 s even when admitted at once.
- WiFi.setSleep is never called; Arduino's default modem power save is active.
- "Connection refused" appears in no saved log; failure timing is unmeasured.
- Button sampling is unaffected: [button] max-poll-gap-ms stays at 14-35 ms during
  polls. Presses are sampled, then rejected at admission.
- Cancellation already exists: jobActive(id) false makes EspHttp's `allowed` closure
  false, DeadlineClient aborts the in-flight read/write/connect at its next check, the
  Sink stops accepting bytes, and the worker unwinds; finishJob returns false for the
  cancelled id and the worker calls Application::discardCancelledResult. That path
  currently marks the retained observation stale, and applyFacts treats every outcome
  other than Success/Failure as a session failure that requests reconciliation.

==================================================
1. PRODUCT RULE
==================================================

A local user action (button, touch, NFC card, USB command) is never rejected because
automatic background work is running.

"Busy" to a user means another USER action is in flight. Automatic reads yield.

A user action that arrives while another user action is running is still rejected
visibly, exactly as today.

  OWNER DECISION (recorded): [x] reject visibly   [ ] queue at most one user action

Sonos authority rules are unchanged: with Sonos in Backoff/Offline a mutation is still
rejected with the recovery notice, and a preempted read never grants authority.

==================================================
2. PREEMPTION, NOT CONCURRENCY
==================================================

Tag every job with an origin: User or Automatic. RuntimeCoordinator::enqueueJob takes it
and WorkerSnapshot exposes it.

Admission for a User-origin request while the worker is Running an Automatic job:

  preempt the automatic job
      ->
  worker Idle (new terminal outcome JobOutcome::Preempted)
      ->
  Submit the user job (Running, new id)
      ->
  the user job waits in the existing single-slot queue while the preempted task unwinds
  cooperatively (the path timeouts already use)
      ->
  the worker picks it up

Add one WorkerLifecycle event (Preempt) guarded on "running job is Automatic"; a Preempt
against a User job or while Idle is ignored and counted. Preempted joins Timeout,
NetworkLost, Unavailable, and Shutdown as a terminal outcome. Invariant: Preempted only
ever applies to an Automatic job.

What a preempted automatic read must NOT do (this is where the existing cancellation
path is wrong for preemption):

- it must not mark the retained observation stale: nothing failed, so the previous
  observation stays exactly as it was, WaveshareUi::fresh() stays true, and the user
  action is not rejected as "Refresh room first"
- it must not raise SessionFailure, set refreshError, or request reconciliation; the user
  job's own outcome handling does that as today
- it must not emit DiscoveryFailed for an open discovery generation; the user job binds
  the same open generation (bindDiscovery already accepts a Discovering state) and its
  own topology read produces the result
- it must not publish anything

Cover both timing cases explicitly: preempted during the pickup topology read, and
preempted during the room read.

Bound: a user job reaches Running within one in-flight socket operation. Give automatic
reads a 1.5 s connect timeout (user jobs keep 3 s) so the bound is at most 1.5 s when a
speaker is unreachable and typically well under 100 ms. Measure it in section 9.

User reads (refresh, room-next, room-select, queue page) are User origin and preempt
automatic reads the same way. Automatic reads never preempt anything.

==================================================
3. DERIVED FLAGS
==================================================

`busy` as shown to users (BoardContext.busy, the Stick "Busy" label, WaveshareUi's
"Busy - try again", CONFIG_BUSY, REBOOT_BUSY) means a User job is Running.

Expose automatic activity separately (for example BoardContext.backgroundActive and a
field in lifecycle-status) so a UI may show "Updating..." without blocking. USB config
and reboot still require an Idle worker of either origin; they may preempt an automatic
read to get there.

Local rejections still create no job (kept from the lifecycle milestone).

==================================================
4. CUT THE COST OF AN AUTOMATIC POLL
==================================================

Target: at most 10 requests, none to a second speaker, about 600 ms typical. Report the
before/after request lists and timings.

- Topology source. For routine polls read GetZoneGroupState from the selected room's own
  address (every player serves household topology). Keep SSDP and the separately learned
  discovery host for recovery only (no selected address known, or the selected address
  fails). The topology subscription follows the topology source, and
  SubscriptionLifecycle already resubscribes on an address change; state that
  consequence in the docs.
- Identity GET. Do not fetch /xml/device_description.xml twice per poll. For reads, the
  topology snapshot from the same job already proves (UUID, address); cache the session
  identity per (address, UUID) and revalidate only when the address changes or a read
  would publish for the wrong UUID. GuardedHttp's per-mutation identity GET stays
  exactly as it is.
- One topology read per job. Pass the pickup topology's eligibility into reconcile(), or
  drop ungrouped() from reconcile() while keeping it in prepare(); the mutation path keeps
  its own fresh check.
- Keep the final GetPositionInfo/GetMediaInfo consistency re-read; it is cheap and it is
  why mixed observations never publish.

Do not change the 10 s cadence here; todo/6-subscription-reconciliation.md owns cadence.

==================================================
5. STICK TOGGLE (OWNER DECISION, OFF BY DEFAULT)
==================================================

  [ ] Stick B sends an explicit Play or Pause derived from the current fresh observed
      state, as the Waveshare Play/Pause button already does, instead of toggle's
      refresh-then-decide. Saves about ten requests per press. Changes the documented
      Stick semantics (docs/hardware.md button table, docs/planner-executor.md toggle
      paragraph).
  [x] Keep toggle as it is.

==================================================
6. WI-FI POWER SAVE: MEASURE, THEN DECIDE
==================================================

Add a development switch (USB command or build flag, never config) for
WiFi.setSleep(false) applied after connection. With the tooling, run at least 50
automatic polls each way on the same board and compare per-request latency (median and
p95 from the existing `<action> http=200 ms=` lines), poll duration, and failure count.
Adopt "off while awake" only if it materially helps; the device deep-sleeps on
inactivity, so the battery cost while awake is bounded. Record the decision and the
numbers that justified it in docs/hardware.md as a durable constraint, then remove the
switch or keep it as a documented diagnostic.

==================================================
7. FAILURE INSTRUMENTATION BEFORE ANY TRANSPORT CHANGE
==================================================

In EspHttp::dispatch, on a transport failure log the phase (begin, connect, send,
headers, body), the HTTPClient error string, errno, elapsed ms, and the host, once per
failure, in the existing transition-log style. Keep the last failure (phase, host,
elapsed, when) in the coordinator snapshot so lifecycle-status shows it. Collect at
least one hour unattended with monitor --stats on both boards and summarize failures by
phase in the report. Propose transport changes there; do not make them in this prompt.

==================================================
8. TESTS
==================================================

WorkerLifecycle:

- Preempt on an Automatic job -> Preempted, Idle; Preempt on a User job or while Idle is
  ignored and counted; Preempted never applies to a User job (invariant)

RuntimeCoordinator / fault harness (extend RuntimeFixture; do not fork it):

- rewrite busyWinsOverDiscoveryRecovery into "user input preempts an automatic read":
  toggle, intent, refresh, room-select, and queue-page during an automatic poll are all
  admitted; the automatic job ends Preempted; the user job is Running; no
  SessionFailure, no stale, no refreshError, and no reconciliation from the preemption
- preempted during pickup topology: the discovery generation stays open, the user job
  binds it and completes it
- preempted during the room read: the retained observation is unchanged and not stale
- a user action during a User job is rejected busy
- Sonos Backoff still rejects mutations; a preempted read grants no authority
- chaos invariants: with Sonos usable and only Automatic work Running, a User submit is
  always admitted and reaches Running within the cancellation bound; a Preempted job
  never publishes, never mutates, and never schedules reconciliation; every Preempted
  outcome belongs to an Automatic job
- the liveness window gains a responsiveness metric: the fraction of simulated time in
  which a User submit would be rejected because of Automatic work must be zero

Poll cost:

- a fixture counting requests per automatic job asserts the new bound and that no
  request goes to a second address once a selected room is known
- identity caching cannot publish state for a different UUID (the existing core_test
  destination-identity fixtures must still pass)

Keep node --run check near its current runtime; extend node --run stress seeds if the
chaos model grows.

==================================================
9. AUTONOMOUS DEVICE VERIFICATION
==================================================

With the tooling from todo/1-device-tooling.md, on the identified Stick, then the
admission loop again on the ws-1.8, with sleep disabled for the session and the profile
restored afterward:

1. Baseline before the change: monitor --stats for 10 minutes; record the busy=1 ratio,
   worker transition count, and per-poll durations. Then flash the change.
2. Admission loop: for 10 minutes send `toggle` over `node --run usb` at random offsets
   (0-10 s) against the selected configured room while it is playing; each toggle
   really pauses or resumes it (the default profile permits mutations and the owner
   mutes the amplifier during loops). Assert zero "Busy; input ignored" rejections
   caused by automatic work, record submit-to-Running latency from the log (input
   accepted -> worker Idle -> Running), and report the busy ratio from step 1 rather
   than from this loop, since every user mutation schedules its normal reconciliation
   read. Leave the room in its original transport state afterward.
3. Poll cost: from the log, list the requests of ten automatic polls; assert at most ten
   per poll and none to a second address, and report median/worst duration against the
   baseline.
4. The Wi-Fi experiment from section 6.
5. One hour unattended monitor --stats per board for section 7.
6. Heap from the heartbeat before and after; no downward trend.

==================================================
10. DOCUMENTATION
==================================================

Four sentences state the old contract and must change to "one user job at a time;
automatic reads yield to user input":

- README.md, Commands and diagnostics: "Input during worker activity rejects as busy."
- docs/planner-executor.md, Admission and frozen work: "Input and config changes while
  Running reject rather than replaying later."
- docs/runtime-lifecycles.md, Bounded work and authority: "The deliberate runtime
  contract is one admitted active job..."
- docs/waveshare-frontend.md, Layout and implementation limits: "Busy input rejects
  visibly; it is not silently queued or replayed." (still true for user-vs-user; say so)

Also document in docs/runtime-lifecycles.md: job origins, Preempted, what a preempted
read never does, the automatic-read connect timeout, the topology source rule and its
subscription consequence, identity caching for reads, and the per-poll request bound.
docs/hardware.md gets the Wi-Fi power-save decision and any measured failure
constraint. No diary.

==================================================
11. GREEN BASELINE
==================================================

  node --run format
  node --run format:cpp
  node --run check
  node --run check:full
  node --run stress

Keep VS Code diagnostics green.

==================================================
12. COMPLETION
==================================================

Stop when:

1. a user action during an automatic read is admitted, never rejected
2. a user action during a user action is still rejected visibly
3. a preempted read publishes nothing, stales nothing, and schedules nothing
4. `busy` shown to users means a user job is running
5. an automatic poll is at most ten requests and never touches a second speaker
6. GuardedHttp's per-mutation identity check is unchanged
7. the Wi-Fi power-save decision is made from measurements and recorded
8. transport failures are instrumented by phase and summarized from an unattended run
9. host tests, chaos invariants, and the responsiveness metric cover all of the above
10. the device verification in section 9 ran and its numbers are in the report
11. docs state the new contract
12. all checks/builds pass

Then commit the finished work: todo/README.md "How to run" rules, a `todo 2:`
subject, the README status cell set to `implemented (<hash>)`, only on a green
baseline, never a push. The prompt is not complete with uncommitted work.

Then report only:

- the admission rule and the preemption bound as measured
- before/after poll request lists and durations
- busy ratio and rejection counts before/after
- the Wi-Fi power-save decision and its numbers
- the failure summary by phase and any proposed transport change
- anything todo/3-lvgl.md, todo/5-new-view-model.md, or
  todo/6-subscription-reconciliation.md must know

Do not begin the LVGL or subscription milestones automatically.
