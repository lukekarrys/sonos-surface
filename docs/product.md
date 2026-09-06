# Product specification

## Purpose and contract

`sonos-surface` is one family's physical Sonos interface, using Apple Music.
**MUST**, **SHOULD**, and **MAY** indicate requirements, recommendations, and
optional behavior. Deliberate contracts below are distinct from reference
observations and experimental assumptions. These documents specify the first
safe vertical slice and its extension points, not a complete Sonos library.

Implementation progress and measured evidence live in [hardware.md](hardware.md).
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
| Waveshare ESP32-S3-Touch-AMOLED-1.8 | Touch controller, playback state, artwork, volume, transport, queue browsing | Queue selection/editing; microphone voice input |
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
speaker ID, and configuration revision. These are not card properties. Use a
stable Sonos identifier for targets; room names are labels. Bind the device's
configured target or explicit UI selection at acceptance. Missing, ambiguous,
or offline targets never fall back to another room. Changing rooms does not
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

- Which room gets playlist shuffle? Supply its stable target during setup.
- What queue interactions and artist-name voice behavior are actually wanted?
  V1 defers queue mutations/selection and uses explicit aliases for voice.

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

The current focus is M5StickS3 + NFC. SSDP/bootstrap discovery and fresh topology
produce a bounded list of independent players, with stable UUID, current name/IP,
coordinator/group, and eligibility. Grouped/bonded/invisible players are unavailable;
there is no coordinator fallback or grouping management. Refresh at boot,
reconnect, topology notifications, and every ten seconds. Missing topology fails
closed. A double-click cycles eligible UUIDs without changing playback.

The device stores its last explicit selection separately from household config.
Boot restores it if eligible; otherwise reports unavailability and selects the
lexicographically first eligible UUID. Subsequent invalidation keeps the selected
identity and rejects new commands, with explicit cycling available for recovery.
Each accepted request binds that identity and runtime policy before execution.
Changing a display name or address cannot change policy identity or authorization.

The `playlist_shuffle_rooms` UUID-to-boolean map is persisted runtime configuration,
not a compile-time policy. Room discovery and selection grant no mutation rights.
Only a separately enabled, explicitly UUID-bound Office test image may mutate.
