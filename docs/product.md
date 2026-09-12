# Product specification

## Purpose and scope

`sonos-surface` is one family's physical interface to Sonos using Apple Music. Both boards run the same portable intent/policy/application core and direct Sonos adapter. A server is not required for card or touch playback. Deliberate contracts live in these specifications; measured facts and unresolved hardware assumptions live in [hardware](hardware.md).

| Supported target | Current capability |
| --- | --- |
| M5StickS3 + M5Stack NFC Universal Unit (ST25R3916) | Read/edit/write NFC music cards with a local phone page; room selection and play/pause buttons; playback/status display |
| Waveshare ESP32-S3-Touch-AMOLED-1.8 | Accepted touch frontend, room selection, now playing, artwork, volume/seek, transport/modes, bounded queue browsing/selection |
| Household Sonos speakers | Direct control of independently eligible configured rooms |
| Mac | Portable tests, device tooling, read-only discovery and shared-adapter probe |

Future voice may normalize curated names into the same intent; it must not become a general assistant or a required dependency of other inputs. 4.3C, NFC on Waveshare, grouping, other music services, general Sonos management, generic scripting/rules, and a general configuration UI are outside current scope.

## Shared architecture

```text
NFC / USB JSON / touch / buttons / writer-authored cards / future voice
  -> normalize and validate MusicIntent
  -> bind selected UUID and resolve source/device/room policy once
  -> plan -> execute -> verify/reconcile through direct Sonos adapter

Sonos observations + separate request outcomes
  -> shared AppState -> board-specific presentation
```

- [Intent](intent.md) owns portable commands, normalized source validity, card encoding, and the writer contract.
- [Policy](policy.md) owns room configuration/identity/selection, source defaults, overrides, per-field precedence/provenance, and revision.
- [Execution](planner-executor.md) owns admission, dependencies, preservation, failures, dispatch safeguards, and reconciliation.
- [Runtime lifecycles](runtime-lifecycles.md) owns bounded jobs, automatic network/session recovery, subscriptions, and deterministic fault injection.
- [Sonos capabilities](sonos-capabilities.md) owns normalized observations, bounded queue pages, seek/selection, and their protocol limits.
- [Waveshare frontend](waveshare-frontend.md) owns current touch/artwork behavior.
- [Hardware](hardware.md) owns board facts, calibration, measured limitations, and unresolved physical issues. [README](../README.md) owns operational commands.

`SurfaceCore` contains no hardware/network SDKs. `SurfaceSonos` owns Apple metadata and SOAP behind `LocalHttp`. `SurfaceDevice` owns ESP32 networking, NVS, workers, NFC, touch, rendering, and separate board adapters. UI and cards never encode Sonos ordering or sleeps. The current executor serializes work with explicit predecessor dependencies; richer concurrency requires evidence before design.

## Family workflows and portability

One continuous card presentation creates one request; removal and retapping create another. Supported NFC inputs are Apple Music URLs in Text, URI, or empty-type records, compact `ss1` Text cards, and declarative v1 JSON Text cards. URL-only and compact cards normalize to source plus explicit Play, with compact mode overrides when present. A deliberate new source card replaces/restarts its source even if already selected. Music sources enter through explicit intents from NFC, USB URLs/JSON, or writer-authored cards/future voice inputs.

The simple writer exposes only Apple Music source, source-valid shuffle, and repeat. It always constructs explicit `transport: "play"`; raw URL and compact encodings imply that field, while structured JSON stores it. The writer chooses raw URL for default cards, `ss1` for explicit modes, and structured JSON for advanced fields or metadata. Its `Default` choice omits a mode field so device/room policy and shared source defaults resolve it when tapped. It never bakes device or room policies into cards. A single card can shuffle in one room, play in order in another, or repeat a single song only in a designated room. Volume and other advanced commands remain schema capabilities outside this UI. Detailed controls, write arming, capacity, and readback requirements live in intent.

Target, request identity, device origin, policy revision, and provenance are not card fields. Accepting work freezes the resolved target UUID and policy. Room selection is a device action and has no playback effect. Grouped targets are unavailable; never control their coordinator or change grouping as a workaround.

