#pragma once

#include <stdint.h>
#include <ConfigGesture.h>

// No Arduino or display dependency: the input state machine can be exercised
// on a host, including clock wrap, held fingers and destructive confirmation.
namespace Recovery {

/// CancelReload is the one action that does not restart the device. It clears a
/// request rather than making one, so Recovery::poll() handles it and callers
/// never see it - see that function, and execute()'s own remarks on why every
/// other action ends in esp_restart().
enum class Action : uint8_t { None, WifiSetup, ResetIdentity, ReloadApplication, CancelReload };
enum class Screen : uint8_t { Closed, Menu, ConfirmIdentity, ConfirmReload, Diagnostics, PresenceTest, GpioMonitor };

struct Rect {
  int16_t x, y, w, h;
  constexpr bool contains(int32_t px, int32_t py) const {
    return px >= x && px < x + w && py >= y && py < y + h;
  }
};

/// Whether two rectangles share no pixel. Half-open on both axes, matching
/// contains().
constexpr bool disjoint(const Rect& a, const Rect& b) {
  return a.x + a.w <= b.x || b.x + b.w <= a.x ||
         a.y + a.h <= b.y || b.y + b.h <= a.y;
}

// Shared by rendering and hit testing, in CAL's 320x240 landscape coordinates.
constexpr Rect kMenuRows[] = {{16, 44, 288, 42}, {16, 90, 288, 42},
                              {16, 136, 288, 42}};
constexpr Rect kPresenceTest = {16, 182, 208, 42};
constexpr Rect kBack = {232, 182, 72, 42};
constexpr Rect kPresenceBack = {16, 190, 288, 44};
constexpr Rect kGpioMode = {224, 4, 80, 28};
constexpr Rect kGpioPrevious = {16, 190, 84, 44};
constexpr Rect kGpioBack = {108, 190, 104, 44};
constexpr Rect kGpioNext = {220, 190, 84, 44};

/// Confirmation buttons. CONFIRM IS ON THE LEFT AND CANCEL ON THE RIGHT, which
/// reverses the usual order and is deliberate: kBack sits bottom-right on the
/// menu, so bottom-right is where an operator has just learned to tap to leave.
/// With Confirm there, the pixels that said "get me out of here" on one screen
/// said "Reset" on the next. The assertion below keeps Cancel in kBack's band.
constexpr Rect kConfirm = {16, 180, 136, 44};
constexpr Rect kCancel = {168, 180, 136, 44};

/// The two corner zones the opening gesture watches, as rectangles rather than
/// inline comparisons, so the assertions below can be written about them.
///
/// THEY STOP AT y=44, WHERE THE FIRST MENU ROW BEGINS. They ran to y=60 when
/// this feature was written, overlapping kMenuRows[0] by a 48x16 band in each
/// corner. The sixth tap opens the menu and returns before any hit test, so that
/// tap was safe - but a seventh, in the same corner the operator had just tapped
/// six times, landed on row 0. Row 0 is Set up Wi-Fi, the one row with no
/// confirmation screen, so one stray contact rebooted the device, forced its
/// portal and left an open setup AP broadcasting until the abandonment timeout.
constexpr Rect kGestureLeft = {0, 0, 64, 44};
constexpr Rect kGestureRight = {256, 0, 64, 44};

// The App watches for the same gesture on a running display and asks CAL for
// the menu by setting cfgmenu. It measures its corners from the panel it is on,
// because it also runs on one standing on its end, so what the two binaries
// share is the shape rather than these rectangles. These assertions are what
// keep them the same shape - the same arrangement App/Motion.cpp uses to keep
// its presence pin and PresenceInput.h from drifting apart.
static_assert(kGestureLeft.w == ConfigGesture::kZoneWidth &&
                  kGestureRight.w == ConfigGesture::kZoneWidth,
              "CAL and App must agree how far in from an edge a corner reaches.");
static_assert(kGestureLeft.h == ConfigGesture::kZoneHeight &&
                  kGestureRight.h == ConfigGesture::kZoneHeight,
              "CAL and App must agree how far down a corner reaches.");
static_assert(kGestureLeft.x == 0, "The left corner starts at the left edge.");
static_assert(kGestureRight.x + kGestureRight.w == 320,
              "The right corner ends at the right edge of CAL's landscape panel.");

constexpr uint32_t kGestureWindowMs = ConfigGesture::kWindowMs;
constexpr uint32_t kMenuIdleMs = 60000;

/// How long a live diagnostics screen may sit untouched. Generous rather than
/// absent: a sensor test wants no deadline a person would notice, but an
/// unbounded one parks the loop it was opened from. On the enrollment QR that
/// loop polls for a device key; in the captive portal it applies credentials a
/// phone has already submitted. A screen left open stops both until somebody
/// returns to the device.
constexpr uint32_t kDiagnosticsIdleMs = 600000;

/// How long holdBootForTouch() listens before anybody has touched. Paid on every
/// boot CAL draws, ahead of the App, so it is deliberately short - long enough
/// for somebody reaching for a panel that has just lit up, not long enough to be
/// read as a slow device.
constexpr uint32_t kBootListenMs = 1200;

/// How long the boot stays held after the most recent tap. Comfortably past
/// kGestureWindowMs so six taps at a human cadence fit inside one hold, and each
/// further tap renews it.
constexpr uint32_t kBootHoldMs = 10000;

/// The ceiling on one holdBootForTouch() call, counted from entry and never
/// renewed. A touch panel reading permanently down would otherwise renew the
/// hold on every pass and stop this device booting for good - and CAL is the
/// image that has to reach the end of its own boot for a cable or an OTA
/// candidate to rescue anything.
constexpr uint32_t kBootHoldCapMs = 120000;

// --- geometry, proved at compile time on both boards --------------------------
//
// These replace a host test that could not fail: it checked each rectangle only
// against its own x/y/w/h, which holds for any rectangle and any half-open
// contains(), so every boundary mutation passed it. A static_assert costs
// nothing, runs in every arduino-cli build of both boards, and needs no host
// compiler at all.

static_assert(disjoint(kGestureLeft, kMenuRows[0]) && disjoint(kGestureRight, kMenuRows[0]),
              "A tap after the menu opens must not land on row 0, which is the "
              "unconfirmed Set up Wi-Fi action.");
static_assert(disjoint(kGestureLeft, kMenuRows[1]) && disjoint(kGestureRight, kMenuRows[1]),
              "A gesture zone over row 1 would open a confirmation on a stray tap.");
static_assert(disjoint(kGestureLeft, kMenuRows[2]) && disjoint(kGestureRight, kMenuRows[2]),
              "A gesture zone over row 2 would open a confirmation on a stray tap.");
static_assert(disjoint(kGestureLeft, kPresenceTest) && disjoint(kGestureRight, kPresenceTest),
              "A gesture zone must not reach the Diagnostics footer.");
static_assert(disjoint(kGestureLeft, kBack) && disjoint(kGestureRight, kBack),
              "A gesture zone must not reach Back.");
static_assert(disjoint(kGestureLeft, kGestureRight), "The two corner zones must not meet.");

static_assert(disjoint(kMenuRows[0], kMenuRows[1]) && disjoint(kMenuRows[1], kMenuRows[2]) &&
                  disjoint(kMenuRows[0], kMenuRows[2]),
              "Menu rows must not overlap; the first match in the hit chain would win silently.");
static_assert(disjoint(kPresenceTest, kBack), "Diagnostics and Back must not overlap.");

static_assert(disjoint(kConfirm, kCancel), "Confirm and Cancel must not overlap.");
static_assert(disjoint(kConfirm, kBack),
              "The destructive confirmation must not sit where Back sits on the menu.");
static_assert(!disjoint(kCancel, kBack),
              "Cancel should hold Back's band, so bottom-right always means leave.");

static_assert(disjoint(kGpioMode, kGpioPrevious) && disjoint(kGpioMode, kGpioBack) &&
                  disjoint(kGpioMode, kGpioNext) && disjoint(kGpioPrevious, kGpioBack) &&
                  disjoint(kGpioBack, kGpioNext),
              "GPIO monitor controls must not overlap.");

static_assert(kGestureLeft.x >= 0 && kGestureRight.x + kGestureRight.w <= 320 &&
                  kMenuRows[2].y + kMenuRows[2].h <= 240 && kCancel.y + kCancel.h <= 240,
              "Every rectangle must fit CAL's 320x240 landscape panel.");

static_assert(kDiagnosticsIdleMs > kMenuIdleMs,
              "A live diagnostics screen should outlast the menu, not undercut it.");

static_assert(kBootHoldMs > kGestureWindowMs,
              "A hold shorter than the gesture window could expire mid-sequence, so six taps "
              "at a human cadence would never open the menu from a boot hold.");
static_assert(kBootHoldCapMs > kBootHoldMs && kBootHoldMs > kBootListenMs,
              "The cap must outlast a hold, and a hold must outlast the listen window.");

struct InputResult {
  Action action = Action::None;
  bool tapped = false;
  bool redraw = false;
  bool restore = false;
};

class Controller {
 public:
  void reset(bool alreadyDown = false) {
    screen_ = Screen::Closed;
    step_ = 0;
    wasDown_ = alreadyDown;
    haveTap_ = false;
    gpioPage_ = 0;
    gpioAll_ = false;
  }

