#pragma once

#include <Arduino.h>

/// The App's own compiled-in constants.
///
/// kServiceHost matches CAL's exactly - both binaries call the same server. This
/// is intentionally NOT shared as a single file the way Identity/Tls are: unlike
/// the NVS schema, nothing breaks if a value here drifts from CAL's copy, and the
/// two loaders' timeout/host constants are not required to move together.
namespace Config {

static constexpr const char* kServiceHost = "api.discoveraroundme.com";

/// Fixed by convention (see DeviceDiscoveryDocument.cs), not discovered at
/// runtime - unlike CAL, the App never fetches the well-known document. It only
/// ever needs this one path and the content routes it already knows by name.
static constexpr const char* kManifestPath = "/api/firmware/manifest";

static constexpr uint32_t kHttpTimeoutMs = 20000;
static constexpr uint32_t kSntpTimeoutMs = 30000;
static constexpr uint32_t kWifiJoinTimeoutMs = 20000;
static constexpr uint8_t kWifiJoinAttempts = 3;

/// Rejects an implausible clock, same rationale as CAL's own copy of this check.
static constexpr time_t kEarliestPlausibleTime = 1767225600;  // 2026-01-01Z

/// How often the running App re-fetches its card content and re-checks the
/// manifest for a newer version. Content is cached server-side already
/// (WeatherResult.FetchedAtUtc), so polling this often costs the device a
/// request, not the upstream provider a fresh call.
static constexpr uint32_t kContentRefreshIntervalMs = 10UL * 60UL * 1000UL;    // 10 minutes
static constexpr uint32_t kUpdateCheckIntervalMs = 60UL * 60UL * 1000UL;       // 1 hour

}  // namespace Config
