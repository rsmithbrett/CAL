// App/ConfigRequest.cpp: the six corner taps that ask CAL for its
// configuration menu, read on a display that is already showing cards.
//
// The production file is compiled as it ships. Display, Loader and Log are
// stubbed, which is the whole of its dependency: it names Display::screenWidth,
// Display::showStatus, Loader::requestConfigMenu and Log::line and nothing
// else. The Loader stub throws, which is how a case tells a sequence that
// reached the sixth tap from one that did not, and how the counter production
// clears just before handing over can be read afterwards.
//
// What each case is here for is TEST_PLAN.md section 16h. The design is
// CAL_TOUCH_RECOVERY_MENU.md, "Reaching it from a running App".

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include <ConfigGesture.h>

#include "ConfigRequest.h"
#include "Display.h"
#include "Loader.h"
#include "Log.h"

namespace {

constexpr unsigned kTaps = ConfigGesture::kTaps;
constexpr uint32_t kBounce = ConfigGesture::kBounceMs;
constexpr uint32_t kWindow = ConfigGesture::kWindowMs;

int gPanelWidth = 320;
int gHandovers = 0;
std::vector<std::string> gShown;

/// Thrown by the Loader stub, which production declares [[noreturn]] and
/// reaches by restarting the device.
struct HandedOver {};

/// One tap sequence, at one panel width.
///
/// ConfigRequest keeps its counter, its window start and its last-tap time in
/// file statics, and production offers no way to clear them. Adding one would
/// be a production change made for a test's convenience, so every sequence
/// opens instead with a tap in the middle of the panel: that tap is in neither
/// corner, and setting the counter back to zero is the one thing production
/// does with a tap in neither corner.
///
/// atTime() is the only function here that calls production, and it delivers
/// exactly one tap. A helper that tapped twice, or that re-centred between
/// taps, would reset the counter underneath a case meant to count to six, which
/// is how a case that could not fail was once read as one that bit.
class Seq {
 public:
  explicit Seq(uint32_t start, uint32_t step = kBounce) : step_(step) {
    ConfigRequest::noteTap(start, gPanelWidth / 2, 120);
    last_ = start;
    first_ = start + step;
  }

  /// Delivers one tap at an absolute time. Returns whether the device is still
  /// running, so a case cannot keep tapping past a restart production does not
  /// return from.
  bool atTime(uint32_t when, int32_t x, int32_t y) {
    last_ = when;
    if (handedOver_) return false;
    try {
      ConfigRequest::noteTap(when, x, y);
    } catch (const HandedOver&) {
      handedOver_ = true;
    }
    return !handedOver_;
  }

  /// One tap, `step` after the previous one.
  bool at(int32_t x, int32_t y) { return atTime(last_ + step_, x, y); }

  /// One tap, `gap` after the previous one.
  bool atAfter(uint32_t gap, int32_t x, int32_t y) { return atTime(last_ + gap, x, y); }

  /// Taps the left corner on an even step and the right corner on an odd one,
  /// which is the order production asks for. Both points are well inside their
  /// zones; the edges have cases of their own below.
  void alternating(unsigned taps) {
    for (unsigned i = 0; i < taps; ++i) {
      at((i % 2) == 0 ? 10 : gPanelWidth - 10, 10);
    }
  }

  /// When the first tap after the centring one landed. The gesture window is
  /// counted from there.
  uint32_t firstTap() const { return first_; }
  uint32_t lastTap() const { return last_; }
  bool handedOver() const { return handedOver_; }

 private:
  uint32_t step_;
  uint32_t last_ = 0;
  uint32_t first_ = 0;
  bool handedOver_ = false;
};

/// Clears what the stubs recorded. The counter inside production is cleared by
/// the next Seq's centring tap.
void clearRecord() {
  gHandovers = 0;
  gShown.clear();
}

}  // namespace

namespace Display {
int screenWidth() { return gPanelWidth; }
void showStatus(const String& headline, const String& detail) {
  gShown.push_back("status:" + headline + "|" + detail);
}
}  // namespace Display

