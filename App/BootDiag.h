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
  /// The server became unreachable and stayed that way, so the App restarted
  /// itself to get a working connection back.
  ///
  /// Specifically: repeated check-in failures where WiFi was associated and a
  /// plain TCP connect to the service succeeded, but the TLS handshake did not.
  /// mbedTLS needs 16,717 bytes contiguous for EACH of a session's two record
  /// buffers - not "roughly 32KB" once, which is how this comment used to read
  /// and which cost an evening of diagnosis before the difference was noticed.
  /// Http.h's kTlsRecordBufferBytes carries the arithmetic. The largest 8-bit
  /// block settles far below even 16,717 once cards have been rendering, so the
  /// handshake fails and nothing in the firmware resets the shared client - one
  /// failure is permanent for the life of the process. A restart is the only
  /// thing observed to recover it, on every occasion across two devices.
  ///
  /// A device in that state renders its cards perfectly and is invisible to the
  /// server: no telemetry, no check-in, no debug stream, and no way to receive
  /// a card policy or a firmware update. It looks healthy on the wall and is
  /// unmanageable. Naming this cause separately from LowHeap matters because
  /// the two want opposite investigations - one is "this device cannot draw",
  /// the other is "this device cannot be reached".
  Unreachable = 5,
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

/// What logResetReason() found, available after it has run.
///
/// Exists so a restart can influence the boot that follows it, which is the
/// only way a self-restarting watchdog can back off across reboots: the
/// variable holding "when did I last restart for this" does not survive the
/// restart, so without this the device would be eligible to restart again five
/// minutes into every boot forever. App.ino uses it to start the
/// unreachability watchdog's clock already running when the previous restart
/// was RestartCause::Unreachable.
///
/// Returns None before logResetReason() has been called, and None on a
/// power-on boot regardless of what was stored - see logResetReason() for why
/// a pending intent interrupted by a power cut is not the reason the device
/// came back up.
RestartCause lastRestartCause();

/// True when the last restart was one this firmware did not ask for - a panic,
/// a watchdog, a brownout - as opposed to a power-on or a deliberate restart.
/// Lets a caller separate "we crashed" from "we rebooted on purpose" without
/// duplicating the mapping.
bool lastResetWasUnexpected();

/// The same two-part reason `logResetReason()` prints, as one short token for
/// the wire: "POWERON_RESET+NONE", "PANIC_RESET+NONE",
/// "SOFTWARE_RESET+UNREACHABLE". Never null; reads "UNREPORTED+NONE" before
/// logResetReason() has run.
///
/// **This exists because the log is not a delivery mechanism, and measurement
/// proved it.** Everything above was already computed and printed, and on
/// 2026-09-11 the server had exactly zero restart causes on record for the
/// whole fleet's history. Two reasons compounded. `[boot]` lines are emitted in
/// setup(), before check-in has authorised the debug stream, so they are
/// dropped at the source. And on the two devices restarting most often the
/// stream was delivering 1.3% and 7.3% of the lines it produced - because a
/// debug POST needs TLS, TLS needs 16,717 bytes contiguous per record buffer,
/// and those devices sit at 8-9KB - short under any reading of the figure, but
/// the figure itself is 16,717 and not "roughly 32KB" (see Http.h). Of 38
/// restarts measured across five hours, 36 were
/// unattributable. A diagnostic that fails on exactly the devices that need
/// diagnosing is not a diagnostic.
///
/// Telemetry is the right carrier precisely because it is not the log: a small
/// fixed-size POST that still succeeds on these devices when the stream does
/// not, landing in a row the server keeps per device rather than in a buffer
/// that can be truncated.
///
/// Deliberately sent on EVERY telemetry report, not just the first after a
/// boot. The obvious economy - report it once, then stop - assumes the first
/// report gets through, and that assumption is what produced zero records. It
/// costs about 25 bytes on a request that already carries far more, and it
/// means the cause is learned on whichever POST first succeeds instead of being
/// lost with the one that did not.
const char* restartReasonToken();

}  // namespace BootDiag
