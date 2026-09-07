# Product specification

## Purpose and contract

`sonos-surface` is one family's physical Sonos interface, using Apple Music.
**MUST**, **SHOULD**, and **MAY** indicate requirements, recommendations, and
optional behavior. Deliberate contracts below are distinct from reference
observations and experimental assumptions. These documents specify the first
safe vertical slice and its extension points, not a complete Sonos library.

Implementation progress and measured evidence live in [hardware.md](hardware.md).
The current [shared capability milestone](sonos-capabilities.md) adds timing,
metadata/artwork references, bounded queue reads, seek, and active-queue selection
as the shared backend. The [Waveshare frontend](waveshare-frontend.md) now adds
now-playing, room selection, volume/seek, and bounded queue selection. Its
overall visual/touch layout is owner-accepted, with mistaps and safer volume
interaction noted for follow-up. It now supports bounded background JPEG artwork.
Playback state uses polling; topology notifications invalidate the selectable-room list.
Unsupported explicit v1 capabilities reject before mutation. Both device playback paths now have physical evidence:
M5 NFC playback and calibrated Waveshare button control are owner-confirmed.
Calibration is now device-local NVS data; native USB/cold-boot recovery and physical
validation of the hardened adapters remain limitations. See
[README](../README.md) for reproducible commands.

## Available hardware and scope

| Available target | Product capability | Later expansion |
| --- | --- | --- |
| M5StickS3 + M5Stack NFC Universal Unit (ST25R3916) | NFC music-card reading/writing, small playback UI, local browser editor/configuration | More family card features |
| Waveshare ESP32-S3-Touch-AMOLED-1.8 | Touch controller, playback state, artwork, volume, transport, queue browsing/selection | Queue editing; microphone voice input |
| Real household Sonos speakers | Direct device control and empirical validation | Multiple controllers targeting the same speaker |
| Optional Mac mini | Mac tests and diagnostic/reference tools | Speech-to-text and curated Apple Music name resolution |

Direct Sonos control belongs in the first milestone. A server is not required
for NFC/touch playback. The Mac mini may later resolve voice into the same
MusicIntent; its unavailability must not disable other inputs. Do not build a
general assistant: ambiguous names require a curated alias or user choice.

Non-goals: grouping, other music services, general Sonos administration, generic
scripting, enterprise infrastructure, and a complete desktop implementation.
An unexpectedly grouped target MUST reject mutations with an explanation;
never silently control its coordinator or dissolve the group.

## Shared architecture

```text
NFC / touch / buttons / resolved voice
  -> normalize MusicIntent -> contextual policy -> planner -> executor
                                                              |
                                                       Sonos transport
                                                              |
Sonos observations -> shared AppState -> hardware-specific UI
Request status -----> shared AppState
```

[Intent](intent.md) defines commands and card data; [policy](policy.md) defines
set-if-missing rules; [planner/executor](planner-executor.md) defines ordering,
failures, and reconciliation. Both boards MUST share these contracts and the
hardware-independent core. Sonos networking stays below the application/planner;
UI, NFC, and microphone SDKs stay in adapters. Mac tests cover portable logic
with small fakes and an injected clock where useful.

A submission envelope binds a unique request ID, input/device origin, target
speaker ID, and configuration revision. These are not card properties. Resolve configured display IDs from current Sonos names to stable UUIDs at topology
refresh; freeze the selected UUID at acceptance. Unresolvable entries never bind.
Selection can visibly fall back to another valid configured room before acceptance. Changing rooms does not
retarget accepted work. Multiple device instances are supported without a
required central coordinator; concurrent external control is best effort.

## Family workflows and state

Normal playback cards explicitly request `transport: "play"`. Source-only
intents preserve playing versus non-playing state. One continuous card
presentation creates one request; removal and a new tap create another.
Reading/editing/writing mode suppresses playback. The browser writer previews
explicit settings and effective room policies separately, arms one write, then
verifies readback. It must not bake policy-derived defaults into cards silently.

Ordinary card and configuration edits MUST NOT require firmware/server
deployment. Persist versioned configuration using validate-then-replace and
retain the last valid copy. V1 configuration is local to each controller;
household policy describes scope, not automatic distribution. Support portable
configuration import/export; automatic synchronization is deferred. Authorize
browser mutations through local pairing and prevent cross-origin writes; the
mechanism is an implementation choice. Keep credentials off cards and out of
commits. Treat card and voice content as untrusted input.

AppState is shared within each controller; controllers converge on Sonos
observations. It contains per-speaker identity/connectivity/capabilities,
observed playback, volume/mute, shuffle/repeat, source/track/artwork, queue pages,
and per-field known/unknown/stale status with observation revisions/times.
Pending request status and policy explanations are separate from observed facts.
Boot/reconnect subscribes and fetches state without requiring user input.
Unknown state is shown as loading/stale, not invented zero volume or stopped
playback. Artwork and paginated queue caches are bounded and never block basic
controls. Commands are one-shot requests, not state to enforce forever.

## Exact first vertical slice

After this documentation task, implement only:

1. A shared minimal parser/normalizer, required shuffle policies, AppState, and
   source-plus-play application path; run small Mac fixtures for omission,
   explicit false, URL kind, policy precedence, and operation ordering.
2. A direct Sonos adapter for one configured ungrouped speaker and one known
   playable Apple Music album. Measure required queue/mode/play ordering on the
   real speaker before encoding adapter dependencies.
3. M5StickS3: read a bare-URL card or a v1 Text-record card explicitly requesting
   source + play, pass the normalized intent unchanged through that core, and
   display pending/success/error plus observed playback.
