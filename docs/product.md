# Product specification

## Purpose and scope

`sonos-surface` is one family's physical interface to Sonos using Apple Music. Both boards run the same portable intent/policy/application core and direct Sonos adapter. A server is not required for card or touch playback. Deliberate contracts live in these specifications; measured facts and unresolved hardware assumptions live in [hardware](hardware.md).

| Supported target | Current capability |
| --- | --- |
| M5StickS3 + M5Stack NFC Universal Unit (ST25R3916) | Read existing NFC music cards; room selection and play/pause buttons; playback/status display |
| Waveshare ESP32-S3-Touch-AMOLED-1.8 | Accepted touch frontend, room selection, now playing, artwork, volume/seek, transport/modes, bounded queue browsing/selection |
| Household Sonos speakers | Direct control of independently eligible configured rooms |
| Mac | Portable tests, device tooling, read-only discovery and shared-adapter probe |

NFC tag writing and a simple local family writer are specified for future work, not implemented. Future voice may normalize curated names into the same intent; it must not become a general assistant or a required dependency of other inputs. 4.3C, NFC on Waveshare, grouping, other music services, general Sonos management, generic scripting/rules, and a general configuration UI are outside current scope.

## Shared architecture

```text
NFC / USB JSON / touch / buttons / future writer or voice
  -> normalize and validate MusicIntent
  -> bind selected UUID and resolve source/device/room policy once
  -> plan -> execute -> verify/reconcile through direct Sonos adapter

Sonos observations + separate request outcomes
  -> shared AppState -> board-specific presentation
```

- [Intent](intent.md) owns portable commands, normalized source validity, card encoding, and the future writer contract.
- [Policy](policy.md) owns room configuration/identity/selection, source defaults, overrides, per-field precedence/provenance, and revision.
- [Execution](planner-executor.md) owns admission, dependencies, preservation, failures, dispatch safeguards, and reconciliation.
- [Sonos capabilities](sonos-capabilities.md) owns normalized observations, bounded queue pages, seek/selection, and their protocol limits.
- [Waveshare frontend](waveshare-frontend.md) owns current touch/artwork behavior.
- [Hardware](hardware.md) owns board facts, calibration, measured limitations, and unresolved physical issues. [README](../README.md) owns operational commands.

`SurfaceCore` contains no hardware/network SDKs. `SurfaceSonos` owns Apple metadata and SOAP behind `LocalHttp`. `SurfaceDevice` owns ESP32 networking, NVS, workers, NFC, touch, rendering, and separate board adapters. UI and cards never encode Sonos ordering or sleeps. The current executor serializes work with explicit predecessor dependencies; richer concurrency requires evidence before design.

## Family workflows and portability

One continuous card presentation creates one request; removal and retapping create another. Supported NFC inputs are Apple Music URLs in Text, URI, or empty-type records, and declarative v1 JSON Text cards. URL-only cards normalize to source plus explicit play. A deliberate new source card replaces/restarts its source even if already selected. Music sources enter through explicit intents from NFC, USB URLs/JSON, or future writer/voice inputs.

The future simple writer exposes only Apple Music source, source-valid shuffle, and repeat. It always writes explicit `transport: "play"`. Its `Default` choice omits a mode field so device/room policy and shared source defaults resolve it when tapped. It never bakes device or room policies into cards. A single card can shuffle in one room, play in order in another, or repeat a single song only in a designated room. Volume and other advanced commands remain schema capabilities outside this UI. Detailed controls, write arming, capacity, and readback requirements live in intent.

Target, request identity, device origin, policy revision, and provenance are not card fields. Accepting work freezes the resolved target UUID and policy. Room selection is a device action and has no playback effect. Grouped targets are unavailable; never control their coordinator or change grouping as a workaround.

## Configuration and state

Committed environment profiles define household configuration shared by controllers. Board selection and environment selection are independent. Host tools resolve local environment secrets and import ordinary JSON over USB; each controller stores it with validate-then-replace semantics. Profile filenames have no targeting semantics. Ordinary configuration changes need no build. Room object keys define target availability; the single runtime mutation switch is specified in [policy](policy.md#device-configuration). Keep household credentials private and off cards; treat incoming JSON/cards as untrusted data.

The future browser writer must pair locally and prevent cross-origin writes. The exact pairing/server implementation is deferred. Read/edit/write mode must suppress card playback and retain unsupported data or refuse rewriting.

AppState distinguishes observed facts from pending/requested values and outcomes. Boot/reconnect fetches existing playback without input. Unknown/stale data must remain visibly unknown/stale, not fabricated zero volume or stopped state. Controllers independently converge on Sonos through polling and topology invalidation; concurrent external control is best effort. Commands are one-shot, never a standing desired state to enforce continuously. Artwork and queue work are bounded and do not become prerequisites for basic control.

## Evidence and remaining boundaries

The owner has confirmed M5 NFC album/playlist/station playback, physical room cycling, calibrated Waveshare center-button control, frontend appearance/usability, and artwork following an external album change. Both boards independently read Sonos. These observations do not establish production reliability or every protocol combination. Native USB/early-boot recovery, intermittent NFC preparation, battery operation, and full-screen calibration limits remain in the hardware document.

Portable fixtures cover validation, policies, operation ordering, omission and explicit false/zero, modes, volume, position, identity, failures, read-only dispatch, and board interaction models. Real playback or physical gesture checks require the owner; autonomous policy checks use fixtures and pure device previews. Do not require unnecessary live playback to establish shared policy semantics.
