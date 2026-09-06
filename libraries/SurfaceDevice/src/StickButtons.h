#pragma once
#include "SurfaceDevice.h"
#include <utility/Button_Class.hpp>

namespace surface::device {
inline Input stickButtonInput(const m5::Button_Class& a, const m5::Button_Class& b) {
  if (a.wasDoubleClicked()) return Input::RoomNext;
  if (a.wasSingleClicked()) return Input::Refresh;
  if (b.wasClicked()) return Input::Toggle;
  return Input::None;
}
inline bool stickButtonPending(const m5::Button_Class& a, const m5::Button_Class& b) {
  return a.isPressed() || a.getClickCount() != 0 || b.isPressed();
}
} // namespace surface::device
