#pragma once
#include <surface_sml.hpp>
#include <cstdint>
#include <limits>
#include <string>

namespace surface::device {
enum class SubscriptionState { Inactive, Subscribing, Healthy, Renewing, Backoff };
enum class SubscriptionError {
  None,
  RequestFailed,
  InvalidResponse,
  Timeout,
  LeaseExpired,
  NetworkLost,
  Shutdown
};
inline const char* subscriptionStateName(SubscriptionState state) {
  switch (state) {
  case SubscriptionState::Inactive:
    return "Inactive";
  case SubscriptionState::Subscribing:
    return "Subscribing";
  case SubscriptionState::Healthy:
    return "Healthy";
  case SubscriptionState::Renewing:
    return "Renewing";
  case SubscriptionState::Backoff:
    return "Backoff";
  }
  return "unknown";
}
inline const char* subscriptionErrorName(SubscriptionError error) {
  switch (error) {
  case SubscriptionError::None:
    return "none";
  case SubscriptionError::RequestFailed:
    return "request-failed";
  case SubscriptionError::InvalidResponse:
    return "invalid-response";
  case SubscriptionError::Timeout:
    return "timeout";
  case SubscriptionError::LeaseExpired:
    return "lease-expired";
  case SubscriptionError::NetworkLost:
    return "network-lost";
  case SubscriptionError::Shutdown:
    return "shutdown";
  }
  return "unknown";
}
struct SubscriptionSnapshot {
  SubscriptionState state = SubscriptionState::Inactive;
  uint64_t since = 0, requestId = 0, deadline = 0, retryAt = 0;
  uint64_t renewAt = 0, leaseUntil = 0, lastNotifyAt = 0;
  uint64_t totalRequests = 0, notifications = 0;
  uint32_t attempts = 0;
  bool networkAvailable = false, sidPresent = false;
  SubscriptionError lastError = SubscriptionError::None;
};
struct SubscriptionEffects {
  virtual ~SubscriptionEffects() = default;
  virtual void request(uint64_t id, bool renewal, const std::string& address,
                       const std::string& sid) = 0;
  virtual void topologyChanged() = 0;
};
namespace subscription_lifecycle {
inline constexpr uint64_t RequestBudgetMs = 5000;
inline constexpr uint64_t MaximumLeaseMs = 300000;
inline constexpr uint64_t InitialBackoffMs = 1000;
inline constexpr uint64_t MaximumBackoffMs = 30000;
struct Inactive {};
struct Subscribing {};
struct Healthy {};
struct Renewing {};
struct Backoff {};
struct NetworkAvailable {
  uint64_t now;
  std::string address;
};
struct NetworkUnavailable {
  uint64_t now;
};
struct RequestSucceeded {
  uint64_t now, id;
  std::string sid;
  uint64_t leaseMs;
};
struct RequestFailed {
  uint64_t now, id;
};
struct DeadlineExpired {
  uint64_t now;
};
struct RenewalDue {
  uint64_t now;
};
struct RetryDue {
  uint64_t now;
};
struct NotifyReceived {
  uint64_t now;
  std::string sid;
};
struct LeaseExpired {
  uint64_t now;
};
struct Shutdown {
  uint64_t now;
};
struct Context {
  SubscriptionSnapshot data;
  SubscriptionEffects& effects;
  std::string address, sid;
  void clearLease() {
    sid.clear();
    data.renewAt = data.leaseUntil = 0;
  }
  void start(uint64_t now, bool renewal) {
    data.since = now;
    data.requestId = ++data.totalRequests;
    data.deadline = now + RequestBudgetMs;
    data.retryAt = 0;
    if (data.attempts != std::numeric_limits<uint32_t>::max())
      ++data.attempts;
    effects.request(data.requestId, renewal, address, sid);
  }
  void replace(const NetworkAvailable& event) {
    clearLease();
    data.networkAvailable = true;
    data.requestId = data.deadline = data.retryAt = 0;
    data.attempts = 0;
    data.lastError = SubscriptionError::None;
    address = event.address;
    data.since = event.now;
    if (!address.empty())
      start(event.now, false);
  }
  bool active(uint64_t id, uint64_t now) const {
    return id != 0 && id == data.requestId && now >= data.since && now < data.deadline &&
           (sid.empty() || now < data.leaseUntil);
  }
  bool valid(const RequestSucceeded& event) const {
    return !event.sid.empty() && event.sid.size() <= 256 &&
           event.sid.find_first_of("\r\n") == std::string::npos &&
           (sid.empty() || event.sid == sid) && event.leaseMs != 0;
  }
  void success(const RequestSucceeded& event) {
    const auto lease = event.leaseMs < MaximumLeaseMs ? event.leaseMs : MaximumLeaseMs;
    auto renewAfter = lease * 4 / 5;
    if (!renewAfter)
      renewAfter = 1;
    sid = event.sid;
    data.since = event.now;
    data.requestId = data.deadline = 0;
    data.leaseUntil = event.now + lease;
    data.renewAt = event.now + renewAfter;
    data.attempts = 0;
    data.lastError = SubscriptionError::None;
  }
  void backoff(uint64_t now, SubscriptionError error) {
    uint64_t delay = InitialBackoffMs;
    for (uint32_t attempt = 1; attempt < data.attempts && delay < MaximumBackoffMs; ++attempt)
      delay = delay * 2 < MaximumBackoffMs ? delay * 2 : MaximumBackoffMs;
    clearLease();
    data.since = now;
    data.requestId = data.deadline = 0;
    data.retryAt = now + delay;
    data.lastError = error;
  }
  void stop(uint64_t now, SubscriptionError error) {
    clearLease();
    address.clear();
    data.networkAvailable = false;
    data.since = now;
    data.requestId = data.deadline = data.retryAt = 0;
    data.attempts = 0;
    data.lastError = error;
  }
};
struct Table {
  auto operator()() const {
    using namespace boost::sml;
    const auto hasAddress = [](const NetworkAvailable& e) { return !e.address.empty(); };
    const auto changedAddress = [](const NetworkAvailable& e, const Context& c) {
      return !e.address.empty() && e.address != c.address;
    };
    const auto emptyAddress = [](const NetworkAvailable& e) { return e.address.empty(); };
    const auto replace = [](const NetworkAvailable& e, Context& c) { c.replace(e); };
    const auto succeeded = [](const RequestSucceeded& e, const Context& c) {
      return c.active(e.id, e.now) && c.valid(e);
    };
    const auto invalid = [](const RequestSucceeded& e, const Context& c) {
      return c.active(e.id, e.now) && !c.valid(e);
    };
    const auto failed = [](const RequestFailed& e, const Context& c) {
      return c.active(e.id, e.now);
    };
    const auto success = [](const RequestSucceeded& e, Context& c) { c.success(e); };
    const auto failure = [](const RequestFailed& e, Context& c) {
      c.backoff(e.now, SubscriptionError::RequestFailed);
    };
    const auto invalidResponse = [](const RequestSucceeded& e, Context& c) {
      c.backoff(e.now, SubscriptionError::InvalidResponse);
    };
    const auto deadline = [](const DeadlineExpired& e, const Context& c) {
      return e.now >= c.data.deadline;
    };
    const auto timeout = [](const DeadlineExpired& e, Context& c) {
      c.backoff(e.now, SubscriptionError::Timeout);
    };
    const auto expired = [](const LeaseExpired& e, const Context& c) {
      return e.now >= c.data.leaseUntil;
    };
    const auto expire = [](const LeaseExpired& e, Context& c) {
      c.backoff(e.now, SubscriptionError::LeaseExpired);
    };
    const auto renewalDue = [](const RenewalDue& e, const Context& c) {
      return e.now >= c.data.renewAt && e.now < c.data.leaseUntil;
    };
    const auto renew = [](const RenewalDue& e, Context& c) { c.start(e.now, true); };
    const auto retryDue = [](const RetryDue& e, const Context& c) {
      return e.now >= c.data.retryAt;
    };
    const auto retry = [](const RetryDue& e, Context& c) { c.start(e.now, false); };
    const auto validNotify = [](const NotifyReceived& e, const Context& c) {
      return !c.sid.empty() && e.sid == c.sid && e.now >= c.data.since && e.now < c.data.leaseUntil;
    };
    const auto notify = [](const NotifyReceived& e, Context& c) {
      c.data.lastNotifyAt = e.now;
      ++c.data.notifications;
      c.effects.topologyChanged();
    };
    const auto unavailable = [](const NetworkUnavailable& e, Context& c) {
      c.stop(e.now, SubscriptionError::NetworkLost);
    };
    const auto shutdown = [](const Shutdown& e, Context& c) {
      c.stop(e.now, SubscriptionError::Shutdown);
    };
    return make_transition_table(
        *state<Inactive> + event<NetworkAvailable>[hasAddress] / replace = state<Subscribing>,
        state<Inactive> + event<NetworkAvailable>[emptyAddress] / replace,
        state<Subscribing> + event<RequestSucceeded>[succeeded] / success = state<Healthy>,
        state<Renewing> + event<RequestSucceeded>[succeeded] / success = state<Healthy>,
        state<Subscribing> + event<RequestSucceeded>[invalid] / invalidResponse = state<Backoff>,
        state<Renewing> + event<RequestSucceeded>[invalid] / invalidResponse = state<Backoff>,
        state<Subscribing> + event<RequestFailed>[failed] / failure = state<Backoff>,
        state<Renewing> + event<RequestFailed>[failed] / failure = state<Backoff>,
        state<Subscribing> + event<DeadlineExpired>[deadline] / timeout = state<Backoff>,
        state<Renewing> + event<DeadlineExpired>[deadline] / timeout = state<Backoff>,
        state<Healthy> + event<RenewalDue>[renewalDue] / renew = state<Renewing>,
        state<Backoff> + event<RetryDue>[retryDue] / retry = state<Subscribing>,
        state<Healthy> + event<LeaseExpired>[expired] / expire = state<Backoff>,
        state<Renewing> + event<LeaseExpired>[expired] / expire = state<Backoff>,
        state<Healthy> + event<NotifyReceived>[validNotify] / notify,
        state<Renewing> + event<NotifyReceived>[validNotify] / notify,
        state<Subscribing> + event<NetworkAvailable>[changedAddress] / replace = state<Subscribing>,
        state<Healthy> + event<NetworkAvailable>[changedAddress] / replace = state<Subscribing>,
        state<Renewing> + event<NetworkAvailable>[changedAddress] / replace = state<Subscribing>,
        state<Backoff> + event<NetworkAvailable>[changedAddress] / replace = state<Subscribing>,
        state<Subscribing> + event<NetworkAvailable>[emptyAddress] / replace = state<Inactive>,
        state<Healthy> + event<NetworkAvailable>[emptyAddress] / replace = state<Inactive>,
        state<Renewing> + event<NetworkAvailable>[emptyAddress] / replace = state<Inactive>,
        state<Backoff> + event<NetworkAvailable>[emptyAddress] / replace = state<Inactive>,
        state<Inactive> + event<NetworkUnavailable> / unavailable,
        state<Subscribing> + event<NetworkUnavailable> / unavailable = state<Inactive>,
        state<Healthy> + event<NetworkUnavailable> / unavailable = state<Inactive>,
        state<Renewing> + event<NetworkUnavailable> / unavailable = state<Inactive>,
        state<Backoff> + event<NetworkUnavailable> / unavailable = state<Inactive>,
        state<Inactive> + event<Shutdown> / shutdown,
        state<Subscribing> + event<Shutdown> / shutdown = state<Inactive>,
        state<Healthy> + event<Shutdown> / shutdown = state<Inactive>,
        state<Renewing> + event<Shutdown> / shutdown = state<Inactive>,
        state<Backoff> + event<Shutdown> / shutdown = state<Inactive>);
  }
};
} // namespace subscription_lifecycle
inline uint64_t parseSubscriptionLeaseMs(const std::string& timeout) {
  if (timeout.size() <= 7 || timeout.compare(0, 7, "Second-") != 0)
    return 0;
  constexpr uint64_t maximumSeconds = subscription_lifecycle::MaximumLeaseMs / 1000;
  uint64_t seconds = 0;
  for (size_t i = 7; i < timeout.size(); ++i) {
    const char digit = timeout[i];
    if (digit < '0' || digit > '9')
      return 0;
    // Validate all digits even after saturating; arbitrarily large values cannot
    // wrap or hide a malformed suffix behind the accepted lease cap.
    if (seconds < maximumSeconds) {
      seconds = seconds * 10 + uint64_t(digit - '0');
      if (seconds > maximumSeconds)
        seconds = maximumSeconds;
    }
  }
  return seconds * 1000;
}
// HTTP/listener ownership and polling fallback belong to the adapter. A pending
// request is usable only while active(id, now); processing its late result is safe.
// NetworkAvailable carries a fresh discovered publisher address, never a cached SID.
class SubscriptionLifecycle {
  subscription_lifecycle::Context context_;
  boost::sml::sm<subscription_lifecycle::Table> machine_;

public:
  explicit SubscriptionLifecycle(SubscriptionEffects& effects)
      : context_{{}, effects, {}, {}}, machine_(context_) {}
  SubscriptionLifecycle(const SubscriptionLifecycle&) = delete;
  SubscriptionLifecycle& operator=(const SubscriptionLifecycle&) = delete;
  template <class Event> bool process(const Event& event) { return machine_.process_event(event); }
  void tick(uint64_t now) {
    const auto s = snapshot();
    if (s.sidPresent && now >= s.leaseUntil)
      process(subscription_lifecycle::LeaseExpired{now});
    else if (s.state == SubscriptionState::Subscribing || s.state == SubscriptionState::Renewing)
      process(subscription_lifecycle::DeadlineExpired{now});
    else if (s.state == SubscriptionState::Healthy)
      process(subscription_lifecycle::RenewalDue{now});
    else if (s.state == SubscriptionState::Backoff)
      process(subscription_lifecycle::RetryDue{now});
  }
  bool active(uint64_t id, uint64_t now) const { return context_.active(id, now); }
  SubscriptionSnapshot snapshot() const {
    auto result = context_.data;
    result.sidPresent = !context_.sid.empty();
    if (machine_.is(boost::sml::state<subscription_lifecycle::Subscribing>))
      result.state = SubscriptionState::Subscribing;
    else if (machine_.is(boost::sml::state<subscription_lifecycle::Healthy>))
      result.state = SubscriptionState::Healthy;
    else if (machine_.is(boost::sml::state<subscription_lifecycle::Renewing>))
      result.state = SubscriptionState::Renewing;
    else if (machine_.is(boost::sml::state<subscription_lifecycle::Backoff>))
      result.state = SubscriptionState::Backoff;
    return result;
  }
};
inline bool subscriptionInvariant(const SubscriptionSnapshot& s) {
  const bool working =
      s.state == SubscriptionState::Subscribing || s.state == SubscriptionState::Renewing;
  const bool leased =
      s.state == SubscriptionState::Healthy || s.state == SubscriptionState::Renewing;
  if (s.sidPresent != leased || s.requestId > s.totalRequests ||
      (s.state != SubscriptionState::Inactive && !s.networkAvailable))
    return false;
  if (working ? s.requestId == 0 || s.deadline <= s.since || s.attempts == 0
              : s.requestId != 0 || s.deadline != 0)
    return false;
  if (leased ? s.renewAt == 0 || s.leaseUntil < s.renewAt : s.renewAt != 0 || s.leaseUntil != 0)
    return false;
  if (s.state == SubscriptionState::Backoff)
    return s.retryAt > s.since && s.retryAt - s.since <= subscription_lifecycle::MaximumBackoffMs;
  return s.retryAt == 0;
}
} // namespace surface::device
