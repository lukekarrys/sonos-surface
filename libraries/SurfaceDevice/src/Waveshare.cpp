#if defined(SURFACE_WAVESHARE)
#include "SurfaceDevice.h"
#include "TouchCoordinates.h"
#include "WaveshareDrawing.h"
#include "WaveshareArtwork.h"
#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <Preferences.h>

namespace surface::device {
namespace {
// The pinned QSPI driver cannot begin twice (it aborts on an installed SPI bus).
// Reuse its successful bus while replaying panel reset/init after touch failures.
class DisplayBus : public Arduino_ESP32QSPI {
  bool attempted = false, initialized = false;
public:
  DisplayBus() : Arduino_ESP32QSPI(12, 11, 4, 5, 6, 7) {}
  bool begin(int32_t speed = GFX_NOT_DEFINED, int8_t mode = GFX_NOT_DEFINED) override {
    if (!attempted) {
      attempted = true;
      initialized = Arduino_ESP32QSPI::begin(speed, mode);
    }
    return initialized;
  }
} bus;
Arduino_OLED* panel = nullptr;
// CO5300 direct one-pixel windows can omit lines/circles. Compose all graphics
// in PSRAM and transmit an aligned, full-screen image instead (GFX issue #780).
class ScreenCanvas : public Arduino_Canvas {
public:
  explicit ScreenCanvas(Arduino_OLED* output) : Arduino_Canvas(368, 448, output) {}
  bool begin(int32_t = GFX_NOT_DEFINED) override {
    if (!_framebuffer)
      _framebuffer = static_cast<uint16_t*>(heap_caps_aligned_alloc(
          16, 368 * 448 * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return _framebuffer != nullptr;
  }
};
ScreenCanvas* gfx = nullptr;
uint8_t touchAddress = 0;
bool ready = false;
bool displayReady = false;
uint32_t lastInit = 0;
TouchCalibration calibration;
bool calibrationStored = false;
void loadCalibration() {
  Preferences prefs;
  calibration = {};
  calibrationStored = false;
  if (prefs.begin("surface", true)) {
    auto text = prefs.getString("touch", "");
    TouchCalibration next;
    calibrationStored = parseCalibration(text.c_str(), next) && next.controller == touchAddress;
    if (calibrationStored) calibration = next;
    prefs.end();
  }
  Serial.printf("[touch] calibration=%s (diagnostics raw)\n",
                calibrationStored ? calibrationJson(calibration).c_str() : "identity fallback: missing/invalid/controller mismatch");
}
bool held = false;
uint32_t lastTouch = 0, maxPollGap = 0;
bool loggingContact = false;
std::string lastScreen;
WaveshareUi ui;
BoardContext uiContext;
int edgeInset = -1; // Temporary serial-controlled display diagnostic; never persisted.
int edgeRadius = 0;
struct Button { int x, y; const char* title; Input input; };
// Owner observed clipped title/bottom corners at the previous 12px margin.
// Conservative layout inset for the visible panel; not a touch transform or
// a measured universal corner radius. Drawing and hit testing share dimensions.
constexpr int buttonWidth = 140, buttonHeight = 60;
constexpr Button buttons[] = {
  {196, 284, "Refresh", Input::Refresh},
  {32, 356, "Play", Input::Play}, {196, 356, "Pause", Input::Pause}
};
const Button* buttonAt(int x, int y) {
  for (const auto& b : buttons)
    if (x >= b.x && x < b.x + buttonWidth && y >= b.y && y < b.y + buttonHeight) return &b;
  return nullptr;
}
bool probe(uint8_t address) {
  Wire.beginTransmission(address); return Wire.endTransmission() == 0;
}
bool writeRegister(uint8_t address, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(address); Wire.write(reg); Wire.write(value);
  return Wire.endTransmission() == 0;
}
#if SURFACE_TOUCH_DIAGNOSTIC
constexpr int calibrationTargets[][2] = {
  {92, 140}, {276, 140}, {92, 270}, {276, 270}, {92, 406}, {276, 406}
};
unsigned calibrationTarget = 0;
bool calibrationArmed = false, calibrationContact = false, calibrationMoved = false;
int firstX = 0, firstY = 0, contactX = 0, contactY = 0;
bool calibrationRetry = false;
void collectCalibration(int x, int y, int fingers) {
  if (calibrationTarget >= 6) return;
  // Ignore a finger already held during boot; require a release before starting.
  if (!calibrationArmed) { if (!fingers) calibrationArmed = true; return; }
  if (fingers) {
    if (!calibrationContact) {
      calibrationContact = true; calibrationMoved = false;
      firstX = x; firstY = y;
    }
    if (fingers != 1 || abs(x - firstX) > 16 || abs(y - firstY) > 16)
      calibrationMoved = true;
    contactX = x; contactY = y;
  } else if (calibrationContact) {
    calibrationContact = false;
    calibrationRetry = calibrationMoved;
    if (calibrationMoved) {
      Serial.printf("[calibration] target=%u retry: tap without dragging\n", calibrationTarget + 1);
      return;
    }
    const auto& t = calibrationTargets[calibrationTarget];
    Serial.printf("[calibration] target=%u expected=%d,%d first=%d,%d last=%d,%d release=%d,%d\n",
                  calibrationTarget + 1, t[0], t[1], firstX, firstY, contactX, contactY, x, y);
    ++calibrationTarget;
    if (calibrationTarget == 6)
      Serial.println("[calibration] COMPLETE: six samples recorded; no correction applied");
  }
}
void diagnosticScreen(int x, int y, int fingers, int event) {
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(12, 10); gfx->println("Touch test / read-only");
  gfx->setCursor(12, 36); gfx->printf("x=%d y=%d fingers=%d", x, y, fingers);
  const auto* hit = buttonAt(x, y);
  gfx->setCursor(12, 62); gfx->printf("hit=%s event=%d", hit ? hit->title : "none", event);
  gfx->setCursor(12, 90);
  if (calibrationTarget < 6) {
    gfx->printf("%s %u/6", calibrationRetry ? "Retry target" : "Tap + center", calibrationTarget + 1);
    const auto& t = calibrationTargets[calibrationTarget];
    gfx->drawCircle(t[0], t[1], 16, RGB565_WHITE);
    gfx->drawFastHLine(t[0] - 10, t[1], 21, RGB565_WHITE);
    gfx->drawFastVLine(t[0], t[1] - 10, 21, RGB565_WHITE);
  } else {
    gfx->println("Done - samples recorded");
  }
  // Show the live marker while touching, but leave the next white target clear.
  if ((fingers || calibrationTarget == 6) && x >= 0 && x < 368 && y >= 0 && y < 448) {
    gfx->drawCircle(x, y, 7, RGB565_YELLOW);
    gfx->drawFastHLine(x - 10, y, 21, RGB565_YELLOW);
    gfx->drawFastVLine(x, y - 10, 21, RGB565_YELLOW);
  }
  auto started = millis();
  gfx->flush();
  Serial.printf("[display] diagnostic flush=%lu ms cursor=%s\n", millis() - started,
                x >= 0 && x < 368 && y >= 0 && y < 448 &&
                gfx->getFramebuffer()[y * 368 + x] == RGB565_YELLOW ? "yellow-in-buffer" : "none");
}
#endif
} // namespace
bool boardBegin(std::string& notice) {
  lastInit = millis();
  displayReady = false;
  // Retry only a failed adapter, never reboot the networking/Sonos runtime.
  Wire.end();
  if (!Wire.begin(15, 14, 100000)) {
    notice = "Waveshare I2C begin failed"; Serial.println(notice.c_str()); return false;
  }
  Wire.setTimeOut(50);
  // Vendor reset sequence: XCA9554 outputs 0..2 drive peripheral resets.
  // This 20ms electrical reset pulse is unrelated to Sonos synchronization.
  if (!probe(0x20) || !writeRegister(0x20, 0x01, 0x00) || !writeRegister(0x20, 0x03, 0xF8)) {
    notice = "Waveshare expander 0x20 missing"; Serial.println(notice.c_str()); return false;
  }
  delay(20);
  if (!writeRegister(0x20, 0x01, 0x07)) {
    notice = "Waveshare reset release failed"; Serial.println(notice.c_str()); return false;
  }
  // Revision choice is a board-only concern. V1 FT3168=0x38; V2 CST820=0x15.
  touchAddress = 0; // A reset invalidates previous readiness; probe afresh on every retry.
  auto deadline = millis() + 1000;
  do {
    if (probe(0x38)) touchAddress = 0x38;
    else if (probe(0x15)) touchAddress = 0x15;
    else delay(10); // Bounded peripheral-ready polling after reset.
  } while (!touchAddress && millis() < deadline);
  if (!touchAddress) { notice = "No FT3168/CST820 touch response"; Serial.println(notice.c_str()); return false; }
  if (touchAddress == 0x38) {
    if (!panel) panel = new Arduino_SH8601(&bus, GFX_NOT_DEFINED, 0, 368, 448);
    if (!writeRegister(touchAddress, 0xA5, 1)) { notice = "FT3168 mode failed"; Serial.println(notice.c_str()); return false; } // Vendor monitor power mode.
    notice = "Waveshare V1 SH8601/FT3168";
  } else {
    if (!panel) panel = new Arduino_CO5300(&bus, GFX_NOT_DEFINED, 0, 368, 448, 16, 0, 0, 0);
    if (!writeRegister(touchAddress, 0xFA, 0x40)) { notice = "CST820 mode failed"; Serial.println(notice.c_str()); return false; } // Vendor periodic touch interrupts.
    notice = "Waveshare V2 CO5300/CST820";
  }
  if (!panel->begin()) { notice = "AMOLED begin failed"; Serial.println(notice.c_str()); return false; }
  if (!gfx) gfx = new ScreenCanvas(panel);
  if (!gfx->begin()) { notice = "AMOLED PSRAM canvas failed"; Serial.println(notice.c_str()); return false; }
  displayReady = true;
  panel->setBrightness(140);
  gfx->setTextSize(2);
  Serial.println("[display] PSRAM canvas=329728 bytes; full-frame flush");
  Serial.println(notice.c_str());
  loadCalibration();
  Serial.printf("[board] display=%dx%d touch=0x%02x SDA=15 SCL=14\n", gfx->width(), gfx->height(), touchAddress);
  ready = true;
  ui.cancelTouch(); held = true; // Require a release after boot/recovery/calibration changes.
  lastScreen.clear();
#if SURFACE_TOUCH_DIAGNOSTIC
  diagnosticScreen(-1, -1, 0, 0);
  Serial.println("[touch] DIAGNOSTIC: coordinates only; touch actions disabled");
#endif
  return true;
}
bool boardCommand(const std::string& line) {
#if !SURFACE_TOUCH_DIAGNOSTIC
  // Read-only navigation aid for serial layout/performance inspection. It emits
  // no intent and does not inject physical touch samples.
  if (line == "ui-screen now" || line == "ui-screen rooms" || line == "ui-screen queue") {
    ui.cancelTouch(); held = true;
    ui.screen = line == "ui-screen now" ? WaveshareScreen::NowPlaying :
      line == "ui-screen rooms" ? WaveshareScreen::Rooms : WaveshareScreen::Queue;
    Serial.printf("[ui] navigation screen=%d (no playback intent)\n",int(ui.screen));
    return true;
  }
#endif
  if (line.compare(0, 13, "display-edge ") == 0) {
    const auto value = line.substr(13);
    if (value == "off") edgeInset = -1;
    else {
      const auto separator = value.find(' ');
      const auto inset = value.substr(0, separator);
      const auto radius = separator == std::string::npos ? "0" : value.substr(separator + 1);
      const auto validNumber = [](const std::string& token, int maximum) {
        return !token.empty() && token.size() <= 3 &&
               token.find_first_not_of("0123456789") == std::string::npos && std::stoi(token) <= maximum;
      };
      if (!validNumber(inset, 64) || !validNumber(radius, 120)) {
        Serial.println("DISPLAY_EDGE_INVALID: use inset 0..64 [radius 0..120], or off"); return true;
      }
      edgeInset = std::stoi(inset);
      edgeRadius = std::stoi(radius);
    }
    ui.cancelTouch(); held = true; lastScreen.clear();
    Serial.printf("DISPLAY_EDGE inset=%d radius=%d (touch actions disabled while visible)\n", edgeInset, edgeRadius);
    return true;
  }
  if (line == "peripherals-retry") {
    ready = false; ui.cancelTouch(); held = true; lastInit = millis() - 5000;
    Serial.println("[board] peripheral retry requested; networking continues");
    return true;
  }
  if (line == "touch-calibration") {
    Serial.printf("CALIBRATION %s\n", calibrationStored ? calibrationJson(calibration).c_str() : "identity");
    return true;
  }
  if (line.compare(0, 18, "touch-calibration ") != 0) return false;
  TouchCalibration next;
  const auto text = line.substr(18);
  if (!ready || !parseCalibration(text, next) || next.controller != touchAddress) {
    Serial.println("CALIBRATION_INVALID (adapter must be ready; valid matching controller required)"); return true;
  }
  Preferences prefs;
  if (!prefs.begin("surface", false)) { Serial.println("CALIBRATION_SAVE_FAILED"); return true; }
  const auto saved = prefs.putString("touch", calibrationJson(next).c_str());
  prefs.end();
  if (!saved) { Serial.println("CALIBRATION_SAVE_FAILED"); return true; }
  calibration = next; calibrationStored = true; ui.cancelTouch(); held = true;
  Serial.printf("CALIBRATION_SAVED %s\n", calibrationJson(calibration).c_str());
  return true;
}
BoardEvent boardPoll() {
  static uint32_t lastPoll = 0;
  const auto now = millis();
  if (lastPoll) maxPollGap = std::max(maxPollGap, uint32_t(now-lastPoll));
  lastPoll = now;
  if (!ready && millis() - lastInit >= 5000) {
    std::string notice;
    const bool recovered = boardBegin(notice);
    Serial.printf("[board] retry ready=%d %s\n", recovered, notice.c_str());
    return {Input::Error, recovered ? "Waveshare peripherals recovered" : notice};
  }
  if (!ready || millis() - lastTouch < 30) return {};
  lastTouch = millis();
  if (edgeInset >= 0) { ui.cancelTouch(); held = true; return {}; }
  // Both vendor drivers read finger count at 0x02 and XY at 0x03..0x06.
  Wire.beginTransmission(touchAddress); Wire.write(0x02);
  static uint32_t lastError = 0;
  static unsigned readErrors = 0;
  if (Wire.endTransmission(false) || Wire.requestFrom(touchAddress, uint8_t(5)) != 5) {
    if (millis() - lastError >= 5000) { Serial.println("[touch] I2C read failed"); lastError = millis(); }
    ui.cancelTouch(); held = true;
#if SURFACE_TOUCH_DIAGNOSTIC
    calibrationContact = false; calibrationArmed = false;
#endif
    if (++readErrors >= 10) {
      ready = false; lastInit = millis(); readErrors = 0;
      return {Input::Error, "Touch unavailable; peripheral reset pending"};
    }
    return {};
  }
  readErrors = 0;
  uint8_t fingers = Wire.read() & 0x0F;
  uint8_t xh = Wire.read(), xl = Wire.read(), yh = Wire.read(), yl = Wire.read();
  static bool loggedRead = false;
  if (!loggedRead) {
    Serial.printf("[touch] first register read OK fingers=%u xy-bytes=%02x %02x %02x %02x\n", fingers, xh, xl, yh, yl);
    loggedRead = true;
  }
  int x = ((xh & 0x0F) << 8) | xl, y = ((yh & 0x0F) << 8) | yl;
#if SURFACE_TOUCH_DIAGNOSTIC
  collectCalibration(x, y, fingers);
  static int previousX = -1, previousY = -1, previousFingers = -1;
  if (fingers != previousFingers || (fingers && (x != previousX || y != previousY))) {
    const auto* hit = buttonAt(x, y);
    Serial.printf("[touch] raw=%02x %02x %02x %02x x=%d y=%d fingers=%d event=%d hit=%s\n",
                  xh, xl, yh, yl, x, y, fingers, xh >> 6, hit ? hit->title : "none");
    diagnosticScreen(x, y, fingers, xh >> 6);
    previousX = x; previousY = y; previousFingers = fingers;
  }
  return {};
#endif
  if (!fingers) {
    if (loggingContact) Serial.printf("[touch] release raw=%d,%d\n",x,y);
    loggingContact = false;
    held = false;
    auto event = ui.touch(0, 0, 0, millis());
    if (event.input != Input::None) {
      Serial.printf("[ui] release action=%d target=%s offset=%lu\n", int(event.input), event.targetId.c_str(), event.start);
      return event;
    }
    return ui.requestQueue();
  }
  if (held) return {};
  const auto point = waveshareTouchPoint(x, y, calibration);
  // Every normal gesture sample uses the saved fit; release uses its last preview.
  static int loggedX = 0, loggedY = 0;
  if (!loggingContact || abs(x-loggedX) >= 8 || abs(y-loggedY) >= 8) {
    Serial.printf("[touch] raw=%d,%d mapped=%d,%d fingers=%d hit=%d\n",
                  x, y, point.x, point.y, fingers, int(ui.hit(point.x, point.y)));
    loggedX = x; loggedY = y;
  }
  loggingContact = true;
  return ui.touch(point.x, point.y, fingers, millis());
}
void boardContext(const BoardContext& context) { uiContext = context; }

void boardRender(const AppState& state, const std::string& notice) {
  if (!displayReady) return;
  if (edgeInset >= 0) {
    const auto screen = "edge:" + std::to_string(edgeInset) + ":" + std::to_string(edgeRadius);
    if (screen == lastScreen) return;
    lastScreen = screen;
    // Illuminate the full addressable panel to distinguish its visible mask
    // from the inset outline. Keep contrast with the one-pixel white border.
    gfx->fillScreen(0x0010); // Dark blue (RGB565).
    if (edgeRadius)
      gfx->drawRoundRect(edgeInset, edgeInset, 368 - 2 * edgeInset, 448 - 2 * edgeInset, edgeRadius, RGB565_WHITE);
    else
      gfx->drawRect(edgeInset, edgeInset, 368 - 2 * edgeInset, 448 - 2 * edgeInset, RGB565_WHITE);
    gfx->setTextColor(RGB565_WHITE);
    gfx->setCursor(80, 190); gfx->printf("Edge inset: %d px", edgeInset);
    gfx->setCursor(80, 220); gfx->printf("Radius: %d px", edgeRadius);
    gfx->setCursor(80, 250); gfx->print("Touch disabled");
    gfx->flush();
    Serial.printf("[display] edge rendered inset=%d radius=%d x=%d..%d y=%d..%d\n",
                  edgeInset, edgeRadius, edgeInset, 367 - edgeInset, edgeInset, 447 - edgeInset);
    return;
  }
#if SURFACE_TOUCH_DIAGNOSTIC
  (void)state; (void)notice;
  return;
#else
  ui.update(state, uiContext, millis());
  if (artworkUpdate(state.observed, uiContext.online, millis())) ui.dirty = true;
  if (!ui.dirty) return;
  ui.dirty = false;
  const auto started = millis();
  WaveshareDrawing drawing(*gfx);
  drawing.draw(ui, artworkPixels());
  const auto drawn = millis();
  gfx->flush();
  Serial.printf("[ui] frame screen=%d room=%s title=%s transport=%s volume=%d position=%lu duration=%lu seek=%d queue-start=%lu readonly=%d busy=%d draw=%lu flush=%lu total=%lu poll-gap-max=%lu heap=%u psram-free=%u\n",
    int(ui.screen), state.observed.room.c_str(), state.observed.title.c_str(), playbackLabel(state.observed.transport),
    state.observed.volume.value_or(-1), state.observed.positionMs.value_or(0), state.observed.durationMs.value_or(0),
    ui.canSeek(), ui.queueStart, uiContext.readOnly, uiContext.busy, drawn-started, millis()-drawn, millis()-started,
    maxPollGap, ESP.getFreeHeap(), ESP.getFreePsram());
  maxPollGap = 0;
  (void)notice;
#endif
}
} // namespace surface::device
#endif
