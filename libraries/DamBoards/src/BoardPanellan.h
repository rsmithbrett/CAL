#pragma once

// Panel configuration for the Panellan ZX2D80CE02S-V10 (WT32-S3-WROVER-N8R2).
//
// ONE COPY, REACHED FROM BOTH SKETCHES. Arduino compiles each sketch folder on
// its own and shares no headers between them, which is why Display, Identity,
// Tls, BootDiag and CalQr each exist twice in this repository. A pin map is the
// one of those that must not: two copies of the numbers below, with nothing
// comparing them, is a dark panel the first time somebody corrects one of them.
// So it lives in a library and both sketches include it as <BoardPanellan.h>.
// The compile scripts put that library on the search path with --libraries.
//
// Only a file that is byte-for-byte identical in both trees belongs here. The
// board configuration is the only one today - CAL's Display.cpp and App's have
// diverged substantially and are not candidates.
//
// The other board needs no file like this. LGFX_AUTODETECT probes a panel ID
// over the bus and matches it against LovyanGFX's own table, and the
// ESP32-2432S028 is in that table. This board is not, under any name, so an
// autodetect run on it returns board_unknown and nothing draws. Hence a pin map
// written out by hand.
//
// SOURCE OF THESE NUMBERS. The vendor publishes an Arduino library covering
// this board under its own model name SC05_X:
// github.com/smartpanle/PanelLan_esp32_arduino, src/board/sc05_x/. Both the pin
// header and the LovyanGFX setup below are transcribed from it. That makes
// every pin here READ rather than MEASURED - no continuity check has been done
// against the hardware, and the board has never been booted.
//
// The panel is an ST7789 driven over an 8-bit i80 parallel bus, not SPI. Touch
// is an FT5x06 on I2C. There is NO SD CARD on this board: the vendor's pin
// header gives every SD pin as -1. That decides asset format on its own, since
// the RGB565 rung needs a card and is refused without one.

#include <LovyanGFX.hpp>

namespace panellan {

// LCD, 8-bit i80 bus. RD and CS are tied off on this board, so both are -1 and
// the panel is write-only - see `readable` below.
constexpr int kLcdWr = 17;
constexpr int kLcdRs = 18;   // D/C
constexpr int kLcdRd = -1;
constexpr int kLcdCs = -1;
constexpr int kLcdD0 = 16;
constexpr int kLcdD1 = 40;
constexpr int kLcdD2 = 15;
constexpr int kLcdD3 = 7;
constexpr int kLcdD4 = 41;
constexpr int kLcdD5 = 42;
constexpr int kLcdD6 = 2;
constexpr int kLcdD7 = 1;

// The panel's reset is the board reset, shared. It is not a dedicated LCD pin.
constexpr int kBoardReset = 3;

constexpr int kBacklight = 47;
constexpr uint32_t kBacklightFreq = 21111;
constexpr uint8_t kBacklightChannel = 7;

// Touch, I2C. Bus 1 rather than 0, as the vendor configures it.
constexpr int kTouchSda = 8;
constexpr int kTouchScl = 9;
constexpr int kTouchInt = 48;
constexpr int kTouchRst = -1;
constexpr uint32_t kTouchFreq = 400000;

// Native panel orientation. Display::startPanel() calls setRotation(1) to reach
// the 320x240 landscape the card layouts are written for, exactly as it does on
// the other board.
constexpr int kPanelWidth = 240;
constexpr int kPanelHeight = 320;

constexpr uint32_t kBusWriteHz = 20 * 1000 * 1000;

// The vendor ships a register sequence rather than using Panel_ST7789's stock
// one, so it is carried over verbatim. Porch, gate and gamma values are panel
// glass characteristics; a stock init on this glass is the usual cause of a
// picture that appears but renders washed out or tinted.
class Panel : public lgfx::Panel_ST7789 {
 protected:
  const uint8_t* getInitCommands(uint8_t listno) const override {
    static constexpr uint8_t list0[] = {
        0x11, 0 + CMD_INIT_DELAY, 120,
        0x36, 1, 0x00,
        0x3A, 1, 0x05,
        0xB2, 5, 0x0C, 0x0C, 0x00, 0x33, 0x33,
        0xB7, 1, 0x46,
        0xBB, 1, 0x1B,
        0xC0, 1, 0x2C,
        0xC2, 1, 0x01,
        0xC3, 1, 0x0F,
        0xC4, 1, 0x20,
        0xC6, 1, 0x0F,
        0xD0, 2, 0xA4, 0xA1,
        0xD6, 1, 0xA1,
        0xE0, 14, 0xF0, 0x00, 0x06, 0x04, 0x05, 0x05, 0x31, 0x44, 0x48, 0x36, 0x12, 0x12, 0x2B, 0x34,
        0xE1, 14, 0xF0, 0x0B, 0x0F, 0x0F, 0x0D, 0x26, 0x31, 0x43, 0x47, 0x38, 0x14, 0x14, 0x2C, 0x32,
        0x21, 0,
        0x29, 0,
        0x2C, 0,
        0xFF, 0xFF,
    };
    switch (listno) {
      case 0: return list0;
      default: return nullptr;
    }
  }
};

}  // namespace panellan

