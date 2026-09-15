#pragma once
#include "SurfaceCore.h"

namespace surface {
// Visible playback state derived from three authorities plus monotonic time.
// Per field: active interaction > valid pending mutation > observed > unknown.
// Board-agnostic: no display, SDK, or locking. Callers derive from a snapshot
// taken outside any critical section.
enum class FieldAuthority { Unknown, Observed, Pending, Interaction };
const char* fieldAuthorityName(FieldAuthority authority);
template <class T> struct VisibleField {
  std::optional<T> value;
  FieldAuthority authority = FieldAuthority::Unknown;
};
// The physical interaction currently owning a visible field. The UI layer owns
// the instance and cancels it when its screen, room, or track changes; one for
// another room never owns a field.
struct InteractionState {
  std::string targetId;
  std::optional<uint32_t> positionMs; // Finger on the seek control.
  std::optional<int> volume;          // Finger on the volume control.
  bool active() const { return positionMs || volume; }
};
// Backstop for an observation that is neither refreshed nor marked stale: the
// projection stops advancing this long after its anchor. Longer than one poll
// interval plus the read budget, so ordinary operation never reaches it.
constexpr uint64_t projectionHorizonMs = 60000;
struct ViewModel {
  bool known = false, stale = true;
  bool updating = false;       // An accepted mutation awaits its terminal outcome.
  bool contentPending = false; // Metadata shown is observed; the new content is not yet.
  VisibleField<PlaybackStatus> transport;
  VisibleField<uint32_t> positionMs;
  std::optional<uint32_t> durationMs;
  VisibleField<int> volume;
  VisibleField<bool> shuffle;
  VisibleField<Repeat> repeat;
};
// Deterministic for equal inputs. Playing time advances only while the visible
// transport is Playing and the observation is fresh; stale holds position.
ViewModel deriveViewModel(const PlaybackState& observed, const PendingState& pending,
                          const InteractionState& interaction, uint64_t nowMs);
} // namespace surface
