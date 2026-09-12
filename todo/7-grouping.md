Implement the first deliberately narrow Sonos GROUPING capability and its dedicated
Grouping screen.

The current multi-screen shell, interactive UI state model, Sonos subscription/
reconciliation model, policy system, target binding, read_only gate, and sleep behavior
are authoritative.

PRODUCT GOAL:

Support:

1. selected logical room = Kitchen
2. Kitchen is playing
3. add Living Room
4. Living Room joins Kitchen's current playback
5. later NFC/transport/source action while selected room is still Kitchen
6. Kitchen + Living Room continue together
7. later remove Living Room
8. Kitchen remains standalone

Do NOT build a general Sonos administration/group manager.

==================================================
1. GROUP STATE BELONGS TO SONOS
==================================================

Grouping is live Sonos topology state.

Do not persist group membership in:

- device config
- NFC cards
- selected-room preferences
- UI desired state across reboot

Groups remain until Sonos or a controller explicitly changes them.

On boot/wake:

  observe current topology

Do not restore a remembered group.

==================================================
2. SELECTED LOGICAL ROOM VS COORDINATOR
==================================================

Preserve the distinction:

SELECTED LOGICAL ROOM
  the room this controller is conceptually controlling
  source of room policy
  screen/header identity
  grouping anchor

EXECUTION COORDINATOR
  current Sonos group coordinator used for group-level transport/source commands

Example:

  selected = Kitchen
  group coordinator = Living Room
  group members = Living Room + Kitchen

UI still says:

  Kitchen

Policy still uses:

  Kitchen

Transport/source executes through:

  Living Room coordinator

Do not silently change selected room to coordinator.

==================================================
3. GROUPED ROOMS REMAIN SELECTABLE
==================================================

Remove any remaining architectural assumption that a grouped configured room is
unavailable merely because it is grouped.

A configured room may remain the selected logical room while:

- standalone
- coordinator
- non-coordinator group member

Topology determines its current execution target.

==================================================
4. NORMALIZED GROUP TOPOLOGY
==================================================

Expose normalized observed information sufficient to determine:

For each configured room:

- roomDisplayId
- UUID
- availability
- standalone/grouped
- group identity
- coordinator UUID
- current member UUIDs/display IDs
- whether it is eligible to join the selected room's group under this milestone

This belongs to observed topology/state.

Do not put raw Sonos protocol details in UI code.

==================================================
5. SUPPORTED ADD OPERATION
==================================================

Support:

  add a currently STANDALONE configured room
  to the group containing the selected logical room

Examples:

Kitchen standalone
Living Room standalone

  add Living Room
      ->
  Kitchen + Living Room

Or:

Kitchen + Bedroom already grouped
Living Room standalone

  add Living Room
      ->
  Kitchen + Bedroom + Living Room

Resolve the selected room's current actual coordinator from topology.

Do not require selected room to be coordinator.

==================================================
6. UNSUPPORTED CROSS-GROUP ADD
==================================================

If the candidate room already belongs to ANOTHER group:

  reject clearly

Example:

  selected Kitchen group:
    Kitchen + Bedroom

  other group:
    Living Room + Office

Request:
  add Living Room

Result:
  unsupported / already part of another group

Do NOT automatically:

- steal Living Room
- dissolve Living Room's current group
- merge both groups
- delegate coordinators

Those remain explicitly out of scope.

==================================================
7. SUPPORTED REMOVE OPERATION
==================================================

Support removing a non-anchor member from the selected room's group.

Example:

  selected = Kitchen

  Kitchen + Living Room + Bedroom

Remove Living Room:

  Kitchen + Bedroom remain grouped
  Living Room becomes standalone

For a two-room group:

  Kitchen + Living Room

Remove Living Room:

  both become standalone naturally

==================================================
8. ANCHOR CANNOT BE REMOVED
==================================================

The selected logical room is the user's group anchor.

Do not allow the simple grouping UI/API to remove it from its own group.

If selected = Kitchen:

  remove Kitchen

is invalid.

Avoid implementing coordinator delegation / anchor replacement in this milestone.

==================================================
9. GROUP-AWARE TRANSPORT
==================================================

Once selected logical room is grouped, existing group-level operations should execute
against its current coordinator.

At minimum:

