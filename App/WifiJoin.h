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

/// Attempts to join with stored credentials, strongest in-range candidate
/// first. Returns false if nothing remembered was reachable.
bool joinStoredNetwork();

}  // namespace WifiJoin
