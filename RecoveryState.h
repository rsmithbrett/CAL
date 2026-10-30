#pragma once

#include <stdint.h>

// No Arduino or display dependency: the input state machine can be exercised
// on a host, including clock wrap, held fingers and destructive confirmation.
namespace Recovery {

enum class Action : uint8_t { None, WifiSetup, ResetIdentity, ReloadApplication };
enum class Screen : uint8_t { Closed, Menu, ConfirmIdentity, ConfirmReload, Diagnostics, PresenceTest, GpioMonitor };

struct Rect {
  int16_t x, y, w, h;
  bool contains(int32_t px, int32_t py) const {
    return px >= x && px < x + w && py >= y && py < y + h;
  }
};

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
constexpr Rect kCancel = {16, 180, 136, 44};
constexpr Rect kConfirm = {168, 180, 136, 44};
constexpr uint32_t kGestureWindowMs = 8000;
constexpr uint32_t kMenuIdleMs = 60000;

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

  Screen screen() const { return screen_; }
  bool visible() const { return screen_ != Screen::Closed; }
  uint8_t gpioPage() const { return gpioPage_; }
  bool gpioAll() const { return gpioAll_; }

  InputResult update(uint32_t now, bool down, int32_t x, int32_t y,
                     bool gestureAllowed = true, uint8_t gpioPortCount = 49) {
    InputResult out;
    const bool rising = down && !wasDown_;
    wasDown_ = down;
    if (visible() && screen_ != Screen::PresenceTest && screen_ != Screen::GpioMonitor &&
        static_cast<uint32_t>(now - lastActivity_) >= kMenuIdleMs) {
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
      const bool left = x >= 0 && x < 64 && y >= 0 && y < 60;
      const bool right = x >= 256 && x < 320 && y >= 0 && y < 60;
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
      else if (kMenuRows[2].contains(x, y)) screen_ = Screen::ConfirmReload;
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
      const uint8_t pages = gpioAll_ ? (gpioPortCount + 3) / 4 : 2;
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
