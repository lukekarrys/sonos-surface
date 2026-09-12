#pragma once
#include "RuntimeAdmission.h"
#include "SubscriptionLifecycle.h"
#include <SurfaceCore.h>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace surface::device {
struct SubscriptionRequest {
  uint64_t id;
  bool renewal;
  std::string address, sid;
};

// Composition shared by the device adapter and host fault tests. The four
// lifecycles remain independent peers: effects record facts, then the
// coordinator delivers explicit events after each machine has finished its
// transition. Every call is serialized by the adapter's state mutex.
class RuntimeCoordinator {
public:
  static constexpr uint64_t ReadBudgetMs = 45000;
  static constexpr uint64_t MutationBudgetMs = 90000;
  static constexpr uint64_t PollIntervalMs = playbackRefreshIntervalMs;
  static constexpr uint64_t AutomaticRateLimitMs = 1000;
  struct Admission {
    bool allowed = false;
    std::string notice, reason;
    uint64_t retryAt = 0;
  };
  struct Bind {
    uint64_t discoveryId = 0;
    bool discardHost = false;
  };
  struct JobOutcomeInput {
    // targetUsable is captured before session work: a room-policy/eligibility
    // miss is not evidence that the Sonos network failed.
    bool targetUsable = false;
    bool sessionOk = false;
    bool queueFailed = false; // Optional queue data never decides job health.
    bool unavailable = false;
  };
  struct WifiPending {
    bool connect = false, disconnect = false;
  };
  struct Snapshots {
    WorkerSnapshot worker;
    WifiSnapshot wifi;
    SonosSnapshot sonos;
    SubscriptionSnapshot subscription;
    bool reconciliationPending, discoveryPending, discardHost;
    uint64_t nextAutomaticJobAt, lastAutomaticJobAt;
  };

private:
  struct WorkerFacts final : WorkerEffects {
    struct Completion {
      uint64_t id;
      JobOutcome outcome;
    };
    std::optional<Completion> completion;
    std::vector<std::string> transitions;
    void started(uint64_t id, uint64_t deadline) override {
      transitions.push_back("worker Idle -> Running id=" + std::to_string(id) +
                            " deadline=" + std::to_string(deadline));
    }
    void finished(uint64_t id, JobOutcome outcome) override {
      completion = Completion{id, outcome};
      transitions.push_back("worker Running -> Idle id=" + std::to_string(id) +
                            " outcome=" + outcomeName(outcome));
    }
    void staleResult(uint64_t id) override {
      transitions.push_back("worker stale-result id=" + std::to_string(id));
    }
  } workerFacts_;
  struct WifiFacts final : WifiEffects {
    WifiPending pending;
    std::optional<bool> availability;
    void beginConnect(uint64_t) override { pending.connect = true; }
    void disconnect() override { pending.disconnect = true; }
    void availabilityChanged(bool available) override { availability = available; }
  } wifiFacts_;
  struct SonosFacts final : SonosEffects {
    bool discoveryPending = false, discardHost = false;
    bool invalidated = false, reconcile = false;
    void discoveryRequested(uint64_t) override { discoveryPending = true; }
    void invalidateAuthority() override {
      discardHost = invalidated = true;
      discoveryPending = false;
    }
    void reconciliationRequested() override { reconcile = true; }
  } sonosFacts_;
  struct SubscriptionFacts final : SubscriptionEffects {
    std::optional<SubscriptionRequest> pending;
    bool reconcile = false;
    void request(uint64_t id, bool renewal, const std::string& address,
                 const std::string& sid) override {
      pending = SubscriptionRequest{id, renewal, address, sid};
    }
    void topologyChanged() override { reconcile = true; }
  } subscriptionFacts_;
  WorkerLifecycle worker_{workerFacts_};
  WifiLifecycle wifi_{wifiFacts_};
  SonosHealth sonos_{sonosFacts_};
  SubscriptionLifecycle subscription_{subscriptionFacts_};
  AppState& display_;
  bool reconciliationPending_ = false;
  bool jobRefresh_ = false, discoveryResultReceived_ = false;
  uint64_t workerDiscoveryId_ = 0;
  uint64_t nextAutomaticJobAt_ = 0, lastAutomaticJobAt_ = 0;
  std::vector<std::string> transitions_;
  std::optional<std::string> notice_;

