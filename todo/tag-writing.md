Implement the NFC TAG WRITER + PHONE-FRIENDLY LOCAL WEB UI milestone.

The current intent/policy, multi-room, grouping, read_only, power-management, and
hardware-target architecture are authoritative.

The first physical writer target is:

  stick-s3 + its existing ST25R3916 NFC reader

Do NOT implement NFC writing on ws-1.8.
Do NOT add 4.3-inch hardware support.
Do NOT add voice.
Do NOT add an iOS app.
Do NOT add a server dependency.
Do NOT add migration/backward-compatibility infrastructure.
Do NOT add historical checkpoint documentation.

Read the current repository, AGENTS.md, intent/policy docs, NFC implementation,
power-management behavior, and current config/tooling before changing anything.

==================================================
1. PRODUCT GOAL
==================================================

Replace the old phone/Shortcut NFC-writing workflow with a self-contained household
workflow.

The Stick should serve a small local web page reachable from a phone on the same LAN.

Primary workflow:

1. open writer page on iPhone
2. paste an Apple Music share URL
3. optionally choose explicit shuffle/repeat behavior
4. tap "Write card"
5. Stick becomes explicitly armed
6. Stick display says to present a card
7. present NFC card
8. write canonical structured intent
9. read card back
10. parse through the NORMAL card parser
11. verify semantic equality
12. show success/failure on both device and browser
13. automatically leave writer mode

Also support:

1. choose "Read / Edit card"
2. present existing card
3. do NOT execute it
4. decode through normal NFC/parser path
5. populate the web editor
6. modify simple fields
7. write/rewrite
8. verify

No cloud service.
No Mac.
No Synology.
No iOS native application.

==================================================
2. CARD SEMANTICS
==================================================

Cards remain portable declarative MusicIntent inputs.

Cards MUST NOT contain:

- selected room
- roomDisplayId
- Sonos UUID
- group membership
- coordinator
- device profile
- read_only
- sleep config
- device policy
- room policy
- resolved policy values unless explicitly selected by the card author

Grouping must remain completely external to cards.

The same card can later be tapped while:

  Kitchen standalone

or:

  Kitchen + Living Room grouped

and normal runtime topology determines where playback executes.

==================================================
3. SIMPLE WRITER UI FIELDS
==================================================

The SIMPLE writer UI exposes only:

SOURCE
- Apple Music share URL

SHUFFLE
- Default
- On
- Off

REPEAT
- options depend on normalized source kind

No transport control is shown.

No volume control is shown.

No room/group control is shown.

No advanced JSON fields are shown in the primary UI.

==================================================
4. TRANSPORT IS ALWAYS PLAY
==================================================

Family-authored music cards always mean:

  play this source

The simple writer therefore always produces the canonical equivalent of:

  transport = play

The user must never be asked to choose:

- play
- pause
- preserve transport
- queue without playing

Do not remove generic transport support from MusicIntent.

This restriction applies only to the simple writer UI/card-authoring workflow.

==================================================
5. VOLUME REMAINS POWER-USER CAPABILITY
==================================================

Volume remains valid in the underlying structured card/MusicIntent schema.

Do NOT remove it.

But the normal writer UI must not expose:

- absolute volume
- relative volume

Cards in actual household use are not expected to control volume.

Advanced/manual card payloads may still contain volume.

==================================================
6. SOURCE-SPECIFIC WRITER OPTIONS
==================================================

After the Apple Music URL is normalized/classified, adapt the simple controls using
the existing source-specific invariant model.

ALBUM

Show:

  Shuffle:
    Default
    On
    Off

  Repeat:
    Default
    Off
    All

Do NOT offer Repeat One.

PLAYLIST

Show:

  Shuffle:
    Default
    On
    Off

  Repeat:
    Default
    Off
    All

Do NOT offer Repeat One.

TRACK / SINGLE SONG

Show:

  Repeat:
    Default
    Off
    One

Do NOT show shuffle.

Do NOT offer Repeat All.

STATION

Do not show shuffle or repeat controls.

Use the SHARED intent validator as authoritative.

The browser UI prevents nonsensical combinations for convenience, but backend/shared
validation must still reject invalid sourced intents from any caller.

Do not duplicate the source-mode rules into a second authoritative validator.

==================================================
7. "DEFAULT" SEMANTICS
==================================================

Use the UI label:

  Default

NOT:

  Preserve

for omitted policy-controlled fields.

Default means:

  omit this explicit field from the card

