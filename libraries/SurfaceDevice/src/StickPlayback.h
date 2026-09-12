#pragma once
#include "SurfaceDevice.h"

namespace surface::device {
// Stick interaction only: generic Previous remains one native skip.
inline BoardEvent stickPreviousEvent(const AppState& state, const std::string& selectedId,
                                     bool online, uint64_t nowMs) {
  BoardEvent event{Input::Intent};
  event.targetId = selectedId;
  const auto& observed = state.observed;
  const bool fresh = online && observed.known && !observed.stale &&
                     observed.targetId == selectedId && nowMs >= observed.observedAtMs &&
                     nowMs - observed.observedAtMs <= playbackRefreshIntervalMs;
  if (fresh && observed.seekable == true && !observed.trackUri.empty() && observed.positionMs &&
      *observed.positionMs > 3000) {
    event.intent.seekPositionMs = 0;
    event.trackIdentity = observed.trackUri;
    event.queueRevision = observed.queueRevision;
  } else {
    event.intent.transport = TransportCommand::Previous;
  }
  return event;
}
} // namespace surface::device
