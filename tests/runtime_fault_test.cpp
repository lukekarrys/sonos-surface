#include "RuntimeCoordinator.h"
#include "lifecycle_test_support.h"
#include <SurfaceSonos.h>
#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <string>

using namespace surface;
using namespace surface::device;
namespace wifi = surface::device::wifi_lifecycle;
namespace sonos = surface::device::sonos_health;

struct RuntimeFixture;
struct FaultHttp : GuardedHttp {
  RuntimeFixture& runtime;
  uint64_t jobId = 0, discoveryId = 0, reads = 0, mutations = 0;
  std::string address, identity = "RINCON_A";
  bool failRead = false, failQueue = false, explicitFailure = false, loseResponse = false;
  std::map<uint64_t, unsigned> writes;
  explicit FaultHttp(RuntimeFixture& value) : runtime(value) {
    readOnly = false;
    targetAllowed = true;
    target = identity;
  }
  uint64_t nowMs() override;
  void pollWait(uint32_t ms) override;
  HttpResponse dispatch(const std::string&, const std::string& action, const std::string&) override;
};
// Protocol parsing/ordering is covered by core_test's DirectSonos fixtures. This
// tiny transport exercises Application plus the actual final HTTP safeguards
// while faults alter the independent runtime models around each dispatch.
struct FaultTransport : SonosTransport {
  FaultHttp& http;
  uint32_t verificationDelay = 0;
  explicit FaultTransport(FaultHttp& value) : http(value) {}
  Result refresh(PlaybackState& state) override {
    auto result = http.request("/read", "urn:AVTransport#GetTransportInfo", "");
    if (result.status != 200)
      return Result::fail(result.error);
    state.targetId = "RINCON_A";
    state.known = true;
    state.stale = false;
    state.room = "Office";
    state.title = "Last observed song";
    state.playback = "PLAYING";
    state.observedAtMs = http.nowMs();
    return {};
  }
  Result queue(uint32_t start, uint32_t, QueuePage& page) override {
    auto result = http.request("/queue", "urn:ContentDirectory#Browse", "");
    if (result.status != 200)
      return Result::fail(result.error);
    page.targetId = "RINCON_A";
    page.start = start;
    page.revision = 1;
    return {};
  }
  Result prepare(const ResolvedIntent&) override {
    PlaybackState state;
    return refresh(state);
  }
  Result execute(Operation operation) override {
    if (operation != Operation::Next)
      return Result::fail("Fixture only executes Next");
    const auto response = http.request("/control", "urn:AVTransport#Next", "");
    return response.status == 200
               ? Result{}
               : Result::fail(response.error, response.status == 0 && !response.notSent);
  }
  Result verify(const ResolvedIntent&, PlaybackState& state) override {
    const auto result = refresh(state);
    if (verificationDelay)
      http.pollWait(verificationDelay);
    return result;
  }
};

// The fixture supplies only platform responses. All lifecycle events, admission,
// discovery binding, result policy, and automatic scheduling run in the same
// coordinator as firmware.
struct RuntimeFixture {
  LifecycleTrace trace;
  uint64_t now = 0, connectCalls = 0, disconnectCalls = 0, accepted = 0;
  uint64_t automaticReads = 0, hostDiscards = 0, livenessWindows = 0;
  bool stopping = false, observedOnline = false;
  std::string selectedId = "RINCON_A", learnedAddress, nextAddress = "192.168.1.2";
  std::set<uint64_t> attempted;
  std::deque<SubscriptionRequest> subscriptionRequests;
  AppState display;
  RuntimeCoordinator coordinator{display};
  FaultHttp http{*this};
  FaultTransport transport{http};
  Application app{transport, {"RINCON_A", {}, 1}, [this](const AppState& state) {
                    publish(http.jobId, state);
                  }};