Then, when the card is eventually PLAYED:

  explicit intent
      >
  room policy
      >
  device policy
      >
  code-owned source default
      >
  preserve if still absent

Examples:

PLAYLIST CARD

  source=playlist
  transport=play
  shuffle omitted
  repeat omitted

On a device/room whose policy says playlist shuffle=true:

  shuffle=true
  repeat=off

Elsewhere:

  shuffle remains preserved
  repeat=off

Same card.

ALBUM CARD

  source=album
  transport=play
  shuffle omitted
  repeat omitted

Ordinary room/device:

  shuffle=false
  repeat=off

A device/room configured to shuffle albums:

  shuffle=true
  repeat=off

Same card.

SINGLE-TRACK CARD

  source=track
  transport=play
  repeat omitted

Ordinary room/device:

  repeat=off

Child device/room with track.repeat=one:

  repeat=one

Same card.

==================================================
8. CANONICAL STRUCTURED FORMAT
==================================================

New cards written by this tool should use the repository's CURRENT canonical structured
card format.

Do NOT invent another writer-specific schema.

Use compact serialization if useful, but preserve the existing semantic schema.

Prefer human-debuggable JSON/structured data unless real tag capacity demonstrates
that it does not fit.

Do not invent opaque binary encoding preemptively.

==================================================
9. CURRENT PLAIN-URL CARDS
==================================================

Plain supported Apple Music URL cards remain a CURRENT supported input format.

When read:

  plain URL
      ->
  existing normalizer
      ->
  equivalent source + play intent

The Read/Edit workflow should show those cards naturally in the editor.

When rewritten through the new writer, it is fine to write the canonical structured
format.

Do not describe this as migration.

Do not add conversion infrastructure.

It is simply:

  accepted input format A
      ->
  edited/written as canonical format B

==================================================
10. ADVANCED FIELD PRESERVATION
==================================================

Important:

The simple editor does NOT expose every field that can exist in a structured card.

For example, a manually authored/power-user card may contain:

- volume
- another currently supported advanced explicit field

When Read/Edit loads such a card:

- parse it normally
- populate the simple visible fields
- retain supported advanced fields internally
- clearly indicate something like:

    "This card contains advanced settings that will be preserved."

When the user changes source/shuffle/repeat and rewrites:

- update the fields represented by the simple editor
- preserve untouched supported advanced fields

Do NOT silently delete an advanced field merely because it is hidden.

However:

If changing SOURCE makes an existing advanced/source-specific field invalid under the
shared intent invariants, do NOT preserve an invalid resulting intent.

Reject the rewrite and explain that an advanced setting conflicts with the new source,
or provide a deliberate "remove incompatible advanced setting" action if that is
substantially cleaner.

Never silently create an invalid intent.

==================================================
11. WEB SERVER
==================================================

Host the writer UI directly on stick-s3.

Use a small embedded HTTP implementation suitable for the existing ESP32 runtime.

Keep it dependency-light.

The UI should work well in Safari on an iPhone.

No:
- cloud
- external JS frameworks
- React
- package/CDN dependency at browser runtime
- internet connection required beyond the phone obtaining the Apple Music URL itself

Serve all writer assets locally from the device.

A small amount of embedded HTML/CSS/JS is expected.

Do not build a generic embedded web-app framework.

==================================================
12. WEB API
==================================================

Build a small explicit API below the UI.

Conceptually useful operations:

  GET  writer status
  POST arm write
  POST arm read/edit
  POST cancel
  GET  current result

Exact endpoint names are implementation details.

A simple polling protocol is preferred.

Do NOT add WebSocket/SSE complexity unless it materially simplifies the implementation.

The browser must be able to observe states such as:

- idle
- armed-write
- armed-read
- card-detected
- writing
- verifying
- success
- failed
- timed-out

Do not expose a general remote Sonos-control API as part of this work.

==================================================
13. WRITER STATE MACHINE
==================================================

Writing must require deliberate transient arming.

Use one explicit state machine.

Conceptually:

  NORMAL_READER

  READ_ARMED

  WRITE_ARMED

  WRITING

  VERIFYING

  SUCCESS

  FAILED

Exact names may differ.

Requirements:

- normal NFC presentation continues existing playback behavior
- writing is IMPOSSIBLE unless explicitly armed
- read-for-edit is IMPOSSIBLE unless explicitly armed
- writer state is not persisted
- reboot returns to normal reader
- arming expires after a reasonable timeout
- one card presentation consumes the arm
- failed writes do not leave the device armed
- cancellation returns to normal reader
- another accidental card after the write cannot immediately be overwritten

