#include "AppService.h"

#include <Arduino.h>
#include <time.h>

#include "Config.h"
#include "Log.h"

namespace AppService {

bool synchroniseTime() {
  Log::line("[time] starting SNTP sync");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  const uint32_t deadline = millis() + Config::kSntpTimeoutMs;
  while (millis() < deadline) {
    const time_t now = time(nullptr);
    if (now > Config::kEarliestPlausibleTime) {
      Log::line("[time] SNTP sync succeeded");
      return true;
    }
    delay(250);
  }
  Log::line("[time] SNTP sync timed out");
  return false;
}

}  // namespace AppService