  explicit RuntimeFixture(uint64_t seed) : trace(seed) { display.observed.targetId = selectedId; }
  WorkerSnapshot jobs() const { return coordinator.snapshots().worker; }
  WifiSnapshot network() const { return coordinator.snapshots().wifi; }
  SonosSnapshot health() const { return coordinator.snapshots().sonos; }
  SubscriptionSnapshot events() const { return coordinator.snapshots().subscription; }
  bool usable(uint64_t generation) const {
    return coordinator.sonosUsable() && health().discoveryId == generation;
  }
  std::string snapshot() const {
    const auto j = jobs();
    return "now=" + std::to_string(now) + " worker=" + (j.running ? "Running" : "Idle") +
           " job=" + std::to_string(j.jobId) + " deadline=" + std::to_string(j.deadline) +
           " wifi=" + wifiStateName(network().state) + " sonos=" + sonosStateName(health().state) +
           " generation=" + std::to_string(health().discoveryId) +
           " subscription=" + subscriptionStateName(events().state) + " selected=" + selectedId +
           " display=" + display.observed.targetId;
  }
  void check(bool valid, const char* condition) const {
    if (!valid)
      trace.check(false, condition, snapshot());
  }
  void consumePlatform() {
    const auto pending = coordinator.drainWifi();
    if (pending.connect)
      ++connectCalls;
    if (pending.disconnect) {
      ++disconnectCalls;
      observedOnline = false;
    }
    if (const auto request = coordinator.drainSubscription()) {
      subscriptionRequests.push_back(*request);
      if (subscriptionRequests.size() > 32)
        subscriptionRequests.pop_front();
    }
    for (const auto& transition : coordinator.drainTransitions())
      trace.record(transition);
  }
  void tick(uint64_t elapsed = 0) {
    now += elapsed;
    coordinator.serviceWifi(now, observedOnline);
    coordinator.service(now);
    consumePlatform();
  }
  void configure() {
    stopping = false;
    coordinator.configAvailable(now);
    consumePlatform();
  }
  void connect() {
    observedOnline = true;
    coordinator.serviceWifi(now, observedOnline);
    consumePlatform();
  }
  void drop() {
    observedOnline = false;
    coordinator.serviceWifi(now, observedOnline);
    consumePlatform();
  }
  void shutdown() {
    stopping = true;
    coordinator.shutdown(now);
    consumePlatform();
  }
  uint64_t queueJob(bool refresh = true) {
    const auto id = coordinator.enqueueJob(now, refresh, [](uint64_t) { return true; }, stopping);
    if (id) {
      ++accepted;
      check(id == accepted, "monotonic accepted job identity");
    }
    consumePlatform();
    return id;
  }
  void pickup(uint64_t id) {
    const auto binding = coordinator.bindDiscovery(now, id);
    http.jobId = id;
    http.discoveryId = binding.discoveryId;
    if (binding.discardHost) {
      ++hostDiscards;
      learnedAddress.clear();
    }
    consumePlatform();
  }
  uint64_t beginJob(bool refresh = true) {
    tick();
    const auto id = queueJob(refresh);
    if (id)
      pickup(id);
    return id;
  }
  void discovery(bool succeeds) {
    if (coordinator.discoveryResult(now, http.jobId, http.discoveryId, succeeds, nextAddress)) {
      learnedAddress = nextAddress;
      http.address = learnedAddress;
    }
    consumePlatform();
  }
  void finish(uint64_t id, bool success) {
    coordinator.finishJob(now, id, {true, success, false, false});
    consumePlatform();
  }
  bool publish(uint64_t id, const AppState& state) {
    return coordinator.publish(now, id, state, selectedId, "", stopping);
  }
  Result command() {
    check(attempted.insert(http.jobId).second, "one deliberate command per worker job");
    MusicIntent intent;
    intent.transport = TransportCommand::Next;
    return app.submit(resolvePolicy(intent, {"RINCON_A", {}, 1}));
  }
  Result reconcile() {
    const auto writes = http.writes;
    const auto result = app.reconcile();
    check(http.writes == writes, "reconciliation never dispatches mutations");
    return result;
  }
  void ready() {
    configure();
    connect();
    const auto id = beginJob();
    check(id != 0, "startup job accepted");
    discovery(true);
    check(reconcile().ok, "startup authoritative read");
    finish(id, true);
  }
  void subscriptionResult(bool ok, const std::string& sid = "sid-1", uint64_t lease = 1000) {
    coordinator.subscriptionResult(now, events().requestId, ok, sid, lease);
    consumePlatform();
  }
  bool automaticDue() { return coordinator.automaticJobDue(now, true, stopping); }
  void faultFreeWindow() {
    // Shutdown deliberately cannot recover by itself. A model boot restores
    // persisted configuration before the no-input recovery window begins.
    if (stopping)
      configure();
    http.failRead = http.failQueue = http.explicitFailure = http.loseResponse = false;
    transport.verificationDelay = 0;
    const auto oldJob = jobs().jobId;
    const auto readsBefore = automaticReads;
    const auto writesBefore = http.writes;
    constexpr uint64_t bound = wifi::ConnectBudgetMs + 2 * sonos::MaximumBackoffMs +
                               sonos::DiscoveryBudgetMs + RuntimeCoordinator::MutationBudgetMs +
                               RuntimeCoordinator::PollIntervalMs;
    const auto deadline = now + bound;
    trace.record("fault-free window begins; no user refresh or intent");
    while (now < deadline) {
      tick(100);
      // The fake AP answers a pending beginConnect within one second.
      if (network().state == WifiState::Connecting && now - network().since >= 500)
        connect();
      while (!subscriptionRequests.empty()) {
        const auto request = subscriptionRequests.front();
        subscriptionRequests.pop_front();
        if (coordinator.subscriptionActive(request.id, now, stopping))
          coordinator.subscriptionResult(now, request.id, true, "sid-recovered", 300000);
      }
      // An old hung job is left to its real coordinator deadline. Only jobs
      // emitted by the production automatic predicate receive successful fake
      // discovery/HTTP completions; no prior mutation is replayed.
      if (automaticDue()) {
        const auto id = beginJob();
        check(id && id != oldJob, "automatic scheduler queues fresh work");
        discovery(true);
        const auto result = reconcile();
        check(result.ok, "fault-free automatic authoritative read succeeds");
        ++automaticReads;
        finish(id, true);
      }
      consumePlatform();
      invariants();
      if (!jobs().running && network().state == WifiState::Online &&
          health().state == SonosState::Ready && events().state == SubscriptionState::Healthy &&
          !display.observed.stale && automaticReads > readsBefore) {
        check(http.writes == writesBefore, "automatic recovery never replays a mutation");
        ++livenessWindows;
        return;
      }
    }
    check(false, "bounded automatic recovery reaches Idle/Online/Ready/Healthy/fresh display");
  }
  void invariants() const {
    check(workerInvariant(jobs()), "worker invariant");
    check(wifiInvariant(network()), "wifi invariant");
    check(sonosInvariant(health()), "sonos invariant");
    check(subscriptionInvariant(events()), "subscription invariant");
    check(health().networkAvailable == network().networkReady,
          "network event delivered to Sonos health");
    check(!events().networkAvailable || network().networkReady,
          "subscription cannot outlive network");
    check(jobs().accepted == accepted, "only queued work allocates monotonic IDs");
    check(display.observed.targetId == selectedId, "only selected UUID is published");
    check(http.writes.size() == http.mutations, "at most one Next dispatch per accepted job");
  }
};
uint64_t FaultHttp::nowMs() { return runtime.now; }
void FaultHttp::pollWait(uint32_t ms) { runtime.tick(ms); }
HttpResponse FaultHttp::dispatch(const std::string&, const std::string& action,
                                 const std::string&) {
  const bool mutation = !isReadOnlySonosAction(action);
  if (!runtime.coordinator.dispatchAllowed(jobId, discoveryId, runtime.now, runtime.stopping,
                                           mutation))
    return {0, "", "runtime dispatch blocked", true};
  if (mutation) {
    runtime.check(runtime.usable(discoveryId), "mutation requires current usable topology");
    ++mutations;
    runtime.check(++writes[jobId] == 1, "no duplicate mutation dispatch for same job");
    if (loseResponse)
      return {0, "", "mutation response timed out"};
    if (explicitFailure)
      return {500, "<errorCode>701</errorCode>", "explicit speaker failure"};
    return {200, "<NextResponse/>", ""};
  }
  ++reads;
  if (failRead || (failQueue && action.find("#Browse") != std::string::npos))
    return {0, "", "read failed", true};
  if (action.empty())
    return {200,
            "<root><device><deviceType>urn:schemas-upnp-org:device:ZonePlayer:1</deviceType>"
            "<UDN>uuid:" +
                identity + "</UDN><roomName>Office</roomName></device></root>",
            ""};
  return {200, "<ReadResponse/>", ""};
}

