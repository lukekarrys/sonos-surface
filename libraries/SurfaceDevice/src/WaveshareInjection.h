#pragma once
#include <array>
#include <cstddef>

namespace surface::device {
// Host-injected touch samples for autonomous verification. Values are already
// calibrated screen coordinates, so they replace the hardware sample after the
// calibration step and change nothing else: admission, read_only, and policy
// still see exactly a finger. Injection is never local activity.
struct InjectedTouch {
  int x = 0, y = 0, fingers = 0; // fingers == 0 is a release sample
};
class InjectedTouchQueue {
public:
  static constexpr std::size_t capacity = 32;
  struct Poll {
    bool cancelled = false; // a physical finger discarded queued samples
    bool consumed = false;  // this sample replaces the hardware sample
    bool held = false;      // consumed while a release is still required
    InjectedTouch sample;
  };
  bool push(const InjectedTouch& sample) {
    if (count == capacity)
      return false;
    samples[(head + count) % capacity] = sample;
    ++count;
    return true;
  }
  // One sample per touch poll, in order. A physical finger always wins and
  // cancels the rest, so a person can take the screen back mid-sequence.
  Poll poll(bool physicalContact, bool releaseRequired) {
    Poll result;
    if (physicalContact) {
      result.cancelled = count != 0;
      cancel();
      return result;
    }
    if (!count)
      return result;
    result.consumed = true;
    result.sample = samples[head];
    head = (head + 1) % capacity;
    --count;
    // Boot, recovery, and navigation require a release before a contact
    // rearms the screen; an injected finger obeys the same rule.
    result.held = releaseRequired && result.sample.fingers != 0;
    return result;
  }
  void cancel() { head = count = 0; }
  std::size_t pending() const { return count; }

private:
  std::array<InjectedTouch, capacity> samples{};
  std::size_t head = 0, count = 0;
};
} // namespace surface::device
