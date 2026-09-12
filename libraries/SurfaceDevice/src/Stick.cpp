#if defined(SURFACE_STICK_S3)
#include "SurfaceDevice.h"
#include <M5Unified.h>
#include <M5UnitUnified.h>
#include <M5UnitUnifiedNFC.h>
#include "StickDisplay.h"
#include "StickButtons.h"
#include "WriterServer.h"
#include "NtagWriter.h"
#include <esp_timer.h>
#include <esp_random.h>
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
CardWriter writer([] {
  uint8_t bytes[16];
  esp_fill_random(bytes, sizeof(bytes));
  const char* hex = "0123456789abcdef";
  std::string id;
  for (uint8_t byte : bytes) {
    id += hex[byte >> 4];
    id += hex[byte & 15];
  }
  return id;
});
WriterServer writerServer;
NtagWritePages writePages;
m5::nfc::a::PICC writerPicc;
CardOwner presentationOwner = CardOwner::Playback;
bool writerPending = false;
uint64_t writerScreenUntil = 0;
WriterState lastWriterState = WriterState::Idle;
bool displayReady = false;
bool registered = false;
bool ready = false;
uint32_t lastInit = 0;
bool latched = false;
unsigned misses = 0;
uint32_t lastPoll = 0;
std::string lastScreen;
BoardContext context;
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