void scenarios() {
  {
    RuntimeFixture f(7);
    f.ready();
    const auto id = f.beginJob(false);
    const auto discovery = f.health().discoveryId;
    f.now += sonos::DiscoveryBudgetMs;
    f.check(f.coordinator.jobActive(id, f.now, false) &&
                f.health().state == SonosState::Discovering,
            "worker outlives discovery deadline before model tick arrives");
    f.check(!f.coordinator.dispatchAllowed(id, discovery, f.now, false, false),
            "discovery deadline independently blocks reads before timer callback");
    f.check(!f.coordinator.dispatchAllowed(id, discovery, f.now, false, true),
            "expired discovery never authorizes mutations");
    f.tick();
    f.check(f.health().state == SonosState::Backoff, "delayed discovery timer schedules recovery");
    f.finish(id, false);
    f.invariants();
  }
  {
    RuntimeFixture f(1);
    f.tick(100000);
    f.check(f.connectCalls == 0 && f.health().totalDiscoveries == 0,
            "unconfigured boot stays quiet");
    f.configure();
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
      f.tick(wifi::ConnectBudgetMs);
      f.check(f.network().state == WifiState::Backoff, "connection cannot hang forever");
      f.tick(f.network().retryAt - f.now);
    }
    f.tick(500);
    f.connect();
    f.check(f.network().networkReady && f.health().totalDiscoveries == 1,
            "delayed connection eventually starts discovery");
    auto id = f.beginJob();
    f.discovery(false);
    f.finish(id, false);
    f.check(f.health().state == SonosState::Backoff, "empty SSDP schedules retry");
    f.tick(f.health().retryAt - f.now);
    id = f.beginJob();
    f.discovery(true);
    f.check(f.reconcile().ok, "SSDP recovery reads authoritative state");
    f.finish(id, true);
    const auto observation = f.display.observed;
    f.drop();
    f.check(f.display.observed.title == observation.title && f.display.observed.stale &&
                f.coordinator.snapshots().discardHost,
            "outage retains visible observations and discards address authority");
    f.tick(f.network().retryAt - f.now);
    f.connect();
    f.nextAddress = "192.168.1.99";
    id = f.beginJob();
    f.discovery(true);
    f.check(f.reconcile().ok && f.http.address == f.nextAddress,
            "speaker address change recovers without old mutation replay");
    f.finish(id, true);
    f.invariants();
  }
  for (bool uncertain : {false, true}) {
    RuntimeFixture f(2 + uncertain);
    f.ready();
    auto id = f.beginJob();
    f.discovery(true);
    f.http.explicitFailure = !uncertain;
    f.http.loseResponse = uncertain;
    const auto result = f.command();
    f.check(!result.ok && result.uncertain == uncertain && f.http.writes[id] == 1,
            "explicit failure and lost mutation response stay distinct");
    f.finish(id, false);
    const auto failedId = id;
    const auto requestId = f.app.state().requestId;
    f.tick(f.health().retryAt - f.now);
    id = f.beginJob();
    f.discovery(true);
    f.http.failRead = true;
    f.check(!f.reconcile().ok && f.app.state().recoveryRequired == uncertain,
            "failed reconciliation preserves uncertainty");
    f.finish(id, false);
    f.tick(f.health().retryAt - f.now);
    f.http.failRead = f.http.explicitFailure = f.http.loseResponse = false;
    id = f.beginJob();
    f.discovery(true);
    f.check(f.reconcile().ok && !f.app.state().recoveryRequired &&
                f.app.state().requestId == requestId && f.http.writes.size() == 1,
            "successful reconciliation only observes, never repeats request");
    f.finish(id, true);
    id = f.beginJob();
    f.discovery(true);
    f.check(f.command().ok && f.http.writes[failedId] == 1 && f.http.writes[id] == 1,
            "new deliberate command succeeds after failure");
    f.finish(id, true);
    f.invariants();
  }
  {
    RuntimeFixture f(8);
    f.ready();
    auto id = f.beginJob();
    f.discovery(true);
    const auto retained = f.app.state();
    f.transport.verificationDelay = uint32_t(f.jobs().deadline - f.now);
    f.check(f.command().ok && !f.jobs().running && f.app.state().status == "succeeded",
            "simulate successful verification returning after worker deadline");
    f.check(f.display.status != "succeeded" && f.http.writes[id] == 1,
            "late success cannot publish directly or dispatch twice");
    const auto requestId = f.app.state().requestId;
    const auto publishedStatus = f.display.status;
    f.app.discardCancelledResult(retained, true);
    f.check(f.app.state().status == "uncertain" && f.app.state().recoveryRequired &&
                f.app.state().observed.stale && f.app.state().requestId == requestId &&
                f.display.status == publishedStatus,
            "retire canceled session result without publishing callback");
    const auto cancellationDetail = f.app.state().detail;
    f.finish(id, true);
    f.transport.verificationDelay = 0;
    f.tick(f.health().retryAt - f.now);
    id = f.beginJob();
    f.discovery(true);
    f.http.failRead = true;
    f.check(!f.reconcile().ok && f.display.status == "uncertain" &&
                f.display.detail == cancellationDetail && f.http.writes.size() == 1,
            "failed recovery cannot resurrect expired session success");
    f.finish(id, false);
    f.http.failRead = false;
    f.tick(f.health().retryAt - f.now);
    id = f.beginJob();
    f.discovery(true);
    f.check(f.reconcile().ok && f.display.status == "uncertain" &&
                f.display.detail == cancellationDetail && !f.app.state().recoveryRequired &&
                f.http.writes.size() == 1,
            "successful recovery retains canceled outcome without replay");
    f.finish(id, true);
    id = f.beginJob();
    f.discovery(true);
    f.check(f.command().ok && f.http.writes.size() == 2 && f.app.state().requestId == requestId + 1,
            "new deliberate command follows canceled-result reconciliation");
    f.finish(id, true);
    f.invariants();
  }
  {
    RuntimeFixture f(4);
    f.ready();
    const auto a = f.beginJob();
    f.discovery(true);
    const auto oldGeneration = f.http.discoveryId;
    f.tick(f.jobs().deadline - f.now);
    f.check(!f.jobs().running && f.display.observed.stale,
            "deadline releases worker and stales observation");
    f.tick(f.health().retryAt - f.now);
    const auto b = f.beginJob();
    f.discovery(true);
    AppState late = f.app.state();
    late.observed.title = "Late A must never display";
    f.check(!f.publish(a, late), "late A publication cannot overwrite B");
    f.finish(a, true);
    f.finish(a, false);
    f.check(f.jobs().jobId == b, "late A completion cannot complete B");
    f.check(!f.coordinator.dispatchAllowed(a, oldGeneration, f.now, false, true),
            "expired A cannot dispatch another operation");
    f.check(!f.coordinator.dispatchAllowed(b, oldGeneration, f.now, false, true),
            "new job cannot reuse old topology generation");
    f.selectedId = "RINCON_B";
    Room selected;
    selected.id = f.selectedId;
    selectObservedRoom(f.display, selected);
    f.check(!f.publish(b, late), "accepted work cannot publish into different selected UUID");
    f.finish(b, true);
    f.invariants();
  }
  {
    RuntimeFixture f(6);
    f.ready();
    const auto id = f.beginJob();
    f.check(f.http.request("/control", "urn:AVTransport#Next", "").notSent && f.http.writes.empty(),
            "discovery reads never grant mutation authority");
    f.discovery(true);
    f.http.readOnly = true;
    f.check(f.http.request("/control", "urn:AVTransport#Next", "").notSent && f.http.writes.empty(),
            "read-only dispatch survives healthy lifecycle");
    f.http.readOnly = false;
    f.http.targetAllowed = false;
    f.check(f.http.request("/control", "urn:AVTransport#Next", "").notSent && f.http.writes.empty(),
            "healthy lifecycle cannot enroll an unconfigured target");
    f.http.targetAllowed = true;
    f.http.identity = "RINCON_B";
    f.check(f.http.request("/control", "urn:AVTransport#Next", "").notSent && f.http.writes.empty(),
            "speaker address reuse cannot authorize another UUID");
    f.http.identity = "RINCON_A";
    f.check(f.command().ok && f.http.writes[id] == 1,
            "all independent dispatch safeguards permit requested mutation");
    f.finish(id, true);
    f.invariants();
  }
  {
    RuntimeFixture f(5);
    f.ready();
    auto request = f.events().requestId;
    f.coordinator.subscriptionResult(f.now, request, false, "", 0);
    auto id = f.beginJob();
    f.discovery(true);
    f.check(f.reconcile().ok, "polling works while subscription failed");
    f.http.failQueue = true;
    const auto observed = f.app.state().observed;
    f.check(!f.app.queue(0, 4).ok && f.app.state().observed.title == observed.title &&
                !f.app.state().observed.stale,
            "queue read failure retains authoritative basic playback");
    f.finish(id, true);
    f.tick(f.events().retryAt - f.now);
    request = f.events().requestId;
    f.coordinator.subscriptionResult(f.now, request, true, "sid-1", 1000);
    f.tick(800);
    f.check(f.events().state == SubscriptionState::Renewing, "renewal becomes due");
    f.subscriptionResult(false);
    const auto notifications = f.events().notifications;
    f.coordinator.notify(f.now, "sid-1", f.stopping);
    f.check(!f.events().sidPresent && f.events().notifications == notifications,
            "failed renewal cannot be revived by stale SID");
    id = f.beginJob();
    f.discovery(true);
    f.check(f.reconcile().ok, "polling survives renewal failure");
    f.finish(id, true);
    f.invariants();
  }
}