==================================================
14. NFC OWNERSHIP
==================================================

A presented NFC card must have exactly one owner.

NORMAL_READER:

  presentation
      ->
  existing card playback path

READ_ARMED:

  presentation
      ->
  read/decode for browser
  NEVER execute MusicIntent

WRITE_ARMED:

  presentation
      ->
  write/verify
  NEVER execute MusicIntent

There must be no race where one presentation both:

- gets written/read
and
- starts music

Centralize this ownership decision.

Do not rely on timing/sleeps to prevent duplicate handling.

==================================================
15. WRITE FLOW
==================================================

When POST/arm-write supplies a proposed simple card:

1. normalize source
2. construct canonical intent
3. transport=play
4. apply explicit writer fields only
5. shared validate
6. serialize
7. ensure payload fits known/card-reported capacity where practical
8. enter WRITE_ARMED

On card presentation:

1. identify compatible writable tag
2. inspect capacity/status where supported
3. write complete NDEF payload
4. immediately read it back
5. decode using normal NFC decoder
6. parse using normal MusicIntent parser
7. semantic-compare against intended card
8. success only if equivalent

Do NOT execute the resulting MusicIntent.

==================================================
16. READ-BACK VERIFICATION
==================================================

A successful low-level write call is NOT enough.

Success requires semantic read-back verification.

Compare parsed intent semantics, not raw serialized bytes.

For example, irrelevant JSON formatting/key order does not matter.

But these differences DO matter:

- omitted vs explicit false
- omitted vs explicit repeat off
- wrong source URL
- wrong source kind
- missing play transport if canonical semantics require it
- lost advanced field during edit
- changed volume
- invalid normalized URL

If verification fails:

  FAILED

Do not report partial success.

Do not execute the tag.

==================================================
17. READ / EDIT FLOW
==================================================

User chooses:

  Read / Edit Card

Device enters READ_ARMED.

Next NFC presentation:

1. read existing NDEF
2. do not execute
3. decode normal current supported card format
4. parse through normal intent parser
5. return editable representation to browser
6. return source kind
7. return visible fields
8. retain supported hidden advanced fields
9. leave READ_ARMED

Browser populates editor.

The user may then modify:

- source
- shuffle
- repeat

and choose Write Card.

Writing uses the normal arm/write/verify flow.

==================================================
18. CARD CAPACITY
==================================================

Measure/inspect actual currently used tag capacity where the hardware/API makes that
available.

Report:

- serialized size for representative:
    album Default/Default
    playlist shuffle=true
    track repeat=one
    a plausible advanced card
- available tag capacity/headroom on a real current card if detectable

Preferred decision order if capacity becomes a problem:

1. compact JSON serialization of current schema
2. remove unnecessary serialization verbosity
3. only then consider a more compact CURRENT wire encoding

Do not invent opaque compression/binary format without actual capacity evidence.

==================================================
19. PHONE SOURCE WORKFLOW
==================================================

V1 source workflow is intentionally simple:

  Apple Music share
      ->
  copy URL
      ->
  paste into writer page

Do not build:

- Apple Music authentication
- Apple Music search
- Apple catalog browser
- iOS app
- Shortcut integration
- required PWA installation

If supporting browser share-target/PWA behavior is trivially small, report it as a
possible later enhancement rather than expanding this milestone.

Paste-first is accepted.

==================================================
20. URL NORMALIZATION
==================================================

Use the exact existing Apple Music source normalizer.

Do not duplicate URL classification logic in browser JS.

The browser may optimistically change controls after backend classification, but
authoritative normalization/source kind comes from shared/backend logic.

Invalid or unsupported URL:

- cannot arm writer
- produces useful browser error
- leaves device in normal reader mode

==================================================
21. POLICY PREVIEW
==================================================

Do NOT bake current policy results into cards.

Optional:

The writer UI MAY show an informational preview for the Stick's currently selected
room, such as:

  Default on Kitchen:
    shuffle=true
    repeat=off

ONLY if this comes cheaply from the existing policy resolver.

This is informational.

It must never cause omitted fields to become explicit.

If this preview adds substantial work, defer it.

==================================================
22. GROUPING
==================================================

Grouping is completely independent of card writing.

Do not expose grouping in writer UI.

Do not store group membership on cards.

When the written card is later used through NORMAL_READER:

- runtime selected logical room supplies policy context
- current Sonos topology supplies group/coordinator execution target
- the card behaves exactly like any other NFC intent

Writer verification must not require Sonos group state.

==================================================
23. READ_ONLY
==================================================

