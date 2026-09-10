#include "BootDiag.h"

#include <esp_system.h>

#include "Log.h"

namespace BootDiag {
namespace {

/// RTC_NOINIT_ATTR: kept in RTC memory and deliberately NOT zeroed by the
/// startup code, which is the whole point - it has to survive a software reset
/// and a panic. It does not survive a power cycle, and that is correct: a
/// power cycle reports POWERON_RESET and has no earlier intent to carry.
///
/// The magic word is what makes the difference between "no intent was
/// recorded" and "this is uninitialised RTC noise from a cold boot"
/// detectable. Without it, garbage in the cause word on the very first boot
/// after power-on would be reported as some arbitrary cause.
RTC_NOINIT_ATTR uint32_t gCauseMagic;
RTC_NOINIT_ATTR uint32_t gCauseValue;

constexpr uint32_t kCauseMagic = 0x43414C31;  // "CAL1"

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

/// Reads the recorded intent, treating an invalid magic as None.
RestartCause readRecordedCause() {
  if (gCauseMagic != kCauseMagic) {
    return RestartCause::None;
  }
  switch (static_cast<RestartCause>(gCauseValue)) {
    case RestartCause::None:
    case RestartCause::LowHeap:
    case RestartCause::Ota:
    case RestartCause::Reprovision:
    case RestartCause::SelfTest:
      return static_cast<RestartCause>(gCauseValue);
  }
  // A value this build does not recognise - most likely an older or newer
  // firmware wrote it. Reported as None rather than guessed at.
  return RestartCause::None;
}

}  // namespace

void recordRestartIntent(RestartCause cause) {
  gCauseMagic = kCauseMagic;
  gCauseValue = static_cast<uint32_t>(cause);
}

bool lastResetWasUnexpected() { return describe(esp_reset_reason()).unexpected; }

void logResetReason() {
  const esp_reset_reason_t reason = esp_reset_reason();
  const ResetDescription described = describe(reason);
  const RestartCause cause = readRecordedCause();

  // Cleared immediately after reading, and before anything else can restart
  // the device. Not tidying - correctness: a recorded OTA intent left in place
  // would make the next unrelated panic report PANIC_RESET + OTA, inventing a
  // causal link and sending the reader after the wrong bug.
  gCauseMagic = 0;
  gCauseValue = static_cast<uint32_t>(RestartCause::None);

  Log::printf("[boot] restart reason: %s + %s", described.name, describeCause(cause));
  Log::printf("[boot] %s", described.meaning);

  if (described.unexpected) {
    // Worth grepping for across a fleet: it separates "something went wrong"
    // from "we restarted on purpose", which have nothing in common
    // diagnostically.
    Log::line("[boot] that restart was NOT requested by this firmware");
  } else if (reason == ESP_RST_SW && cause == RestartCause::None) {
    // A deliberate restart that recorded no reason is a hole in this
    // instrumentation, not a property of the device - said out loud so it gets
    // fixed rather than shrugged at.
    Log::line(
        "[boot] a deliberate restart recorded no cause - some restart path is not calling "
        "BootDiag::recordRestartIntent()");
  }
}

}  // namespace BootDiag
