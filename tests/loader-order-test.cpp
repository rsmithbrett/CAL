// The order in which App/Loader.cpp commits its NVS flags, and which flags each
// of its three paths commits at all.
//
// A sequence rather than a final state, because the final state is reached only
// if the device keeps power through both commits. Every write is recorded in
// order and the assertions read that record. See TEST_PLAN.md section 16d.

#include <algorithm>
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "Loader.h"
#include "Identity.h"
#include "Log.h"
#include <esp_ota_ops.h>

namespace {

std::vector<std::string> writes;
bool updateRequestedFlag = false;
bool configMenuFlag = false;
bool secretHeld = true;
bool factoryMissing = false;
const esp_partition_t factory;

struct Restarted {};

void run(void (*path)()) {
  writes.clear();
  updateRequestedFlag = false;
  configMenuFlag = false;
  secretHeld = true;
  try {
    path();
    assert(false);  // every path ends in esp_restart()
  } catch (const Restarted&) {
  }
}

}  // namespace

const esp_partition_t* esp_partition_find_first(int, int, const char*) {
  return factoryMissing ? nullptr : &factory;
}
int esp_ota_set_boot_partition(const esp_partition_t* p) {
  assert(p == &factory);
  writes.push_back("boot=factory");
  return ESP_OK;
}
[[noreturn]] void esp_restart() { throw Restarted{}; }

namespace Identity {
void setUpdateRequested(bool requested) {
  updateRequestedFlag = requested;
  writes.push_back(requested ? "updreq=1" : "updreq=0");
}
void setConfigMenuRequested(bool requested) {
  configMenuFlag = requested;
  writes.push_back(requested ? "cfgmenu=1" : "cfgmenu=0");
}
bool configMenuRequested() { return configMenuFlag; }
void clearSecret() {
  secretHeld = false;
  writes.push_back("secret=cleared");
}
}  // namespace Identity

namespace Log {
void line(const char*) {}
// Recorded in the same sequence as the flag writes, so the assertions below
// pin where it sits: bootFactoryAndRestart() selects the partition and then
// flushes, which is the last thing to happen before esp_restart().
void flushNow() { writes.push_back("flush"); }
}  // namespace Log

int main() {
  // UPDREQ IS COMMITTED FIRST. The two flags are separate NVS commits and the
  // boot partition is still the App's until the selection below, so a power cut
  // between them comes back running this same App holding whichever one landed.
  // updreq alone is cleared by CAL on its next boot - installApplication() after
  // an install, CAL.ino where the server confirms the installed version is
  // current. cfgmenu alone is cleared by nothing in the App, so it waits through
  // every later App boot and then opens configuration at an unattended device.
  run(&Loader::requestConfigMenu);
  const std::vector<std::string> expected = {"updreq=1", "cfgmenu=1", "boot=factory",
                                             "flush"};
  assert(writes == expected);
  assert(writes[0] == "updreq=1" && writes[1] == "cfgmenu=1");
  assert(updateRequestedFlag && configMenuFlag && secretHeld);

  // The partition is selected after both flags, so an interrupted pair never
  // comes back into CAL with a half-written request.
  const auto boot = std::find(writes.begin(), writes.end(), "boot=factory");
  const auto cfg = std::find(writes.begin(), writes.end(), "cfgmenu=1");
  assert(cfg < boot);

  // The other two paths through bootFactoryAndRestart(), so a change to the
  // shared tail is measured against all three callers.
  run(&Loader::requestUpdate);
  assert(writes == std::vector<std::string>({"updreq=1", "boot=factory", "flush"}));
  assert(!configMenuFlag);

  run(&Loader::returnToLoaderForReprovisioning);
  assert(writes ==
         std::vector<std::string>({"secret=cleared", "updreq=1", "boot=factory", "flush"}));
  assert(!secretHeld && !configMenuFlag);

  // A missing factory partition restarts anyway rather than hanging, and the
  // flags are already committed by then.
  factoryMissing = true;
  run(&Loader::requestConfigMenu);
  assert(writes == std::vector<std::string>({"updreq=1", "cfgmenu=1", "flush"}));
  factoryMissing = false;

  std::cout << "PASS: loader NVS write order, all three paths and a missing factory partition\n";
}
