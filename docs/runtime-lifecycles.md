# Runtime lifecycle and recovery

## Independent machines and effects

The runtime uses four independent Boost.Ext SML 1.2.0 machines. They contain explicit event/guard/action tables, monotonic millisecond inputs, and injected effects. The portable headers live in `SurfaceDevice/src` and contain no Arduino, HTTP, queue, logging, or sleep calls. There is no global or nested machine, and playback/UI state remains in AppState and the board presentation models.

| Machine | Transitions | Effects |
| --- | --- | --- |
| `WorkerLifecycle` | Idle → Running on accepted Submit; Running → Idle on matching success/failure, deadline, network loss, unavailable worker, or shutdown | Started, terminal outcome, stale-result diagnostic |
| `WifiLifecycle` | Unconfigured → Connecting when configuration exists; Connecting → Online on a current connection result or Backoff on failure/deadline; Online → Backoff on loss; Backoff → Connecting when due; removal/shutdown → Unconfigured | Begin connection, disconnect, availability change |
| `SonosHealth` | Offline → Discovering when network becomes available; Ready → Discovering for fresh topology before work; Discovering → Ready on current discovery or Backoff on failure/deadline; session failure → Backoff; due retry → Discovering; network loss/shutdown → Offline | Request discovery, invalidate authority, request authoritative reconciliation |
| `SubscriptionLifecycle` | Inactive → Subscribing after a publisher is discovered; valid response → Healthy; renewal due → Renewing; failed/invalid response, deadline, or lease expiry → Backoff; due retry → Subscribing; address change → fresh Subscribing; network loss/shutdown → Inactive | Request subscribe/renew, invalidate topology on a current NOTIFY |

`RuntimeCoordinator` owns the four peer machines and their composition: admission, discovery binding/results, job outcomes, automatic scheduling, and pending effects. It contains no transition table and uses each machine’s event and `tick()` methods. Availability, discovery results, and job failures remain explicit events between peers. Firmware callers serialize coordinator calls and shared publication with `stateMutex`; `Runtime.cpp` supplies platform adapters, the task/queue, and local state publication. Effects record pending work or local facts; Wi-Fi, HTTP, NVS, and serial adapters act outside the mutex. Nonblocking queue admission runs under the mutex to commit accepted work and room selection together. Only the main task persists room preference, so an expired worker cannot overwrite a newer selection through a late NVS write.

SML is pinned as a single upstream header and license by `setup`; the flat `surface_sml.hpp` filename permits Arduino library discovery. Host tests use the same transition tables and `RuntimeCoordinator` composition as firmware. The library is a transition implementation detail, not part of the persisted configuration or card format.

## Bounded work and authority

The deliberate runtime contract is one admitted active job and one physical Sonos task. Each accepted job has a monotonically increasing 64-bit ID, start time, and deadline. `busy` is derived from Running. The UI/main loop checks the worker deadline independently of the task, and HTTP admission checks it again without relying on a timely main-loop tick. Stale success/failure cannot terminate a newer job. Publication additionally requires the currently active job and selected UUID.

Local input, room, and UI-context validation completes before reserving and queuing a job. Local rejections create no worker lifecycle outcome and request no network reconciliation. Busy takes precedence over a temporary discovery/recovery notice. Explicit refresh, room, and queue-page requests respect Sonos backoff and report recovery with its pending retry time; they cannot bypass it.

The current experimental budgets are 45 seconds for reads/discovery and 90 seconds for mutation jobs, including queue wait and topology work. Discovery also has its own 45-second deadline. Every terminal event releases logical admission. An expired task unwinds cooperatively; a newer job can wait in the single queue while it does so. Tasks are never forcibly deleted while holding SDK locks. Initial task/queue allocation failure retries at a bounded 30-second cadence.

`RuntimeCoordinator` wraps `RuntimeAdmission` as the shared host/firmware authority fence. Control/discovery SOAP and identity HTTP need an active job, observed network readiness, and the matching discovery generation. Subscription HTTP has its own request identity/deadline; artwork retains its independent generation lifecycle. Discovery reads require an unexpired discovery deadline; mutation dispatch additionally requires Ready. `GuardedHttp` independently retains the runtime read-only gate, configured-room permission, and frozen destination UUID checks. The adapter still rechecks independent topology before operations. No old intent is scheduled by recovery.

`DeadlineClient` also bounds the physical socket work. Reads, peeks, writes, and connection attempts honor cancellation/deadline; progress from trickled headers, chunks, or partial writes cannot extend the job indefinitely. Speaker connections use discovered numeric addresses and avoid DNS. Connect waits remain bounded by the smaller of their configured timeout and remaining lifetime; cancellation during an already-started connect can take up to that bounded wait. SOAP connections cap cumulative response bytes at 64 KiB plus 8 KiB framing, and bodies remain capped at 64 KiB. Subscription responses cap at 8 KiB. The NOTIFY listener caps the complete header read at 8 KiB and 500 ms.

