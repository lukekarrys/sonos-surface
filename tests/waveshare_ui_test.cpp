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
BoardEvent tap(WaveshareUi& ui, int x, int y, uint32_t now = 100) {
  assert(ui.touch(x, y, 1, now).input == Input::None);
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
} // namespace
int main() {
  auto ui = makeUi();
  assert(tap(ui, 184, 286).intent.transport == TransportCommand::Play);
  auto s = observation();
  s.observed.transport = PlaybackStatus::Playing;
  ui.update(s, context(), 200);
  assert(tap(ui, 184, 286).intent.transport == TransportCommand::Pause);
  assert(tap(ui, 76, 286).intent.transport == TransportCommand::Previous);
  assert(tap(ui, 292, 286).intent.transport == TransportCommand::Next);
  assert(tap(ui, 124, 286).input == Input::None); // Visible transport gap.
  ui = makeUi();
  for (int x = 52; x <= 316; ++x)
    assert(ui.touch(x, 351, 1, 300 + x).input == Input::None);
  assert(ui.volumePreview == 100 && ui.state.observed.volume == 20);
  auto e = ui.touch(0, 0, 0, 700);
  assert(e.input == Input::Intent && e.targetId == "room-a" && e.intent.volume->value == 100 &&
         !e.intent.volume->relative);
  assert(!ui.volumePreview && ui.state.observed.volume == 20);
  assert(ui.touch(0, 0, 0, 701).input == Input::None);
  assert(tap(ui, 52, 351).intent.volume->value == 0);
  assert(ui.toast.find("READ ONLY") == 0 && ui.state.status == "idle");
  assert(ui.touch(52, 220, 1, 800).input == Input::None);
  assert(ui.touch(184, 220, 1, 850).input == Input::None);
  e = ui.touch(0, 0, 0, 900);
  assert(e.intent.seekPositionMs == 60000 && !e.intent.transport && !ui.seekPreview);
  assert(e.trackIdentity == "track-a" && e.queueRevision == 7 && validateIntent(e.intent).ok);
  assert(ui.state.observed.positionMs == 10000);
  s = observation();
  s.observed.source = PlaybackSource::Live;
  s.observed.seekable = false;
  s.observed.queueBacked = false;
  s.observed.durationMs.reset();
  s.observed.positionMs.reset();
  ui.update(s, context(), 1000);
  assert(!ui.canSeek());
  assert(tap(ui, 184, 220).input == Input::None);
  assert(tap(ui, 80, 395).input == Input::None); // No mode changes for live source.
  ui = makeUi();
  assert(tap(ui, 80, 395).intent.shuffle == true);
  assert(tap(ui, 180, 395).intent.repeat == Repeat::All);
  s = observation();
  s.observed.shuffle = true;
  s.observed.repeat = Repeat::All;
  ui.update(s, context(), 1100);
  assert(tap(ui, 80, 395).intent.shuffle == false);
  assert(tap(ui, 180, 395).intent.repeat == Repeat::One);
  s.observed.repeat = Repeat::One;
  ui.update(s, context(), 1200);
  assert(tap(ui, 180, 395).intent.repeat == Repeat::Off);
  s.observed.repeat.reset();
  s.observed.shuffle.reset();
  ui.update(s, context(), 1201);
  assert(tap(ui, 80, 395).input == Input::None && tap(ui, 180, 395).input == Input::None);
  ui = makeUi();
  assert(tap(ui, 160, 48).input == Input::None && ui.screen == WaveshareScreen::Rooms);
  e = tap(ui, 184, 196);
  assert(e.input == Input::RoomSelect && e.text == "bedroom" && !e.intent.transport);
  assert(ui.screen == WaveshareScreen::NowPlaying);
  ui = makeUi();
  e = tap(ui, 286, 395);
  assert(ui.screen == WaveshareScreen::Queue && e.input == Input::QueuePage && e.start == 4 &&
         e.count == 4);
  assert(ui.requestQueue().input == Input::None); // No duplicate request while waiting.
  auto c = context();
  c.busy = true;
  ui.update(observation(), c, 1300);
  s = observation();
  s.queue = page();
  ui.update(s, context(), 1400);
  e = tap(ui, 184, 196);
  assert(e.intent.queueIndex == 5 && !e.intent.transport);
  e = tap(ui, 266, 389);
  assert(e.input == Input::QueuePage && e.start == 8 && e.count == 4);
  s.queue = page(8);
  s.queue->items.resize(3);
  ui.update(s, context(), 1500);
  assert(tap(ui, 184, 316).input == Input::None); // Empty last row.
  assert(tap(ui, 266, 389).input == Input::None); // Last page.
  e = tap(ui, 100, 389);
  assert(e.input == Input::QueuePage && e.start == 4);
  s.queue = page();
  s.observed.queueBacked = false;
  ui.update(s, context(), 1600);
  assert(tap(ui, 184, 136).input == Input::None && ui.toast.find("Stored queue") == 0);
  s.observed.queueBacked = true;
  s.observed.queueRevision = 8;
  ui.update(s, context(), 1700);
  assert(tap(ui, 184, 136).input == Input::None); // Revision mismatch.
  s.queue.reset();
  s.queueError = "Network failed";
  ui.update(s, context(), 1800);
  assert(ui.requestQueue().input == Input::None);
  assert(tap(ui, 275, 48).input == Input::QueuePage); // Explicit retry.
  // Empty/shrunken queues repair the visible offset and request only one page.
  ui = makeUi();
  tap(ui, 286, 395);
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
  tap(ui, 286, 395);
  s = observation();
  s.queue = page();
  ui.update(s, context(), 1900);
  assert(ui.touch(184, 196, 1, 2000).input == Input::None);
  assert(selectObservedRoom(s, context().rooms[1]));
  ui.update(s, context(), 2010);
  assert(ui.screen == WaveshareScreen::NowPlaying && !ui.state.queue &&
         ui.state.observed.title.empty() && ui.queueStart == 0);
  assert(ui.touch(0, 0, 0, 2020).input == Input::None);
  // External track/source changes cancel seek instead of targeting the replacement.
  ui = makeUi();
  ui.touch(184, 220, 1, 2100);
  s = observation();
  s.observed.trackUri = "track-b";
  s.observed.title = "New track";
  ui.update(s, context(), 2110);
  assert(!ui.seekPreview && ui.state.observed.title == "New track");
  assert(ui.touch(0, 0, 0, 2120).input == Input::None);
  // Cancelled drags, multi-touch, peripheral errors and calibration resets cannot submit.
  ui = makeUi();
  ui.touch(184, 351, 1, 2200);
  ui.touch(190, 351, 2, 2210);
  assert(ui.touch(0, 0, 0, 2220).input == Input::None);
  ui.touch(184, 351, 1, 2300);
  ui.cancelTouch();
  assert(ui.touch(0, 0, 0, 2310).input == Input::None);
  ui.touch(184, 286, 1, 2400);
  ui.touch(280, 286, 1, 2410);
  assert(ui.touch(0, 0, 0, 2420).input == Input::None);
  c = context();
  c.busy = true;
  ui.update(observation(), c, 2500);
  assert(tap(ui, 184, 286).input == Input::None);
  c.busy = false;
  c.online = false;
  ui.update(observation(), c, 2600);
  assert(tap(ui, 184, 351).input == Input::None);
  s = observation();
  s.observed.stale = true;
  ui.update(s, context(), 2700);
  assert(tap(ui, 184, 286).input == Input::None);
  s.observed.stale = false;
  s.recoveryRequired = true;
  ui.update(s, context(), 2800);
  assert(tap(ui, 184, 286).input == Input::None);
  // Failed/read-only completion restores observed values; a later refresh cannot replay its toast.
  ui = makeUi();
  tap(ui, 184, 351);
  s = observation();
  s.requestId = 1;
  s.status = "failed";
  s.detail = "READ_ONLY_BLOCKED";
  ui.update(s, context(), 3000);
  assert(ui.toast == "READ ONLY - command not sent" && !ui.volumePreview &&
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
  assert(tap(ui, 316, 220).intent.seekPositionMs == UINT32_MAX);
  // ui-state reports the live model, including what a host injection needs.
  ui = makeUi();
  ui.notify("Hello", 5000);
  assert(ui.touch(184, 351, 1, 5100).input == Input::None);
  auto json = waveshareStateJson(ui, WaveshareFrame{7, 21, 28, 44}, true, 3, true);
  assert(json["screen"] == "now" && json["queueStart"] == 0 && json["roomStart"] == 0 &&
         json["rooms"] == 2);
  assert(json["touch"]["touching"] == true && json["touch"]["control"] == "volume" &&
         json["touch"]["held"] == true && json["touch"]["cancelled"] == false &&
         json["touch"]["injectPending"] == 3 && json["touch"]["injectOpen"] == true);
  assert(json["preview"]["volume"] == 50 && json["preview"]["seek"].is_null());
  assert(json["toast"] == "Hello" && json["busy"] == false && json["online"] == true &&
         json["readOnly"] == true);
  assert(json["observed"]["room"] == "Office" && json["observed"]["targetId"] == "room-a" &&
         json["observed"]["known"] == true && json["observed"]["stale"] == false &&
         json["observed"]["transport"] == "Paused" && json["observed"]["title"] == "Track A");
  assert(json["observed"]["volume"] == 20 && json["observed"]["positionMs"] == 10000 &&
         json["observed"]["durationMs"] == 120000 && json["observed"]["seekable"] == true);
  assert(json["observed"]["queueIndex"] == 5 && json["observed"]["queueTotal"] == 11 &&
         json["observed"]["queueRevision"] == 7);
  assert(json["frame"]["draw"] == 7 && json["frame"]["flush"] == 21 &&
         json["frame"]["total"] == 28 && json["frame"]["pollGapMax"] == 44);
  ui.cancelTouch();
  json = waveshareStateJson(ui, {}, false, 0);
  assert(json["touch"]["touching"] == false && json["touch"]["control"] == "none" &&
         json["touch"]["held"] == false && json["touch"]["cancelled"] == true &&
         json["touch"]["injectOpen"] == false && json["preview"]["volume"].is_null());
  ui.screen = WaveshareScreen::Queue;
  ui.queueStart = 4;
  assert(waveshareStateJson(ui, {}, false, 0)["screen"] == "queue");
  ui.screen = WaveshareScreen::Rooms;
  assert(waveshareStateJson(ui, {}, false, 0)["screen"] == "rooms");
  s = observation();
  s.observed.volume.reset();
  s.observed.positionMs.reset();
  s.observed.durationMs.reset();
  s.observed.queueRevision.reset();
  ui.update(s, context(), 5200);
  json = waveshareStateJson(ui, {}, false, 0);
  assert(json["observed"]["volume"].is_null() && json["observed"]["positionMs"].is_null() &&
         json["observed"]["durationMs"].is_null() && json["observed"]["queueRevision"].is_null() &&
         json["observed"]["seekable"] == false);
  std::cout << "Waveshare UI checks passed: release intents, navigation, bounded pages, "
               "stale/cancel/busy/read-only reconciliation, ui-state shape\n";
}
