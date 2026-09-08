#if defined(SURFACE_STICK_S3)
#include "SurfaceDevice.h"
#include <M5Unified.h>
#include <M5UnitUnified.h>
#include <M5UnitUnifiedNFC.h>
#include "StickDisplay.h"
#include "StickButtons.h"
#include <Wire.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <driver/gpio.h>

namespace surface::device {
namespace {
m5::unit::UnitUnified units;
m5::unit::UnitNFC unit;
m5::nfc::NFCLayerA nfc{unit};
StickDisplay display;
bool displayReady = false;
bool registered = false;
bool ready = false;
uint32_t lastInit = 0;
bool latched = false;
unsigned misses = 0;
uint32_t lastPoll = 0;
std::string lastScreen;
LocalActivity activity = LocalActivity::None;
bool releaseAfterBoot = true;
} // namespace
bool boardBegin(std::string& notice) {
  rtc_gpio_deinit(GPIO_NUM_11);
  gpio_deep_sleep_hold_dis();
  for (int pin : {9, 10, 21, 38, 39, 40, 41, 45})
    gpio_hold_dis(static_cast<gpio_num_t>(pin));
  displayReady = display.beginFixed();
  M5.Display = display;
  auto cfg = M5.config();
  cfg.fallback_board = m5::board_t::board_M5StickS3;
  cfg.internal_imu = cfg.internal_rtc = cfg.internal_mic = cfg.internal_spk = false;
  cfg.external_display_value = 0;
  M5.begin(cfg);
  M5.Display.setRotation(1);
  M5.Display.setTextSize(1);
  M5.Display.setBrightness(100);
  displayReady = displayReady && M5.getBoard() == m5::board_t::board_M5StickS3 &&
                 M5.Display.width() == 240 && M5.Display.height() == 135;
  M5.Power.setExtOutput(true);
  const bool power = M5.Power.getExtOutput();
  const bool wire = Wire.begin(9, 10, 100000);
  Wire.setTimeOut(50);
  auto unitConfig = unit.component_config();
  unitConfig.clock = 100000;
  unit.component_config(unitConfig);
  registered = wire && units.add(unit, Wire);
  ready = registered && units.begin();
  lastInit = millis();
  Serial.printf("[board] M5 id=%d display=%ldx%ld display-ready=%d Grove power=%d SDA=9 SCL=10 "
                "I2C=%d NFC=0x50 ready=%d\n",
                M5.getBoard(), M5.Display.width(), M5.Display.height(), displayReady, power, wire,
                ready);
  notice = !displayReady ? "Stick display init FAILED (see serial)"
           : ready       ? "NFC ready: tap card"
                         : "NFC init FAILED: check Grove cable; retrying";
  Serial.println(notice.c_str());
  return displayReady && ready;
}
bool boardCommand(const std::string&) { return false; }

static BoardEvent pollInput() {
  static uint32_t lastButtons = 0, buttonReport = 0, maxButtonGap = 0;
  const auto now = millis();
  const auto gap = lastButtons ? now - lastButtons : 0;
  maxButtonGap = std::max<uint32_t>(maxButtonGap, gap);
  lastButtons = now;
  M5.update();
  if (M5.BtnA.isPressed() || M5.BtnB.isPressed())
    activity = LocalActivity::Button;
  // A wake press is consumed by boot, not turned into a refresh/toggle. Wait
  // for release and for the M5 click decision window to expire before actions.
  if (releaseAfterBoot) {
    if (!stickButtonPending(M5.BtnA, M5.BtnB))
      releaseAfterBoot = false;
    return {};
  }
  if (M5.BtnA.wasChangePressed())
    Serial.printf("[button] A %s clicks=%u poll-gap-ms=%lu\n",
                  M5.BtnA.isPressed() ? "pressed" : "released", M5.BtnA.getClickCount(), gap);
  if (M5.BtnA.wasDecideClickCount())
    Serial.printf("[button] A decided clicks=%u\n", M5.BtnA.getClickCount());
  if (now - buttonReport >= 5000) {
    Serial.printf("[button] max-poll-gap-ms=%lu\n", maxButtonGap);
    maxButtonGap = 0;
    buttonReport = now;
  }
  static uint32_t lastDisplayInit = 0;
  if (!displayReady && millis() - lastDisplayInit >= 5000) {
    lastDisplayInit = millis();
    displayReady = display.beginFixed();
    M5.Display = display;
    M5.Display.setRotation(1);
    M5.Display.setTextSize(1);
    M5.Display.setBrightness(100);
    lastScreen.clear();
    Serial.printf("[display] fixed Stick retry ready=%d\n", displayReady);
  }
  if (!ready && millis() - lastInit >= 5000) {
    lastInit = millis();
    M5.Power.setExtOutput(true);
    if (!registered) {
      if (Wire.begin(9, 10, 100000)) {
        Wire.setTimeOut(50);
        registered = units.add(unit, Wire);
      }
    }
    ready = registered && units.begin();
    Serial.printf("[nfc] init retry ready=%d\n", ready);
    if (ready)
      return {Input::Error, "NFC recovered: tap card"};
  }
  const auto button = stickButtonInput(M5.BtnA, M5.BtnB);
  if (button != Input::None)
    return {button, ""};
  // Keep sampling an in-progress gesture; resume NFC without resetting its
  // presentation latch once the click count is decided or the hold is released.
  if (stickButtonPending(M5.BtnA, M5.BtnB))
    return {};
  if (!ready || millis() - lastPoll < 200)
    return {};
  lastPoll = millis();
  units.update();
  m5::nfc::a::PICC picc;
  // WUPA includes halted tags. REQA-only polling would mistake a held, halted
  // card for removal and could submit it repeatedly.
  if (!nfc.wakeup(picc.atqa)) {
    if (latched && ++misses >= 3) {
      latched = false;
      Serial.println("[nfc] removed; ready for next presentation");
    }
    return {};
  }
  misses = 0;
  if (!nfc.select(picc))
    return {};
  if (latched) {
    nfc.deactivate();
    return {};
  }
  latched = true;
  activity = LocalActivity::Nfc;
  Serial.printf("[nfc] detected uid=%s\n", picc.uidAsString().c_str());
  // identify() itself reactivates/probes and then halts the card. Keep the
  // required post-identification reactivation, but distinguish its failure
  // from failure inside identify(); the old combined message hid that evidence.
  auto phaseStarted = millis();
  const bool identified = nfc.identify(picc);
  Serial.printf("[nfc] phase=identify ok=%d ms=%lu\n", identified, millis() - phaseStarted);
  if (!identified) {
    nfc.deactivate();
    return {Input::Error, "NFC identify failed; remove and retap"};
  }
  phaseStarted = millis();
  const bool reactivated = nfc.reactivate(picc);
  Serial.printf("[nfc] phase=reactivate ok=%d ms=%lu\n", reactivated, millis() - phaseStarted);
  if (!reactivated) {
    nfc.deactivate();
    return {Input::Error, "NFC reactivate failed; remove and retap"};
  }
  Serial.printf("[nfc] type=%s user-bytes=%u\n", picc.typeAsString().c_str(), picc.userAreaSize());
  if (!picc.supportsNDEF()) {
    nfc.deactivate();
    return {Input::Error, "NFC-A tag does not support NDEF"};
  }
  bool valid = false;
  m5::nfc::ndef::TLV message;
  const auto start = millis();
  bool read = nfc.ndefIsValidFormat(valid) && valid && nfc.ndefRead(message);
  // A bounded, read-only view of the first user pages distinguishes an empty
  // on-card message from a vendor decoder problem. Never format/write the tag.
  if (picc.isNTAG2()) {
    uint8_t bytes[16]{};
    const bool rawRead = nfc.read16(bytes, 4);
    Serial.printf("[nfc] raw-page4 read=%d bytes=", rawRead);
    if (rawRead)
      for (auto b : bytes)
        Serial.printf("%02x ", b);
    Serial.println();
  }
  nfc.deactivate();
  Serial.printf("[nfc] read=%d valid=%d ms=%lu\n", read, valid, millis() - start);
  if (!read || !message.isMessageTLV())
    return {Input::Error, "No readable NDEF message"};
  const auto& records = message.records();
  if (records.size() != 1)
    return {Input::Error, "Expected exactly one NFC record"};
  const auto& record = records.front();
  Serial.printf("[nfc] TNF=%u type=%s payload-bytes=%lu\n", unsigned(record.tnf()), record.type(),
                record.payloadSize());
  const std::string type = record.type();
  std::string payload;
  if (type == "U" && record.payloadSize())
    Serial.printf("[nfc] URI prefix=0x%02x\n", record.payload()[0]);
  auto result = decodeNdefRecord(uint8_t(record.tnf()), type, record.payload(),
                                 record.payloadSize(), payload);
  if (!result.ok)
    return {Input::Error, result.error};
  if (type.empty())
    Serial.println("[nfc] raw URL (empty type)");
  Serial.printf("[nfc] decoded payload-bytes=%u (accepted fields logged by core)\n",
                unsigned(payload.size()));
  return {Input::Payload, payload};
}
BoardEvent boardPoll() {
  activity = LocalActivity::None;
  auto event = pollInput();
  event.activity = activity;
  return event;
}
void boardPrepareSleep() {
  M5.Speaker.end();
  M5.Mic.end();
  if (displayReady) {
    M5.Display.setBrightness(0);
    M5.Display.sleep();
  }
  // PM1 rails are independent: L3B LCD/codec/mic, L1 IMU, Grove NFC/IR.
  // The amplifier is on L0, so explicitly keep its SHDN low as well.
  auto& pm = M5.Power.M5pm1;
  bool ok = pm.setGPIOOutput(m5::M5PM1_Class::gpio3, false);
  ok = pm.setGPIOFunction(m5::M5PM1_Class::gpio3, m5::M5PM1_Class::gpio) && ok;
  ok = pm.setGPIOMode(m5::M5PM1_Class::gpio3, m5::M5PM1_Class::output) && ok;
  ok = pm.setGPIOOutput(m5::M5PM1_Class::gpio2, false) && ok;
  // BMI270 shares the retained PM1 bus without the codec's I2C isolation.
  // Keep L1/IO supply on and suspend all sensors to avoid back-powering it
  // through the L2 I2C pull-ups (schematic sheet 3).
  ok = pm.setLDOOutput(true) && ok;
  ok = M5.In_I2C.writeRegister8(0x68, 0x7d, 0, 100000) && ok;
  delayMicroseconds(500); // BMI270 advanced-power-save inter-write requirement.
  ok = M5.In_I2C.writeRegister8(0x68, 0x7c, 1, 100000) && ok;
  Wire.end();
  ok = pm.setExtOutput(false) && ok;
  ok = pm.setLedEnLevel(false) && ok;
  // Clear PM1 timer/watchdog and external wake settings left across ESP resets.
  ok = M5.In_I2C.writeRegister8(0x6e, 0x0a, 0, 100000) && ok;
  ok = M5.In_I2C.writeRegister8(0x6e, 0x18, 0, 100000) && ok;
  ok = M5.In_I2C.writeRegister8(0x6e, 0x3c, 0, 100000) && ok;
  // Prevent SPI/I2C pins back-powering switched-off peripherals during deep sleep.
  for (int pin : {9, 10, 21, 38, 39, 40, 41, 45}) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
    gpio_hold_en(static_cast<gpio_num_t>(pin));
  }
  gpio_deep_sleep_hold_en();
  Serial.printf("[power] Stick peripherals-off=%d; deep sleep; wake=A GPIO11 low\n", ok);
}
[[noreturn]] void boardSleep() {
  // A has a 10k pull-up to retained L2. Do not use the PM1 IRQ: it also
  // aggregates charging/IMU events. PM1 power-off admits VIN insertion wake.
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(GPIO_NUM_11, 0));
  rtc_gpio_pullup_en(GPIO_NUM_11);
  rtc_gpio_pulldown_dis(GPIO_NUM_11);
  esp_deep_sleep_start();
}
void boardRender(const AppState& state, const std::string& notice) {
  if (!displayReady)
    return;
  const auto& observed = state.observed;
  const auto detail = state.refreshError.empty() ? state.detail : "Refresh: " + state.refreshError;
  auto screen = std::string("sonos-surface / NFC\n") + notice + "\n" +
                (observed.known ? observed.room + ": " + observed.playback : "Playback: unknown") +
                (observed.stale ? " [stale]" : "") + "\n" + observed.title + "\n" + state.status +
                ": " + detail + "\nA: refresh  B: play/pause";
  if (screen == lastScreen)
    return;
  lastScreen = screen;
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setCursor(0, 0);
  // Bound each section to keep result and current-state visible on a 240x135 UI.
  M5.Display.println(observed.room.empty() ? "Discovering rooms..."
                                           : observed.room.substr(0, 38).c_str());
  M5.Display.println(notice.substr(0, 76).c_str());
  M5.Display.printf("%s%s\n", observed.known ? observed.playback.c_str() : "Unknown playback",
                    observed.stale ? " [stale]" : "");
  M5.Display.println(observed.title.substr(0, 70).c_str());
  M5.Display.println(state.status.c_str());
  M5.Display.println(detail.substr(0, 100).c_str());
  M5.Display.setCursor(0, 124);
  M5.Display.println("A:refresh AA:room B:play/pause");
}
} // namespace surface::device
#endif
