#pragma once

#include <Arduino.h>

/// Card-agnostic polling for the XPT2046 resistive touch controller wired to
/// this exact 2.8" ILI9341 panel - the same "Cheap Yellow Display" family as
/// C:\Users\Administrator\Documents\CYD-Dickey's TouchKeyboard.cpp, a real,
/// working reference for driving this controller on this hardware. The
/// actual hardware read lives in Display::readTouchRaw() (see Display.h's
/// own remarks on why it lives there rather than here - in short, Display
/// already owns the one LGFX instance for this panel, and a second
/// LGFX_AUTODETECT instance touching the same SPI bus would risk
/// re-initialising hardware Display already brought up). This module adds
/// only the small, reusable event shape on top of that raw read.
///
/// Deliberately kept ignorant of cards, or any other UX concept - App.ino's
/// loop() polls wasTapped() once per iteration, the same way it already
/// polls forceUpdateCheckRequested() for the BOOT button, and today's one
/// caller happens to wire a tap to "advance to the next card" (see
/// App.ino's CardKind/refreshCurrentCard()). Nothing here assumes that: a
/// future caller wanting a different gesture, or a second on-screen
/// control, is free to build on this same wasTapped() (or add a sibling
/// function reading the raw coordinate a different way) without this file's
/// shape needing to change.
///
/// **UNVERIFIED ON HARDWARE.** This board's display/panel was confirmed
/// working on a real ELEGOO unit earlier this session, but that
/// confirmation never exercised touch - nobody has put a finger on this
/// glass yet. A clean compile proves this code builds against LovyanGFX's
/// touch API, not that a tap here actually produces a coordinate on this
/// specific board. See README's Open questions.
namespace Touch {

/// True for exactly one loop() iteration per physical tap - edge-detected
/// against the previous iteration's reading, the same debounce idiom
/// App.ino's own forceUpdateCheckRequested() uses for the BOOT button, so
/// holding a finger down doesn't fire this repeatedly.
bool wasTapped();

}  // namespace Touch
