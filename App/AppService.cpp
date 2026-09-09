#include "AppService.h"

#include <Arduino.h>
#include <sys/time.h>
#include <time.h>

#include "Config.h"
#include "Log.h"

namespace AppService {
namespace {

/// A fixed value with no reason to occur naturally, distinguishing "this was
/// actually written by stashTimeForFastReboot() this power-on" from
/// whatever bit pattern the RTC domain happens to power up with. The RTC
/// slow-memory domain survives esp_restart() (a software reset) but is NOT
/// backed by battery or supercap here - a genuine power loss (unplugged, EN
/// pin held low, a dead/disconnected supply) de-energises it too, and it
/// comes back as undefined content, not zeroed. Checking only "is
/// gStashedTimeUtc nonzero" would risk treating that undefined content as a
/// real timestamp on the rare cold boot where it happens to look plausible;
/// requiring this exact magic value alongside it makes that a
/// once-in-four-billion coincidence rather than a real risk.
constexpr uint32_t kStashMagic = 0xC0A57A57;  // "CoAsTaSt" - arbitrary, just distinctive

RTC_DATA_ATTR uint32_t gStashMagic = 0;
RTC_DATA_ATTR time_t gStashedTimeUtc = 0;

}  // namespace

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

void stashTimeForFastReboot() {
  gStashedTimeUtc = time(nullptr);
  gStashMagic = kStashMagic;
}

bool trySkipSyncAfterFastReboot() {
  if (gStashMagic != kStashMagic) {
    // Either a genuine cold boot (RTC domain never energised, or energised
    // with unrelated content - see kStashMagic's own remarks), or a normal
    // boot that was never preceded by stashTimeForFastReboot() at all (a
    // fresh flash, CAL handing over after an OTA install, a plain power
    // cycle). Nothing to restore.
    return false;
  }

  // Consumed exactly once: cleared immediately, before anything below can
  // fail or this function can be short-circuited, so a boot that stalls
  // somewhere between here and its own next real stash never replays this
  // same reading again on some later, unrelated restart.
  gStashMagic = 0;
  const time_t restored = gStashedTimeUtc;

  if (restored <= Config::kEarliestPlausibleTime) {
    // Should not happen - stashTimeForFastReboot() is only ever called
    // after a boot that has already synchronised time successfully - but
    // this is the one place a corrupt or implausible stash would otherwise
    // silently feed a wrong clock into TLS certificate validation. Fall
    // back to the ordinary blocking sync rather than trust it.
    return false;
  }

  struct timeval tv = {.tv_sec = restored, .tv_usec = 0};
  settimeofday(&tv, nullptr);
  Log::printf("[time] restored %lu from before the last restart - skipping the blocking SNTP wait",
              static_cast<unsigned long>(restored));

  // Still worth getting a precise answer, just not on the critical path
  // this function exists to shorten: configTime() only starts the SNTP
  // client (DNS lookup, one UDP round trip when it lands) and returns
  // immediately - it does not block waiting for a response the way the
  // polling loop in synchroniseTime() above does. Whatever it converges to
  // corrects the handful of seconds this restored value is already off by,
  // silently, the next time anything reads the clock.
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  return true;
}

}  // namespace AppService