void unavailableTargetsKeepNormalPolling() {
  // Grouping, a renamed configured room, and a vanished accepted UUID all
  // leave the household/publisher reachable. Exercise actual RoomSelection
  // warnings and publication through the production coordinator.
  for (unsigned missing = 0; missing < 3; ++missing) {
    RuntimeFixture f(100 + missing);
    f.ready();
    f.subscriptionResult(true, "sid-room-warning", 300000);
    RoomSelection selection;
    selection.configured["office"] = {};
    selection.update({{"RINCON_A", "Office", f.nextAddress, "", "", true, "office"}});
    const auto boundId = selection.selectedId;
    const auto originalHost = f.learnedAddress;
    const auto originalDiscards = f.hostDiscards;
    const auto originalSubscription = f.events().totalRequests;
    uint64_t previousPoll = 0;
    for (unsigned poll = 0; poll < (missing == 2 ? 4U : 3U); ++poll) {
      // The intent is accepted while A is eligible, then its frozen UUID
      // disappears during discovery. The following reads are automatic.
      uint64_t id = 0;
      if (missing == 2 && poll == 0) {
        id = f.beginJob(false);
      } else {
        const auto dueAt =
            f.coordinator.snapshots().lastAutomaticJobAt + RuntimeCoordinator::PollIntervalMs;
        f.tick(dueAt - f.now);
        f.check(f.automaticDue(), "normal poll remains scheduled for unavailable room");
        if (previousPoll)
          f.check(f.now - previousPoll == RuntimeCoordinator::PollIntervalMs,
                  "unavailable room uses exactly the ordinary ten-second poll cadence");
        previousPoll = f.now;
        id = f.beginJob();
        ++f.automaticReads;
      }
      f.check(id != 0, "unavailable room job is admitted against healthy household");
      f.discovery(true);
      if (missing == 0)
        selection.update({{boundId, "Office", f.nextAddress, "RINCON_B", "", false, "office"}});
      else if (missing == 1)
        selection.update({{boundId, "Study", f.nextAddress, "", "", true, "study"}});
      else
        selection.update({{"RINCON_B", "Kitchen", f.nextAddress, "", "", true, "kitchen"}});
      f.check(selection.selected() == nullptr, "discovered rooms contain no eligible target");
      const auto warning =
          missing == 2 ? std::string("Selected room unavailable/grouped") : selection.warning;
      f.check(!warning.empty(), "missing/renamed/grouped target has a specific warning");
      auto unavailable = f.display;
      unavailable.observed.stale = true;
      unavailable.refreshError = warning;
      f.check(f.coordinator.publish(f.now, id, unavailable, f.selectedId, warning),
              "unavailable observation publishes its room warning");
      f.coordinator.finishJob(f.now, id, {false, false, false, false});
      f.consumePlatform();
      f.check(f.jobs().lastOutcome == JobOutcome::Failure && f.health().state == SonosState::Ready,
              "missing target is a failed job without a Sonos session failure");
      f.check(!f.coordinator.snapshots().discardHost && f.hostDiscards == originalDiscards &&
                  f.learnedAddress == originalHost,
              "missing target never invalidates authority or discards SSDP host");
      f.check(f.events().state == SubscriptionState::Healthy &&
                  f.events().totalRequests == originalSubscription,
              "missing target retains healthy publisher without resubscription");
      f.check(f.display.refreshError == warning, "specific room warning survives completion");
      f.check(!f.coordinator.snapshots().reconciliationPending,
              "missing target does not schedule extra reconciliation");
      f.tick(RuntimeCoordinator::AutomaticRateLimitMs);
      f.check(!f.automaticDue(), "unavailable target does not cause an immediate retry");
      f.invariants();
    }
    f.check(f.automaticReads == 3, "all three ordinary missing-room polls ran");
  }
}

