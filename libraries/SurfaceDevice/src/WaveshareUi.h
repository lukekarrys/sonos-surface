#pragma once
#include "SurfaceDevice.h"
#include <algorithm>
#include <tuple>
#include <cstdlib>

namespace surface::device {
// Device-local, portable interaction logic. No network, persistence, or SDKs.
struct UiRect {
  int x, y, w, h;
  bool contains(int px, int py) const { return px >= x && px < x+w && py >= y && py < y+h; }
};
namespace waveshareLayout {
constexpr UiRect header{32, 28, 304, 44};
constexpr UiRect progress{40, 210, 288, 42};
constexpr UiRect previous{32, 258, 88, 56}, play{128, 258, 112, 56}, next{248, 258, 88, 56};
constexpr UiRect volume{40, 322, 288, 44};
constexpr UiRect shuffle{32, 374, 94, 42}, repeat{134, 374, 94, 42}, queue{236, 374, 100, 42};
constexpr UiRect back{32, 28, 84, 44}, reload{228, 28, 108, 44};
constexpr UiRect pageBack{32, 364, 140, 48}, pageNext{196, 364, 140, 48};
constexpr UiRect row(unsigned i) { return {32, 108 + int(i)*60, 304, 56}; }
constexpr int sliderLeft = 52, sliderRight = 316;
constexpr uint32_t pageSize = 4;
}
enum class WaveshareScreen { NowPlaying, Rooms, Queue };
enum class WaveshareControl { None, RoomHeader, Previous, Play, Next, Volume, Seek, Shuffle, Repeat,
                              Queue, Back, Reload, PageBack, PageNext, Row0, Row1, Row2, Row3 };
inline Repeat nextRepeat(Repeat r) { return r == Repeat::Off ? Repeat::All : r == Repeat::All ? Repeat::One : Repeat::Off; }
inline const char* repeatLabel(Repeat r) { return r == Repeat::Off ? "Off" : r == Repeat::All ? "All" : "One"; }
inline const char* playbackLabel(PlaybackStatus s) {
  switch (s) {
    case PlaybackStatus::Playing: return "Playing";
    case PlaybackStatus::Paused: return "Paused";
    case PlaybackStatus::Stopped: return "Stopped";
    case PlaybackStatus::NoMedia: return "No music";
    case PlaybackStatus::Transitioning: return "Changing track";
    default: return "Waiting for state";
  }
}
class WaveshareUi {
  WaveshareControl contact = WaveshareControl::None;
  bool touching = false, cancelled = false, queueRequested = false, sawQueueBusy = false;
  int firstX = 0, firstY = 0;
  std::string gestureRoom, gestureTrack;
  std::optional<uint32_t> gestureRevision, gestureDuration;
  uint32_t toastUntil = 0;
  std::string lastOutcome, lastFeedback;
  bool outcomeInitialized = false;
  static uint32_t valueAt(int x, uint32_t maximum) {
    using namespace waveshareLayout;
    const auto clamped = std::clamp(x, sliderLeft, sliderRight) - sliderLeft;
    return (uint64_t(clamped) * maximum + (sliderRight-sliderLeft)/2) / (sliderRight-sliderLeft);
  }
  BoardEvent mutation(MusicIntent intent, const std::string& label, uint32_t now) {
    if (context.busy) { notify("Busy - try again", now); return {}; }
    if (!fresh()) { notify("Refresh room first", now); return {}; }
    if (state.recoveryRequired) { notify("Check room; recovery needed", now); return {}; }
    // Read-only attempts still traverse the shared planner and HTTP guard.
    notify(context.readOnly ? "READ ONLY: " + label : "Requested: " + label, now);
    BoardEvent event; event.input = Input::Intent; event.intent = std::move(intent);
    event.targetId = state.observed.targetId;
    event.trackIdentity = state.observed.trackUri; event.queueRevision = state.observed.queueRevision;
    return event;
  }
public:
  AppState state;
  BoardContext context;
  WaveshareScreen screen = WaveshareScreen::NowPlaying;
  uint32_t queueStart = 0, roomStart = 0;
  std::optional<uint32_t> volumePreview, seekPreview;
  std::string toast;
  bool dirty = true;
  bool fresh() const { return context.online && state.observed.known && !state.observed.stale; }
  bool canSeek() const { const auto& o = state.observed; return fresh() && o.seekable == true && o.durationMs && *o.durationMs > 0 && o.positionMs; }
  bool canPlay() const { auto s = state.observed.transport; return fresh() && (s == PlaybackStatus::Playing || s == PlaybackStatus::Paused || s == PlaybackStatus::Stopped); }
  bool activeQueue() const { return fresh() && state.observed.queueBacked == true; }
  bool matchingPage() const { return state.queue && state.queue->targetId == state.observed.targetId && state.queue->start == queueStart; }
  bool canSelectQueue() const { return activeQueue() && matchingPage() && state.observed.queueRevision == state.queue->revision; }
  void notify(const std::string& text, uint32_t now) { toast = text; toastUntil = now + 3500; dirty = true; }
  void cancelTouch() { contact = WaveshareControl::None; touching = false; cancelled = true; volumePreview.reset(); seekPreview.reset(); dirty = true; }
  void update(const AppState& next, const BoardContext& c, uint32_t now) {
    const auto visual = [](const PlaybackState& o) {
      return std::tie(o.targetId,o.room,o.known,o.stale,o.title,o.artist,o.album,o.transport,o.source,
        o.volume,o.mute,o.shuffle,o.repeat,o.positionMs,o.durationMs,o.seekable,o.queueBacked,o.queueIndex,o.queueTotal,o.queueRevision);
    };
    bool roomsChanged = c.rooms.size() != context.rooms.size();
    if (!roomsChanged) for (unsigned i = 0; i < c.rooms.size(); ++i)
      if (c.rooms[i].id != context.rooms[i].id || c.rooms[i].name != context.rooms[i].name) roomsChanged = true;
    const bool pageChanged = next.queue.has_value() != state.queue.has_value() ||
      (next.queue && state.queue && (next.queue->observedAtMs != state.queue->observedAtMs ||
       next.queue->start != state.queue->start || next.queue->revision != state.queue->revision));
    dirty = dirty || visual(next.observed) != visual(state.observed) || pageChanged || roomsChanged ||
      next.refreshError != state.refreshError || next.queueError != state.queueError ||
      next.recoveryRequired != state.recoveryRequired || c.busy != context.busy ||
      c.readOnly != context.readOnly || c.online != context.online;
    if (touching && ((screen == WaveshareScreen::Rooms && roomsChanged) ||
                     (screen == WaveshareScreen::Queue && pageChanged))) cancelTouch();
    if (next.observed.targetId != state.observed.targetId) {
      cancelTouch(); queueStart = roomStart = 0; queueRequested = sawQueueBusy = false;
      screen = WaveshareScreen::NowPlaying; toast.clear(); outcomeInitialized = false;
    }
    if (touching && (next.observed.trackUri != gestureTrack || next.observed.queueRevision != gestureRevision ||
                     next.observed.durationMs != gestureDuration || next.observed.stale || !c.online)) cancelTouch();
    // Observation replacement also drops pages; never retain an independent UI cache.
    state = next; context = c;
    if (queueRequested && c.busy) sawQueueBusy = true;
    if (queueRequested && !c.busy && (sawQueueBusy || state.queue || !state.queueError.empty())) {
      queueRequested = sawQueueBusy = false;
    }
    if (matchingPage() && queueStart && queueStart >= state.queue->total) {
      queueStart = state.queue->total ? ((state.queue->total-1)/waveshareLayout::pageSize)*waveshareLayout::pageSize : 0;
      queueRequested = false;
    }
    if (roomStart >= c.rooms.size()) roomStart = 0;
    const auto outcome = std::to_string(next.requestId) + ":" + next.status + ":" + next.detail;
    if (outcomeInitialized && outcome != lastOutcome && next.status != "idle" && next.status != "pending") {
      const bool blocked = next.detail.find("READ_ONLY") != std::string::npos;
      if (blocked) notify("READ ONLY - command not sent", now);
      else if (next.status == "succeeded") notify(c.readOnly ? "READ ONLY - no change sent" : "Updated from Sonos", now);
      else notify("Command failed - check room", now);
    }
    lastOutcome = outcome; outcomeInitialized = true;
    if (c.feedback != lastFeedback && !c.feedback.empty()) notify(c.feedback, now);
    lastFeedback = c.feedback;
    if (!toast.empty() && int32_t(now-toastUntil) >= 0) { toast.clear(); dirty = true; }
  }
  WaveshareControl hit(int x, int y) const {
    using namespace waveshareLayout;
    if (screen == WaveshareScreen::NowPlaying) {
      if (header.contains(x,y)) return WaveshareControl::RoomHeader;
      if (previous.contains(x,y)) return WaveshareControl::Previous;
      if (play.contains(x,y)) return WaveshareControl::Play;
      if (next.contains(x,y)) return WaveshareControl::Next;
      if (volume.contains(x,y)) return WaveshareControl::Volume;
      if (progress.contains(x,y)) return WaveshareControl::Seek;
      if (shuffle.contains(x,y)) return WaveshareControl::Shuffle;
      if (repeat.contains(x,y)) return WaveshareControl::Repeat;
      if (queue.contains(x,y)) return WaveshareControl::Queue;
    } else {
      if (back.contains(x,y)) return WaveshareControl::Back;
      if (reload.contains(x,y)) return WaveshareControl::Reload;
      if (pageBack.contains(x,y)) return WaveshareControl::PageBack;
      if (pageNext.contains(x,y)) return WaveshareControl::PageNext;
      for (unsigned i = 0; i < 4; ++i) if (row(i).contains(x,y)) return WaveshareControl(int(WaveshareControl::Row0)+i);
    }
    return WaveshareControl::None;
  }
  BoardEvent requestQueue() {
    if (screen != WaveshareScreen::Queue || context.busy || !context.online || state.observed.targetId.empty() || queueRequested) return {};
    if (matchingPage() || !state.queueError.empty()) return {};
    queueRequested = true; sawQueueBusy = false;
    BoardEvent event; event.input = Input::QueuePage; event.targetId = state.observed.targetId;
    event.start = queueStart; event.count = waveshareLayout::pageSize;
    return event;
  }
  BoardEvent touch(int x, int y, int fingers, uint32_t now) {
    if (fingers > 1 || (fingers && (x < 0 || y < 0 || x >= 368 || y >= 448))) { cancelTouch(); return {}; }
    if (fingers) {
      if (cancelled) return {};
      if (!touching) {
        touching = true; firstX = x; firstY = y; contact = hit(x,y);
        gestureRoom = state.observed.targetId; gestureTrack = state.observed.trackUri;
        gestureRevision = state.observed.queueRevision; gestureDuration = state.observed.durationMs;
      }
      if (contact == WaveshareControl::Volume && fresh() && state.observed.volume) volumePreview = valueAt(x,100);
      else if (contact == WaveshareControl::Seek && canSeek()) seekPreview = valueAt(x,*state.observed.durationMs);
      else if (abs(x-firstX) > 16 || abs(y-firstY) > 16 || hit(x,y) != contact) contact = WaveshareControl::None;
      dirty = true;
      return {};
    }
    if (cancelled) { cancelled = false; return {}; }
    if (!touching) return {};
    touching = false;
    auto chosen = contact; contact = WaveshareControl::None;
    auto volume = volumePreview, seek = seekPreview; volumePreview.reset(); seekPreview.reset(); dirty = true;
    if (gestureRoom != state.observed.targetId) return {};
    MusicIntent intent;
    switch (chosen) {
      case WaveshareControl::RoomHeader: screen = WaveshareScreen::Rooms; roomStart = 0; return {};
      case WaveshareControl::Back: screen = WaveshareScreen::NowPlaying; return {};
      case WaveshareControl::Queue:
        screen = WaveshareScreen::Queue;
        queueStart = state.observed.queueIndex.value_or(0)/waveshareLayout::pageSize*waveshareLayout::pageSize;
        queueRequested = false; return requestQueue();
      case WaveshareControl::Reload:
        if (context.busy) { notify("Busy - try again",now); return {}; }
        if (screen == WaveshareScreen::Rooms) return {Input::Refresh, ""};
        state.queue.reset(); state.queueError.clear(); queueRequested = false; return requestQueue();
      case WaveshareControl::PageBack:
      case WaveshareControl::PageNext: {
        const bool forward = chosen == WaveshareControl::PageNext;
        if (screen == WaveshareScreen::Rooms) {
          if (forward && uint64_t(roomStart)+4 < context.rooms.size()) roomStart += 4;
          if (!forward && roomStart >= 4) roomStart -= 4;
        } else {
          if (context.busy) { notify("Busy - try again",now); return {}; }
          if (forward && matchingPage() && uint64_t(queueStart)+4 < state.queue->total) queueStart += 4;
          else if (!forward && queueStart >= 4) queueStart -= 4;
          else return {};
          state.queue.reset(); state.queueError.clear(); queueRequested = false; return requestQueue();
        }
        return {};
      }
      case WaveshareControl::Volume:
        if (!volume) break;
        intent.volume = Volume{false,int(*volume)}; return mutation(intent,"Volume " + std::to_string(*volume),now);
      case WaveshareControl::Seek:
        if (!seek || !canSeek()) { notify("This source cannot seek",now); return {}; }
        intent.seekPositionMs = *seek; return mutation(intent,"Seek",now);
      case WaveshareControl::Previous: intent.transport = TransportCommand::Previous; return mutation(intent,"Previous",now);
      case WaveshareControl::Next: intent.transport = TransportCommand::Next; return mutation(intent,"Next",now);
      case WaveshareControl::Play:
        if (!canPlay()) break;
        intent.transport = state.observed.transport == PlaybackStatus::Playing ? TransportCommand::Pause : TransportCommand::Play;
        return mutation(intent,*intent.transport == TransportCommand::Pause ? "Pause" : "Play",now);
      case WaveshareControl::Shuffle:
        if (!activeQueue() || !state.observed.shuffle.has_value()) break;
        intent.shuffle = !*state.observed.shuffle; return mutation(intent,"Shuffle",now);
      case WaveshareControl::Repeat:
        if (!activeQueue() || !state.observed.repeat) break;
        intent.repeat = nextRepeat(*state.observed.repeat); return mutation(intent,"Repeat " + std::string(repeatLabel(*intent.repeat)),now);
      case WaveshareControl::Row0: case WaveshareControl::Row1: case WaveshareControl::Row2: case WaveshareControl::Row3: {
        const unsigned row = int(chosen)-int(WaveshareControl::Row0);
        if (screen == WaveshareScreen::Rooms && roomStart+row < context.rooms.size()) {
          if (context.busy) { notify("Busy - try again",now); return {}; }
          screen = WaveshareScreen::NowPlaying;
          BoardEvent event; event.input = Input::RoomSelect; event.text = context.rooms[roomStart+row].displayId;
          return event;
        }
        if (screen == WaveshareScreen::Queue && matchingPage() && row < state.queue->items.size()) {
          if (!canSelectQueue()) { notify("Stored queue - selection unavailable",now); return {}; }
          intent.queueIndex = state.queue->items[row].index;
          return mutation(intent,"Queue item " + std::to_string(*intent.queueIndex+1),now);
        }
        return {};
      }
      default: return {};
    }
    notify("Control unavailable - refresh",now); return {};
  }
};
} // namespace surface::device
