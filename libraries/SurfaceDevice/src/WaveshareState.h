#pragma once
#include "WaveshareShell.h"
#include <optional>
#include <surface_json.hpp>

namespace surface::device {
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
inline nlohmann::json transportJson(const std::optional<PlaybackStatus>& value) {
  return value ? nlohmann::json(playbackLabel(*value)) : nlohmann::json(nullptr);
}
// One visible field's four authorities, for asserting precedence on device.
template <typename T, typename Format>
nlohmann::json layersJson(const std::optional<T>& observed, const std::optional<T>& pending,
                          const std::optional<T>& interaction, const VisibleField<T>& visible,
                          Format format) {
  nlohmann::json field = nlohmann::json::object();
  field["observed"] = format(observed);
  field["pending"] = format(pending);
  field["interaction"] = format(interaction);
  field["visible"] = format(visible.value);
  field["authority"] = fieldAuthorityName(visible.authority);
  return field;
}
// The portable part of `ui-state` at monotonic `now`; the LVGL adapter adds
// its sample, frame, and memory fields. Built field by field on purpose: a
// large nested initializer inflates the USB command handler's frame
// (docs/hardware.md).
inline nlohmann::json waveshareStateJson(const WaveshareShell& shell, bool held,
                                         unsigned injectPending, bool injectOpen = false,
                                         uint64_t now = 0) {
  const auto& ui = shell.nowPlaying;
  const auto& observed = ui.state.observed;
  nlohmann::json state = nlohmann::json::object();
  state["screen"] = surfaceScreenName(shell.active());
  state["view"] = waveshareScreenName(ui.screen);
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
  // The state model: observed, pending, interaction, and visible per field.
  const auto view = ui.view(now);
  const auto& pending = ui.state.pending;
  const bool owned = view.updating;
  const std::optional<uint32_t> touchPosition =
      ui.interaction.targetId == observed.targetId ? ui.interaction.positionMs : std::nullopt;
  const std::optional<int> touchVolume =
      ui.interaction.targetId == observed.targetId ? ui.interaction.volume : std::nullopt;
  const auto observedTransport = observed.transport == PlaybackStatus::Unknown
                                     ? std::nullopt
                                     : std::optional<PlaybackStatus>(observed.transport);
  auto& model = state["model"];
  model["now"] = now;
  model["pendingJobId"] = pending.jobId;
  model["updating"] = view.updating;
  model["contentPending"] = view.contentPending;
  model["stale"] = view.stale;
  model["transport"] =
      layersJson(observedTransport, owned ? pending.transport : std::nullopt,
                 std::optional<PlaybackStatus>(), view.transport,
                 [](const std::optional<PlaybackStatus>& value) { return transportJson(value); });
  const auto number = [](const auto& value) { return optionalJson(value); };
  model["position"] = layersJson(observed.positionMs, owned ? pending.positionMs : std::nullopt,
                                 touchPosition, view.positionMs, number);
  model["position"]["observedAtMs"] = observed.positionObservedAtMs;
  model["volume"] = layersJson(observed.volume, owned ? pending.volume : std::nullopt, touchVolume,
                               view.volume, number);
  state["toast"] = ui.toast;
  state["busy"] = ui.context.busy;
  state["backgroundActive"] = ui.context.backgroundActive;
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
  return state;
}
} // namespace surface::device
