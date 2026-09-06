# Planner, executor, and state specification

## Deliberate boundaries

The current slice uses a serial typed operation list, one worker, bounded
readiness polling, and no mutation retries. Playback subscriptions, deduplication,
cancellation, and richer recovery are deferred. [Hardware notes](hardware.md)
record exact limits and evidence; the reliability contract below describes the
direction of implementation, not features already proven on the boards.

The planner converts a validated, policy-resolved MusicIntent into a finite
acyclic dependency graph. The executor schedules it and handles failures; the
Sonos adapter implements protocol calls and supplies capability/readiness
facts. Inputs/UI never construct Sonos operation lists. Start with only the
operations needed by the [first vertical slice](product.md#exact-first-vertical-slice).
A small typed plan is sufficient; no generic workflow engine is required.

Bind target and policy revision at acceptance. Within an executor, run mutating
requests FIFO, one per target; different targets may proceed independently.
Bound the pending queue and return busy on overflow. Before mutation, resolve
source metadata, verify an ungrouped target and supported intent, and refresh
state needed for preservation or relative volume. Capture baselines when the
request reaches execution, not while queued. Unavailable required state fails
before mutation; never substitute defaults. These are local guarantees, not a
global lock against other boards or Sonos apps.

## Operation contract and dependencies

Every operation encodes, in data or typed code:

- Identity, concrete target/arguments, prerequisites, and dependency IDs.
- Conflicting resources and adapter concurrency limits.
- Finite attempt timeout and overall request budget.
- Bounded retries/backoff, eligible failures, and retry-safety classification.
- Completion/readiness evidence and verification or reconciliation requirements.

A dependency completes when its readiness predicate holds, not merely when an
arbitrary delay expires. Acknowledgment may suffice only if the adapter has
validated that it makes the dependent action safe. Otherwise use an event or
bounded read/poll. Backoff, deadlines, and subscription timers are legitimate;
sleeps MUST NOT encode the semantics of a card or operation dependency.

Station + play uses SelectStation (SetAVTransportURI with station metadata) →
Play. It does not clear/add/select a queue or set shuffle/repeat. Verify the
station URI and playing state, plus preserved mute; a generic PLAYING result on
a different source is insufficient. Reject explicit unsupported mode fields
before selection. Station source + omitted transport/pause prepares non-playing transport with Stop
before selection. Explicit source + play retains the established station path. The stored queue remains available for later queue use.

Illustrative queue source + play + volume plan:

```text
Preflight -> optional transport preparation -> ClearQueue -> AddSource
                                                              |
                                                         SelectQueue
                                                              |
                                                   Apply required settings
                                                              |
Preflight -> SetVolume -------------------------------------> Play
                                                              |
                                                      Verify observations
```

This is a semantic example, not a mandated SOAP call list. The adapter must
establish insertion/selection readiness and where mode changes belong. Volume
may overlap queue work when demonstrated safe; Play waits for requested volume
and required settings. Conflicting resources cannot overlap even without an
explicit dependency. Begin real adapters with one in-flight mutation per speaker
until measurements justify concurrency. Test allowed overlap with a small fake.

## Preservation and state-dependent planning

Refresh relative-volume baselines and required preservation fields before
mutation. Compute a relative volume target once, clamp to 0–100, and freeze that
absolute target for retries. Do not apply the delta twice. For Sonos combined
play-mode writes, combine requested/resolved shuffle or repeat with the fresh
omitted component. If source replacement changes an omitted mode or mute,
restore the captured value; otherwise omit the unnecessary write. Record these
baseline calculations as planning reasons, separate from policy provenance.

Source replacement changes content/position, starting at the new queue's initial
position subject to shuffle. With omitted transport, capture playing versus
non-playing and preserve that category. Unknown/transitional playback requires
refresh or failure before mutation. Explicit pause also means non-playing,
including already stopped. An adapter must prepare transport as needed so
non-playing requests do not audibly start, and requested settings are ready
before new-source playback. Reject unsupported preservation rather than silently
changing semantics. Exact preparation is an experimental adapter concern.

A volume-only request has no queue/transport operations. Source + pause ends
without Play; source + omitted transport captured as playing follows the play
path. A deliberate new source-card tap replaces/restarts even a matching source.
`next` is one native advance, not a source replacement or implicit play.

## Retry safety and failure behavior

Distinguish definite rejection/no effect, acknowledged success, and uncertain
effect such as timeout after dispatch. A timeout does not prove failure.

| Kind | Required retry behavior |
| --- | --- |
| Reads | Bounded transient retries |
| Absolute volume/mode or play/pause targets | Reconcile after uncertainty; retry frozen arguments only while preconditions hold |
| Queue clearing/replacement | Never blindly repeat a destructive step after uncertainty |
| Queue insertion | May duplicate content; establish whether insertion occurred or stop uncertain |
| Queue selection | Reconcile queue identity/selection before retrying |
| Next | Never resend after dispatch unless no effect is proven |

A repeated clear can destroy content added externally; apparent idempotency is
not sufficient. An acknowledged action and fresh postcondition can establish
completion, but a matching late event alone does not prove causality. For
ambiguous non-idempotent effects, including duplicate tracks, preserve uncertainty.

On terminal failure/uncertainty, stop dispatching new mutations, skip dependent
work, let in-flight work settle within budget, and reconcile potentially touched
state. Completed independent effects remain: volume may succeed while insertion
fails after clearing the queue. Do not roll back automatically or restore old
queues over external changes. A deliberate new request may retry after refresh.

Cancellation stops queued/future dispatch, not already-sent Sonos calls. A timed
out call may finish later; hold further local mutations while the adapter
establishes a safe baseline. If unresolved effects remain, mark the target
uncertain and require recovery rather than releasing it on an arbitrary timer.
Recovery means adapter-supported evidence of settled state, or an explicit user
retry acknowledging the remaining uncertainty; neither claims global isolation.
After reboot, refresh and never automatically replay old mutations.

## Identity, outcomes, and observability

Request IDs deduplicate identical resubmissions within a bounded documented
in-memory retention window. Same ID with different bound input is invalid.
Cards do not contain request IDs; removal/new presentation creates a new request.
After retention expiry/reboot, uncertain actions need refresh and a new user
request, not an assumption of exactly-once delivery. V1 does not coalesce separate
intents; optimization must not drop relative presses or queue replacements.

Expose per-operation timing, attempts, evidence, errors, and skipped work, plus
target, policy revision/provenance, and preservation baselines. Final status is:

- `succeeded`: required effects verified.
- `failed`: known failure with no effect.
- `partial`: known failure/cancellation with confirmed effects remaining.
- `uncertain`: at least one effect cannot be established; include known results.
- `cancelled`: cancellation with no confirmed or possible effect.

UI pending/requested values remain separate from observed AppState. Report useful
consequences, for example “Volume set; queue insertion failed.” A successful
command is not a promise against a later external change.

In the current implementation, `AppState.refreshError` holds the latest failed
observation read separately from command `status`/`detail`. Successful refresh,
verification, or reconciliation clears that observation error. A failed refresh
retains the last snapshot as stale. Refresh recovery never clears an uncertain
command's recovery requirement or replays that command.

## Events and reconciliation

Commands cause changes; subscriptions promptly update AppState; polling repairs
missed events/drift. On boot/reconnect, subscribe and fetch initial state without
waiting for user input. Reconcile events received during snapshots: a read begun
before a newer field event must not overwrite that event. Use protocol sequence
evidence where available and refresh disputed fields; local receipt order alone
cannot establish remote order. Unknown/stale facts remain visibly unknown/stale.

Process observations during execution. Renew subscriptions before expiry and
refresh after event gaps, reconnect, or renewal failure. Unexpected changes to
required queue/source/mode baselines invalidate affected unsent work. Recheck
known target/group eligibility and operation preconditions before dispatch;
these checks reduce races but are not atomic against external controllers.

Reconciliation reads current reality; it MUST NOT continuously enforce old
intents. Queue changes invalidate cached pages; discard artwork for a replaced
track. Timer values, polling bounds, and retry budgets are adapter configuration
to measure on real speakers, not user-intent fields.

## Evidence and validation

The [reference implementation](product.md#questions-and-empirical-validation)
provides examples of sequential actions and event-backed state. It does not
establish this adapter's retry safety, concurrency, or readiness predicates.
Test those facts on available Sonos hardware early, including older speakers,
Apple metadata/account access, queue side effects, paused/stopped behavior,
combined shuffle/repeat, event loss, and simultaneous external changes.

Small Mac fixtures should cover dependency ordering and permitted overlap;
omitted-state preservation and explicit false/zero; relative retries; timeout
after successful insertion; partial effects/cancellation; target serialization;
and an event arriving during a snapshot. Add failure cases as operations are
introduced. Use an injected clock instead of real sleeps. Passing fake tests
does not prove hardware/protocol behavior, and a complete simulator is not a
prerequisite for the first device slice.

## Stick milestone execution contracts

Acceptance parses/validates input and freezes a resolved intent with target UUID
and policy revision before handing it to the worker. A per-UUID application retains
uncertainty even after cycling away and back. The worker checks fresh discovery
against that bound UUID; it never substitutes the current selection. Input during
work (including room gestures) returns busy; one accepted request runs at a time.

Relative volume is captured once in adapter preflight and sent as SetVolume with
frozen absolute arguments; zero/clamped-no-change deltas do not write. There are
still no automatic mutation retries. Next/previous are single native commands;
a positive SOAP response plus fresh state is their completion evidence, not a
claim that the track index must change at a boundary. Uncertain dispatch blocks
further requests for that target. Source omission plans no queue operations.
Source replacement with omitted transport restores its preflight playing category.

Topology subscriptions invalidate the list only; a new full snapshot determines
eligibility. Polling repairs missed events. Identity and independence are rechecked
before every operation, and the HTTP boundary independently verifies the compiled
Office UUID/name authorization. These checks reduce external-controller races;
they cannot make topology changes atomic with a SOAP dispatch.

Topology event subscription/renewal follows the HTTP GENA messages in the
[UPnP Device Architecture, section 4](https://openconnectivity.org/upnp-specs/UPnP-arch-DeviceArchitecture-v2.0-20200417.pdf).
The device honors the granted subscription duration and periodically retries a
failed subscription; events invalidate snapshots rather than installing state.
