#include "SonosHealth.h"
#include "lifecycle_test_support.h"
#include <cassert>
#include <vector>
using namespace surface::device;
using namespace surface::device::sonos_health;
struct Effects final : SonosEffects {
  std::vector<uint64_t> starts;
  uint64_t invalidations = 0, discoveryInvalidations = 0, reads = 0;
  std::string cachedHost, observedRoom = "Office";
  std::vector<std::string> mutations;
  void discoveryRequested(uint64_t id) override {
    assert(starts.empty() || id > starts.back());
    starts.push_back(id);
  }
  void invalidateAuthority(bool discovery) override {
    ++invalidations;
    if (discovery) {
      ++discoveryInvalidations;
      cachedHost.clear();
    }
  }
  void reconciliationRequested() override { ++reads; }
  bool mutate(const SonosHealth& health, uint64_t id, const std::string& command) {
    if (!health.usable(id))
      return false;
    mutations.push_back(command);
    return true;
  }
};
void recoveryAndMutationAuthority() {
  Effects effects;
  SonosHealth health(effects);
  assert(sonosInvariant(health.snapshot()));
  health.tick(100000);
  health.process(RefreshRequested{100000});
  assert(effects.starts.empty() && !effects.mutate(health, 0, "play"));
  health.process(NetworkAvailable{100000});
  auto s = health.snapshot();
  assert(s.discoveryId == 1 && s.deadline == 145000);
  health.process(NetworkAvailable{100001});
  health.process(RefreshRequested{100001});
  assert(effects.starts.size() == 1);
  health.process(DiscoveryFailed{100010, 1}); // SSDP returned no speakers.
  assert(health.snapshot().retryAt == 101010 && effects.invalidations == 1);
  health.process(RefreshRequested{100011}); // Input must not bypass failure backoff.
  health.process(NetworkAvailable{100011});
  health.tick(101009);
  assert(effects.starts.size() == 1 && effects.observedRoom == "Office");
  health.tick(101010);
  assert(health.snapshot().discoveryId == 2);
  health.process(DiscoverySucceeded{101011, 1});
  assert(health.snapshot().state == SonosState::Discovering && effects.reads == 0);
  effects.cachedHost = "192.0.2.10";
  health.process(DiscoverySucceeded{101012, 2});
  assert(health.usable(2) && !health.usable(1) && effects.reads == 1);
  assert(health.snapshot().lastSuccessfulDiscovery == 101012);
  assert(effects.mutate(health, 2, "next"));
  health.process(SessionFailure{101013, 2}); // Uncertain HTTP result: do not replay next.
  assert(effects.cachedHost == "192.0.2.10" && effects.observedRoom == "Office" &&
         effects.invalidations == 2 && effects.discoveryInvalidations == 1);
  assert(!effects.mutate(health, 2, "next"));
  health.tick(health.snapshot().retryAt);
  s = health.snapshot();
  effects.cachedHost = "192.0.2.11"; // Rebooted speaker recovered at another address.
  health.process(DiscoverySucceeded{s.since + 5, s.discoveryId});
  assert(health.usable(s.discoveryId) && effects.reads == 2 && effects.mutations.size() == 1);
  const auto recoveredId = s.discoveryId;
  health.process(RefreshRequested{s.since + 6});
  s = health.snapshot();
  assert(s.discoveryId == recoveredId + 1 && !health.usable(recoveredId));
  assert(effects.cachedHost == "192.0.2.11"); // Fresh topology can probe the healthy host.
  health.process(SessionFailure{s.since + 1, recoveredId});
  assert(health.snapshot().state == SonosState::Discovering);
  health.process(DiscoverySucceeded{s.since + 2, s.discoveryId});
  assert(health.usable(s.discoveryId) && effects.reads == 2);
  health.process(SessionFailure{s.since + 3, recoveredId});
  assert(health.usable(s.discoveryId));
  health.process(NetworkUnavailable{s.since + 4});
  assert(!health.usable(s.discoveryId) && effects.observedRoom == "Office");
  assert(health.snapshot().lastSuccessfulDiscovery == s.since + 2);
  assert(sonosInvariant(health.snapshot()));
}
void deadlinesAndRetries() {
  Effects effects;
  SonosHealth health(effects);
  // Controlled uint64 clocks remain correct across the Arduino millis() boundary.
  health.process(NetworkAvailable{UINT32_MAX - 100});
  auto s = health.snapshot();
  health.tick(s.deadline - 1);
  assert(health.snapshot().state == SonosState::Discovering);
  health.process(DiscoverySucceeded{s.deadline, s.discoveryId});
  assert(health.snapshot().state == SonosState::Discovering && effects.reads == 0);
  health.tick(s.deadline);
  assert(health.snapshot().lastError == SonosError::DiscoveryTimeout);
  assert(health.snapshot().retryAt == s.deadline + 1000);
  const uint64_t delays[] = {2000, 4000, 8000, 16000, 30000, 30000, 30000};
  for (auto delay : delays) {
    health.tick(health.snapshot().retryAt);
    s = health.snapshot();
    health.process(DiscoveryFailed{s.since + 1, s.discoveryId - 1});
    health.process(DiscoverySucceeded{s.since + 1, s.discoveryId - 1});
    assert(health.snapshot().state == SonosState::Discovering);
    health.tick(s.deadline);
    assert(health.snapshot().retryAt == s.deadline + delay);
    assert(sonosInvariant(health.snapshot()));
  }
  health.tick(health.snapshot().retryAt);
  s = health.snapshot();
  health.process(DiscoverySucceeded{s.since + 1, s.discoveryId});
  health.process(SessionSucceeded{s.since + 1, s.discoveryId});
  assert(health.snapshot().attempts == 0);
  health.process(SessionFailure{s.since + 2, s.discoveryId});
  assert(health.snapshot().retryAt == s.since + 1002);
  health.tick(health.snapshot().retryAt);
  s = health.snapshot();
  health.process(SessionFailure{s.since + 1, s.discoveryId});
  assert(health.snapshot().state == SonosState::Backoff);
  assert(health.snapshot().retryAt == s.since + 2001);
}
void repeatedSessionFailure() {
  Effects effects;
  SonosHealth health(effects);
  health.process(NetworkAvailable{0});
  const uint64_t delays[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000};
  uint32_t failures = 0;
  for (const auto delay : delays) {
    auto s = health.snapshot();
    health.process(DiscoverySucceeded{s.since + 1, s.discoveryId});
    assert(health.snapshot().attempts == 0);
    assert(health.snapshot().consecutiveSessionFailures == failures);
    health.process(SessionFailure{s.since + 2, s.discoveryId});
    assert(health.snapshot().retryAt == s.since + 2 + delay);
    assert(health.snapshot().consecutiveSessionFailures == ++failures);
    health.tick(health.snapshot().retryAt);
  }
  auto s = health.snapshot();
  health.process(DiscoverySucceeded{s.since + 1, s.discoveryId});
  health.process(SessionSucceeded{s.since + 2, s.discoveryId - 1});
  assert(health.snapshot().consecutiveSessionFailures == failures);
  health.process(SessionSucceeded{s.since + 3, s.discoveryId});
  assert(health.snapshot().consecutiveSessionFailures == 0);
  health.process(SessionFailure{s.since + 4, s.discoveryId});
  assert(health.snapshot().retryAt == s.since + 1004);
}
void shutdownAndNetworkLoss() {
  for (const auto state :
       {SonosState::Offline, SonosState::Discovering, SonosState::Ready, SonosState::Backoff}) {
    for (bool shutdown : {false, true}) {
      Effects effects;
      SonosHealth health(effects);
      if (state != SonosState::Offline)
        health.process(NetworkAvailable{0});
      if (state == SonosState::Ready)
        health.process(DiscoverySucceeded{1, 1});
      if (state == SonosState::Backoff)
        health.process(DiscoveryFailed{1, 1});
      if (shutdown)
        health.process(Shutdown{5});
      else
        health.process(NetworkUnavailable{5});
      assert(health.snapshot().state == SonosState::Offline && sonosInvariant(health.snapshot()));
      const auto attempts = effects.starts.size();
      health.tick(1000000);
      health.process(DiscoverySucceeded{1000000, 1});
      health.process(DiscoveryFailed{1000000, 1});
      health.process(SessionFailure{1000000, 1});
      assert(effects.starts.size() == attempts && !health.usable(1));
      health.process(NetworkAvailable{1000001});
      auto s = health.snapshot();
      health.process(DiscoverySucceeded{1000002, s.discoveryId});
      assert(health.usable(s.discoveryId) && sonosInvariant(health.snapshot()));
    }
  }
}
std::string describe(const SonosSnapshot& s) {
  return std::string(sonosStateName(s.state)) + " since=" + std::to_string(s.since) +
         " deadline=" + std::to_string(s.deadline) + " retryAt=" + std::to_string(s.retryAt) +
         " id=" + std::to_string(s.discoveryId) + " attempts=" + std::to_string(s.attempts) +
         " sessionFailures=" + std::to_string(s.consecutiveSessionFailures) +
         " stale=" + std::to_string(s.staleResults) + " error=" + sonosErrorName(s.lastError);
}
void chaos(uint64_t seed, uint64_t steps) {
  LifecycleTrace trace(seed);
  Effects effects;
  SonosHealth health(effects);
  SonosState expected = SonosState::Offline;
  uint64_t now = 0, id = 0, total = 0, since = 0, deadline = 0, retryAt = 0;
  uint64_t successes = 0, lastSuccess = 0, reads = 0, invalidations = 0, stale = 0;
  uint64_t discoveryInvalidations = 0;
  uint32_t attempts = 0, sessionFailures = 0;
  bool recovering = false;
  for (uint64_t step = 0; step < steps; ++step) {
    now += trace.random() % 12000;
    const auto choice = trace.random() % 15;
    const auto callback = trace.random() % 2 ? id : total + 1;
    const char* names[] = {"network-up",
                           "network-down",
                           "refresh",
                           "discovered",
                           "failed",
                           "session-failed",
                           "tick",
                           "timeout",
                           "retry",
                           "shutdown",
                           "stale-success",
                           "stale-failure",
                           "stale-session-failure",
                           "session-succeeded",
                           "stale-session-succeeded"};
    trace.record(std::string(names[choice]) + " now=" + std::to_string(now) +
                 " id=" + std::to_string(callback));
    bool begin = false, loss = false;
    switch (choice) {
    case 0:
      health.process(NetworkAvailable{now});
      if (expected == SonosState::Offline) {
        begin = recovering = true;
      }
      break;
    case 1:
    case 9:
      if (choice == 1)
        health.process(NetworkUnavailable{now});
      else
        health.process(Shutdown{now});
      if (expected != SonosState::Offline) {
        ++invalidations;
        ++discoveryInvalidations;
        since = now;
      }
      expected = SonosState::Offline;
      id = deadline = retryAt = 0;
      attempts = sessionFailures = 0;
      recovering = false;
      break;
    case 2:
      health.process(RefreshRequested{now});
      if (expected == SonosState::Ready) {
        begin = true;
        recovering = false;
      }
      break;
    case 3:
      health.process(DiscoverySucceeded{now, callback});
      if (expected == SonosState::Discovering && callback == id && now < deadline) {
        expected = SonosState::Ready;
        since = lastSuccess = now;
        deadline = 0;
        attempts = 0;
        ++successes;
        if (recovering)
          ++reads;
        recovering = false;
      } else
        ++stale;
      break;
    case 4:
      health.process(DiscoveryFailed{now, callback});
      loss = expected == SonosState::Discovering && callback == id;
      if (!loss)
        ++stale;
      break;
    case 5:
      health.process(SessionFailure{now, callback});
      loss =
          (expected == SonosState::Discovering || expected == SonosState::Ready) && callback == id;
      if (!loss)
        ++stale;
      break;
    case 6:
      health.tick(now);
      loss = expected == SonosState::Discovering && now >= deadline;
      begin = expected == SonosState::Backoff && now >= retryAt;
      break;
    case 7:
      health.process(DiscoveryTimeout{now});
      loss = expected == SonosState::Discovering && now >= deadline;
      break;
    case 8:
      health.process(RetryDue{now});
      begin = expected == SonosState::Backoff && now >= retryAt;
      break;
    case 10:
      health.process(DiscoverySucceeded{now, UINT64_MAX});
      ++stale;
      break;
    case 11:
      health.process(DiscoveryFailed{now, UINT64_MAX});
      ++stale;
      break;
    case 12:
      health.process(SessionFailure{now, UINT64_MAX});
      ++stale;
      break;
    case 13:
      health.process(SessionSucceeded{now, callback});
      if (expected == SonosState::Ready && callback == id)
        sessionFailures = 0;
      else
        ++stale;
      break;
    case 14:
      health.process(SessionSucceeded{now, UINT64_MAX});
      ++stale;
      break;
    }
    if (loss) {
      if (choice == 5)
        ++sessionFailures;
      const auto failures = choice == 5 && sessionFailures > attempts ? sessionFailures : attempts;
      const uint64_t delay = failures > 5 ? 30000 : 1000ULL << (failures ? failures - 1 : 0);
      expected = SonosState::Backoff;
      since = now;
      id = deadline = 0;
      retryAt = now + delay;
      recovering = true;
      ++invalidations;
      if (choice != 5)
        ++discoveryInvalidations;
    }
    if (begin) {
      expected = SonosState::Discovering;
      since = now;
      id = ++total;
      ++attempts;
      deadline = now + 45000;
      retryAt = 0;
    }
    const auto s = health.snapshot();
    trace.check(sonosInvariant(s), "Sonos invariant", describe(s));
    trace.check(s.state == expected && s.discoveryId == id && s.totalDiscoveries == total &&
                    s.successfulDiscoveries == successes &&
                    s.lastSuccessfulDiscovery == lastSuccess && s.deadline == deadline &&
                    s.retryAt == retryAt && s.since == since && s.staleResults == stale &&
                    s.recovering == recovering && s.attempts == attempts &&
                    s.consecutiveSessionFailures == sessionFailures,
                "independent reference model", describe(s));
    trace.check(effects.starts.size() == total && effects.reads == reads &&
                    effects.invalidations == invalidations &&
                    effects.discoveryInvalidations == discoveryInvalidations &&
                    effects.mutations.empty() && effects.observedRoom == "Office",
                "recovery effects retain observations and never replay mutations", describe(s));
    trace.check(health.usable(id) == (expected == SonosState::Ready) && !health.usable(total + 1),
                "mutation authority requires current discovery and network", describe(s));
  }
  // A lost callback has a deadline and an automatic recovery path without input.
  health.process(Shutdown{now});
  health.process(NetworkAvailable{now});
  health.tick(now + DiscoveryBudgetMs);
  trace.check(health.snapshot().state == SonosState::Backoff, "discovery cannot wedge",
              describe(health.snapshot()));
  health.tick(health.snapshot().retryAt);
  const auto s = health.snapshot();
  health.process(DiscoverySucceeded{s.since + 1, s.discoveryId});
  trace.check(health.usable(s.discoveryId), "eventual recovery after faults stop",
              describe(health.snapshot()));
}
int main(int argc, char** argv) {
  const auto seed = argc > 1 ? std::stoull(argv[1]) : 0x50a05eedULL;
  const auto steps = argc > 2 ? std::stoull(argv[2]) : 100000ULL;
  recoveryAndMutationAuthority();
  deadlinesAndRetries();
  repeatedSessionFailure();
  shutdownAndNetworkLoss();
  chaos(seed, steps);
  std::cout << "Sonos lifecycle: examples + " << steps << " transitions seed=" << seed << '\n';
}
