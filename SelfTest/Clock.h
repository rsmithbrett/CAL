#pragma once

#include <Arduino.h>

/// Wall-clock time for this sketch, from SNTP.
///
/// Two things need it. The report carries startedAtUtc and completedAtUtc
/// (SelfTestReportRequest), which are absolute instants and cannot be derived
/// from millis(). And the POST validates the server's certificate chain against
/// the embedded root bundle, which checks the certificate's validity window
/// against the system clock; an ESP32 that has not synced starts at the epoch,
/// and a 1970 clock puts every live certificate in the future.
///
/// So a run with no sync still runs its four categories and still logs them in
/// full to the debug stream. What it cannot do is produce a report the server
/// will store. sync() reports which of those happened rather than halting.
namespace Clock {

/// Blocks until SNTP returns a time at or after Config::kEarliestPlausibleTime,
/// or until Config::kSntpTimeoutMs elapses. Call after WiFi is up. Returns
/// whether the clock is now set; safe to call again on a later run, and returns
/// true immediately once it is.
bool sync();

/// Whether the clock has been set to a plausible time.
bool isSet();

/// Seconds since the Unix epoch, UTC. Zero when the clock is not set.
time_t nowUtc();

/// An instant as ISO 8601 with a Z offset, for example
/// "2026-11-01T14:02:11Z" - the shape SelfTestReportRequest's two
/// DateTimeOffset fields parse. Empty string when the clock is not set, so a
/// caller omits the member rather than sending year one.
String toIso8601(time_t instant);

}  // namespace Clock
