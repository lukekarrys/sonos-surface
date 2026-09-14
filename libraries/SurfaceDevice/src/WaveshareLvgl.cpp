#if defined(SURFACE_WAVESHARE_1_8) && SURFACE_LVGL_PLAYGROUND
#include "WaveshareLvgl.h"
#include "ArtworkState.h"
#include "WaveshareUi.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include <cstdio>
#include <cstring>

extern "C" void surfaceLvglAssertFailed(const char* file, int line) {
  Serial.printf("[lvgl] ASSERT %s:%d\n", file, line);
  Serial.flush();
  abort();
}

namespace surface::device::lvgl {
namespace {
constexpr int width = 368, height = 448, stripeRows = 40, canvasRows = 400;
constexpr size_t stripeBytes = size_t(width) * stripeRows * sizeof(uint16_t);
constexpr size_t frameBytes = size_t(width) * height * sizeof(uint16_t);
Arduino_GFX* panel = nullptr;
uint16_t* frame = nullptr;  // The adapter's PSRAM canvas: DIRECT-mode buffer.
uint16_t* stripe = nullptr; // Internal SRAM: PARTIAL-mode buffer.
lv_display_t* display = nullptr;
lv_indev_t* indev = nullptr;
LvglRenderMode mode = LvglRenderMode::Partial;
LvglTouchSample sample;
bool stopped = false;
// Frame diagnostics, in the style of the [ui] frame line. A touch-driven frame
// logs itself; the rest are summarized every five seconds.
uint32_t pendingSampleAt = 0, frameSampleAt = 0, handlerStart = 0, refreshStart = 0,
         renderStart = 0, renderMs = 0, flushMs = 0, flushes = 0, flushPixels = 0;
uint32_t* pollGap = nullptr;
struct Totals {
  uint32_t frames = 0, renderMs = 0, flushMs = 0, pixels = 0, touchFrames = 0, touchMs = 0,
           touchMaxMs = 0;
} totals;
struct Frame {
  uint32_t touchToFlush = 0, sampleToHandler = 0, render = 0, flush = 0, flushes = 0, pixels = 0,
           pollGapMax = 0;
} lastFrame;
uint32_t frames = 0;

PlaygroundScreen current = PlaygroundScreen::Targets;
lv_obj_t* screens[playgroundScreenCount] = {};
lv_obj_t* slider = nullptr;
lv_obj_t* sliderValue = nullptr;
lv_obj_t* pad = nullptr;
lv_obj_t* cursor = nullptr;
lv_obj_t* cursorLabel = nullptr;
lv_obj_t* observed = nullptr;
lv_obj_t* artworkImage = nullptr;
lv_obj_t* countLabel = nullptr;
lv_obj_t* drawing = nullptr;
std::string observedText;
const uint16_t* artworkShown = nullptr;
lv_image_dsc_t artworkDescriptor{};
int lastDrawX = -1, lastDrawY = -1;

// Every interactive widget carries a name for the logs and, for click
// targets, its size bucket (0 large, 1 medium, 2 small, 3 big; -1 none).
struct Named {
  char name[12];
  int size, bucket;
};
Named targets[16];
unsigned namedCount = 0;
unsigned clicks[4] = {};
Named* name(const char* text, int size = 0, int bucket = -1) {
  auto& entry = targets[namedCount++];
  snprintf(entry.name, sizeof entry.name, "%s", text);
  entry.size = size;
  entry.bucket = bucket;
  return &entry;
}
lv_point_t activePoint() {
  lv_point_t point{-1, -1};
  if (auto* active = lv_indev_active())
    lv_indev_get_point(active, &point);
  return point;
}
void refreshCounts() {
  lv_label_set_text_fmt(countLabel, "L %u  M %u\nS %u  big %u", clicks[0], clicks[1], clicks[2],
                        clicks[3]);
}
void onEvent(lv_event_t* e) {
  const auto code = lv_event_get_code(e);
  const auto* named = static_cast<const Named*>(lv_event_get_user_data(e));
  const char* label = nullptr;
  switch (code) {
  case LV_EVENT_PRESSED:
    label = "pressed";
    break;
  case LV_EVENT_RELEASED:
    label = "released";
    break;
  case LV_EVENT_CLICKED:
    label = "clicked";
    break;
  case LV_EVENT_PRESS_LOST:
    label = "press-lost";
    break;
  case LV_EVENT_INDEV_RESET:
    label = "indev-reset";
    // The press is cancelled without a release; drop the pressed look now.
    lv_obj_remove_state(static_cast<lv_obj_t*>(lv_event_get_target(e)), LV_STATE_PRESSED);
    break;
  case LV_EVENT_SCROLL_END:
    label = "scroll-end";
    break;
  default:
    return;
  }
  if (code == LV_EVENT_CLICKED && named->bucket >= 0) {
    ++clicks[named->bucket];
    refreshCounts();
  }
  const auto point = activePoint();
  Serial.printf("[lvgl] event=%s target=%s size=%d x=%d y=%d screen=%s\n", label, named->name,
                named->size, int(point.x), int(point.y), playgroundScreenName(current));
}
lv_obj_t* named(lv_obj_t* object, Named* entry) {
  lv_obj_set_user_data(object, entry);
  lv_obj_add_event_cb(object, onEvent, LV_EVENT_ALL, entry);
  return object;
}
lv_obj_t* label(lv_obj_t* parent, int x, int y, const char* text, const lv_font_t* font = nullptr) {
  auto* object = lv_label_create(parent);
  lv_obj_set_pos(object, x, y);
  lv_label_set_text(object, text);
  if (font)
    lv_obj_set_style_text_font(object, font, 0);
  return object;
}
lv_obj_t* squareButton(lv_obj_t* parent, int x, int y, int size, const char* text, Named* entry) {
  auto* button = named(lv_button_create(parent), entry);
  lv_obj_set_pos(button, x, y);
  lv_obj_set_size(button, size, size);
  lv_obj_set_style_pad_all(button, 0, 0);
  lv_obj_set_style_radius(button, 6, 0);
  auto* caption = lv_label_create(button);
  lv_label_set_text(caption, text);
  lv_obj_center(caption);
  return button;
}
lv_obj_t* screen(const char* title) {
  auto* object = lv_obj_create(nullptr);
  lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
  label(object, 12, 8, title, &lv_font_montserrat_20);
  return object;
}
void onSlider(lv_event_t* e) {
  const auto value = lv_slider_get_value(static_cast<lv_obj_t*>(lv_event_get_target(e)));
  lv_label_set_text_fmt(sliderValue, "%d", int(value));
  const auto point = activePoint();
  Serial.printf("[lvgl] slider value=%d x=%d y=%d\n", int(value), int(point.x), int(point.y));
}
void onPad(lv_event_t* e) {
  const auto code = lv_event_get_code(e);
  if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING)
    return;
  const auto point = activePoint();
  lv_area_t area;
  lv_obj_get_coords(pad, &area);
  lv_obj_set_pos(cursor, point.x - area.x1 - 10, point.y - area.y1 - 10);
  lv_label_set_text_fmt(cursorLabel, "%d,%d", int(point.x), int(point.y));
}
void onCanvas(lv_event_t* e) {
  const auto code = lv_event_get_code(e);
  if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST || code == LV_EVENT_INDEV_RESET) {
    lastDrawX = lastDrawY = -1;
    return;
  }
  if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING)
    return;
  const auto point = activePoint();
  lv_area_t area;
  lv_obj_get_coords(drawing, &area);
  const int x = point.x - area.x1, y = point.y - area.y1;
  if (lastDrawX >= 0 && (x != lastDrawX || y != lastDrawY)) {
    lv_layer_t layer;
    lv_canvas_init_layer(drawing, &layer);
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_palette_main(LV_PALETTE_CYAN);
    line.width = 4;
    line.round_start = line.round_end = 1;
    line.p1 = {lastDrawX, lastDrawY};
    line.p2 = {x, y};
    lv_draw_line(&layer, &line);
    lv_canvas_finish_layer(drawing, &layer);
  }
  lastDrawX = x;
  lastDrawY = y;
}
void onClear(lv_event_t* e) {
  if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    lv_canvas_fill_bg(drawing, lv_color_black(), LV_OPA_COVER);
}
void buildTargets() {
  auto* s = screens[0] = screen("1/3 targets");
  for (int i = 0; i < 4; ++i) {
    char text[12];
    snprintf(text, sizeof text, "large-%d", i + 1);
    squareButton(s, 12 + i * 88, 48, 44, "L", name(text, 44, 0));
  }
  for (int i = 0; i < 5; ++i) {
    char text[12];
    snprintf(text, sizeof text, "medium-%d", i + 1);
    squareButton(s, 12 + i * 70, 112, 32, "M", name(text, 32, 1));
  }
  for (int i = 0; i < 6; ++i) {
    char text[12];
    snprintf(text, sizeof text, "small-%d", i + 1);
    squareButton(s, 12 + i * 58, 164, 24, "s", name(text, 24, 2));
  }
  auto* big = named(lv_button_create(s), name("big", 160, 3));
  lv_obj_set_pos(big, 12, 210);
  lv_obj_set_size(big, 160, 64);
  auto* caption = label(big, 0, 0, "TAP", &lv_font_montserrat_28);
  lv_obj_center(caption);
  countLabel = label(s, 184, 214, "");
  refreshCounts();
  // Thin-primitive probe at odd offsets: a 1 px border and 1 px lines.
  auto* probe = lv_obj_create(s);
  lv_obj_set_pos(probe, 13, 297);
  lv_obj_set_size(probe, 141, 57);
  lv_obj_set_style_radius(probe, 0, 0);
  lv_obj_set_style_border_width(probe, 1, 0);
  lv_obj_set_style_pad_all(probe, 0, 0);
  lv_obj_remove_flag(probe, lv_obj_flag_t(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  static const lv_point_precise_t horizontal[] = {{3, 11}, {121, 11}},
                                  vertical[] = {{129, 5}, {129, 51}};
  for (const auto* points : {horizontal, vertical}) {
    auto* line = lv_line_create(probe);
    lv_line_set_points(line, points, 2);
    lv_obj_set_style_line_width(line, 1, 0);
    lv_obj_set_style_line_color(line, lv_color_white(), 0);
  }
  label(probe, 5, 21, "1px lines");
  // Local animation: a rotating arc, redrawn continuously while visible.
  auto* arc = lv_arc_create(s);
  lv_obj_set_pos(arc, 292, 290);
  lv_obj_set_size(arc, 64, 64);
  lv_arc_set_bg_angles(arc, 0, 360);
  lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
  lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
  lv_anim_t animation;
  lv_anim_init(&animation);
  lv_anim_set_var(&animation, arc);
  lv_anim_set_values(&animation, 0, 360);
  lv_anim_set_duration(&animation, 2000);
  lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_exec_cb(&animation, [](void* object, int32_t angle) {
    lv_arc_set_angles(static_cast<lv_obj_t*>(object), angle, angle + 90);
  });
  lv_anim_start(&animation);
  label(s, 12, 364, "BOOT: next screen");
}
void buildSlider() {
  auto* s = screens[1] = screen("2/3 slider");
  observed = label(s, 12, 40, "");
  lv_obj_set_width(observed, 276);
  lv_label_set_long_mode(observed, LV_LABEL_LONG_DOT);
  artworkDescriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
  artworkDescriptor.header.cf = LV_COLOR_FORMAT_RGB565;
  artworkDescriptor.header.w = artworkSize;
  artworkDescriptor.header.h = artworkSize;
  artworkDescriptor.header.stride = artworkSize * 2;
  artworkDescriptor.data_size = artworkSize * artworkSize * 2;
  artworkImage = lv_image_create(s);
  lv_obj_set_pos(artworkImage, 292, 40);
  lv_obj_set_size(artworkImage, artworkSize, artworkSize);
  lv_obj_add_flag(artworkImage, LV_OBJ_FLAG_HIDDEN);
  slider = named(lv_slider_create(s), name("slider", 300, -1));
  lv_obj_set_pos(slider, 34, 128);
  lv_obj_set_size(slider, 300, 20);
  lv_slider_set_range(slider, 0, 100);
  lv_slider_set_value(slider, 50, LV_ANIM_OFF);
  lv_obj_set_ext_click_area(slider, 14);
  lv_obj_add_event_cb(slider, onSlider, LV_EVENT_VALUE_CHANGED, nullptr);
  sliderValue = label(s, 34, 158, "50", &lv_font_montserrat_28);
  pad = named(lv_obj_create(s), name("pad", 344, -1));
  lv_obj_set_pos(pad, 12, 204);
  lv_obj_set_size(pad, 344, 92);
  lv_obj_set_style_pad_all(pad, 0, 0);
  lv_obj_remove_flag(pad, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(pad, onPad, LV_EVENT_ALL, nullptr);
  cursorLabel = label(pad, 6, 4, "track finger");
  cursor = lv_obj_create(pad);
  lv_obj_set_size(cursor, 20, 20);
  lv_obj_set_style_radius(cursor, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(cursor, lv_palette_main(LV_PALETTE_AMBER), 0);
  lv_obj_set_style_border_width(cursor, 0, 0);
  lv_obj_remove_flag(cursor, lv_obj_flag_t(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_set_pos(cursor, 162, 36);
  auto* list = named(lv_obj_create(s), name("list", 344, -1));
  lv_obj_set_pos(list, 12, 304);
  lv_obj_set_size(list, 344, 132);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, 4, 0);
  for (int i = 1; i <= 24; ++i) {
    char text[24];
    snprintf(text, sizeof text, "Scroll row %d", i);
    auto* row = label(list, 0, 0, text);
    lv_obj_set_width(row, lv_pct(100));
  }
}
void buildCanvas() {
  auto* s = screens[2] = screen("3/3 canvas: draw");
  auto* clear = named(lv_button_create(s), name("clear", 90, -1));
  lv_obj_set_pos(clear, 266, 4);
  lv_obj_set_size(clear, 90, 36);
  auto* caption = label(clear, 0, 0, "Clear");
  lv_obj_center(caption);
  lv_obj_add_event_cb(clear, onClear, LV_EVENT_CLICKED, nullptr);
  drawing = named(lv_canvas_create(s), name("canvas", 368, -1));
  lv_obj_set_pos(drawing, 0, 48);
  const size_t bytes = LV_CANVAS_BUF_SIZE(width, canvasRows, 16, LV_DRAW_BUF_STRIDE_ALIGN);
  auto* buffer = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (buffer) {
    lv_canvas_set_buffer(drawing, buffer, width, canvasRows, LV_COLOR_FORMAT_RGB565);
    lv_canvas_fill_bg(drawing, lv_color_black(), LV_OPA_COVER);
  } else
    Serial.println("[lvgl] canvas allocation failed");
  lv_obj_add_flag(drawing, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(drawing, onCanvas, LV_EVENT_ALL, nullptr);
}
void flush(lv_display_t* d, const lv_area_t* area, uint8_t* pixels) {
  const auto started = millis();
  const int w = area->x2 - area->x1 + 1, h = area->y2 - area->y1 + 1;
  if (mode == LvglRenderMode::Direct) {
    // LVGL rendered dirty areas straight into the PSRAM canvas; keep the
    // known-good full-frame transfer once per refresh.
    if (lv_display_flush_is_last(d))
      panel->draw16bitRGBBitmap(0, 0, frame, width, height);
  } else
    panel->draw16bitRGBBitmap(area->x1, area->y1, reinterpret_cast<uint16_t*>(pixels), w, h);
  flushMs += millis() - started;
  ++flushes;
  flushPixels += uint32_t(w) * h;
  lv_display_flush_ready(d);
}
void onDisplay(lv_event_t* e) {
  switch (lv_event_get_code(e)) {
  case LV_EVENT_INVALIDATE_AREA:
    if (mode == LvglRenderMode::PartialEven) {
      auto* area = static_cast<lv_area_t*>(lv_event_get_param(e));
      const auto aligned = evenAlignedArea({area->x1, area->y1, area->x2, area->y2}, width, height);
      area->x1 = aligned.x1;
      area->y1 = aligned.y1;
      area->x2 = aligned.x2;
      area->y2 = aligned.y2;
    }
    break;
  case LV_EVENT_REFR_START:
    refreshStart = millis();
    frameSampleAt = pendingSampleAt;
    pendingSampleAt = 0;
    flushMs = flushes = flushPixels = renderMs = 0;
    break;
  case LV_EVENT_RENDER_START:
    renderStart = millis();
    break;
  case LV_EVENT_RENDER_READY:
    renderMs = millis() - renderStart;
    break;
  case LV_EVENT_REFR_READY: {
    if (!flushes)
      break;
    const auto now = millis();
    const auto drawn = renderMs > flushMs ? renderMs - flushMs : 0;
    ++frames;
    lastFrame = {frameSampleAt ? now - frameSampleAt : 0,
                 frameSampleAt ? handlerStart - frameSampleAt : 0,
                 drawn,
                 flushMs,
                 flushes,
                 flushPixels,
                 pollGap ? *pollGap : 0};
    if (pollGap)
      *pollGap = 0;
    ++totals.frames;
    totals.renderMs += drawn;
    totals.flushMs += flushMs;
    totals.pixels += flushPixels;
    if (frameSampleAt) {
      ++totals.touchFrames;
      totals.touchMs += lastFrame.touchToFlush;
      totals.touchMaxMs = std::max(totals.touchMaxMs, lastFrame.touchToFlush);
      Serial.printf("[lvgl] frame touch-to-flush=%lu sample-to-handler=%lu render=%lu flush=%lu "
                    "flushes=%lu px=%lu poll-gap-max=%lu heap=%lu psram-free=%lu\n",
                    lastFrame.touchToFlush, lastFrame.sampleToHandler, lastFrame.render,
                    lastFrame.flush, lastFrame.flushes, lastFrame.pixels, lastFrame.pollGapMax,
                    ESP.getFreeHeap(), ESP.getFreePsram());
    }
    break;
  }
  default:
    break;
  }
}
void readTouch(lv_indev_t*, lv_indev_data_t* data) {
  data->point.x = sample.x;
  data->point.y = sample.y;
  data->state = sample.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
void applyRenderMode() {
  const bool direct = mode == LvglRenderMode::Direct;
  lv_display_set_buffers(display, direct ? frame : stripe, nullptr,
                         direct ? frameBytes : stripeBytes,
                         direct ? LV_DISPLAY_RENDER_MODE_DIRECT : LV_DISPLAY_RENDER_MODE_PARTIAL);
  if (auto* active = lv_display_get_screen_active(display))
    lv_obj_invalidate(active);
  Serial.printf("[lvgl] render mode=%s buffer=%u bytes\n", lvglRenderModeName(mode),
                unsigned(direct ? frameBytes : stripeBytes));
}
} // namespace

bool begin(Arduino_GFX& output, uint16_t* canvas, uint32_t now) {
  panel = &output;
  frame = canvas;
  if (display) {
    // Peripheral recovery reset the panel; repaint from the existing model.
    lv_obj_invalidate(lv_display_get_screen_active(display));
    return true;
  }
  stripe = static_cast<uint16_t*>(
      heap_caps_aligned_alloc(16, stripeBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!stripe)
    return false;
  const auto heap = ESP.getFreeHeap(), psram = ESP.getFreePsram();
  Serial.printf("[lvgl] init stage=lv_init stack-free=%u\n",
                unsigned(uxTaskGetStackHighWaterMark(nullptr)));
  lv_init();
  lv_tick_set_cb([] { return uint32_t(millis()); });
#if LV_USE_LOG
  lv_log_register_print_cb([](lv_log_level_t, const char* text) {
    Serial.printf("[lvgl] log %s", text);
    if (!*text || text[strlen(text) - 1] != '\n')
      Serial.println();
  });
#endif
  Serial.println("[lvgl] init stage=display");
  display = lv_display_create(width, height);
  lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(display, flush);
  lv_display_add_event_cb(display, onDisplay, LV_EVENT_ALL, nullptr);
  applyRenderMode();
  indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, readTouch);
  // Event mode: the adapter's 30 ms touch poll drives lv_indev_read directly,
  // so a sample is never left waiting for a second timer.
  lv_indev_set_mode(indev, LV_INDEV_MODE_EVENT);
  sample = {0, 0, false, now};
  Serial.println("[lvgl] init stage=screens");
  buildTargets();
  buildSlider();
  buildCanvas();
  current = PlaygroundScreen::Targets;
  lv_screen_load(screens[0]);
  Serial.printf("[lvgl] init stage=loaded stack-free=%u\n",
                unsigned(uxTaskGetStackHighWaterMark(nullptr)));
  Serial.printf("[lvgl] init version=%d.%d.%d stripe=%u bytes canvas=%u bytes heap-before=%lu "
                "heap-after=%lu psram-before=%lu psram-after=%lu\n",
                LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, unsigned(stripeBytes),
                unsigned(frameBytes), heap, ESP.getFreeHeap(), psram, ESP.getFreePsram());
  return true;
}
void touch(int x, int y, bool pressed, uint32_t now) {
  if (!indev)
    return;
  sample = {x, y, pressed, now};
  if (pressed)
    pendingSampleAt = now;
  lv_indev_read(indev);
}
void cancelTouch(uint32_t now) {
  if (!indev)
    return;
  const bool wasPressed = sample.pressed;
  sample = {0, 0, false, now};
  if (wasPressed) {
    // Drop the active object first so the release reaches no widget, then
    // ignore the finger until it is actually lifted.
    lv_indev_reset(indev, nullptr);
    lv_indev_wait_release(indev);
  }
  lv_indev_read(indev);
}
void service(uint32_t now, uint32_t& pollGapMax) {
  if (stopped || !display)
    return;
  pollGap = &pollGapMax;
  handlerStart = now;
  lv_timer_handler();
  pollGap = nullptr;
  static uint32_t lastStats = 0;
  if (now - lastStats >= 5000) {
    lastStats = now;
    Serial.printf("[lvgl] stats frames=%lu render-ms=%lu flush-ms=%lu px=%lu touch-frames=%lu "
                  "touch-avg=%lu touch-max=%lu mode=%s screen=%s heap=%lu psram-free=%lu "
                  "stack-free=%u\n",
                  totals.frames, totals.renderMs, totals.flushMs, totals.pixels, totals.touchFrames,
                  totals.touchFrames ? totals.touchMs / totals.touchFrames : 0, totals.touchMaxMs,
                  lvglRenderModeName(mode), playgroundScreenName(current), ESP.getFreeHeap(),
                  ESP.getFreePsram(), unsigned(uxTaskGetStackHighWaterMark(nullptr)));
    totals = {};
  }
}
void render(const AppState& state, const BoardContext& context, const uint16_t* artwork,
            bool artworkChanged, uint32_t) {
  if (!display)
    return;
  const auto& o = state.observed;
  std::string text = (o.room.empty() ? "No room" : o.room) + " - " + playbackLabel(o.transport) +
                     "\n" + (o.title.empty() ? "(no title)" : o.title) + "\nvol " +
                     (o.volume ? std::to_string(*o.volume) : "?") +
                     (context.readOnly ? " - READ ONLY" : "") + (context.busy ? " - busy" : "") +
                     (context.online ? "" : " - offline");
  if (text != observedText) {
    observedText = text;
    lv_label_set_text(observed, text.c_str());
  }
  if (artwork != artworkShown || artworkChanged) {
    artworkShown = artwork;
    if (artwork) {
      artworkDescriptor.data = reinterpret_cast<const uint8_t*>(artwork);
      lv_image_set_src(artworkImage, &artworkDescriptor);
      lv_obj_remove_flag(artworkImage, LV_OBJ_FLAG_HIDDEN);
      lv_obj_invalidate(artworkImage);
    } else {
      lv_image_set_src(artworkImage, nullptr);
      lv_obj_add_flag(artworkImage, LV_OBJ_FLAG_HIDDEN);
    }
  }
}
void nextScreen(const char* source) {
  if (!display)
    return;
  cancelTouch(millis());
  current = nextPlaygroundScreen(current);
  const auto started = millis();
  lv_screen_load(screens[unsigned(current)]);
  Serial.printf("[ui] nav next screen=%u name=%s source=%s load=%lu\n", unsigned(current),
                playgroundScreenName(current), source, millis() - started);
}
bool setRenderMode(const std::string& text) {
  LvglRenderMode next;
  if (!parseLvglRenderMode(text, next) || !display)
    return false;
  mode = next;
  applyRenderMode();
  return true;
}
const char* hitName(int x, int y) {
  if (!display)
    return "none";
  lv_point_t point{x, y};
  for (auto* object = lv_indev_search_obj(lv_display_get_screen_active(display), &point); object;
       object = lv_obj_get_parent(object))
    if (const auto* entry = static_cast<const Named*>(lv_obj_get_user_data(object)))
      return entry->name;
  return "none";
}
nlohmann::json stateJson(bool held, unsigned injectPending, bool injectOpen) {
  nlohmann::json state = nlohmann::json::object();
  state["lvgl"] = true;
  state["screen"] = playgroundScreenName(current);
  state["renderMode"] = lvglRenderModeName(mode);
  state["slider"] = slider ? int(lv_slider_get_value(slider)) : -1;
  auto& counts = state["clicks"];
  counts["large"] = clicks[0];
  counts["medium"] = clicks[1];
  counts["small"] = clicks[2];
  counts["big"] = clicks[3];
  auto& touch = state["touch"];
  touch["pressed"] = sample.pressed;
  touch["x"] = sample.x;
  touch["y"] = sample.y;
  touch["held"] = held;
  touch["cancelled"] = false;
  touch["injectPending"] = injectPending;
  touch["injectOpen"] = injectOpen;
  auto& timing = state["frame"];
  timing["frames"] = frames;
  timing["touchToFlush"] = lastFrame.touchToFlush;
  timing["sampleToHandler"] = lastFrame.sampleToHandler;
  timing["render"] = lastFrame.render;
  timing["flush"] = lastFrame.flush;
  timing["flushes"] = lastFrame.flushes;
  timing["pixels"] = lastFrame.pixels;
  timing["pollGapMax"] = lastFrame.pollGapMax;
  state["heap"] = ESP.getFreeHeap();
  state["psramFree"] = ESP.getFreePsram();
  return state;
}
void stop() { stopped = true; }
} // namespace surface::device::lvgl
#endif
