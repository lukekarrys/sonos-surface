#pragma once
#include <SurfaceCore.h>

namespace surface::device {
enum class Input { None, Source, Play, Pause, Refresh, Payload, Error };
struct BoardEvent { Input input = Input::None; std::string text; };
bool boardBegin(std::string& notice);
BoardEvent boardPoll();
void boardRender(const AppState& state, const std::string& notice);
void begin();
void loop();
} // namespace surface::device
