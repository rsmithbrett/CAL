#pragma once

/// The App's half of the handoff back to CAL.
///
/// The App cannot write the partition it is executing from (see partitions.csv
/// - a single OTA slot means there is nowhere else to write it to), so it can
/// never install an update itself. What it CAN do is point the next boot at
/// the factory partition and restart - CAL then runs, sees Identity's flag or
/// its own missing-application check, and does the actual download.
namespace Loader {

/// Sets Identity::updateRequested and reboots into CAL to fetch a newer build.
/// Never returns.
[[noreturn]] void requestUpdate();

/// Forgets the locally stored device secret, then reboots into CAL (with the
/// same update-requested flag requestUpdate() sets, so CAL does its own WiFi
/// join rather than handing straight back to this same App) after the server
/// has rejected this device's secret. CAL tries its normally-remembered
/// network first, same as any other boot - this does not force the
/// captive-portal network-setup flow, since a rejected secret says nothing
/// about which network the device is on. Forgetting the secret is what makes
/// the next CAL boot actually re-enroll instead of presenting the same
/// already-rejected value forever - see the .cpp for the loop this closes.
/// Never returns.
[[noreturn]] void returnToLoaderForReprovisioning();

}  // namespace Loader
