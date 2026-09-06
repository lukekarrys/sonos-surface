#if defined(SURFACE_STICK)
#include "SurfaceDevice.h"
#include <M5Unified.h>
#include <M5UnitUnified.h>
#include <M5UnitUnifiedNFC.h>
#include <wiring/m5_unit_unified_wiring.hpp>

namespace surface::device {
namespace {
m5::unit::UnitUnified units;
m5::unit::UnitNFC unit;
m5::nfc::NFCLayerA nfc{unit};
bool ready = false;
bool latched = false;
unsigned misses = 0;
uint32_t lastPoll = 0;
std::string lastScreen;
} // namespace
bool boardBegin(std::string& notice) {
  auto cfg = M5.config();
  cfg.fallback_board = m5::board_t::board_M5StickS3;
  M5.begin(cfg);
  M5.Display.setRotation(1);
  M5.Display.setTextSize(1);
  M5.Display.setBrightness(100);
  M5.Power.setExtOutput(true);
  const auto pins = m5::unit::wiring::i2cPins();
  Serial.printf("[board] M5 id=%d display=%dx%d Grove enabled; SDA=%d SCL=%d NFC=0x50\n",
                M5.getBoard(), M5.Display.width(), M5.Display.height(), pins.sda, pins.scl);
  ready = m5::unit::wiring::addI2C(units, unit, 100000) && units.begin();
  notice = ready ? "NFC ready: tap card" : "NFC init FAILED: check Grove cable";
  Serial.println(notice.c_str());
  return ready;
}
BoardEvent boardPoll() {
  M5.update();
  if (M5.BtnA.wasClicked()) return {Input::Refresh, ""};
  if (M5.BtnB.wasClicked()) return {Input::Pause, ""};
  if (!ready || millis() - lastPoll < 200) return {};
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
  if (!nfc.select(picc)) return {};
  if (latched) { nfc.deactivate(); return {}; }
  latched = true;
  Serial.printf("[nfc] detected uid=%s\n", picc.uidAsString().c_str());
  if (!nfc.identify(picc) || !nfc.reactivate(picc)) {
    nfc.deactivate();
    return {Input::Error, "NFC identify/reactivate failed"};
  }
  Serial.printf("[nfc] type=%s user-bytes=%u\n", picc.typeAsString().c_str(), picc.userAreaSize());
  if (!picc.supportsNDEF()) { nfc.deactivate(); return {Input::Error, "NFC-A tag does not support NDEF"}; }
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
    if (rawRead) for (auto b : bytes) Serial.printf("%02x ", b);
    Serial.println();
  }
  nfc.deactivate();
  Serial.printf("[nfc] read=%d valid=%d ms=%lu\n", read, valid, millis() - start);
  if (!read || !message.isMessageTLV()) return {Input::Error, "No readable NDEF message"};
  const auto& records = message.records();
  if (records.size() != 1) return {Input::Error, "Expected exactly one NFC record"};
  const auto& record = records.front();
  Serial.printf("[nfc] TNF=%u type=%s payload-bytes=%u\n", unsigned(record.tnf()), record.type(), record.payloadSize());
  const std::string type = record.type();
  std::string payload;
  if (type == "U" && record.payloadSize()) Serial.printf("[nfc] URI prefix=0x%02x\n", record.payload()[0]);
  auto result = decodeNdefRecord(uint8_t(record.tnf()), type, record.payload(), record.payloadSize(), payload);
  if (!result.ok) return {Input::Error, result.error};
  if (type.empty()) Serial.println("[nfc] legacy raw URL (empty type)");
  Serial.printf("[nfc] payload=%s\n", payload.c_str());
  return {Input::Payload, payload};
}
void boardRender(const AppState& state, const std::string& notice) {
  const auto& observed = state.observed;
  auto screen = std::string("sonos-surface / NFC\n") + notice + "\n" +
    (observed.known ? observed.room + ": " + observed.playback : "Playback: unknown") +
    (observed.stale ? " [stale]" : "") + "\n" + observed.title + "\n" +
    state.status + ": " + state.detail + "\nA: refresh  B: pause";
  if (screen == lastScreen) return;
  lastScreen = screen;
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setCursor(0, 0);
  // Bound each section to keep result and current-state visible on a 240x135 UI.
  M5.Display.println("sonos-surface / NFC");
  M5.Display.println(notice.substr(0, 76).c_str());
  M5.Display.printf("%s%s\n", observed.known ? observed.playback.c_str() : "Unknown playback", observed.stale ? " [stale]" : "");
  M5.Display.println(observed.title.substr(0, 70).c_str());
  M5.Display.println(state.status.c_str());
  M5.Display.println(state.detail.substr(0, 100).c_str());
  M5.Display.setCursor(0, 124);
  M5.Display.println("A: refresh   B: pause");
}
} // namespace surface::device
#endif
