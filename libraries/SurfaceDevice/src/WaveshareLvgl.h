#pragma once
#if defined(SURFACE_WAVESHARE_1_8) && !SURFACE_TOUCH_DIAGNOSTIC
#include "SurfaceDevice.h"
#include "WaveshareShell.h"
#include <surface_json.hpp>
class Arduino_GFX;

// LVGL adapter for ws-1.8. LVGL owns widgets, hit-testing, dragging,
// scrolling, screens, animations, and invalidation; the application owns
// authoritative state (AppState and the portable WaveshareShell/WaveshareUi
// model); Waveshare.cpp owns the panel, the PSRAM canvas, and the calibrated
// touch sample. Widget values are never authoritative: observed values flow
// AppState -> model -> widgets, and widget events flow into the model, whose
// release-to-submit rules produce BoardEvents.
//
// Render path, fixed by decision: DIRECT mode into the adapter's PSRAM canvas
// with one full-frame flush per refresh, because partial windows tear on this
// panel (docs/hardware.md).
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
// buffer and builds the three top-level screens; a later call after
// peripheral recovery only repaints.
bool begin(Arduino_GFX& panel, uint16_t* canvas, WaveshareShell& shell, uint32_t now);
// One calibrated (or injected) sample per touch poll: the single indev path.
void touch(int x, int y, bool pressed, uint32_t now);
// Release the pointer and drop the active press so nothing completes later.
void cancelTouch(uint32_t now);
// lv_timer_handler plus frame diagnostics; pollGapMax is reported and reset
// with each flushed frame.
void service(uint32_t now, uint32_t& pollGapMax);
// Sync the Now Playing widgets from the model copy when it changed, and the
// cover image when it changed.
void render(const uint16_t* artwork, bool artworkChanged, uint32_t now);
// The shell changed its active screen: cancel the LVGL press and load that
// screen. Returns the load time in ms.
uint32_t load();
// The Now Playing model changed its sub-view: show that container now.
void syncView();
const char* hitName(int x, int y);
nlohmann::json stateJson(bool held, unsigned injectPending, bool injectOpen);
// The display-edge diagnostic paints the canvas itself while paused.
void pause(bool paused);
// Stop servicing before the panel is powered down for deep sleep.
void stop();
} // namespace surface::device::lvgl
#endif
