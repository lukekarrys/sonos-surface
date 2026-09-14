#pragma once
#if defined(SURFACE_WAVESHARE_1_8) && SURFACE_LVGL_PLAYGROUND
#include "SurfaceDevice.h"
#include "WaveshareLvglSupport.h"
#include <surface_json.hpp>
class Arduino_GFX;

// LVGL playground adapter (todo/3-lvgl.md). LVGL is the presentation and
// input engine only: application state stays in AppState, and widget values
// are never authoritative Sonos state.
//
// Threading rule: the Arduino main task owns LVGL. lv_init, lv_timer_handler,
// the flush callback, the indev callback, and every widget create/update/delete
// run only from Waveshare.cpp's poll/render path. The Sonos worker and artwork
// tasks publish into AppState and the artwork buffers under their existing
// locks; the main task copies the snapshot and updates widgets from the copy.
// No lv_* call from any other task, ever.
//
// Power rule: nothing here touches DevicePower. The raw finger sample in
// Waveshare.cpp is the only touch activity channel; LVGL timers, animations,
// redraws, and injected USB input never count as local activity.
namespace surface::device::lvgl {
// Initializes LVGL once on the panel with the PSRAM canvas as the DIRECT-mode
// buffer; a later call after peripheral recovery only repaints.
bool begin(Arduino_GFX& panel, uint16_t* canvas, uint32_t now);
// One calibrated (or injected) sample per touch poll: the single indev path.
void touch(int x, int y, bool pressed, uint32_t now);
// Release the pointer and drop the active press so nothing completes later.
void cancelTouch(uint32_t now);
// lv_timer_handler plus frame diagnostics; pollGapMax is reported and reset
// with each flushed frame.
void service(uint32_t now, uint32_t& pollGapMax);
// Update the observed-value widgets from the main task's state copy.
void render(const AppState& state, const BoardContext& context, const uint16_t* artwork,
            bool artworkChanged, uint32_t now);
// BOOT short press, injected button, or `ui-nav next`.
void nextScreen(const char* source);
bool setRenderMode(const std::string& name);
const char* hitName(int x, int y);
nlohmann::json stateJson(bool held, unsigned injectPending, bool injectOpen);
// Stop servicing before the panel is powered down for deep sleep.
void stop();
} // namespace surface::device::lvgl
#endif
