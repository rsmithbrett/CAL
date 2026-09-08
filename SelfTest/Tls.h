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
/// Deliberately validates against a root bundle rather than pinning the leaf.
/// Pinning is ordinarily good practice on an embedded device and is a liability
/// here: server certificates rotate on a ninety-day cycle, CAL cannot be
/// updated over the air, and a device trusting exactly one leaf certificate
/// would stop working the day that certificate is replaced - taking every unit
/// in the field with it simultaneously.
bool configure(NetworkClientSecure& client);

}  // namespace Tls
