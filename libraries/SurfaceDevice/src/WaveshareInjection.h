#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

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
  // A host command gap is not a lifted finger. An open injected gesture holds
  // the screen between samples, bounded so a stopped host cannot hold it for
  // longer than a person would notice.
  static constexpr uint32_t gestureTimeoutMs = 1500;
  struct Poll {
    bool cancelled = false; // a physical finger discarded queued samples
    bool consumed = false;  // this sample replaces the hardware sample
    bool held = false;      // consumed while a release is still required
    bool suppress = false;  // no sample yet; the open gesture keeps the screen
    bool expired = false;   // the open gesture timed out; hardware resumes
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
  Poll poll(bool physicalContact, bool releaseRequired, uint32_t now) {
    Poll result;
    if (physicalContact) {
      result.cancelled = count != 0 || gesture;
      cancel();
      return result;
    }
    if (!count) {
      if (!gesture)
        return result;
      if (uint32_t(now - lastSampleMs) <= gestureTimeoutMs) {
        result.suppress = true;
        return result;
      }
      gesture = false;
      result.expired = true;
      return result;
    }
    result.consumed = true;
    result.sample = samples[head];
    head = (head + 1) % capacity;
    --count;
    lastSampleMs = now;
    // Boot, recovery, and navigation require a release before a contact
    // rearms the screen; an injected finger obeys the same rule and starts
    // no gesture, exactly as a held hardware contact starts none.
    result.held = releaseRequired && result.sample.fingers != 0;
    gesture = result.sample.fingers != 0 && !result.held;
    return result;
  }
  void cancel() {
    head = count = 0;
    gesture = false;
  }
  std::size_t pending() const { return count; }
  bool open() const { return gesture; }

private:
  std::array<InjectedTouch, capacity> samples{};
  std::size_t head = 0, count = 0;
  uint32_t lastSampleMs = 0;
  bool gesture = false;
};
} // namespace surface::device
