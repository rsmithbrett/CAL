#pragma once

#include <Arduino.h>

/// Why this device restarted, in two parts: how the chip reset, and what this
/// firmware was trying to achieve when it asked.
///
/// **Why one part is not enough.** `esp_reset_reason()` alone answers "how" -
/// power-on, software, panic, brownout - and that already separates four
/// diagnoses that used to be indistinguishable from the server, where the only
/// evidence was `totalBoots` going up. But it flattens every deliberate restart
/// into a single `SOFTWARE_RESET`, and this firmware has several for entirely
/// unrelated reasons: the heap watchdog, an OTA install, and a return to CAL
/// for reprovisioning. Knowing a restart was deliberate without knowing which
/// one it was is barely better than not knowing.
///
/// The obvious answer - "read the log line the firmware printed just before it
/// restarted" - is exactly wrong, and was the first version of this file. That
/// line may never have left the device: the remote stream is flushed on a
/// timer, so a restart is precisely the moment a log line is most likely to be
/// lost, and a crash-adjacent one certainly is. The intent has to be recorded
/// somewhere that survives the reboot, not narrated into a buffer that may not.
///
/// So the reason reads as a pair:
///
///   POWERON_RESET  + NONE       power was applied; nothing to explain
///   SOFTWARE_RESET + LOW_HEAP   the heap watchdog restarted us
///   SOFTWARE_RESET + OTA        restarting to install an update
///   PANIC_RESET    + NONE       we crashed; no intent was recorded, by
///                               definition, because nobody meant to
///   BROWNOUT_RESET + NONE       the supply sagged; not a software fault
///
/// A `NONE` beside a `SOFTWARE_RESET` is itself a finding worth acting on: it
/// means something restarted this device deliberately without recording why,
/// which is a gap in this instrumentation rather than a property of the device.
namespace BootDiag {

/// What this firmware was trying to do when it asked for a restart.
///
/// Values are explicit and never renumbered: they are written to RTC memory by
/// one boot and read by the next, so a build that renumbers them mid-upgrade
/// would misreport the reason for exactly the restart that installed it.
enum class RestartCause : uint32_t {
  None = 0,
  /// The heap-health watchdog in App.ino decided the device could no longer
  /// draw its cards.
  LowHeap = 1,
  /// Rebooting into CAL to install a firmware update.
  Ota = 2,
  /// Returning to CAL to be reprovisioned - new WiFi, new owner, factory
  /// reset.
  Reprovision = 3,
  /// A self-test build was requested for this device.
  SelfTest = 4,
};

/// Records what this restart is FOR, immediately before calling
/// `esp_restart()` (or handing off to Loader, which restarts on this
/// firmware's behalf).
///
/// Stored in RTC memory rather than NVS, deliberately. RTC survives a software
/// reset and a panic but not a power cycle - which is exactly the desired
/// behaviour, because a power cycle reports `POWERON_RESET` and there is no
/// earlier intent worth carrying into it. It also costs no flash write, and a
/// restart path is the last place to want one: NVS wear and a slow commit on
/// the way out of a crash-adjacent reboot buy nothing.
///
/// Call it as late as possible, once the restart is certain. A recorded intent
/// that is then not acted on would make the NEXT genuine crash report a cause
/// it had nothing to do with.
void recordRestartIntent(RestartCause cause);

/// Logs the two-part reason for the restart that just happened, and then
/// clears the recorded intent.
///
/// Call once, early in `setup()`, before anything else can obscure it.
///
/// **The clear is not tidying, it is correctness.** Without it, a deliberate
/// OTA restart would leave `OTA` sitting in RTC memory, and the next
/// unrelated panic would be reported as `PANIC_RESET + OTA` - inventing a
/// causal link that does not exist and sending whoever reads it after the
/// wrong bug.
void logResetReason();

/// True when the last restart was one this firmware did not ask for - a panic,
/// a watchdog, a brownout - as opposed to a power-on or a deliberate restart.
/// Lets a caller separate "we crashed" from "we rebooted on purpose" without
/// duplicating the mapping.
bool lastResetWasUnexpected();

}  // namespace BootDiag