void busyWinsOverDiscoveryRecovery() {
  RuntimeFixture f(110);
  f.ready();
  const auto id = f.beginJob();
  f.check(f.health().state == SonosState::Discovering, "routine poll discovers current topology");
  for (bool refresh : {false, true}) {
    const auto rejected = f.coordinator.admission(f.now, refresh);
    f.check(!rejected.allowed && rejected.notice == "Busy; input ignored" &&
                rejected.reason == "worker busy",
            "busy wins over Sonos discovery for toggle, intent, and refresh admission");
  }
  f.finish(id, false);
  f.invariants();
}

void recoveryRejectsWithoutClearingRoom() {
  for (bool offline : {false, true}) {
    RuntimeFixture f(120 + offline);
    f.ready();
    if (offline)
      f.drop();
    else {
      const auto id = f.beginJob();
      f.discovery(false);
      f.finish(id, false);
    }
    const auto retained = f.display.observed;
    const auto worker = f.jobs();
    const auto sonosBefore = f.health();
    const auto wifiBefore = f.network();
    const auto reconcile = f.coordinator.snapshots().reconciliationPending;
    unsigned queueCalls = 0;
    for (const auto* action : {"refresh", "room-next", "room-select", "queue-page"}) {
      f.trace.record(action);
      const auto rejected = f.coordinator.admission(f.now, true);
      f.check(!rejected.allowed && rejected.retryAt > f.now &&
                  rejected.notice.find("Sonos recovering; retrying in ") == 0 &&
                  rejected.reason.find("retryAt=" + std::to_string(rejected.retryAt)) !=
                      std::string::npos,
              "recovery rejection exposes its reason and pending retry time");
      f.check(f.coordinator.enqueueJob(f.now, true,
                                       [&](uint64_t) {
                                         ++queueCalls;
                                         return true;
                                       }) == 0,
              "refresh and room/queue inputs cannot bypass recovery backoff");
    }
    f.check(queueCalls == 0 && f.jobs().accepted == worker.accepted &&
                f.jobs().completed == worker.completed &&
                f.coordinator.snapshots().reconciliationPending == reconcile,
            "local recovery rejection allocates no worker and requests no reconciliation");
    f.check(f.health().state == sonosBefore.state && f.health().retryAt == sonosBefore.retryAt &&
                f.network().retryAt == wifiBefore.retryAt,
            "explicit refresh preserves both peers' existing retry schedule");
    f.check(f.display.observed.targetId == retained.targetId &&
                f.display.observed.title == retained.title && f.display.observed.stale,
            "rejected room action preserves stale selected-room observation");
    f.invariants();
  }
}