  /// Opens the menu with no gesture, for the boot where the App asked for it by
  /// setting cfgmenu. The idle clock starts now, so a menu nobody touches closes
  /// on kMenuIdleMs the same way a gesture-opened one does.
  ///
  /// step_ is cleared because a half-finished sequence from before would
  /// otherwise still be counting when the menu closes.
  void open(uint32_t now) {
    screen_ = Screen::Menu;
    step_ = 0;
    lastActivity_ = now;
  }

  Screen screen() const { return screen_; }
  bool visible() const { return screen_ != Screen::Closed; }
  uint8_t gpioPage() const { return gpioPage_; }
  bool gpioAll() const { return gpioAll_; }

  /// gpioEventCapacity is how many recorded changes the Changes view can hold,
  /// which decides its page count. It was a hardcoded 2 here, correct only while
  /// GpioMonitor kept exactly eight events: raising that cap would have made the
  /// extra events unreachable with nothing to say so. Recovery.cpp passes the
  /// real constant.
  /// reloadPending turns row 3 into Cancel reload. The row toggles rather than a
  /// fifth one being added, which would mean a new rectangle, a new assertion and
  /// a tighter pitch on a menu already three rows and a footer deep - and the row
  /// a person taps for "something about the application" is the same row either
  /// way. A cancel is not destructive, so it takes effect without a confirmation.
  InputResult update(uint32_t now, bool down, int32_t x, int32_t y,
                     bool gestureAllowed = true, uint8_t gpioPortCount = 49,
                     uint8_t gpioEventCapacity = 8, bool reloadPending = false) {
    InputResult out;
    const bool rising = down && !wasDown_;
    wasDown_ = down;
    const bool live = screen_ == Screen::PresenceTest || screen_ == Screen::GpioMonitor;
    const uint32_t idleLimit = live ? kDiagnosticsIdleMs : kMenuIdleMs;
    if (visible() && static_cast<uint32_t>(now - lastActivity_) >= idleLimit) {
      screen_ = Screen::Closed;
      step_ = 0;
      out.restore = true;
      return out;  // The touch at expiry cannot also press a restored screen.
    }
    if (step_ && static_cast<uint32_t>(now - started_) >= kGestureWindowMs) {
      step_ = 0;
    }
    if (!gestureAllowed && !visible()) step_ = 0;
    // A held finger or a drag is one tap. Ignore contact bounce as well.
    if (!rising || (haveTap_ && static_cast<uint32_t>(now - lastTap_) < 100)) {
      return out;
    }
    haveTap_ = true;
    lastTap_ = now;
    out.tapped = true;

    if (!visible()) {
      if (!gestureAllowed) return out;
      const bool left = kGestureLeft.contains(x, y);
      const bool right = kGestureRight.contains(x, y);
      const bool expected = (step_ % 2 == 0) ? left : right;
      if (expected) {
        if (step_ == 0) started_ = now;
        ++step_;
      } else {
        // An unexpected left tap can start a fresh sequence immediately.
        step_ = left ? 1 : 0;
        started_ = now;
      }
      if (step_ == 6) {
        step_ = 0;
        screen_ = Screen::Menu;
        lastActivity_ = now;
        out.redraw = true;
      }
      return out;
    }

    lastActivity_ = now;
    if (screen_ == Screen::Menu) {
      if (kMenuRows[0].contains(x, y)) out.action = Action::WifiSetup;
      else if (kMenuRows[1].contains(x, y)) screen_ = Screen::ConfirmIdentity;
      else if (kMenuRows[2].contains(x, y)) {
        if (reloadPending) out.action = Action::CancelReload;
        else screen_ = Screen::ConfirmReload;
      }
      else if (kPresenceTest.contains(x, y)) screen_ = Screen::Diagnostics;
      else if (kBack.contains(x, y)) {
        screen_ = Screen::Closed;
        out.restore = true;
      }
      out.redraw = visible() && screen_ != Screen::Menu;
    } else if (screen_ == Screen::Diagnostics) {
      if (kMenuRows[0].contains(x, y)) screen_ = Screen::PresenceTest;
      else if (kMenuRows[1].contains(x, y)) {
        screen_ = Screen::GpioMonitor; gpioAll_ = false; gpioPage_ = 0;
      } else if (kMenuRows[2].contains(x, y)) screen_ = Screen::Menu;
      out.redraw = screen_ != Screen::Diagnostics;
    } else if (screen_ == Screen::GpioMonitor) {
      // Four rows a page in both views. Guarded against zero because a modulo by
      // it is how Previous and Next would fault.
      const uint8_t rows = gpioAll_ ? gpioPortCount : gpioEventCapacity;
      const uint8_t pages = rows ? static_cast<uint8_t>((rows + 3) / 4) : 1;
      if (kGpioMode.contains(x, y)) { gpioAll_ = !gpioAll_; gpioPage_ = 0; out.redraw = true; }
      else if (kGpioPrevious.contains(x, y)) { gpioPage_ = (gpioPage_ + pages - 1) % pages; out.redraw = true; }
      else if (kGpioNext.contains(x, y)) { gpioPage_ = (gpioPage_ + 1) % pages; out.redraw = true; }
      else if (kGpioBack.contains(x, y)) { screen_ = Screen::Diagnostics; out.redraw = true; }
    } else if (screen_ == Screen::PresenceTest) {
      if (kPresenceBack.contains(x, y)) {
        screen_ = Screen::Diagnostics;
        out.redraw = true;
      }
    } else if (kCancel.contains(x, y)) {
      screen_ = Screen::Menu;
      out.redraw = true;
    } else if (kConfirm.contains(x, y)) {
      out.action = screen_ == Screen::ConfirmIdentity ? Action::ResetIdentity
                                                     : Action::ReloadApplication;
    }
    return out;
  }

 private:
  uint32_t started_ = 0;
  uint32_t lastActivity_ = 0;
  uint32_t lastTap_ = 0;
  Screen screen_ = Screen::Closed;
  uint8_t step_ = 0;
  bool wasDown_ = false;
  bool haveTap_ = false;
  uint8_t gpioPage_ = 0;
  bool gpioAll_ = false;
};

}  // namespace Recovery
