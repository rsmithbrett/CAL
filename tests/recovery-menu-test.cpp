#include <cassert>
#include <iostream>
#include <limits>

#include "Recovery.h"
#include "Display.h"
#include "Identity.h"
#include "Journal.h"
#include "BootDiag.h"
#include "Updater.h"
#include <PresenceInput.h>
#include <esp_ota_ops.h>

namespace {
uint32_t clockMs = 0;
bool fingerDown = false;
int32_t touchX = 0, touchY = 0;
bool secretHeld = true, forced = false, update = false, reload = false;
bool factorySelected = false, saveFails = false, factoryMissing = false;
bool updateSaveFails = false;
int clears = 0, failures = 0, draws = 0;
Recovery::Screen drawn = Recovery::Screen::Closed;
// The second argument production passes to showRecoveryMenu. Recorded rather
// than dropped so the stub cannot quietly diverge from the real signature
// again - it took both arguments for a week before this was noticed, and the
// test had not linked on main since.
bool drawnReloadPending = false;
PresenceTest::State lastPresence;
bool presenceSupported = false;
uint32_t adcMv = 140;
int adcReads = 0, attenuationCalls = 0, presenceDraws = 0;
int digitalReads = 0, pinModes = 0, gpioDraws = 0;
bool digitalHigh = false;
GpioMonitor::State lastGpio;
BootDiag::RestartCause cause = BootDiag::RestartCause::None;
const esp_partition_t factory;
struct Restart {};
struct Stopped {};

Recovery::InputResult tap(Recovery::Controller& c, int32_t x, int32_t y,
                          uint32_t gap = 200) {
  clockMs += gap;
  c.update(clockMs - 1, false, x, y);
  return c.update(clockMs, true, x, y);
}

void open(Recovery::Controller& c) {
  for (int i = 0; i < 6; ++i) {
    const auto r = tap(c, i % 2 ? 288 : 32, 30);
    assert(r.action == Recovery::Action::None);
    assert(c.visible() == (i == 5));
  }
  assert(c.screen() == Recovery::Screen::Menu);
}

void stateTests() {
  using namespace Recovery;
  Controller c;
  open(c);
  assert(!c.update(clockMs + 1, true, 20, 60).tapped); // held/dragged finger
  assert(tap(c, 40, 110).action == Action::None);
  assert(c.screen() == Screen::ConfirmIdentity);
  assert(tap(c, 30, 200).action == Action::None); // Cancel
  assert(c.screen() == Screen::Menu);
  tap(c, 40, 110);
  assert(tap(c, 200, 200).action == Action::ResetIdentity);
  c.reset(); open(c);
  assert(tap(c, 40, 155).action == Action::None);
  assert(c.screen() == Screen::ConfirmReload);
  assert(tap(c, 200, 200).action == Action::ReloadApplication);
  c.reset(); open(c);
  assert(tap(c, 40, 60).action == Action::WifiSetup);
  c.reset(); open(c);
  assert(tap(c, 260, 200).restore && !c.visible()); // Back
  c.reset(); open(c);
  assert(tap(c, 40, 200).action == Action::None);
  assert(c.screen() == Screen::Diagnostics);
  tap(c, 40, 60);
  assert(c.screen() == Screen::PresenceTest);
  assert(tap(c, 150, 210).action == Action::None);
  assert(c.screen() == Screen::Diagnostics);
  tap(c, 40, 110);
  assert(c.screen() == Screen::GpioMonitor && !c.gpioAll());
  clockMs += kMenuIdleMs;
  assert(!c.update(clockMs, false, 0, 0).restore); // live tests do not expire
  tap(c, 250, 15);
  assert(c.gpioAll() && c.gpioPage() == 0);
  tap(c, 260, 210);
  assert(c.gpioPage() == 1);
  tap(c, 40, 210);
  assert(c.gpioPage() == 0);
  tap(c, 150, 210);
  assert(c.screen() == Screen::Diagnostics);
  tap(c, 40, 155);
  assert(c.screen() == Screen::Menu);

  // Wrong corner and outside taps reset the sequence; 5 taps never open it.
  c.reset();
  tap(c, 32, 30); tap(c, 160, 30);
  for (int i = 0; i < 5; ++i) tap(c, i % 2 ? 32 : 288, 30);
  assert(!c.visible());
  c.reset();
  for (int i = 0; i < 5; ++i) tap(c, i % 2 ? 288 : 32, 30);
  assert(!c.visible());
  tap(c, 288, 30, kGestureWindowMs);
  assert(!c.visible());

  // Long touches, slides and quick contact bounce cannot supply six taps.
  c.reset();
  for (int i = 0; i < 20; ++i) {
    clockMs += 200;
    c.update(clockMs, true, i % 2 ? 288 : 32, 30);
  }
  assert(!c.visible());
  c.reset();
  for (int i = 0; i < 6; ++i) tap(c, i % 2 ? 288 : 32, 30, 10);
  assert(!c.visible());

  c.reset(); open(c);
  tap(c, 40, 110);
  clockMs += kMenuIdleMs;
  const auto expired = c.update(clockMs, true, 200, 200);
  assert(expired.restore && expired.action == Action::None && !c.visible());

  // Unsigned elapsed time works through millis() rollover for both timers.
  c.reset(); clockMs = std::numeric_limits<uint32_t>::max() - 500;
  open(c);
  clockMs += kMenuIdleMs;
  assert(c.update(clockMs, false, 0, 0).restore);
  c.reset(); clockMs = std::numeric_limits<uint32_t>::max() - 1000;
  tap(c, 32, 30);
  tap(c, 288, 30, kGestureWindowMs);
  assert(!c.visible());

  // Zones exclude the centre, bottom, out-of-bounds points and right edge.
  for (const auto& row : kMenuRows) {
    assert(row.contains(row.x, row.y));
    assert(!row.contains(row.x + row.w, row.y));
    assert(!row.contains(row.x, row.y + row.h));
  }
  c.reset();
  for (int i = 0; i < 6; ++i) {
    clockMs += 200;
    c.update(clockMs - 1, false, 0, 0, false);
    c.update(clockMs, true, i % 2 ? 288 : 32, 30, false);
  }
  assert(!c.visible()); // not on a QR screen
  c.reset(true);
  assert(!c.update(clockMs, true, 32, 30).tapped);

  // THE APP'S REQUEST OPENS THE MENU WITH NO GESTURE. CAL calls Controller::open
  // on the boot that follows six taps on the running App, having cleared cfgmenu
  // first. See CAL_TOUCH_RECOVERY_MENU.md.
  c.reset();
  assert(!c.visible());
  c.open(clockMs);
  assert(c.visible() && c.screen() == Screen::Menu);
  assert(tap(c, 40, 60).action == Action::WifiSetup); // rows live immediately

  // The idle clock runs from the open, so a menu nobody touches still closes.
  c.reset();
  c.open(clockMs);
  assert(c.update(clockMs + kMenuIdleMs, false, 0, 0).restore);
  assert(!c.visible());

  // A HALF-FINISHED GESTURE DOES NOT SURVIVE THE OPEN. Drop the step_ reset from
  // Controller::open and the three taps below leave the counter at 3, so the
  // three after the menu closes reach six and reopen it with nobody asking.
  c.reset();
  for (int i = 0; i < 3; ++i) {
    clockMs += 200;
    c.update(clockMs - 1, false, 0, 0);
    c.update(clockMs, true, i % 2 ? 288 : 32, 30);
  }
  assert(!c.visible());
  c.open(clockMs);
  assert(tap(c, 260, 200).restore && !c.visible()); // Back
  for (int i = 0; i < 3; ++i) {
    clockMs += 200;
    c.update(clockMs - 1, false, 0, 0);
    c.update(clockMs, true, i % 2 ? 288 : 32, 30);
  }
  assert(!c.visible()); // three is not six, and the earlier three did not carry
}

void resetActions() {
  secretHeld = true; forced = false; update = false; reload = false;
  clears = 0; factorySelected = false; saveFails = false; factoryMissing = false;
  failures = 0; cause = BootDiag::RestartCause::None;
  updateSaveFails = false;
}

void expectRestart(Recovery::Action action) {
  try { Recovery::execute(action); assert(false); } catch (const Restart&) {}
  assert(factorySelected && update && failures == 0);
}

Recovery::InputResult uiTap(int32_t x, int32_t y) {
  fingerDown = false; Recovery::poll(); clockMs += 200;
  touchX = x; touchY = y; fingerDown = true;
  return Recovery::poll();
}

void integrationTests() {
  using namespace Recovery;
  // Exercise production Recovery.cpp, with only hardware/NVS/drawing stubbed.
  end();
  assert(!poll().tapped && !visible());
  fingerDown = true; begin();
  assert(!poll().tapped); // consume the finger that opened this setup screen
  for (int i = 0; i < 6; ++i) {
    fingerDown = false; poll();
    clockMs += 200; touchX = i % 2 ? 288 : 32; touchY = 30;
    fingerDown = true; poll();
  }
  assert(visible() && drawn == Screen::Menu && draws == 1);
  // The menu is drawn with the reload flag production reads, not without it.
  // A stub that took one argument where production passed two is what stopped
  // this whole file linking, so the second argument is asserted, not ignored.
  assert(drawnReloadPending == reload);
  fingerDown = false; poll();
  clockMs += 200; touchX = 40; touchY = 200; fingerDown = true;
  assert(poll().action == Action::None);
  assert(drawn == Screen::Diagnostics);
  uiTap(40, 60);
  assert(visible() && presenceDraws == 1 && presenceSupported == PresenceInput::kSupported);
  if constexpr (PresenceInput::kSupported) {
    assert(adcReads == 1 && attenuationCalls == 1 && !lastPresence.high);
    adcMv = 3155;
    clockMs += 49; poll(); assert(adcReads == 1);
    clockMs += 1; poll(); assert(adcReads == 2);
    clockMs += 200; poll();
    assert(lastPresence.high && lastPresence.changes == 1 && lastPresence.mv == 3155);
  } else {
    clockMs += 1000; poll();
    assert(adcReads == 0 && attenuationCalls == 0 && !lastPresence.hasReading);
  }
  fingerDown = false; poll(); clockMs += 200;
  touchX = 150; touchY = 210; fingerDown = true;
  poll(); assert(drawn == Screen::Diagnostics);
  const int readsOnExit = adcReads;
  clockMs += 500; poll(); assert(adcReads == readsOnExit);
  uiTap(40, 110);
  assert(gpioDraws == 1 && lastGpio.count() == 0);
  assert(pinModes > 0 && digitalReads > 0);
  digitalHigh = true;
  clockMs += 50; poll();
  clockMs += 150; poll();
  clockMs += 50; poll();
  assert(lastGpio.count() > 0);
  uiTap(250, 15); // All-pins mode must keep the measurements/counters.
  assert(lastGpio.count() > 0);
  uiTap(150, 210);
  assert(drawn == Screen::Diagnostics);
  const int readsAfterGpio = digitalReads + adcReads;
  clockMs += 500; poll(); assert(readsAfterGpio == digitalReads + adcReads);
  end(); assert(!visible());

  resetActions(); expectRestart(Action::WifiSetup);
  assert(secretHeld && clears == 0 && forced && !reload);
  assert(cause == BootDiag::RestartCause::Reprovision);
  resetActions(); expectRestart(Action::ResetIdentity);
  assert(!secretHeld && clears == 1 && !forced && !reload);
  resetActions(); expectRestart(Action::ReloadApplication);
  assert(secretHeld && clears == 0 && !forced && reload);
  assert(cause == BootDiag::RestartCause::Ota);

  resetActions(); saveFails = true;
  try { Recovery::execute(Action::ReloadApplication); assert(false); }
  catch (const Stopped&) {}
  assert(failures == 1 && clears == 0 && cause == BootDiag::RestartCause::None);
  resetActions(); factoryMissing = true;
  try { Recovery::execute(Action::ResetIdentity); assert(false); }
  catch (const Stopped&) {}
  assert(failures == 1 && clears == 0 && !update);
  resetActions(); updateSaveFails = true;
  try { Recovery::execute(Action::ResetIdentity); assert(false); }
  catch (const Stopped&) {}
  assert(secretHeld && clears == 0 && !update && failures == 1);

  // Use the production manifest decision, including same-version reinstalls.
  Updater::Manifest manifest;
  manifest.ok = true; manifest.isConfigured = true; manifest.version = "current";
  assert(!manifest.needsInstall("current", true));
  assert(manifest.needsInstall("current", true, true));
  assert(manifest.needsInstall("old", true));
  assert(manifest.needsInstall("current", false));
  manifest.isConfigured = false;
  assert(!manifest.needsInstall("current", true, true));
  assert(!manifest.needsInstall("old", false, true));
  manifest.isConfigured = true; manifest.ok = false;
  assert(!manifest.needsInstall("current", true, true));
  assert(!manifest.needsInstall("old", false, true));
}
}  // namespace