- source replacement
- play
- pause
- next
- previous
- seek
- shuffle
- repeat
- queue selection

Cards do NOT alter grouping.

Example:

  selected = Kitchen
  Kitchen + Living Room grouped
  tap playlist card

Result:

  playlist starts on existing Kitchen + Living Room group

Do NOT auto-ungroup.

==================================================
10. VOLUME REMAINS ROOM-LOCAL
==================================================

Do not add group-volume behavior.

Volume UI on a controller selected to Kitchen changes Kitchen's individual volume even
when Kitchen is grouped.

Do not route room-local volume through coordinator.

If mute exists as room-local state, retain the same principle.

==================================================
11. POLICY REMAINS SELECTED-ROOM LOCAL
==================================================

Resolve policy from selected logical room.

Do not use:

- coordinator's room policy
- merged group policy
- every member's policy

Example:

  selected Kitchen:
    playlist.shuffle=true

  coordinator Living Room:
    playlist policy irrelevant

Tap playlist:

  resolve shuffle=true from Kitchen
  execute result against current group coordinator

==================================================
12. TOPOLOGY / EVENT INTEGRATION
==================================================

Grouping changes made from the official Sonos app must naturally update observed state.

Example:

1. selected = Kitchen
2. official app groups Kitchen + Living Room
3. subscription/topology observation updates
4. Grouping screen shows Living Room as grouped
5. next card controls the group

Likewise external ungroup:

1. external controller ungroups
2. observed topology updates
3. selected room stays Kitchen
4. next card controls Kitchen standalone

Do not require grouping changes to originate here.

==================================================
13. REQUEST SAFETY / RACES
==================================================

A grouped intent has:

  logical identity:
    selected room UUID

and:

  execution identity:
    current verified coordinator UUID

Do not blindly freeze a coordinator indefinitely if topology changes before dispatch.

Before group-level mutation:

- verify selected logical room still resolves
- verify its current group/coordinator
- verify coordinator destination identity
- ensure the request cannot redirect to an unrelated group

If topology changed incompatibly:

  fail/reconcile

rather than targeting a surprising destination.

Keep this proportional; do not build a distributed transaction system.

==================================================
14. READ_ONLY
==================================================

Group join/remove are Sonos mutations.

Therefore:

  read_only=true

blocks grouping mutations through the normal shared mutation gate.

Observation/group display remains available.

Do not introduce a group-specific mutation flag.

==================================================
15. GROUPING SCREEN UX
==================================================

Use the dedicated Grouping screen.

The current display is a proving ground, so prioritize:

- large touch targets
- explicit state
- minimal interaction ambiguity

Do NOT try to show many tiny checkboxes simultaneously.

A good model is one candidate room at a time:

  GROUP: Kitchen

  Living Room

     [ ADD ]

       1 / 3

or:

  GROUP: Kitchen

  Living Room

    [ REMOVE ]

       1 / 2

Provide simple large Previous/Next candidate navigation if needed.

The selected anchor room should be visibly identified and non-removable.

Candidate states should distinguish:

- already in selected group
- standalone / can add
- in another group / unavailable
- offline/unavailable

Do not use tiny global swipe navigation.

Touch gestures remain local to this screen.

==================================================
16. APPLY MODEL
==================================================

For the first implementation, choose whichever interaction is simpler and safer:

A. immediate ADD/REMOVE per room

or

B. local desired-membership editing + explicit Apply

Prefer immediate ADD/REMOVE if it significantly reduces temporary desired-state
complexity.

If using Apply:

- desired membership exists only as InteractionState
- compute supported diff against current observed topology
- topology remains authoritative
- cancel screen interaction on external incompatible changes

Do not persist desired grouping.

==================================================
17. OPTIMISTIC UI
==================================================

Use the current PendingState/ViewModel architecture.

A local group mutation may show immediate pending feedback:

  Adding...
  Removing...

But do not fabricate final topology indefinitely.

Observed Sonos topology confirms the result.

On failure:

  clear pending
  return to observed group state
  show concise failure

External topology remains authoritative.

==================================================
18. PLAYGROUND
==================================================

Use Playground where helpful to expose:

- normalized group
- selected room
- coordinator
- candidate eligibility
- pending group operation

Do not make Playground required for normal grouping use.

==================================================
19. SCREEN NAVIGATION
==================================================

