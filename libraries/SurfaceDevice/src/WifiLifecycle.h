#pragma once
#include <surface_sml.hpp>
#include <cstdint>
#include <limits>

namespace surface::device {
enum class WifiState { Unconfigured, Connecting, Online, Backoff };
enum class WifiError { None, ConnectTimeout, Disconnected, ConfigRemoved, Shutdown };
inline const char* wifiStateName(WifiState state) {
  switch (state) {
  case WifiState::Unconfigured:
    return "Unconfigured";
  case WifiState::Connecting:
    return "Connecting";
  case WifiState::Online:
    return "Online";
  case WifiState::Backoff:
    return "Backoff";
  }
  return "unknown";
}
inline const char* wifiErrorName(WifiError error) {
  switch (error) {
  case WifiError::None:
    return "none";
  case WifiError::ConnectTimeout:
    return "connect-timeout";
  case WifiError::Disconnected:
    return "disconnected";
  case WifiError::ConfigRemoved:
    return "config-removed";
  case WifiError::Shutdown:
    return "shutdown";
  }
  return "unknown";
}
struct WifiSnapshot {
  WifiState state = WifiState::Unconfigured;
  uint64_t since = 0, deadline = 0, retryAt = 0, attemptId = 0;
  uint64_t totalAttempts = 0, connections = 0;
  uint32_t attempts = 0;
  WifiError lastError = WifiError::None;
  bool networkReady = false;
};
struct WifiEffects {
  virtual ~WifiEffects() = default;
  virtual void beginConnect(uint64_t attemptId) = 0;
  virtual void disconnect() = 0;
  virtual void availabilityChanged(bool available) = 0;
};
namespace wifi_lifecycle {
inline constexpr uint64_t ConnectBudgetMs = 15000;
inline constexpr uint64_t InitialBackoffMs = 1000;
inline constexpr uint64_t MaximumBackoffMs = 30000;
struct Unconfigured {};
struct Connecting {};
struct Online {};
struct Backoff {};
struct ConfigAvailable {
  uint64_t now;
};
struct ConfigRemoved {
  uint64_t now;
};
struct Connected {
  uint64_t now, attemptId;
};
struct Disconnected {
  uint64_t now, attemptId;
};
struct ConnectTimeout {
  uint64_t now;
};
struct RetryDue {
  uint64_t now;
};
struct Shutdown {
  uint64_t now;
};
struct Context {
  WifiSnapshot data;
  WifiEffects& effects;
  void begin(uint64_t now) {
    data.since = now;
    data.deadline = now + ConnectBudgetMs;
    data.retryAt = 0;
    data.attemptId = ++data.totalAttempts;
    if (data.attempts != std::numeric_limits<uint32_t>::max())
      ++data.attempts;
    effects.beginConnect(data.attemptId);
  }
  void connected(uint64_t now) {
    data.since = now;
    data.deadline = 0;
    data.attempts = 0;
    ++data.connections;
    data.lastError = WifiError::None;
    data.networkReady = true;
    effects.availabilityChanged(true);
  }
  void backoff(uint64_t now, WifiError error) {
    uint64_t delay = InitialBackoffMs;
    for (uint32_t attempt = 1; attempt < data.attempts && delay < MaximumBackoffMs; ++attempt)
      delay = delay * 2 < MaximumBackoffMs ? delay * 2 : MaximumBackoffMs;
    data.since = now;
    data.deadline = data.attemptId = 0;
    data.retryAt = now + delay;
    data.lastError = error;
    const bool wasReady = data.networkReady;
    data.networkReady = false;
    if (wasReady)
      effects.availabilityChanged(false);
    effects.disconnect();
  }
  void stop(uint64_t now, WifiError error, bool active) {
    data.since = now;
    data.deadline = data.retryAt = data.attemptId = 0;
    data.attempts = 0;
    data.lastError = error;
    const bool wasReady = data.networkReady;
    data.networkReady = false;
    if (wasReady)
      effects.availabilityChanged(false);
    if (active)
      effects.disconnect();
  }
};
struct Table {
  auto operator()() const {
    using namespace boost::sml;
    const auto connected = [](const Connected& e, const Context& c) {
      return e.attemptId == c.data.attemptId && e.now >= c.data.since && e.now < c.data.deadline;
    };
    const auto disconnected = [](const Disconnected& e, const Context& c) {
      return e.attemptId == c.data.attemptId && e.now >= c.data.since;
    };
    const auto timeout = [](const ConnectTimeout& e, const Context& c) {
      return e.now >= c.data.deadline;
    };
    const auto retry = [](const RetryDue& e, const Context& c) { return e.now >= c.data.retryAt; };
    const auto begin = [](const ConfigAvailable& e, Context& c) {
      c.data.lastError = WifiError::None;
      c.begin(e.now);
    };
    const auto retryConnect = [](const RetryDue& e, Context& c) { c.begin(e.now); };
    const auto online = [](const Connected& e, Context& c) { c.connected(e.now); };
    const auto timedOut = [](const ConnectTimeout& e, Context& c) {
      c.backoff(e.now, WifiError::ConnectTimeout);
    };
    const auto lost = [](const Disconnected& e, Context& c) {
      c.backoff(e.now, WifiError::Disconnected);
    };
    const auto removeActive = [](const ConfigRemoved& e, Context& c) {
      c.stop(e.now, WifiError::ConfigRemoved, true);
    };
    const auto removeInactive = [](const ConfigRemoved& e, Context& c) {
      c.stop(e.now, WifiError::ConfigRemoved, false);
    };
    const auto shutdownActive = [](const Shutdown& e, Context& c) {
      c.stop(e.now, WifiError::Shutdown, true);
    };
    const auto shutdownInactive = [](const Shutdown& e, Context& c) {
      c.stop(e.now, WifiError::Shutdown, false);
    };
    return make_transition_table(
        *state<Unconfigured> + event<ConfigAvailable> / begin = state<Connecting>,
        state<Connecting> + event<Connected>[connected] / online = state<Online>,
        state<Connecting> + event<ConnectTimeout>[timeout] / timedOut = state<Backoff>,
        state<Connecting> + event<Disconnected>[disconnected] / lost = state<Backoff>,
        state<Online> + event<Disconnected>[disconnected] / lost = state<Backoff>,
        state<Backoff> + event<RetryDue>[retry] / retryConnect = state<Connecting>,
        state<Connecting> + event<ConfigRemoved> / removeActive = state<Unconfigured>,
        state<Online> + event<ConfigRemoved> / removeActive = state<Unconfigured>,
        state<Backoff> + event<ConfigRemoved> / removeInactive = state<Unconfigured>,
        state<Connecting> + event<Shutdown> / shutdownActive = state<Unconfigured>,
        state<Online> + event<Shutdown> / shutdownActive = state<Unconfigured>,
        state<Backoff> + event<Shutdown> / shutdownInactive = state<Unconfigured>);
  }
};
} // namespace wifi_lifecycle
// The adapter owns credentials and actual connection work. Callbacks carry the
// attempt identity; disconnected polls during Connecting are not failures.
// ConfigAvailable is idempotent until ConfigRemoved or Shutdown is processed.
class WifiLifecycle {
  wifi_lifecycle::Context context_;
  boost::sml::sm<wifi_lifecycle::Table> machine_;

public:
  explicit WifiLifecycle(WifiEffects& effects) : context_{{}, effects}, machine_(context_) {}
  WifiLifecycle(const WifiLifecycle&) = delete;
  WifiLifecycle& operator=(const WifiLifecycle&) = delete;
  template <class Event> bool process(const Event& event) { return machine_.process_event(event); }
  void tick(uint64_t now) {
    const auto state = snapshot().state;
    if (state == WifiState::Connecting)
      process(wifi_lifecycle::ConnectTimeout{now});
    else if (state == WifiState::Backoff)
      process(wifi_lifecycle::RetryDue{now});
  }
  WifiSnapshot snapshot() const {
    auto result = context_.data;
    if (machine_.is(boost::sml::state<wifi_lifecycle::Connecting>))
      result.state = WifiState::Connecting;
    else if (machine_.is(boost::sml::state<wifi_lifecycle::Online>))
      result.state = WifiState::Online;
    else if (machine_.is(boost::sml::state<wifi_lifecycle::Backoff>))
      result.state = WifiState::Backoff;
    return result;
  }
};
inline bool wifiInvariant(const WifiSnapshot& s) {
  if (s.networkReady != (s.state == WifiState::Online) || s.connections > s.totalAttempts ||
      s.attemptId > s.totalAttempts)
    return false;
  switch (s.state) {
  case WifiState::Unconfigured:
    return s.attemptId == 0 && s.deadline == 0 && s.retryAt == 0 && s.attempts == 0;
  case WifiState::Connecting:
    return s.attemptId != 0 && s.attempts != 0 && s.deadline > s.since && s.retryAt == 0;
  case WifiState::Online:
    return s.attemptId != 0 && s.deadline == 0 && s.retryAt == 0 && s.attempts == 0;
  case WifiState::Backoff:
    return s.attemptId == 0 && s.deadline == 0 && s.retryAt > s.since &&
           s.retryAt - s.since <= wifi_lifecycle::MaximumBackoffMs;
  }
  return false;
}
} // namespace surface::device
