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
///
/// **That finding fired against this file's own first implementation, and the
/// storage had to change because of it.** The intent was originally held in
/// RTC memory (`RTC_NOINIT_ATTR`), on the reasoning that it survives a software
/// reset and a panic, costs no flash write, and correctly does not survive a
/// power cycle. Every part of that is true in isolation and it still did not
/// work, because of something specific to this device's boot architecture: the
/// App never restarts straight into itself. Every restart goes App -> CAL ->
/// App, since CAL is the permanent factory-partition loader and runs on every
/// boot. Two software resets, with a different binary executing in between.
/// Observed on device 17: `recordRestartIntent(Ota)` runs at the check-in
/// update branch, and the very next App boot reports `SOFTWARE_RESET + NONE`
/// - twice in a row in one serial capture.
///
/// So the intent now lives in NVS, which is the one mechanism already proven to
/// carry a value across that exact hop: it is how CAL's own
/// `Identity::updateRequested` flag gets from the App to CAL in the first
/// place. The flash write this costs is the price of the diagnostic working at
/// all, and it is bounded - a write happens only on a deliberate restart, and
/// the clear only when something was actually recorded, so an ordinary
/// power-on boot writes nothing.
namespace BootDiag {

/// What this firmware was trying to do when it asked for a restart.
///
/// Values are explicit and never renumbered: they are written to NVS by one
/// boot and read by the next, so a build that renumbers them mid-upgrade would
/// misreport the reason for exactly the restart that installed it.
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
/// Stored in NVS, not RTC memory - see this namespace's own remarks above for
/// the measurement that forced that, and why RTC's otherwise-correct
/// properties are defeated by every restart passing through CAL.
///
/// One consequence of NVS worth knowing: unlike RTC, it DOES survive a power
/// cycle. That is handled on the read side rather than the write side - a
/// `POWERON_RESET` reports `NONE` regardless of what is stored, because a
/// recorded intent that a power cut interrupted is not the reason the device
/// came back up.
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
/// OTA restart would leave `OTA` sitting in storage, and the next unrelated
/// panic would be reported as `PANIC_RESET + OTA` - inventing a causal link
/// that does not exist and sending whoever reads it after the wrong bug. With
/// NVS this matters more than it did with RTC, not less: the stored value now
/// outlives a power cycle, so nothing else would ever remove it.
void logResetReason();

/// True when the last restart was one this firmware did not ask for - a panic,
/// a watchdog, a brownout - as opposed to a power-on or a deliberate restart.
/// Lets a caller separate "we crashed" from "we rebooted on purpose" without
/// duplicating the mapping.
bool lastResetWasUnexpected();

}  // namespace BootDiag
