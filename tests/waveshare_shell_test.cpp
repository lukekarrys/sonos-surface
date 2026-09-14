#include "WaveshareShell.h"
#include "WaveshareState.h"
#include "DevicePower.h"
#include <cassert>
#include <iostream>
#include <string>
using namespace surface;
using namespace surface::device;
namespace {
AppState observation() {
  AppState s;
  auto& o = s.observed;
  o.targetId = "room-a";
  o.room = "Office";
  o.known = true;
  o.stale = false;
  o.transport = PlaybackStatus::Paused;
  o.source = PlaybackSource::Queue;
  o.title = "Track A";
  o.trackUri = "track-a";
  o.volume = 20;
  o.seekable = true;
  o.positionMs = 10000;
  o.durationMs = 120000;
  o.queueBacked = true;
  o.queueRevision = 7;
  return s;
}
BoardContext context() {
  BoardContext c;
  c.online = true;
  c.rooms = {{"room-a", "Office", "1", "", "", true, "office"},
             {"room-b", "Bedroom", "2", "", "", true, "bedroom"}};
  return c;
}
WaveshareShell makeShell() {
  WaveshareShell shell;
  shell.update(observation(), context(), 1);
  shell.touch(0, 0, 0, 2);
  return shell;
}
BoardEvent tap(WaveshareShell& shell, WaveshareControl control, uint32_t now) {
  shell.pressed(control);
  assert(shell.touch(184, 286, 1, now).input == Input::None);
  return shell.touch(0, 0, 0, now + 30);
}
// Feed one raw BOOT press of `holdMs` starting at `t`, sampled every 5 ms
// through the shell, driving DevicePower exactly as the runtime loop does.
// Returns how many times the shell navigated.
unsigned press(WaveshareShell& shell, DevicePower& power, uint32_t& t, uint32_t holdMs,
               uint32_t settleMs = 200) {
  unsigned navigations = 0;
  const auto start = t;
  for (; t < start + holdMs + settleMs; t += 5) {
    const bool low = t < start + holdMs;
    LocalActivity activity = LocalActivity::None;
    navigations += shell.boot(low, t, activity);
    assert(activity == (low ? LocalActivity::Button : LocalActivity::None));
    power.poll(t, activity);
    if (low)
      assert(power.inactivityMs(t) == 0);
  }
  return navigations;
}
std::string name(const WaveshareShell& shell) { return surfaceScreenName(shell.active()); }
} // namespace
int main() {
  {
    // Fresh boot starts on Now Playing; next cycles all three screens and wraps.
    auto shell = makeShell();
    assert(shell.active() == SurfaceScreen::NowPlaying && name(shell) == "now");
    assert(shell.next() == SurfaceScreen::Grouping && name(shell) == "grouping");
    assert(shell.next() == SurfaceScreen::Playground && name(shell) == "playground");
    assert(shell.next() == SurfaceScreen::NowPlaying && name(shell) == "now");
    assert(nextSurfaceScreen(SurfaceScreen::Playground) == SurfaceScreen::NowPlaying);
  }
  {
    // The press that booted the device (BOOT low from the first sample) is
    // consumed until a release; its level still counts as local activity.
    WaveshareShell shell;
    shell.update(observation(), context(), 0);
    DevicePower power(0, 300);
    uint32_t t = 0;
    for (; t < 400; t += 5) {
      LocalActivity activity = LocalActivity::None;
      assert(!shell.boot(true, t, activity) && activity == LocalActivity::Button);
      power.poll(t, activity);
    }
    for (; t < 500; t += 5) {
      LocalActivity activity = LocalActivity::None;
      assert(!shell.boot(false, t, activity) && activity == LocalActivity::None);
      power.poll(t, activity);
    }
    assert(shell.active() == SurfaceScreen::NowPlaying && power.inactivityMs(t) == t - 395);
    // A short press navigates once, on release, and resets inactivity while low.
    assert(press(shell, power, t, 100) == 1 && shell.active() == SurfaceScreen::Grouping);
    assert(power.inactivityMs(t) == 205); // Since the last low sample.
    // A hold longer than one second is activity but performs no navigation.
    assert(press(shell, power, t, 1500) == 0 && shell.active() == SurfaceScreen::Grouping);
    assert(press(shell, power, t, BootButton::holdLimitMs) == 1 &&
           shell.active() == SurfaceScreen::Playground);
    // ui-nav next is the same transition without any activity channel.
    const auto inactivity = power.inactivityMs(t);
    assert(shell.next() == SurfaceScreen::NowPlaying);
    power.poll(t, LocalActivity::None);
    assert(power.inactivityMs(t) == inactivity && !power.sleepRequested());
  }
  {
    // Changing screens mutates nothing: no event, no observed change, no
    // preview, and the model still reconciles accepted work while away.
    auto shell = makeShell();
    auto event = tap(shell, WaveshareControl::Play, 100);
    assert(event.input == Input::Intent && event.intent.transport == TransportCommand::Play);
    const auto before = shell.nowPlaying.state;
    shell.next();
    shell.next();
    assert(shell.nowPlaying.state.observed.targetId == before.observed.targetId &&
           shell.nowPlaying.state.observed.transport == before.observed.transport &&
           shell.nowPlaying.state.status == before.status &&
           shell.nowPlaying.state.requestId == before.requestId);
    assert(!shell.nowPlaying.volumePreview && !shell.nowPlaying.seekPreview);
    auto s = observation();
    s.requestId = 1;
    s.status = "succeeded";
    s.observed.transport = PlaybackStatus::Playing;
    shell.update(s, context(), 300);
    assert(shell.active() == SurfaceScreen::Playground);
    assert(shell.nowPlaying.toast == "READ ONLY - no change sent" &&
           shell.nowPlaying.state.observed.transport == PlaybackStatus::Playing);
  }
  {
    // Touch reaches only the active screen: a press on Grouping or Playground
    // never starts a Now Playing gesture, and Now Playing works again after
    // cycling back.
    auto shell = makeShell();
    shell.next();
    shell.pressed(WaveshareControl::Play);
    assert(shell.touch(184, 286, 1, 100).input == Input::None);
    assert(!shell.nowPlaying.contactActive());
    assert(shell.touch(0, 0, 0, 130).input == Input::None);
    shell.next();
    shell.pressed(WaveshareControl::Volume);
    shell.slid(WaveshareControl::Volume, 90, 100);
    assert(shell.touch(184, 351, 1, 200).input == Input::None && !shell.nowPlaying.volumePreview);
    assert(shell.touch(0, 0, 0, 230).input == Input::None);
    shell.next();
    assert(shell.touch(0, 0, 0, 260).input == Input::None);
    assert(tap(shell, WaveshareControl::Play, 300).intent.transport == TransportCommand::Play);
  }
  {
    // Leaving Now Playing cancels its gesture: a volume preview active at
    // navigation submits nothing, and the eventual release lands nowhere.
    auto shell = makeShell();
    shell.pressed(WaveshareControl::Volume);
    shell.slid(WaveshareControl::Volume, 80, 100);
    assert(shell.touch(263, 351, 1, 100).input == Input::None);
    assert(shell.nowPlaying.volumePreview == 80);
    assert(shell.next() == SurfaceScreen::Grouping);
    assert(!shell.nowPlaying.volumePreview && !shell.nowPlaying.contactActive() &&
           shell.nowPlaying.releaseRequired());
    assert(shell.touch(263, 351, 1, 130).input == Input::None);
    assert(shell.touch(0, 0, 0, 160).input == Input::None);
    assert(shell.nowPlaying.state.observed.volume == 20);
    // Back on Now Playing, the first release sample rearms and a tap works.
    shell.next();
    shell.next();
    assert(shell.touch(0, 0, 0, 200).input == Input::None && !shell.nowPlaying.releaseRequired());
    assert(tap(shell, WaveshareControl::Play, 300).intent.transport == TransportCommand::Play);
    // The same for a seek preview and for a plain button press.
    shell.pressed(WaveshareControl::Seek);
    shell.slid(WaveshareControl::Seek, 500, 1000);
    assert(shell.touch(184, 220, 1, 400).input == Input::None);
    assert(shell.nowPlaying.seekPreview == 60000);
    shell.next();
    assert(!shell.nowPlaying.seekPreview && shell.touch(0, 0, 0, 430).input == Input::None);
    shell.next();
    shell.next();
    shell.touch(0, 0, 0, 460);
    shell.pressed(WaveshareControl::Play);
    assert(shell.touch(184, 286, 1, 500).input == Input::None);
    shell.next();
    assert(shell.touch(0, 0, 0, 530).input == Input::None);
    shell.next();
    shell.next();
    assert(shell.touch(0, 0, 0, 560).input == Input::None);
    assert(shell.touch(0, 0, 0, 590).input == Input::None);
    // Navigating with no gesture in progress requires no release.
    shell.next();
    assert(!shell.nowPlaying.releaseRequired());
  }
  {
    // Rooms and Queue are sub-views: the cycle discards them and a later
    // return shows Now Playing itself.
    auto shell = makeShell();
    assert(tap(shell, WaveshareControl::RoomHeader, 100).input == Input::None);
    assert(shell.nowPlaying.screen == WaveshareScreen::Rooms);
    shell.next();
    assert(shell.active() == SurfaceScreen::Grouping &&
           shell.nowPlaying.screen == WaveshareScreen::NowPlaying);
    shell.next();
    shell.next();
    assert(shell.active() == SurfaceScreen::NowPlaying &&
           shell.nowPlaying.screen == WaveshareScreen::NowPlaying);
    assert(tap(shell, WaveshareControl::Queue, 200).input == Input::QueuePage);
    assert(shell.nowPlaying.screen == WaveshareScreen::Queue);
    shell.next();
    assert(shell.nowPlaying.screen == WaveshareScreen::NowPlaying);
  }
  {
    // A room change keeps the top-level screen and resets the sub-view.
    auto shell = makeShell();
    shell.next();
    shell.next();
    auto s = observation();
    assert(selectObservedRoom(s, context().rooms[1]));
    shell.update(s, context(), 200);
    assert(shell.active() == SurfaceScreen::Playground &&
           shell.nowPlaying.screen == WaveshareScreen::NowPlaying);
    shell = makeShell();
    tap(shell, WaveshareControl::RoomHeader, 100);
    assert(shell.nowPlaying.screen == WaveshareScreen::Rooms);
    shell.update(s, context(), 200);
    assert(shell.active() == SurfaceScreen::NowPlaying &&
           shell.nowPlaying.screen == WaveshareScreen::NowPlaying);
  }
  {
    // ui-screen jumps directly; a sub-view belongs to Now Playing only, and
    // ui-state names both levels.
    auto shell = makeShell();
    shell.show(SurfaceScreen::NowPlaying, WaveshareScreen::Queue);
    auto json = waveshareStateJson(shell, true, 0);
    assert(json["screen"] == "now" && json["view"] == "queue");
    shell.show(SurfaceScreen::Playground, WaveshareScreen::Queue);
    json = waveshareStateJson(shell, true, 0);
    assert(json["screen"] == "playground" && json["view"] == "now");
    shell.show(SurfaceScreen::Grouping);
    assert(waveshareStateJson(shell, false, 0)["screen"] == "grouping");
    shell.show(SurfaceScreen::NowPlaying, WaveshareScreen::Rooms);
    json = waveshareStateJson(shell, false, 0);
    assert(json["screen"] == "now" && json["view"] == "rooms");
  }
  std::cout << "Waveshare shell checks passed: default and cycling screens, BOOT activity and "
               "consumption, no Sonos mutation on navigation, screen-local touch, cancelled "
               "previews, sub-view reset, ui-screen and ui-state names\n";
}