`read_only` governs SONOS MUTATIONS only.

NFC writing is an intentional local hardware operation.

Therefore:

  read_only=true

MAY still perform deliberately armed NFC writes.

Requirements:

- writer mode never changes read_only
- writing does not require Sonos availability
- read/edit does not require Sonos availability
- newly written cards are not executed during verification
- later NORMAL_READER presentation obeys read_only normally

Do not add a second persistent "allow writing" config flag.

Explicit transient writer arming is the NFC-write safeguard.

==================================================
24. POWER / INACTIVITY INTERACTION
==================================================

Integrate cleanly with the current inactivity/sleep system.

Opening/interacting with the local writer page is meaningful local device interaction
for writer purposes.

At minimum these should record activity:

- arm read
- arm write
- cancel
- actual card presentation
- successful/failed writer operation

While the device is actively WRITE_ARMED or READ_ARMED:

- do not enter automatic sleep mid-operation

Use the existing arm timeout as the bound.

After arm expires/cancels/completes:

- normal inactivity semantics resume

Do NOT permanently disable power management merely because the web server exists.

Background HTTP polling from an idle browser must NOT keep the device awake forever.

This distinction is important:

USER ACTION:
  arm/write/read/cancel
      -> activity

PASSIVE STATUS POLL:
      -> not activity

If the device sleeps, the writer page may naturally lose connectivity.

Physical-button wake remains the only wake path.

==================================================
25. DEVICE DISPLAY
==================================================

Keep the Stick display simple.

Normal operation remains normal.

During writer states, show clear compact states such as:

  WRITER
  Tap card

  READING
  ...

  WRITING
  ...

  VERIFYING

  WRITE OK

  WRITE FAILED

Exact wording/layout is flexible.

There must be no ambiguity when a card presentation will WRITE instead of PLAY.

Do not build a rich writer UI on the tiny screen.

==================================================
26. WEB UI
==================================================

Make the phone page deliberately simple.

Suggested primary layout:

  NFC Card Writer

  Apple Music URL
  [________________________]

  Source:
    Album / Playlist / Track / Station
    (shown after normalization)

  Shuffle
    [Default] [On] [Off]
    when supported

  Repeat
    appropriate source-specific options

  [Write Card]

  [Read / Edit Card]

Status area:

  Idle
  Waiting for card...
  Writing...
  Verifying...
  Success
  Failed: <concise reason>

If an edited card contains hidden advanced settings, show:

  Advanced settings present — preserved

Do not expose a general JSON editor in the primary UI.

A collapsible debug view of canonical serialized intent is acceptable if cheap and
useful, but not required.

==================================================
27. NETWORK SAFETY
==================================================

This is a trusted home-LAN personal tool, not an internet service.

Still implement reasonable embedded-service hygiene:

- bounded HTTP body sizes
- strict method/path handling
- strict submitted data validation
- no arbitrary filesystem access
- do not reveal Wi-Fi credentials/config
- do not return complete secret-bearing config
- no arbitrary URL fetch as part of card-writing submission
- one active writer operation at a time

Do not build accounts/authentication/OAuth/TLS infrastructure for the LAN writer.

If an extremely cheap per-device protection mechanism already fits naturally, evaluate
it, but do not derail the milestone.

==================================================
28. CONCURRENCY
==================================================

The HTTP server must not destabilize:

- NFC polling
- Sonos worker
- topology observation
- existing Stick buttons
- Wi-Fi recovery
- sleep logic

Do not perform long blocking HTTP work on a timing-sensitive NFC/UI path if avoidable.

Only one writer session/arm may exist at a time.

Concurrent browser requests must not create two write operations.

==================================================
29. SERIAL / DEVELOPMENT SURFACE
==================================================

Retain enough serial diagnostics to develop/debug:

- writer state
- normalized source kind
- payload size
- NFC capacity if known
- write result
- verification result

Never log Wi-Fi credentials.

Do not require serial access for ordinary phone use.

==================================================
30. PORTABLE TESTS
==================================================

Add tests for at least:

SERIALIZATION

- canonical album card
- canonical playlist card
- canonical track card
- explicit false survives
- explicit repeat off survives
- omitted remains omitted
- transport play generated by simple writer
- volume omitted by simple writer

SOURCE OPTIONS

- album supports shuffle and repeat off/all
- playlist supports shuffle and repeat off/all
- track supports repeat off/one only
- station has neither
- invalid source/mode rejected by shared validator

DEFAULT

- writer Default omits field
- Default does not materialize policy result

