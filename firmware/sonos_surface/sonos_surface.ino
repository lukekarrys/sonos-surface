#include <SurfaceDevice.h>
#if defined(SURFACE_WAVESHARE_1_8)
// LVGL renders on the main task; its software renderer needs more than the
// 8 KiB Arduino default beneath the runtime's own frames.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);
#endif
void setup() { surface::device::begin(); }
void loop() { surface::device::loop(); }
