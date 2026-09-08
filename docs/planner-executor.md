# Planner, executor, and state

## Admission and frozen work

Every input normalizes and validates through SurfaceCore. Source-specific mode invariants reject before policy or any Sonos effect. Bind the selected configured, uniquely resolved, eligible UUID and resolve [policy](policy.md) once. The accepted `ResolvedIntent` contains concrete optional fields, per-field provenance, UUID, and configuration revision. Queueing, room switching, configuration replacement, or topology changes cannot rerun policy or retarget accepted work.

The current runtime has one Sonos worker and at most one outstanding job. Input and config changes while busy reject rather than replaying later. A per-UUID Application session retains observations and uncertain-command recovery state. Fresh topology before work must still contain the bound selectable UUID; if it is unavailable, fail instead of using the new selected room. Sessions update addresses only for their existing UUID.

Stick B and USB `toggle` bind UUID/context first, then refresh that room. PLAYING normalizes to explicit Pause; PAUSED_PLAYBACK/STOPPED to Play. Unknown, stale, wrong-target, transitioning, or no-media states reject. The ordinary policy/planner path then runs with that captured context. Toggle is not a card/schema field and does not imply a source, mode, volume, or queue change.

## Typed operations and preflight

`Plan` is a finite serial list of typed operations; each operation depends on its predecessor completing. Inputs never construct SOAP sequences. There is no generic workflow engine or concurrent mutation scheduler. Adapter readiness checks, bounded polls, and protocol acknowledgments establish dependencies; card semantics never use sleeps. Pure `preview` diagnostics stop after planning and never create work.

Preflight resolves source URI/metadata, verifies identity/independence and supported capabilities, and reads fresh state needed for preservation or relative volume. Missing required state fails before mutations. Baselines are captured at execution, not acceptance. These are local safeguards, not a lock on other Sonos controllers.

Queue source replacement uses Stop → ClearQueue → AddSource → SelectQueue → ApplyMode → requested volume → Play/RestoreTransport as applicable. Clear waits for zero entries; add requires positive NumTracksAdded and a nonempty queue. Mode/volume settings precede playback. Verification checks selected queue, transport, requested/preserved modes, and mute. This cannot prove catalog identity against a simultaneous external queue replacement.

Station selection uses direct SetAVTransportURI → Play when play is explicit. Omitted transport/pause prepares non-playing transport with Stop before selecting and restores the captured category if required. It never clears/adds/selects the stored queue or sets play modes. Verify the requested station URI, transport, volume where requested, and preserved mute. A generic PLAYING result on a different station is insufficient. URI mapping does not prove Apple entitlement; insertion can still fail after a destructive queue clear.

