#include "DevicePower.h"
#include "RoomConfig.h"
#include "WaveshareUi.h"
#include "ArtworkState.h"
#include "ConsoleWrite.h"
#include <cassert>
#include <iostream>
using namespace surface;
using namespace surface::device;

void consoleBackpressure() {
  struct Port {
    int available = 0;
    size_t writes = 0, limit = 1000;
    std::string output;
    int availableForWrite() { return available; }
    size_t write(const uint8_t* data, size_t size) {
      ++writes;
      const size_t count = std::min(size, limit);
      output.append(reinterpret_cast<const char*>(data), count);
      return count;
    }
  } port;
  uint32_t time = 0;
  unsigned yields = 0;
  const auto now = [&] { return time; };
  const auto yield = [&] {
    ++time;
    ++yields;
  };
  // A plugged-in host that never consumes output cannot delay any diagnostic.
  for (unsigned i = 0; i < 1000; ++i)
    assert(!writeConsoleLine(port, "background log\n", 0, now, yield));
  assert(time == 0 && yields == 0 && port.writes == 0);
  port.available = 2;
  assert(!writeConsoleLine(port, "whole line\n", 0, now, yield) && port.output.empty());
  port.available = 100;
  assert(writeConsoleLine(port, "whole line\n", 0, now, yield));
  assert(port.output == "whole line\n" && yields == 0);
  // Explicit replies can wait for a reader, but have a total deadline.
  port.available = 0;
  assert(writeConsoleLine(port, "device-config {}\n", 250, now, [&] {
    yield();
    if (time == 3)
      port.available = 100;
  }));
  assert(time == 3 && port.output == "whole line\ndevice-config {}\n");
  port.available = -1;
  time = UINT32_MAX - 10;
  yields = 0;
  assert(!writeConsoleLine(port, "CONFIG_SAVED\n", 250, now, yield));
  assert(yields == 250); // The deadline also holds across millis rollover.
  port.available = 100;
  port.limit = 2;
  port.writes = 0;
  yields = 0;
  assert(!writeConsoleLine(port, "reply\n", 250, now, yield));
  assert(port.writes == 1 && yields == 0); // No duplicate prefix on a short write.
}

int main() {
  consoleBackpressure();
  using Json = nlohmann::json;
  uint32_t seconds = 99;
  assert(parseSleepTimeout(Json::object(), seconds) && seconds == 300);
  DevicePower defaults(1000);
  assert(!defaults.poll(300999) && defaults.poll(301000));
  assert(defaults.sleepRequested() && !defaults.poll(600000));
  defaults.recordLocalActivity(600001);
  assert(defaults.inactivityMs(600001) == 599001); // Shutdown is irreversible until boot.

  for (const Json& value : {Json(0), Json(15), Json(UINT32_MAX)}) {
    Json document;
    assert(parseConfigDocument(Json{{"sleep_timeout_seconds", value}}.dump(), document));
    assert(parseSleepTimeout(document, seconds) && seconds == value.get<uint32_t>());
  }
  for (const Json& value : {Json(-1), Json(1.5), Json("15"), Json(true), Json(nullptr),
                            Json(uint64_t(UINT32_MAX) + 1)}) {
    Json unchanged = {{"rooms", Json::object()}};
    const auto previous = unchanged;
    assert(!parseConfigDocument(Json{{"sleep_timeout_seconds", value}}.dump(), unchanged));
    assert(unchanged == previous);
  }
  DevicePower disabled(0, 0);
  assert(!disabled.poll(UINT64_C(900000000000)) && !disabled.sleepRequested());
  DevicePower large(0, UINT32_MAX);
  assert(!large.poll(uint64_t(UINT32_MAX) * 1000 - 1));
  assert(large.poll(uint64_t(UINT32_MAX) * 1000));

  for (auto activity : {LocalActivity::Button, LocalActivity::Touch, LocalActivity::Nfc}) {
    DevicePower power(0, 15);
    assert(!power.poll(14999, activity));
    assert(power.inactivityMs(15000) == 1 && !power.poll(15000));
    assert(!power.poll(29998) && power.poll(29999));
    assert(!power.poll(30000, activity) && !power.poll(45000));
    DevicePower boundary(0, 15);
    assert(!boundary.poll(15000, activity) && boundary.poll(30000));
  }

  // Exercise the same UI/observation producers used by the device. Updating
  // observation, rendering, and automatic queue requests carry no physical
  // activity, even if their BoardEvent has a non-None application input.
  for (bool readOnly : {false, true}) {
    DevicePower power(0, 15);
    WaveshareUi ui;
    ArtworkState artwork;
    AppState state;
    BoardContext context;
    context.readOnly = readOnly;
    context.online = true;
    context.rooms = {{"one", "Office", "1", "", "", true, "office"}};
    state.observed.targetId = "one";
    state.observed.known = true;
    state.observed.stale = false;
    for (uint64_t now : {1000, 4000, 7000, 10000, 14999}) {
      if (now == 1000) // Sonos state update / playback progression
        state.observed.positionMs = 4000;
      if (now == 4000) { // External track change
        state.observed.trackUri = "new-track";
        state.observed.artwork = "http://speaker/cover.jpg";
        artwork.select(state.observed);
      }
      if (now == 7000) // Artwork completion
        artwork.complete(artwork.generation, true, now);
      if (now == 10000) // Topology/availability refresh
        context.rooms.front().eligible = false;
      if (now == 14999) // Background room selection
        state.observed.targetId = "two";
      ui.update(state, context, now);
      auto event = ui.requestQueue();
      assert(event.activity == LocalActivity::None);
      assert(!power.poll(now, event.activity));
      assert(power.inactivityMs(now) == now);
    }
    assert(power.poll(15000) && !power.poll(15001));

    // The adapter annotates a local room gesture independently of its action;
    // USB/background room selection has the default None activity.
    DevicePower roomGesture(0, 15);
    BoardEvent localRoom(Input::RoomNext);
    localRoom.activity = LocalActivity::Button;
    assert(!roomGesture.poll(14999, localRoom.activity));
    BoardEvent backgroundRoom(Input::RoomSelect, "office");
    assert(!roomGesture.poll(15000, backgroundRoom.activity));
    assert(roomGesture.poll(29999, backgroundRoom.activity));
  }
  std::cout << "Power checks passed: physical activity, background independence, config, one-shot "
               "sleep\n";
}
