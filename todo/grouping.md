Add the first deliberately narrow Sonos GROUPING capability.

PRODUCT GOAL

Support this exact household workflow:

1. Device is selected to Kitchen.
2. Tap an NFC card -> music plays in Kitchen.
3. Later add Living Room so it joins what Kitchen is currently playing.
4. Kitchen + Living Room remain grouped.
5. Later tap another NFC card while the device is still selected to Kitchen.
6. The new source plays on the EXISTING Kitchen + Living Room group.
7. Later remove Living Room and return to Kitchen standalone.

Group membership is persistent live Sonos household state.

It is NOT:
- stored on NFC cards
- part of MusicIntent
- automatically removed after playback
- time based
- automatically recreated from device config

Do NOT build a general Sonos grouping manager.

Read the current topology, target-binding, policy, planner/executor, Sonos adapter,
read_only, and UI architecture before changing anything.

==================================================
1. CORE MODEL: LOGICAL ROOM VS EXECUTION TARGET
==================================================

Introduce/clarify the distinction between:

SELECTED LOGICAL ROOM

The room the physical controller is set to.

Example:

  Kitchen

This remains:
- the room shown as selected in UI
- the roomDisplayId used for policy context
- the anchor for grouping UI
- persisted/preferred according to existing semantics

EXECUTION TARGET

For group-level Sonos transport/source operations:

  selected room standalone
      -> selected room itself

  selected room grouped
      -> current group's coordinator

The selected room must NOT silently change merely because it joins a group or is not
the coordinator.

Example:

  selected room = Kitchen

  live group:
    Living Room = coordinator
    Kitchen = member

The physical controller still says:

  Kitchen

but group-level transport/source commands execute against the verified Living Room
coordinator.

Do not expose coordinator identity as the primary user-facing room.

==================================================
2. GROUPED ROOMS ARE NO LONGER GENERALLY UNAVAILABLE
==================================================

The current implementation intentionally excludes grouped rooms from selectable
targets.

That rule must now change.

A configured room may remain selectable while grouped.

Topology should expose enough normalized information for each configured room to know:

- stable room UUID
- room name/displayId
- standalone vs grouped
- group ID
- coordinator UUID
- group members
- whether the room is currently controllable under this project's supported grouping
  subset

Do NOT interpret "grouped" by itself as unavailable.

A configured selected room can remain the logical target while grouped.

==================================================
3. CURRENT GROUP STATE IS AUTHORITATIVE
==================================================

Sonos topology owns group state.

Do not persist desired/current group membership in device config.

Do not assume a group created by sonos-surface remains unchanged.

The official Sonos app or another controller may:

- add a room
- remove a room
- dissolve a group
- change coordinator
- create another group

Topology refresh/event observation must replace our understanding with observed Sonos
state.

Group membership is not desired state that we continuously enforce.

==================================================
4. MINIMUM GROUP MUTATIONS
==================================================

Implement only the minimum operations necessary for:

  add standalone room to selected room's current group

and:

  remove non-anchor room from selected room's current group

Conceptually expose application operations such as:

  addRoomToSelectedGroup(roomDisplayId)

  removeRoomFromSelectedGroup(roomDisplayId)

Exact API naming is flexible.

These are NOT MusicIntent fields.

Grouping is controller/application state management, separate from declarative music
intent.

==================================================
5. ADD ROOM
==================================================

Initial supported add operation:

Given:

  selected logical room = Kitchen

Current topology:

  Kitchen standalone
  Living Room standalone

Request:

  add Living Room

Desired result:

  Kitchen + Living Room grouped
  current Kitchen playback continues
  Living Room joins/synchronizes to Kitchen's current group playback

If Kitchen is ALREADY grouped:

  Kitchen + Bedroom

and Living Room is standalone:

  add Living Room

Desired result:

  Kitchen + Bedroom + Living Room

The joining room should join the CURRENT GROUP containing the selected logical room.

Resolve that group's actual coordinator from live topology.

Do not require the selected room itself to be coordinator.

==================================================
6. INITIAL ADD CONSTRAINTS
==================================================

For this first slice, a room may be added only if it is currently standalone.

If the requested room is already part of the selected room's group:

  no-op / already grouped

If it belongs to some OTHER existing group:

  reject with a clear unsupported/conflict result

Do NOT automatically:

- steal a room from another group
- merge two existing groups
- dissolve the other group
- delegate coordinators
- reorganize arbitrary topology

Example:

  Kitchen + Bedroom

  Living Room + Office

