Implement the INTERACTIVE UI STATE MODEL: Observed / Pending / Interaction / ViewModel.

This prompt is self-contained; it is the spec. docs/product.md already states the
principle ("AppState distinguishes observed facts from pending/requested values and
outcomes"); this milestone makes it explicit, deterministic, and testable, and uses the
existing multi-screen shell and Playground for the visible parts.

PHASES

  PHASE A (host-only, portable)
    Sections 1-4, 8-13, 15, and the Phase A tests in 16. No display, no screen, no board
    code. Requires todo/2-user-input-priority.md (job origins, preemption, and the
    user-job meaning of `busy`). It may run in a separate git worktree in parallel with
    todo/3-lvgl.md and Phase A of todo/6-subscription-reconciliation.md.

  PHASE B (device UI)
    Sections 5-7 (Playground), 9 (Now Playing volume adoption), 14, 17, 18, 19, and the
    Phase B tests in 16. Requires todo/4-ws-1.8-multiscreen.md and merged Phase A.

If asked to run only Phase A, stop after its completion items and report.

Do NOT add AVTransport/RenderingControl event subscriptions; the existing topology
subscription stays as it is.
Do NOT implement grouping yet.
Do NOT redesign the entire Now Playing layout.
Do NOT introduce a heavyweight UI framework or a reactive runtime.
Do NOT add historical checkpoint documentation.

Read first: AGENTS.md; docs/product.md (configuration and state); docs/planner-executor.md
(observation and reconciliation; outcomes); docs/runtime-lifecycles.md (bounded work);
docs/sonos-capabilities.md (normalized state); libraries/SurfaceCore/src/SurfaceCore.h
(PlaybackState, AppState, Application) and the Application implementation in
SurfaceCore.cpp; libraries/SurfaceDevice/src/RuntimeCoordinator.h (enqueueJob returns the
job id; finishJob is the terminal outcome), Runtime.cpp (submit, publish, the render
copy under stateMutex), WaveshareUi.h (volumePreview/seekPreview and release-to-submit
are today's interaction state); tests/waveshare_ui_test.cpp, tests/core_test.cpp,
tests/runtime_fault_test.cpp.

==================================================
1. REQUIRED STATE AUTHORITY MODEL
==================================================

Establish explicit conceptual layers:

  ObservedState
  PendingState
  InteractionState
  ViewModel

Do not force these exact class names if the existing architecture suggests cleaner
names.

The required semantics are:

ObservedState
  authoritative facts observed from Sonos

PendingState
  bounded optimistic local mutations awaiting reconciliation

InteractionState
  ephemeral physical interaction currently owning a visible field

ViewModel
  deterministic visible state derived from those layers + monotonic time

Placement (decide here, not per file):

- ObservedState is the existing PlaybackState inside AppState. Do not rename it; add
  only what is missing. The position anchor fields already exist (positionMs,
  observedAtMs, transport, durationMs).
- PendingState and deriveViewModel(observed, pending, interaction, now) are portable
  headers in SurfaceCore, board-agnostic, with no display or Arduino includes, so the
  Stick can consume them later.
- InteractionState lives in the device UI layer and REPLACES WaveshareUi::volumePreview
  and seekPreview. Do not keep two preview mechanisms.

==================================================
2. NO NETWORK CALLBACK -> WIDGET MUTATION
==================================================

Network/Sonos callbacks must update application state.

They must not directly manipulate visible widgets.

Touch handlers update interaction/pending state.

Rendering consumes a derived snapshot.

Conceptually:

  Sonos/network
      -> ObservedState

  local mutation
      -> PendingState

  touch
      -> InteractionState

  all three + monotonicNow
      -> deriveViewModel()
      -> render

Keep locking/ownership simple and explicit. Today the main task copies sharedState under
stateMutex every 100 ms and renders from the copy; derive the ViewModel on the main task
from that copy plus the main task's own InteractionState. Do not derive under the mutex.

==================================================
3. FIELD-LEVEL PRECEDENCE
==================================================

For each visible field:

  active interaction
      >
  valid pending mutation
      >
  observed state
      >
  unknown/loading

This is per-field.

An active seek interaction must not freeze:

- volume
- metadata
- transport
- room/group observations

Likewise a pending shuffle mutation must not freeze unrelated state.

==================================================
4. LOCAL PLAYBACK POSITION
==================================================

Observed playback position must become an anchor:

  observedPositionMs
  observedAtMonotonicMs
  transportState
  durationMs

When playing:

  projectedPosition =
    observedPositionMs +
    elapsed monotonic time since observation

When paused/stopped:

  projectedPosition = observedPositionMs

Clamp appropriately.

This projection belongs to ViewModel.

Do not continuously mutate ObservedState to advance time.

Do not increase Sonos polling merely to make the progress indicator move.

Anchor caveat: DirectSonos::refresh reads RelTime near the start of an ~8-call snapshot
and stamps observedAtMs at the end, so the anchor can be up to about one second late.
Either stamp a position-specific observation time at the GetPositionInfo call (a small
SurfaceSonos change, fixture-tested) or accept the skew and clamp. Choose, and state the
choice in the report.

==================================================
5. PLAYGROUND: PROGRESS CLOCK
==================================================

Use Playground to visibly exercise the local projection.

Show:

- observed position
- projected/visible position
- duration
- transport state

Update visible projected position locally.

It is acceptable to render several times per second for a smooth test.

No additional Sonos requests should result merely from these local UI ticks.

Rendering cost: on the hand-rolled track a moving clock makes every 100 ms frame dirty,
and a full-frame flush costs 43-63 ms of main-task time. Rate-limit clock-only redraws
to at most 2 Hz on that track and measure the poll gap. On the LVGL track only the
label/bar invalidates; keep it that way.

==================================================
6. PLAYGROUND: SEEK DRAG
==================================================

Add a deliberately large seek interaction to Playground.

While finger is down:

  InteractionState owns visible seek position.

The thumb/value must follow the finger immediately.

Incoming observed Sonos position changes must continue updating ObservedState but MUST
NOT move the active thumb.

On release:

  create one seek mutation
  transition visible ownership from InteractionState to PendingState
  send ONE Sonos seek request

Do not continuously seek while dragging.

On authoritative reconciliation:

  clear pending

On failure/timeout:

  clear pending
  visible state returns to observed/projected state

==================================================
7. OPTIMISTIC PLAY/PAUSE
==================================================

Move play/pause through PendingState semantics.

On local request:

  render requested state immediately
  dispatch asynchronously

On confirmation:

  observed state matches
  clear pending

On failure:

  clear pending
  return to observed

Do not make the user wait for HTTP before visual feedback.

Pending identity and lifetime, mapped onto the existing runtime:

- A pending entry is created at successful admission, i.e. when
  RuntimeCoordinator::enqueueJob accepts the job and returns its id. It carries the
  accepted intent's explicit fields (transport, volume, seek, shuffle, repeat) and that
  job id. Local rejections (busy, invalid, room changed, worker unavailable) create no
  pending state.
- It clears on that job's terminal outcome (finishJob). Success: the job's own
  verify/refresh publication is the confirmation. Failure, Timeout, NetworkLost,
  Shutdown, cancellation: clear. Uncertain: clear the optimistic value but keep
  recoveryRequired visible exactly as today.
- A late completion for a different job id never touches it.
- One user job at a time (todo/2-user-input-priority.md): while a user mutation is
  pending, another user tap still rejects as busy, while automatic reads yield to user
  input and never cause a rejection. "Optimistic" means immediate visual feedback, not a
  queue of taps.
- Pending horizon: the mutation job budget is 90 s. Keep the pending value visible until
  the terminal outcome and show the existing "Updating..." affordance; do not add a
  second, shorter timer. If measured behavior makes this feel wrong, propose the change
  in the report rather than adding it silently.

Use Playground to expose/debug this behavior if useful.

Then, if clean, have existing Now Playing consume the same derived transport state.

==================================================
8. OPTIMISTIC SHUFFLE/REPEAT
==================================================

Support the same bounded pending model for shuffle/repeat.

Do not duplicate source validity/policy rules.

Existing MusicIntent/policy validation remains authoritative.

==================================================
9. VOLUME MODEL
==================================================

Make the state model capable of:

  active interaction volume
  pending requested volume
  observed volume

If the existing Now Playing volume interaction can safely adopt it, do so.

Do not redesign volume UI merely for this milestone.

The important test is that an incoming observed volume update cannot move a control under
an active finger.

==================================================
10. METADATA / QUEUE
==================================================

Metadata and queue remain observed facts.

Do not fabricate:

- title
- artist
- album
- artwork
- duration
- queue contents

for optimistic source changes.

A pending source mutation may display a loading/pending state, but authoritative
metadata comes from Sonos.

==================================================
11. PENDING LIFETIME
==================================================

Pending state is bounded.

Every pending field/request must eventually:

- confirm
- fail
- be superseded
- time out/reconcile

Do not create persistent desired state.

Authoritative Sonos eventually wins.

Reuse the existing request/outcome identity: the worker job id from the coordinator and
AppState.requestId/status/detail. Do not invent a second identity.

==================================================
12. EXTERNAL CHANGES DURING INTERACTION
==================================================

Explicitly support cases such as:

- user dragging seek while external volume changes
- user dragging seek while track position observations arrive
- pending pause while external controller changes volume
- pending shuffle while metadata changes

Only the actively owned/pending field gets optimistic precedence.

Everything else should continue updating.

==================================================
13. STALE/OFFLINE
==================================================

Preserve freshness.

Do not let locally projected state imply indefinite authority after Sonos becomes
unreachable.

Pending requests time out/fail appropriately.

On reconnect, fresh authoritative observations replace stale state.

A stale observation (observed.stale, Wi-Fi offline, Sonos recovering) stops the
projection from advancing and is shown as stale, exactly as the current UI marks it.

==================================================
14. SCREEN LIFECYCLE
==================================================

InteractionState owned by a screen must be cancelled when leaving that screen.

Pending accepted Sonos mutations survive screen changes.

ObservedState is global application state.

ViewModel derivation may be shared while screens choose what subset to render.

A room change clears pending and interaction state along with the observation, through
the existing selectObservedRoom path.

==================================================
15. THREADING
==================================================

Avoid holding UI/state locks during:

- HTTP
- SOAP parsing
- artwork downloads
- Sonos waits

Prefer short state snapshot/update critical sections.

Do not introduce an elaborate reactive runtime.

==================================================
16. TESTS
==================================================

Use controlled monotonic time.

Phase A (portable, host-only):

PLAYBACK CLOCK
- playing advances
- paused does not
- duration clamps
- new anchor replaces projection
- stale observation stops projection

SEEK (model level)
- drag owns position
- observed update cannot move active drag
- release creates exactly one pending seek
- pending owns after release
- confirmation clears
- failure returns to observed

TRANSPORT
- optimistic play/pause
- confirmation
- failure
- contradictory authoritative result eventually wins
- a late completion for another job id does not clear pending
- local rejection creates no pending

FIELD INDEPENDENCE
- seek interaction does not freeze volume
- volume interaction does not freeze metadata
- pending shuffle does not freeze transport

STALE/OFFLINE
- pending timeout (job deadline)
- stale observation representation
- reconnect replaces stale state

ROOM CHANGE
- room change clears pending and interaction

Phase B (device UI model):

SCREEN LIFECYCLE
- leaving Playground cancels drag
- pending accepted request survives

UI
- seek sends exactly one request on release (already true today; keep as regression)
- volume under an active finger ignores an observed update

==================================================
17. PLAYGROUND DIAGNOSTICS
==================================================

Make Playground useful for observing this architecture.

A small development-only state display may show things such as:

  observed
  pending
  interaction
  visible

for the currently exercised field.

Mirror the same values to USB by extending the `ui-state` command from
todo/1-device-tooling.md (observed/pending/interaction/visible for transport, position,
and volume, plus the pending job id) so an agent can assert precedence on device.

This is intentionally diagnostic.

Do not spread debug overlays through production UI.

==================================================
18. NOW PLAYING ADOPTION
==================================================

Once the shared model is proven:

Have Now Playing consume the derived state for straightforward existing controls such
as:

- transport
- playback position
- volume where appropriate
- shuffle/repeat if displayed

Do not redesign its layout.

The goal is to stop maintaining two competing UI state architectures.

Stick may consume the derived transport state for its playback line; it is not required
in this milestone.

==================================================
19. AUTONOMOUS DEVICE VERIFICATION (PHASE B)
==================================================

On the identified ws-1.8 port, with the default profile (read_only=false, so injected
taps really mutate the configured room; the owner mutes the amplifier) and sleep
disabled for the session:

1. With the selected room playing and no local interaction, read `ui-state` twice three
   seconds apart: projected position advanced, observed position unchanged between
   polls, and no extra Sonos request in the log.
2. Inject a seek drag (press, moves, hold). Read `ui-state` during the hold and again
   after an observed poll arrives: interaction owns position while observed still
   updates. Release: exactly one seek request appears in the log, pending owns the
   position, and after the job publishes its outcome pending clears.
3. Inject Play/Pause: `ui-state` shows the pending transport immediately; after the
   job's verify publication it clears and observed matches the requested state. Repeat
   once with a read_only=true profile to confirm the blocked outcome clears pending and
   visible returns to observed, then restore the default profile.
4. Inject `ui-nav next` mid-drag: interaction cancelled, nothing sent.
5. Read the heartbeat heap before and after the session.

==================================================
20. GREEN BASELINE
==================================================

Run all canonical checks/builds and preserve editor-green state.

==================================================
21. COMPLETION
==================================================

Phase A stops when items 1, 2, 3, 7, 8, 12, and 13 hold with portable tests only.

Stop when:

1. observed/pending/interaction authority is explicit
2. visible state derives deterministically
3. playback position advances locally
4. seek dragging cannot be disrupted by incoming observations
5. seek sends once on release
6. play/pause is optimistic
7. unrelated fields remain live during interactions
8. pending state is bounded and keyed to the worker job id
9. screen changes cancel interactions but not accepted mutations
10. Playground visibly proves the model and `ui-state` mirrors it
11. Now Playing uses the shared model where practical
12. local rejections create no pending state
13. all tests/builds pass

Then commit the finished work (also when stopping after Phase A): todo/README.md "How to run" rules, a `todo 5:`
subject, the README status cell set to `implemented (<hash>)`, only on a green
baseline, never a push. The prompt is not complete with uncommitted work.

Then report:

- final state ownership model and where each layer lives
- pending reconciliation semantics and the anchor-skew choice
- Playground interactions available for testing
- injected verification results
- anything that should be physically tested before subscriptions

Do not begin Sonos subscriptions automatically.
