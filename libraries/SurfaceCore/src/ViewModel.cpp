#include "ViewModel.h"

#include <algorithm>

namespace surface {
namespace {
template <class T>
VisibleField<T> precedence(const std::optional<T>& interaction, const std::optional<T>& pending,
                           const std::optional<T>& observed) {
  if (interaction)
    return {interaction, FieldAuthority::Interaction};
  if (pending)
    return {pending, FieldAuthority::Pending};
  if (observed)
    return {observed, FieldAuthority::Observed};
  return {};
}
template <class T>
VisibleField<T> precedence(const std::optional<T>& pending, const std::optional<T>& observed) {
  return precedence<T>(std::nullopt, pending, observed);
}
} // namespace

const char* fieldAuthorityName(FieldAuthority authority) {
  switch (authority) {
  case FieldAuthority::Observed:
    return "observed";
  case FieldAuthority::Pending:
    return "pending";
  case FieldAuthority::Interaction:
    return "interaction";
  default:
    return "unknown";
  }
}

ViewModel deriveViewModel(const PlaybackState& observed, const PendingState& pending,
                          const InteractionState& interaction, uint64_t nowMs) {
  ViewModel view;
  view.known = observed.known;
  view.stale = observed.stale;
  view.durationMs = observed.durationMs;
  const bool owned = pending.active() && pending.targetId == observed.targetId;
  const bool touching = interaction.targetId == observed.targetId;
  view.updating = owned;
  view.contentPending = owned && pending.content;
  const auto observedTransport = observed.transport == PlaybackStatus::Unknown
                                     ? std::nullopt
                                     : std::optional<PlaybackStatus>(observed.transport);
  view.transport = precedence(owned ? pending.transport : std::nullopt, observedTransport);
  view.volume = precedence(touching ? interaction.volume : std::nullopt,
                           owned ? pending.volume : std::nullopt, observed.volume);
  view.shuffle = precedence(owned ? pending.shuffle : std::nullopt, observed.shuffle);
  view.repeat = precedence(owned ? pending.repeat : std::nullopt, observed.repeat);

  // Playing time since an anchor, following the visible transport: observed
  // until the pending mutation was accepted, then the pending transport.
  const auto playingSince = [&](uint64_t anchor) -> uint64_t {
    if (!observed.known || observed.stale || nowMs <= anchor)
      return 0;
    const bool transportPending = owned && pending.transport;
    const auto split = transportPending ? std::clamp(pending.acceptedAtMs, anchor, nowMs) : nowMs;
    uint64_t elapsed = observed.transport == PlaybackStatus::Playing ? split - anchor : 0;
    if (transportPending && *pending.transport == PlaybackStatus::Playing)
      elapsed += nowMs - split;
    return std::min(elapsed, projectionHorizonMs);
  };
  const auto clampPosition = [&](uint64_t position) {
    const uint64_t limit =
        observed.durationMs && *observed.durationMs ? *observed.durationMs : UINT32_MAX;
    return uint32_t(std::min(position, limit));
  };
  if (touching && interaction.positionMs)
    view.positionMs = {clampPosition(*interaction.positionMs), FieldAuthority::Interaction};
  else if (owned && pending.positionMs && pending.trackUri == observed.trackUri)
    view.positionMs = {
        clampPosition(uint64_t(*pending.positionMs) + playingSince(pending.acceptedAtMs)),
        FieldAuthority::Pending};
  else if (observed.positionMs)
    view.positionMs = {
        clampPosition(uint64_t(*observed.positionMs) + playingSince(observed.positionObservedAtMs)),
        FieldAuthority::Observed};
  return view;
}
} // namespace surface
