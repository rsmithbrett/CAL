#pragma once

#include <Arduino.h>

/// Phase 2 of the CAL-over-the-air design: a CAL running from `ota_0` copying
/// itself into `factory`. See CAL_OTA_DESIGN.md section 10 for the full design
/// and, more importantly, for the power-cut table.
///
/// **THE INVARIANT THIS MODULE EXISTS TO PRESERVE.** `otadata` names a partition
/// holding a whole, bootable image at every instant. Everything here follows from
/// that: the copy completes and is verified before `otadata` moves, `ota_0` is
/// cleaned up by a different boot than the one that copied it, and the survivor
/// of any interruption needs nothing but power - no network, no TLS, no card.
///
/// **Why a CAL would be in `ota_0` at all.** Because that is the only partition
/// firmware can write without overwriting the code doing the writing. An ESP32
/// app image is partition-agnostic, verified on device 17 on 2026-09-16: CAL ran
/// from `0x170000` and reached its whole ladder. So a new CAL arrives the way a
/// new App does - downloaded into `ota_0` by the CAL already running - and then
/// has to walk itself home.
namespace SelfInstall {

/// Whether this CAL is executing from `ota_0` rather than `factory`, which makes
/// it a candidate. Cheap, and safe to call before anything is initialised.
bool runningAsCandidate();

/// The candidate's whole job. Copies `ota_0` into `factory`, verifies it, marks
/// `ota_0` for cleanup by the next boot, erases `otadata` and restarts.
///
/// Does not return on success - it restarts the device. Returns false when it
/// refused or failed, having left `otadata` untouched, which means the next boot
/// is this same candidate and it can try again. That is the design's safety
/// property rather than a convenience.
bool applyCandidate();

/// Called once at boot by a CAL running from `factory`. If the previous boot was
/// a candidate that finished, this erases `ota_0`'s image header so the ordinary
/// ladder does not mistake a CAL sitting there for an installable App and hand
/// over to it - which would produce another candidate, and a boot loop.
///
/// Safe and almost free when no cleanup is pending, which is every ordinary boot.
void cleanUpAfterCandidate();

}  // namespace SelfInstall
