#include "WaveshareUi.h"
#include "WaveshareState.h"
#include <cassert>
#include <iostream>
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
  o.queueIndex = 5;
  o.queueTotal = 11;
  o.queueRevision = 7;
  o.shuffle = false;
  o.repeat = Repeat::Off;
  return s;
}
BoardContext context() {
  BoardContext c;
  c.online = true;
  c.rooms = {{"room-a", "Office", "1", "", "", true, "office"},
             {"room-b", "Bedroom", "2", "", "", true, "bedroom"}};
  return c;
}
// LVGL reports the control under the press; the sample stream then starts
// the gesture and the release sample submits it.
BoardEvent tap(WaveshareUi& ui, WaveshareControl control, uint32_t now = 100) {
  ui.pressed(control);
  assert(ui.touch(184, 286, 1, now).input == Input::None);
  return ui.touch(0, 0, 0, now + 30);
}
// A slider gesture: press, one knob position, release.
BoardEvent slide(WaveshareUi& ui, WaveshareControl control, uint32_t value, uint32_t maximum,
                 uint32_t now = 100) {
  ui.pressed(control);
  ui.slid(control, value, maximum);
  assert(ui.touch(184, 351, 1, now).input == Input::None);
  return ui.touch(0, 0, 0, now + 30);
}
WaveshareUi makeUi() {
  WaveshareUi ui;
  ui.update(observation(), context(), 1);
  ui.touch(0, 0, 0, 2);
  return ui;
}
QueuePage page(uint32_t start = 4) {
  QueuePage p;
  p.targetId = "room-a";
  p.start = start;
  p.total = 11;
  p.revision = 7;
  p.observedAtMs = 10;
  for (unsigned i = 0; i < 4; ++i) {
    QueueItem item;
    item.index = start + i;
    item.title = "Track " + std::to_string(item.index);
    p.items.push_back(item);
  }
  return p;
}
bool inside(const UiRect& r) {
  using namespace waveshareLayout;
  return r.x >= reachable.x && r.y >= reachable.y && r.x + r.w <= reachable.x + reachable.w &&
         r.y + r.h <= reachable.y + reachable.h;
}
} // namespace
int main() {
  {
    // The accepted layout stays inside the calibrated reachable area and the
    // corner mask; transport and mode targets keep their sizes.
    using namespace waveshareLayout;
    for (const auto& r : {header, progress, previous, play, next, volume, shuffle, repeat, queue,
                          back, reload, pageBack, pageNext, row(0), row(1), row(2), row(3)})
      assert(inside(r));
    for (const auto& r : {previous, play, next, shuffle, repeat, queue})
      assert(r.h >= 40 && r.w >= 40);
    assert(sliderLeft >= reachable.x && sliderRight <= reachable.x + reachable.w);
    assert(row(3).y + row(3).h <= pageBack.y);
  }
  auto ui = makeUi();
  assert(tap(ui, WaveshareControl::Play).intent.transport == TransportCommand::Play);
  auto s = observation();
  s.observed.transport = PlaybackStatus::Playing;
  ui.update(s, context(), 200);
  assert(tap(ui, WaveshareControl::Play).intent.transport == TransportCommand::Pause);
  assert(tap(ui, WaveshareControl::Previous).intent.transport == TransportCommand::Previous);
  assert(tap(ui, WaveshareControl::Next).intent.transport == TransportCommand::Next);
  assert(tap(ui, WaveshareControl::None).input == Input::None); // A gap between buttons.
  // Crossing from one button into another during a press submits nothing.
  ui.pressed(WaveshareControl::Play);
  ui.touch(184, 286, 1, 250);
  ui.pressed(WaveshareControl::Next);
  ui.touch(260, 286, 1, 260);
  assert(ui.touch(0, 0, 0, 270).input == Input::None);
  // Sliding off a button and lifting submits nothing.
  ui.pressed(WaveshareControl::Play);
  ui.touch(184, 286, 1, 280);
  ui.pressLost();
  assert(ui.touch(0, 0, 0, 290).input == Input::None);
  ui = makeUi();
  // The knob position is the preview; observation is untouched until release
  // submits one absolute value.
  ui.pressed(WaveshareControl::Volume);
  for (uint32_t v = 0; v <= 100; ++v) {
    ui.slid(WaveshareControl::Volume, v, 100);
    assert(ui.touch(52 + int(v * 264 / 100), 351, 1, 300 + v).input == Input::None);
  }
  assert(ui.interaction.volume == 100 && ui.state.observed.volume == 20);
  auto e = ui.touch(0, 0, 0, 700);
  assert(e.input == Input::Intent && e.targetId == "room-a" && e.intent.volume->value == 100 &&
         !e.intent.volume->relative);
  assert(!ui.interaction.volume && ui.state.observed.volume == 20);
  assert(ui.touch(0, 0, 0, 701).input == Input::None);
  assert(slide(ui, WaveshareControl::Volume, 0, 100).intent.volume->value == 0);
  assert(ui.toast.find("READ ONLY") == 0 && ui.state.status == "idle");
  // A knob reported before the first sample of its gesture still previews.
  ui.pressed(WaveshareControl::Seek);
  ui.slid(WaveshareControl::Seek, 0, waveshareLayout::seekResolution);
  assert(ui.touch(52, 220, 1, 800).input == Input::None);
  assert(ui.interaction.positionMs == 0);
  ui.slid(WaveshareControl::Seek, 500, waveshareLayout::seekResolution);
  assert(ui.touch(184, 220, 1, 850).input == Input::None);
  e = ui.touch(0, 0, 0, 900);
  assert(e.intent.seekPositionMs == 60000 && !e.intent.transport && !ui.interaction.positionMs);
  assert(e.trackIdentity == "track-a" && e.queueRevision == 7 && validateIntent(e.intent).ok);
  assert(ui.state.observed.positionMs == 10000);
  // A knob report for a control the finger is not on changes nothing.
  ui.pressed(WaveshareControl::Play);
  ui.touch(184, 286, 1, 910);
  ui.slid(WaveshareControl::Volume, 90, 100);
  assert(!ui.interaction.volume);
  assert(ui.touch(0, 0, 0, 920).intent.transport == TransportCommand::Play);
  s = observation();
  s.observed.source = PlaybackSource::Live;
  s.observed.seekable = false;
  s.observed.queueBacked = false;
  s.observed.durationMs.reset();
  s.observed.positionMs.reset();
  ui.update(s, context(), 1000);
  assert(!ui.canSeek());
  assert(slide(ui, WaveshareControl::Seek, 500, 1000).input == Input::None &&
         !ui.interaction.positionMs);
  assert(tap(ui, WaveshareControl::Shuffle).input == Input::None); // No mode changes for live.
  ui = makeUi();
  assert(tap(ui, WaveshareControl::Shuffle).intent.shuffle == true);
  assert(tap(ui, WaveshareControl::Repeat).intent.repeat == Repeat::All);
  s = observation();
  s.observed.shuffle = true;
  s.observed.repeat = Repeat::All;
  ui.update(s, context(), 1100);
  assert(tap(ui, WaveshareControl::Shuffle).intent.shuffle == false);
  assert(tap(ui, WaveshareControl::Repeat).intent.repeat == Repeat::One);
  s.observed.repeat = Repeat::One;
  ui.update(s, context(), 1200);
  assert(tap(ui, WaveshareControl::Repeat).intent.repeat == Repeat::Off);
  s.observed.repeat.reset();
  s.observed.shuffle.reset();
  ui.update(s, context(), 1201);
  assert(tap(ui, WaveshareControl::Shuffle).input == Input::None &&
         tap(ui, WaveshareControl::Repeat).input == Input::None);
  ui = makeUi();
  assert(tap(ui, WaveshareControl::RoomHeader).input == Input::None &&
         ui.screen == WaveshareScreen::Rooms);
  e = tap(ui, WaveshareControl::Row1);
  assert(e.input == Input::RoomSelect && e.text == "bedroom" && !e.intent.transport);
  assert(ui.screen == WaveshareScreen::NowPlaying);
  ui = makeUi();
  e = tap(ui, WaveshareControl::Queue);
  assert(ui.screen == WaveshareScreen::Queue && e.input == Input::QueuePage && e.start == 4 &&
         e.count == 4);
  assert(ui.requestQueue().input == Input::None); // No duplicate request while waiting.
  auto c = context();
  c.busy = true;
  ui.update(observation(), c, 1300);
  s = observation();
  s.queue = page();
  ui.update(s, context(), 1400);
  e = tap(ui, WaveshareControl::Row1);
  assert(e.intent.queueIndex == 5 && !e.intent.transport);
  e = tap(ui, WaveshareControl::PageNext);
  assert(e.input == Input::QueuePage && e.start == 8 && e.count == 4);
  s.queue = page(8);
  s.queue->items.resize(3);
  ui.update(s, context(), 1500);
  assert(tap(ui, WaveshareControl::Row3).input == Input::None);     // Empty last row.
  assert(tap(ui, WaveshareControl::PageNext).input == Input::None); // Last page.
  e = tap(ui, WaveshareControl::PageBack);
  assert(e.input == Input::QueuePage && e.start == 4);
  s.queue = page();
  s.observed.queueBacked = false;
  ui.update(s, context(), 1600);
  assert(tap(ui, WaveshareControl::Row0).input == Input::None &&
         ui.toast.find("Stored queue") == 0);
  s.observed.queueBacked = true;
  s.observed.queueRevision = 8;
  ui.update(s, context(), 1700);
  assert(tap(ui, WaveshareControl::Row0).input == Input::None); // Revision mismatch.
  s.queue.reset();
  s.queueError = "Network failed";
  ui.update(s, context(), 1800);
  assert(ui.requestQueue().input == Input::None);
  assert(tap(ui, WaveshareControl::Reload).input == Input::QueuePage); // Explicit retry.
  // Empty/shrunken queues repair the visible offset and request only one page.
  ui = makeUi();
  tap(ui, WaveshareControl::Queue);
  s = observation();
  s.queue = page();
  s.queue->total = 0;
  s.queue->items.clear();
  ui.update(s, context(), 1801);
  assert(ui.queueStart == 0);
  e = ui.requestQueue();
  assert(e.input == Input::QueuePage && e.start == 0 && e.count == 4);
  assert(ui.requestQueue().input == Input::None);
  // A room switch uses shared projection and cancels a held gesture/page immediately.
  ui = makeUi();
  tap(ui, WaveshareControl::Queue);
  s = observation();
  s.queue = page();
  ui.update(s, context(), 1900);
  ui.pressed(WaveshareControl::Row1);
  assert(ui.touch(184, 196, 1, 2000).input == Input::None);
  assert(selectObservedRoom(s, context().rooms[1]));
  ui.update(s, context(), 2010);
  assert(ui.screen == WaveshareScreen::NowPlaying && !ui.state.queue &&
         ui.state.observed.title.empty() && ui.queueStart == 0);
  assert(ui.touch(0, 0, 0, 2020).input == Input::None);
  // External track/source changes cancel seek instead of targeting the replacement.
  ui = makeUi();
  ui.pressed(WaveshareControl::Seek);
  ui.slid(WaveshareControl::Seek, 500, 1000);
  ui.touch(184, 220, 1, 2100);
  assert(ui.interaction.positionMs == 60000);
  s = observation();
  s.observed.trackUri = "track-b";
  s.observed.title = "New track";
  ui.update(s, context(), 2110);
  assert(!ui.interaction.positionMs && ui.state.observed.title == "New track");
  assert(ui.touch(0, 0, 0, 2120).input == Input::None);
  // Cancelled drags, multi-touch, peripheral errors and calibration resets cannot submit.
  ui = makeUi();
  ui.pressed(WaveshareControl::Volume);
  ui.slid(WaveshareControl::Volume, 50, 100);
  ui.touch(184, 351, 1, 2200);
  ui.touch(190, 351, 2, 2210);
  assert(!ui.interaction.volume && ui.touch(0, 0, 0, 2220).input == Input::None);
  ui.pressed(WaveshareControl::Volume);
  ui.slid(WaveshareControl::Volume, 50, 100);
  ui.touch(184, 351, 1, 2300);
  ui.cancelTouch();
  assert(ui.touch(0, 0, 0, 2310).input == Input::None);
  // A press reported while a release is still required starts no gesture.
  ui.cancelTouch();
  ui.pressed(WaveshareControl::Play);
  assert(ui.touch(184, 286, 1, 2320).input == Input::None);
  assert(ui.touch(0, 0, 0, 2330).input == Input::None && !ui.releaseRequired());
  assert(ui.touch(184, 286, 1, 2340).input == Input::None); // No control under this press.
  assert(ui.touch(0, 0, 0, 2350).input == Input::None);
  ui.pressed(WaveshareControl::Play);
  ui.touch(184, 286, 1, 2400);
  ui.touch(280, 286, 1, 2410);
  assert(ui.touch(0, 0, 0, 2420).input == Input::None); // Moved more than 16 px.
  ui.pressed(WaveshareControl::Volume);
  ui.slid(WaveshareControl::Volume, 30, 100);
  ui.touch(130, 351, 1, 2430);
  ui.touch(130, 400, 1, 2440); // A slider drag may wander; the knob still counts.
  ui.slid(WaveshareControl::Volume, 31, 100);
  assert(ui.touch(0, 0, 0, 2450).intent.volume->value == 31);
  c = context();
  c.busy = true;
  ui.update(observation(), c, 2500);
  assert(tap(ui, WaveshareControl::Play).input == Input::None && ui.toast == "Busy - try again");
  // An automatic read in progress is not busy: the observation stays fresh.
  c.busy = false;
  c.backgroundActive = true;
  ui.update(observation(), c, 2550);
  assert(ui.fresh() && tap(ui, WaveshareControl::Play).input == Input::Intent &&
         ui.toast != "Busy - try again" && ui.toast != "Refresh room first");
  c.backgroundActive = false;
  c.online = false;
  ui.update(observation(), c, 2600);
  assert(slide(ui, WaveshareControl::Volume, 50, 100).input == Input::None);
  s = observation();
  s.observed.stale = true;
  ui.update(s, context(), 2700);
  assert(tap(ui, WaveshareControl::Play).input == Input::None);
  s.observed.stale = false;
  s.recoveryRequired = true;
  ui.update(s, context(), 2800);
  assert(tap(ui, WaveshareControl::Play).input == Input::None);
  // Failed/read-only completion restores observed values; a later refresh cannot replay its toast.
  ui = makeUi();
  slide(ui, WaveshareControl::Volume, 50, 100);
  s = observation();
  s.requestId = 1;
  s.status = "failed";
  s.detail = "READ_ONLY_BLOCKED";
  ui.update(s, context(), 3000);
  assert(ui.toast == "READ ONLY - command not sent" && !ui.interaction.volume &&
         ui.state.observed.volume == 20);
  ui.update(s, context(), 7000);
  assert(ui.toast.empty());
  ui.update(s, context(), 8000);
  assert(ui.toast.empty());
  // UINT32 duration is safe during proportional preview.
  ui = makeUi();
  s = observation();
  s.observed.durationMs = UINT32_MAX;
  ui.update(s, context(), 9000);
  assert(slide(ui, WaveshareControl::Seek, 1000, 1000).intent.seekPositionMs == UINT32_MAX);
  assert(slide(ui, WaveshareControl::Seek, 2000, 1000).intent.seekPositionMs == UINT32_MAX);
  {
    // An observed volume update cannot move the control under an active finger,
    // and the finger's value is what release submits.
    ui = makeUi();
    ui.pressed(WaveshareControl::Volume);
    ui.slid(WaveshareControl::Volume, 70, 100);
    assert(ui.touch(237, 351, 1, 3100).input == Input::None);
    s = observation();
    s.observed.volume = 35;
    ui.update(s, context(), 3110);
    auto view = ui.view(3110);
    assert(ui.contactActive() && ui.interaction.volume == 70 && ui.state.observed.volume == 35 &&
           view.volume.value == 70 && view.volume.authority == FieldAuthority::Interaction);
    // Unrelated fields stay live under the finger.
    s.observed.transport = PlaybackStatus::Playing;
    s.observed.title = "Track B elsewhere";
    ui.update(s, context(), 3120);
    view = ui.view(3120);
    assert(ui.contactActive() && view.transport.value == PlaybackStatus::Playing &&
           ui.state.observed.title == "Track B elsewhere");
    e = ui.touch(0, 0, 0, 3130);
    assert(e.intent.volume->value == 70 && !ui.interaction.active());
    // Seek sends exactly one request on release; ownership then passes to the
    // pending mutation the runtime admits, and the change repaints.
    ui = makeUi();
    e = slide(ui, WaveshareControl::Seek, 500, 1000, 3200);
    assert(e.input == Input::Intent && e.intent.seekPositionMs == 60000);
    assert(ui.touch(0, 0, 0, 3240).input == Input::None &&
           ui.touch(0, 0, 0, 3270).input == Input::None);
    s = observation();
    s.pending = pendingMutation(9, resolvePolicy(e.intent, {"room-a", {}, 1}), s.observed, 3300);
    ui.dirty = false;
    ui.update(s, context(), 3300);
    view = ui.view(3300);
    assert(ui.dirty && view.updating && view.positionMs.value == 60000u &&
           view.positionMs.authority == FieldAuthority::Pending);
    // Now Playing consumes the derived transport: a pending Pause shows paused,
    // and the play control acts on what is visible.
    s = observation();
    s.observed.transport = PlaybackStatus::Playing;
    MusicIntent pause;
    pause.transport = TransportCommand::Pause;
    s.pending = pendingMutation(10, resolvePolicy(pause, {"room-a", {}, 1}), s.observed, 3400);
    ui.update(s, context(), 3400);
    view = ui.view(3400);
    assert(view.transport.value == PlaybackStatus::Paused &&
           view.transport.authority == FieldAuthority::Pending);
    assert(tap(ui, WaveshareControl::Play, 3410).intent.transport == TransportCommand::Play);
    // The terminal outcome clears pending; the observation wins and repaints.
    s.pending = {};
    ui.dirty = false;
    ui.update(s, context(), 3500);
    assert(ui.dirty && ui.view(3500).transport.authority == FieldAuthority::Observed &&
           tap(ui, WaveshareControl::Play, 3510).intent.transport == TransportCommand::Pause);
  }
  // ui-state reports the live model, including what a host injection needs.
  WaveshareShell shell;
  shell.nowPlaying = makeUi();
  shell.nowPlaying.notify("Hello", 5000);
  shell.pressed(WaveshareControl::Volume);
  shell.slid(WaveshareControl::Volume, 50, 100);
  assert(shell.touch(184, 351, 1, 5100).input == Input::None);
  auto json = waveshareStateJson(shell, true, 3, true);
  assert(json["screen"] == "now" && json["view"] == "now" && json["queueStart"] == 0 &&
         json["roomStart"] == 0 && json["rooms"] == 2);
  assert(json["touch"]["touching"] == true && json["touch"]["control"] == "volume" &&
         json["touch"]["held"] == true && json["touch"]["cancelled"] == false &&
         json["touch"]["injectPending"] == 3 && json["touch"]["injectOpen"] == true);
  auto model = json["model"];
  assert(model["volume"]["interaction"] == 50 && model["volume"]["observed"] == 20 &&
         model["volume"]["visible"] == 50 && model["volume"]["authority"] == "interaction" &&
         model["position"]["interaction"].is_null() && model["pendingJobId"] == 0);
  assert(json["toast"] == "Hello" && json["busy"] == false && json["backgroundActive"] == false &&
         json["online"] == true && json["readOnly"] == true);
  assert(json["observed"]["room"] == "Office" && json["observed"]["targetId"] == "room-a" &&
         json["observed"]["known"] == true && json["observed"]["stale"] == false &&
         json["observed"]["transport"] == "Paused" && json["observed"]["title"] == "Track A");
  assert(json["observed"]["volume"] == 20 && json["observed"]["positionMs"] == 10000 &&
         json["observed"]["durationMs"] == 120000 && json["observed"]["seekable"] == true);
  assert(json["observed"]["queueIndex"] == 5 && json["observed"]["queueTotal"] == 11 &&
         json["observed"]["queueRevision"] == 7);
  assert(!json.contains("frame"));
  shell.nowPlaying.cancelTouch();
  json = waveshareStateJson(shell, false, 0);
  assert(json["touch"]["touching"] == false && json["touch"]["control"] == "none" &&
         json["touch"]["held"] == false && json["touch"]["cancelled"] == true &&
         json["touch"]["injectOpen"] == false && json["model"]["volume"]["interaction"].is_null() &&
         json["model"]["volume"]["authority"] == "observed");
  shell.nowPlaying.screen = WaveshareScreen::Queue;
  shell.nowPlaying.queueStart = 4;
  assert(waveshareStateJson(shell, false, 0)["view"] == "queue");
  shell.nowPlaying.screen = WaveshareScreen::Rooms;
  assert(waveshareStateJson(shell, false, 0)["view"] == "rooms");
  s = observation();
  s.observed.volume.reset();
  s.observed.positionMs.reset();
  s.observed.durationMs.reset();
  s.observed.queueRevision.reset();
  shell.update(s, context(), 5200);
  json = waveshareStateJson(shell, false, 0);
  assert(json["observed"]["volume"].is_null() && json["observed"]["positionMs"].is_null() &&
         json["observed"]["durationMs"].is_null() && json["observed"]["queueRevision"].is_null() &&
         json["observed"]["seekable"] == false);
  {
    // ui-state mirrors each layer for transport, position, and volume, with the
    // pending job id, at the monotonic time it was asked.
    WaveshareShell mirror;
    s = observation();
    s.observed.transport = PlaybackStatus::Playing;
    s.observed.positionObservedAtMs = 20000;
    MusicIntent pause;
    pause.transport = TransportCommand::Pause;
    pause.volume = Volume{false, 44};
    s.pending = pendingMutation(12, resolvePolicy(pause, {"room-a", {}, 1}), s.observed, 22000);
    mirror.update(s, context(), 1);
    json = waveshareStateJson(mirror, false, 0, false, 25000)["model"];
    assert(json["now"] == 25000 && json["pendingJobId"] == 12 && json["updating"] == true &&
           json["stale"] == false);
    assert(json["transport"]["observed"] == "Playing" && json["transport"]["pending"] == "Paused" &&
           json["transport"]["interaction"].is_null() && json["transport"]["visible"] == "Paused" &&
           json["transport"]["authority"] == "pending");
    // Playing until the pause was accepted, then held: 10 s + 2 s.
    assert(json["position"]["observed"] == 10000 && json["position"]["observedAtMs"] == 20000 &&
           json["position"]["pending"].is_null() && json["position"]["visible"] == 12000 &&
           json["position"]["authority"] == "observed");
    assert(json["volume"]["observed"] == 20 && json["volume"]["pending"] == 44 &&
           json["volume"]["visible"] == 44 && json["volume"]["authority"] == "pending");
  }
  std::cout
      << "Waveshare UI checks passed: layout bounds, release intents, knob previews, "
         "navigation, bounded pages, stale/cancel/busy/read-only reconciliation, finger-owned "
         "volume, release to pending, derived transport, ui-state layers\n";
}
