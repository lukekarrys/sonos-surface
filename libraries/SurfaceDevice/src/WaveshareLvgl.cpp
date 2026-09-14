#if defined(SURFACE_WAVESHARE_1_8) && !SURFACE_TOUCH_DIAGNOSTIC
#include "WaveshareLvgl.h"
#include "ArtworkState.h"
#include "WaveshareState.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include <cstdio>
#include <cstring>

extern "C" {
void surfaceLvglAssertFailed(const char* file, int line) {
  Serial.printf("[lvgl] ASSERT %s:%d\n", file, line);
  Serial.flush();
  abort();
}
// LV_STDLIB_CUSTOM allocator (lv_conf.h): every LVGL allocation prefers PSRAM
// and falls back to internal SRAM, so widgets and draw buffers never compete
// with networking for the scarce internal heap. Pools are not supported.
void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void*, size_t) { return nullptr; }
void lv_mem_remove_pool(lv_mem_pool_t) {}
void* lv_malloc_core(size_t size) {
  return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
}
void* lv_realloc_core(void* p, size_t size) {
  return heap_caps_realloc_prefer(p, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
}
void lv_free_core(void* p) { heap_caps_free(p); }
void lv_mem_monitor_core(lv_mem_monitor_t* monitor) { *monitor = {}; }
lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
}

