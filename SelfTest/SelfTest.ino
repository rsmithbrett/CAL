// SelfTest - a diagnostic firmware, delivered the same way App is.
//
// Built the night of 2026-09-06/07 after hours of correlating scattered
// telemetry/debug-log lines across two real firmware bugs (a PNG-decode
// read-buffer heap fragmentation issue and an Sd::begin() with no retry -
// both already fixed on main, see App/Display.cpp's and
// App/SdStorage.cpp's own remarks) with no dedicated way to directly ask
// "does the screen work, does the SD card work" independent of the normal
// App's incidental logging. See README.md's "SelfTest" section for the
// full brief this was built against.
//
// Delivered exactly the way App is: CAL's Updater::installApplication()
// fetches whatever the manifest says into ota_0 and installs it - this
// sketch requires ZERO changes to CAL, Updater.cpp, or the loader's control
// flow. A per-device manifest override (server-side, built concurrently in
// the DiscoverAroundMe repo) is what points one specific unit at this build
// instead of the fleet's normal App version; this sketch has no idea that
// override exists and does not need to.
//
// This is a tool, not a product: it never phones home for content, it just
// proves the hardware works and reports the results. It runs the same four
// test categories on a loop (or on a BOOT-button press) so a marginal SD
// card's intermittent behaviour has a chance to actually show up, rather
// than running once and leaving the device sitting on a single sample.

#include <WiFi.h>

// Overrides the ESP32 Arduino core's weak getArduinoLoopTaskStackSize() -
// same override, same reasoning, same doubled 16384 value App.ino already
// uses (see that file's own extensive remarks on the live "Guru Meditation
// Error ... Stack canary watchpoint triggered (loopTask)" crash this fixed
// there). This sketch's own call chains - HTTPClient + NetworkClientSecure +
// a JsonDocument in Report.cpp, SD file I/O with local buffers in
// SdTest.cpp - are shallower than App's multi-card rotation, but "shallower
// than the thing that actually crashed a device" is not the same claim as
// "safe at the 8192-byte default", and this override costs nothing to keep
// applying defensively. Must stay a plain global function, not inside any
// namespace - see App.ino's own remarks on why.
size_t getArduinoLoopTaskStackSize(void) {
  return 16384;
}

#include "Config.h"
#include "Display.h"
#include "Identity.h"
#include "Log.h"
#include "MemoryTest.h"
#include "Report.h"
#include "ScreenTest.h"
#include "SdTest.h"
#include "WifiJoin.h"

namespace {

// Same pin CAL/App both use for their own boot-time gestures - not reused
// for a WiFi-reset gesture here (this sketch has no captive portal and
// nothing to reset), only for "run the suite again right now" below.
constexpr uint8_t kBootButtonPin = 0;

/// True for exactly one loop() iteration per physical press - same
/// edge-detection technique App.ino's forceUpdateCheckRequested() uses, so
/// holding the button doesn't fire this repeatedly.
bool rerunRequested() {
  static bool wasPressed = false;
  const bool isPressed = digitalRead(kBootButtonPin) == LOW;
  const bool justPressed = isPressed && !wasPressed;
  wasPressed = isPressed;
  return justPressed;
}

/// Blocks until WiFi is up, retrying indefinitely. Unlike App.ino's own
/// copy of this idea, there is no reprovisioning escape hatch here - this
/// sketch is a diagnostic tool for an already-provisioned device (per the
/// brief it was built against) and has no captive-portal flow of its own to
/// fall back into. A unit with no working network simply keeps retrying and
/// says so on screen, which is itself useful diagnostic information about
/// this specific unit.
void ensureWifiConnected() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }
  while (!WifiJoin::joinStoredNetwork()) {
    Display::showFailure("Could not join WiFi", "Retrying... check the device is provisioned.");
    Log::line("[selftest] still not connected, retrying in 10s");
    delay(10000);
  }
}

