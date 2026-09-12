Add EVENT-DRIVEN SONOS STATE SYNCHRONIZATION with periodic polling as reconciliation
and fallback.

The current ObservedState / PendingState / InteractionState / ViewModel architecture is
authoritative.

This milestone changes HOW authoritative Sonos observations arrive.

It must NOT change UI authority semantics.

Do NOT implement grouping mutations yet.
Do NOT make polling the UI clock.
Do NOT build a second subscription mechanism beside the existing one.
Do NOT add historical checkpoint documentation.

WHAT ALREADY EXISTS (extend it)

- libraries/SurfaceDevice/src/SubscriptionLifecycle.h: a Boost.Ext SML machine for ONE
  GENA subscription (Inactive / Subscribing / Healthy / Renewing / Backoff) with a 5 s
  request budget, lease capped at 300 s, renewal at 80 %, 1-30 s backoff, SID validation,
  stale-NOTIFY rejection, a snapshot, and an invariant.
- RuntimeCoordinator.h composes it with the worker, Wi-Fi, and Sonos health machines as
  peers and feeds it NetworkAvailable{address} after discovery and
  NetworkUnavailable/Shutdown on loss.
- Runtime.cpp TopologyEvents: a WiFiServer on port 1401, serviced by the Sonos worker
  task between jobs (100 ms queue wait). It accepts NOTIFY /topology, reads headers only
  (8 KiB, 500 ms), matches the SID through coordinator.notify, replies 200/412, and
  discards the body. Subscribe/renew requests go through JobNetworkClient under the
  subscription deadline. Only the topology subscription exists; NOTIFY bodies are never
  parsed.
- USB lifecycle-status prints the subscription snapshot; transition logs exist.
- tests/subscription_lifecycle_test.cpp (examples + chaos) and
  tests/runtime_fault_test.cpp (composed harness with fake subscription requests and
  NOTIFYs, plus a liveness window).
- docs/runtime-lifecycles.md describes all of it.
- Job origins and preemption from todo/2-user-input-priority.md: automatic reads are
  preemptible by user input and never cause a busy rejection, `busy` means a user job
  is running, routine polls read topology from the selected room's own address, and an
  automatic poll is bounded to at most ten requests. Every automatic read this
  milestone adds must keep those properties.

This milestone generalizes that machinery to N subscriptions and gives NOTIFY bodies
meaning.

PHASES

  PHASE A (runtime, host-first)
    Sections 1-7, 10-16, the lifecycle/observation/fallback/power tests in 19, 21, 22.
    Needs no UI. Requires todo/2-user-input-priority.md. It may run in a separate git
    worktree in parallel with todo/3-lvgl.md and Phase A of todo/5-new-view-model.md.

  PHASE B (UI integration)
    Sections 8, 17, 18, the optimistic tests in 19, and 20. Requires
    todo/5-new-view-model.md (Phase B) and merged Phase A of this prompt.

Read first: AGENTS.md; docs/runtime-lifecycles.md; docs/planner-executor.md (observation
and reconciliation); docs/sonos-capabilities.md (normalized state, observation);
SubscriptionLifecycle.h; RuntimeCoordinator.h; Runtime.cpp (TopologyEvents, the worker
loop, publish); SurfaceSonos.cpp (refresh, parseTopology, the XML helpers);
WriterHttp.h and WriterServer.cpp (bounded HTTP parsing patterns on stick-s3);
ArtworkState.h; the two test files above.

==================================================
1. GOAL
==================================================

Healthy external Sonos changes should normally appear promptly without waiting for a
multi-second poll.

Use Sonos UPnP/GENA event subscriptions where appropriate, especially:

- AVTransport
- RenderingControl

and any other CURRENT service required to keep the existing observed state accurate.

Subscribe only for the SELECTED room's player. The existing ZoneGroupTopology
subscription on the discovery host stays unchanged.

Polling remains a reconciliation/fallback mechanism.

==================================================
2. CALLBACK SERVER
==================================================

Implement the minimum HTTP callback surface required for Sonos NOTIFY events.

Keep it separate conceptually from the NFC writer API even if they share an underlying
HTTP listener.

Concretely: extend the existing port-1401 listener; do not add a server.

- One callback path per subscription instance, for example /topology, /avtransport,
  /rendering, each validated against that instance's own SID and request identity.
  Unknown paths and unknown/expired SIDs reply 412 as today.