void boundDiscoveryFailureIsImmediate() {
  RuntimeFixture f(130);
  f.ready();
  const auto id = f.beginJob();
  const auto deadline = f.health().deadline;
  f.check(f.http.discoveryId != 0 && !f.coordinator.snapshots().discoveryPending,
          "worker owns the pending discovery generation");
  f.coordinator.finishJob(f.now, id, {});
  f.consumePlatform();
  f.check(!f.jobs().running && f.health().state == SonosState::Backoff &&
              f.health().lastError == SonosError::DiscoveryFailed && f.health().retryAt < deadline,
          "bound job without a result fails discovery immediately instead of stalling forty-five "
          "seconds");
  f.invariants();
}

void queuedDiscoveryRejectionReportsRecovery() {
  RuntimeFixture f(135);
  f.configure();
  f.connect();
  f.tick(10000);
  const auto id = f.queueJob();
  f.tick(sonos::DiscoveryBudgetMs - 10000);
  f.check(f.jobs().running && f.health().state == SonosState::Backoff,
          "discovery deadline can precede a queued worker's deadline");
  const auto retryAt = f.health().retryAt;
  const auto reconcile = f.coordinator.snapshots().reconciliationPending;
  const auto binding = f.coordinator.bindDiscovery(f.now, id);
  const auto notice = f.coordinator.drainNotice();
  const auto messages = f.coordinator.drainTransitions();
  f.check(binding.discoveryId == 0 && notice &&
              notice->find("Sonos recovering; retrying in ") == 0 &&
              std::any_of(messages.begin(), messages.end(),
                          [&](const std::string& message) {
                            return message.find("queued job rejected:") == 0 &&
                                   message.find("retryAt=" + std::to_string(retryAt)) !=
                                       std::string::npos;
                          }),
          "unbound queued job reports its recovery notice and logged retry time");
  f.coordinator.finishJob(f.now, id, {});
  f.check(f.health().retryAt == retryAt &&
              f.coordinator.snapshots().reconciliationPending == reconcile,
          "queued local rejection leaves backoff and reconciliation unchanged");
  f.check(!f.coordinator.drainNotice(), "queued recovery notice is emitted once");
  f.invariants();
}

void localRejectionsCreateNoWork() {
  RuntimeFixture f(138);
  f.ready();
  const auto before = f.coordinator.snapshots();
  const auto reads = f.http.reads;
  const auto writes = f.http.writes;
  const auto connectCalls = f.connectCalls;
  unsigned queueCalls = 0;
  const auto enqueue = [&] {
    return f.coordinator.enqueueJob(f.now, false, [&](uint64_t) {
      ++queueCalls;
      return true;
    });
  };
  MusicIntent malformed;
  const auto parsed = parseIntent("{ malformed card", malformed);
  if (parsed.ok)
    enqueue();
  f.check(!parsed.ok, "malformed card rejects before enqueue");
  Room room{f.selectedId, "Office", f.nextAddress, "", "", true, "office"};
  f.display.observed.trackUri = "current-track";
  f.display.observed.queueRevision = 1;
  for (unsigned mismatch = 0; mismatch < 5; ++mismatch) {
    BoardEvent event;
    event.targetId = f.selectedId;
    event.trackIdentity = "current-track";
    event.queueRevision = 1;
    if (mismatch == 0)
      event.targetId = "RINCON_B";
    else if (mismatch == 1) {
      event.intent.seekPositionMs = 5000;
      event.trackIdentity = "old-track";
    } else if (mismatch == 2) {
      event.intent.seekPositionMs = 5000;
      event.queueRevision = 0;
    } else if (mismatch == 3) {
      event.intent.queueIndex = 1;
      event.queueRevision = 0;
    }
    const auto matches =
        runtimeUiMatches(mismatch == 4 ? nullptr : &room, f.display.observed, event);
    if (matches)
      enqueue();
    f.check(!matches,
            "stale target, track, queue revision, or missing room rejects before enqueue");
  }
  f.tick(RuntimeCoordinator::AutomaticRateLimitMs);
  const auto after = f.coordinator.snapshots();
  f.check(queueCalls == 0 && after.worker.accepted == before.worker.accepted &&
              after.worker.completed == before.worker.completed &&
              after.reconciliationPending == before.reconciliationPending && !f.automaticDue() &&
              f.http.reads == reads && f.http.writes == writes && f.connectCalls == connectCalls &&
              after.sonos.totalDiscoveries == before.sonos.totalDiscoveries &&
              after.subscription.totalRequests == before.subscription.totalRequests,
          "pure local rejections consume no job ID, completion, reconciliation, or network work");
  f.check(f.coordinator.enqueueJob(f.now, false, [](uint64_t) { return false; }) == 0 &&
              f.jobs().accepted == before.worker.accepted &&
              f.jobs().completed == before.worker.completed &&
              !f.coordinator.snapshots().reconciliationPending,
          "failed platform queue admission also leaves the worker lifecycle untouched");
  const auto id = f.beginJob(false);
  f.check(id == before.worker.accepted + 1, "next valid request gets the next unconsumed identity");
  f.discovery(true);
  f.finish(id, true);
  f.invariants();
}

