#include "Clock.h"

#include <time.h>

#include "Config.h"
#include "Log.h"

namespace Clock {
namespace {

bool gSet = false;

}  // namespace

bool isSet() { return gSet; }

time_t nowUtc() {
  if (!gSet) {
    return 0;
  }
  return time(nullptr);
}

bool sync() {
  if (gSet) {
    return true;
  }

  // UTC with no daylight offset. Every timestamp this sketch sends is UTC, and
  // a local zone here would only have to be undone before serializing.
  configTime(0, 0, Config::kNtpPrimary, Config::kNtpSecondary);

  const uint32_t deadline = millis() + Config::kSntpTimeoutMs;
  while (millis() < deadline) {
    const time_t now = time(nullptr);
    // SNTP is unauthenticated, so a result before this sketch could have been
    // built is treated as "not synced yet" rather than as an answer. Without
    // the floor the unset epoch clock reads as a successful sync.
    if (now >= Config::kEarliestPlausibleTime) {
      gSet = true;
      Log::printf("[selftest-clock] synced: %s", toIso8601(now).c_str());
      return true;
    }
    delay(250);
  }

  Log::printf("[selftest-clock] no SNTP answer within %lums - the report will carry no "
              "timestamps and the POST is expected to fail certificate validation",
              static_cast<unsigned long>(Config::kSntpTimeoutMs));
  return false;
}

String toIso8601(time_t instant) {
  if (instant <= 0) {
    return String();
  }

  struct tm utc;
  if (gmtime_r(&instant, &utc) == nullptr) {
    return String();
  }

  char buffer[32];
  if (strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
    return String();
  }
  return String(buffer);
}

}  // namespace Clock
