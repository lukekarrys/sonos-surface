#include "StickButtons.h"
#include "StickPlayback.h"
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
  for (unsigned clicks : {1u, 2u, 3u, 4u}) {
    for (unsigned spacing : {100u, 200u, 400u}) {
      m5::Button_Class a, b;
      DevicePower power(0, 1);
      std::vector<Input> actions;
      uint32_t lastRelease = 0, decided = 0;
      for (uint32_t t = 0; t < 2500; t += 5) {
        bool pressed = false;
        for (unsigned n = 0; n < clicks; ++n)
          pressed |= t >= 100 + n * spacing && t < 160 + n * spacing;
        a.setRawState(t, false);
        b.setRawState(t, pressed);
        const auto activity = stickButtonActivity(a, b);
        if (activity == LocalActivity::Button) {
          assert(!power.poll(t, activity) && power.inactivityMs(t) == 0);
          assert(stickButtonPending(a, b));
        }
        if (b.wasClicked())
          lastRelease = t;
        if (b.wasDecideClickCount())
          decided = t;
        const auto event = stickButtonInput(a, b);
        if (event != Input::None) {
          assert(b.wasDecideClickCount());
          actions.push_back(event);
        }
        if (t == 120)
          assert(actions.empty() && power.inactivityMs(t) == 0);
      }
      // Strictly greater than 500 ms after the final debounced release:
      // with 5-ms sampling, the decision arrives 505 ms after that release.
      assert(decided == lastRelease + 505 && !stickButtonPending(a, b));
      const std::vector<Input> expected =
          clicks == 4 ? std::vector<Input>{}
                      : std::vector<Input>{clicks == 1   ? Input::Toggle
                                           : clicks == 2 ? Input::Next
                                                         : Input::StickPrevious};
      assert(actions == expected);
    }
  }
  // A side-button hold remains activity, with no music action on release.
  {
    m5::Button_Class a, b;
    for (uint32_t t = 0; t < 1800; t += 5) {
      a.setRawState(t, false);
      b.setRawState(t, t >= 100 && t < 800);
      assert(stickButtonInput(a, b) == Input::None);
      if (b.isPressed())
        assert(stickButtonActivity(a, b) == LocalActivity::Button);
    }
    assert(!stickButtonPending(a, b));
  }
  surface::AppState state;
  auto& o = state.observed;
  o.known = true;
  o.stale = false;
  o.targetId = "room";
  o.trackUri = "track";
  o.queueRevision = 7;
  o.observedAtMs = 100;
  o.seekable = true;
  auto resolve = [&] { return stickPreviousEvent(state, "room", true, 200); };
  for (uint32_t position : {0u, 1000u, 3000u, 3001u, 102000u}) {
    o.positionMs = position;
    const auto event = resolve();
    assert(event.input == Input::Intent && event.targetId == "room");
    if (position > 3000) {
      assert(event.intent.seekPositionMs == 0 && !event.intent.transport);
      assert(event.trackIdentity == "track" && event.queueRevision == 7);
    } else
      assert(!event.intent.seekPositionMs &&
             event.intent.transport == surface::TransportCommand::Previous);
  }
  for (unsigned fallback = 0; fallback < 10; ++fallback) {
    auto changed = state;
    switch (fallback) {
    case 0:
      changed.observed.positionMs.reset();
      break;
    case 1:
      changed.observed.seekable = false;
      break;
    case 2:
      changed.observed.seekable.reset();
      break;
    case 3:
      changed.observed.stale = true;
      break;
    case 4:
      changed.observed.known = false;
      break;
    case 5:
      changed.observed.targetId = "other";
      break;
    case 6:
      changed.observed.trackUri.clear();
      break;
    case 7:
      changed.observed.observedAtMs = 201;
      break;
    default:
      break;
    }
    const auto event =
        stickPreviousEvent(changed, "room", fallback != 8, fallback == 9 ? 10101 : 200);
    assert(!event.intent.seekPositionMs &&
           event.intent.transport == surface::TransportCommand::Previous);
  }
  assert(stickPreviousEvent(state, "room", true, 10100).intent.seekPositionMs == 0);
  std::cout
      << "Stick button checks passed: finalized gestures, immediate activity, smart previous\n";
}
