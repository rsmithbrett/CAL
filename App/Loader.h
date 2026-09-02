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

/// Reboots into CAL with no update request set - CAL falls back to its
/// own network-provisioning flow, the App's only path back to that flow
/// since it has no captive-portal code of its own. Never returns.
[[noreturn]] void returnToLoaderForReprovisioning();

}  // namespace Loader
