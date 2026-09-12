#pragma once
#include "WorkerLifecycle.h"
#include "WifiLifecycle.h"
#include "SonosHealth.h"
#include "SurfaceDevice.h"

namespace surface::device {
inline bool runtimeUiMatches(const Room* selected, const PlaybackState& observed,
                             const BoardEvent& event) {
  return selected && selected->eligible && selected->id == event.targetId &&
         (!event.intent.seekPositionMs || (observed.trackUri == event.trackIdentity &&
                                           observed.queueRevision == event.queueRevision)) &&
         (!event.intent.queueIndex || observed.queueRevision == event.queueRevision);
}
inline bool runtimeJobActive(const WorkerSnapshot& worker, uint64_t id, uint64_t now,
                             bool stopping) {
  return !stopping && worker.running && worker.jobId == id && now < worker.deadline;
}
// This is the real adapter's final dispatch fence, shared by fault fixtures.
// Read-only/UUID/allowlist checks remain independently enforced by GuardedHttp.
inline bool runtimeDispatchAllowed(const WorkerSnapshot& worker, const WifiSnapshot& wifi,
                                   const SonosSnapshot& sonos, uint64_t id, uint64_t discoveryId,
                                   uint64_t now, bool stopping, bool mutation) {
  return runtimeJobActive(worker, id, now, stopping) && wifi.networkReady &&
         sonos.networkAvailable && discoveryId != 0 && sonos.discoveryId == discoveryId &&
         (sonos.state == SonosState::Ready ||
          (!mutation && sonos.state == SonosState::Discovering && now < sonos.deadline));
}
} // namespace surface::device
