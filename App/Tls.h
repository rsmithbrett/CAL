#pragma once

#include <NetworkClientSecure.h>

/// Transport security, configured in exactly one place.
///
/// This exists as its own unit because getting it wrong is silent. An
/// unvalidated connection looks identical to a validated one until somebody on
/// the household network is reading the device secret out of the headers, and
/// the device presents that secret on every request.
namespace Tls {

/// Configures a client to validate the server certificate against trusted
/// roots. Returns false if no trust source is available, and callers must treat
/// that as a hard failure rather than continuing unvalidated.
///
/// Deliberately validates against trusted ROOTS rather than pinning the leaf,
/// and that distinction carries the whole argument. Leaf pinning is ordinarily
/// good practice on an embedded device and is a liability here: server
/// certificates rotate on a ninety-day cycle, CAL cannot be updated over the
/// air, and a device trusting exactly one leaf certificate would stop working
/// the day that certificate is replaced - taking every unit in the field with
/// it simultaneously.
///
/// The trust SOURCE has since changed and that reasoning has not. It was the
/// ESP32 core's ~150-certificate Mozilla bundle; it is now the two ISRG roots,
/// embedded directly (TlsRoots.h). Roots are not the thing that rotates every
/// ninety days - ISRG Root X2 runs to 2040 - so narrowing the trust store
/// leaves the anti-leaf-pinning argument above completely intact. What it does
/// cost is issuer flexibility: the service can no longer move to a non-ISRG CA
/// without a firmware release. TlsRoots.h states that trade and the field
/// measurement that forced it, so read it before widening or narrowing this
/// again.
///
/// Called exactly once per boot now, from Http::begin() - see Http.h. Every
/// HTTP call site used to hold its own `NetworkClientSecure client;` as a
/// stack local and call this on it fresh before every single request; those
/// all now share one persistent client instead, so this only ever needs to
/// run once for the client's whole lifetime. Safe to move this early because
/// what this function actually does - hand over a baked-in trust anchor - never
/// touches the live connection and has nothing to redo once a connection has
/// opened, closed, or reopened underneath it.
bool configure(NetworkClientSecure& client);

}  // namespace Tls
