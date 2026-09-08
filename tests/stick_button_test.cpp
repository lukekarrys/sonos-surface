#include "StickButtons.h"
#include <cassert>
#include <iostream>
using namespace surface::device;

// Run the pinned M5 debouncer/click counter and the exact adapter mapping with
// timestamped raw edges. No Arduino SDK, guessed event flags, or real sleeps.
int main() {
  for (unsigned spacing : {100u, 200u, 400u}) {
    m5::Button_Class a, b;
    unsigned rooms = 0, refreshes = 0;
    bool deferred = false;
    for (uint32_t t = 0; t < 1500; t += 5) {
      bool pressed = (t >= 100 && t < 160) || (t >= 100 + spacing && t < 160 + spacing);
      a.setRawState(t, pressed);
      b.setRawState(t, false);
      const auto event = stickButtonInput(a, b);
      rooms += event == Input::RoomNext;
      refreshes += event == Input::Refresh;
      if (t == 180)
        deferred = stickButtonPending(a, b);
    }
    assert(rooms == 1 && refreshes == 0 && deferred && !stickButtonPending(a, b));
  }
  for (bool hold : {false, true}) {
    m5::Button_Class a, b;
    unsigned rooms = 0, refreshes = 0;
    for (uint32_t t = 0; t < 1800; t += 5) {
      a.setRawState(t, t >= 100 && t < (hold ? 800u : 160u));
      b.setRawState(t, false);
      auto event = stickButtonInput(a, b);
      rooms += event == Input::RoomNext;
      refreshes += event == Input::Refresh;
    }
    assert(rooms == 0 && refreshes == (hold ? 0u : 1u) && !stickButtonPending(a, b));
  }
  m5::Button_Class a, b;
  unsigned toggles = 0;
  for (uint32_t t = 0; t < 1000; t += 5) {
    a.setRawState(t, false);
    b.setRawState(t, t >= 100 && t < 160);
    toggles += stickButtonInput(a, b) == Input::Toggle;
  }
  assert(toggles == 1);
  std::cout
      << "Stick button checks passed: single/double/hold/B, deferred NFC, one action per gesture\n";
}
