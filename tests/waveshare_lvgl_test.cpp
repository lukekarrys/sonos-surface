#include "WaveshareButton.h"
#include <cassert>
#include <iostream>
using namespace surface::device;
namespace {
// Feed one raw press of `holdMs` starting at `at`, sampled every 5 ms, and
// count the short presses reported.
unsigned press(BootButton& button, uint32_t& t, uint32_t holdMs, uint32_t settleMs = 200) {
  unsigned actions = 0;
  const auto start = t;
  for (; t < start + holdMs + settleMs; t += 5)
    actions += button.sample(t >= start && t < start + holdMs, t);
  return actions;
}
} // namespace
int main() {
  {
    // The press that woke or booted the device is consumed: no action until a
    // release has been seen, then a normal short press acts on release.
    BootButton button;
    uint32_t t = 0;
    unsigned actions = 0;
    for (; t < 400; t += 5)
      actions += button.sample(true, t);
    assert(actions == 0 && button.pressed() && !button.released());
    for (; t < 500; t += 5)
      actions += button.sample(false, t);
    assert(actions == 0 && !button.pressed() && button.released());
    assert(press(button, t, 100) == 1);
    assert(press(button, t, 100) == 1);
  }
  {
    // Booting with BOOT released arms after one debounced released sample.
    BootButton button;
    uint32_t t = 0;
    for (; t < 100; t += 5)
      assert(!button.sample(false, t));
    assert(button.released());
    assert(press(button, t, 100) == 1);
    // Debounce: a level that does not outlast 30 ms is neither a press nor an
    // action (sampled every 5 ms, a 30 ms pulse never reaches the window).
    assert(press(button, t, 10) == 0 && press(button, t, 25) == 0);
    assert(press(button, t, BootButton::debounceMs) == 0);
    assert(press(button, t, BootButton::debounceMs + 5) == 1);
    // A hold longer than one second performs no action; exactly one second does.
    assert(press(button, t, 1500) == 0);
    assert(press(button, t, BootButton::holdLimitMs) == 1);
    assert(press(button, t, BootButton::holdLimitMs + 5) == 0);
  }
  {
    // A release bounce inside a press is debounced away: one action, on the
    // final release edge, and the action fires exactly once per press.
    BootButton button;
    uint32_t t = 0;
    for (; t < 100; t += 5)
      button.sample(false, t);
    unsigned actions = 0;
    uint32_t actedAt = 0;
    for (; t < 600; t += 5) {
      const bool pressed = (t >= 100 && t < 200) || (t >= 210 && t < 300);
      if (button.sample(pressed, t)) {
        ++actions;
        actedAt = t;
      }
    }
    assert(actions == 1 && actedAt == 330);
    // Activity is the raw level, sampled by the adapter; the debounced state
    // follows 30 ms later.
    assert(!button.pressed());
    for (; t < 620; t += 5)
      button.sample(true, t);
    assert(!button.pressed());
    for (; t < 640; t += 5)
      button.sample(true, t);
    assert(button.pressed());
  }
  std::cout << "Waveshare LVGL checks passed: BOOT debounce/edge/hold/wake rules\n";
}