Seek/SelectQueueItem run before other requested effects. Their fresh content, revision, bounds, timing tolerance, and single-attempt behavior are defined in [Sonos capabilities](sonos-capabilities.md#seek-and-queue-selection).

## Preservation

Only fields still absent **after policy** preserve prior values. With a new album, playlist, or track, repeat normally resolves off; that is a policy value, not a preserved baseline. Source defaults never run for a no-source mode command.

Sonos combines shuffle/repeat into one mode. Combine resolved fields with the fresh omitted component; restore captured mode/mute if source selection has side effects. For a track, declarative shuffle is unsupported, but the adapter may carry the existing combined mode's shuffle component without deriving a track shuffle policy. Never otherwise reassert omitted volume.

Source changes replace content/position. Omitted transport preserves playing versus non-playing, not exact paused/stopped labels or the prior position. Explicit pause also means non-playing, including already stopped. Unknown/transitional transport must refresh or fail before mutation. Source-only while playing resumes the new source; source-only while non-playing and source+pause do not issue Play.

Relative volume reads a fresh baseline once, computes `clamp(current + delta, 0, 100)`, and freezes that absolute target. Re-execution must not apply the delta again. Zero/clamped-no-change deltas need no write. A volume-only intent does not change source/queue. Next/previous issue one native skip, with no replay fallback at queue boundaries or implicit Play.

## HTTP dispatch safeguards

`GuardedHttp::request` is the final boundary used by ESP HTTP and host tests. Runtime `read_only=true` blocks all non-read actions, including unknown future actions, before dispatch. False allows only requested writes to a configured, selectable target. The gate independently reads the destination root ZonePlayer UUID and compares it with the frozen target; embedded MediaServer/Renderer UDNs are not identities. A changed name cannot authorize a different UUID.

The Sonos adapter rechecks identity and independent topology before every operation. Grouped, bonded, invisible, or unverifiable targets reject; never substitute a coordinator or dissolve a group. These checks reduce external-controller races but cannot make a topology check and SOAP mutation atomic. The Mac probe permanently uses read-only dispatch. There is no room-specific compile-time authorization.

## Failures and verification

There are no automatic mutation retries. Stop dispatching later operations after failure and reconcile potentially affected state. A queue-clear success followed by insertion failure can leave an empty queue; never automatically roll back over external edits. Lost or malformed responses after dispatch are uncertain, not proof of no effect. Insertion and native skips must never be blindly repeated.

An uncertain result blocks further mutations for that UUID even after cycling away and back. Ordinary refresh does not clear this requirement or replay work. Recovery currently requires inspection and a deliberate restart/new request; reboot refreshes state without replaying the previous command. General command cancellation, durable request-ID deduplication, automatic recovery, and cross-device serialization are deferred.

Device inactivity shutdown closes local/USB admission and the ESP HTTP dispatch gate independently of `read_only`. Artwork cancellation and Wi-Fi shutdown stop background work; the topology listener closes when its worker next yields, and the remote subscription lease expires without a blocking unsubscribe. Deep sleep terminates remaining tasks and in-flight sockets. A request admitted before shutdown may already have affected Sonos, including a partially changed queue; no rollback or replay occurs. Wake starts fresh and reconciles authoritative state. Shutdown never waits for the executor's full network budget and never persists transient execution state.

Current outcomes are `succeeded` (required result verified), `failed`, `partial`, and `uncertain`. The executor counts completed operations, including no-ops, so `partial` after an already-satisfied Stop and READ_ONLY_BLOCKED queue clear does not imply a write occurred. Error detail and dispatch logs distinguish this case. Next/previous completion is acknowledgment plus fresh state, not a guarantee that track index changes at a boundary.

Connect/HTTP timeouts are 3/8 seconds, with a 60-second dispatch budget, 10-second readiness/verification windows, and 200-ms condition polls. In-flight HTTP can extend a window by its timeout. These are experimental adapter bounds, not intent semantics or end-to-end latency guarantees.

## Observation and reconciliation

AppState keeps request `status`/`detail` and policy provenance separate from `observed` playback and `refreshError`. A failed read retains the last same-room snapshot as stale. Successful refresh/verification/reconciliation clears the read error while retaining command outcomes and uncertainty. UI previews never become observed facts.

Selection clears the displayed observation/page immediately; outcomes from another UUID cannot publish into the new room. Revisiting a session invalidates its cached observation while retaining command uncertainty. Queue pages and artwork have bounded identity/revision lifetimes defined in their capability/frontend docs.

The runtime reads at boot/reconnect, after commands, at a nominal ten-second cadence, and after topology invalidation. GENA on port 1401 only invalidates topology; a fresh full snapshot supplies room decisions. Subscription renewal honors the granted lifetime and failed subscriptions retry. Full AVTransport, RenderingControl, and ContentDirectory event subscriptions are deferred. Observations never reapply old intents. Busy execution/network delays can extend the polling interval.

Host fixtures exercise ordering, omission, frozen relative volume/modes, failure and uncertainty, destination identity, read-only gating, selected-state projection, and queue/position races with an injected clock. [Hardware](hardware.md) separates measured real-device behavior from protocol fixtures and experimental assumptions.
