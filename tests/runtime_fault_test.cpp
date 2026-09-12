#include "RuntimeAdmission.h"
#include "SubscriptionLifecycle.h"
#include "lifecycle_test_support.h"
#include <SurfaceSonos.h>
#include <deque>
#include <map>
#include <set>
#include <string>

using namespace surface;
using namespace surface::device;
namespace worker = surface::device::worker_lifecycle;
namespace wifi = surface::device::wifi_lifecycle;
namespace sonos = surface::device::sonos_health;
namespace subscription = surface::device::subscription_lifecycle;

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

// Peer models never own one another. Effects enqueue adapter events; drain()
// delivers them only after the emitting transition has completed.
struct RuntimeFixture : WorkerEffects, WifiEffects, SonosEffects, SubscriptionEffects {
  enum class Message { Online, Offline, PublisherUnavailable };
  LifecycleTrace trace;
  uint64_t now = 0, connectCalls = 0, disconnectCalls = 0, discoveryRequests = 0;
  uint64_t reconciliations = 0, accepted = 0, terminalCount = 0;
  bool stopping = false, reconcilePending = false, topologyPending = false;
  std::string selectedId = "RINCON_A", learnedAddress, nextAddress = "192.168.1.2";
  std::deque<Message> messages;
  std::map<uint64_t, unsigned> terminals;
  std::map<uint64_t, uint64_t> generations;
  std::set<uint64_t> attempted;
  struct SubscriptionRequest {
    uint64_t id;
    bool renewal;
    std::string address, sid;
  };
  std::deque<SubscriptionRequest> subscriptionRequests;
  WorkerLifecycle jobs{*this};
  WifiLifecycle network{*this};
  SonosHealth health{*this};
  SubscriptionLifecycle events{*this};
  AppState display;
  FaultHttp http{*this};
  FaultTransport transport{http};
  Application app{transport, {"RINCON_A", {}, 1}, [this](const AppState& state) {
                    publish(http.jobId, state);
                  }};

