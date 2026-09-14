#pragma once
#include <cstdint>

namespace surface::device {
// BOOT (GPIO0) short-press detection for screen navigation. Portable: the
// adapter feeds raw samples with millis(). The raw level is still the local
// activity channel exactly as before; the action is the debounced release edge
// of a press no longer than the hold limit. BOOT is the deep-sleep wake source
// and is still low while the device boots, so no press counts until a release
// has been observed (the same rule Stick applies after boot).
class BootButton {
public:
  static constexpr uint32_t debounceMs = 30, holdLimitMs = 1000;
  // Returns true exactly once per completed short press.
  bool sample(bool pressed, uint32_t now) {
    if (pressed != raw) {
      raw = pressed;
      rawSince = now;
    }
    if (uint32_t(now - rawSince) < debounceMs)
      return false;
    if (raw) {
      if (!stable) {
        stable = true;
        pressedAt = rawSince;
        countedPress = armed;
      }
      return false;
    }
    const bool released = stable;
    stable = false;
    armed = true;
    return released && countedPress && uint32_t(rawSince - pressedAt) <= holdLimitMs;
  }
  bool pressed() const { return stable; }
  // False until the first debounced release after boot.
  bool released() const { return armed; }

private:
  bool raw = false, stable = false, armed = false, countedPress = false;
  uint32_t rawSince = 0, pressedAt = 0;
};
} // namespace surface::device