namespace surface::device::lvgl {
namespace {
constexpr int width = 368, height = 448;
constexpr size_t frameBytes = size_t(width) * height * sizeof(uint16_t);
Arduino_GFX* panel = nullptr;
uint16_t* frame = nullptr; // The adapter's PSRAM canvas: the DIRECT-mode buffer.
lv_display_t* display = nullptr;
lv_indev_t* indev = nullptr;
WaveshareShell* shell = nullptr;
// The latest calibrated sample the adapter delivered; the read callback
// reports exactly this. Injected samples arrive through the same call.
struct Sample {
  int x = 0, y = 0;
  bool pressed = false;
} sample;
bool stopped = false, paused = false;
// Frame diagnostics. A touch-driven frame logs itself; the rest are
// summarized every five seconds.
uint32_t pendingSampleAt = 0, frameSampleAt = 0, handlerStart = 0, renderStart = 0, renderMs = 0,
         flushMs = 0, flushes = 0, flushPixels = 0;
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

lv_color_t white() { return lv_color_white(); }
lv_color_t black() { return lv_color_black(); }
lv_color_t muted() { return lv_color_hex(0x888C88); }
lv_color_t accent() { return lv_color_hex(0x68F0D8); }
lv_color_t tile() { return lv_color_hex(0x181C18); }
lv_color_t amber() { return lv_color_hex(0xF8B040); }

// Every interactive widget carries a name for the logs and hit reports, the
// Now Playing control it stands for, and for the playground's click targets
// its size bucket (0 large, 1 medium, 2 small; -1 none).
struct Named {
  char name[12];
  WaveshareControl control;
  int size, bucket;
};
constexpr unsigned namedCapacity = 48;
Named names[namedCapacity];
unsigned namedCount = 0;
unsigned clicks[3] = {};
Named* name(const char* text, WaveshareControl control = WaveshareControl::None, int size = 0,
            int bucket = -1) {
  if (namedCount >= namedCapacity)
    surfaceLvglAssertFailed(__FILE__, __LINE__);
  auto& entry = names[namedCount++];
  snprintf(entry.name, sizeof entry.name, "%s", text);
  entry.control = control;
  entry.size = size;
  entry.bucket = bucket;
  return &entry;
}

// The three top-level screens: one LVGL screen each, built once at boot and
// switched with lv_screen_load after the shell changed its active screen.
// enter runs after the load; leaving needs nothing beyond the press
// cancellation every switch performs.
struct Screen {
  void (*build)(lv_obj_t* screen);
  void (*enter)();
  lv_obj_t* object = nullptr;
};

// Now Playing and its Rooms/Queue sub-views, one container each.
lv_obj_t* viewNow = nullptr;
lv_obj_t *roomLabel, *sonosLabel, *offlineLabel, *artworkImage, *placeholder, *titleLabel,
    *artistLabel, *albumLabel, *statusLabel, *seekSlider, *positionLabel, *durationLabel,
    *previousButton, *playButton, *nextButton, *volumeLabel, *volumeSlider, *shuffleButton,
    *repeatButton, *queueButton, *hintLabel;
struct ListView {
  lv_obj_t *view, *back, *reload, *room, *empty, *emptyDetail, *rows[4], *rowTitles[4],
      *rowDetails[4], *rowMarks[4], *count, *pageBack, *pageNext, *status;
} rooms, queue;
const uint16_t* artworkShown = nullptr;
lv_image_dsc_t artworkDescriptor{};
lv_obj_t* countLabel = nullptr;
lv_obj_t* playSlider = nullptr;
lv_obj_t* sliderValue = nullptr;
lv_obj_t* pad = nullptr;
lv_obj_t* cursor = nullptr;
lv_obj_t* cursorLabel = nullptr;

lv_point_t activePoint() {
  lv_point_t point{-1, -1};
  if (auto* active = lv_indev_active())
    lv_indev_get_point(active, &point);
  return point;
}
void refreshCounts() {
  lv_label_set_text_fmt(countLabel, "L %u   M %u   S %u", clicks[0], clicks[1], clicks[2]);
}
// Widget events reach the model as the control LVGL found under the press;
// the sample stream keeps the gesture rules.
void onEvent(lv_event_t* e) {
  const auto code = lv_event_get_code(e);
  const auto* named = static_cast<const Named*>(lv_event_get_user_data(e));
  const char* label = nullptr;
  switch (code) {
  case LV_EVENT_PRESSED:
    label = "pressed";
    if (named->control != WaveshareControl::None)
      shell->pressed(named->control);
    break;
  case LV_EVENT_RELEASED:
    label = "released";
    break;
  case LV_EVENT_CLICKED:
    label = "clicked";
    break;
  case LV_EVENT_PRESS_LOST:
    label = "press-lost";
    if (named->control != WaveshareControl::None)
      shell->pressLost();
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
                named->size, int(point.x), int(point.y), surfaceScreenName(shell->active()));
}
void onModelSlider(lv_event_t* e) {
  auto* object = static_cast<lv_obj_t*>(lv_event_get_target(e));
  const auto* named = static_cast<const Named*>(lv_obj_get_user_data(object));
  shell->slid(named->control, uint32_t(lv_slider_get_value(object)),
              uint32_t(lv_slider_get_max_value(object)));
}
lv_obj_t* named(lv_obj_t* object, Named* entry) {
  lv_obj_set_user_data(object, entry);
  lv_obj_add_event_cb(object, onEvent, LV_EVENT_ALL, entry);
  return object;
}
lv_obj_t* label(lv_obj_t* parent, int x, int y, const char* text, const lv_font_t* font = nullptr,
                lv_color_t color = lv_color_white(), int width = 0) {
  auto* object = lv_label_create(parent);
  lv_obj_set_pos(object, x, y);
  lv_label_set_text(object, text);
  if (font)
    lv_obj_set_style_text_font(object, font, 0);
  lv_obj_set_style_text_color(object, color, 0);
  if (width) {
    lv_obj_set_width(object, width);
    lv_label_set_long_mode(object, LV_LABEL_LONG_MODE_DOTS);
  }
  return object;
}
// A full-screen, invisible container: sub-views toggle by hiding it.
lv_obj_t* container(lv_obj_t* parent) {
  auto* object = lv_obj_create(parent);
  lv_obj_set_pos(object, 0, 0);
  lv_obj_set_size(object, width, height);
  lv_obj_set_style_pad_all(object, 0, 0);
  lv_obj_set_style_border_width(object, 0, 0);
  lv_obj_set_style_radius(object, 0, 0);
  lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
  lv_obj_remove_flag(object, lv_obj_flag_t(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  return object;
}
lv_obj_t* caption(lv_obj_t* button) { return lv_obj_get_child(button, 0); }
lv_obj_t* button(lv_obj_t* parent, const UiRect& r, const char* text, Named* entry,
                 const lv_font_t* font = nullptr) {
  auto* object = named(lv_button_create(parent), entry);
  lv_obj_set_pos(object, r.x, r.y);
  lv_obj_set_size(object, r.w, r.h);
  lv_obj_set_style_pad_all(object, 0, 0);
  lv_obj_set_style_radius(object, 10, 0);
  lv_obj_set_style_shadow_width(object, 0, 0);
  lv_obj_set_style_bg_color(object, tile(), 0);
  auto* text_ = label(object, 0, 0, text, font);
  lv_obj_center(text_);
  return object;
}
// A model slider: the accepted thin track between x=52 and x=316 with a large
// touch area, press-locked so a drag keeps it wherever the finger wanders.
lv_obj_t* slider(lv_obj_t* parent, int y, int32_t maximum, Named* entry) {
  auto* object = named(lv_slider_create(parent), entry);
  lv_obj_set_pos(object, waveshareLayout::sliderLeft, y);
  lv_obj_set_size(object, waveshareLayout::sliderRight - waveshareLayout::sliderLeft, 6);
  lv_slider_set_range(object, 0, maximum);
  lv_slider_set_value(object, 0, LV_ANIM_OFF);
  lv_obj_set_ext_click_area(object, 16);
  lv_obj_add_flag(object, LV_OBJ_FLAG_PRESS_LOCK);
  lv_obj_set_style_bg_color(object, tile(), LV_PART_MAIN);
  lv_obj_set_style_bg_color(object, accent(), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(object, muted(), LV_PART_INDICATOR | LV_STATE_DISABLED);
  lv_obj_set_style_bg_color(object, white(), LV_PART_KNOB);
  lv_obj_set_style_bg_color(object, amber(), LV_PART_KNOB | LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, LV_PART_KNOB | LV_STATE_DISABLED);
  lv_obj_set_style_pad_all(object, 6, LV_PART_KNOB);
  lv_obj_add_event_cb(object, onModelSlider, LV_EVENT_VALUE_CHANGED, nullptr);
  return object;
}
lv_obj_t* screen() {
  auto* object = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(object, black(), 0);
  lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
  return object;
}
// Sync helpers change a widget only when its value differs, so an unchanged
// model invalidates nothing.
void setText(lv_obj_t* object, const char* text) {
  if (strcmp(lv_label_get_text(object), text) != 0)
    lv_label_set_text(object, text);
}
void setText(lv_obj_t* object, const std::string& text) { setText(object, text.c_str()); }
void setColor(lv_obj_t* object, lv_color_t color) {
  if (!lv_color_eq(lv_obj_get_style_text_color(object, LV_PART_MAIN), color))
    lv_obj_set_style_text_color(object, color, 0);
}
void setHidden(lv_obj_t* object, bool hidden) {
  if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN) != hidden)
    lv_obj_set_flag(object, LV_OBJ_FLAG_HIDDEN, hidden);
}
void setEnabled(lv_obj_t* object, bool enabled) {
  if (lv_obj_has_state(object, LV_STATE_DISABLED) == enabled)
    lv_obj_set_state(object, LV_STATE_DISABLED, !enabled);
}
// Buttons stay pressable when unavailable so the model can explain the
// refusal; the look dims them, and a selected control inverts.
void look(lv_obj_t* object, bool enabled, bool selected = false) {
  const auto background = selected ? accent() : tile();
  if (!lv_color_eq(lv_obj_get_style_bg_color(object, LV_PART_MAIN), background))
    lv_obj_set_style_bg_color(object, background, 0);
  setColor(caption(object), selected ? black() : enabled ? white() : muted());
}
std::string timeText(std::optional<uint32_t> ms) {
  if (!ms)
    return "--:--";
  const auto sec = *ms / 1000;
  return std::to_string(sec / 60) + ":" + (sec % 60 < 10 ? "0" : "") + std::to_string(sec % 60);
}

void buildListView(ListView& v, lv_obj_t* parent, const char* title) {
  using namespace waveshareLayout;
  v.view = container(parent);
  v.back =
      button(v.view, back, "Back", name("back", WaveshareControl::Back), &lv_font_montserrat_20);
  v.reload = button(v.view, reload, "Refresh", name("reload", WaveshareControl::Reload));
  label(v.view, 132, 44, title, nullptr, accent());
  v.room = label(v.view, 32, 83, "", &lv_font_montserrat_20, white(), 304);
  v.empty = label(v.view, 40, 160, "", &lv_font_montserrat_20, white(), 296);
  v.emptyDetail = label(v.view, 52, 188, "", nullptr, muted(), 284);
  static const char* const rowNames[] = {"row0", "row1", "row2", "row3"};
  for (unsigned i = 0; i < 4; ++i) {
    const auto r = row(i);
    v.rows[i] =
        button(v.view, r, "", name(rowNames[i], WaveshareControl(int(WaveshareControl::Row0) + i)),
               &lv_font_montserrat_20);
    auto* text = caption(v.rows[i]);
    lv_obj_set_width(text, r.w - 24);
    lv_label_set_long_mode(text, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(text, LV_ALIGN_TOP_LEFT, 12, 6);
    v.rowTitles[i] = text;
    v.rowDetails[i] = label(v.rows[i], 12, 33, "", nullptr, muted(), r.w - 24);
    v.rowMarks[i] = lv_obj_create(v.rows[i]);
    lv_obj_set_pos(v.rowMarks[i], 0, 0);
    lv_obj_set_size(v.rowMarks[i], 4, r.h);
    lv_obj_set_style_radius(v.rowMarks[i], 2, 0);
    lv_obj_set_style_border_width(v.rowMarks[i], 0, 0);
    lv_obj_set_style_bg_color(v.rowMarks[i], accent(), 0);
    lv_obj_remove_flag(v.rowMarks[i],
                       lv_obj_flag_t(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_add_flag(v.rowMarks[i], LV_OBJ_FLAG_HIDDEN);
  }
  v.count = label(v.view, 52, 349, "", nullptr, muted(), 284);
  v.pageBack = button(v.view, pageBack, "Earlier", name("page-back", WaveshareControl::PageBack),
                      &lv_font_montserrat_20);
  v.pageNext = button(v.view, pageNext, "More", name("page-next", WaveshareControl::PageNext),
                      &lv_font_montserrat_20);
  v.status = label(v.view, 40, 426, "", nullptr, amber(), 296);
  lv_obj_add_flag(v.view, LV_OBJ_FLAG_HIDDEN);
}
void buildNowPlaying(lv_obj_t* s) {
  using namespace waveshareLayout;
  viewNow = container(s);
  auto* headerButton =
      button(viewNow, header, "", name("room-header", WaveshareControl::RoomHeader));
  lv_obj_set_style_bg_opa(headerButton, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_opa(headerButton, LV_OPA_30, LV_STATE_PRESSED);
  roomLabel = caption(headerButton);
  lv_obj_set_style_text_font(roomLabel, &lv_font_montserrat_20, 0);
  lv_obj_set_width(roomLabel, 270);
  lv_label_set_long_mode(roomLabel, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_align(roomLabel, LV_ALIGN_LEFT_MID, 0, 0);
  auto* chevron = label(headerButton, 0, 0, LV_SYMBOL_DOWN, nullptr, accent());
  lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, -8, 0);
  sonosLabel = label(viewNow, 32, 76, "SONOS", nullptr, muted());
  offlineLabel = label(viewNow, 206, 76, "", nullptr, amber(), 130);
  lv_obj_set_style_text_align(offlineLabel, LV_TEXT_ALIGN_RIGHT, 0);
  placeholder = lv_obj_create(viewNow);
  lv_obj_set_pos(placeholder, 32, 96);
  lv_obj_set_size(placeholder, artworkSize, artworkSize);
  lv_obj_set_style_radius(placeholder, 10, 0);
  lv_obj_set_style_border_width(placeholder, 0, 0);
  lv_obj_set_style_pad_all(placeholder, 0, 0);
  lv_obj_set_style_bg_color(placeholder, tile(), 0);
  lv_obj_remove_flag(placeholder, lv_obj_flag_t(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  auto* record = lv_obj_create(placeholder);
  lv_obj_set_size(record, 40, 40);
  lv_obj_center(record);
  lv_obj_set_style_radius(record, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(record, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(record, 1, 0);
  lv_obj_set_style_border_color(record, muted(), 0);
  lv_obj_remove_flag(record, lv_obj_flag_t(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  auto* dot = lv_obj_create(placeholder);
  lv_obj_set_size(dot, 10, 10);
  lv_obj_center(dot);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(dot, 0, 0);
  lv_obj_set_style_bg_color(dot, accent(), 0);
  lv_obj_remove_flag(dot, lv_obj_flag_t(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  artworkDescriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
  artworkDescriptor.header.cf = LV_COLOR_FORMAT_RGB565;
  artworkDescriptor.header.w = artworkSize;
  artworkDescriptor.header.h = artworkSize;
  artworkDescriptor.header.stride = artworkSize * 2;
  artworkDescriptor.data_size = artworkSize * artworkSize * 2;
  artworkImage = lv_image_create(viewNow);
  lv_obj_set_pos(artworkImage, 32, 96);
  lv_obj_set_size(artworkImage, artworkSize, artworkSize);
  lv_obj_add_flag(artworkImage, LV_OBJ_FLAG_HIDDEN);
  titleLabel = label(viewNow, 108, 97, "", &lv_font_montserrat_20, white(), 228);
  lv_obj_set_height(titleLabel, 48);
  artistLabel = label(viewNow, 108, 145, "", &lv_font_montserrat_20, muted(), 228);
  albumLabel = label(viewNow, 32, 169, "", nullptr, muted(), 304);
  statusLabel = label(viewNow, 32, 190, "", nullptr, muted(), 304);
  seekSlider = slider(viewNow, 216, seekResolution, name("seek", WaveshareControl::Seek));
  positionLabel = label(viewNow, 52, 236, "--:--", nullptr, muted());
  durationLabel = label(viewNow, 240, 236, "--:--", nullptr, muted(), 76);
  lv_obj_set_style_text_align(durationLabel, LV_TEXT_ALIGN_RIGHT, 0);
  previousButton = button(viewNow, previous, LV_SYMBOL_PREV,
                          name("previous", WaveshareControl::Previous), &lv_font_montserrat_20);
  playButton =
      button(viewNow, play, "Play", name("play", WaveshareControl::Play), &lv_font_montserrat_20);
  nextButton = button(viewNow, next, LV_SYMBOL_NEXT, name("next", WaveshareControl::Next),
                      &lv_font_montserrat_20);
  volumeLabel = label(viewNow, 52, 324, "Volume --", &lv_font_montserrat_20, muted(), 264);
  volumeSlider = slider(viewNow, 348, 100, name("volume", WaveshareControl::Volume));
  shuffleButton =
      button(viewNow, shuffle, "Shuffle --", name("shuffle", WaveshareControl::Shuffle));
  repeatButton = button(viewNow, repeat, "Repeat --", name("repeat", WaveshareControl::Repeat));
  queueButton = button(viewNow, waveshareLayout::queue, "Queue",
                       name("queue", WaveshareControl::Queue), &lv_font_montserrat_20);
  hintLabel = label(viewNow, 52, 426, "", nullptr, muted(), 284);
  buildListView(rooms, s, "ROOMS");
  buildListView(queue, s, "QUEUE");
}
void buildGrouping(lv_obj_t* s) {
  auto* title = label(s, 0, 170, "GROUPING", &lv_font_montserrat_28, accent(), width);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  auto* next = label(s, 0, 210, "Coming next", &lv_font_montserrat_20, muted(), width);
  lv_obj_set_style_text_align(next, LV_TEXT_ALIGN_CENTER, 0);
  label(s, 32, 396, "BOOT: next screen", nullptr, muted());
}
lv_obj_t* squareButton(lv_obj_t* parent, int x, int y, int size, const char* text, Named* entry) {
  auto* object = named(lv_button_create(parent), entry);
  lv_obj_set_pos(object, x, y);
  lv_obj_set_size(object, size, size);
  lv_obj_set_style_pad_all(object, 0, 0);
  lv_obj_set_style_radius(object, 6, 0);
  lv_obj_set_style_shadow_width(object, 0, 0);
  auto* text_ = lv_label_create(object);
  lv_label_set_text(text_, text);
  lv_obj_center(text_);
  return object;
}
void onPlaySlider(lv_event_t* e) {
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
// Development screen: the interaction experiments, laid out inside the
// reachable area and corner mask so they measure the real product
// constraints. Nothing here reaches the model or Sonos.
void buildPlayground(lv_obj_t* s) {
  label(s, 32, 28, "PLAYGROUND", &lv_font_montserrat_20, accent());
  for (int i = 0; i < 4; ++i) {
    char text[12];
    snprintf(text, sizeof text, "large-%d", i + 1);
    squareButton(s, 32 + i * 86, 60, 44, "L", name(text, WaveshareControl::None, 44, 0));
  }
  for (int i = 0; i < 5; ++i) {
    char text[12];
    snprintf(text, sizeof text, "medium-%d", i + 1);
    squareButton(s, 32 + i * 66, 112, 32, "M", name(text, WaveshareControl::None, 32, 1));
  }
  for (int i = 0; i < 6; ++i) {
    char text[12];
    snprintf(text, sizeof text, "small-%d", i + 1);
    squareButton(s, 32 + i * 56, 152, 24, "s", name(text, WaveshareControl::None, 24, 2));
  }
  countLabel = label(s, 32, 184, "", nullptr, muted());
  refreshCounts();
  playSlider = named(lv_slider_create(s), name("slider", WaveshareControl::None, 264));
  lv_obj_set_pos(playSlider, waveshareLayout::sliderLeft, 212);
  lv_obj_set_size(playSlider, waveshareLayout::sliderRight - waveshareLayout::sliderLeft, 20);
  lv_slider_set_range(playSlider, 0, 100);
  lv_slider_set_value(playSlider, 50, LV_ANIM_OFF);
  lv_obj_set_ext_click_area(playSlider, 14);
  lv_obj_add_flag(playSlider, LV_OBJ_FLAG_PRESS_LOCK);
  lv_obj_add_event_cb(playSlider, onPlaySlider, LV_EVENT_VALUE_CHANGED, nullptr);
  sliderValue = label(s, 32, 240, "50", &lv_font_montserrat_20);
  pad = named(lv_obj_create(s), name("pad", WaveshareControl::None, 304));
  lv_obj_set_pos(pad, 32, 268);
  lv_obj_set_size(pad, 304, 64);
  lv_obj_set_style_pad_all(pad, 0, 0);
  lv_obj_remove_flag(pad, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(pad, onPad, LV_EVENT_ALL, nullptr);
  cursorLabel = label(pad, 6, 4, "track finger");
  cursor = lv_obj_create(pad);
  lv_obj_set_size(cursor, 20, 20);
  lv_obj_set_style_radius(cursor, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(cursor, amber(), 0);
  lv_obj_set_style_border_width(cursor, 0, 0);
  lv_obj_remove_flag(cursor, lv_obj_flag_t(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_set_pos(cursor, 142, 22);
  auto* list = named(lv_obj_create(s), name("list", WaveshareControl::None, 304));
  lv_obj_set_pos(list, 32, 340);
  lv_obj_set_size(list, 304, 76);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, 4, 0);
  for (int i = 1; i <= 24; ++i) {
    char text[24];
    snprintf(text, sizeof text, "Scroll row %d", i);
    auto* row = label(list, 0, 0, text);
    lv_obj_set_width(row, lv_pct(100));
  }
}
Screen screens[surfaceScreenCount] = {
    {buildNowPlaying, syncView}, {buildGrouping, nullptr}, {buildPlayground, nullptr}};

void syncNowPlaying() {
  const auto& ui = shell->nowPlaying;
  const auto& o = ui.state.observed;
  const auto& c = ui.context;
  setText(roomLabel, o.room.empty() ? "Choose room" : o.room);
  setText(sonosLabel, c.readOnly ? "READ ONLY" : "SONOS");
  setColor(sonosLabel, c.readOnly ? amber() : muted());
  setText(offlineLabel, !c.online ? "Wi-Fi offline" : o.stale ? "State out of date" : "");
  const char* sourceTitle = !o.known                                        ? "Loading room..."
                            : o.source == PlaybackSource::Live              ? "Live / TV"
                            : o.source == PlaybackSource::AppleMusicStation ? "Radio station"
                            : o.source == PlaybackSource::Queue             ? "Queue playback"
                            : o.source == PlaybackSource::Other             ? "Audio source"
                                                                            : "Nothing selected";
  setText(titleLabel, o.title.empty() ? sourceTitle : o.title.c_str());
  setText(artistLabel, o.artist.empty() ? "Artist unavailable" : o.artist);
  setText(albumLabel, o.album);
  const std::string status = !ui.toast.empty()                ? ui.toast
                             : c.busy || c.backgroundActive   ? "Updating..."
                             : ui.state.recoveryRequired      ? "Check room; recovery needed"
                             : !ui.state.refreshError.empty() ? "Room unavailable - retry"
                             : !c.online                      ? "Wi-Fi offline"
                                                              : playbackLabel(o.transport);
  setText(statusLabel, status);
  setColor(statusLabel, !ui.toast.empty() || !ui.state.refreshError.empty() ? amber() : muted());
  // A slider mid-drag belongs to LVGL; only an idle slider follows observation.
  if (!ui.seekPreview) {
    const auto duration = o.durationMs.value_or(0);
    const auto position = std::min(o.positionMs.value_or(0), duration);
    lv_slider_set_value(
        seekSlider,
        duration ? int32_t(uint64_t(position) * waveshareLayout::seekResolution / duration) : 0,
        LV_ANIM_OFF);
  }
  setEnabled(seekSlider, ui.canSeek());
  setText(positionLabel, timeText(ui.seekPreview ? ui.seekPreview : o.positionMs));
  setColor(positionLabel, ui.seekPreview ? amber() : muted());
  setText(durationLabel,
          o.source == PlaybackSource::Live || o.source == PlaybackSource::AppleMusicStation
              ? "LIVE"
              : timeText(o.durationMs));
  look(previousButton, ui.fresh());
  setText(caption(playButton), o.transport == PlaybackStatus::Playing ? "Pause" : "Play");
  look(playButton, ui.canPlay(), ui.canPlay());
  look(nextButton, ui.fresh());
  setText(volumeLabel, "Volume " +
                           (ui.volumePreview ? std::to_string(*ui.volumePreview)
                            : o.volume       ? std::to_string(*o.volume)
                                             : "--") +
                           (o.mute == true ? " (muted)" : ""));
  setColor(volumeLabel, ui.volumePreview ? amber() : muted());
  if (!ui.volumePreview)
    lv_slider_set_value(volumeSlider, int32_t(std::min<uint32_t>(o.volume.value_or(0), 100)),
                        LV_ANIM_OFF);
  setEnabled(volumeSlider, ui.fresh() && o.volume.has_value());
  setText(caption(shuffleButton),
          "Shuffle " + std::string(o.shuffle ? (*o.shuffle ? "On" : "Off") : "--"));
  look(shuffleButton, ui.activeQueue() && o.shuffle.has_value());
  setText(caption(repeatButton), "Repeat " + std::string(o.repeat ? repeatLabel(*o.repeat) : "--"));
  look(repeatButton, ui.activeQueue() && o.repeat.has_value());
  setText(hintLabel, ui.seekPreview || ui.volumePreview ? "Preview - lift to submit"
                     : o.queueIndex && o.queueTotal ? "Track " + std::to_string(*o.queueIndex + 1) +
                                                          " of " + std::to_string(*o.queueTotal)
                                                    : "Tap room name to switch");
}
void syncStatus(const ListView& v) {
  const auto& ui = shell->nowPlaying;
  setText(v.room, ui.state.observed.room.empty() ? "Choose a room" : ui.state.observed.room);
  setText(v.status, !ui.toast.empty()                                ? ui.toast
                    : ui.context.readOnly                            ? "READ ONLY"
                    : ui.context.busy || ui.context.backgroundActive ? "Updating..."
                                                                     : "SONOS");
}
void syncRooms() {
  const auto& ui = shell->nowPlaying;
  const auto& list = ui.context.rooms;
  syncStatus(rooms);
  setHidden(rooms.empty, !list.empty());
  setText(rooms.empty, ui.context.busy || ui.context.backgroundActive ? "Finding rooms..."
                                                                      : "No selectable rooms");
  setHidden(rooms.emptyDetail, true);
  for (unsigned i = 0; i < 4; ++i) {
    const bool shown = ui.roomStart + i < list.size();
    setHidden(rooms.rows[i], !shown);
    if (!shown)
      continue;
    const auto& room = list[ui.roomStart + i];
    setText(rooms.rowTitles[i], room.name);
    setHidden(rooms.rowDetails[i], true);
    look(rooms.rows[i], true, room.id == ui.state.observed.targetId);
  }
  setText(rooms.count, std::to_string(list.size()) + " configured rooms available");
  look(rooms.pageBack, ui.roomStart > 0);
  look(rooms.pageNext, uint64_t(ui.roomStart) + 4 < list.size());
}
void syncQueue() {
  const auto& ui = shell->nowPlaying;
  syncStatus(queue);
  const bool page = ui.matchingPage();
  const unsigned items = page ? unsigned(std::min<size_t>(ui.state.queue->items.size(), 4)) : 0;
  for (unsigned i = 0; i < 4; ++i) {
    setHidden(queue.rows[i], i >= items);
    if (i >= items)
      continue;
    const auto& item = ui.state.queue->items[i];
    const bool current = ui.canSelectQueue() && ui.state.observed.queueIndex == item.index;
    setText(queue.rowTitles[i],
            std::to_string(item.index + 1) + ". " + (item.title.empty() ? "Untitled" : item.title));
    setColor(queue.rowTitles[i], current ? accent() : white());
    setText(queue.rowDetails[i], item.artist.empty() ? "Artist unavailable" : item.artist);
    setHidden(queue.rowMarks[i], !current);
  }
  if (page) {
    const auto& p = *ui.state.queue;
    setHidden(queue.empty, !p.items.empty());
    lv_obj_set_pos(queue.empty, 52, 174);
    setText(queue.empty, p.total ? "No items on this page" : "Queue is empty");
    setHidden(queue.emptyDetail, true);
    setText(queue.count, "Page " + std::to_string(p.start / 4 + 1) + " / " +
                             std::to_string(std::max<uint64_t>(1, (uint64_t(p.total) + 3) / 4)) +
                             (ui.activeQueue() ? "" : " - stored queue"));
  } else {
    setHidden(queue.empty, false);
    lv_obj_set_pos(queue.empty, 52, 158);
    setText(queue.empty, !ui.state.queueError.empty() ? "Queue could not load"
                         : !ui.context.online         ? "Wi-Fi offline"
                                                      : "Loading queue...");
    setHidden(queue.emptyDetail, false);
    setText(queue.emptyDetail,
            !ui.state.queueError.empty() ? "Tap Refresh to try again" : "Four items per page");
    setText(queue.count, "");
  }
  look(queue.pageBack, ui.queueStart > 0);
  look(queue.pageNext, page && uint64_t(ui.queueStart) + 4 < ui.state.queue->total);
}
void flush(lv_display_t* d, const lv_area_t* area, uint8_t*) {
  const auto started = millis();
  // LVGL rendered the dirty areas straight into the PSRAM canvas; the panel
  // receives the known-good full-frame transfer once per refresh.
  if (lv_display_flush_is_last(d))
    panel->draw16bitRGBBitmap(0, 0, frame, width, height);
  flushMs += millis() - started;
  ++flushes;
  flushPixels += uint32_t(area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
  lv_display_flush_ready(d);
}
void onDisplay(lv_event_t* e) {
  switch (lv_event_get_code(e)) {
  case LV_EVENT_REFR_START:
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
} // namespace

bool begin(Arduino_GFX& output, uint16_t* canvas, WaveshareShell& model, uint32_t) {
  panel = &output;
  frame = canvas;
  shell = &model;
  if (display) {
    // Peripheral recovery reset the panel; repaint from the existing widgets.
    lv_obj_invalidate(lv_display_get_screen_active(display));
    return true;
  }
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
  lv_display_set_buffers(display, frame, nullptr, frameBytes, LV_DISPLAY_RENDER_MODE_DIRECT);
  indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, readTouch);
  // Event mode: the adapter's 30 ms touch poll drives lv_indev_read directly,
  // so a sample is never left waiting for a second timer.
  lv_indev_set_mode(indev, LV_INDEV_MODE_EVENT);
  sample = {};
  Serial.println("[lvgl] init stage=screens");
  for (auto& entry : screens) {
    entry.object = screen();
    entry.build(entry.object);
  }
  lv_screen_load(screens[unsigned(shell->active())].object);
  syncView();
  Serial.printf("[lvgl] init stage=loaded stack-free=%u\n",
                unsigned(uxTaskGetStackHighWaterMark(nullptr)));
  Serial.printf("[lvgl] init version=%d.%d.%d mode=direct canvas=%u bytes heap-before=%lu "
                "heap-after=%lu psram-before=%lu psram-after=%lu\n",
                LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, unsigned(frameBytes),
                heap, ESP.getFreeHeap(), psram, ESP.getFreePsram());
  return true;
}
void touch(int x, int y, bool pressed, uint32_t now) {
  if (!indev)
    return;
  // A release keeps the last finger position, as touch controllers report it:
  // LVGL finishes the gesture (slider value, click target) at that point.
  if (!pressed) {
    x = sample.x;
    y = sample.y;
  }
  if (pressed != sample.pressed)
    Serial.printf("[lvgl] touch %s x=%d y=%d\n", pressed ? "down" : "up", x, y);
  sample = {x, y, pressed};
  if (pressed)
    pendingSampleAt = now;
  lv_indev_read(indev);
}
void cancelTouch(uint32_t) {
  if (!indev)
    return;
  const bool wasPressed = sample.pressed;
  sample.pressed = false;
  if (wasPressed) {
    // Drop the active object first so the release reaches no widget, then
    // ignore the finger until it is actually lifted.
    lv_indev_reset(indev, nullptr);
    lv_indev_wait_release(indev);
  }
  lv_indev_read(indev);
}
void service(uint32_t now, uint32_t& pollGapMax) {
  if (stopped || paused || !display)
    return;
  pollGap = &pollGapMax;
  handlerStart = now;
  lv_timer_handler();
  pollGap = nullptr;
  static uint32_t lastStats = 0;
  if (now - lastStats >= 5000) {
    lastStats = now;
    Serial.printf("[lvgl] stats frames=%lu render-ms=%lu flush-ms=%lu px=%lu touch-frames=%lu "
                  "touch-avg=%lu touch-max=%lu screen=%s heap=%lu psram-free=%lu stack-free=%u\n",
                  totals.frames, totals.renderMs, totals.flushMs, totals.pixels, totals.touchFrames,
                  totals.touchFrames ? totals.touchMs / totals.touchFrames : 0, totals.touchMaxMs,
                  surfaceScreenName(shell->active()), ESP.getFreeHeap(), ESP.getFreePsram(),
                  unsigned(uxTaskGetStackHighWaterMark(nullptr)));
    totals = {};
  }
}
void render(const uint16_t* artwork, bool artworkChanged, uint32_t) {
  if (!display)
    return;
  if (artwork != artworkShown || artworkChanged) {
    artworkShown = artwork;
    if (artwork) {
      artworkDescriptor.data = reinterpret_cast<const uint8_t*>(artwork);
      lv_image_set_src(artworkImage, &artworkDescriptor);
      lv_obj_invalidate(artworkImage);
    } else
      lv_image_set_src(artworkImage, nullptr);
    setHidden(artworkImage, !artwork);
    setHidden(placeholder, artwork != nullptr);
  }
  auto& ui = shell->nowPlaying;
  if (!ui.dirty)
    return;
  ui.dirty = false;
  syncView();
  syncNowPlaying();
  syncRooms();
  syncQueue();
  const auto& o = ui.state.observed;
  Serial.printf("[ui] frame screen=%s view=%s room=%s title=%s transport=%s volume=%d "
                "position=%lu duration=%lu seek=%d queue-start=%lu readonly=%d busy=%d heap=%lu "
                "psram-free=%lu\n",
                surfaceScreenName(shell->active()), waveshareScreenName(ui.screen), o.room.c_str(),
                o.title.c_str(), playbackLabel(o.transport), o.volume.value_or(-1),
                o.positionMs.value_or(0), o.durationMs.value_or(0), ui.canSeek(), ui.queueStart,
                ui.context.readOnly, ui.context.busy, ESP.getFreeHeap(), ESP.getFreePsram());
}
uint32_t load() {
  if (!display)
    return 0;
  // A press started on the old screen must not release on the new one.
  cancelTouch(millis());
  const auto started = millis();
  auto& entry = screens[unsigned(shell->active())];
  lv_screen_load(entry.object);
  if (entry.enter)
    entry.enter();
  return millis() - started;
}
void syncView() {
  if (!display)
    return;
  const auto view = shell->nowPlaying.screen;
  setHidden(viewNow, view != WaveshareScreen::NowPlaying);
  setHidden(rooms.view, view != WaveshareScreen::Rooms);
  setHidden(queue.view, view != WaveshareScreen::Queue);
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
  auto state = waveshareStateJson(*shell, held, injectPending, injectOpen);
  state["lvgl"] = true;
  state["slider"] = playSlider ? int(lv_slider_get_value(playSlider)) : -1;
  auto& counts = state["clicks"];
  counts["large"] = clicks[0];
  counts["medium"] = clicks[1];
  counts["small"] = clicks[2];
  auto& touch = state["touch"];
  touch["pressed"] = sample.pressed;
  touch["x"] = sample.x;
  touch["y"] = sample.y;
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
void pause(bool value) {
  if (paused == value)
    return;
  paused = value;
  if (!paused && display)
    lv_obj_invalidate(lv_display_get_screen_active(display));
}
void stop() { stopped = true; }
} // namespace surface::device::lvgl
#endif