// Named LGFX so Display.cpp's `LGFX lcd;` and every call after it is the same
// line on both boards. That is the whole point of keeping the board difference
// at this one boundary.
class LGFX : public lgfx::LGFX_Device {
  panellan::Panel _panel;
  lgfx::Bus_Parallel8 _bus;
  lgfx::Light_PWM _light;
  lgfx::Touch_FT5x06 _touch;

 public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.port = 0;
      cfg.freq_write = panellan::kBusWriteHz;
      cfg.pin_wr = panellan::kLcdWr;
      cfg.pin_rd = panellan::kLcdRd;
      cfg.pin_rs = panellan::kLcdRs;
      cfg.pin_d0 = panellan::kLcdD0;
      cfg.pin_d1 = panellan::kLcdD1;
      cfg.pin_d2 = panellan::kLcdD2;
      cfg.pin_d3 = panellan::kLcdD3;
      cfg.pin_d4 = panellan::kLcdD4;
      cfg.pin_d5 = panellan::kLcdD5;
      cfg.pin_d6 = panellan::kLcdD6;
      cfg.pin_d7 = panellan::kLcdD7;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }

    {
      auto cfg = _panel.config();
      cfg.pin_cs = panellan::kLcdCs;
      cfg.pin_rst = panellan::kBoardReset;
      cfg.pin_busy = -1;
      cfg.memory_width = panellan::kPanelWidth;
      cfg.memory_height = panellan::kPanelHeight;
      cfg.panel_width = panellan::kPanelWidth;
      cfg.panel_height = panellan::kPanelHeight;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      // Adds to whatever setRotation() asks for. If the first boot draws the
      // picture upside down, this is the knob - the vendor's value is kept
      // rather than reasoned about, because they know how the glass is mounted.
      cfg.offset_rotation = 2;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      // RD is tied off, so nothing can be read back from the panel.
      cfg.readable = false;
      cfg.invert = true;
      cfg.rgb_order = true;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }

    {
      auto cfg = _light.config();
      cfg.pin_bl = panellan::kBacklight;
      cfg.invert = false;
      cfg.freq = panellan::kBacklightFreq;
      cfg.pwm_channel = panellan::kBacklightChannel;
      _light.config(cfg);
      _panel.setLight(&_light);
    }

    {
      auto cfg = _touch.config();
      cfg.x_min = 0;
      cfg.x_max = panellan::kPanelWidth;
      cfg.y_min = 0;
      cfg.y_max = panellan::kPanelHeight;
      cfg.bus_shared = false;
      cfg.offset_rotation = 0;
      cfg.i2c_port = 1;
      cfg.pin_sda = panellan::kTouchSda;
      cfg.pin_scl = panellan::kTouchScl;
      cfg.pin_int = panellan::kTouchInt;
      cfg.pin_rst = panellan::kTouchRst;
      cfg.freq = panellan::kTouchFreq;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }

    setPanel(&_panel);
  }
};
