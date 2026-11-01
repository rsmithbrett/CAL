#pragma once

#include <Arduino.h>

/// SelfTest's own compiled-in constants.
///
/// kServiceHost matches CAL and App's exactly - all three binaries call the
/// same server. Not shared as a single file the way Identity/Tls are, for
/// the same reason App/Config.h isn't shared with CAL's copy: nothing breaks
/// if a value here drifts, and each sketch directory is its own independent
/// arduino-cli compile unit anyway.
namespace Config {

static constexpr const char* kServiceHost = "api.discoveraroundme.com";

/// Where the finished report lands. The server maps this exact path
/// (FirmwareEndpoints.cs) and documents it in ApiEndpointDocs; it sits under
/// /api/firmware/ with the manifest and binary routes because it is part of
/// the same device-facing firmware surface.
///
/// This sketch has no discovery document to read a path from the way CAL does,
/// so a value here that the server does not map costs a reflash to correct. The
/// server-side test SelfTestReportWireAgreementTests reads this constant off
/// disk and compares it with the route, so the two cannot drift apart with both
/// trees' tests green.
static constexpr const char* kSelfTestReportPath = "/api/firmware/selftest/report";

static constexpr uint32_t kHttpTimeoutMs = 20000;
static constexpr uint32_t kWifiJoinTimeoutMs = 20000;
static constexpr uint8_t kWifiJoinAttempts = 3;

/// Where the clock comes from, and how long Clock::sync() waits for it. Two
/// servers because a single unreachable one costs the whole run its timestamps
/// and its ability to validate the server certificate.
static constexpr const char* kNtpPrimary = "pool.ntp.org";
static constexpr const char* kNtpSecondary = "time.nist.gov";
static constexpr uint32_t kSntpTimeoutMs = 30000;

/// Rejects an implausible clock. SNTP is unauthenticated, so a result before
/// this date is treated as a failure rather than as an answer - the same floor
/// and the same reasoning as CAL's own Config.h.
static constexpr time_t kEarliestPlausibleTime = 1767225600;  // 2026-01-01Z

/// How long a finished results screen stays up before the whole suite runs
/// again on its own - see SelfTest.ino's own remarks on why this loops
/// instead of running once. Long enough to actually read four lines of
/// pass/fail text from across a room; short enough that a marginal SD card's
/// intermittent behaviour shows up within a few minutes of watching, not
/// after leaving and coming back an hour later.
static constexpr uint32_t kRerunIntervalMs = 30UL * 1000UL;

/// Reported as `firmwareVersion` in every report body (see Report.cpp).
/// This sketch has no OTA-delivered version of its own the way App does
/// (Identity::installedAppVersion() reflects what the manifest installed
/// App as - a per-device SelfTest override, per this task's brief, does not
/// change that value) - so a plain compiled-in string is the honest answer
/// to "what build of what firmware is this", bumped by hand when this
/// sketch changes in a way worth telling the server apart from.
static constexpr const char* kSelfTestVersion = "selftest-1.0.0";

}  // namespace Config
