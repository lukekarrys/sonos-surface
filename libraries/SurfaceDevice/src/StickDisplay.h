#pragma once
#include <M5Unified.h>
#include <lgfx/v1/panel/Panel_ST7789.hpp>

namespace surface::device {
// Fixed StickS3 wiring and PM1 LCD power sequence from pinned M5GFX 0.2.28.
// Supply a known board AND panel to M5Unified: fallback_board/M5GFX_BOARD are
// detection hints, not overrides. No dependency on M5GFX's AUTODETECT NVS cache.
class StickDisplay : public m5gfx::M5GFX {
  lgfx::Bus_SPI bus;
  lgfx::Panel_ST7789 panel;
  lgfx::Light_PWM light;

public:
  StickDisplay() {
    _board = m5::board_t::board_M5StickS3;
    auto b = bus.config();
    b.spi_host = SPI3_HOST;
    b.spi_mode = 0;
    b.freq_write = 40000000;
    b.freq_read = 16000000;
    b.pin_mosi = 39;
    b.pin_miso = -1;
    b.pin_sclk = 40;
    b.pin_dc = 45;
    b.spi_3wire = true;
    bus.config(b);
    panel.bus(&bus);
    auto p = panel.config();
    p.pin_cs = 41;
    p.pin_rst = 21;
    p.panel_width = 135;
    p.panel_height = 240;
    p.offset_x = 52;
    p.offset_y = 40;
    p.offset_rotation = 0;
    p.readable = true;
    p.invert = true;
    p.bus_shared = false;
    panel.config(p);
    auto l = light.config();
    l.pin_bl = 38;
    l.pwm_channel = 7;
    l.freq = 256;
    l.invert = false;
    l.offset = 16;
    light.config(l);
    panel.setLight(&light);
    setPanel(&panel);
  }
  bool beginFixed() {
    constexpr auto port = I2C_NUM_1;
    constexpr uint8_t pm1 = 0x6e;
    constexpr uint32_t frequency = 100000;
    lgfx::i2c::init(port, 47, 48);
    // Disable PM1 idle sleep before other transactions (PM1 survives warm reset).
    bool power = bool(lgfx::i2c::writeRegister8(port, pm1, 0x09, 0, 0, frequency));
    power = lgfx::i2c::bitOff(port, pm1, 0x16, 1 << 2, frequency) && power;
    power = lgfx::i2c::bitOn(port, pm1, 0x10, 1 << 2, frequency) && power;
    power = lgfx::i2c::bitOff(port, pm1, 0x13, 1 << 2, frequency) && power;
    power = lgfx::i2c::bitOn(port, pm1, 0x11, 1 << 2, frequency) && power;
    Serial.printf("[display] StickS3 fixed target; PM1 LCD power=%d\n", power);
    if (!power)
      return false;
    delay(100);                 // Pinned vendor LCD power settling time.
    return M5GFX::init(&panel); // Explicit panel overload bypasses autodetect.
  }
};
} // namespace surface::device
