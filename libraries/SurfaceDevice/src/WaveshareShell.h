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
  // seek drag on Now Playing or Playground) requires a release before the
  // next contact rearms, so nothing started here completes elsewhere. Pending
  // accepted mutations live in AppState and survive.
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
  // The controls a screen's widgets stand for. Now Playing owns all of them;
  // Playground exercises the shared model with its own seek and play/pause
  // widgets only; Grouping owns none.
  bool owns(WaveshareControl control) const {
    return active_ == SurfaceScreen::NowPlaying ||
           (active_ == SurfaceScreen::Playground &&
            (control == WaveshareControl::Seek || control == WaveshareControl::Play));
  }
  // Widget reports from LVGL's hit-testing. They can only come from the
  // active screen's widgets; the shell still drops a control that screen does
  // not own so no hidden screen ever reacts.
  void pressed(WaveshareControl control) {
    if (owns(control))
      nowPlaying.pressed(control);
  }
  void pressLost() {
    if (active_ != SurfaceScreen::Grouping)
      nowPlaying.pressLost();
  }
  void slid(WaveshareControl control, uint32_t value, uint32_t maximum) {
    if (owns(control))
      nowPlaying.slid(control, value, maximum);
  }
  // Every touch sample reaches only the active screen. Now Playing and
  // Playground drive the shared interaction model; Grouping owns no actions.
  BoardEvent touch(int x, int y, int fingers, uint32_t now) {
    if (active_ == SurfaceScreen::Grouping)
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
