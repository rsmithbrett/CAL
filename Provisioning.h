#pragma once

#include <Arduino.h>

/// Phase one of enrollment: getting the device onto the household network.
///
/// A device from the factory holds no credentials and cannot be reached over a
/// network, so this part is unavoidably local. The device raises its own access
/// point and shows a code that joins a phone to it directly.
namespace Provisioning {

/// Raises the access point, serves the portal, and blocks until credentials
/// have been accepted and saved. Returns false if the household abandoned the
/// process long enough that a restart is more useful than continuing to wait.
bool run();

/// Attempts to join with stored credentials. Retries, because a single attempt
/// after boot times out often enough on a correct password - especially right
/// after a power cut - that treating one failure as wrong credentials would
/// send working devices back to provisioning.
bool joinStoredNetwork();

/// Whether the last joinStoredNetwork() stopped because the household held the
/// BOOT button during the ladder rather than because the networks failed.
///
/// The button is watched throughout the join, not only at power-on: the wait
/// the household is sitting through is exactly when they want to say the WiFi
/// has changed. Reading this clears it.
bool setupRequestedDuringJoin();

}  // namespace Provisioning