Selected = Kitchen.

Request:

  add Living Room

Result:

  REJECT:
  Living Room is already in another group.

This can be expanded later only if there is a real household requirement.

==================================================
7. REMOVE ROOM
==================================================

Initial supported removal:

Selected logical room = Kitchen.

Current group:

  Kitchen
  Living Room
  Bedroom

Request:

  remove Living Room

Desired:

  Kitchen + Bedroom remain grouped
  Living Room becomes standalone

Likewise removing Bedroom is supported.

The selected logical room is the GROUP ANCHOR from the user's perspective.

Do not allow the simple grouping operation to remove the selected room itself.

If:

  selected = Kitchen

then:

  remove Kitchen

is invalid through this API/UI.

This avoids coordinator handoff / ambiguous anchor semantics in the first implementation.

==================================================
8. DISSOLVING A TWO-ROOM GROUP
==================================================

Given:

  selected = Kitchen

Group:

  Kitchen + Living Room

Request:

  remove Living Room

Result:

  Kitchen standalone
  Living Room standalone

This is the normal supported way to dissolve the simple group.

Do not require a separate "ungroup all" operation for this milestone.

==================================================
9. COORDINATOR COMPLEXITY
==================================================

Do not assume the selected logical room is coordinator.

Topology determines coordinator.

Joining should target the actual current coordinator appropriately.

For removal, use the simplest Sonos operation that makes the REMOVED room standalone
without unnecessarily changing the remaining group.

If a requested operation would require:

- coordinator delegation
- BecomeGroupCoordinator...
- merging existing groups
- removing the selected anchor/coordinator while preserving others
- another materially more complicated coordinator transition

reject it as unsupported in this milestone.

Do not implement those operations speculatively.

==================================================
10. GROUP-AWARE MUSIC EXECUTION
==================================================

This is the most important behavioral change.

When an accepted MusicIntent has a selected logical room:

1. freeze selected logical room identity/context
2. resolve policy using that SELECTED ROOM
3. inspect sufficiently fresh topology
4. determine the selected room's current group
5. resolve the group's current coordinator
6. execute GROUP-LEVEL operations against that coordinator

This means:

  selected = Kitchen

  Kitchen + Living Room grouped

  tap album card

must replace/play the album across Kitchen + Living Room.

Do NOT automatically ungroup Kitchen before playing the card.

Do NOT treat the grouped Kitchen as unavailable.

==================================================
11. REQUEST BINDING + TOPOLOGY RACES
==================================================

Preserve the project's strong target-binding semantics, but adapt them for groups.

There are now two identities:

  logical policy/interaction target
      = selected room UUID

  network execution target for group transport
      = verified current coordinator UUID

Do not simply freeze a coordinator forever at NFC acceptance if topology may change
before execution.

Design the minimum safe binding semantics.

Required invariant:

A topology change must NEVER cause an accepted Kitchen intent to execute against an
unrelated group.

Before group-level mutation:

- verify the selected logical room still belongs to the expected current group context
- resolve/verify its coordinator
- verify destination identity at HTTP dispatch as today

If topology changed incompatibly between acceptance and execution:

  fail/reconcile

rather than redirecting the request somewhere surprising.

Do not build a distributed transaction system.

Keep the safety rule understandable and testable.

==================================================
12. WHICH OPERATIONS ARE GROUP-LEVEL
==================================================

For this milestone treat these as group-level / coordinator-targeted:

- source replacement
- play
- pause
- next
- previous
- seek
- queue selection
- shuffle
- repeat

Use actual Sonos semantics where one of these differs, but document the durable
exception.

QUEUE

Queue observation/selection for a selected grouped room should reflect the active
group/coordinator queue.

The UI/application should not display a stale standalone queue for Kitchen while
Kitchen is currently playing Living Room's coordinated group queue.

==================================================
13. VOLUME STAYS ROOM-LOCAL
==================================================

Do NOT add group volume in this milestone.

Existing volume controls should continue to mean:

  volume of the SELECTED LOGICAL ROOM

Example:

  selected = Kitchen
  group = Kitchen + Living Room

Volume +/-/slider on the Kitchen controller changes Kitchen's individual volume.

It does NOT change Living Room volume.

This distinction should be explicit in code/tests.

Do not route individual volume through the group coordinator merely because other
transport operations are coordinator-targeted.

Mute, if currently supported as room-local state, should follow the same principle.

