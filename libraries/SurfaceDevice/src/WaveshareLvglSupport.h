#pragma once
#include <cstdint>
#include <string>

namespace surface::device {
// Pure helpers for the LVGL playground adapter; no LVGL or Arduino headers so
// they compile in host tests.

// Inclusive area bounds, LVGL's lv_area_t convention.
struct LvglArea {
  int x1, y1, x2, y2;
};
// The pinned CO5300 driver performs no address-window alignment. Grow an
// invalidated area outward to an even start and an even size (an odd inclusive
// end), clamped to the panel, so every flushed window has even x/y/w/h.
inline LvglArea evenAlignedArea(LvglArea area, int width, int height) {
  area.x1 &= ~1;
  area.y1 &= ~1;
  area.x2 |= 1;
  area.y2 |= 1;
  if (area.x2 >= width)
    area.x2 = width - 1;
  if (area.y2 >= height)
    area.y2 = height - 1;
  return area;
}

// Section 6 of todo/3-lvgl.md, in order: PARTIAL with a small internal
// stripe, PARTIAL plus even alignment, then DIRECT into the PSRAM canvas with
// the known-good full-frame flush. DIRECT is the default: the panel has no
// tearing-effect line, and partial windows tear on any motion (hardware.md).
enum class LvglRenderMode { Partial, PartialEven, Direct };
inline const char* lvglRenderModeName(LvglRenderMode mode) {
  switch (mode) {
  case LvglRenderMode::PartialEven:
    return "even";
  case LvglRenderMode::Direct:
    return "direct";
  default:
    return "partial";
  }
}
inline bool parseLvglRenderMode(const std::string& text, LvglRenderMode& mode) {
  if (text == "partial")
    mode = LvglRenderMode::Partial;
  else if (text == "even")
    mode = LvglRenderMode::PartialEven;
  else if (text == "direct")
    mode = LvglRenderMode::Direct;
  else
    return false;
  return true;
}

// Playground screens cycle on one BOOT short press or `ui-nav next`.
enum class PlaygroundScreen { Targets, Slider, Canvas };
constexpr unsigned playgroundScreenCount = 3;
inline PlaygroundScreen nextPlaygroundScreen(PlaygroundScreen screen) {
  return PlaygroundScreen((unsigned(screen) + 1) % playgroundScreenCount);
}
inline const char* playgroundScreenName(PlaygroundScreen screen) {
  switch (screen) {
  case PlaygroundScreen::Slider:
    return "slider";
  case PlaygroundScreen::Canvas:
    return "canvas";
  default:
    return "targets";
  }
}

// The latest calibrated sample the adapter delivered; LVGL's read callback
// reports exactly this. Injected samples arrive through the same call.
struct LvglTouchSample {
  int x = 0, y = 0;
  bool pressed = false;
  uint32_t at = 0;
};
} // namespace surface::device