4. Waveshare: a touch button emits the same album intent through the same core
   and adapter, with equivalent status. Validate display/touch on the board.
5. Read current state on boot and after commands; validate a minimal playback
   subscription and reconnect/refresh path against an external Sonos change.

Acceptance: either board plays the configured album directly, reflects a
speaker already playing at boot, and reports failures without blind destructive
retries. Capture actual timing and hardware quirks. Do not make NFC writing,
artwork, queue browsing, voice, or an elaborate simulator prerequisites.
The slice may expose a subset of v1 capabilities, but must reject unsupported
explicit or policy-derived fields before any effect, never silently drop them.

## Questions and empirical validation

Product questions that remain for the family, without blocking that slice:

- Which rooms get playlist shuffle? Supply their display IDs during setup.
- What queue interactions and artist-name voice behavior are actually wanted?
  The current milestone supports existing active-queue selection, defers general
  queue editing, and uses explicit aliases for voice.

Existing cards are confirmed by the owner to contain only an Apple Music URL in
an NDEF Text/URI record or a well-known record with an empty type and raw URL
payload (confirmed in the owner's NFC Tools screenshots).
Supporting these alongside new v1 cards is required for
testing and initial use; rewriting the existing collection is not a prerequisite.
Legacy compatibility also includes Apple Music station share URLs, including
personal stations. These select a direct radio source through the same intent
and application layers; they do not replace the stored queue.

Agents should choose the build stack, drivers, bounded parser limits, pairing
mechanism, polling/retry timings, and caches through small experiments rather
than asking preference questions. Test actual NFC tag formats/capacity/read-write
support, display/touch behavior, memory budgets, and board/library quirks early.

Reference reviewed: [node-sonos-http-api at 6644198](https://github.com/lukekarrys/node-sonos-http-api/tree/664419878228cbd66dd36956e5c03fc68a09f587).
Its [device routes](https://github.com/lukekarrys/node-sonos-http-api/blob/664419878228cbd66dd36956e5c03fc68a09f587/src/device.ts)
parse Apple album/playlist URLs and track query IDs, use combined play modes,
and fetch initial state alongside subscriptions. Its
[action runner](https://github.com/lukekarrys/node-sonos-http-api/blob/664419878228cbd66dd36956e5c03fc68a09f587/src/sonos.ts)
awaits operations sequentially. This is reference evidence, not proof of Sonos
protocol behavior; transitive library behavior was not audited.

Validate Apple URL-to-playable-metadata mapping/account access, queue readiness
and side effects, shuffle/repeat preservation, no unintended autoplay while
non-playing, older-speaker latency, safe concurrency, subscription renewal/loss,
snapshot races, and interference from other controllers. Encode measured facts
in the adapter rather than assumptions in MusicIntent.

## Stick room and policy milestone

Persist two independent device controls in USB/NVS configuration:
`read_only` (defaults true) and `rooms` (defaults empty). Discovery learns the full
household; only uniquely resolved configured rooms that are independently eligible
and have an address enter selection. Never automatically enroll newly discovered
rooms. Grouped, missing, bonded, invisible, or unverifiable targets are unavailable;
there is no grouping management or coordinator substitution.

`roomDisplayId` is derived from the current room name: ASCII lowercase letters and
digits, whitespace/hyphens/underscores collapsed to one hyphen, other ASCII
punctuation stripped, and edge hyphens trimmed. Examples: `Office` → `office`,
`Kids' Room` → `kids-room`. Empty results, non-ASCII names, control characters,
and IDs longer than 64 characters are reported as invalid. There is no locale
transliteration. Configured IDs must already be canonical; do not normalize typos.
Collisions are ambiguous even if one colliding player is currently ineligible.

At each topology refresh, resolve both room-list and playlist-policy IDs against
all discovered rooms. Invalid, missing, ambiguous, or unavailable room entries
remain configured and produce detailed serial warnings plus a concise display
warning. Valid entries continue safely. A rename invalidates the old ID, including
its policy rule; an old UUID preference never repairs it. Legacy `sonos_uid` is
ignored for selection; `sonos_ip` is only an optional discovery hint.

A double-click cycles configured eligible rooms by case-insensitive human name,
with UUID as tiebreaker, without changing playback. USB `room-next` uses the same
path. Save the preferred display ID in `surface/preferred-id`. Restore it when
eligible at boot; otherwise show a warning and choose the first valid sorted room.
If the current selection disappears, use the same deterministic fallback. A
previously unavailable configured room reappears automatically, without stealing
the current selection. Empty selection blocks new intents and requests config repair.

Read-only mode still parses, binds UUID, resolves policy, and logs a complete static
plan; available preflight reads run normally. The final HTTP gate blocks any
mutating action (including unknown actions) before network dispatch. Serial and
Stick display identify the mode. `read-only true` / `read-only false` persist the
flag and reboot without rebuilding firmware. Configuration updates also reboot;
input/config changes while a request is busy reject. With read-only disabled,
only resolved configured eligible targets may receive requested effects. The
adapter rechecks UUID and independence; these checks cannot make external topology
changes atomic. No room-specific name/UUID build authorization remains.

Accepted requests retain their resolved UUID and shuffle result/revision across
selection, topology, rename, or configuration changes; they can fail if that UUID
becomes unavailable, but can never switch to the new selection. Policy config uses
`playlist_shuffle_rooms` display-ID-to-boolean entries, resolved into a trusted
UUID map before policy runs. See [policy](policy.md) and [execution](planner-executor.md).
