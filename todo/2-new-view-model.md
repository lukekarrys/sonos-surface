Implement the INTERACTIVE UI STATE MODEL described by the current UI-state TODO/spec.

This milestone establishes frontend-like declarative state semantics using the existing
multi-screen shell and Playground.

Do NOT implement Sonos event subscriptions yet.
Do NOT implement grouping yet.
Do NOT redesign the entire Now Playing layout.
Do NOT introduce a heavyweight UI framework.
Do NOT add historical checkpoint documentation.

Read the current UI-state TODO/spec and treat it as the product/architecture target.

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

Keep locking/ownership simple and explicit.

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

Reuse existing request/outcome identity semantics where possible.

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

==================================================
14. SCREEN LIFECYCLE
==================================================

InteractionState owned by a screen must be cancelled when leaving that screen.

Pending accepted Sonos mutations survive screen changes.

ObservedState is global application state.

ViewModel derivation may be shared while screens choose what subset to render.

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

Test:

PLAYBACK CLOCK
- playing advances
- paused does not
- duration clamps
- new anchor replaces projection

SEEK
- drag owns position
- observed update cannot move active drag
- release sends exactly one seek
- pending owns after release
- confirmation clears
- failure returns to observed

TRANSPORT
- optimistic play/pause
- confirmation
- failure
- contradictory authoritative result eventually wins

FIELD INDEPENDENCE
- seek interaction does not freeze volume
- volume interaction does not freeze metadata
- pending shuffle does not freeze transport

SCREEN LIFECYCLE
- leaving Playground cancels drag
- pending accepted request survives

STALE/OFFLINE
- pending timeout
- stale observation representation
- reconnect replaces stale state

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

==================================================
19. GREEN BASELINE
==================================================

Run all canonical checks/builds and preserve editor-green state.

==================================================
20. COMPLETION
==================================================

Stop when:

1. observed/pending/interaction authority is explicit
2. visible state derives deterministically
3. playback position advances locally
4. seek dragging cannot be disrupted by incoming observations
5. seek sends once on release
6. play/pause is optimistic
7. unrelated fields remain live during interactions
8. pending state is bounded
9. screen changes cancel interactions but not accepted mutations
10. Playground visibly proves the model
11. Now Playing uses the shared model where practical
12. all tests/builds pass

Then report:

- final state ownership model
- pending reconciliation semantics
- Playground interactions available for testing
- anything that should be physically tested before subscriptions

Do not begin Sonos subscriptions automatically.