namespace Loader {
void requestConfigMenu() {
  ++gHandovers;
  gShown.push_back("handover");
  throw HandedOver{};
}
}  // namespace Loader

namespace Log {
void line(const char*) {}
}  // namespace Log

int main() {
  // Six alternating taps, kBounce apart, open the menu. Every case below
  // measures a departure from this one.
  clearRecord();
  {
    Seq s(1000);
    s.alternating(kTaps);
    assert(s.handedOver());
    assert(gHandovers == 1);
  }

  // The gesture is read before anything is drawn, and the person is told what
  // is about to happen before the device goes. Both strings are what a reader
  // sees in the second before a restart, so they are pinned rather than
  // described.
  assert(gShown.size() == 2);
  assert(gShown[0] == "status:Configuration|Restarting to open it.");
  assert(gShown[1] == "handover");

  // --- alternation ----------------------------------------------------------
  //
  // Six taps in one corner are what handling the device produces, and a display
  // that took them would reboot into the loader under somebody's hand. Both
  // corners, because the counter treats them differently: an unexpected left
  // tap starts a fresh sequence and an unexpected right tap does not.
  clearRecord();
  {
    Seq s(100000);
    for (unsigned i = 0; i < kTaps; ++i) s.at(10, 10);
    assert(!s.handedOver());
  }
  assert(gHandovers == 0 && gShown.empty());

  clearRecord();
  {
    Seq s(200000);
    for (unsigned i = 0; i < kTaps; ++i) s.at(gPanelWidth - 10, 10);
    assert(!s.handedOver());
  }
  assert(gHandovers == 0 && gShown.empty());

  // A sequence that begins on the right wastes its first tap: the counter is at
  // zero, zero expects the left corner, so the sixth tap of this run is only
  // the fifth of a sequence and the seventh is what opens the menu.
  clearRecord();
  {
    Seq s(300000);
    for (unsigned i = 0; i < kTaps; ++i) {
      s.at((i % 2) == 0 ? gPanelWidth - 10 : 10, 10);
    }
    assert(!s.handedOver());
    assert(gHandovers == 0);
    s.at(gPanelWidth - 10, 10);
    assert(s.handedOver());
    assert(gHandovers == 1);
  }

  // --- the tap count --------------------------------------------------------
  //
  // Driven from ConfigGesture::kTaps rather than from a 6, which is what makes
  // this case say "the count is the one the two binaries share" instead of
  // "the count is six". kTaps - 1 taps leave the display alone; the next one
  // does not.
  clearRecord();
  {
    Seq s(400000);
    s.alternating(kTaps - 1);
    assert(!s.handedOver());
    assert(gHandovers == 0 && gShown.empty());
    s.at(gPanelWidth - 10, 10);  // the right corner the counter is waiting for
    assert(s.handedOver());
    assert(gHandovers == 1);
  }

  // --- the bounce filter ----------------------------------------------------
  //
  // One finger bouncing delivers contacts closer together than kBounce, and a
  // whole sequence arriving inside one bounce window is that at its worst. None
  // of it counts past the first tap.
  clearRecord();
  {
    Seq s(500000, 1);
    s.alternating(kTaps);
    assert(!s.handedOver());
    assert(gHandovers == 0 && gShown.empty());
  }

  // The boundary, on one tap. Five taps that count, then the sixth a
  // millisecond short of kBounce: swallowed, and nothing is drawn. A swallowed
  // tap does not move the last-tap time either, so the same tap one
  // millisecond later is exactly kBounce from the fifth and is read.
  clearRecord();
  {
    Seq s(600000);
    s.alternating(kTaps - 1);
    s.atAfter(kBounce - 1, gPanelWidth - 10, 10);
    assert(!s.handedOver());
    assert(gHandovers == 0 && gShown.empty());
    s.atAfter(1, gPanelWidth - 10, 10);
    assert(s.handedOver());
    assert(gHandovers == 1);
  }

  // A bounce swallowed mid-sequence does not break the alternation the real tap
  // then satisfies: two left taps a millisecond apart, then the right tap the
  // counter was waiting for.
  clearRecord();
  {
    Seq s(700000);
    s.at(10, 10);           // counts, counter at one
    s.atAfter(1, 10, 10);   // inside the bounce window, swallowed
    s.atAfter(kBounce, gPanelWidth - 10, 10);  // counter at two
    s.at(10, 10);
    s.at(gPanelWidth - 10, 10);
    s.at(10, 10);
    assert(!s.handedOver());
    s.at(gPanelWidth - 10, 10);
    assert(s.handedOver());
    assert(gHandovers == 1);
  }

  // --- the window -----------------------------------------------------------
  //
  // All six land inside kWindow, counted from the tap that took the counter off
  // zero, or the sequence is abandoned. One millisecond under the window still
  // opens the menu.
  clearRecord();
  {
    Seq s(800000);
    s.alternating(kTaps - 1);
    s.atTime(s.firstTap() + kWindow - 1, gPanelWidth - 10, 10);
    assert(s.handedOver());
    assert(gHandovers == 1);
  }

  // At the window exactly, the counter is cleared before the tap is read. What
  // arrives is a right tap against a counter at zero, which expects the left
  // corner, so it counts for nothing and the sequence starts over.
  clearRecord();
  {
    Seq s(900000);
    s.alternating(kTaps - 1);
    s.atTime(s.firstTap() + kWindow, gPanelWidth - 10, 10);
    assert(!s.handedOver());
    assert(gHandovers == 0 && gShown.empty());
    // The display is still reachable: a fresh six opens it.
    s.alternating(kTaps);
    assert(s.handedOver());
    assert(gHandovers == 1);
  }

  // --- an unexpected left tap -----------------------------------------------
  //
  // A left tap arriving where the right corner was expected starts a fresh
  // sequence at one rather than throwing it away at zero, so five more taps
  // finish it. Six more would mean the tap had been wasted.
  clearRecord();
  {
    Seq s(1000000);
    s.alternating(kTaps - 1);  // counter at five, expecting the right corner
    s.at(10, 10);              // left instead, so the counter is back at one
    assert(!s.handedOver());
    s.at(gPanelWidth - 10, 10);
    s.at(10, 10);
    s.at(gPanelWidth - 10, 10);
    s.at(10, 10);
    assert(!s.handedOver());
    s.at(gPanelWidth - 10, 10);
    assert(s.handedOver());
    assert(gHandovers == 1);
  }

  // --- the corners come off the panel in use --------------------------------
  //
  // A display standing on its end is 240 across, so its right corner begins at
  // 176. Both panels and the pixel either side of each edge, because a right
  // corner measured off a fixed 320 reads correctly on one panel and nowhere at
  // all on the other. 480 is here as well, and it is not a panel the fleet has:
  // two widths can be satisfied by a switch on the board, and a third says the
  // figure is read from the panel in use.
  for (const int width : {320, 240, 480}) {
    gPanelWidth = width;
    const int32_t rightEdge = width - ConfigGesture::kZoneWidth;

    // The first pixel of the right corner.
    clearRecord();
    {
      Seq s(1100000);
      for (unsigned i = 0; i < kTaps; ++i) s.at((i % 2) == 0 ? 10 : rightEdge, 0);
      assert(s.handedOver());
      assert(gHandovers == 1);
    }

    // The pixel before it is in neither corner, so twice the taps open nothing.
    clearRecord();
    {
      Seq s(1200000);
      for (unsigned i = 0; i < 2 * kTaps; ++i) s.at((i % 2) == 0 ? 10 : rightEdge - 1, 0);
      assert(!s.handedOver());
      assert(gHandovers == 0 && gShown.empty());
    }

    // The last pixel of the panel is inside the corner.
    clearRecord();
    {
      Seq s(1300000);
      for (unsigned i = 0; i < kTaps; ++i) s.at((i % 2) == 0 ? 10 : width - 1, 0);
      assert(s.handedOver());
    }
  }
  gPanelWidth = 320;

  // --- how far a corner reaches ---------------------------------------------
  //
  // kZoneWidth in from the edge and kZoneHeight down, half-open on both. The
  // height matters past the gesture: 44 is where CAL's first menu row begins,
  // and that row is the one with no confirmation behind it.
  clearRecord();
  {
    Seq s(1500000);
    for (unsigned i = 0; i < kTaps; ++i) {
      const bool left = (i % 2) == 0;
      s.at(left ? ConfigGesture::kZoneWidth - 1 : gPanelWidth - ConfigGesture::kZoneWidth,
           ConfigGesture::kZoneHeight - 1);
    }
    assert(s.handedOver());
    assert(gHandovers == 1);
  }

  clearRecord();
  {
    Seq s(1600000);
    for (unsigned i = 0; i < 2 * kTaps; ++i) {
      s.at((i % 2) == 0 ? ConfigGesture::kZoneWidth : gPanelWidth - 10, 10);
    }
    assert(!s.handedOver());
    assert(gHandovers == 0);
  }

  clearRecord();
  {
    Seq s(1700000);
    for (unsigned i = 0; i < 2 * kTaps; ++i) {
      s.at((i % 2) == 0 ? 10 : gPanelWidth - 10, ConfigGesture::kZoneHeight);
    }
    assert(!s.handedOver());
    assert(gHandovers == 0);
  }

  // --- the clock rolls over -------------------------------------------------
  //
  // millis() wraps at 49 days. Every elapsed figure production reads is an
  // unsigned subtraction, so a sequence spanning the wrap reads the same as one
  // that does not. These two are the bounce window and the gesture window
  // either side of zero.
  clearRecord();
  {
    Seq s(0xFFFFFE00u);
    s.alternating(kTaps);
    assert(s.handedOver());
    assert(s.lastTap() < 0x10000u);  // the sequence really did cross zero
    assert(gHandovers == 1);
  }

  clearRecord();
  {
    Seq s(0xFFFFF000u);
    s.alternating(kTaps - 1);
    s.atTime(s.firstTap() + kWindow, gPanelWidth - 10, 10);
    assert(s.lastTap() < 0x10000u);
    assert(!s.handedOver());  // expired across the wrap
    assert(gHandovers == 0);
  }

  // --- after a handover -----------------------------------------------------
  //
  // Production clears its counter before calling Loader, so the seventh tap in
  // the corner somebody has just tapped six times counts as a first tap and
  // nothing more. On a device the restart makes that moot; here it is what says
  // the clearing happens before the call rather than after it.
  //
  // Two things about the shape of this case, and both of them are the case
  // working rather than decoration.
  //
  // Not a Seq, because a centring tap would clear the counter this is reading.
  //
  // And inside the gesture window the handover left behind. Written 100,000 ms
  // later, the window expiry clears a stale counter on the next tap all by
  // itself, so the case held whether or not production cleared it - and
  // mut/cr12-clear-after, which moves the clear to after the handover call,
  // passed. Run 41317326384 is that, recorded because it is what an unfailable
  // case looks like from the outside.
  clearRecord();
  {
    Seq s(1800000);
    s.alternating(kTaps);
    assert(s.handedOver());
    assert(gHandovers == 1);
  }
  {
    uint32_t now = 1800700;  // 100 after the sixth tap, 7,400 inside the window
    bool restartedEarly = false;
    unsigned i = 0;
    for (; i < kTaps - 1; ++i) {
      try {
        ConfigRequest::noteTap(now, (i % 2) == 0 ? 10 : 310, 10);
      } catch (const HandedOver&) {
        restartedEarly = true;
        break;
      }
      now += kBounce;
    }
    assert(!restartedEarly);  // a counter left at kTaps would hand over at once
    assert(gHandovers == 1);
    bool restarted = false;
    try {
      ConfigRequest::noteTap(now, 310, 10);
    } catch (const HandedOver&) {
      restarted = true;
    }
    assert(restarted);
    assert(gHandovers == 2);
  }

  std::cout << "PASS: config-request gesture - alternation, count, bounce, window, "
               "corners off the live panel width, zone edges and clock wrap\n";
}
