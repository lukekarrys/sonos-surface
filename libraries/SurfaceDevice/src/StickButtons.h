#pragma once
#include "SurfaceDevice.h"
#include <utility/Button_Class.hpp>

namespace surface::device {
inline Input stickButtonInput(const m5::Button_Class& a, const m5::Button_Class& b) {
  if (a.wasDoubleClicked())
    return Input::RoomNext;
  if (a.wasSingleClicked())
    return Input::Refresh;
  // Only the finalized count emits an action, never the intermediate releases.
  if (b.wasDecideClickCount()) {
    switch (b.getClickCount()) {
    case 1:
      return Input::Toggle;
    case 2:
      return Input::Next;
    case 3:
      return Input::StickPrevious;
    }
  }
  return Input::None;
}
inline bool stickButtonPending(const m5::Button_Class& a, const m5::Button_Class& b) {
  return a.isPressed() || a.getClickCount() != 0 || b.isPressed() || b.getClickCount() != 0;
}
inline LocalActivity stickButtonActivity(const m5::Button_Class& a, const m5::Button_Class& b) {
  return a.isPressed() || b.isPressed() ? LocalActivity::Button : LocalActivity::None;
}
} // namespace surface::device