  explicit RuntimeFixture(uint64_t seed) : trace(seed) { display.observed.targetId = selectedId; }
  std::string snapshot() const {
    const auto j = jobs.snapshot();
    return "now=" + std::to_string(now) + " worker=" + (j.running ? "Running" : "Idle") +
           " job=" + std::to_string(j.jobId) + " deadline=" + std::to_string(j.deadline) +
           " wifi=" + wifiStateName(network.snapshot().state) +
           " sonos=" + sonosStateName(health.snapshot().state) +
           " generation=" + std::to_string(health.snapshot().discoveryId) +
           " subscription=" + subscriptionStateName(events.snapshot().state) +
           " selected=" + selectedId + " display=" + display.observed.targetId;
  }
  void check(bool valid, const char* condition) const {
    if (!valid)
      trace.check(false, condition, snapshot());
  }
  void started(uint64_t id, uint64_t) override {
    ++accepted;
    check(id == accepted, "monotonic job identity");
  }
  void finished(uint64_t id, JobOutcome outcome) override {
    ++terminalCount;
    check(++terminals[id] == 1, "one terminal callback per accepted job");
    if (outcome != JobOutcome::Success) {
      reconcilePending = true;
    }
    if (outcome != JobOutcome::Success && outcome != JobOutcome::Failure) {
      display.observed.stale = true;
      display.queue.reset();
      if (display.status == "pending") {
        display.status = "failed";
        display.detail = outcomeName(outcome);
      }
    }
  }
  void staleResult(uint64_t) override {}
  void beginConnect(uint64_t) override { ++connectCalls; }
  void disconnect() override { ++disconnectCalls; }
  void availabilityChanged(bool online) override {
    messages.push_back(online ? Message::Online : Message::Offline);
  }
  void discoveryRequested(uint64_t) override { ++discoveryRequests; }
  void invalidateAuthority() override {
    learnedAddress.clear();
    display.observed.stale = true;
    display.queue.reset();
    messages.push_back(Message::PublisherUnavailable);
  }
  void reconciliationRequested() override {
    ++reconciliations;
    reconcilePending = true;
  }
  void request(uint64_t id, bool renewal, const std::string& address,
               const std::string& sid) override {
    subscriptionRequests.push_back({id, renewal, address, sid});
    if (subscriptionRequests.size() > 32)
      subscriptionRequests.pop_front();
  }
  void topologyChanged() override { topologyPending = true; }
  void drain() {
    while (!messages.empty()) {
      const auto message = messages.front();
      messages.pop_front();
      if (message == Message::Online)
        health.process(sonos::NetworkAvailable{now});
      else if (message == Message::Offline) {
        health.process(sonos::NetworkUnavailable{now});
        jobs.process(worker::NetworkUnavailable{});
        events.process(subscription::NetworkUnavailable{now});
      } else if (network.snapshot().networkReady)
        events.process(subscription::NetworkAvailable{now, ""});
      else
        events.process(subscription::NetworkUnavailable{now});
    }
  }
  void tick(uint64_t elapsed = 0) {
    now += elapsed;
    network.tick(now);
    drain();
    const auto before = jobs.snapshot();
    jobs.process(worker::DeadlineExpired{now});
    if (before.running && !jobs.snapshot().running && generations[before.jobId])
      health.process(sonos::SessionFailure{now, generations[before.jobId]});
    health.tick(now);
    events.tick(now);
    drain();
  }
  void configure() {
    stopping = false;
    network.process(wifi::ConfigAvailable{now});
    drain();
  }
  void connect() {
    network.process(wifi::Connected{now, network.snapshot().attemptId});
    drain();
  }
  void drop() {
    network.process(wifi::Disconnected{now, network.snapshot().attemptId});
    drain();
  }
  uint64_t beginJob() {
    tick();
    const auto id = stopping ? 0 : jobs.submit(now, 1000);
    if (!id)
      return 0;
    if (health.snapshot().state == SonosState::Ready)
      health.process(sonos::RefreshRequested{now});
    http.jobId = id;
    http.discoveryId = health.snapshot().discoveryId;
    generations[id] = http.discoveryId;
    return id;
  }
  void discovery(bool succeeds) {
    const auto id = http.discoveryId;
    if (!runtimeJobActive(jobs.snapshot(), http.jobId, now, stopping))
      return;
    if (succeeds)
      health.process(sonos::DiscoverySucceeded{now, id});
    else
      health.process(sonos::DiscoveryFailed{now, id});
    if (succeeds && health.usable(id)) {
      learnedAddress = nextAddress;
      http.address = learnedAddress;
      events.process(subscription::NetworkAvailable{now, learnedAddress});
    }
    drain();
  }
  void finish(uint64_t id, bool success, bool unhealthy = false) {
    tick();
    if (runtimeJobActive(jobs.snapshot(), id, now, stopping)) {
      if (unhealthy)
        health.process(sonos::SessionFailure{now, generations[id]});
      else if (success)
        health.process(sonos::SessionSucceeded{now, generations[id]});
    }
    if (success)
      jobs.process(worker::Success{id});
    else
      jobs.process(worker::Failure{id});
    drain();
  }
  bool publish(uint64_t id, const AppState& state) {
    return runtimeJobActive(jobs.snapshot(), id, now, stopping) &&
           publishSelectedState(display, state, selectedId);
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
    if (result.ok)
      reconcilePending = false;
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
  void invariants() const {
    check(workerInvariant(jobs.snapshot()), "worker invariant");
    check(wifiInvariant(network.snapshot()), "wifi invariant");
    check(sonosInvariant(health.snapshot()), "sonos invariant");
    check(subscriptionInvariant(events.snapshot()), "subscription invariant");
    check(health.snapshot().networkAvailable == network.snapshot().networkReady,
          "network event delivered to Sonos health");
    check(!events.snapshot().networkAvailable || network.snapshot().networkReady,
          "subscription cannot outlive network");
    check(jobs.snapshot().accepted == accepted && jobs.snapshot().completed == terminalCount,
          "worker model matches effect counts");
    check(display.observed.targetId == selectedId, "only selected UUID is published");
    check(http.writes.size() == http.mutations, "at most one Next dispatch per accepted job");
  }
};
uint64_t FaultHttp::nowMs() { return runtime.now; }
void FaultHttp::pollWait(uint32_t ms) { runtime.tick(ms); }
HttpResponse FaultHttp::dispatch(const std::string&, const std::string& action,
                                 const std::string&) {
  const bool mutation = !isReadOnlySonosAction(action);
  if (!runtimeDispatchAllowed(runtime.jobs.snapshot(), runtime.network.snapshot(),
                              runtime.health.snapshot(), jobId, discoveryId, runtime.now,
                              runtime.stopping, mutation))
    return {0, "", "runtime dispatch blocked", true};
  if (mutation) {
    runtime.check(runtime.health.usable(discoveryId), "mutation requires current usable topology");
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
    f.configure();
    f.connect();
    const auto id = f.jobs.submit(f.now, 90000);
    const auto discovery = f.health.snapshot().discoveryId;
    f.now += sonos::DiscoveryBudgetMs;
    f.check(f.jobs.active(id, f.now) && f.health.snapshot().state == SonosState::Discovering,
            "worker outlives discovery deadline before model tick arrives");
    f.check(!runtimeDispatchAllowed(f.jobs.snapshot(), f.network.snapshot(), f.health.snapshot(),
                                    id, discovery, f.now, false, false),
            "discovery deadline independently blocks reads before timer callback");
    f.check(!runtimeDispatchAllowed(f.jobs.snapshot(), f.network.snapshot(), f.health.snapshot(),
                                    id, discovery, f.now, false, true),
            "expired discovery never authorizes mutations");
    f.tick();
    f.check(f.health.snapshot().state == SonosState::Backoff,
            "delayed discovery timer schedules recovery");
    f.finish(id, false);
    f.invariants();
  }
  {
    RuntimeFixture f(1);
    f.tick(100000);
    f.check(f.connectCalls == 0 && f.discoveryRequests == 0, "unconfigured boot stays quiet");
    f.configure();
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
      f.tick(wifi::ConnectBudgetMs);
      f.check(f.network.snapshot().state == WifiState::Backoff, "connection cannot hang forever");
      f.tick(f.network.snapshot().retryAt - f.now);
    }
    f.tick(500);
    f.connect();
    f.check(f.network.snapshot().networkReady && f.discoveryRequests == 1,
            "delayed connection eventually starts discovery");
    auto id = f.beginJob();
    f.discovery(false);
    f.finish(id, false, true);
    f.check(f.health.snapshot().state == SonosState::Backoff, "empty SSDP schedules retry");
    f.tick(f.health.snapshot().retryAt - f.now);
    id = f.beginJob();
    f.discovery(true);
    f.check(f.reconcile().ok, "SSDP recovery reads authoritative state");
    f.finish(id, true);
    const auto observation = f.display.observed;
    f.drop();
    f.check(f.display.observed.title == observation.title && f.display.observed.stale &&
                f.learnedAddress.empty(),
            "outage retains visible observations and discards address authority");
    f.tick(f.network.snapshot().retryAt - f.now);
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
    id = f.beginJob();
    f.discovery(true);
    f.http.failRead = true;
    f.check(!f.reconcile().ok && f.app.state().recoveryRequired == uncertain,
            "failed reconciliation preserves uncertainty");
    f.finish(id, false, true);
    f.tick(f.health.snapshot().retryAt - f.now);
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
    f.transport.verificationDelay = 1000;
    f.check(f.command().ok && !f.jobs.snapshot().running && f.app.state().status == "succeeded",
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
    f.tick(f.health.snapshot().retryAt - f.now);
    id = f.beginJob();
    f.discovery(true);
    f.http.failRead = true;
    f.check(!f.reconcile().ok && f.display.status == "uncertain" &&
                f.display.detail == cancellationDetail && f.http.writes.size() == 1,
            "failed recovery cannot resurrect expired session success");
    f.finish(id, false, true);
    f.http.failRead = false;
    f.tick(f.health.snapshot().retryAt - f.now);
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
    f.tick(1000);
    f.check(!f.jobs.snapshot().running && f.display.observed.stale,
            "deadline releases worker and stales observation");
    f.tick(f.health.snapshot().retryAt - f.now);
    const auto b = f.beginJob();
    f.discovery(true);
    AppState late = f.app.state();
    late.observed.title = "Late A must never display";
    f.check(!f.publish(a, late), "late A publication cannot overwrite B");
    f.jobs.process(worker::Success{a});
    f.jobs.process(worker::Failure{a});
    f.check(f.jobs.snapshot().jobId == b, "late A completion cannot complete B");
    f.check(!runtimeDispatchAllowed(f.jobs.snapshot(), f.network.snapshot(), f.health.snapshot(), a,
                                    oldGeneration, f.now, false, true),
            "expired A cannot dispatch another operation");
    f.check(!runtimeDispatchAllowed(f.jobs.snapshot(), f.network.snapshot(), f.health.snapshot(), b,
                                    oldGeneration, f.now, false, true),
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
    auto request = f.events.snapshot().requestId;
    f.events.process(subscription::RequestFailed{f.now, request});
    auto id = f.beginJob();
    f.discovery(true);
    f.check(f.reconcile().ok, "polling works while subscription failed");
    f.http.failQueue = true;
    const auto observed = f.app.state().observed;
    f.check(!f.app.queue(0, 4).ok && f.app.state().observed.title == observed.title &&
                !f.app.state().observed.stale,
            "queue read failure retains authoritative basic playback");
    f.finish(id, true);
    f.tick(f.events.snapshot().retryAt - f.now);
    request = f.events.snapshot().requestId;
    f.events.process(subscription::RequestSucceeded{f.now, request, "sid-1", 1000});
    f.tick(800);
    f.check(f.events.snapshot().state == SubscriptionState::Renewing, "renewal becomes due");
    f.events.process(subscription::RequestFailed{f.now, f.events.snapshot().requestId});
    const auto notifications = f.events.snapshot().notifications;
    f.events.process(subscription::NotifyReceived{f.now, "sid-1"});
    f.check(!f.events.snapshot().sidPresent && f.events.snapshot().notifications == notifications,
            "failed renewal cannot be revived by stale SID");
    id = f.beginJob();
    f.discovery(true);
    f.check(f.reconcile().ok, "polling survives renewal failure");
    f.finish(id, true);
    f.invariants();
  }
}

void chaos(uint64_t seed, uint64_t steps) {
  RuntimeFixture f(seed);
  f.ready();
  for (uint64_t step = 0; step < steps; ++step) {
    f.tick(f.trace.random() % 250);
    const auto event = f.trace.random() % 19;
    const auto before = f.jobs.snapshot();
    const auto generation = f.health.snapshot().discoveryId;
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
      if (before.running && f.health.usable(f.http.discoveryId) &&
          !f.attempted.count(before.jobId)) {
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
      f.jobs.process(worker::Success{stale});
      f.jobs.process(worker::Failure{stale});
      f.check(f.jobs.snapshot().jobId == before.jobId, "stale completion leaves active job intact");
      AppState late = f.app.state();
      f.check(!f.publish(stale, late), "stale job cannot publish");
      break;
    }
    case 11: {
      const auto sub = f.events.snapshot();
      f.events.process(subscription::RequestSucceeded{f.now, sub.requestId, "sid-chaos", 1500});
      break;
    }
    case 12:
      f.events.process(subscription::RequestFailed{f.now, f.events.snapshot().requestId});
      break;
    case 13: {
      const auto notifications = f.events.snapshot().notifications;
      f.events.process(subscription::NotifyReceived{f.now, "sid-stale"});
      f.check(f.events.snapshot().notifications == notifications,
              "stale SID never invalidates topology");
      break;
    }
    case 14:
      f.nextAddress = f.nextAddress == "192.168.1.2" ? "192.168.1.99" : "192.168.1.2";
      break;
    case 15:
      f.stopping = true;
      f.jobs.process(worker::Shutdown{});
      f.network.process(wifi::Shutdown{f.now});
      f.health.process(sonos::Shutdown{f.now});
      f.events.process(subscription::Shutdown{f.now});
      f.drain();
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
      f.jobs.process(worker::WorkerUnavailable{});
      break;
    case 18:
      f.finish(before.jobId, false, true);
      break;
    }
    f.drain();
    const auto j = f.jobs.snapshot();
    const auto w = f.network.snapshot();
    const auto s = f.health.snapshot();
    const bool authorized = j.running && j.jobId == f.http.jobId && f.now < j.deadline &&
                            !f.stopping && w.state == WifiState::Online &&
                            s.state == SonosState::Ready && s.discoveryId == f.http.discoveryId &&
                            f.http.discoveryId != 0;
    f.check(runtimeDispatchAllowed(j, w, s, f.http.jobId, f.http.discoveryId, f.now, f.stopping,
                                   true) == authorized,
            "real mutation fence matches independent combined authority model");
    f.invariants();
  }
  f.tick(sonos::DiscoveryBudgetMs + wifi::ConnectBudgetMs);
  f.check(!f.jobs.snapshot().running, "final running work always reaches deadline");
  std::cout << "runtime faults: composed scenarios + " << steps << " events seed=" << seed
            << " mutations=" << f.http.mutations << " reads=" << f.http.reads
            << " connections=" << f.network.snapshot().connections << '\n';
}
int main(int argc, char** argv) {
  scenarios();
  chaos(argc > 1 ? std::stoull(argv[1]) : 0x72756e74696d65ULL,
        argc > 2 ? std::stoull(argv[2]) : 100000ULL);
}
