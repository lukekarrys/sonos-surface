#include "WifiLifecycle.h"
#include "lifecycle_test_support.h"
#include <cassert>
#include <vector>
using namespace surface::device;
using namespace surface::device::wifi_lifecycle;
struct Effects final : WifiEffects {
  std::vector<uint64_t> starts;
  uint64_t disconnects = 0, online = 0, offline = 0;
  bool available = false, valid = true;
  void beginConnect(uint64_t id) override {
    valid = valid && !available && id != 0 && (starts.empty() || id > starts.back());
    starts.push_back(id);
  }
  void disconnect() override { ++disconnects; }
  void availabilityChanged(bool ready) override {
    valid = valid && ready != available;
    available = ready;
    if (ready)
      ++online;
    else
      ++offline;
  }
};
void examples() {
  Effects effects;
  WifiLifecycle wifi(effects);
  assert(wifiInvariant(wifi.snapshot()));
  wifi.tick(100000);
  wifi.process(Connected{100000, 1});
  wifi.process(Disconnected{100000, 1});
  assert(effects.starts.empty());
  wifi.process(ConfigAvailable{100000});
  const auto first = wifi.snapshot().attemptId;
  assert(first == 1 && wifi.snapshot().deadline == 115000);
  wifi.process(ConfigAvailable{100001});
  assert(effects.starts.size() == 1);
  wifi.process(Connected{100000, first});
  assert(wifi.snapshot().state == WifiState::Online && effects.available);
  assert(wifi.snapshot().attempts == 0);
  wifi.process(Connected{100001, first});
  assert(effects.online == 1);
  wifi.process(Disconnected{100001, first + 1});
  assert(effects.available);
  wifi.process(Disconnected{100002, first});
  assert(wifi.snapshot().state == WifiState::Backoff && wifi.snapshot().retryAt == 101002);
  assert(effects.offline == 1 && effects.disconnects == 1);
  wifi.process(Connected{100003, first});
  wifi.process(Disconnected{100003, first});
  assert(wifi.snapshot().retryAt == 101002);
  wifi.tick(101001);
  assert(wifi.snapshot().state == WifiState::Backoff);
  wifi.tick(101002);
  const auto second = wifi.snapshot().attemptId;
  assert(second == 2);
  wifi.process(Connected{101003, first});
  wifi.process(Disconnected{101003, first});
  assert(wifi.snapshot().state == WifiState::Connecting);
  wifi.tick(116001);
  assert(wifi.snapshot().state == WifiState::Connecting);
  wifi.process(Connected{116002, second});
  assert(wifi.snapshot().state == WifiState::Connecting);
  wifi.tick(116002);
  assert(wifi.snapshot().state == WifiState::Backoff);
  assert(wifi.snapshot().lastError == WifiError::ConnectTimeout);
  assert(wifi.snapshot().retryAt == 117002);
  const uint64_t delays[] = {2000, 4000, 8000, 16000, 30000, 30000, 30000};
  for (const auto delay : delays) {
    wifi.tick(wifi.snapshot().retryAt);
    const auto deadline = wifi.snapshot().deadline;
    wifi.tick(deadline);
    assert(wifi.snapshot().state == WifiState::Backoff);
    assert(wifi.snapshot().retryAt == deadline + delay);
    assert(wifiInvariant(wifi.snapshot()));
  }
  wifi.tick(wifi.snapshot().retryAt);
  auto s = wifi.snapshot();
  wifi.process(Connected{s.since + 2000, s.attemptId});
  assert(wifi.snapshot().state == WifiState::Online && wifi.snapshot().attempts == 0);
  wifi.process(Disconnected{s.since + 2001, s.attemptId});
  assert(wifi.snapshot().retryAt == s.since + 3001);
  wifi.tick(wifi.snapshot().retryAt);
  s = wifi.snapshot();
  wifi.process(Disconnected{s.since + 1, s.attemptId});
  assert(wifi.snapshot().lastError == WifiError::Disconnected);
  assert(wifi.snapshot().retryAt == s.since + 1001);
  wifi.process(ConfigRemoved{s.since + 2});
  assert(wifi.snapshot().state == WifiState::Unconfigured);
  const auto attempts = effects.starts.size();
  wifi.tick(s.since + 1000000);
  assert(effects.valid && effects.starts.size() == attempts && wifiInvariant(wifi.snapshot()));
}
void removalAndShutdown() {
  for (auto state :
       {WifiState::Unconfigured, WifiState::Connecting, WifiState::Online, WifiState::Backoff}) {
    for (bool shutdown : {false, true}) {
      Effects effects;
      WifiLifecycle wifi(effects);
      if (state != WifiState::Unconfigured)
        wifi.process(ConfigAvailable{0});
      if (state == WifiState::Online)
        wifi.process(Connected{10, 1});
      if (state == WifiState::Backoff)
        wifi.tick(ConnectBudgetMs);
      if (shutdown)
        wifi.process(Shutdown{20000});
      else
        wifi.process(ConfigRemoved{20000});
      assert(wifi.snapshot().state == WifiState::Unconfigured && wifiInvariant(wifi.snapshot()));
      assert(!effects.available);
      const auto calls = effects.starts.size();
      wifi.tick(1000000);
      wifi.process(Connected{1000000, 1});
      wifi.process(Disconnected{1000000, 1});
      assert(effects.starts.size() == calls);
      wifi.process(ConfigAvailable{1000001});
      const auto id = wifi.snapshot().attemptId;
      wifi.process(Connected{1000002, id});
      assert(effects.valid && wifi.snapshot().state == WifiState::Online &&
             wifiInvariant(wifi.snapshot()));
    }
  }
}
std::string describe(const WifiSnapshot& s) {
  return std::string(wifiStateName(s.state)) + " since=" + std::to_string(s.since) +
         " deadline=" + std::to_string(s.deadline) + " retryAt=" + std::to_string(s.retryAt) +
         " id=" + std::to_string(s.attemptId) + " attempts=" + std::to_string(s.attempts) +
         " error=" + wifiErrorName(s.lastError);
}
void chaos(uint64_t seed, uint64_t steps) {
  LifecycleTrace trace(seed);
  Effects effects;
  WifiLifecycle wifi(effects);
  WifiState expected = WifiState::Unconfigured;
  uint64_t now = 0, id = 0, total = 0, since = 0, deadline = 0, retryAt = 0, online = 0;
  uint64_t backoff = 1000;
  for (uint64_t step = 0; step < steps; ++step) {
    now += trace.random() % 4000;
    const auto choice = trace.random() % 10;
    const auto callback = trace.random() % 2 ? id : total + 1;
    const char* names[] = {
        "config",   "connected",       "disconnected",      "tick", "timeout", "retry", "removed",
        "shutdown", "stale-connected", "stale-disconnected"};
    trace.record(std::string(names[choice]) + " now=" + std::to_string(now) +
                 " id=" + std::to_string(callback));
    bool begin = false, loss = false;
    switch (choice) {
    case 0:
      wifi.process(ConfigAvailable{now});
      if (expected == WifiState::Unconfigured) {
        begin = true;
        backoff = 1000;
      }
      break;
    case 1:
      wifi.process(Connected{now, callback});
      if (expected == WifiState::Connecting && callback == id && now < deadline) {
        expected = WifiState::Online;
        since = now;
        deadline = 0;
        backoff = 1000;
        ++online;
      }
      break;
    case 2:
      wifi.process(Disconnected{now, callback});
      loss = (expected == WifiState::Connecting || expected == WifiState::Online) && callback == id;
      break;
    case 3:
      wifi.tick(now);
      loss = expected == WifiState::Connecting && now >= deadline;
      begin = expected == WifiState::Backoff && now >= retryAt;
      break;
    case 4:
      wifi.process(ConnectTimeout{now});
      loss = expected == WifiState::Connecting && now >= deadline;
      break;
    case 5:
      wifi.process(RetryDue{now});
      begin = expected == WifiState::Backoff && now >= retryAt;
      break;
    case 6:
    case 7:
      if (choice == 6)
        wifi.process(ConfigRemoved{now});
      else
        wifi.process(Shutdown{now});
      expected = WifiState::Unconfigured;
      id = deadline = retryAt = 0;
      backoff = 1000;
      break;
    case 8:
      wifi.process(Connected{now, UINT64_MAX});
      break;
    case 9:
      wifi.process(Disconnected{now, UINT64_MAX});
      break;
    }
    if (loss) {
      const bool wasConnecting = expected == WifiState::Connecting;
      expected = WifiState::Backoff;
      since = now;
      id = deadline = 0;
      retryAt = now + backoff;
      if (wasConnecting)
        backoff = backoff * 2 < 30000 ? backoff * 2 : 30000;
    }
    if (begin) {
      expected = WifiState::Connecting;
      since = now;
      id = ++total;
      deadline = now + 15000;
      retryAt = 0;
    }
    const auto s = wifi.snapshot();
    trace.check(effects.valid, "monotonic attempts and distinct availability effects", describe(s));
    trace.check(wifiInvariant(s), "Wi-Fi invariant", describe(s));
    trace.check(s.state == expected && s.attemptId == id && s.totalAttempts == total &&
                    s.connections == online && s.deadline == deadline && s.retryAt == retryAt,
                "independent reference model", describe(s));
    trace.check(effects.available == (expected == WifiState::Online) &&
                    effects.starts.size() == total && effects.online == online,
                "effects agree with network readiness", describe(s));
    if (expected != WifiState::Unconfigured)
      trace.check(s.since == since, "state entry time", describe(s));
  }
  // A connection that never completes must always leave Connecting when driven.
  trace.record("final shutdown/config/tick now=" + std::to_string(now));
  wifi.process(Shutdown{now});
  wifi.process(ConfigAvailable{now});
  wifi.tick(now + ConnectBudgetMs);
  trace.check(effects.valid && wifiInvariant(wifi.snapshot()) &&
                  wifi.snapshot().state == WifiState::Backoff,
              "connection is bounded", describe(wifi.snapshot()));
}
int main(int argc, char** argv) {
  const auto seed = argc > 1 ? std::stoull(argv[1]) : 0x5eed1234ULL;
  const auto steps = argc > 2 ? std::stoull(argv[2]) : 100000ULL;
  examples();
  removalAndShutdown();
  chaos(seed, steps);
  std::cout << "Wi-Fi lifecycle: examples + " << steps << " transitions seed=" << seed << '\n';
}
