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
/// This file used to discard the coordinate entirely: `wasTapped()` returned
/// a bare bool, so a tap anywhere on the glass meant one single thing.
/// Display::readTouchRaw() was already handing back x and y and they were
/// simply thrown away. Now the tap is classified into a zone, because there
/// are three different things a tap can mean.
///
/// **Zone priority is deliberate and fixed:** action buttons are tested
/// first, then the reverse (left edge) and forward (right edge) strips.
/// The edge strips are full-height and have no visible chrome of their own,
/// so a button that happens to sit near an edge must win - the same ordering
/// CYD-Dickey's own touch handler uses, where its menu/HOMES buttons in the
/// bottom corners are checked before its `x < 16` / `x > 304` edge zones for
/// exactly that reason.
///
/// Still deliberately ignorant of *cards*: this file knows about rectangles
/// and screen edges, not about what advancing means. CardManager owns that.
///
/// **UNVERIFIED ON HARDWARE.** This board's display/panel was confirmed
/// working on a real ELEGOO unit earlier, but that confirmation never
/// exercised touch - nobody has put a finger on this glass yet. A clean
/// compile proves this code builds against LovyanGFX's touch API, not that a
/// tap here produces a coordinate on this specific board, and certainly not
/// that the coordinate lands where this file's zones assume it does. Zones
/// make that unverified assumption *load-bearing* in a way a bare
/// tap-anywhere never was: an inverted or unscaled axis would previously
/// have gone unnoticed and would now send a "forward" tap backwards. See
/// README's Open questions.
namespace Touch {

struct Rect {
  int16_t x = 0;
  int16_t y = 0;
  int16_t w = 0;
  int16_t h = 0;
};

enum class Hit : uint8_t {
  None,
  /// One of the zones handed to setActionZones(); `actionIndex` says which.
  ActionButton,
  Reverse,
  Forward,
};

struct Tap {
  Hit hit = Hit::None;
  uint8_t actionIndex = 0;
  int32_t x = 0;
  int32_t y = 0;
};

/// The hit rectangles for whatever buttons are currently drawn. Set after
/// every card draw (CardManager does this) and cleared to zero buttons by
/// any draw that has none, so a stale zone from the previous card can never
/// fire on the current one. Geometry is decided by Display, which is the
/// only thing that knows this panel's layout; the hit test lives here.
void setActionZones(const Rect* zones, uint8_t count);

/// True for exactly one loop() iteration per physical tap - edge-detected
/// against the previous iteration's reading, the same debounce idiom
/// App.ino's own forceUpdateCheckRequested() uses for the BOOT button, so
/// holding a finger down doesn't fire this repeatedly. Fills `tap` only when
/// it returns true.
bool poll(Tap& tap);

}  // namespace Touch
