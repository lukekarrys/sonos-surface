#pragma once
#include "WaveshareButton.h"
#include "WaveshareUi.h"

namespace surface::device {
// Top-level screens, cycled by one BOOT short press: Now Playing -> Grouping
// -> Playground -> Now Playing. Rooms and Queue stay sub-views of Now Playing
// (WaveshareUi::screen); the cycle discards an open sub-view and a later
// return shows Now Playing itself.
enum class SurfaceScreen { NowPlaying, Grouping, Playground };
constexpr unsigned surfaceScreenCount = 3;
inline SurfaceScreen nextSurfaceScreen(SurfaceScreen screen) {
  return SurfaceScreen((unsigned(screen) + 1) % surfaceScreenCount);
}
inline const char* surfaceScreenName(SurfaceScreen screen) {
  switch (screen) {
  case SurfaceScreen::Grouping:
    return "grouping";
  case SurfaceScreen::Playground:
    return "playground";
  default:
    return "now";
  }
}

// Portable navigation state and touch ownership. This is UI state only: it is
// never persisted, every boot and wake starts on Now Playing, and changing
// screens never touches AppState, the selected room, or accepted Sonos work.
// On the device each top-level screen is one LVGL screen; the adapter loads
// the active one after every change here and cancels the LVGL press itself.
class WaveshareShell {
  SurfaceScreen active_ = SurfaceScreen::NowPlaying;
  BootButton boot_;
  // Leaving a screen cancels the touch interaction it owns and discards an
  // open Now Playing sub-view. A gesture in progress (including a volume or
  // seek preview) requires a release before the next contact rearms, so
  // nothing started here completes elsewhere.
  void leave() {
    if (nowPlaying.contactActive())
      nowPlaying.cancelTouch();
    nowPlaying.screen = WaveshareScreen::NowPlaying;
  }

public:
  WaveshareUi nowPlaying;
  SurfaceScreen active() const { return active_; }
  // The one explicit navigation action: BOOT release, `ui-button boot`, or
  // `ui-nav next`. Returns the screen now active.
  SurfaceScreen next() {
    leave();
    active_ = nextSurfaceScreen(active_);
    nowPlaying.dirty = true;
    return active_;
  }
  // `ui-screen`: jump to a screen and, for Now Playing, a sub-view.
  void show(SurfaceScreen screen, WaveshareScreen view = WaveshareScreen::NowPlaying) {
    leave();
    active_ = screen;
    nowPlaying.screen = screen == SurfaceScreen::NowPlaying ? view : WaveshareScreen::NowPlaying;
    nowPlaying.dirty = true;
  }
  // One raw BOOT sample per poll. The low level is local activity exactly as
  // before; the debounced release edge of a press no longer than one second is
  // the navigation action, and the press that woke or booted the device is
  // consumed until a release. Returns true when it navigated.
  bool boot(bool low, uint32_t now, LocalActivity& activity) {
    if (low)
      activity = LocalActivity::Button;
    if (!boot_.sample(low, now))
      return false;
    next();
    return true;
  }
  // Widget reports from LVGL's hit-testing. They can only come from Now
  // Playing's widgets, which exist on that screen alone; the shell still
  // drops them anywhere else so no hidden screen ever reacts.
  void pressed(WaveshareControl control) {
    if (active_ == SurfaceScreen::NowPlaying)
      nowPlaying.pressed(control);
  }
  void pressLost() {
    if (active_ == SurfaceScreen::NowPlaying)
      nowPlaying.pressLost();
  }
  void slid(WaveshareControl control, uint32_t value, uint32_t maximum) {
    if (active_ == SurfaceScreen::NowPlaying)
      nowPlaying.slid(control, value, maximum);
  }
  // Every touch sample reaches only the active screen. Now Playing keeps its
  // interaction model; Grouping and Playground own no application actions and
  // their local state lives in their widgets.
  BoardEvent touch(int x, int y, int fingers, uint32_t now) {
    if (active_ != SurfaceScreen::NowPlaying)
      return {};
    auto event = nowPlaying.touch(x, y, fingers, now);
    if (!fingers && event.input == Input::None)
      return nowPlaying.requestQueue();
    return event;
  }
  // Observation replacement: a room change resets the Now Playing sub-view
  // and never changes the top-level screen.
  void update(const AppState& next, const BoardContext& context, uint32_t now) {
    nowPlaying.update(next, context, now);
  }
};
} // namespace surface::device
