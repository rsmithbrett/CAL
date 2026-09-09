#pragma once

/// Time sync only - the App has no discovery document to fetch (see Config.h's
/// remarks on kManifestPath for why) and no enrollment concern of its own.
namespace AppService {

/// Same ordering requirement as CAL's Service::synchroniseTime: must happen
/// before any HTTPS call, since certificate validity checking needs a
/// plausible clock.
bool synchroniseTime();

/// Called from App.ino's checkHeapHealth() right before its own deliberate
/// esp_restart() - stashes the current, already-correct clock into the
/// ESP32's RTC slow-memory domain, which (unlike ordinary RAM) survives a
/// software reset. See trySkipSyncAfterFastReboot()'s own remarks for what
/// this buys the next boot.
void stashTimeForFastReboot();

/// Called from setup() before the blocking synchroniseTime() wait. A boot
/// that immediately follows one of this App's own deliberate esp_restart()
/// calls (the heap-health watchdog, currently the only caller of
/// stashTimeForFastReboot()) has a clock that was correct a few seconds
/// ago - unlike a genuine cold boot (dead battery, first flash, a real
/// power cycle), where the RTC domain itself has no power and there is
/// nothing to restore. Applies the stashed time immediately via
/// settimeofday() if a valid stash is found (consuming it - see this
/// function's own implementation for why exactly once), starts the real
/// SNTP client in the background for eventual precision, and returns true
/// so setup() can skip the blocking wait entirely. Returns false (nothing
/// to restore, or the stash was never written this power-on) when setup()
/// must fall back to the ordinary blocking synchroniseTime().
bool trySkipSyncAfterFastReboot();

}  // namespace AppService