uint32_t millis() { return clockMs; }
void delay(uint32_t) { throw Stopped{}; } // terminal error path, never a reboot
void analogSetPinAttenuation(uint8_t pin, int attenuation) {
  assert(GpioMonitor::kind(pin) == GpioMonitor::Kind::Analog && attenuation == ADC_11db);
  ++attenuationCalls;
}
uint32_t analogReadMilliVolts(uint8_t pin) {
  assert(GpioMonitor::kind(pin) == GpioMonitor::Kind::Analog);
  ++adcReads;
  return adcMv;
}
void pinMode(uint8_t pin, int mode) {
  assert(GpioMonitor::kind(pin) == GpioMonitor::Kind::Digital && mode == INPUT);
  ++pinModes;
}
int digitalRead(uint8_t pin) {
  assert(GpioMonitor::kind(pin) == GpioMonitor::Kind::Digital);
  ++digitalReads;
  return digitalHigh ? HIGH : 0;
}
const esp_partition_t* esp_partition_find_first(int, int, const char*) {
  return factoryMissing ? nullptr : &factory;
}
int esp_ota_set_boot_partition(const esp_partition_t* p) {
  assert(p == &factory); factorySelected = true; return ESP_OK;
}
[[noreturn]] void esp_restart() { throw Restart{}; }
namespace Display {
bool readTouch(int32_t& x, int32_t& y) { x = touchX; y = touchY; return fingerDown; }
void showRecoveryMenu(Recovery::Screen screen, bool reloadPending) {
  drawn = screen; drawnReloadPending = reloadPending; ++draws;
}
void showFailure(const String&, const String&) { ++failures; }
void showPresenceTest(const PresenceTest::State& reading, uint32_t, bool supported, bool) {
  lastPresence = reading; presenceSupported = supported; ++presenceDraws;
}
void showGpioMonitor(const GpioMonitor::State& state, bool, uint8_t, uint32_t, bool) {
  lastGpio = state; ++gpioDraws;
}
}
namespace Identity {
bool hasSecret() { return secretHeld; }
void clearSecret() { ++clears; if (!saveFails) secretHeld = false; }
bool provisioningForced() { return forced; }
void setProvisioningForced(bool v) { if (!saveFails) forced = v; }
bool updateRequested() { return update; }
void setUpdateRequested(bool v) { if (!saveFails && !updateSaveFails) update = v; }
bool appReloadRequested() { return reload; }
void setAppReloadRequested(bool v) { if (!saveFails) reload = v; }
}
namespace Journal {
void line(const char*) {}
void poll() {}
void printf(const char*, ...) {}
}
namespace BootDiag {
void recordRestartIntentIfNoneRecorded(RestartCause value) { cause = value; }
}
int main() {
  stateTests();
  PresenceTest::State reading;
  reading.sample(UINT32_MAX - 100, 1499, 1500);
  assert(reading.hasReading && !reading.high && reading.changes == 0);
  reading.sample(149, 1500, 1500); // threshold inclusive; history crosses rollover
  assert(reading.high && reading.changes == 1 && reading.historyAt(47) == 2);
  reading.sample(399, 3155, 1500);
  assert(reading.changes == 1);
  reading.sample(20000, 140, 1500); // missing time is unknown, not faked HIGH
  assert(!reading.high && reading.changes == 2 && reading.historyAt(46) == 0);
  assert(reading.historyAt(47) == 1);
  reading.reset(); assert(!reading.hasReading && reading.changes == 0);
  GpioMonitor::State gpio;
  for (uint8_t pin : GpioMonitor::kInputs) {
    assert(GpioMonitor::kind(pin) == GpioMonitor::Kind::Analog || GpioMonitor::kind(pin) == GpioMonitor::Kind::Digital);
    gpio.sample(pin, UINT32_MAX - 200, 0);
    gpio.sample(pin, UINT32_MAX - 100, 3300);
    assert(gpio.input(pin)->changes == 0); // reject transient/noise
    gpio.sample(pin, 50, 3300); // elapsed 151 ms through rollover
    assert(gpio.input(pin)->changes == 1);
  }
  assert(gpio.count() > 0);
  GpioMonitor::State noise;
  const uint8_t testPin = GpioMonitor::kInputs[0];
  noise.sample(testPin, 0, 0);
  noise.sample(testPin, 10, 3300);
  noise.sample(testPin, 100, 0);
  noise.sample(testPin, 300, 0);
  assert(noise.count() == 0 && noise.input(testPin)->changes == 0);
  if constexpr (PresenceInput::kSupported) {
    assert(GpioMonitor::kind(39) == GpioMonitor::Kind::Reserved); // touch MISO
    assert(GpioMonitor::kInputCount == 3);
  } else {
    assert(GpioMonitor::kInputCount == 6);
    for (uint8_t pin = 26; pin <= 37; ++pin) assert(GpioMonitor::kind(pin) == GpioMonitor::Kind::Reserved);
    for (uint8_t pin : {uint8_t(7), uint8_t(8), uint8_t(9), uint8_t(19), uint8_t(20), uint8_t(40), uint8_t(47), uint8_t(48)}) {
      assert(GpioMonitor::kind(pin) == GpioMonitor::Kind::Reserved);
    }
  }
  for (int i = 0; i < 12; ++i) {
    const uint32_t value = i % 2 ? 3300 : 0;
    gpio.sample(GpioMonitor::kInputs[0], 1000 + i * 500, value);
    gpio.sample(GpioMonitor::kInputs[0], 1200 + i * 500, value);
  }
  assert(gpio.count() == GpioMonitor::kEventCount);
  integrationTests();
  std::cout << "PASS: recovery gesture, confirmation, timeouts, wrap, rendering handoff and actions\n";
  std::cout << "Controller RAM: " << sizeof(Recovery::Controller) << " bytes (host ABI)\n";
}