Cancellation closes later dispatch and discards late state publication. After synchronous work returns, the worker retires its cancelled Application result before reusing the session: prior observations remain stale, cancelled mutations retain an uncertain outcome, and cancelled reads retain the previous command outcome. A later reconciliation cannot republish a rejected late success. An already-dispatched command may have affected the speaker; neither cancellation nor recovery rolls it back or repeats it.

## Automatic recovery and stale observations

Wi-Fi connections have a 15-second deadline and deterministic 1, 2, 4, 8, 16, then 30-second retries. Successful connection resets connection backoff. Arduino automatic reconnect is disabled; the application owns connection attempts and availability events. Background transitions remain unrelated to the physical inactivity/wake contract.

Discovery failures discard the cached discovery address and retry with the same capped backoff. Discovery success resets discovery-failure backoff. Repeated session failures retain their separate backoff across successful topology reads until an authoritative session read succeeds. This covers a speaker that still answers topology while its playback/session endpoint remains unavailable. SSDP only runs as part of bounded discovery work when a cached topology host fails or has been invalidated.

A successful discovery with no eligible configured target is a failed job, not a Sonos reachability failure. Missing, grouped, or unconfigured targets keep Sonos health Ready, preserve the cached discovery host and healthy subscription, and keep the specific room-selection warning visible. The same rule applies when an accepted intent’s frozen UUID is no longer eligible. Normal ten-second polling continues so a returning room can rejoin without recovery backoff or repeated SSDP. Discovery failures and failed session reads or commands against a usable target enter Sonos recovery.

Transient network/discovery failure retains room labels, the selected UUID, and the last displayable observation while marking it stale and removing queue-page authority. Retained room bindings cannot authorize mutations. Fresh successful topology replaces the room projection using the existing allowlist/fallback rules; an actually absent/grouped room is still unavailable. Recovery reads replace stale playback facts automatically. Normal polling remains at a nominal ten-second interval and continues when subscriptions fail. Automatic recovery submissions are rate-limited, and recovery needs no manual refresh or reboot.

`Application::reconcile()` retires prepared transport state, reads fresh same-UUID playback, and verifies independent topology. Failure retains the stale observation and uncertainty block. Success clears the block for a newly requested command while preserving the previous request outcome. It never resumes old operations. Ordinary `refresh()` alone does not clear uncertainty. Optional queue-page failure retains basic playback, reports `AppState.queueError`, and leaves an otherwise successful job at lifecycle Success without scheduling another reconciliation. Queue pages are not a prerequisite for recovery/control.

Subscriptions use a five-second request deadline, validated positive granted lifetimes capped at the requested 300 seconds, and renewal at 80% of the granted lease. Failure or expiry discards SID authority and retries a fresh subscription with 1–30-second backoff. Network loss discards the lease immediately even if the task is occupied. Old responses and expired/mismatched SID notifications cannot resurrect a subscription. Listener startup still waits for initialized Wi-Fi. Polling and reconciliation supply basic functionality independently of event delivery.

## Diagnostics and deterministic fault tests

USB `lifecycle-status` reads compact snapshots without Sonos work: worker identity/deadline/outcome, Wi-Fi state/retries, Sonos discovery generation/last success, and subscription lease/retry/last-NOTIFY times. It exposes SID presence only, never SID contents or credentials. Serial transition records include state changes and reasons; ordinary polling does not produce lifecycle transition logs unless it actually transitions.

`node --run check` runs the ordinary lifecycle examples and 100,000 deterministic event steps per lifecycle and composed runtime harness under address/undefined-behavior sanitizers with warnings as errors. Invariants cover ownership/terminal uniqueness, generation fencing, bounded connection/discovery/lease state, mutation authority, and no mutation replay. Fakes inject missing/delayed connections, empty/recovered SSDP, changed speaker addresses, HTTP failures/timeouts, late callbacks, session/queue failures, renewal failure, and stale notifications. Separate socket fixtures cover continuous header/body/chunk progress and partial writes.

The composed harness periodically enters a bounded fault-free window during both normal and stress runs. Its fake Wi-Fi connects within one second and discovery, HTTP, and subscription requests succeed. Only coordinator service calls and the shared automatic-job predicate drive recovery, with no user refresh or command. Within the connection, backoff, discovery, job, and polling budgets, the harness requires an Idle worker, Online Wi-Fi, Ready Sonos, a Healthy subscription, a fresh displayed observation, and at least one automatic read. This checks automatic recovery as well as safety invariants.

For heavier reproduction:

```sh
node --run stress
node --run stress -- --seed 12345 --steps 1000000
```

The default runs one million steps per model/harness for three fixed seeds. `--seed` accepts an unsigned decimal or hexadecimal 64-bit value; `--steps` accepts 1–10,000,000. Failure output includes the seed, step, last 32 events, state, and relevant context. The normal suite and stress task use only fakes; no Sonos mutations, physical gestures, or persistent device changes occur.

These contracts do not claim recovery from a CPU crash, a true SDK deadlock, failure before Arduino startup, or power loss. Hardware sleep/wake behavior and unresolved native USB/early-boot limits remain in [hardware](hardware.md). Physical speaker/network timing remains useful validation, but deterministic host failures do not depend on owner-timed scenarios.