==================================================
14. POLICY CONTEXT STAYS SELECTED-ROOM LOCAL
==================================================

Policy is resolved using the SELECTED LOGICAL ROOM.

Do not use:

- coordinator room policy
- every group member's policy
- merged policies

Example:

  selected = Kitchen
  Kitchen policy: playlist.shuffle=true
  coordinator happens to be Living Room

Tap playlist card:

  policy context = Kitchen
  shuffle=true

The resolved result then applies to the active group.

This preserves deterministic behavior when coordinator identity changes.

==================================================
15. READ_ONLY
==================================================

Grouping operations are Sonos mutations.

Therefore:

  read_only=true

must block:

- joining rooms
- removing rooms
- any other group mutation

while still allowing:

- topology observation
- displaying current groups
- calculating proposed grouping changes
- diagnostics

Do not create another grouping-specific safety flag.

==================================================
16. GROUP UI MODEL
==================================================

Add the minimum shared/application representation needed for a UI to show:

  selected logical room
  current group members
  available standalone configured rooms that could join

Do not put screen coordinates/layout into shared code.

The intended future interaction is approximately:

  Kitchen

  Playing in:
    ✓ Kitchen
    ✓ Living Room
    □ Office
    □ Bedroom

           Apply

But do NOT require this exact visual implementation.

The shared layer should make that UI straightforward.

==================================================
17. WS-1.8 UI
==================================================

Add a SIMPLE grouping interaction to the existing ws-1.8 only if it can fit without
destabilizing the accepted UI.

This is a prototype/personal device, so compact/quirky is acceptable.

Preferred entry point:

  room selector / room header
      -> Group / Playing In action

or another obvious touch path.

The grouping view should show only configured rooms relevant to this device.

Represent:

- selected anchor room
- rooms currently in its group
- standalone rooms eligible to join
- unavailable rooms already in another group

The selected anchor should always remain checked/non-removable.

Allow user to choose desired membership and Apply.

On Apply:

- compute minimal supported diff
- remove eligible current members first/appropriately
- add eligible standalone rooms
- reject unsupported cross-group operations clearly

Do not build drag/drop or complex group management.

If the 1.8 screen makes this genuinely unreasonable, implement the shared capability
and a serial/test surface first, then report that UI should wait for larger hardware.

Do not compromise core grouping semantics merely to fit the small display.

==================================================
18. STICK-S3 UI / BUTTONS
==================================================

Do NOT overload existing Stick button gestures with complex group management.

Stick should become GROUP-AWARE:

- selected room remains selectable while grouped
- NFC card controls selected room's current group
- display may indicate grouped state compactly

But actual group creation/removal does not need a button-only UX in this milestone.

Serial/debug commands may exercise group operations for hardware testing.

Preserve existing room-switch button behavior.

==================================================
19. EXTERNAL SONOS APP CHANGES
==================================================

Explicitly test/reason about this flow:

1. sonos-surface selected = Kitchen
2. Sonos app groups Living Room with Kitchen
3. sonos-surface observes topology change
4. selected logical room remains Kitchen
5. next NFC card controls Kitchen's now-current group

And:

1. Kitchen + Living Room grouped
2. Sonos app ungroups them
3. sonos-surface observes topology
4. selected logical room remains Kitchen
5. next NFC card controls Kitchen standalone

Do not require group changes to originate from sonos-surface.

==================================================
20. GROUP PERSISTENCE
==================================================

Do not implement a group timeout.

Do not automatically dissolve groups when:

- playback stops
- a track ends
- a new NFC card is tapped
- device sleeps
- controller reboots
- controller switches selected room

Group state belongs to Sonos and persists until explicitly changed by some controller
or Sonos behavior.

On wake/reboot:

  rediscover topology
  -> observe whatever groups currently exist

Do not attempt to restore a remembered group.

==================================================
21. SLEEP INTERACTION
==================================================

Preserve the current power-management work.

Device sleep does NOT dissolve groups.

Example:

  Kitchen + Living Room grouped
  controller sleeps
  Sonos keeps playing
  controller wakes later
  topology discovers Kitchen + Living Room still grouped
  controller resumes controlling that group

Background topology/group events do not count as local user activity for the
inactivity timer.

Do not make group membership keep the controller awake.

==================================================
22. SONOS ADAPTER
==================================================

Add only the Sonos AVTransport/group operations actually needed for this narrow slice.

Keep raw SOAP/group protocol details inside SurfaceSonos.

Normalize results/errors above that layer.

