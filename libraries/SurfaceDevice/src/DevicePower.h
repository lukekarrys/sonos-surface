#pragma once
#include <cstdint>

namespace surface::device {
// Only physical adapters produce activity. AppState and network work have no
// activity channel. A contact can be meaningful even when no action is admitted.
enum class LocalActivity { None, Button, Touch, Nfc };
constexpr uint32_t defaultSleepTimeoutSeconds = 300;

class DevicePower {
  uint64_t lastLocalActivityMs;
  uint64_t timeoutMs;
  bool requested = false;

public:
  explicit DevicePower(uint64_t now = 0, uint32_t seconds = defaultSleepTimeoutSeconds)
      : lastLocalActivityMs(now), timeoutMs(uint64_t(seconds) * 1000) {}
  void recordLocalActivity(uint64_t now) {
    if (!requested)
      lastLocalActivityMs = now;
  }
  uint64_t inactivityMs(uint64_t now) const { return now - lastLocalActivityMs; }
  // The caller supplies monotonic milliseconds. Activity wins at the boundary.
  bool poll(uint64_t now, LocalActivity activity = LocalActivity::None) {
    if (requested)
      return false;
    if (activity != LocalActivity::None)
      recordLocalActivity(now);
    if (!timeoutMs || inactivityMs(now) < timeoutMs)
      return false;
    requested = true;
    return true;
  }
  bool sleepRequested() const { return requested; }
};
} // namespace surface::device