static Result readCard(std::string& payload) {
  bool valid = false;
  m5::nfc::ndef::TLV message;
  if (!nfc.ndefIsValidFormat(valid) || !valid || !nfc.ndefRead(message) || !message.isMessageTLV())
    return Result::fail("No readable NDEF message");
  const auto& records = message.records();
  if (records.size() != 1)
    return Result::fail("Expected exactly one NFC record");
  const auto& record = records.front();
  Serial.printf("[nfc] TNF=%u type=%s payload-bytes=%lu\n", unsigned(record.tnf()), record.type(),
                record.payloadSize());
  return decodeNdefRecord(uint8_t(record.tnf()), record.type(), record.payload(),
                          record.payloadSize(), payload);
}
static bool prepareCard(m5::nfc::a::PICC& picc, std::string& error) {
  const auto start = millis();
  if (!nfc.identify(picc)) {
    error = "NFC identify failed";
    return false;
  }
  if (!nfc.reactivate(picc)) {
    error = "NFC reactivate failed";
    return false;
  }
  Serial.printf("[nfc] type=%s user-bytes=%u prepare-ms=%lu\n", picc.typeAsString().c_str(),
                picc.userAreaSize(), millis() - start);
  if (!picc.supportsNDEF()) {
    error = "NFC-A tag does not support NDEF";
    return false;
  }
  return true;
}
static void writerStep() {
  if (!writerPending)
    return;
  auto finish = [] {
    writerPending = false;
    writePages.clear();
    nfc.deactivate();
  };
  if (!writer.active() || writer.status() == WriterState::ArmedRead ||
      writer.status() == WriterState::ArmedWrite) {
    // A cancelled/expired operation can be rearmed while a button delays cleanup.
    // Release the old NFC session without consuming the new arm.
    finish();
    return;
  }
  if (writer.status() == WriterState::Detected) {
    std::string error;
    if (!prepareCard(writerPicc, error)) {
      writer.fail(error);
      finish();
      return;
    }
    writer.setCapacity(writerPicc.userAreaSize());
    if (presentationOwner == CardOwner::Read) {
      uint8_t header[16]{}, first[16]{};
      if (writerPicc.isNTAG2() && nfc.read16(header, 0) && header[12] == 0xe1) {
        const size_t capacity = std::min<size_t>(writerPicc.userAreaSize(), size_t(header[14]) * 8);
        NtagLayout layout;
        if (nfc.read16(first, 4))
          inspectNtagLayout(writerPicc.userAreaSize(), capacity, first, layout);
        writer.setCapacity(capacity, layout.prefixBytes);
      }
      writer.reading();
      return;
    }
    using Type = m5::nfc::a::Type;
    if (writerPicc.type != Type::NTAG_213 && writerPicc.type != Type::NTAG_215 &&
        writerPicc.type != Type::NTAG_216) {
      writer.fail("Writer supports NTAG213/215/216 only");
      finish();
      return;
    }
    uint8_t page0[16]{}, dynamic[16]{}, first[16]{};
    const size_t userBytes = writerPicc.userAreaSize();
    if (!nfc.read16(page0, 0) || !nfc.read16(dynamic, uint8_t(4 + userBytes / 4)) ||
        !nfc.read16(first, 4)) {
      writer.fail("Cannot inspect tag capacity and protection");
      finish();
      return;
    }
    size_t capacity = 0;
    auto result = inspectNtag(userBytes, page0, dynamic, capacity);
    NtagLayout layout;
    if (result.ok)
      result = inspectNtagLayout(userBytes, capacity, first, layout);
    writer.setCapacity(capacity, layout.prefixBytes);
    if (result.ok)
      result = writePages.begin(writer.payload(), capacity, layout.prefixBytes);
    if (!result.ok) {
      writer.fail(result.error);
      finish();
      return;
    }
    const size_t terminator = layout.terminator;
    // Reject trailing reserved/control TLVs before they could be overwritten.
    uint8_t tail[16]{};
    const size_t tailOffset = std::min(terminator / 4 * 4, userBytes - 16);
    if (!nfc.read16(tail, uint8_t(4 + tailOffset / 4)) || tail[terminator - tailOffset] != 0xfe) {
      writer.fail("Unsupported trailing tag data; expected one NDEF message and terminator");
      finish();
      return;
    }
    const bool empty = layout.messageBytes == 0;
    std::string existing;
    if (!empty)
      result = readCard(existing);
    if (result.ok)
      result = writer.checkEdit(writerPicc.uidAsString(), existing);
    if (!result.ok) {
      writer.fail(result.error);
      finish();
      return;
    }
    writer.writing();
    return;
  }
  if (writer.status() == WriterState::Reading) {
    std::string payload;
    auto result = readCard(payload);
    if (result.ok)
      writer.read(writerPicc.uidAsString(), payload);
    else
      writer.fail(result.error);
    finish();
    return;
  }
  if (writer.status() == WriterState::Writing) {
    const auto page = writePages.page();
    if (!nfc.write4(page.first, page.second.data(), 4, true)) {
      writer.fail("Page write failed; card may be incomplete");
      finish();
      return;
    }
    writePages.advance();
    if (writePages.done())
      writer.verifying();
    return;
  }
  if (writer.status() == WriterState::Verifying) {
    // Re-select the original UID after writing, then use the ordinary NDEF decoder.
    std::string payload;
    auto result = nfc.reactivate(writerPicc) ? readCard(payload)
                                             : Result::fail("Card removed before verification");
    if (result.ok)
      writer.verify(payload);
    else
      writer.fail("Verification failed: " + result.error);
    finish();
  }
}

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
  if (writerPending) {
    writerStep();
    return {};
  }
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
  presentationOwner = writer.present(esp_timer_get_time() / 1000);
  if (presentationOwner == CardOwner::Read || presentationOwner == CardOwner::Write) {
    writerPicc = picc;
    writerPending = true;
    return {};
  }
  if (presentationOwner == CardOwner::Ignore) {
    nfc.deactivate();
    return {};
  }
  std::string error, payload;
  if (!prepareCard(picc, error)) {
    nfc.deactivate();
    return {Input::Error, error + "; remove and retap"};
  }
  auto result = readCard(payload);
  nfc.deactivate();
  if (!result.ok)
    return {Input::Error, result.error};
  return {Input::Payload, payload};
}
BoardEvent boardPoll() {
  activity = LocalActivity::None;
  const uint64_t now = esp_timer_get_time() / 1000;
  writer.tick(now);
  const bool pageActivity = writerServer.poll(writer, now);
  auto event = pollInput();
  if (writer.takeActivity() || pageActivity)
    activity = LocalActivity::Writer;
  if (writer.status() != lastWriterState) {
    lastWriterState = writer.status();
    writerScreenUntil = now + 7000;
    const auto status = writer.snapshot();
    Serial.printf(
        "[writer] state=%s source=%s encoding=%s payload-bytes=%u tag-bytes=%u capacity=%u "
        "detail=%s\n",
        writerStateName(writer.status()), status["sourceKind"].get<std::string>().c_str(),
        status["encoding"].get<std::string>().c_str(),
        unsigned(status["payloadBytes"].get<size_t>()), unsigned(status["tagBytes"].get<size_t>()),
        unsigned(status["capacity"].get<size_t>()), status["detail"].get<std::string>().c_str());
  }
  event.activity = activity;
  return event;
}
bool boardWriterActive() { return writer.active(); }
void boardPrepareSleep() {
  writerServer.stop();
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
void boardContext(const BoardContext& value) { context = value; }

// Fixed-height text rows cannot wrap into another section. Fit complete UTF-8
// characters to the panel width, marking any text that exceeds its row budget.
static void displayText(int y, std::string text, unsigned rows = 1) {
  for (char& c : text)
    if (static_cast<unsigned char>(c) < 0x20)
      c = ' ';
  for (unsigned row = 0; row < rows; ++row) {
    size_t end = 0;
    while (end < text.size()) {
      size_t next = end + 1;
      while (next < text.size() && (static_cast<unsigned char>(text[next]) & 0xc0) == 0x80)
        ++next;
      const bool ellipsis = row + 1 == rows && next < text.size();
      if (M5.Display.textWidth((text.substr(0, next) + (ellipsis ? "..." : "")).c_str()) > 236)
        break;
      end = next;
    }
    const auto line = text.substr(0, end) + (row + 1 == rows && end < text.size() ? "..." : "");
    M5.Display.setCursor(2, y + int(row) * 8);
    M5.Display.print(line.c_str());
    text.erase(0, end);
  }
}

void boardRender(const AppState& state, const std::string& notice) {
  if (!displayReady)
    return;
  if (writer.active() || (lastWriterState != WriterState::Idle &&
                          uint64_t(esp_timer_get_time() / 1000) < writerScreenUntil)) {
    const auto status = writer.snapshot();
    auto screen = std::string("WRITER: ") + writerStateName(writer.status()) + "\n" +
                  status["detail"].get<std::string>();
    if (screen != lastScreen) {
      lastScreen = screen;
      M5.Display.fillScreen(TFT_BLACK);
      M5.Display.setTextColor(TFT_WHITE);
      M5.Display.setTextWrap(true);
      M5.Display.setCursor(0, 0);
      M5.Display.println(screen.c_str());
      M5.Display.printf("\nhttp://%s/\n", writerServer.address().c_str());
    }
    return;
  }
  const auto& observed = state.observed;
  const auto detail = state.refreshError.empty() ? state.detail : "Refresh: " + state.refreshError;
  const auto playback = std::string(observed.known ? observed.playback : "Unknown playback") +
                        (observed.stale ? " [stale]" : "") + " | Vol " +
                        (observed.volume ? std::to_string(*observed.volume) : "?") +
                        (observed.mute.value_or(false) ? " muted" : "");
  const auto modes = "Mode: " + (observed.mode.empty() ? "unknown" : observed.mode);
  const auto device = std::string(context.online ? "WiFi OK" : "WiFi offline") + " | " +
                      (context.readOnly ? "READ ONLY" : "CONTROL") + " | " +
                      (context.busy ? "Busy" : "Idle");
  const auto screen = observed.room + "\n" + observed.title + "\n" + observed.artist + "\n" +
                      observed.album + "\n" + playback + "\n" + modes + "\n" + device + "\n" +
                      notice + "\n" + state.status + "\n" + detail + "\n" + writerServer.address();
  if (screen == lastScreen)
    return;
  lastScreen = screen;
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setTextWrap(false);
  displayText(0, observed.room.empty() ? "Discovering rooms..." : observed.room);
  displayText(10, "Song: " + (observed.title.empty() ? "unknown" : observed.title));
  displayText(20, "Artist: " + (observed.artist.empty() ? "unknown" : observed.artist));
  displayText(30, "Album: " + (observed.album.empty() ? "unknown" : observed.album));
  displayText(40, playback);
  displayText(50, modes);
  M5.Display.drawFastHLine(0, 61, 240, TFT_DARKGREY);
  displayText(66, device);
  displayText(74, notice);
  displayText(82, "Request: " + state.status);
  displayText(90, detail, 2);
  M5.Display.drawFastHLine(0, 109, 240, TFT_DARKGREY);
  displayText(113, "Writer: http://" + writerServer.address() + "/");
  displayText(125, "A:refresh AA:room B:play/pause");
}
} // namespace surface::device
#endif