ROUND TRIP

- intent -> serialize -> normal parse -> semantic equality

URL CARDS

- plain URL reads into editable source/play representation
- rewrite emits canonical structured card

ADVANCED FIELDS

- hidden supported field survives read/edit/rewrite
- modifying visible field preserves hidden field
- source change that conflicts with hidden advanced setting does not produce invalid
  card

STATE MACHINE

- cannot write unless armed
- cannot read-for-edit unless armed
- arm timeout
- one-shot consumption
- cancel
- write failure disarms
- verification failure disarms
- reboot/default state is NORMAL_READER

OWNERSHIP

- normal presentation executes
- read-armed presentation never executes
- write-armed presentation never executes
- verification read never executes
- returning to normal restores normal NFC behavior

READ_ONLY

- read_only has no effect on deliberate NFC writing
- read_only still blocks later Sonos execution normally

POWER

- explicit writer actions reset activity
- passive browser status polling does not
- armed writer suppresses auto-sleep
- completion/cancel/timeout restores normal sleep eligibility

GROUPING

- card format contains no group data

HTTP

- oversized body rejects
- malformed JSON rejects
- unsupported source rejects
- concurrent arm attempt handled deterministically
- secret data not exposed

==================================================
31. HOST / TOOLING GREEN BASELINE
==================================================

Keep the repository's current Node/TypeScript and C++ green baseline intact.

Run current canonical tasks, including:

  node --run format
  node --run format:cpp
  node --run check
  node --run check:full

Regenerate the appropriate C++ editor compilation database if shared/test files change.

Do not leave VS Code with false diagnostics in new writer-related C++ tests.

==================================================
32. PHYSICAL TESTING
==================================================

Use stick-s3 autonomously for builds/flashing/read-only diagnostics where allowed.

Do NOT rewrite arbitrary existing family cards during autonomous work.

When physical NFC writing is ready, STOP and give me a concise test using one disposable
or expendable tag.

First physical test:

1. wake Stick
2. open writer page on phone
3. paste known Apple Music URL
4. confirm source classification
5. select legal explicit options
6. press Write Card
7. Stick clearly shows armed state
8. present disposable tag
9. write occurs once
10. read-back verification succeeds
11. browser reports success
12. device returns to normal reader
13. present card again normally
14. verify parsed intent is correct
15. optionally execute if current read_only setting permits and I want to test playback

Second physical test:

1. choose Read / Edit
2. present written card
3. browser populates fields
4. modify shuffle/repeat
5. rewrite
6. verify
7. read again

Also test one existing plain-URL card in READ mode WITHOUT rewriting it first.

==================================================
33. DOCS
==================================================

Update durable current docs only.

Document:

- supported card formats
- simple writer fields
- Default semantics
- source-specific option matrix
- explicit play behavior
- hidden advanced-field preservation
- one-shot arm/read/write/verify model
- local web workflow
- read_only distinction
- interaction with sleep
- grouping independence

Do not add:
- implementation diary
- owner acceptance checkpoint
- temporary test-tag IDs
- migration instructions
- old Shortcut workflow history

If hardware button semantics change, update the existing docs/hardware.md button/state
diagram as required by AGENTS.md.

==================================================
34. SCOPE LIMITS
==================================================

Explicitly do NOT implement:

- iOS app
- Shortcut action
- PWA requirement
- Apple Music search
- Apple Music authentication
- cloud writer
- Synology/Mac middleman
- room selection on cards
- group selection on cards
- volume in simple UI
- transport selector
- queueIndex/seek writer fields
- generic JSON card editor
- NFC writing on ws-1.8
- 4.3-inch support
- voice

==================================================
35. COMPLETION
==================================================

Stop when:

1. stick-s3 hosts a usable iPhone-friendly writer page
2. simple UI exposes only source + shuffle + repeat
3. transport is always play
4. source-specific invalid modes cannot be authored
5. Default omits values for policy resolution
6. new tags use canonical structured format
7. plain URL cards can be read/edit-loaded
8. advanced hidden fields are preserved safely
9. writing requires explicit one-shot arm
10. read/edit presentations never execute
11. writes are read-back verified through the normal parser
12. read_only does not block intentional NFC writes
13. group state never enters card contents
14. writer interaction integrates safely with sleep
15. existing normal NFC behavior remains intact
16. check and check:full pass

Then give me only:

- local writer URL/discovery method
- final simple UI behavior
- tag payload sizes/headroom observed
- physical test procedure
- anything genuinely deferred

Do not begin another milestone automatically.
