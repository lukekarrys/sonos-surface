#pragma once
#include "WaveshareUi.h"
#include <optional>
#include <surface_json.hpp>

namespace surface::device {
// Last rendered frame cost, reported by ui-state for host-side verification.
struct WaveshareFrame {
  uint32_t draw = 0, flush = 0, total = 0, pollGapMax = 0;
};
inline const char* waveshareScreenName(WaveshareScreen screen) {
  switch (screen) {
  case WaveshareScreen::Rooms:
    return "rooms";
  case WaveshareScreen::Queue:
    return "queue";
  default:
    return "now";
  }
}
inline const char* waveshareControlName(WaveshareControl control) {
  switch (control) {
  case WaveshareControl::RoomHeader:
    return "room-header";
  case WaveshareControl::Previous:
    return "previous";
  case WaveshareControl::Play:
    return "play";
  case WaveshareControl::Next:
    return "next";
  case WaveshareControl::Volume:
    return "volume";
  case WaveshareControl::Seek:
    return "seek";
  case WaveshareControl::Shuffle:
    return "shuffle";
  case WaveshareControl::Repeat:
    return "repeat";
  case WaveshareControl::Queue:
    return "queue";
  case WaveshareControl::Back:
    return "back";
  case WaveshareControl::Reload:
    return "reload";
  case WaveshareControl::PageBack:
    return "page-back";
  case WaveshareControl::PageNext:
    return "page-next";
  case WaveshareControl::Row0:
    return "row0";
  case WaveshareControl::Row1:
    return "row1";
  case WaveshareControl::Row2:
    return "row2";
  case WaveshareControl::Row3:
    return "row3";
  default:
    return "none";
  }
}
template <typename T> nlohmann::json optionalJson(const std::optional<T>& value) {
  return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
}
// Built field by field on purpose: the main loop's 8 KiB stack cannot hold a
// large nested initializer for a USB command handler (docs/hardware.md).
inline nlohmann::json waveshareStateJson(const WaveshareUi& ui, const WaveshareFrame& frame,
                                         bool held, unsigned injectPending,
                                         bool injectOpen = false) {
  const auto& observed = ui.state.observed;
  nlohmann::json state = nlohmann::json::object();
  state["screen"] = waveshareScreenName(ui.screen);
  state["queueStart"] = ui.queueStart;
  state["roomStart"] = ui.roomStart;
  state["rooms"] = ui.context.rooms.size();
  auto& touch = state["touch"];
  touch["touching"] = ui.contactActive();
  touch["control"] = waveshareControlName(ui.activeControl());
  // held and cancelled both require a release before a contact rearms.
  touch["held"] = held;
  touch["cancelled"] = ui.releaseRequired();
  touch["injectPending"] = injectPending;
  // An open injected gesture holds the screen between samples.
  touch["injectOpen"] = injectOpen;
  auto& preview = state["preview"];
  preview["volume"] = optionalJson(ui.volumePreview);
  preview["seek"] = optionalJson(ui.seekPreview);
  state["toast"] = ui.toast;
  state["busy"] = ui.context.busy;
  state["online"] = ui.context.online;
  state["readOnly"] = ui.context.readOnly;
  auto& playback = state["observed"];
  playback["room"] = observed.room;
  playback["targetId"] = observed.targetId;
  playback["known"] = observed.known;
  playback["stale"] = observed.stale;
  playback["transport"] = playbackLabel(observed.transport);
  playback["title"] = observed.title;
  playback["volume"] = optionalJson(observed.volume);
  playback["positionMs"] = optionalJson(observed.positionMs);
  playback["durationMs"] = optionalJson(observed.durationMs);
  playback["seekable"] = ui.canSeek();
  playback["queueIndex"] = optionalJson(observed.queueIndex);
  playback["queueTotal"] = optionalJson(observed.queueTotal);
  playback["queueRevision"] = optionalJson(observed.queueRevision);
  auto& timing = state["frame"];
  timing["draw"] = frame.draw;
  timing["flush"] = frame.flush;
  timing["total"] = frame.total;
  timing["pollGapMax"] = frame.pollGapMax;
  return state;
}
} // namespace surface::device
