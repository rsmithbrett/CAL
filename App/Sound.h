#pragma once

#include <Arduino.h>

// sdkconfig.h before the board gate further down, for the reason Display.cpp
// gives at its own top: without it CONFIG_IDF_TARGET_ESP32S3 is undefined where
// the gate is evaluated and an S3 build takes the fleet board's branch while
// compiling clean.
#include "sdkconfig.h"

#if defined(CONFIG_IDF_TARGET_ESP32S3)
// Names the pins the flash and PSRAM controller owns on this target. Included
// for the guard below, which reads those numbers from here rather than
// repeating them.
#include <soc/spi_pins.h>
#endif

/// The speaker on the SPEAKER header, and the small amplifier beside it.
///
/// THE SPARE PINS ON THE FLEET BOARD CANNOT BE FOUND BY GREPPING THIS
/// REPOSITORY. That panel comes up through LGFX_AUTODETECT, so every pin the
/// display and the touch controller claim is written in LovyanGFX's board table
/// rather than anywhere here. For that board the table gives the XPT2046 four
/// pins of its own - SCLK 25, MOSI 32, MISO 39, CS 33 - on a software SPI bus
/// with bus_shared false. A pin that looks unclaimed here can be one of those
/// four.
///
/// GPIO26 is what is actually left there: it is a DAC, it is the Sunton speaker
/// pin, and it appears neither in LovyanGFX's table for that board nor in
/// PowerProbe.h's free-pin reasoning.
///
/// The Panellan has a speaker connector and no documented pin behind it, which is
/// why it holds -1 rather than a candidate. The vendor datasheet (WT32S3-28S
/// PRO / ZX2D80CE02S, 2025/06/11) lists four connectors in its Interface Package
/// table and one of them is a speaker: a 2-pin Molex SD-53398-0271, beside the
/// 7-pin debug, the 8-pin expand-I/O and the 4-pin RS485. No pin table anywhere
/// in that document says which GPIO drives it. The Extended IO header (Tab.2)
/// gives this board's only unclaimed pins as GPIO 10, 11, 12, 13, 14 and 21, and
/// names no speaker among them. So the connector is a vendor fact and the pin
/// behind it is not a fact this repository has. -1 stays until somebody puts a
/// meter on it.
///
/// This comment used to say the vendor pin header gave every I2S pin as -1 and
/// named no speaker. `BoardPanellan.h` carries no I2S fields at all, and the
/// vendor does name one.
///
/// GPIO26 is not a candidate here the way it is on the fleet board. On this
/// target it is the PSRAM chip select - `CONFIG_SPIRAM_CS_IO=26` in the
/// generated sdkconfig, `MSPI_IOMUX_PIN_NUM_CS1` in the SDK's own pin table.
/// Attaching LEDC to it routes a square wave onto the line the cache reads
/// PSRAM over, and that heap holds most of what a PSRAM build allocates.
namespace Sound {

/// The amplifier input, or -1 on a board with no amplifier. One pin, not a
/// candidate list.
#if defined(CONFIG_IDF_TARGET_ESP32S3)
constexpr int8_t kSpeakerPin = -1;
#else
constexpr int8_t kSpeakerPin = 26;
#endif

constexpr uint32_t kToneHz = 1000;

/// Plays one short tone at the end of setup(), so a chirp means this device has
/// just finished booting. That is worth having on its own: this fleet restarts
/// for reasons that have been hard to catch, and a unit that announces its own
/// restarts is one somebody standing near it can diagnose without reading a
/// log.
///
/// Silence on a board with a pin means either that the pin does not drive the
/// amplifier or that the amplifier has an enable line this firmware is not
/// holding. Both answers need a meter on the amplifier input, and neither can
/// be settled by trying further pins, because the pins that board has spare are
/// accounted for. Silence on a board with no pin is this function returning
/// after one log line.
///
/// Roughly 250 ms and blocking where a tone is played. Acceptable once per boot
/// before the rotation starts drawing, and not acceptable anywhere inside
/// loop().
void chirpBootIdentification();

#if defined(CONFIG_IDF_TARGET_ESP32S3)
// A peripheral pin in the flash-and-PSRAM range stops being a peripheral pin
// the moment this module drives it, and the symptom is a cache fault some
// distance from here rather than a dead speaker. The range is CS1 through MOSI,
// which is the set a quad-PSRAM module bonds; D4 upward are brought out only on
// an octal part and are left to a board that knows it has one.
static_assert(kSpeakerPin < MSPI_IOMUX_PIN_NUM_CS1 || kSpeakerPin > MSPI_IOMUX_PIN_NUM_MOSI,
              "the speaker pin is one of this target's flash/PSRAM pins - GPIO26 is the PSRAM "
              "chip select");
#endif

}  // namespace Sound
