#pragma once
#include <SurfaceCore.h>
#include "DevicePower.h"

namespace surface::device {
enum class Input {
  None,
  RoomNext,
  Play,
  Pause,
  Toggle,
  Refresh,
  Payload,
  Error,
  Intent,
  RoomSelect,
  QueuePage
};
struct BoardEvent {
  BoardEvent(Input value = Input::None, std::string message = {})
      : input(value), text(std::move(message)) {}
  Input input = Input::None;
  LocalActivity activity = LocalActivity::None;
  std::string text;
  MusicIntent intent;
  std::string targetId, trackIdentity;
  std::optional<uint32_t> queueRevision;
  uint32_t start = 0, count = 4;
};
struct BoardContext {
  std::vector<Room> rooms;
  bool readOnly = true, online = false, busy = false;
  std::string feedback;
};
#if defined(SURFACE_WAVESHARE_1_8)
void boardContext(const BoardContext& context);
#endif
#if defined(SURFACE_STICK_S3)
bool boardWriterActive();
#endif
bool boardBegin(std::string& notice);
BoardEvent boardPoll();
bool boardCommand(const std::string& line);
void boardRender(const AppState& state, const std::string& notice);
void boardPrepareSleep();
[[noreturn]] void boardSleep();
void begin();
void loop();
} // namespace surface::device
