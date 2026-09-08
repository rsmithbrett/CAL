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

/// Where the finished report lands. Designed against the parallel server-side
/// task's brief rather than a live, confirmed endpoint - see Report.h for how
/// this sketch behaves if the shape or path turns out to differ once that
/// side is built.
static constexpr const char* kSelfTestReportPath = "/api/selftest/report";

static constexpr uint32_t kHttpTimeoutMs = 20000;
static constexpr uint32_t kWifiJoinTimeoutMs = 20000;
static constexpr uint8_t kWifiJoinAttempts = 3;

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
