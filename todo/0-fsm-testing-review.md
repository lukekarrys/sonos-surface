# Review of the uncommitted FSM lifecycle work — spec for the next agent

This reviews the uncommitted tree against `todo/0-fsm-testing.md`. It changes no code. It is written so an agent can execute it directly. Read `AGENTS.md`, `todo/0-fsm-testing.md`, and `docs/runtime-lifecycles.md` first; all of their constraints still apply (no giant/nested FSM, no UI redesign, no migration or diary docs, clean breaking changes, keep `node --run check` green).

## Verified baseline

| Check | Result |
| --- | --- |
| `node --run check` | passes (633 behavioral checks; 100,000-step chaos per lifecycle and for the composed runtime harness) |
| `node --run build:stick-s3` | builds, 0 warnings |
| `node --run build:ws-1.8` | builds, 0 warnings |
| `node --run stress` | not run in this review; run it at the end of your work |

## What the work delivers (accepted as-is)

- Boost.Ext SML 1.2.0 is pinned by `scripts/setup.ts` as a single flat header in `SurfaceSml`; no Boost distribution, no competing FSM library. This satisfies todo section 1.
- Four independent peer machines with explicit tables, injected effects, monotonic caller-supplied clocks, snapshot structs, centralized invariants, and name helpers: `WorkerLifecycle.h`, `WifiLifecycle.h`, `SonosHealth.h`, `SubscriptionLifecycle.h`. No machine owns another; cross-machine communication is explicit events. Sections 2, 3, 4, 10, 14, 18, 20, 21, 23 are met.
- `busy` is derived from Running (`workerBusy()`), jobs carry monotonic 64-bit IDs, start time, and deadline, and stale completions are rejected and counted. Sections 4, 5, 6 are met.
- `DeadlineClient.h` bounds socket work by the job deadline and cancellation; `RuntimeAdmission.h` is the shared final dispatch fence used by firmware and the fault harness.
- `Application::reconcile()` / `DirectSonos::reconcile()` retire prepared work before an authoritative read; `discardCancelledResult()` prevents a late result from publishing under a newer job. No mutation replay anywhere.
- Host tests: per-lifecycle example tests plus seeded chaos with independent reference models, a composed fault harness (`tests/runtime_fault_test.cpp`), socket fixtures (`tests/deadline_client_test.cpp`), `node --run stress -- --seed --steps`. Sections 7, 8, 13, 24, 25 are met.
- Transition logging (state changes only) and a USB `lifecycle-status` snapshot without secrets. Section 22 met.
- Documentation is durable (no diary), and `docs/runtime-lifecycles.md` is a reasonable architecture page.

Do not re-litigate the machine boundaries, states, events, budgets, or backoff policy. The findings below are about the composition glue in `libraries/SurfaceDevice/src/Runtime.cpp` and about what the host harness actually covers.

## Findings, ordered by priority

Line numbers refer to the current uncommitted `libraries/SurfaceDevice/src/Runtime.cpp`.

### F1 (high) — an unavailable or unconfigured target is reported as a Sonos session failure

Evidence: `Runtime.cpp:794` computes `sessionOk = discovered.ok && target.eligible && !target.address.empty()` and `Runtime.cpp:801` passes `unhealthy = !sessionOk` to `finishJob`. `RoomSelection::update` (`libraries/SurfaceCore/src/Rooms.cpp`) keeps only eligible rooms, so `selection.selected()` is null whenever no configured room is eligible: all rooms grouped, a rename that breaks the display-ID binding, or a config key that matches nothing. An intent whose bound UUID left the eligible list hits the same path.

Consequence: every job in that condition emits `SessionFailure` → `SonosHealth` Backoff → `invalidateAuthority()` (`Runtime.cpp:98`) which discards the SSDP host, tears down the topology subscription, and overwrites `sharedState.refreshError` with "Sonos unavailable; recovering" (`Runtime.cpp:103`), hiding the actual `CONFIG_WARNING` such as `ROOM MISSING: office`. Polling stops until `RetryDue`, so the cadence degrades from 10 s to the escalating backoff (max 30 s), each retry runs SSDP (todo section 16 says do not hammer SSDP), and the subscription is re-created every cycle. The Sonos network is healthy the whole time. Sonos health must model reachability of Sonos, not eligibility of the selected room.

Required behavior:

- `unhealthy` is true only when a usable target existed (`discovered.ok && target.eligible && !target.address.empty()`) and the session read or command failed. An absent, grouped, or unconfigured target is a Failure outcome with Sonos staying Ready and ordinary 10 s polling continuing, so a room that rejoins is noticed within one poll.
- The unavailable publication must keep the selection warning (`ROOM MISSING …`, `ROOM UNAVAILABLE …`, `Selected room unavailable/grouped`) visible; `invalidateAuthority()` must not overwrite a more specific `refreshError` for this case (it is fine for real reachability failures).
- Keep the existing behavior for a reachable target whose HTTP fails: that is a session failure and must rediscover.

Test to add (host): with rooms discovered but none eligible, run three consecutive automatic jobs and assert Sonos stays Ready, no `invalidateAuthority`, no SSDP host discard, subscription stays Healthy, the warning text survives publication, and the next poll is scheduled at the normal interval. Add the same for an intent whose target UUID is missing from the eligible list.

### F2 (high) — the runtime composition is untested; the fault harness re-implements it and diverges

The machines are shared with firmware, but the wiring in `Runtime.cpp` (effects structs at lines 55–120, `reserveJob`/`finishJob` at 175–199, `serviceWifi` 220–263, `serviceSonos` 264–282, the worker's discovery binding at 611–640 and result handling at 667–700, and the automatic-job predicate at `Runtime.cpp:1331`) is not compiled on the host. `tests/runtime_fault_test.cpp` builds its own `RuntimeFixture` that re-implements the wiring, and it differs from the firmware in ways that matter:

1. Publisher invalidation: firmware sends the subscription `NetworkUnavailable` (`subscriptionUnavailableLocked`, line 133); the fixture sends `NetworkAvailable{now, ""}` when the network is up.
2. Cut pending mutation: firmware sets `status = "uncertain"` and `recoveryRequired` (line 70–76); the fixture sets `status = "failed"`. Firmware is right.
3. Discovery binding: firmware binds `job->discoveryId` only when Sonos is Discovering and fails the job otherwise (line 630); the fixture binds whatever `discoveryId` exists.
4. Session failure after deadline: firmware raises it from the `finished` effect for Timeout/NetworkLost/Unavailable/Shutdown (line 70–71); the fixture raises it in `tick()`.
5. The fixture never models the ineligible-target path, so F1 is invisible to it.
6. The fixture never models the main-loop scheduling predicate (`discover || (Ready && (reconciliationPending || interval))`, 1 s rate limit), so success criterion 9 (no manual refresh) is not proven; only safety invariants are asserted, never liveness.

Required change: extract the composition into a portable header, `libraries/SurfaceDevice/src/RuntimeCoordinator.h`, compiled by both firmware and the host harness. It is glue, not a state machine: it must contain no transition table and must not merge or nest the four machines. Suggested shape (adjust names freely, keep the split):

```cpp
class RuntimeCoordinator {
  WorkerLifecycle worker; WifiLifecycle wifi; SonosHealth sonos; SubscriptionLifecycle subscription;
  // the four effects structs from Runtime.cpp move here and only record facts
public:
  // main task, caller holds stateMutex
  void configAvailable(uint64_t now);
  void shutdown(uint64_t now);
  void serviceWifi(uint64_t now, bool observedOnline);   // returns nothing; see drain()
  void service(uint64_t now);                             // worker deadline + sonos/subscription tick() with logging
  uint64_t reserveJob(uint64_t now, bool refresh);
  bool automaticJobDue(uint64_t now, ...);                // the predicate at Runtime.cpp:1331
  // worker task, caller holds stateMutex
  struct Bind { uint64_t discoveryId; bool discardHost; }; Bind bindDiscovery(uint64_t now, uint64_t jobId);
  void discoveryResult(uint64_t now, uint64_t jobId, uint64_t discoveryId, bool ok, const std::string& host);
  bool finishJob(uint64_t now, uint64_t jobId, JobOutcomeInput);   // encodes the F1 decision
  bool jobActive(uint64_t id, uint64_t now, bool stopping) const;
  bool dispatchAllowed(...) const;                        // wraps runtimeDispatchAllowed
  // platform work emitted by effects, consumed after releasing the mutex
  struct Pending { bool connect, disconnect; std::optional<SubscriptionRequest> subscribe; std::vector<std::string> transitions; bool reconcile; };
  Pending drain();
  Snapshots snapshots() const;                            // for lifecycle-status and tests
};
```

`Runtime.cpp` keeps only platform code: the mutex around every coordinator call, `WiFi.*`, `HTTPClient`/`JobNetworkClient`, the FreeRTOS queue and task, NVS, serial, and the `AppState`/`RoomSelection` mutations the coordinator asks for (or pass references to them into the coordinator; either is acceptable, but the decision logic must live in the header).

Then rewrite `RuntimeFixture` to own a `RuntimeCoordinator` plus `FaultHttp`/`FaultTransport`, deleting the re-implemented effects and `drain()`. Keep the existing scenarios and the chaos loop; they should pass unchanged in intent.

Add a liveness invariant to the chaos loop: every N steps enter a fault-free window (Wi-Fi answers `beginConnect` with `Connected` within 1 s, discovery succeeds, HTTP succeeds, subscription requests succeed), then drive only `service()`/`serviceWifi()` and `automaticJobDue()` with no user events for a bounded simulated time (`ConnectBudgetMs + 2 × MaximumBackoffMs + DiscoveryBudgetMs + job budget + 10 s poll`). Assert worker Idle, Wi-Fi Online, Sonos Ready, subscription Healthy, `display.observed.stale == false`, and at least one automatic read happened. This is the executable form of success criteria 7 and 9.

If you judge the full extraction too large for one pass, the minimum acceptable alternative is to move the pure decision functions (`sessionOutcome` for F1, `bindDiscovery`, `automaticJobDue`, the `finished` side-effects) into `RuntimeAdmission.h` and unit-test them, and to fix divergences 1–4 in the fixture so it mirrors firmware. Prefer the full extraction; the bugs in F1, F4, and F7 are exactly the class the current harness cannot see.

### F3 (medium) — "Sonos unavailable; recovering" is shown for ordinary busy rejections

`submit()` (`Runtime.cpp:826`) and `submitToggle()` (`Runtime.cpp:959`) check `sonosUsable()` before `reserveJob()`. Every worker job starts by processing `RefreshRequested`, which moves Sonos from Ready to Discovering for the discovery phase of the job, so a card tap or button during a routine poll is rejected with the recovery notice and the log line "input rejected: Sonos authority unavailable" instead of "Busy; input ignored".

Fix: check the worker first (busy wins), then Sonos usability. Keep the Sonos check; it is the correct answer when the worker is idle and Sonos is Offline/Backoff.

### F4 (medium) — explicit refresh, room-next, room-select, and queue-page requests are dropped silently outside Discovering

`Runtime.cpp:630` fails any job whose pickup did not bind a discovery generation, which is every job submitted while Sonos is Backoff or Offline (RefreshRequested is intentionally ignored outside Ready, and the Sonos test asserts input must not bypass failure backoff; keep that). There is no log and no notice, `rooms` over USB prints nothing, and `selectRoom` has already cleared the observation, so the UI shows an empty room until the backoff retry.

Fix: log the reason with the pending `retryAt`, set a transient notice ("Sonos recovering; retrying in Ns"), and do not treat this as a Failure that requests reconciliation (see F5). Do not shortcut the backoff.

### F5 (medium) — local rejections count as job failures and trigger a network read

`reserveJob()` runs before validation in `submit()`, `selectRoom()`, and `submitToggle()`. Rejections at `Runtime.cpp:864`, `896`, `947`, `982` call `finishJob(jobId, false)`, which produces outcome Failure, increments `completedJobs`, logs a Running → Idle transition, and sets `reconciliationPending` (`Runtime.cpp:69`), which schedules a Sonos read within 1 s. A parse error or "UI rejected: room/content differs" is purely local and must cause no network activity.

Fix: validate first and make `reserveJob` + `xQueueSend` the last, atomic step under the mutex, so no job exists to fail. Do not add a new outcome to `WorkerLifecycle` for this; the machine should only see jobs that were actually queued.

### F6 (medium) — `serviceSonos` re-implements the tested `tick()` methods

`Runtime.cpp:264–282` duplicates `SonosHealth::tick` and `SubscriptionLifecycle::tick` inline (to obtain logging), so the firmware does not run the code the tests exercise. Replace with the real `tick()` calls wrapped by a before/after snapshot diff for logging, exactly as `serviceWifi` already does. Give each machine one `xxxEventLocked` helper and route the boot-time `ConfigAvailable` through it instead of the hand-written log at `Runtime.cpp:1201`. This collapses naturally into F2.

### F7 (low) — a bound discovery whose job dies without a result stalls Sonos for 45 s

After `bindDiscovery` clears `discoveryPending`, the exits at `Runtime.cpp:611` (job no longer active) and `667` (inactive after discovery) call `finishJob(false)` without `DiscoverySucceeded`/`DiscoveryFailed`. Timeout, NetworkLost, Unavailable and Shutdown already raise `SessionFailure` from the `finished` effect, so only the plain Failure path is affected, but in that case Sonos stays Discovering with `discoveryPending == false`, the automatic-job predicate cannot fire (it needs `discoveryPending` or Ready), and recovery waits for the full discovery deadline. Fix: any job exit after binding without a result must emit `DiscoveryFailed{discoveryId}`. Put this inside the coordinator's `finishJob` so the harness covers it.

### F8 (low) — an optional queue-page failure fails the whole job and requests reconciliation

`Runtime.cpp:801` passes `sessionOk && !queueFailed` as success. `docs/runtime-lifecycles.md` says queue-page failure is not a prerequisite for recovery or control. Treat queue failure as lifecycle Success (the `AppState.queueError` already carries the failure) so it does not trigger an extra read.

### F9 (low) — small simplifications in `EspHttp::dispatch`

- `Runtime.cpp:355` repeats the `WiFi.status()` check that `allowed()` already performed at line 344.
- `EspHttp::nowMs()` at `Runtime.cpp:417` duplicates the free `nowMs()`.
- `Sink::write` and the `JobNetworkClient` `allowed` callback each take `stateMutex` per 1 KiB chunk. Acceptable on the ESP32; leave it, but do not add more per-byte locking.

### F10 (low) — documentation touch-ups

- `README.md` and `docs/runtime-lifecycles.md` show a specific seed (`1592594996`) as the stress example. Todo section 30 excludes transient seeds; use an obviously illustrative value such as `--seed 12345`.
- After F2, update the sentence "Host tests use the same transition tables as firmware" in `docs/runtime-lifecycles.md` to state that the composition (coordinator) is shared too, and describe the liveness check in the "Diagnostics and deterministic fault tests" section.
- After F1, `docs/policy.md` and `docs/runtime-lifecycles.md` should say that an ineligible/missing configured room keeps Sonos health Ready and normal polling; only reachability failures back off.

### F11 (info) — firmware-only behavior that host tests cannot see

Not blockers; note them in the final report as remaining device-validation items rather than fixing blind.

- `DeadlineClient` calls `this->setTimeout(0)` on cancellation. On the pinned `NetworkClient`, `setTimeout(uint32_t seconds)` hides `Stream::setTimeout(unsigned long ms)`; it compiles and the reads use `MSG_DONTWAIT` and `DeadlineSocket::write` uses `MSG_DONTWAIT`, so behavior should be correct, but a single device observation of a stalled speaker response being cut at the job deadline (not at the 8 s inactivity timeout) is the only way to confirm the `HTTPClient` + `NetworkClient` interplay.
- Wi-Fi auto-reconnect is now disabled and the application owns 15 s connect budgets with 1–30 s backoff. The measured 291 s outage recovery in `docs/hardware.md` predates this; the doc already says so. A physical AP-off/on check is useful but is owner-timed and therefore optional under todo section 27.
- The worker task keeps its 24576-byte stack while now constructing `JobNetworkClient` with three `std::function`s per request; watch the heartbeat heap line during any device run.

## Execution order

1. F2 extraction (mechanical move, no behavior change), get `check` green with the rewritten fixture driving the real coordinator.
2. F1, F5, F3, F4, F7, F8 inside the coordinator, each with a host test that fails before and passes after.
3. F6 and F9 cleanups, then F10 docs.
4. Add the liveness window to the chaos loop and to `stress`.
5. Gates: `node --run format`, `node --run format:cpp`, `node --run check`, `node --run check:full`, `node --run stress`. Keep VS Code diagnostics green.

## Do not

- Add states, events, or a "Degraded" mode to any machine to work around F1; the machines are correct, the glue is wrong.
- Let an explicit user refresh bypass Sonos backoff.
- Reintroduce a global `busy` flag or any transition logic into `Runtime.cpp`.
- Add reboot-based recovery, LVGL, UI redesign, or migration/compat paths.
- Commit this review file or any session log as durable documentation; delete it when the work is done.

## Final report expectations

Follow todo section 34. In addition, state for each finding above whether it was fixed, with the host test that proves it, and list the coordinator API that replaced the `Runtime.cpp` glue.