  void logWifi(const WifiSnapshot& before) {
    const auto after = wifi_.snapshot();
    if (before.state != after.state)
      transitions_.push_back(std::string("wifi ") + wifiStateName(before.state) + " -> " +
                             wifiStateName(after.state) +
                             " attempt=" + std::to_string(after.totalAttempts) +
                             " reason=" + wifiErrorName(after.lastError));
  }
  void logSonos(const SonosSnapshot& before) {
    const auto after = sonos_.snapshot();
    if (before.state != after.state)
      transitions_.push_back(std::string("sonos ") + sonosStateName(before.state) + " -> " +
                             sonosStateName(after.state) +
                             " generation=" + std::to_string(after.discoveryId) +
                             " reason=" + sonosErrorName(after.lastError));
  }
  void logSubscription(const SubscriptionSnapshot& before) {
    const auto after = subscription_.snapshot();
    if (before.state != after.state || before.requestId != after.requestId)
      transitions_.push_back(std::string("subscription ") + subscriptionStateName(before.state) +
                             " -> " + subscriptionStateName(after.state) +
                             " reason=" + subscriptionErrorName(after.lastError));
  }
  template <class Event> bool wifiEvent(const Event& event) {
    const auto before = wifi_.snapshot();
    const bool accepted = wifi_.process(event);
    logWifi(before);
    return accepted;
  }
  template <class Event> bool sonosEvent(const Event& event) {
    const auto before = sonos_.snapshot();
    const bool accepted = sonos_.process(event);
    logSonos(before);
    return accepted;
  }
  template <class Event> bool subscriptionEvent(const Event& event) {
    const auto before = subscription_.snapshot();
    const bool accepted = subscription_.process(event);
    logSubscription(before);
    return accepted;
  }
  template <class Event> bool workerEvent(const Event& event) {
    const bool accepted = worker_.process(event);
    for (auto& transition : workerFacts_.transitions)
      transitions_.push_back(std::move(transition));
    workerFacts_.transitions.clear();
    return accepted;
  }
  void applySonosFacts(uint64_t now) {
    if (sonosFacts_.invalidated) {
      sonosFacts_.invalidated = false;
      display_.observed.stale = true;
      display_.queue.reset();
      display_.refreshError = "Sonos unavailable; recovering";
      subscriptionEvent(subscription_lifecycle::NetworkUnavailable{now});
      subscriptionFacts_.pending.reset();
    }
    if (sonosFacts_.reconcile || subscriptionFacts_.reconcile) {
      reconciliationPending_ = true;
      sonosFacts_.reconcile = subscriptionFacts_.reconcile = false;
    }
  }
  void applyFacts(uint64_t now, bool reconcileFailure = true) {
    applySonosFacts(now);
    if (!workerFacts_.completion)
      return;
    const auto outcome = workerFacts_.completion->outcome;
    workerFacts_.completion.reset();
    if (outcome != JobOutcome::Success && (outcome != JobOutcome::Failure || reconcileFailure))
      reconciliationPending_ = true;
    if (outcome != JobOutcome::Success && outcome != JobOutcome::Failure) {
      if (workerDiscoveryId_)
        sonosEvent(sonos_health::SessionFailure{now, workerDiscoveryId_});
      applySonosFacts(now);
      display_.observed.stale = true;
      display_.queue.reset();
      display_.refreshError = std::string("Worker ") + outcomeName(outcome);
      if (display_.status == "pending") {
        display_.status = "uncertain";
        display_.recoveryRequired = true;
        display_.detail = display_.refreshError;
      }
    }
  }
  void applyAvailability(uint64_t now) {
    if (!wifiFacts_.availability)
      return;
    const bool available = *wifiFacts_.availability;
    wifiFacts_.availability.reset();
    if (available) {
      sonosEvent(sonos_health::NetworkAvailable{now});
      reconciliationPending_ = true;
      applyFacts(now);
    } else {
      sonosEvent(sonos_health::NetworkUnavailable{now});
      workerEvent(worker_lifecycle::NetworkUnavailable{});
      applyFacts(now);
      display_.observed.stale = true;
      display_.queue.reset();
      display_.refreshError = "WiFi unavailable";
    }
  }
  void expireWorker(uint64_t now) {
    workerEvent(worker_lifecycle::DeadlineExpired{now});
    applyFacts(now);
  }
  Admission recoveryAdmission(uint64_t now) const {
    const auto wifi = wifi_.snapshot();
    const auto sonos = sonos_.snapshot();
    const auto retryAt = sonos.retryAt ? sonos.retryAt : wifi.retryAt;
    std::string notice = "Sonos unavailable; recovering";
    if (retryAt > now) {
      const auto remaining = retryAt - now;
      const auto seconds = remaining / 1000 + (remaining % 1000 != 0);
      notice = "Sonos recovering; retrying in " + std::to_string(seconds) + "s";
    } else if (wifi.state == WifiState::Unconfigured)
      notice = "WiFi not configured; Sonos unavailable";
    return {false, std::move(notice),
            std::string("Sonos authority unavailable state=") + sonosStateName(sonos.state) +
                " retryAt=" + std::to_string(retryAt),
            retryAt};
  }

public:
  explicit RuntimeCoordinator(AppState& display) : display_(display) {}
  RuntimeCoordinator(const RuntimeCoordinator&) = delete;
  RuntimeCoordinator& operator=(const RuntimeCoordinator&) = delete;
  Snapshots snapshots() const {
    return {worker_.snapshot(),       wifi_.snapshot(),       sonos_.snapshot(),
            subscription_.snapshot(), reconciliationPending_, sonosFacts_.discoveryPending,
            sonosFacts_.discardHost,  nextAutomaticJobAt_,    lastAutomaticJobAt_};
  }
  void configAvailable(uint64_t now) {
    wifiEvent(wifi_lifecycle::ConfigAvailable{now});
    applyAvailability(now);
  }
  void configRemoved(uint64_t now) {
    wifiEvent(wifi_lifecycle::ConfigRemoved{now});
    wifiFacts_.pending.connect = false;
    applyAvailability(now);
  }
  void shutdown(uint64_t now) {
    workerEvent(worker_lifecycle::Shutdown{});
    applyFacts(now);
    wifiEvent(wifi_lifecycle::Shutdown{now});
    wifiFacts_.pending.connect = false;
    sonosEvent(sonos_health::Shutdown{now});
    applyFacts(now);
    subscriptionEvent(subscription_lifecycle::Shutdown{now});
    subscriptionFacts_.pending.reset();
    wifiFacts_.availability.reset();
  }
  void serviceWifi(uint64_t now, bool observedOnline) {
    auto before = wifi_.snapshot();
    wifi_.tick(now);
    logWifi(before);
    const auto current = wifi_.snapshot();
    // A status sampled before beginConnect is consumed cannot be its result.
    if (current.state == WifiState::Connecting && observedOnline && !wifiFacts_.pending.connect)
      wifiEvent(wifi_lifecycle::Connected{now, current.attemptId});
    else if (current.state == WifiState::Online && !observedOnline)
      wifiEvent(wifi_lifecycle::Disconnected{now, current.attemptId});
    applyAvailability(now);
  }
  void service(uint64_t now) {
    expireWorker(now);
    const auto sonosBefore = sonos_.snapshot();
    sonos_.tick(now);
    logSonos(sonosBefore);
    applyFacts(now);
    const auto subscriptionBefore = subscription_.snapshot();
    subscription_.tick(now);
    logSubscription(subscriptionBefore);
    applyFacts(now);
  }
  void workerAvailable() { reconciliationPending_ = true; }
  bool sonosUsable() const {
    return wifi_.snapshot().networkReady && sonos_.usable(sonos_.snapshot().discoveryId);
  }
  Admission admission(uint64_t now, bool refresh, bool stopping = false) {
    service(now);
    if (worker_.snapshot().running)
      return {false, "Busy; input ignored", "worker busy", 0};
    if (stopping)
      return {false, "Device stopping", "device stopping", 0};
    const auto sonos = sonos_.snapshot();
    if (!wifi_.snapshot().networkReady ||
        (sonos.state != SonosState::Ready && !(refresh && sonos.state == SonosState::Discovering)))
      return recoveryAdmission(now);
    return {true, {}, {}, 0};
  }
  // Validate local input before calling. The callback performs a nonblocking
  // queue send with this predicted ID while the caller still holds its mutex;
  // the consumer must acquire that mutex before it can use the queued job.
  // A rejected queue send never enters Running or consumes an identity.
  template <class Enqueue>
  uint64_t enqueueJob(uint64_t now, bool refresh, Enqueue&& enqueue, bool stopping = false) {
    if (!admission(now, refresh, stopping).allowed)
      return 0;
    const auto budget = refresh ? ReadBudgetMs : MutationBudgetMs;
    const auto accepted = worker_.snapshot().accepted;
    if (accepted == std::numeric_limits<uint64_t>::max() ||
        now > std::numeric_limits<uint64_t>::max() - budget || !enqueue(accepted + 1))
      return 0;
    workerDiscoveryId_ = 0;
    discoveryResultReceived_ = false;
    jobRefresh_ = refresh;
    workerEvent(worker_lifecycle::Submit{now, budget});
    return worker_.snapshot().jobId;
  }
  bool automaticJobDue(uint64_t now, bool workerAvailable, bool stopping = false) {
    service(now);
    const auto sonos = sonos_.snapshot();
    const bool discover = sonos.state == SonosState::Discovering && sonosFacts_.discoveryPending;
    if (stopping || !wifi_.snapshot().networkReady || !workerAvailable ||
        worker_.snapshot().running || now < nextAutomaticJobAt_ ||
        !(discover || (sonos.state == SonosState::Ready &&
                       (reconciliationPending_ || now - lastAutomaticJobAt_ >= PollIntervalMs))))
      return false;
    nextAutomaticJobAt_ = now + AutomaticRateLimitMs;
    lastAutomaticJobAt_ = now;
    reconciliationPending_ = false;
    return true;
  }
  Bind bindDiscovery(uint64_t now, uint64_t jobId) {
    service(now);
    if (!jobActive(jobId, now, false))
      return {};
    if (!workerDiscoveryId_)
      sonosEvent(sonos_health::RefreshRequested{now});
    applyFacts(now);
    const auto sonos = sonos_.snapshot();
    if (sonos.state != SonosState::Discovering ||
        (workerDiscoveryId_ && workerDiscoveryId_ != sonos.discoveryId)) {
      const auto rejected = recoveryAdmission(now);
      notice_ = rejected.notice;
      transitions_.push_back("queued job rejected: " + rejected.reason);
      return {};
    }
    workerDiscoveryId_ = sonos.discoveryId;
    sonosFacts_.discoveryPending = false;
    const bool discardHost = std::exchange(sonosFacts_.discardHost, false);
    return {workerDiscoveryId_, discardHost};
  }
  bool discoveryResult(uint64_t now, uint64_t jobId, uint64_t discoveryId, bool ok,
                       const std::string& host) {
    expireWorker(now);
    if (!jobActive(jobId, now, false) || !discoveryId || discoveryId != workerDiscoveryId_ ||
        discoveryResultReceived_)
      return false;
    discoveryResultReceived_ = true;
    if (ok)
      sonosEvent(sonos_health::DiscoverySucceeded{now, discoveryId});
    else
      sonosEvent(sonos_health::DiscoveryFailed{now, discoveryId});
    applyFacts(now);
    if (!ok || !sonos_.usable(discoveryId))
      return false;
    if (jobRefresh_)
      reconciliationPending_ = false; // This job performs the authoritative recovery read.
    subscriptionEvent(subscription_lifecycle::NetworkAvailable{now, host});
    applyFacts(now);
    return true;
  }
  bool finishJob(uint64_t now, uint64_t jobId, JobOutcomeInput input) {
    expireWorker(now);
    const auto worker = worker_.snapshot();
    const bool accepted = worker.running && worker.jobId == jobId;
    bool reconcileFailure = true;
    if (accepted) {
      if (!input.unavailable && workerDiscoveryId_ && !discoveryResultReceived_ &&
          sonos_.snapshot().state == SonosState::Discovering) {
        sonosEvent(sonos_health::DiscoveryFailed{now, workerDiscoveryId_});
        applyFacts(now);
      } else if (!input.unavailable && input.targetUsable && !input.sessionOk) {
        sonosEvent(sonos_health::SessionFailure{now, workerDiscoveryId_});
        applyFacts(now);
      } else if (!input.unavailable && input.targetUsable && input.sessionOk) {
        sonosEvent(sonos_health::SessionSucceeded{now, workerDiscoveryId_});
      }
      // A discovered household with no eligible bound/selected target remains
      // healthy. Its warning stays visible until the next ordinary poll.
      reconcileFailure =
          input.targetUsable || !discoveryResultReceived_ || !sonos_.usable(workerDiscoveryId_);
      // Pickup can lose authority after queue admission; that local rejection
      // must not request an extra read or bypass the existing retry schedule.
      if (!workerDiscoveryId_)
        reconcileFailure = false;
    }
    if (accepted && input.unavailable)
      workerEvent(worker_lifecycle::WorkerUnavailable{});
    else if (input.targetUsable && input.sessionOk)
      workerEvent(worker_lifecycle::Success{jobId});
    else
      workerEvent(worker_lifecycle::Failure{jobId});
    applyFacts(now, reconcileFailure);
    return accepted;
  }
  bool jobActive(uint64_t id, uint64_t now, bool stopping = false) const {
    return runtimeJobActive(worker_.snapshot(), id, now, stopping);
  }
  bool dispatchAllowed(uint64_t id, uint64_t discoveryId, uint64_t now, bool stopping,
                       bool mutation) const {
    return runtimeDispatchAllowed(worker_.snapshot(), wifi_.snapshot(), sonos_.snapshot(), id,
                                  discoveryId, now, stopping, mutation);
  }
  bool publish(uint64_t now, uint64_t id, const AppState& state, const std::string& selectedId,
               const std::string& warning, bool stopping = false) {
    if (!jobActive(id, now, stopping))
      return false;
    const bool published = publishSelectedState(display_, state, selectedId);
    if (!warning.empty())
      display_.refreshError = warning;
    return published;
  }
  bool subscriptionActive(uint64_t id, uint64_t now, bool stopping = false) const {
    return !stopping && wifi_.snapshot().networkReady && subscription_.active(id, now);
  }
  bool subscriptionResult(uint64_t now, uint64_t id, bool ok, const std::string& sid,
                          uint64_t leaseMs) {
    const bool accepted =
        ok ? subscriptionEvent(subscription_lifecycle::RequestSucceeded{now, id, sid, leaseMs})
           : subscriptionEvent(subscription_lifecycle::RequestFailed{now, id});
    applyFacts(now);
    return accepted;
  }
  bool notify(uint64_t now, const std::string& sid, bool stopping = false) {
    if (stopping)
      return false;
    const bool accepted = subscriptionEvent(subscription_lifecycle::NotifyReceived{now, sid});
    applyFacts(now);
    return accepted;
  }
  // Each destination drains only its own effects. The main task must not steal
  // a subscription request before the networking worker can consume it.
  WifiPending drainWifi() { return std::exchange(wifiFacts_.pending, WifiPending{}); }
  std::optional<SubscriptionRequest> drainSubscription() {
    return std::exchange(subscriptionFacts_.pending, std::nullopt);
  }
  std::vector<std::string> drainTransitions() {
    return std::exchange(transitions_, std::vector<std::string>{});
  }
  std::optional<std::string> drainNotice() { return std::exchange(notice_, std::nullopt); }
};
} // namespace surface::device
