#include "BootDiag.h"

#include <Preferences.h>

#include "Journal.h"

namespace BootDiag {
namespace {

/// The same NVS namespace and key App/BootDiag.cpp uses, and they have to be
/// character-for-character identical: this is a message from one binary to
/// another, and NVS has no way to tell you the reader looked somewhere else.
/// CAL's own Identity namespace ("cal") is deliberately not reused - the App
/// cannot open it by that name, and the whole value of this record is that the
/// App can read it.
constexpr const char* kNvsNamespace = "bootdiag";
constexpr const char* kKeyCause = "cause";

/// The cause in words, for the journal only - never for the glass. Everything
/// in this file happens in the last moments before a restart, and putting a
/// diagnostic about diagnostics on the screen would replace a household's
/// "Starting" message with a sentence written for whoever is holding a USB
/// cable.
///
/// The spellings match App/BootDiag.cpp's describeCause() so a journal read
/// over USB and a telemetry row read on the server use the same words for the
/// same restart.
const char* describe(RestartCause cause) {
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
    case RestartCause::Unreachable:
      return "UNREACHABLE";
    case RestartCause::LowHeapResponse:
      return "LOW_HEAP_RESPONSE";
    case RestartCause::CalHandover:
      return "CAL_HANDOVER";
    case RestartCause::CalInstalledApp:
      return "CAL_INSTALLED_APP";
    case RestartCause::CalSelfInstall:
      return "CAL_SELF_INSTALL";
  }
  // A value neither copy of the enum knows. Reported rather than guessed at,
  // and reachable only if an App newer than this CAL wrote one.
  return "UNRECOGNISED";
}

}  // namespace

void recordRestartIntentIfNoneRecorded(RestartCause cause) {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, /*readOnly=*/false)) {
    // Said out loud rather than swallowed. Without this the App's next boot
    // reports "+ NONE" and its own message sends the reader hunting for a
    // missing call site, when the call was made and the storage refused it.
    Journal::printf("[boot] NVS would not open to record restart cause %s - the App's "
                    "next boot will wrongly report NONE",
                    describe(cause));
    return;
  }

  // Read before writing, which is the whole behaviour this function is named
  // for. See BootDiag.h: on any restart the App asked for, the App recorded a
  // more specific cause before handing back to CAL, and CAL must not spend the
  // record on the fact that it was CAL that pressed the button.
  const uint8_t existing = prefs.getUChar(kKeyCause, static_cast<uint8_t>(RestartCause::None));
  if (existing != static_cast<uint8_t>(RestartCause::None)) {
    prefs.end();
    // Kept short on purpose: Journal::printf formats into a fixed 160-byte
    // scratch and truncates past it. The reasoning lives in BootDiag.h; this
    // line only has to say which answer won.
    Journal::printf("[boot] restart cause %s already recorded - kept, not replaced by %s",
                    describe(static_cast<RestartCause>(existing)), describe(cause));
    return;
  }

  prefs.putUChar(kKeyCause, static_cast<uint8_t>(cause));
  // Explicit, not left to the destructor: this is called immediately before a
  // restart and the commit has to have happened by the time the reset lands.
  prefs.end();
  Journal::printf("[boot] restart cause recorded as %s - the App's next boot reports that "
                  "rather than an unexplained software reset",
                  describe(cause));
}

}  // namespace BootDiag
