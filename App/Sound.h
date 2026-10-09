#pragma once

#include <Arduino.h>

/// The speaker on the SPEAKER header, and the small amplifier beside it.
///
/// THE SPARE PINS ON THIS BOARD CANNOT BE FOUND BY GREPPING THIS REPOSITORY.
/// The panel comes up through LGFX_AUTODETECT, so every pin the display and
/// the touch controller claim is written in LovyanGFX's board table rather
/// than anywhere here. For this board that table gives the XPT2046 four pins
/// of its own - SCLK 25, MOSI 32, MISO 39, CS 33 - on a software SPI bus with
/// bus_shared false. A pin that looks unclaimed here can be one of those four.
///
/// GPIO26 is what is actually left: it is a DAC, it is the Sunton speaker pin,
/// and it appears neither in LovyanGFX's table for this board nor in
/// PowerProbe.h's free-pin reasoning.
namespace Sound {

/// The amplifier input. One pin, not a candidate list.
constexpr uint8_t kSpeakerPin = 26;

constexpr uint32_t kToneHz = 1000;

/// Plays one short tone on GPIO26 at the end of setup(), so a chirp means
/// this device has just finished booting. That is worth having on its own:
/// this fleet restarts for reasons that have been hard to catch, and a unit
/// that announces its own restarts is one somebody standing near it can
/// diagnose without reading a log.
///
/// Silence means either that GPIO26 does not drive the amplifier or that the
/// amplifier has an enable line this firmware is not holding. Both answers
/// need a meter on the amplifier input, and neither can be settled by trying
/// further pins, because the pins this board has spare are accounted for.
///
/// Roughly 250 ms and blocking. Acceptable once per boot before the rotation
/// starts drawing, and not acceptable anywhere inside loop().
void chirpBootIdentification();

}  // namespace Sound
