#pragma once

#include <Arduino.h>

/// Card-agnostic polling for the XPT2046 resistive touch controller wired to
/// this exact 2.8" ILI9341 panel.
///
/// Sampling runs on its own task and classification runs on loop()'s. The
/// reason and the measurements are in TOUCH_SAMPLING_DESIGN.md; the short
/// version is that the fleet logs around 816 iterations an hour longer than
/// 250ms, tap detection is rising-edge, and a tap that started and ended
/// inside one of those was not delayed but erased. Sampling every 10ms
/// regardless of what loop() is doing is the only thing that fixes that, and
/// on this board it is safe to do from a second thread: LovyanGFX's table
/// gives the XPT2046 a software SPI bus of its own with bus_shared false, so a
/// touch read never reaches the display's bus.
///
/// What crosses between the two threads is one queue of raw coordinates and
/// four counters. Zone classification stays on loop()'s side, because
/// setActionZones() is called from the draw path on that same thread, so
/// reading the zones there needs no lock.
///
/// The hardware read lives in Display::readTouchRaw(): Display owns the one
/// LGFX instance for this panel, and a second LGFX_AUTODETECT instance would
/// risk re-initialising hardware Display already brought up.
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
  /// How long the tap waited in the queue before poll() delivered it. Zero on
  /// an unloaded device; a few hundred milliseconds says it arrived while
  /// loop() was inside something slow, which is what the queue is for.
  uint32_t ageMs = 0;
};

/// Starts the sampling task. Call once, after Display::begin(), because the
/// task reads the panel from its first tick.
///
/// A task that cannot be created is logged and left at that: poll() then never
/// finds anything, which is how the device already behaves when the controller
/// does not answer, and not a reason to refuse to boot a wall display.
void begin();

/// The hit rectangles for whatever buttons are currently drawn. Set after
/// every card draw (CardManager does this) and cleared to zero buttons by
/// any draw that has none, so a stale zone from the previous card can never
/// fire on the current one. Geometry is decided by Display, which is the
/// only thing that knows this panel's layout; the hit test lives here.
///
/// Call from loop()'s thread only. The sampling task never reads these.
void setActionZones(const Rect* zones, uint8_t count);

/// Takes the oldest tap the sampling task queued, classifies it against the
/// current zones, and fills `tap`. Returns false when nothing is waiting.
///
/// Two rules keep a queued tap from becoming a press nobody made, both argued
/// in TOUCH_SAMPLING_DESIGN.md: a tap older than 1.5 seconds is dropped, and
/// several taps waiting together collapse to the first, because more than one
/// in the queue means somebody tapped again thinking the first was lost.
///
/// Safe to call as often as you like.
bool poll(Tap& tap);

/// One line for the debug stream, written by whoever owns the check-in. Says
/// how many taps were sampled, delivered, dropped for age and collapsed since
/// the last call, then clears the counters.
///
/// Reported rather than inferred: a slow screen and a tap that never reached
/// the glass look identical from a photograph.
void logAndResetCounters();

}  // namespace Touch
