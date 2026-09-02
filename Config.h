#pragma once

#include <Arduino.h>

/// The only two things compiled into CAL that name the outside world.
///
/// Everything else - endpoint paths, the QR target, timing parameters, branding
/// - is fetched from the discovery document at runtime. Anything hard-coded
/// here is fixed for the service life of the hardware, because CAL sits in the
/// factory partition and is only replaced over USB.
namespace Config {

/// A DNS name, deliberately not an address. An IP compiled into firmware
/// strands every unit the day the service moves; a name can be repointed. The
/// name must be one whose registration will not be lost.
static constexpr const char* kServiceHost = "api.discoveraroundme.com";

/// Conventional, stable by design. The discovery document it returns carries
/// the real endpoint paths, so none of those are compiled in.
static constexpr const char* kWellKnownPath = "/.well-known/discoveraroundme";

/// Access point name shown during WiFi provisioning. A suffix derived from the
/// MAC is appended at runtime - two devices in one household otherwise present
/// identical names and the phone silently picks one.
static constexpr const char* kSetupApPrefix = "Discover-Setup";

/// Defaults only. The discovery document overrides all of these, because a
/// value that proves wrong in the field cannot be corrected in firmware.
static constexpr uint32_t kHttpTimeoutMs = 20000;
static constexpr uint32_t kSntpTimeoutMs = 30000;
static constexpr uint32_t kWifiJoinTimeoutMs = 20000;
static constexpr uint8_t kWifiJoinAttempts = 3;

/// How long Provisioning::run() waits for a phone to submit credentials before
/// giving up. Long enough that finding glasses and typing a passphrase is not
/// a race, short enough that a household that walked away does not leave the
/// device holding its access point open indefinitely - every minute spent
/// broadcasting an open AP is a minute the device is not doing its job.
static constexpr uint32_t kProvisioningAbandonTimeoutMs = 15UL * 60UL * 1000UL;  // 15 minutes

/// Rejects an implausible clock. SNTP is unauthenticated, so a result before
/// the build date is treated as a failure rather than as an answer.
static constexpr time_t kEarliestPlausibleTime = 1767225600;  // 2026-01-01Z

}  // namespace Config
