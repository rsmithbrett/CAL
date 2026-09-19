#pragma once

#include <Arduino.h>

/// CAL's half of the restart reason the App reports.
///
/// **The App owns this diagnostic.** `App/BootDiag.h` is where the two-part
/// reason - how the chip reset, and what the firmware was trying to achieve
/// when it asked - is explained at length, and the App is the only binary that
/// reads the recorded intent, clears it, and carries it on telemetry. CAL never
/// checks in and cannot report anything about itself, so it has no use for the
/// read side at all. What CAL has is restarts.
///
/// Every handover to the application is an `esp_restart()` (see
/// `Updater::bootApplication()`), and so is a candidate copying itself into
/// factory (`SelfInstall::applyCandidate()`). Those restarts ARE the App boot
/// that follows, which means the cause the App reports for that boot is CAL's
/// to supply - and until this file existed CAL supplied nothing, so the App
/// said:
///
///   [boot] restart reason: SOFTWARE_RESET + NONE
///   [boot] a deliberate restart recorded no cause - some restart path is not
///          calling BootDiag::recordRestartIntent()
///
/// Observed on device 17 on 2026-09-17 at about 02:03 UTC, on the App boot
/// immediately after CAL installed v2026.09.16.0003 and handed over. The
/// complaint surfaced in the App and the hole was here, which is the shape this
/// diagnostic will keep producing: the cause is written by one boot and read by
/// the next, so the binary that reports the gap is never the binary that left
/// it. The App had asked for nothing on that boot - CAL had found ota_0 empty
/// and gone and fetched an App on its own initiative - so nothing was in NVS to
/// find, and a restart CAL chose deliberately was reported as one nobody could
/// account for.
///
/// **This is a deliberate partial copy of App/BootDiag.h, not a shared
/// header.** The two sketches compile independently, each taking only the files
/// sitting beside it (see `ci/build-firmware.sh`), which is why `Identity`,
/// `Display` and `Tls` already exist twice in this repository. The enum values
/// below are the wire format between the two binaries - one build writes the
/// number, a different build reads it - so they MUST stay identical to
/// App/BootDiag.h's, and neither copy may ever renumber.
namespace BootDiag {

/// What the firmware was trying to do when it asked for a restart.
///
/// MUST MATCH App/BootDiag.h VALUE FOR VALUE. CAL writes these numbers into
/// NVS and the App reads them back after a reboot, so a value that means one
/// thing here and another there would misreport exactly the restart that
/// installed the mismatch. The App-only causes are listed rather than omitted
/// for the same reason: leaving a gap invites the next person to fill it.
enum class RestartCause : uint32_t {
  None = 0,
  /// App: the heap-health watchdog.
  LowHeap = 1,
  /// App: rebooting into CAL to install a firmware update.
  Ota = 2,
  /// App: returning to CAL to be reprovisioned.
  Reprovision = 3,
  /// App: a self-test build was requested for this device.
  SelfTest = 4,
  /// App: the server became unreachable and stayed that way.
  Unreachable = 5,
  /// App: check-in responses arrived and there was no heap to parse them.
  LowHeapResponse = 6,
  /// CAL finished its boot ladder and started the application that was already
  /// installed. Nothing was downloaded and nothing was wrong - this is the
  /// ordinary end of a CAL boot, and it is worth naming precisely because it is
  /// ordinary: an App boot reporting this restarted because CAL handed over,
  /// which is a different fact from an App that restarted itself.
  CalHandover = 7,
  /// CAL downloaded and installed an application image, then started it. The
  /// distinction from CalHandover is the whole point on a device that reboots
  /// unexpectedly: one says "CAL passed the baton", the other says "CAL
  /// replaced the binary first", and only the second explains a device that
  /// came back running something different.
  ///
  /// NOT the same as RestartCause::Ota, which means the App asked for an update
  /// and is recorded by the App before it hands back to CAL. This one is for an
  /// install CAL decided on by itself - no bootable App on the flash, boot
  /// attempts exhausted, or a version mismatch found on a boot the App never
  /// requested. That is exactly the case device 17 hit, and the case that had
  /// no cause at all.
  CalInstalledApp = 8,
  /// A staged CAL candidate copied itself into the factory partition, or is
  /// about to be booted so it can - see CAL_OTA_DESIGN.md section 10. Covers
  /// the whole trampoline rather than one hop of it: the chain is CAL -> booted
  /// candidate -> new CAL -> downloaded App, three restarts before an App runs
  /// again, and "CAL updated itself" is the one sentence that explains all of
  /// them to somebody reading a reboot heatmap.
  CalSelfInstall = 9,
};

/// Records what this restart is FOR, unless something has already recorded a
/// cause that has not been read yet.
///
/// **Why "unless" - the App's answer has to win.** A restart the App asked for
/// is App -> CAL -> App: the App records `Ota`, reboots into CAL, CAL does the
/// work, and CAL's own `esp_restart()` is what finally starts the App again.
/// CAL is the last writer on that path but the App is the one that knows why
/// any of it is happening, so a plain write here would replace `OTA` with
/// `CAL_INSTALLED_APP` on every single update and throw away the more specific
/// answer - turning a working diagnostic into a worse one while appearing to
/// fix something.
///
/// The rule this settles on is first-writer-wins, and it reads as a sentence:
/// the cause names what STARTED the restart chain, not the last hop of it.
/// Everything downstream of the first `esp_restart()` is mechanism. When
/// nothing has been recorded, CAL started the chain and this is where the cause
/// comes from.
///
/// The App clears the record when it reads it (`takeRecordedCause()`), so
/// "already recorded" can only mean an intent from a chain still in progress,
/// never a stale one from last week.
///
/// Call it as late as possible, once the restart is certain - a recorded intent
/// that is then not acted on would make the NEXT genuine crash report a cause
/// it had nothing to do with. `Updater::bootApplication()` calls it after
/// `esp_ota_set_boot_partition()` has succeeded, for precisely that reason.
void recordRestartIntentIfNoneRecorded(RestartCause cause);

}  // namespace BootDiag
