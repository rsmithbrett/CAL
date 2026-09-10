#pragma once

/// Whether a newer build than the one currently running has been published.
///
/// Deliberately much smaller than CAL's Updater: the App only ever needs to
/// decide whether to call Loader::requestUpdate() - the sha256/size
/// verification and the actual flash write stay exclusively CAL's job (see
/// Loader.h), so this file doesn't duplicate that logic.
namespace AppUpdater {

/// The three genuinely different answers to "should I update?".
///
/// **This used to be a bool, and the missing third state was a real bug.**
/// Every failure path - TLS setup, a request that would not begin, a non-200,
/// a response that would not parse - returned false, and the BOOT-button
/// handler in App.ino renders false as "already up to date" on screen and in
/// the log. So a device that could not reach the server at all told whoever
/// walked over and pressed the button that everything was fine. Observed on
/// device 7, which reported "up to date" while running a build four versions
/// behind the fleet, having last reached the server four hours earlier.
///
/// That is the worst available answer at exactly the wrong moment: the button
/// exists for someone standing at the device wanting to force an update, and a
/// silent "all good" sends them away believing the opposite of the truth.
/// "Could not check" and "nothing to install" have to be distinguishable, so
/// they are separate values rather than one falsy one.
enum class UpdateCheck {
  /// A newer build is published and this device should hand off to CAL.
  Newer,
  /// The server was reached and answered; this device is already current.
  UpToDate,
  /// The question could not be answered - network, TLS, HTTP or parse failure.
  /// NOT the same as UpToDate, and must never be reported as it.
  CheckFailed,
};

/// Asks the server what it should be running. Never throws and never blocks
/// longer than Config::kHttpTimeoutMs; failures come back as
/// UpdateCheck::CheckFailed with the specific reason already logged.
UpdateCheck checkForUpdate();

}  // namespace AppUpdater