Screen switching must not alter group membership.

Leaving Grouping screen:

- cancel local candidate-navigation/interaction state
- retain already accepted pending Sonos mutation normally

Do not dissolve or modify groups on screen exit.

==================================================
20. SLEEP
==================================================

Sleep does not alter Sonos grouping.

Group events do not reset inactivity.

On wake:

  fresh topology observation
  current real groups appear

Do not restore pre-sleep group state.

==================================================
21. SONOS ADAPTER
==================================================

Implement only the group protocol actions needed for:

- joining a standalone room to selected room's current group
- making a non-anchor member standalone

Keep:

- x-rincon URI details
- AVTransport grouping SOAP calls
- coordinator mechanics

inside the Sonos adapter.

UI/shared policy code should operate on normalized operations/results.

Do not add unsupported coordinator-management operations speculatively.

==================================================
22. TEST MATRIX
==================================================

TOPOLOGY

- grouped configured room remains selectable
- coordinator normalized correctly
- members normalized correctly
- external topology changes update observed state

JOIN

- standalone -> selected standalone
- standalone -> selected existing group
- already same group -> harmless/no-op
- room in other group -> reject
- read_only -> reject

REMOVE

- non-anchor member becomes standalone
- other members remain grouped
- two-room group dissolves to standalone
- anchor removal rejects
- read_only rejects

TRANSPORT

- selected standalone -> itself
- selected coordinator -> coordinator
- selected non-coordinator -> coordinator
- card does not alter grouping
- subsequent card continues existing group

POLICY

- selected-room policy used independent of coordinator

VOLUME

- selected room volume remains local

RACES

- coordinator change resolves safely
- selected room leaving expected group before dispatch does not redirect to unrelated
  group

UI

- candidate eligibility displayed correctly
- pending add/remove
- failure reconciliation
- external grouping change while screen open
- leaving screen cancels only local interaction

SLEEP

- group state not mutated on sleep
- wake uses newly observed topology

==================================================
23. PHYSICAL ACCEPTANCE
==================================================

Start mutation testing only under normal read_only semantics.

Use the exact real household flow:

1. select Kitchen
2. Kitchen standalone playing
3. open Grouping
4. add Living Room
5. verify synchronized playback
6. return to Now Playing
7. tap NFC card / Next / Play-Pause
8. verify both rooms remain together and respond
9. return to Grouping
10. remove Living Room
11. verify Kitchen standalone

Then external-controller test:

1. official Sonos app groups rooms
2. device observes it
3. UI reflects it
4. next card controls observed group
5. official app ungroups
6. device observes it without refresh

==================================================
24. DOCUMENTATION
==================================================

Document current grouping semantics:

- group state belongs to Sonos
- selected logical room differs from coordinator
- cards never contain grouping
- policy uses selected room
- source/transport are group-level
- volume remains room-local
- supported join/remove subset
- cross-group merge/anchor removal unsupported

Do not add acceptance chronology.

==================================================
25. SCOPE LIMITS
==================================================

Do NOT implement:

- merging two existing groups
- stealing rooms from another group
- arbitrary coordinator delegation
- removing selected anchor
- saved group presets
- automatic whole-house grouping
- group volume
- group membership in cards
- group membership in config
- group timeout
- automatic group restoration

==================================================
26. GREEN BASELINE
==================================================

Run all canonical formatting/check/build tasks and keep editor diagnostics green.

==================================================
27. COMPLETION
==================================================

Stop when:

1. grouped configured rooms remain usable logical targets
2. selected room/coordinator distinction is explicit
3. standalone rooms can join selected group
4. non-anchor rooms can leave
5. cross-group merging is rejected
6. existing source/transport controls follow current group
7. volume stays room-local
8. policy stays selected-room-local
9. external Sonos group changes appear automatically
10. Grouping screen provides usable large-target controls
11. sleep/read_only semantics remain correct
12. all checks/builds pass

Then commit the finished work: todo/README.md "How to run" rules, a `todo 7:`
subject, the README status cell set to `implemented (<hash>)`, only on a green
baseline, never a push. The prompt is not complete with uncommitted work.

Then report:

- normalized group model
- exact supported operations
- exact unsupported cases
- Grouping-screen interaction chosen
- physical acceptance results/remaining test

Do not begin another milestone automatically.