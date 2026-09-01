#pragma once

#include <Arduino.h>

/// Talking to the server: discovery, and the values discovery returns.
///
/// CAL compiles in a host name and a well-known path and nothing else. Every
/// other address it uses comes from the document fetched here, so an endpoint
/// can move without stranding hardware that cannot be reprogrammed.
namespace Service {

struct Discovery {
  bool ok = false;

  // Paths, not full URLs - the host is already known and a returned host would
  // be an opportunity to redirect a fleet somewhere unintended.
  String manifestPath;
  String binaryPath;
  String pairingPath;
  String brandAssetPath;

  /// Where an unprovisioned unit reports its hardware address to ask for a key.
  /// The only endpoint CAL calls without a device secret.
  String enrollmentPath;

  /// Where the QR on the splash should point. Often an agent's own address
  /// which redirects onward to the service, sometimes the service directly.
  /// The server decides; CAL only renders what it is given.
  String qrUrl;
  String qrCaption;

  /// Timing the server can retune without new firmware.
  uint32_t httpTimeoutMs = 0;
};

/// GETs the well-known document. Requires time to be synchronised first,
/// because it runs over TLS.
Discovery fetchDiscovery();

/// True once SNTP has produced a plausible time. TLS must not be attempted
/// before this: the device boots believing it is 1970 and would reject every
/// certificate it is offered as not yet valid.
bool synchroniseTime();

}  // namespace Service