void optionalQueueFailureIsLifecycleSuccess() {
  RuntimeFixture f(140);
  f.ready();
  f.tick(RuntimeCoordinator::PollIntervalMs);
  f.check(f.automaticDue(), "ordinary automatic queue/read job becomes due");
  const auto id = f.beginJob();
  f.discovery(true);
  f.check(f.reconcile().ok, "basic authoritative read succeeds before optional queue failure");
  f.http.failQueue = true;
  f.check(!f.app.queue(0, 4).ok && !f.display.queueError.empty(),
          "optional queue error is published separately");
  f.coordinator.finishJob(f.now, id, {true, true, true, false});
  const auto reads = f.http.reads;
  f.tick(RuntimeCoordinator::AutomaticRateLimitMs);
  f.check(f.jobs().lastOutcome == JobOutcome::Success && f.health().state == SonosState::Ready &&
              !f.coordinator.snapshots().reconciliationPending && !f.automaticDue() &&
              f.http.reads == reads && !f.display.observed.stale,
          "optional queue failure completes successfully without an extra authoritative read");
  f.invariants();
}

void publicDecisionsHonorDeadlinesBeforeTimerService() {
  {
    RuntimeFixture f(150);
    f.configure();
    f.connect();
    f.now = f.health().deadline;
    const auto rejected = f.coordinator.admission(f.now, true);
    f.check(!rejected.allowed && f.health().state == SonosState::Backoff &&
                rejected.retryAt > f.now && !f.jobs().running,
            "admission observes an expired discovery before the main timer callback");
    f.now = rejected.retryAt;
    f.check(f.automaticDue(), "automatic predicate services due discovery retry itself");
    const auto id = f.beginJob();
    f.discovery(true);
    f.finish(id, true);
    f.invariants();
  }
  {
    RuntimeFixture f(151);
    f.configure();
    f.connect();
    f.tick(10000);
    const auto id = f.queueJob();
    f.now = f.health().deadline;
    const auto binding = f.coordinator.bindDiscovery(f.now, id);
    f.check(binding.discoveryId == 0 && f.health().state == SonosState::Backoff &&
                f.coordinator.drainNotice().has_value(),
            "pickup observes expired discovery and reports recovery without a separate tick");
    f.coordinator.finishJob(f.now, id, {});
    f.invariants();
  }
  {
    RuntimeFixture f(152);
    f.ready();
    const auto id = f.beginJob();
    f.discovery(true);
    f.now = f.jobs().deadline;
    f.check(!f.automaticDue() && !f.jobs().running && f.jobs().lastJobId == id &&
                f.jobs().lastOutcome == JobOutcome::Timeout &&
                f.health().state == SonosState::Backoff,
            "automatic predicate expires worker before deciding whether work is due");
    f.now = f.health().retryAt;
    f.check(f.automaticDue(), "timed-out worker recovers automatically at the existing backoff");
    const auto next = f.beginJob();
    f.discovery(true);
    f.check(f.reconcile().ok, "automatic recovery reads fresh state after unsignaled deadline");
    f.finish(next, true);
    f.invariants();
  }
}

void sharedTicksLogBootAndRecoveryTransitions() {
  RuntimeFixture f(160);
  const auto logged = [&](const std::string& transition) {
    const auto messages = f.coordinator.drainTransitions();
    return std::any_of(messages.begin(), messages.end(),
                       [&](const std::string& message) { return message.find(transition) == 0; });
  };
  f.coordinator.configAvailable(f.now);
  f.check(logged("wifi Unconfigured -> Connecting"),
          "boot ConfigAvailable uses the same transition logger as later Wi-Fi events");
  f.consumePlatform();
  f.connect();
  f.now = f.health().deadline;
  f.coordinator.service(f.now);
  f.check(f.health().lastError == SonosError::DiscoveryTimeout &&
              logged("sonos Discovering -> Backoff"),
          "coordinator calls and logs the shared Sonos deadline tick");
  f.now = f.health().retryAt;
  f.coordinator.service(f.now);
  f.check(logged("sonos Backoff -> Discovering"),
          "coordinator calls and logs the shared Sonos retry tick");
  const auto id = f.beginJob();
  f.discovery(true);
  f.check(f.reconcile().ok, "automatic retry can perform authoritative work");
  f.finish(id, true);
  f.subscriptionResult(true, "sid-shared-tick", 1000);
  f.now = f.events().renewAt;
  f.coordinator.service(f.now);
  f.check(logged("subscription Healthy -> Renewing"),
          "coordinator calls and logs the shared subscription renewal tick");
  f.now = f.events().leaseUntil;
  f.coordinator.service(f.now);
  f.check(f.events().lastError == SubscriptionError::LeaseExpired &&
              logged("subscription Renewing -> Backoff"),
          "coordinator calls and logs the shared subscription lease-expiry tick");
  f.coordinator.service(f.now);
  f.check(f.coordinator.drainTransitions().empty(), "unchanged service polls produce no log noise");
  f.invariants();
}