- Read the body only for known paths, with an explicit cap (propose 16 KiB; a Sonos
  LastChange NOTIFY is typically 2-6 KiB), the same absolute-deadline discipline as the
  header read, and Content-Length required. Parse only after a complete body.
- The listener is serviced by the worker between jobs, so NOTIFY handling latency is
  bounded by the running job (a refresh is 1-2 s; a mutation can be longer). Accept
  this, measure it, and do not move the listener to another task in this milestone.
- On stick-s3 the writer server (port 80) is a separate WiFiServer; nothing is shared.

Strictly validate:

- method
- callback path/token
- bounded body size
- relevant headers
- known subscription identity

Do not expose a general unauthenticated mutation API.

==================================================
3. SUBSCRIPTION LIFECYCLE
==================================================

Explicitly model:

- subscribe
- subscription ID/SID
- timeout
- renewal
- expiration
- resubscription
- invalidation after Wi-Fi loss
- invalidation after topology/device identity change

Renew before expiry.

After network reconnect:

  discard stale subscription assumptions
  fetch authoritative state
  resubscribe

Concretely: keep SubscriptionLifecycle's transition table as it is and instantiate it N
times (topology on the discovery host; AVTransport and RenderingControl on the selected
room's player), composed by RuntimeCoordinator as peers. No nesting, no new machine type,
no combined state. Each instance keys on (service, target UUID, address).

- A selected-room change, or a changed address for that UUID, resets the AV/RC instances
  (the NetworkUnavailable-style stop) and starts them fresh with the new player's
  address. The old lease is abandoned without a blocking UNSUBSCRIBE, as today.
- Keep the existing request budget, lease cap, renewal fraction, and backoff.
- Extend SubscriptionEffects::request with the service so the adapter knows which event
  URL to use (/MediaRenderer/AVTransport/Event, /MediaRenderer/RenderingControl/Event).
- SUBSCRIBE/RENEW are not Sonos mutations. They bypass GuardedHttp today and continue
  to; read_only does not gate them.
- Both targets share Runtime.cpp, so stick-s3 receives events too. No Stick UI change is
  required.

==================================================
4. EVENT PARSING
==================================================

Normalize incoming event state into the same ObservedState update path used by polling.

Do not create event-specific UI state.

Events may update:

- transport
- metadata
- playback mode
- volume
- mute if supported
- relevant source identity/state

Preserve unknown values rather than inventing defaults.

Concretely: add portable parsers to SurfaceSonos (for example parseAvTransportEvent and
parseRenderingControlEvent). The property set carries an XML-escaped LastChange document:
unescape once, parse with tinyxml2 under the existing bounded-string discipline, and
produce a partial observation containing only the fields present in the event, each
optional.

Merge policy (event -> ObservedState):

- fields present in the event replace; absent fields are retained
- each observation source carries a generation/timestamp, so an older poll result cannot
  overwrite a newer event field and vice versa (section 10)
- a full refresh() remains the authoritative reconciliation and keeps its "content
  changed during read" check
- a track/source identity change from an event resets the position anchor to the event's
  RelTime if present, otherwise to unknown until the next poll; artwork generation
  advances through the existing ArtworkState.select path
- events never reapply an intent and never touch request status/detail

Fixtures: capture real NOTIFY bodies passively on the device (Sonos sends a full-state
initial NOTIFY immediately after every SUBSCRIBE, so no mutation is needed), sanitize
them, and check them in as test fixtures.

==================================================
5. POSITION
==================================================

Do not expect events to provide a UI animation clock.

Continue using the local monotonic playback projection.

Position reconciliation should periodically replace the authoritative position anchor.

Do not poll once per second.

==================================================
6. RECONCILIATION CADENCE
==================================================

Start with an intentionally moderate cadence.

Initial target:

PLAYING:
  authoritative position/state reconciliation approximately every 5 seconds

PAUSED/STOPPED:
  approximately every 15-30 seconds

Use measured behavior to tune.

Do not treat these exact numbers as product constants if an adaptive/simple approach is
cleaner.

Today RuntimeCoordinator::PollIntervalMs (= playbackRefreshIntervalMs, 10 s) drives
automaticJobDue, and one full snapshot costs about ten requests after
todo/2-user-input-priority.md (thirteen before it). A 5 s full snapshot would double
today's background load, so split the two reads:

- position-anchor read: GetTransportInfo + GetPositionInfo only (two requests, no
  topology, no identity), used for the fast PLAYING cadence; it replaces the position
  anchor and transport state and touches nothing else
- full reconciliation snapshot: the existing refresh, used for the slow cadence and
  after any event that changes track/source identity

Make both intervals a function of (AV/RC subscription health, observed transport) inside
the coordinator so the fault harness covers them. No config field. Without healthy AV/RC
subscriptions keep the existing 10 s full-snapshot cadence. Both reads are automatic
jobs and therefore preemptible by user input; the responsiveness invariant from
todo/2-user-input-priority.md must still hold in the fault harness at the new cadence.

The key invariant:

  polling repairs state
  polling does not animate UI

==================================================
7. HEALTH / FALLBACK
==================================================

Track whether subscriptions are healthy.

If events stop, subscription expires, renewal fails, or callback service is unhealthy:

- continue functioning through polling
- retry/resubscribe with bounded backoff
- do not make UI unusable

When subscription health returns, avoid duplicate/conflicting observation paths.

Both event and poll results ultimately update the same authoritative state.

==================================================
8. OPTIMISTIC RECONCILIATION
==================================================

Incoming event confirmation should clear matching PendingState promptly.

Example:

  local Pause
      -> pending paused
      -> SOAP request
      -> AVTransport event paused
      -> ObservedState paused
      -> pending clears

Do not require the next reconciliation poll to clear a mutation already authoritatively
observed.

Contradictory authoritative events must participate in existing bounded conflict
semantics.

An event may clear a pending field only when it carries that field and the value matches
the pending request; it never clears unrelated pending fields and never changes the
worker job's outcome.

==================================================
9. EXTERNAL CONTROLLER TESTS
==================================================

The important product behavior is external control.

While the device is idle/awake, changes made through another Sonos controller should
appear without touching this device:

- play
- pause
- next
- previous
- seek
- volume
- source/track change
- shuffle/repeat where events support them

Healthy event-driven updates should normally be visible within roughly one second.

Do not turn this into a hard timing guarantee.

==================================================
10. EVENT ORDERING / DUPLICATES
==================================================

Assume:

- duplicate events can occur
- polling can race with events
- an older poll may complete after a newer event
- subscription renewal can overlap state refresh

ObservedState updates must not blindly regress newer known state.

Reuse existing request/revision/freshness mechanisms where possible.

At minimum, retain observation timestamps/generations sufficient to avoid an obviously
older asynchronous result overwriting a newer one.

Do not build a distributed event-log system.

==================================================
11. METADATA / ARTWORK
==================================================

Track/source identity changes received from events should invalidate/update metadata
through the existing authoritative state path.

Artwork loading remains asynchronous.

An event must not cause:

  callback thread
      ->
  synchronous artwork download
      ->
  UI stall

Instead:

  event
      ->
  ObservedState source identity changes
      ->
  artwork worker notices generation change
      ->
  fetches asynchronously

Preserve existing late-artwork rejection/generation semantics.

==================================================
12. QUEUE
==================================================

Do not continuously poll queue contents.

If available events indicate queue/source changes:

- invalidate queue freshness
- refresh when currently needed by UI
- or refresh according to the existing bounded queue behavior

A queue screen may justify more eager refresh while visible.

Do not fetch queue on every position or transport event.

==================================================
13. POWER / INACTIVITY
==================================================

Sonos events and reconciliation polls are background activity.

They MUST NOT reset the user's inactivity/sleep timer.

Subscription renewal MUST NOT reset inactivity.

On device sleep:

- stop callback admission cleanly
- discard subscriptions
- shut down networking normally

On wake:

- discover/fetch authoritative state
- establish fresh subscriptions

Do not attempt to preserve subscription state across deep sleep.

==================================================
14. HTTP SERVER COEXISTENCE
==================================================

The device may already expose HTTP for NFC writing.

Already satisfied in this repo: the writer server (port 80, stick-s3 only) and the event
listener (port 1401) are separate WiFiServer instances. Keep them separate.

Reuse shared low-level server infrastructure if that is clean, but keep route ownership
explicit.

Writer routes and Sonos event callback routes have different trust/input models.

Do not:

- let Sonos callback bodies reach writer handlers
- weaken writer Origin/Host protections
- require browser-style Origin for Sonos NOTIFY
- expose event callback routes as arbitrary public state mutation APIs

==================================================
15. MEMORY / BOUNDS
==================================================

Treat incoming event XML as untrusted LAN input.

Use:

- bounded body sizes
- bounded parsed strings
- existing SOAP/XML parsing discipline
- no unbounded retained event history

Do not retain raw NOTIFY bodies after normalization except transiently for diagnostics.

==================================================
16. SUBSCRIPTION DIAGNOSTICS
==================================================

Expose concise development diagnostics for:

- service
- speaker/device identity
- SID
- subscription health
- expiration/renewal
- last event time
- last successful reconciliation
- resubscription/failure reason

Extend the existing USB lifecycle-status output with one entry per subscription
instance; keep exposing SID presence only, never SID contents.

Do not flood normal serial logs with every progress-like event.

Prefer state-transition logging:

  subscribed
  renewed
  expired
  retrying
  recovered

A debug mode may expose more detail if one already exists.

==================================================
17. PLAYGROUND
==================================================

Use the existing Playground to validate the event/state architecture.

Show enough development information to answer:

- what value is Observed?
- what value is Pending?
- what value is Interaction-owned?
- when was last event received?
- when was last reconciliation poll?
- is subscription healthy?

Mirror the same information through `ui-state` (todo/1-device-tooling.md) so an agent
can read it over USB.

Do not turn the Playground into a permanent network-monitor UI.

The purpose is to make state behavior inspectable while developing.

==================================================
18. NOW PLAYING
==================================================

Now Playing should naturally benefit from event-driven observations through the shared
state model.

Do not add separate event handling to Now Playing.

It should simply render updated ViewModel snapshots.

External changes should therefore appear without screen-specific code.

==================================================
19. PORTABLE TESTS
==================================================

Extend tests/subscription_lifecycle_test.cpp and tests/runtime_fault_test.cpp rather than
creating parallel harnesses: N instances, per-instance NOTIFY delivery with bodies, the
cadence function, selected-room resubscription, and the liveness window now requiring
Healthy AV/RC instances as well. Put parser fixtures in core_test or a new
sonos_event_test host target registered in scripts/host-test-targets.ts.

Test subscription lifecycle:

- initial subscription
- valid NOTIFY
- renewal
- expiration
- renewal failure
- bounded retry
- reconnect -> fresh subscribe
- stale SID rejected/ignored appropriately
- selected-room change resubscribes the AV/RC instances and abandons the old lease
- the topology instance is unaffected by AV/RC failures and vice versa

Test observation behavior:

- AVTransport event updates transport
- metadata event changes source identity and advances artwork generation
- RenderingControl event updates volume
- duplicate event is harmless
- older poll cannot overwrite a newer authoritative event where detectable
- an event with a subset of fields retains the others
- oversized or malformed NOTIFY bodies are rejected without state change

Test optimistic integration (Phase B):

- local pending Pause clears from confirming event
- pending seek reconciles from new position
- contradictory event eventually wins
- unrelated event does not clear unrelated pending field

Test fallback:

- no healthy subscription -> polling maintains state at the 10 s cadence
- subscription loss does not break controls
- recovered subscription resumes event-driven behavior and the slower cadence

Test power:

- event does not count as local activity
- renewal does not count as activity
- sleep discards subscription state
- wake starts fresh

==================================================
20. PHYSICAL ACCEPTANCE
==================================================

Autonomous first (no owner, no mutation), on the identified port with sleep disabled for
the session:

1. Flash, wait for READY, and read lifecycle-status until the AV and RC instances are
   Healthy.
2. The initial NOTIFY after SUBSCRIBE carries full state: assert from the log that it
   was parsed and merged and that transport, volume, and track identity match the next
   poll.
3. Wait through one renewal (lease capped at 300 s, renewal at 240 s) and assert the
   transition log.
4. Access-point interruption is not automatable; use the host fault harness for
   reconnect, and on the device only observe that lease expiry recovers.

OWNER DECISION (recorded, see AGENTS.md): the agent MAY perform Sonos mutations against
any room listed in the `rooms` allowlist of the configuration currently flashed to the
device under test (config/default.json unless another profile was flashed), both from
the Mac and through the device itself. Read the allowlist from that device's
`config-status` reply, never from a guessed file. Rooms outside it are never touched.
Prefer bounded, reversible changes and restore the prior state afterward: volume +1 then
-1; pause then resume only a room that was already playing; seek within the current
track; next then previous. The owner mutes the amplifier during long loops. Do not flip
read_only to complete a test; use a read_only=true profile deliberately when mutations
would be disruptive. External-change latency is still measured by driving the speaker
from the host, because "external" means "not this device".

Host tool for this: add scripts/sonos-external.ts exposed as

  node --run sonos:external -- --room DISPLAY_ID [--config PATH] volume +1 | volume -1
      | pause | play | seek 30 | next | previous

It resolves the room through the existing discovery, refuses any room absent from the
profile's `rooms` (default config/default.json), issues exactly one SOAP call, and prints
the resulting observed state. Implement it either as a host C++ target beside
tests/sonos_read.cpp with a mutation-permitted GuardedHttp, or as a small TypeScript SOAP
client; keep scripts/probe.ts permanently read-only either way. Test the allowlist
refusal and the single-call behavior with fakes.

Autonomous external-controller run (after the no-mutation steps above), with the device
awake, no local interaction, and the selected room already playing:

5. For each of volume, pause/resume, seek, and next/previous: issue the change from the
   host tool, then watch the event log and `ui-state`. Record NOTIFY arrival latency and
   observed-state update latency per change type, and restore the prior value.
6. Confirm the local projected clock, artwork transition on next/previous, and the
   absence of any UI interaction corruption from the log.
7. Report median and worst latency per change type.

Owner follow-up, for what the host tool does not cover:

1. start playback of a new source from the official Sonos app
2. verify device updates promptly
3. change shuffle/repeat externally where supported
4. temporarily interrupt Wi-Fi or otherwise break subscriptions:
   - verify polling/reconnect restores state
   - verify subscriptions recover
   - verify no manual refresh is required

Do not mutate household Sonos state outside the flashed allowlist merely for testing.

==================================================
21. DOCUMENTATION
==================================================

Update durable architecture docs with:

- events are preferred prompt observation path
- polling is reconciliation/fallback
- local monotonic projection animates playback position
- subscription lifecycle/reconnect behavior (docs/runtime-lifecycles.md: N instances,
  cadence function, listener body bounds)
- background events do not count as local activity
- docs/sonos-capabilities.md and docs/planner-executor.md no longer say full
  AVTransport/RenderingControl subscriptions are deferred

Do not document:

- temporary SIDs
- one-off event timings
- physical acceptance chronology
- test log names

If measured behavior causes a durable design constraint, document only that constraint.

==================================================
22. GREEN BASELINE
==================================================

Run:

  node --run format
  node --run format:cpp
  node --run check
  node --run check:full
  node --run stress

Keep the C++ editor baseline green.

==================================================
23. COMPLETION
==================================================

Phase A stops when items 1, 2, 3, 5, 7, 9, 12, 13, and 14 hold with host tests and both
firmware builds.

Stop when:

1. AVTransport/RenderingControl changes can arrive through subscriptions
2. subscription lifecycle renews/reconnects safely
3. ObservedState receives both events and reconciliation polls through one path
4. local playback progress still uses monotonic projection
5. polling is no longer the UI animation mechanism
6. healthy external Sonos changes normally appear promptly
7. subscription failure degrades to polling rather than breaking controls
8. events can confirm/clear matching PendingState
9. background events do not affect inactivity
10. Playground and ui-state make subscription/reconciliation state inspectable
11. the autonomous steps in section 20 passed and are logged
12. the existing topology subscription is unchanged
13. N subscription instances share one lifecycle table and one listener
14. all tests/builds pass

Then commit the finished work (also when stopping after Phase A): todo/README.md "How to run" rules, a `todo 6:`
subject, the README status cell set to `implemented (<hash>)`, only on a green
baseline, never a push. The prompt is not complete with uncommitted work.

Then report only:

- services subscribed to
- subscription lifecycle
- reconciliation cadence
- measured external-change responsiveness (or the initial-NOTIFY latency if no owner
  test was possible)
- fallback behavior
- anything that should be resolved before grouping

Do not begin grouping automatically.