/// Runs all four categories back to back, reports the results (logged in
/// full regardless of network state, best-effort POSTed if it is), and
/// leaves the pass/fail summary on screen. Returns the overall pass count
/// purely for a one-line Serial summary - nothing branches on it.
void runSuiteAndShowResults() {
  Log::line("[selftest] ===== starting test run =====");
  Display::showStatus("Running SelfTest", "Screen, SD, file I/O, memory");

  Display::showStatus("Testing screen", "Fills, text, PNG decode");
  const ScreenTest::Result screen = ScreenTest::run();
  Log::printf("[selftest] screen test: %s (%s)", screen.passed ? "PASS" : "FAIL",
              screen.detail.c_str());

  Display::showStatus("Testing SD card", "Mount, format round-trip, file I/O");
  const SdTest::Results sd = SdTest::run();
  Log::printf("[selftest] sd format test: mountedBefore=%d mountedAfter=%d formatSucceeded=%d",
              sd.format.mountedBefore, sd.format.mountedAfter, sd.format.formatSucceeded);

  Display::showStatus("Testing memory", "Allocating at stress sizes");
  const MemoryTest::Results memory = MemoryTest::run();

  Display::showStatus("Sending report", "Debug stream + server (best-effort)");
  const bool posted = Report::sendAndLog(screen, sd, memory);
  Log::printf("[selftest] report POST %s", posted ? "accepted" : "not accepted (see log above)");

  // Build the on-screen summary: one line per category, matching the
  // brief's "clearly, readably, one line per test category with
  // pass/fail/detail, not just 'done'" requirement.
  Display::ResultLine lines[6];
  uint8_t lineCount = 0;
  uint16_t totalRun = 0;
  uint16_t totalPassed = 0;

  lines[lineCount].label = "Screen";
  lines[lineCount].passed = screen.passed;
  lines[lineCount].detail = screen.detail;
  ++lineCount;
  ++totalRun;
  if (screen.passed) ++totalPassed;

  lines[lineCount].label = "SD format/reformat";
  lines[lineCount].passed = sd.format.formatSucceeded && sd.format.mountedBefore;
  lines[lineCount].detail = sd.format.mountedBefore
                                ? (sd.format.formatSucceeded ? "round-trip ok"
                                                              : sd.format.errorDetail)
                                : "SD.begin() failed - no card / mount failed";
  ++lineCount;
  ++totalRun;
  if (lines[lineCount - 1].passed) ++totalPassed;

  uint8_t fileIoPassed = 0;
  for (uint8_t i = 0; i < sd.fileIoCount; ++i) {
    ++totalRun;
    if (sd.fileIo[i].writeSucceeded && sd.fileIo[i].readSucceeded &&
        sd.fileIo[i].verifySucceeded) {
      ++fileIoPassed;
      ++totalPassed;
    }
  }
  lines[lineCount].label = "File I/O (" + String(sd.fileIoCount) + " sizes)";
  lines[lineCount].passed = (fileIoPassed == sd.fileIoCount);
  lines[lineCount].detail = String(fileIoPassed) + "/" + String(sd.fileIoCount) + " sizes verified";
  ++lineCount;

  uint8_t memPassed = 0;
  for (uint8_t i = 0; i < memory.count; ++i) {
    ++totalRun;
    if (memory.allocations[i].allocationSucceeded) {
      ++memPassed;
      ++totalPassed;
    }
  }
  lines[lineCount].label = "Memory alloc (" + String(memory.count) + " sizes)";
  lines[lineCount].passed = (memPassed == memory.count);
  lines[lineCount].detail = String(memPassed) + "/" + String(memory.count) +
                            " sizes allocated (largest attempted " +
                            String(memory.count > 0 ? memory.allocations[memory.count - 1].requestedBytes : 0) +
                            " bytes)";
  ++lineCount;

  Display::showResults(lines, lineCount, totalPassed, totalRun);
  Log::printf("[selftest] ===== run complete: %u/%u passed =====", totalPassed, totalRun);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  pinMode(kBootButtonPin, INPUT_PULLUP);

  Display::begin();
  Display::showStatus("SelfTest starting", "");

  Identity::begin();
  Log::printf("[selftest] booting, mac=%s", Identity::macAddress().c_str());

  ensureWifiConnected();
  WiFi.setAutoReconnect(true);

  runSuiteAndShowResults();
}

void loop() {
  ensureWifiConnected();
  Log::poll();

  static uint32_t lastRunMs = 0;
  const uint32_t now = millis();

  const bool timeForRerun = (now - lastRunMs) >= Config::kRerunIntervalMs;
  if (rerunRequested() || timeForRerun) {
    lastRunMs = now;
    runSuiteAndShowResults();
  }

  delay(50);
}
