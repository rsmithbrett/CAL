#include "AppService.h"

#include <Arduino.h>
#include <time.h>

#include "Config.h"

namespace AppService {

bool synchroniseTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  const uint32_t deadline = millis() + Config::kSntpTimeoutMs;
  while (millis() < deadline) {
    const time_t now = time(nullptr);
    if (now > Config::kEarliestPlausibleTime) {
      return true;
    }
    delay(250);
  }
  return false;
}

}  // namespace AppService
