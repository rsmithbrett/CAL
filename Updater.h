#pragma once

#include <Arduino.h>

#include "BootDiag.h"
#include "Service.h"

/// Fetching and installing the application image.
///
/// This is the reason CAL exists. The application occupies the single OTA slot
/// and therefore cannot rewrite itself - it can only ask, by way of an NVS flag
/// and a reboot, for CAL to do it.
namespace Updater {

struct Manifest {
  bool ok = false;
  bool isConfigured = false;  // false when no build has been marked current
  String version;
  String sha256;              // lower-case hex, verified before the image is committed
  uint32_t sizeBytes = 0;
};

Manifest fetchManifest(const Service::Discovery& discovery, const String& pathOverride = String());

/// Downloads into the OTA partition and verifies the manifest's hash before
/// committing. Returns false without disturbing the installed image on any
/// failure - a half-written partition is never marked bootable.
bool installApplication(const Service::Discovery& discovery, const Manifest& manifest,
                        bool asCalCandidate = false);

/// Caches the brand splash into LittleFS. Purely cosmetic and never fatal:
/// failure leaves the neutral splash in place.
bool cacheBrandAssets(const Service::Discovery& discovery);

/// Hands control to whatever is in `ota_0`. Records a boot attempt first, so an
/// application that never reaches steady state is eventually recognised as bad
/// rather than retried forever. Does not return on success.
///
/// `describedAs` names what is actually being booted, for the journal only. It
/// exists because this is also how a staged CAL candidate is started, and with
/// nothing passed the log line reads the App version out of nvs - which during a
/// CAL update names an App that was overwritten by the candidate a moment
/// earlier. On 2026-09-16 that produced "handing over to 'v2026.09.16.0003'"
/// immediately before a CAL booted, in the one log anybody was reading.
///
/// `cause` is the other half of that, and it goes somewhere `describedAs` never
/// could. The journal line is on flash and needs a USB cable to read; the cause
/// is written to NVS, read by the App on the boot this restart produces, and
/// carried on every telemetry report from then on - so it is how a device that
/// rebooted in somebody's hallway gets diagnosed without anybody visiting it.
/// The default is deliberately the ordinary case rather than no cause at all:
/// this function is the only `esp_restart()` in CAL's handover path, so
/// recording HERE rather than at each call site is what makes it impossible for
/// a future caller to restart the device without saying why. That was the
/// actual defect - device 17 reported `SOFTWARE_RESET + NONE` on 2026-09-17
/// because CAL installed an App on its own initiative and handed over silently.
///
/// Pass something more specific when the caller knows more (an install just
/// happened, or this is a CAL candidate rather than an App). Passing the App's
/// own causes is neither needed nor possible to get wrong: see
/// BootDiag::recordRestartIntentIfNoneRecorded() for why an intent the App
/// already recorded is left exactly as it is.
void bootApplication(const char* describedAs = nullptr,
                     BootDiag::RestartCause cause = BootDiag::RestartCause::CalHandover);

/// True when an application image is present and has not exhausted its boot
/// attempts.
bool haveBootableApplication();

}  // namespace Updater
