#include <cassert>
#include <iostream>
#include <limits>

#include "Recovery.h"
#include "Display.h"
#include "Identity.h"
#include "Journal.h"
#include "BootDiag.h"
#include "Updater.h"
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
  assert(tap(c, 40, 200).restore && !c.visible()); // Back

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
const esp_partition_t* esp_partition_find_first(int, int, const char*) {
  return factoryMissing ? nullptr : &factory;
}
int esp_ota_set_boot_partition(const esp_partition_t* p) {
  assert(p == &factory); factorySelected = true; return ESP_OK;
}
[[noreturn]] void esp_restart() { throw Restart{}; }
namespace Display {
bool readTouch(int32_t& x, int32_t& y) { x = touchX; y = touchY; return fingerDown; }
void showRecoveryMenu(Recovery::Screen screen) { drawn = screen; ++draws; }
void showFailure(const String&, const String&) { ++failures; }
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
}
namespace BootDiag {
void recordRestartIntentIfNoneRecorded(RestartCause value) { cause = value; }
}
int main() {
  stateTests();
  integrationTests();
  std::cout << "PASS: recovery gesture, confirmation, timeouts, wrap, rendering handoff and actions\n";
  std::cout << "Controller RAM: " << sizeof(Recovery::Controller) << " bytes (host ABI)\n";
}
