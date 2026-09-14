/* Project-owned LVGL 9 configuration for the ws-1.8 build. Everything not
 * listed keeps the lv_conf_internal.h default. Only the widgets, fonts, and
 * features the screens use are enabled; examples, demos, file systems, image
 * codecs, and every other extra are off. */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16

/* Allocator: WaveshareLvgl.cpp implements lv_malloc_core and friends on the
 * ESP heap with PSRAM preferred and internal SRAM as the fallback. Internal
 * SRAM is the scarce resource (networking lives there); widgets, styles, label
 * texts, and LVGL's draw buffers all fit PSRAM's speed. Free heap/PSRAM are
 * measured instead of lv_mem_monitor, which needs the built-in pool. */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CUSTOM
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

/* Refresh cadence aligned with the adapter's 30 ms touch poll. The indev runs
 * in event mode, fed from that poll; this period only paces rendering. */
#define LV_DEF_REFR_PERIOD 30
#define LV_DPI_DEF 130

#define LV_USE_OS LV_OS_NONE
#define LV_USE_DRAW_SW 1
#define LV_DRAW_SW_COMPLEX 1
#define LV_DRAW_SW_DRAW_UNIT_CNT 1
#define LV_USE_FLOAT 0
#define LV_USE_MATRIX 0

/* Logging: warnings and errors reach the serial console through the adapter's
 * print callback. -DSURFACE_LVGL_LOG_LEVEL=LV_LOG_LEVEL_NONE silences it, or
 * a lower level such as LV_LOG_LEVEL_INFO widens it, at build time. */
#ifndef SURFACE_LVGL_LOG_LEVEL
#define SURFACE_LVGL_LOG_LEVEL LV_LOG_LEVEL_WARN
#endif
#define LV_USE_LOG 1
#define LV_LOG_LEVEL SURFACE_LVGL_LOG_LEVEL
#define LV_LOG_PRINTF 0
/* A failed assertion must never spin silently (LVGL's default is while(1)):
 * report it and abort so the panic handler prints a backtrace and reboots. */
#define LV_ASSERT_HANDLER_INCLUDE "WaveshareLvglAssert.h"
#define LV_ASSERT_HANDLER surfaceLvglAssertFailed(__FILE__, __LINE__);
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_ASSERT_STYLE 0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ 0

#define LV_USE_SYSMON 0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0
#define LV_USE_PROFILER 0
#define LV_USE_OBJ_ID 0
#define LV_USE_OBJ_NAME 0
#define LV_USE_OBJ_PROPERTY 0
#define LV_USE_GESTURE_RECOGNITION 0

#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14
#define LV_USE_FONT_PLACEHOLDER 1
#define LV_USE_BIDI 0
#define LV_USE_ARABIC_PERSIAN_CHARS 0

/* Widgets the screens use. */
#define LV_USE_LABEL 1
#define LV_USE_BUTTON 1
#define LV_USE_BAR 1
#define LV_USE_SLIDER 1
#define LV_USE_ARC 0
#define LV_USE_CANVAS 0
#define LV_USE_LINE 0
#define LV_USE_IMAGE 1
#define LV_USE_ANIMIMG 0
#define LV_USE_ARCLABEL 0
#define LV_USE_BUTTONMATRIX 0
#define LV_USE_CALENDAR 0
#define LV_USE_CHART 0
#define LV_USE_CHECKBOX 0
#define LV_USE_DROPDOWN 0
#define LV_USE_IMAGEBUTTON 0
#define LV_USE_KEYBOARD 0
#define LV_USE_LED 0
#define LV_USE_LIST 0
#define LV_USE_LOTTIE 0
#define LV_USE_MENU 0
#define LV_USE_MSGBOX 0
#define LV_USE_ROLLER 0
#define LV_USE_SCALE 0
#define LV_USE_SPAN 0
#define LV_USE_SPINBOX 0
#define LV_USE_SPINNER 0
#define LV_USE_SWITCH 0
#define LV_USE_TABLE 0
#define LV_USE_TABVIEW 0
#define LV_USE_TEXTAREA 0
#define LV_USE_TILEVIEW 0
#define LV_USE_WIN 0
#define LV_USE_3DTEXTURE 0

#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_THEME_DEFAULT_GROW 1
#define LV_THEME_DEFAULT_TRANSITION_TIME 80
#define LV_USE_THEME_SIMPLE 0
#define LV_USE_THEME_MONO 0
#define LV_USE_FLEX 1
#define LV_USE_GRID 0

/* No file systems, codecs, or extras. */
#define LV_USE_FS_STDIO 0
#define LV_USE_FS_POSIX 0
#define LV_USE_FS_WIN32 0
#define LV_USE_FS_FATFS 0
#define LV_USE_FS_MEMFS 0
#define LV_USE_FS_LITTLEFS 0
#define LV_USE_FS_ARDUINO_ESP_LITTLEFS 0
#define LV_USE_FS_ARDUINO_SD 0
#define LV_USE_FS_UEFI 0
#define LV_USE_LODEPNG 0
#define LV_USE_LIBPNG 0
#define LV_USE_BMP 0
#define LV_USE_TJPGD 0
#define LV_USE_LIBJPEG_TURBO 0
#define LV_USE_GIF 0
#define LV_USE_QRCODE 0
#define LV_USE_BARCODE 0
#define LV_USE_FREETYPE 0
#define LV_USE_TINY_TTF 0
#define LV_USE_RLOTTIE 0
#define LV_USE_THORVG 0
#define LV_USE_VECTOR_GRAPHIC 0
#define LV_USE_LZ4 0
#define LV_USE_SVG 0
#define LV_USE_FFMPEG 0
#define LV_USE_SNAPSHOT 0
#define LV_USE_MONKEY 0
#define LV_USE_GRIDNAV 0
#define LV_USE_FRAGMENT 0
#define LV_USE_IMGFONT 0
#define LV_USE_OBSERVER 0
#define LV_USE_IME_PINYIN 0
#define LV_USE_FILE_EXPLORER 0
#define LV_USE_XML 0
#define LV_USE_TRANSLATION 0
#define LV_USE_TEST 0
#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0
#define LV_USE_DEMO_WIDGETS 0
#define LV_USE_DEMO_BENCHMARK 0
#define LV_USE_DEMO_STRESS 0
#define LV_USE_DEMO_MUSIC 0
#define LV_USE_DEMO_KEYPAD_AND_ENCODER 0
#define LV_USE_DEMO_RENDER 0
#define LV_USE_DEMO_SCROLL 0
#define LV_USE_DEMO_FLEX_LAYOUT 0
#define LV_USE_DEMO_MULTILANG 0
#define LV_USE_DEMO_TRANSFORM 0
#define LV_USE_DEMO_VECTOR_GRAPHIC 0
#define LV_USE_DEMO_EBIKE 0
#define LV_USE_DEMO_HIGH_RES 0
#define LV_USE_DEMO_SMARTWATCH 0

#endif /* LV_CONF_H */
