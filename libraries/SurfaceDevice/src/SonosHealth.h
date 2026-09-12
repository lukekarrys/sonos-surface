#pragma once
#include <surface_sml.hpp>
#include <cstdint>
#include <limits>

namespace surface::device {
enum class SonosState { Offline, Discovering, Ready, Backoff };
enum class SonosError {
  None,
  NetworkLost,
  DiscoveryFailed,
  DiscoveryTimeout,
  SessionFailure,
  Shutdown
};
inline const char* sonosStateName(SonosState state) {
  switch (state) {
  case SonosState::Offline:
    return "Offline";
  case SonosState::Discovering:
    return "Discovering";
  case SonosState::Ready:
    return "Ready";
  case SonosState::Backoff:
    return "Backoff";
  }
  return "unknown";
}
inline const char* sonosErrorName(SonosError error) {
  switch (error) {
  case SonosError::None:
    return "none";
  case SonosError::NetworkLost:
    return "network-lost";
  case SonosError::DiscoveryFailed:
    return "discovery-failed";
  case SonosError::DiscoveryTimeout:
    return "discovery-timeout";
  case SonosError::SessionFailure:
    return "session-failure";
  case SonosError::Shutdown:
    return "shutdown";
  }
  return "unknown";
}
struct SonosSnapshot {
  SonosState state = SonosState::Offline;
  uint64_t since = 0, deadline = 0, retryAt = 0, discoveryId = 0;
  uint64_t totalDiscoveries = 0, successfulDiscoveries = 0, lastSuccessfulDiscovery = 0;
  uint64_t staleResults = 0;
  uint32_t attempts = 0, consecutiveSessionFailures = 0;
  SonosError lastError = SonosError::None;
  bool networkAvailable = false, recovering = false;
};
struct SonosEffects {
  virtual ~SonosEffects() = default;
  virtual void discoveryRequested(uint64_t discoveryId) = 0;
  // Always revoke session authority, retaining observed UI state. Discovery
  // invalidation also discards the topology host and subscription authority.
  virtual void invalidateAuthority(bool discovery) = 0;
  // Recovery schedules an authoritative read, never a saved mutation command.
  virtual void reconciliationRequested() = 0;
};
namespace sonos_health {
inline constexpr uint64_t DiscoveryBudgetMs = 45000;
inline constexpr uint64_t InitialBackoffMs = 1000;
inline constexpr uint64_t MaximumBackoffMs = 30000;
struct Offline {};
struct Discovering {};
struct Ready {};
struct Backoff {};
struct NetworkAvailable {
  uint64_t now;
};
struct NetworkUnavailable {
  uint64_t now;
};
struct RefreshRequested {
  uint64_t now;
};
struct DiscoverySucceeded {
  uint64_t now, id;
};
struct DiscoveryFailed {
  uint64_t now, id;
};
struct DiscoveryTimeout {
  uint64_t now;
};
struct RetryDue {
  uint64_t now;
};
struct SessionFailure {
  uint64_t now, id;
};
struct SessionSucceeded {
  uint64_t now, id;
};
struct Shutdown {
  uint64_t now;
};
struct Context {
  SonosSnapshot data;
  SonosEffects& effects;
  void begin(uint64_t now, bool recovering) {
    data.since = now;
    data.deadline = now + DiscoveryBudgetMs;
    data.retryAt = 0;
    data.discoveryId = ++data.totalDiscoveries;
    data.recovering = recovering;
    if (data.attempts != std::numeric_limits<uint32_t>::max())
      ++data.attempts;
    effects.discoveryRequested(data.discoveryId);
  }
  void ready(uint64_t now) {
    data.since = data.lastSuccessfulDiscovery = now;
    data.deadline = 0;
    data.attempts = 0;
    data.lastError = SonosError::None;
    ++data.successfulDiscoveries;
    const bool recovered = data.recovering;
    data.recovering = false;
    if (recovered)
      effects.reconciliationRequested();
  }
  void backoff(uint64_t now, SonosError error) {
    if (error == SonosError::SessionFailure &&
        data.consecutiveSessionFailures != std::numeric_limits<uint32_t>::max())
      ++data.consecutiveSessionFailures;
    const auto failures =
        error == SonosError::SessionFailure && data.consecutiveSessionFailures > data.attempts
            ? data.consecutiveSessionFailures
            : data.attempts;
    uint64_t delay = InitialBackoffMs;
    for (uint32_t attempt = 1; attempt < failures && delay < MaximumBackoffMs; ++attempt)
      delay = delay * 2 < MaximumBackoffMs ? delay * 2 : MaximumBackoffMs;
    data.since = now;
    data.deadline = data.discoveryId = 0;
    data.retryAt = now + delay;
    data.lastError = error;
    data.recovering = true;
    effects.invalidateAuthority(error != SonosError::SessionFailure);
  }
  void stop(uint64_t now, SonosError error) {
    data.since = now;
    data.deadline = data.retryAt = data.discoveryId = 0;
    data.attempts = data.consecutiveSessionFailures = 0;
    data.networkAvailable = data.recovering = false;
    data.lastError = error;
    effects.invalidateAuthority(true);
  }
};
struct Table {
  auto operator()() const {
    using namespace boost::sml;
    const auto begin = [](const NetworkAvailable& e, Context& c) {
      c.data.networkAvailable = true;
      c.begin(e.now, true);
    };
    const auto refresh = [](const RefreshRequested& e, Context& c) { c.begin(e.now, false); };
    const auto success = [](const DiscoverySucceeded& e, const Context& c) {
      return e.id == c.data.discoveryId && e.now >= c.data.since && e.now < c.data.deadline;
    };
    const auto failure = [](const DiscoveryFailed& e, const Context& c) {
      return e.id == c.data.discoveryId && e.now >= c.data.since;
    };
    const auto session = [](const SessionFailure& e, const Context& c) {
      return e.id == c.data.discoveryId && e.now >= c.data.since;
    };
    const auto sessionSuccess = [](const SessionSucceeded& e, const Context& c) {
      return e.id == c.data.discoveryId && e.now >= c.data.since;
    };
    const auto late = [](Context& c) { ++c.data.staleResults; };
    const auto ready = [](const DiscoverySucceeded& e, Context& c) { c.ready(e.now); };
    const auto failed = [](const DiscoveryFailed& e, Context& c) {
      c.backoff(e.now, SonosError::DiscoveryFailed);
    };
    const auto sessionFailed = [](const SessionFailure& e, Context& c) {
      c.backoff(e.now, SonosError::SessionFailure);
    };
    const auto timeout = [](const DiscoveryTimeout& e, const Context& c) {
      return e.now >= c.data.deadline;
    };
    const auto timedOut = [](const DiscoveryTimeout& e, Context& c) {
      c.backoff(e.now, SonosError::DiscoveryTimeout);
    };
    const auto retry = [](const RetryDue& e, const Context& c) { return e.now >= c.data.retryAt; };
    const auto retryDiscovery = [](const RetryDue& e, Context& c) { c.begin(e.now, true); };
    const auto offline = [](const NetworkUnavailable& e, Context& c) {
      c.stop(e.now, SonosError::NetworkLost);
    };
    const auto shutdown = [](const Shutdown& e, Context& c) {
      c.stop(e.now, SonosError::Shutdown);
    };
    return make_transition_table(
        *state<Offline> + event<NetworkAvailable> / begin = state<Discovering>,
        state<Ready> + event<RefreshRequested> / refresh = state<Discovering>,
        state<Discovering> + event<DiscoverySucceeded>[success] / ready = state<Ready>,
        state<Discovering> + event<DiscoveryFailed>[failure] / failed = state<Backoff>,
        state<Discovering> + event<DiscoveryTimeout>[timeout] / timedOut = state<Backoff>,
        state<Ready> + event<SessionFailure>[session] / sessionFailed = state<Backoff>,
        state<Discovering> + event<SessionFailure>[session] / sessionFailed = state<Backoff>,
        state<Ready> + event<SessionSucceeded>[sessionSuccess] /
                           [](Context& c) { c.data.consecutiveSessionFailures = 0; },
        state<Backoff> + event<RetryDue>[retry] / retryDiscovery = state<Discovering>,
        state<Discovering> + event<DiscoverySucceeded>[!success] / late,
        state<Discovering> + event<DiscoveryFailed>[!failure] / late,
        state<Discovering> + event<SessionFailure>[!session] / late,
        state<Ready> + event<SessionFailure>[!session] / late,
        state<Ready> + event<SessionSucceeded>[!sessionSuccess] / late,
        state<Discovering> + event<SessionSucceeded> / late,
        state<Offline> + event<SessionSucceeded> / late,
        state<Backoff> + event<SessionSucceeded> / late,
        state<Offline> + event<DiscoverySucceeded> / late,
        state<Offline> + event<DiscoveryFailed> / late,
        state<Offline> + event<SessionFailure> / late,
        state<Backoff> + event<DiscoverySucceeded> / late,
        state<Backoff> + event<DiscoveryFailed> / late,
        state<Backoff> + event<SessionFailure> / late,
        state<Ready> + event<DiscoverySucceeded> / late,
        state<Ready> + event<DiscoveryFailed> / late,
        state<Discovering> + event<NetworkUnavailable> / offline = state<Offline>,
        state<Ready> + event<NetworkUnavailable> / offline = state<Offline>,
        state<Backoff> + event<NetworkUnavailable> / offline = state<Offline>,
        state<Discovering> + event<Shutdown> / shutdown = state<Offline>,
        state<Ready> + event<Shutdown> / shutdown = state<Offline>,
        state<Backoff> + event<Shutdown> / shutdown = state<Offline>);
  }
};
} // namespace sonos_health
// Each worker job requests fresh discovery before using room addresses. An
// already requested recovery discovery can be consumed by the next worker job.
// The adapter binds that job to discoveryId and checks usable(id) at mutation
// dispatch. Cached observations and actual HTTP/SSDP work live outside this model.
class SonosHealth {
  sonos_health::Context context_;
  boost::sml::sm<sonos_health::Table> machine_;

public:
  explicit SonosHealth(SonosEffects& effects) : context_{{}, effects}, machine_(context_) {}
  SonosHealth(const SonosHealth&) = delete;
  SonosHealth& operator=(const SonosHealth&) = delete;
  template <class Event> bool process(const Event& event) { return machine_.process_event(event); }
  void tick(uint64_t now) {
    const auto state = snapshot().state;
    if (state == SonosState::Discovering)
      process(sonos_health::DiscoveryTimeout{now});
    else if (state == SonosState::Backoff)
      process(sonos_health::RetryDue{now});
  }
  SonosSnapshot snapshot() const {
    auto result = context_.data;
    if (machine_.is(boost::sml::state<sonos_health::Discovering>))
      result.state = SonosState::Discovering;
    else if (machine_.is(boost::sml::state<sonos_health::Ready>))
      result.state = SonosState::Ready;
    else if (machine_.is(boost::sml::state<sonos_health::Backoff>))
      result.state = SonosState::Backoff;
    return result;
  }
  bool usable(uint64_t discoveryId) const {
    const auto s = snapshot();
    return s.state == SonosState::Ready && s.networkAvailable && discoveryId == s.discoveryId;
  }
};
inline bool sonosInvariant(const SonosSnapshot& s) {
  if (s.networkAvailable != (s.state != SonosState::Offline) ||
      s.successfulDiscoveries > s.totalDiscoveries || s.discoveryId > s.totalDiscoveries)
    return false;
  switch (s.state) {
  case SonosState::Offline:
    return s.discoveryId == 0 && s.deadline == 0 && s.retryAt == 0 && s.attempts == 0 &&
           !s.recovering && s.consecutiveSessionFailures == 0;
  case SonosState::Discovering:
    return s.discoveryId != 0 && s.deadline > s.since && s.retryAt == 0 && s.attempts != 0;
  case SonosState::Ready:
    return s.discoveryId != 0 && s.deadline == 0 && s.retryAt == 0 && s.attempts == 0 &&
           !s.recovering && s.successfulDiscoveries != 0;
  case SonosState::Backoff:
    return s.discoveryId == 0 && s.deadline == 0 && s.retryAt > s.since && s.recovering &&
           s.retryAt - s.since <= sonos_health::MaximumBackoffMs;
  }
  return false;
}
} // namespace surface::device
