#include "BootDiag.h"

#include <Preferences.h>
#include <esp_system.h>

#include "Log.h"

namespace BootDiag {
namespace {

/// NVS rather than RTC memory. This was RTC_NOINIT_ATTR and it did not work -
/// see BootDiag.h's own remarks for the capture that proved it. The short
/// version: every restart on this device is App -> CAL -> App, with CAL's
/// separate binary executing in between, and the recorded cause did not survive
/// that. NVS does, demonstrably, because it is how CAL's own updateRequested
/// flag crosses the same hop.
///
/// No magic word is needed here, unlike the RTC version: an absent NVS key
/// reads back as the supplied default, so "nothing recorded" is directly
/// representable and there is no uninitialised-noise case to defend against.
constexpr const char* kNvsNamespace = "bootdiag";
constexpr const char* kKeyCause = "cause";

/// The reason, in words, plus what it actually implies. The second half is the
/// point: `ESP_RST_TASK_WDT` means nothing to someone who has not just been
/// reading ESP-IDF headers, and a diagnostic that needs a second lookup to
/// interpret is half a diagnostic.
struct ResetDescription {
  const char* name;
  const char* meaning;
  bool unexpected;
};

ResetDescription describe(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:
      return {"POWERON_RESET", "power was applied - a cold start, not a restart", false};
    case ESP_RST_SW:
      return {"SOFTWARE_RESET", "this firmware asked for it - the cause beside this says what for",
              false};
    case ESP_RST_DEEPSLEEP:
      return {"DEEPSLEEP_RESET",
              "woke from deep sleep - this firmware never sleeps, so this is unexpected in itself",
              true};
    case ESP_RST_PANIC:
      return {"PANIC_RESET",
              "a crash: null dereference, stack canary, or a failed assertion. The backtrace went "
              "to serial only - see the [stack] min-free figures, which corroborate a canary trip",
              true};
    case ESP_RST_INT_WDT:
      return {"INT_WDT_RESET", "interrupt watchdog - interrupts were disabled or an ISR overran",
              true};
    case ESP_RST_TASK_WDT:
      return {"TASK_WDT_RESET",
              "task watchdog - a task held its core without yielding. loop() doing long "
              "synchronous work is the usual cause on this firmware",
              true};
    case ESP_RST_WDT:
      return {"WDT_RESET", "another watchdog fired", true};
    case ESP_RST_BROWNOUT:
      return {"BROWNOUT_RESET",
              "the supply voltage sagged - a weak cable, an underpowered port, or current draw at "
              "WiFi transmit peaks. NOT a software fault, and chasing it in code is wasted effort",
              true};
    case ESP_RST_SDIO:
      return {"SDIO_RESET", "reset over SDIO", true};
    case ESP_RST_EXT:
      return {"EXT_RESET",
              "an external pin reset it - the board's reset button, or esptool asserting EN. Note "
              "DTR/RTS drive EN and GPIO0 on this board (see the recovery procedure on "
              "/diag/firmware), so opening a serial port with DTR asserted lands here",
              false};
    default:
      return {"UNKNOWN_RESET", "esp_reset_reason() returned a value this firmware does not know",
              true};
  }
}

const char* describeCause(RestartCause cause) {
  switch (cause) {
    case RestartCause::None:
      return "NONE";
    case RestartCause::LowHeap:
      return "LOW_HEAP";
    case RestartCause::Ota:
      return "OTA";
    case RestartCause::Reprovision:
      return "REPROVISION";
    case RestartCause::SelfTest:
      return "SELF_TEST";
  }
  return "UNRECOGNISED";
}

/// Reads the recorded intent and clears it in the same NVS session, so the
/// read-then-clear pair costs one open instead of two. The clear is skipped
/// entirely when nothing was recorded, which is what keeps an ordinary
/// power-on boot free of flash writes.
RestartCause takeRecordedCause() {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, /*readOnly=*/false)) {
    // Not fatal and not silent: without this the reason pair would read
    // "+ NONE" and look like a missing recordRestartIntent() call somewhere,
    // sending the reader after a bug in the wrong file.
    Log::line("[boot] could not open NVS to read the recorded restart cause");
    return RestartCause::None;
  }

  const uint8_t stored = prefs.getUChar(kKeyCause, static_cast<uint8_t>(RestartCause::None));

  if (stored != static_cast<uint8_t>(RestartCause::None)) {
    prefs.putUChar(kKeyCause, static_cast<uint8_t>(RestartCause::None));
  }
  prefs.end();

  switch (static_cast<RestartCause>(stored)) {
    case RestartCause::None:
    case RestartCause::LowHeap:
    case RestartCause::Ota:
    case RestartCause::Reprovision:
    case RestartCause::SelfTest:
      return static_cast<RestartCause>(stored);
  }
  // A value this build does not recognise - most likely an older or newer
  // firmware wrote it. Reported as None rather than guessed at.
  return RestartCause::None;
}

}  // namespace

void recordRestartIntent(RestartCause cause) {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, /*readOnly=*/false)) {
    Log::line("[boot] could not open NVS to record the restart cause - the next boot will say NONE");
    return;
  }
  prefs.putUChar(kKeyCause, static_cast<uint8_t>(cause));
  // Explicit, not left to the destructor: this is called immediately before a
  // restart, and the commit has to have happened by the time the reset lands.
  prefs.end();
}

bool lastResetWasUnexpected() { return describe(esp_reset_reason()).unexpected; }

void logResetReason() {
  const esp_reset_reason_t reason = esp_reset_reason();
  const ResetDescription described = describe(reason);

  // Always taken, even on a power-on boot where the value is discarded below:
  // the clear has to happen regardless, or a recorded intent interrupted by a
  // power cut would sit in NVS and be attributed to some later restart.
  const RestartCause recorded = takeRecordedCause();

  // NVS outlives a power cycle, so a stored intent has to be suppressed here
  // rather than relied on to evaporate the way the old RTC copy did. If power
  // was applied, the reason the device came back up is that power was applied -
  // whatever it had been meaning to do beforehand did not cause this boot.
  const RestartCause cause = (reason == ESP_RST_POWERON) ? RestartCause::None : recorded;

  if (reason == ESP_RST_POWERON && recorded != RestartCause::None) {
    // Worth saying rather than swallowing: it means a deliberate restart was
    // recorded and then interrupted by a power cut before it completed.
    Log::printf("[boot] a %s restart was pending when power was lost - discarded",
                describeCause(recorded));
  }

  Log::printf("[boot] restart reason: %s + %s", described.name, describeCause(cause));
  Log::printf("[boot] %s", described.meaning);

  if (described.unexpected) {
    // Worth grepping for across a fleet: it separates "something went wrong"
    // from "we restarted on purpose", which have nothing in common
    // diagnostically.
    Log::line("[boot] that restart was NOT requested by this firmware");
  } else if (reason == ESP_RST_SW && cause == RestartCause::None) {
    // Now a trustworthy diagnosis. When the cause lived in RTC memory this
    // same line fired on restarts that HAD recorded an intent, because the
    // storage could not survive App -> CAL -> App - so it accused the call
    // sites of a fault that was really in this file. With the cause in NVS the
    // storage is no longer a suspect and the message means what it says.
    //
    // A deliberate restart that recorded no reason is a hole in this
    // instrumentation, not a property of the device - said out loud so it gets
    // fixed rather than shrugged at.
    Log::line(
        "[boot] a deliberate restart recorded no cause - some restart path is not calling "
        "BootDiag::recordRestartIntent()");
  }
}

}  // namespace BootDiag
