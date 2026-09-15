#include <SurfaceCore.h>
#include <ViewModel.h>
#include <cassert>
#include <iostream>

using namespace surface;
namespace {
constexpr uint64_t anchorAt = 1000;
PlaybackState playing() {
  PlaybackState o;
  o.targetId = "RINCON_A";
  o.known = true;
  o.stale = false;
  o.transport = PlaybackStatus::Playing;
  o.title = "Song A";
  o.trackUri = "track:a";
  o.positionMs = 30000;
  o.positionObservedAtMs = anchorAt;
  o.observedAtMs = anchorAt;
  o.durationMs = 180000;
  o.volume = 20;
  o.shuffle = false;
  o.repeat = Repeat::Off;
  return o;
}
MusicIntent command(TransportCommand transport) {
  MusicIntent intent;
  intent.transport = transport;
  return intent;
}
PendingState accept(uint64_t jobId, const MusicIntent& intent, const PlaybackState& observed,
                    uint64_t now) {
  return pendingMutation(jobId, resolvePolicy(intent, {observed.targetId, {}, 1}), observed, now);
}
template <class T>
bool shows(const VisibleField<T>& field, const std::optional<T>& value, FieldAuthority authority) {
  return field.value == value && field.authority == authority;
}
const PendingState nothingPending;
const InteractionState noTouch{"RINCON_A", std::nullopt, std::nullopt};

void playbackClock() {
  auto o = playing();
  // Playing advances from the anchor without mutating the observation.
  auto view = deriveViewModel(o, nothingPending, noTouch, anchorAt + 2500);
  assert(shows<uint32_t>(view.positionMs, 32500, FieldAuthority::Observed));
  assert(o.positionMs == 30000u && view.durationMs == 180000u && view.known && !view.stale);
  assert(deriveViewModel(o, nothingPending, noTouch, anchorAt + 2500).positionMs.value ==
         view.positionMs.value); // Deterministic for equal inputs.
  // Before the anchor (clock sampled earlier than the copy) never goes backward.
  assert(deriveViewModel(o, nothingPending, noTouch, anchorAt - 10).positionMs.value == 30000u);
  // Paused and stopped do not advance.
  for (auto status :
       {PlaybackStatus::Paused, PlaybackStatus::Stopped, PlaybackStatus::Transitioning}) {
    o.transport = status;
    assert(deriveViewModel(o, nothingPending, noTouch, anchorAt + 9000).positionMs.value == 30000u);
  }
  // Duration clamps; unknown duration does not.
  o = playing();
  o.positionMs = 170000;
  assert(deriveViewModel(o, nothingPending, noTouch, anchorAt + 20000).positionMs.value == 180000u);
  o.durationMs.reset();
  assert(deriveViewModel(o, nothingPending, noTouch, anchorAt + 50000).positionMs.value == 220000u);
  // A new anchor replaces the projection instead of adding to it.
  o = playing();
  assert(deriveViewModel(o, nothingPending, noTouch, anchorAt + 8000).positionMs.value == 38000u);
  o.positionMs = 12000; // External seek observed at the next poll.
  o.positionObservedAtMs = anchorAt + 8000;
  assert(deriveViewModel(o, nothingPending, noTouch, anchorAt + 9000).positionMs.value == 13000u);
  // Stale holds the anchor and is shown stale; unknown shows nothing.
  o = playing();
  o.stale = true;
  view = deriveViewModel(o, nothingPending, noTouch, anchorAt + 20000);
  assert(view.stale && view.positionMs.value == 30000u &&
         shows(view.transport, std::optional(PlaybackStatus::Playing), FieldAuthority::Observed));
  PlaybackState unknown;
  unknown.targetId = "RINCON_A";
  view = deriveViewModel(unknown, nothingPending, noTouch, 5000);
  assert(!view.known && view.stale && view.transport.authority == FieldAuthority::Unknown &&
         !view.positionMs.value && view.volume.authority == FieldAuthority::Unknown);
  // An observation that is neither refreshed nor marked stale stops advancing.
  o = playing();
  o.durationMs.reset();
  assert(deriveViewModel(o, nothingPending, noTouch, anchorAt + 10 * projectionHorizonMs)
             .positionMs.value == 30000u + projectionHorizonMs);
}

void seekModel() {
  auto o = playing();
  // Drag owns position; an observed update cannot move it.
  InteractionState drag{"RINCON_A", 90000, std::nullopt};
  assert(shows<uint32_t>(deriveViewModel(o, nothingPending, drag, anchorAt).positionMs, 90000,
                         FieldAuthority::Interaction));
  o.positionMs = 41000;
  o.positionObservedAtMs = anchorAt + 10000;
  o.volume = 33;
  auto view = deriveViewModel(o, nothingPending, drag, anchorAt + 10500);
  assert(shows<uint32_t>(view.positionMs, 90000, FieldAuthority::Interaction) &&
         shows<int>(view.volume, 33, FieldAuthority::Observed));
  drag.positionMs = 999999; // A knob past the end clamps to the duration.
  assert(deriveViewModel(o, nothingPending, drag, anchorAt).positionMs.value == 180000u);
  // Release: one pending seek owns position and keeps playing time.
  MusicIntent seek;
  seek.seekPositionMs = 90000;
  const auto pending = accept(7, seek, o, anchorAt + 11000);
  assert(pending.jobId == 7 && pending.positionMs == 90000u && !pending.transport &&
         !pending.volume && !pending.content && pending.trackUri == "track:a");
  view = deriveViewModel(o, pending, noTouch, anchorAt + 12000);
  assert(shows<uint32_t>(view.positionMs, 91000, FieldAuthority::Pending) && view.updating);
  // Observations during the job do not take the field back.
  o.positionMs = 45000;
  o.positionObservedAtMs = anchorAt + 11500;
  assert(deriveViewModel(o, pending, noTouch, anchorAt + 12000).positionMs.value == 91000u);
  // Confirmation: the terminal outcome clears; observed (the verify read) owns.
  auto cleared = pending;
  assert(!clearPending(cleared, 6) && cleared.jobId == 7);
  assert(clearPending(cleared, 7) && !cleared.active());
  o.positionMs = 90400;
  o.positionObservedAtMs = anchorAt + 11800;
  assert(shows<uint32_t>(deriveViewModel(o, cleared, noTouch, anchorAt + 12000).positionMs, 90600,
                         FieldAuthority::Observed));
  // Failure: cleared the same way, and the observation shows where playback is.
  o.positionMs = 45000;
  o.positionObservedAtMs = anchorAt + 11500;
  assert(shows<uint32_t>(deriveViewModel(o, cleared, noTouch, anchorAt + 12000).positionMs, 45500,
                         FieldAuthority::Observed));
  // A track changed by another controller never shows the old track's seek.
  o.trackUri = "track:b";
  assert(deriveViewModel(o, pending, noTouch, anchorAt + 12000).positionMs.authority ==
         FieldAuthority::Observed);
}

void transportModel() {
  auto o = playing();
  // Optimistic pause freezes position where it was at acceptance.
  const auto pause = accept(3, command(TransportCommand::Pause), o, anchorAt + 4000);
  auto view = deriveViewModel(o, pause, noTouch, anchorAt + 9000);
  assert(shows(view.transport, std::optional(PlaybackStatus::Paused), FieldAuthority::Pending) &&
         shows<uint32_t>(view.positionMs, 34000, FieldAuthority::Observed) && view.updating &&
         !view.contentPending);
  // Confirmation: observed matches, pending clears.
  o.transport = PlaybackStatus::Paused;
  o.positionMs = 34100;
  o.positionObservedAtMs = anchorAt + 5000;
  auto confirmed = pause;
  assert(clearPending(confirmed, 3));
  view = deriveViewModel(o, confirmed, noTouch, anchorAt + 9000);
  assert(shows(view.transport, std::optional(PlaybackStatus::Paused), FieldAuthority::Observed) &&
         view.positionMs.value == 34100u && !view.updating);
  // Contradictory authoritative result: another controller resumed before the
  // verify read. Pending shows the request until its outcome; then Sonos wins.
  o.transport = PlaybackStatus::Playing;
  view = deriveViewModel(o, pause, noTouch, anchorAt + 9000);
  assert(view.transport.value == PlaybackStatus::Paused);
  view = deriveViewModel(o, confirmed, noTouch, anchorAt + 9000);
  assert(shows(view.transport, std::optional(PlaybackStatus::Playing), FieldAuthority::Observed));
  // Optimistic play from paused advances from acceptance, not from the old anchor.
  o = playing();
  o.transport = PlaybackStatus::Paused;
  const auto play = accept(4, command(TransportCommand::Play), o, anchorAt + 60000);
  view = deriveViewModel(o, play, noTouch, anchorAt + 61500);
  assert(shows(view.transport, std::optional(PlaybackStatus::Playing), FieldAuthority::Pending) &&
         view.positionMs.value == 31500u);
  // A late completion for a different job id never clears.
  auto current = play;
  assert(!clearPending(current, 3) && !clearPending(current, 0) && current.jobId == 4);
  // Skips and sources mark content pending without fabricating metadata.
  o = playing();
  auto next = accept(5, command(TransportCommand::Next), o, anchorAt);
  view = deriveViewModel(o, next, noTouch, anchorAt);
  assert(next.content && !next.transport && view.contentPending &&
         view.transport.authority == FieldAuthority::Observed);
  MusicIntent album;
  album.source =
      Source{"https://music.apple.com/us/album/example/12345", "us", "12345", SourceKind::Album};
  album.transport = TransportCommand::Play;
  next = accept(6, album, o, anchorAt);
  assert(next.content && next.transport == PlaybackStatus::Playing);
  // Relative volume has no optimistic value; absolute volume clamps.
  MusicIntent volume;
  volume.volume = Volume{true, 5};
  assert(!accept(8, volume, o, anchorAt).volume);
  volume.volume = Volume{false, 140};
  assert(accept(8, volume, o, anchorAt).volume == 100);
}

void fieldIndependence() {
  auto o = playing();
  // A seek interaction does not freeze volume, transport, or modes.
  InteractionState drag{"RINCON_A", 60000, std::nullopt};
  o.volume = 44;
  o.transport = PlaybackStatus::Paused;
  o.shuffle = true;
  auto view = deriveViewModel(o, nothingPending, drag, anchorAt);
  assert(shows<int>(view.volume, 44, FieldAuthority::Observed) &&
         shows(view.transport, std::optional(PlaybackStatus::Paused), FieldAuthority::Observed) &&
         shows<bool>(view.shuffle, true, FieldAuthority::Observed));
  // A volume interaction does not freeze a track change: timing and transport
  // follow the new observation while the finger keeps volume.
  InteractionState finger{"RINCON_A", std::nullopt, 70};
  o = playing();
  o.title = "Song B";
  o.trackUri = "track:b";
  o.durationMs = 200000;
  o.positionMs = 1000;
  o.volume = 25;
  view = deriveViewModel(o, nothingPending, finger, anchorAt + 1000);
  assert(shows<int>(view.volume, 70, FieldAuthority::Interaction) &&
         shows<uint32_t>(view.positionMs, 2000, FieldAuthority::Observed) &&
         view.durationMs == 200000u);
  // A pending shuffle does not freeze transport, position, or volume, and
  // metadata changing underneath it stays observed.
  MusicIntent shuffle;
  shuffle.shuffle = true;
  const auto pending = accept(9, shuffle, o, anchorAt);
  o.transport = PlaybackStatus::Paused;
  o.title = "Song C";
  o.volume = 12;
  view = deriveViewModel(o, pending, noTouch, anchorAt + 5000);
  assert(shows<bool>(view.shuffle, true, FieldAuthority::Pending) &&
         shows(view.transport, std::optional(PlaybackStatus::Paused), FieldAuthority::Observed) &&
         shows<int>(view.volume, 12, FieldAuthority::Observed) &&
         shows<Repeat>(view.repeat, Repeat::Off, FieldAuthority::Observed) &&
         view.positionMs.value == 1000u && !view.contentPending);
  // Pending volume yields to a finger on the same control; observed yields to both.
  MusicIntent level;
  level.volume = Volume{false, 50};
  const auto pendingVolume = accept(10, level, o, anchorAt);
  assert(shows<int>(deriveViewModel(o, pendingVolume, noTouch, anchorAt).volume, 50,
                    FieldAuthority::Pending));
  assert(shows<int>(deriveViewModel(o, pendingVolume, finger, anchorAt).volume, 70,
                    FieldAuthority::Interaction));
}

void roomChange() {
  AppState display;
  display.observed = playing();
  display.pending = accept(11, command(TransportCommand::Pause), display.observed, anchorAt);
  const InteractionState finger{"RINCON_A", 20000, 60};
  // A session's publication replaces observations but keeps the display's pending.
  AppState incoming;
  incoming.observed = playing();
  incoming.observed.volume = 31;
  assert(publishSelectedState(display, incoming, "RINCON_A") && display.pending.jobId == 11 &&
         display.observed.volume == 31);
  // Selection clears the observation and pending values together; an
  // interaction begun in the old room owns nothing in the new one.
  Room bedroom{"RINCON_B", "Bedroom", "", "", "", true, "bedroom"};
  assert(selectObservedRoom(display, bedroom) && !display.pending.active());
  const auto view = deriveViewModel(display.observed, display.pending, finger, anchorAt);
  assert(!view.updating && view.volume.authority == FieldAuthority::Unknown &&
         view.positionMs.authority == FieldAuthority::Unknown &&
         view.transport.authority == FieldAuthority::Unknown);
  // A pending entry for another target never owns fields either.
  display.observed = playing();
  display.observed.targetId = "RINCON_B";
  display.pending = accept(12, command(TransportCommand::Pause), playing(), anchorAt);
  assert(
      deriveViewModel(display.observed, display.pending, noTouch, anchorAt).transport.authority ==
      FieldAuthority::Observed);
  assert(std::string(fieldAuthorityName(FieldAuthority::Interaction)) == "interaction" &&
         std::string(fieldAuthorityName(FieldAuthority::Pending)) == "pending" &&
         std::string(fieldAuthorityName(FieldAuthority::Observed)) == "observed" &&
         std::string(fieldAuthorityName(FieldAuthority::Unknown)) == "unknown");
}
} // namespace

int main() {
  playbackClock();
  seekModel();
  transportModel();
  fieldIndependence();
  roomChange();
  std::cout << "View model checks passed: playback clock, seek drag and pending seek, optimistic "
               "transport, field independence, stale projection, room change\n";
}
