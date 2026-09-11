#pragma once

/// Joining WiFi the App already knows about.
///
/// Named WifiJoin rather than the more obvious "Network" because the ESP32
/// core's own WiFi.h transitively #include<>s a system header of that exact
/// name (network_event_handle_t, NetworkInterface, etc.) - a sketch-local
/// Network.h sitting on the same include path shadows it and takes down
/// WiFi.h's compile with missing-type errors that have nothing to do with
/// this file's own contents.
///
/// Unlike CAL's Provisioning module, there is no captive-portal fallback here:
/// teaching a device a NEW network is CAL's job (it can raise an access point;
/// the App cannot). If nothing remembered is reachable, the App's job is to get
/// back to CAL - see Loader::returnToLoader - not to provision one itself.
namespace WifiJoin {

/// Whether joinStoredNetwork() below narrates itself on the glass
/// ("Looking for known networks", "Connecting to WiFi ... (attempt 2)") or
/// only into the log. On by default, so a module that never calls this
/// behaves exactly as it always did.
///
/// **This exists because those two lines were the thing painting over the boot
/// splash.** `App.ino`'s setup() has skipped its own "Checking the time" and
/// "Loading" screens whenever a splash reached the glass since the splash was
/// moved ahead of the network stack, and the README says in as many words that
/// the logo then "stays up through the WiFi join". It never did: this file's
/// showStatus() calls are not App.ino's, were not covered by that check, and
/// run a few hundred milliseconds after the splash is drawn. The branded boot
/// that feature was written for has therefore been a single frame of logo
/// followed by network jargon on every device in the fleet, which is exactly
/// what a household reported seeing.
///
/// The same switch does the second job App.ino needs: silencing the join on a
/// restart the firmware asked for itself. See gNarrateBoot there - nobody
/// plugged this device in, so nobody is waiting to be told it found a network.
///
/// Deliberately a runtime switch rather than a parameter on joinStoredNetwork()
/// or a compile-time choice, because the same call site wants both answers at
/// different times: ensureWifiConnected() runs in setup() (where the splash or
/// a silent recovery owns the screen) and again from loop() on every iteration
/// (where a lost network SHOULD say so over whatever card is up, because by
/// then there is no boot for it to be part of).
///
/// Never suppresses a failure screen. Those are App.ino's, not this file's,
/// and "Could not join WiFi - hold BOOT for 3 seconds" is the one message a
/// household has to be able to act on.
void setProgressVisible(bool visible);

/// Attempts to join with stored credentials, strongest in-range candidate
/// first. Returns false if nothing remembered was reachable.
bool joinStoredNetwork();

}  // namespace WifiJoin