void chaos(uint64_t seed, uint64_t steps) {
  RuntimeFixture f(seed);
  f.ready();
  for (uint64_t step = 0; step < steps; ++step) {
    f.tick(f.trace.random() % 250);
    const auto event = f.trace.random() % 19;
    const auto before = f.jobs();
    const auto generation = f.health().discoveryId;
    f.trace.record("event=" + std::to_string(event) + " job=" + std::to_string(before.jobId) +
                   " generation=" + std::to_string(generation) + " now=" + std::to_string(f.now));
    switch (event) {
    case 0:
      f.configure();
      break;
    case 1:
      f.connect();
      break;
    case 2:
      f.drop();
      break;
    case 3:
    case 4:
      f.beginJob();
      break;
    case 5:
    case 6:
      f.discovery(event == 5);
      break;
    case 7:
      if (before.running && f.usable(f.http.discoveryId) && !f.attempted.count(before.jobId)) {
        f.http.loseResponse = f.trace.random() % 3 == 0;
        f.http.explicitFailure = f.trace.random() % 3 == 0;
        f.command();
        f.http.loseResponse = f.http.explicitFailure = false;
      }
      break;
    case 8:
      if (before.running) {
        f.http.failRead = f.trace.random() % 3 == 0;
        f.reconcile();
        f.http.failRead = false;
      }
      break;
    case 9:
      f.finish(before.jobId, f.trace.random() % 2);
      break;
    case 10: {
      const auto stale = before.jobId ? before.jobId - 1 : f.accepted;
      f.finish(stale, true);
      f.finish(stale, false);
      f.check(f.jobs().jobId == before.jobId, "stale completion leaves active job intact");
      AppState late = f.app.state();
      f.check(!f.publish(stale, late), "stale job cannot publish");
      break;
    }
    case 11: {
      const auto sub = f.events();
      f.coordinator.subscriptionResult(f.now, sub.requestId, true, "sid-chaos", 1500);
      break;
    }
    case 12:
      f.subscriptionResult(false);
      break;
    case 13: {
      const auto notifications = f.events().notifications;
      f.coordinator.notify(f.now, "sid-stale", f.stopping);
      f.check(f.events().notifications == notifications, "stale SID never invalidates topology");
      break;
    }
    case 14:
      f.nextAddress = f.nextAddress == "192.168.1.2" ? "192.168.1.99" : "192.168.1.2";
      break;
    case 15:
      f.shutdown();
      break;
    case 16:
      if (before.running) {
        f.http.failQueue = true;
        const auto observed = f.app.state().observed;
        f.check(!f.app.queue(0, 4).ok && f.app.state().observed.title == observed.title,
                "queue failure does not erase playback");
        f.http.failQueue = false;
      }
      break;
    case 17:
      f.coordinator.finishJob(f.now, before.jobId, {false, false, false, true});
      break;
    case 18:
      f.finish(before.jobId, false);
      break;
    }
    f.consumePlatform();
    const auto j = f.jobs();
    const auto w = f.network();
    const auto s = f.health();
    const bool authorized = j.running && j.jobId == f.http.jobId && f.now < j.deadline &&
                            !f.stopping && w.state == WifiState::Online &&
                            s.state == SonosState::Ready && s.discoveryId == f.http.discoveryId &&
                            f.http.discoveryId != 0;
    f.check(f.coordinator.dispatchAllowed(f.http.jobId, f.http.discoveryId, f.now, f.stopping,
                                          true) == authorized,
            "real mutation fence matches independent combined authority model");
    f.invariants();
    if ((step + 1) % 1000 == 0)
      f.faultFreeWindow();
  }
  f.tick(sonos::DiscoveryBudgetMs + wifi::ConnectBudgetMs);
  f.check(!f.jobs().running, "final running work always reaches deadline");
  std::cout << "runtime faults: composed scenarios + " << steps << " events seed=" << seed
            << " mutations=" << f.http.mutations << " reads=" << f.http.reads
            << " connections=" << f.network().connections
            << " liveness-windows=" << f.livenessWindows << '\n';
}
int main(int argc, char** argv) {
  scenarios();
  unavailableTargetsKeepNormalPolling();
  busyWinsOverDiscoveryRecovery();
  recoveryRejectsWithoutClearingRoom();
  boundDiscoveryFailureIsImmediate();
  queuedDiscoveryRejectionReportsRecovery();
  localRejectionsCreateNoWork();
  optionalQueueFailureIsLifecycleSuccess();
  publicDecisionsHonorDeadlinesBeforeTimerService();
  sharedTicksLogBootAndRecoveryTransitions();
  chaos(argc > 1 ? std::stoull(argv[1]) : 0x72756e74696d65ULL,
        argc > 2 ? std::stoull(argv[2]) : 100000ULL);
}
