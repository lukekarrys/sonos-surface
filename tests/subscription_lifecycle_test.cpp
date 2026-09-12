#include "SubscriptionLifecycle.h"
#include "lifecycle_test_support.h"
#include <cassert>
#include <vector>
using namespace surface::device;
using namespace surface::device::subscription_lifecycle;
struct Request {
  uint64_t id;
  bool renewal;
  std::string address, sid;
};
struct Effects final : SubscriptionEffects {
  std::vector<Request> requests;
  uint64_t invalidations = 0;
  bool valid = true;
  void request(uint64_t id, bool renewal, const std::string& address,
               const std::string& sid) override {
    valid = valid && !address.empty() && renewal == !sid.empty() &&
            (requests.empty() || id > requests.back().id);
    requests.push_back({id, renewal, address, sid});
  }
  void topologyChanged() override { ++invalidations; }
};
void leaseParsing() {
  assert(parseSubscriptionLeaseMs("Second-1") == 1000);
  assert(parseSubscriptionLeaseMs("Second-030") == 30000);
  assert(parseSubscriptionLeaseMs("Second-300") == 300000);
  assert(parseSubscriptionLeaseMs("Second-301") == 300000);
  assert(parseSubscriptionLeaseMs("Second-18446744073709551616") == 300000);
  assert(parseSubscriptionLeaseMs("Second-" + std::string(1000, '9')) == 300000);
  for (const auto& value : {"", "Second-", "Second-0", "Second-000", "Second--1", "Second-+1",
                            "Second-infinite", "Second-Infinite", "second-30", "Second-30 ",
                            " Second-30", "Second-1.5", "Second-30\n", "Second-999999999999999x"})
    assert(parseSubscriptionLeaseMs(value) == 0);
}
void examples() {
  Effects effects;
  SubscriptionLifecycle subscription(effects);
  assert(subscriptionInvariant(subscription.snapshot()));
  subscription.tick(1000);
  assert(!subscription.process(NotifyReceived{1000, "sid-a"}));
  subscription.process(NetworkAvailable{1000, ""});
  assert(effects.requests.empty());
  subscription.process(NetworkAvailable{1000, "speaker-a"});
  auto a = subscription.snapshot().requestId;
  assert(a == 1 && subscription.active(a, 5999) && !subscription.active(a, 6000));
  subscription.process(NetworkAvailable{1001, "speaker-a"});
  assert(effects.requests.size() == 1);
  subscription.process(RequestSucceeded{1002, a + 1, "sid-a", 10000});
  subscription.process(RequestFailed{1002, a + 1});
  assert(subscription.snapshot().state == SubscriptionState::Subscribing);
  subscription.tick(5999);
  assert(subscription.snapshot().state == SubscriptionState::Subscribing);
  subscription.tick(6000);
  assert(subscription.snapshot().state == SubscriptionState::Backoff);
  assert(subscription.snapshot().lastError == SubscriptionError::Timeout);
  subscription.tick(6999);
  assert(effects.requests.size() == 1);
  subscription.tick(7000);
  auto b = subscription.snapshot().requestId;
  assert(b == 2);
  subscription.process(RequestSucceeded{7001, a, "sid-a", 10000});
  subscription.process(RequestFailed{7001, a});
  assert(subscription.snapshot().requestId == b);
  subscription.process(RequestSucceeded{7002, b, "sid-b", 10000});
  auto s = subscription.snapshot();
  assert(s.state == SubscriptionState::Healthy && s.sidPresent);
  assert(s.renewAt == 15002 && s.leaseUntil == 17002 && s.attempts == 0);
  assert(!subscription.process(NotifyReceived{7003, "sid-a"}));
  assert(subscription.process(NotifyReceived{7003, "sid-b"}));
  assert(subscription.snapshot().lastNotifyAt == 7003 && effects.invalidations == 1);
  subscription.tick(15001);
  assert(subscription.snapshot().state == SubscriptionState::Healthy);
  subscription.tick(15002);
  assert(subscription.snapshot().state == SubscriptionState::Renewing);
  assert(effects.requests.back().renewal && effects.requests.back().sid == "sid-b");
  assert(subscription.process(NotifyReceived{15003, "sid-b"}));
  subscription.process(RequestSucceeded{15004, subscription.snapshot().requestId, "sid-b", 10000});
  assert(subscription.snapshot().state == SubscriptionState::Healthy);
  assert(subscription.snapshot().leaseUntil == 25004);
  subscription.tick(23004);
  subscription.process(RequestFailed{23005, subscription.snapshot().requestId});
  assert(subscription.snapshot().state == SubscriptionState::Backoff &&
         !subscription.snapshot().sidPresent);
  assert(!subscription.process(NotifyReceived{23006, "sid-b"}));
  subscription.tick(24005);
  assert(!effects.requests.back().renewal && effects.requests.back().sid.empty());
  subscription.process(
      RequestSucceeded{24006, subscription.snapshot().requestId, "sid-c", UINT64_MAX});
  s = subscription.snapshot();
  assert(s.leaseUntil == 324006 && s.renewAt == 264006);
  subscription.tick(s.renewAt);
  subscription.process(
      RequestSucceeded{s.renewAt + 1, subscription.snapshot().requestId, "different-sid", 10000});
  assert(subscription.snapshot().lastError == SubscriptionError::InvalidResponse &&
         !subscription.snapshot().sidPresent);
  assert(effects.valid && subscriptionInvariant(subscription.snapshot()));
}
void failures() {
  Effects effects;
  SubscriptionLifecycle subscription(effects);
  subscription.process(NetworkAvailable{0, "speaker"});
  uint64_t now = 1;
  const uint64_t delays[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000};
  for (const auto delay : delays) {
    subscription.process(RequestFailed{now, subscription.snapshot().requestId});
    assert(subscription.snapshot().retryAt == now + delay);
    subscription.tick(now + delay - 1);
    assert(subscription.snapshot().state == SubscriptionState::Backoff);
    subscription.tick(now + delay);
    assert(subscription.snapshot().state == SubscriptionState::Subscribing);
    now += delay + 1;
  }
  for (const auto& sid : {std::string{}, std::string("bad\r\nSID"), std::string(257, 'x')}) {
    subscription.process(RequestSucceeded{now, subscription.snapshot().requestId, sid, 10000});
    assert(subscription.snapshot().lastError == SubscriptionError::InvalidResponse);
    now = subscription.snapshot().retryAt;
    subscription.tick(now++);
  }
  subscription.process(RequestSucceeded{now, subscription.snapshot().requestId, "sid", 0});
  assert(subscription.snapshot().lastError == SubscriptionError::InvalidResponse);
  now = subscription.snapshot().retryAt;
  subscription.tick(now);
  subscription.process(RequestSucceeded{now + 1, subscription.snapshot().requestId, "sid", 1000});
  assert(subscription.snapshot().state == SubscriptionState::Healthy);
  const auto expires = subscription.snapshot().leaseUntil;
  subscription.tick(expires);
  assert(subscription.snapshot().lastError == SubscriptionError::LeaseExpired);
  assert(!subscription.process(NotifyReceived{expires, "sid"}));
  subscription.tick(expires + 1000);
  subscription.process(RequestFailed{expires + 1001, subscription.snapshot().requestId});
  assert(subscription.snapshot().retryAt == expires + 2001);
  assert(effects.valid && subscriptionInvariant(subscription.snapshot()));
}
void networkAndAddress() {
  for (const auto state :
       {SubscriptionState::Inactive, SubscriptionState::Subscribing, SubscriptionState::Healthy,
        SubscriptionState::Renewing, SubscriptionState::Backoff}) {
    for (const auto action : {0, 1, 2, 3}) {
      Effects effects;
      SubscriptionLifecycle subscription(effects);
      if (state != SubscriptionState::Inactive)
        subscription.process(NetworkAvailable{0, "speaker-a"});
      if (state == SubscriptionState::Healthy || state == SubscriptionState::Renewing)
        subscription.process(RequestSucceeded{1, 1, "sid-a", 10000});
      if (state == SubscriptionState::Renewing)
        subscription.tick(8001);
      if (state == SubscriptionState::Backoff)
        subscription.process(RequestFailed{1, 1});
      const auto oldId = subscription.snapshot().requestId;
      if (action == 0)
        subscription.process(NetworkUnavailable{9000});
      else if (action == 1)
        subscription.process(Shutdown{9000});
      else if (action == 2)
        subscription.process(NetworkAvailable{9000, "speaker-b"});
      else
        subscription.process(NetworkAvailable{9000, ""});
      const auto newId = subscription.snapshot().requestId;
      subscription.process(RequestSucceeded{9001, oldId, "sid-a", 10000});
      subscription.process(RequestFailed{9001, oldId});
      assert(!subscription.process(NotifyReceived{9001, "sid-a"}));
      assert(!subscription.snapshot().sidPresent && subscription.snapshot().requestId == newId);
      assert(subscriptionInvariant(subscription.snapshot()));
      if (action == 2) {
        assert(subscription.snapshot().state == SubscriptionState::Subscribing);
        assert(effects.requests.back().address == "speaker-b" && !effects.requests.back().renewal);
      } else {
        assert(subscription.snapshot().state == SubscriptionState::Inactive);
        subscription.tick(100000);
        subscription.process(NetworkAvailable{100001, "speaker-b"});
      }
      auto s = subscription.snapshot();
      subscription.process(RequestSucceeded{s.since + 1, s.requestId, "sid-b", 10000});
      assert(subscription.snapshot().state == SubscriptionState::Healthy && effects.valid);
    }
  }
}
void delayedRenewal() {
  Effects effects;
  SubscriptionLifecycle subscription(effects);
  subscription.process(NetworkAvailable{0, "speaker"});
  subscription.process(RequestSucceeded{1, 1, "sid", 10000});
  subscription.tick(8001);
  const auto renewal = subscription.snapshot().requestId;
  assert(subscription.active(renewal, 10000));
  assert(!subscription.active(renewal, 10001));
  assert(!subscription.process(RequestSucceeded{10001, renewal, "sid", 10000}));
  assert(!subscription.process(NotifyReceived{10001, "sid"}));
  subscription.tick(10001);
  assert(subscription.snapshot().lastError == SubscriptionError::LeaseExpired);
  subscription.tick(11001);
  const auto fresh = subscription.snapshot().requestId;
  subscription.process(RequestSucceeded{11002, renewal, "sid", 10000});
  assert(subscription.snapshot().requestId == fresh);
  subscription.process(RequestSucceeded{11002, fresh, "new-sid", 300000});
  subscription.tick(251002);
  const auto deadline = subscription.snapshot().deadline;
  subscription.tick(deadline);
  assert(subscription.snapshot().lastError == SubscriptionError::Timeout);
  assert(!subscription.snapshot().sidPresent);
  assert(effects.valid && subscriptionInvariant(subscription.snapshot()));
}
struct Model {
  SubscriptionState state = SubscriptionState::Inactive;
  std::string address, sid;
  bool network = false;
  uint64_t id = 0, total = 0, since = 0, deadline = 0, retryAt = 0, leaseUntil = 0, renewAt = 0;
  uint64_t delay = 1000, notifications = 0;
  void start(uint64_t now, bool renewing) {
    state = renewing ? SubscriptionState::Renewing : SubscriptionState::Subscribing;
    since = now;
    id = ++total;
    deadline = now + 5000;
    retryAt = 0;
  }
  void clearLease() {
    sid.clear();
    leaseUntil = renewAt = 0;
  }
  void available(uint64_t now, const std::string& host) {
    if (state != SubscriptionState::Inactive && address == host)
      return;
    clearLease();
    address = host;
    network = true;
    delay = 1000;
    id = deadline = retryAt = 0;
    since = now;
    if (host.empty())
      state = SubscriptionState::Inactive;
    else
      start(now, false);
  }
  bool active(uint64_t callback, uint64_t now) const {
    return id && id == callback && now < deadline && (sid.empty() || now < leaseUntil);
  }
  void failure(uint64_t now) {
    const bool hadRequest = id != 0;
    state = SubscriptionState::Backoff;
    since = now;
    id = deadline = 0;
    clearLease();
    retryAt = now + delay;
    if (hadRequest)
      delay = delay * 2 < 30000 ? delay * 2 : 30000;
  }
  void success(uint64_t now, const std::string& value, uint64_t lease) {
    state = SubscriptionState::Healthy;
    since = now;
    sid = value;
    id = deadline = 0;
    leaseUntil = now + lease;
    renewAt = now + lease * 4 / 5;
    delay = 1000;
  }
  void stop(uint64_t now) {
    state = SubscriptionState::Inactive;
    network = false;
    address.clear();
    clearLease();
    id = deadline = retryAt = 0;
    since = now;
    delay = 1000;
  }
  void tick(uint64_t now) {
    if (!sid.empty() && now >= leaseUntil)
      failure(now);
    else if (id && now >= deadline)
      failure(now);
    else if (state == SubscriptionState::Healthy && now >= renewAt)
      start(now, true);
    else if (state == SubscriptionState::Backoff && now >= retryAt)
      start(now, false);
  }
};
std::string describe(const SubscriptionSnapshot& s) {
  return std::string(subscriptionStateName(s.state)) + " id=" + std::to_string(s.requestId) +
         " deadline=" + std::to_string(s.deadline) + " leaseUntil=" + std::to_string(s.leaseUntil) +
         " retryAt=" + std::to_string(s.retryAt) + " SID=" + std::to_string(s.sidPresent) +
         " error=" + subscriptionErrorName(s.lastError);
}
void chaos(uint64_t seed, uint64_t steps) {
  LifecycleTrace trace(seed);
  Effects effects;
  SubscriptionLifecycle subscription(effects);
  Model model;
  uint64_t now = 0;
  for (uint64_t step = 0; step < steps; ++step) {
    now += trace.random() % 2000;
    const auto choice = trace.random() % 14;
    const auto callback = trace.random() % 2 ? model.id : model.total + 1;
    const auto host = trace.random() % 3 ? "speaker-a" : "speaker-b";
    const auto sid = trace.random() % 3 ? "sid-a" : "sid-b";
    const uint64_t lease = trace.random() % 3 ? 10000 : 0;
    const char* names[] = {"available",  "available", "succeeded",     "succeeded",   "failed",
                           "tick",       "tick",      "notify",        "unavailable", "shutdown",
                           "no-address", "deadline",  "stale-success", "stale-notify"};
    trace.record(std::string(names[choice]) + " now=" + std::to_string(now) +
                 " id=" + std::to_string(callback) + " address=" + host + " sid=" + sid +
                 " lease=" + std::to_string(lease));
    switch (choice) {
    case 0:
    case 1:
      subscription.process(NetworkAvailable{now, host});
      model.available(now, host);
      break;
    case 2:
    case 3:
      subscription.process(RequestSucceeded{now, callback, sid, lease});
      if (model.active(callback, now)) {
        if (lease && (model.sid.empty() || model.sid == sid))
          model.success(now, sid, lease);
        else
          model.failure(now);
      }
      break;
    case 4:
      subscription.process(RequestFailed{now, callback});
      if (model.active(callback, now))
        model.failure(now);
      break;
    case 5:
    case 6:
      subscription.tick(now);
      model.tick(now);
      break;
    case 7:
      subscription.process(NotifyReceived{now, sid});
      if (!model.sid.empty() && model.sid == sid && now < model.leaseUntil)
        ++model.notifications;
      break;
    case 8:
      subscription.process(NetworkUnavailable{now});
      model.stop(now);
      break;
    case 9:
      subscription.process(Shutdown{now});
      model.stop(now);
      break;
    case 10:
      subscription.process(NetworkAvailable{now, ""});
      model.available(now, "");
      break;
    case 11:
      subscription.process(DeadlineExpired{now});
      if (model.id && now >= model.deadline)
        model.failure(now);
      break;
    case 12:
      subscription.process(RequestSucceeded{now, UINT64_MAX, "stale", 10000});
      break;
    case 13:
      subscription.process(NotifyReceived{now, "stale"});
      break;
    }
    const auto s = subscription.snapshot();
    trace.check(subscriptionInvariant(s), "subscription invariant", describe(s));
    trace.check(s.state == model.state && s.requestId == model.id &&
                    s.totalRequests == model.total && s.deadline == model.deadline &&
                    s.retryAt == model.retryAt && s.leaseUntil == model.leaseUntil &&
                    s.renewAt == model.renewAt && s.networkAvailable == model.network &&
                    s.sidPresent == !model.sid.empty(),
                "independent reference model", describe(s));
    trace.check(effects.valid && effects.requests.size() == model.total &&
                    effects.invalidations == model.notifications &&
                    s.notifications == model.notifications,
                "effect identities and valid notifications", describe(s));
  }
  subscription.process(Shutdown{now});
  subscription.process(NetworkAvailable{now, "speaker"});
  subscription.tick(now + RequestBudgetMs);
  trace.check(subscription.snapshot().state == SubscriptionState::Backoff,
              "queued request has bounded deadline", describe(subscription.snapshot()));
}
int main(int argc, char** argv) {
  const auto seed = argc > 1 ? std::stoull(argv[1]) : 0x5eed1234ULL;
  const auto steps = argc > 2 ? std::stoull(argv[2]) : 100000ULL;
  leaseParsing();
  examples();
  failures();
  networkAndAddress();
  delayedRenewal();
  chaos(seed, steps);
  std::cout << "subscription lifecycle: examples + " << steps << " transitions seed=" << seed
            << '\n';
}