Do not expose x-rincon URI construction or coordinator-specific SOAP details to UI,
NFC, or policy code.

Reuse topology state already available rather than performing redundant discovery for
every operation.

==================================================
23. TEST MATRIX
==================================================

Add portable tests for at least:

TOPOLOGY

- standalone configured room remains selectable
- grouped configured room remains selectable
- group coordinator represented correctly
- group membership represented correctly

JOIN

- standalone room joins selected standalone room
- standalone room joins selected room's existing group
- already-in-same-group is no-op
- room in another group rejects
- read_only blocks join

REMOVE

- non-anchor member becomes standalone
- remaining group stays intact
- removing last non-anchor dissolves two-room group naturally
- removing selected anchor rejects
- read_only blocks removal

EXECUTION TARGET

- standalone selected room -> itself
- grouped selected room -> coordinator
- selected non-coordinator -> coordinator
- coordinator changes but selected room remains same
- incompatible topology race fails rather than targeting unrelated group

POLICY

- selected room policy used when selected room is coordinator
- selected room policy still used when another room is coordinator
- coordinator policy never substitutes for selected-room policy

CARDS

- tapping card does not dissolve current group
- subsequent card continues on same group
- same card standalone controls one room
- same card grouped controls group

VOLUME

- selected-room volume remains room-local
- volume is not redirected to coordinator
- group membership does not cause group-volume mutation

QUEUE

- grouped selected room observes coordinator/current group queue
- queue selection targets coordinator
- topology change invalidates stale queue state

EXTERNAL CHANGES

- external join updates topology
- external ungroup updates topology
- selected logical room survives both

SLEEP

- sleep request does not mutate grouping
- wake uses newly observed topology
- group event does not reset inactivity timer

==================================================
24. REAL SONOS TESTING
==================================================

Start with read_only=true.

Autonomously validate:

- topology/group parsing
- coordinator identification
- current household groups
- proposed join/remove planning
- group-aware execution-target resolution

Do not change existing groups while read_only=true.

When mutation testing is ready, stop and give me a minimal physical test using two
rooms I explicitly choose.

Suggested sequence after I enable mutations:

1. Kitchen standalone playing something
2. add Living Room
3. verify synchronized playback
4. tap a new NFC card with selected room still Kitchen
5. verify both continue together on new source
6. remove Living Room
7. verify Kitchen remains standalone/current
8. optionally make the same group from official Sonos app
9. verify sonos-surface follows it correctly

Do not manipulate unrelated household groups autonomously.

==================================================
25. DOCUMENTATION
==================================================

Update durable docs for the CURRENT model.

Document:

- selected logical room vs group coordinator
- group state belongs to Sonos topology
- groups persist until explicitly changed
- cards never contain grouping
- policy uses selected logical room
- transport/source is group-level
- volume remains selected-room local
- supported grouping subset
- unsupported cross-group/coordinator cases

Update docs/hardware.md only for physical UI/button behavior if this milestone actually
changes those semantics.

Do not add test checkpoints/history.

==================================================
26. SCOPE LIMITS
==================================================

Explicitly out of scope:

- merging two existing groups
- stealing rooms from unrelated groups
- arbitrary coordinator delegation
- removing selected anchor while preserving group
- group volume
- saved/preset groups
- group membership on NFC cards
- group membership in device config
- automatic group restoration
- group timeout
- whole-house "group everything" shortcut
- voice grouping
- general Sonos administration

Implement the smallest useful grouping model first.

==================================================
27. GREEN BASELINE
==================================================

Run the repository's current canonical green checks.

At minimum:

  node --run format
  node --run format:cpp
  node --run check
  node --run check:full

Ensure the C++ editor compilation database remains valid after any shared-code changes.

==================================================
28. COMPLETION
==================================================

Stop when:

1. grouped configured rooms remain selectable
2. selected logical room remains distinct from coordinator
3. standalone rooms can join selected room's group
4. non-anchor members can leave
5. unrelated existing groups are never merged automatically
6. card/source/transport operations follow selected room's current coordinator
7. cards never alter grouping
8. policy remains selected-room based
9. volume remains selected-room local
10. external Sonos grouping changes are observed
11. sleep/reboot does not dissolve or restore groups
12. read_only blocks grouping mutations
13. tests/builds are green

Then give me:

- final normalized group model
- exact supported join/remove cases
- exact unsupported cases
- how coordinator resolution is made safe
- whether ws-1.8 received grouping UI or that was deferred
- concise physical test procedure

Do not begin