## Configuration and state

Committed environment profiles define household configuration shared by controllers. Board selection and environment selection are independent. Host tools resolve local environment secrets and import ordinary JSON over USB; each controller stores it with validate-then-replace semantics. Profile filenames have no targeting semantics. Ordinary configuration changes need no build. Room object keys define target availability; the single runtime mutation switch is specified in [policy](policy.md#device-configuration). Keep household credentials private and off cards; treat incoming JSON/cards as untrusted data.

The [local Stick writer](tag-writer.md) accepts manual URL entry and an iOS/iPadOS Share Sheet Shortcut that creates a bounded, expiring RAM draft and opens the same editor. Draft creation never arms NFC, executes an intent, or changes room/configuration state. Browser arming requires the same Origin; only harmless draft creation accepts originless native requests. Unexpected Host headers and explicit foreign origins reject. Read/edit/write presentations suppress playback. Editing retains supported hidden fields and optional metadata; unsupported documents remain inspectable but cannot be rewritten.

AppState distinguishes observed facts from pending/requested values and outcomes. Boot/reconnect fetches existing playback without input. Unknown/stale data must remain visibly unknown/stale, not fabricated zero volume or stopped state. Controllers independently converge on Sonos through polling and topology invalidation; concurrent external control is best effort. Commands are one-shot, never a standing desired state to enforce continuously. Artwork and queue work are bounded and do not become prerequisites for basic control.

The Stick's normal display separates observed Sonos information (room, song, artist, album, playback, volume, and mode) from device diagnostics (Wi-Fi, mutation gate, worker activity, input notice, and request progress/errors). Horizontal rules separate those sections and the bottom writer address/button reminders. Text is bounded to its section; long values are truncated with an ellipsis. Active card-writing operations retain their dedicated status screen.

## Inactivity and physical wake

Device configuration `sleep_timeout_seconds` is an integer from 0 to 4294967295, defaulting to 300 when omitted. Positive values request sleep after that many seconds without local interaction; zero disables automatic sleep for development. Profiles choose the timeout independently of target, room policy, and `read_only`. USB power does not suppress sleep.

One monotonic device timer consumes local activity: pressed buttons, touchscreen contact (including diagnostic screens), and a newly presented NFC card while awake. Activity counts even if an action is rejected, no control is hit, or the card cannot be decoded. A held card counts once until removal and retap; it cannot keep the device awake indefinitely. Held buttons/touch contacts continue counting as interaction. Opening the Stick writer page, successful explicit draft creation, and deliberate editor/source/read/write/cancel actions also count. Armed and active writer operations postpone sleep within a bounded timeout; completion/failure counts as interaction. Draft existence, failed draft creation, and passive draft/status reads do not count or suppress sleep. USB commands, Sonos polling/events, track/position/queue changes, artwork, topology/availability, retries, Wi-Fi traffic, and rendering do not count.

At timeout the runtime stops accepting actions, closes HTTP admission, cancels artwork, shuts down networking/peripherals, and enters the board's sleep state. It does not save transient application state. Physical wake runs normal boot, loads configuration and the existing preferred room, reconnects/discovers, and fetches authoritative state. Connections, subscriptions, caches, and pending actions are never restored or replayed. Only physical buttons are configured to wake the CPU; touch, NFC, network, and timer wake are disabled. See the [hardware button/state tables](hardware.md#inactivity-power-and-physical-buttons) for electrical controls and power-loss limitations.

## Evidence and remaining boundaries

The owner has confirmed M5 NFC album/playlist/station playback, physical room cycling, calibrated Waveshare center-button control, frontend appearance/usability, and artwork following an external album change. Both boards independently read Sonos. These observations do not establish production reliability or every protocol combination. Native USB/early-boot recovery, intermittent NFC preparation, battery operation, and full-screen calibration limits remain in the hardware document.

Portable fixtures cover validation, policies, operation ordering, omission and explicit false/zero, modes, volume, position, identity, failures, read-only dispatch, and board interaction models. Real playback or physical gesture checks require the owner; autonomous policy checks use fixtures and pure device previews. Do not require unnecessary live playback to establish shared policy semantics.
