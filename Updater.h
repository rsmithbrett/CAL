#pragma once

#include <Arduino.h>

#include "Service.h"

/// Fetching and installing the application image.
///
/// This is the reason CAL exists. The application occupies the single OTA slot
/// and therefore cannot rewrite itself - it can only ask, by way of an NVS flag
/// and a reboot, for CAL to do it.
namespace Updater {

struct Manifest {
  bool ok = false;
  bool isConfigured = false;  // false when no build has been marked current
  String version;
  String sha256;              // lower-case hex, verified before the image is committed
  uint32_t sizeBytes = 0;
};

Manifest fetchManifest(const Service::Discovery& discovery);

/// Downloads into the OTA partition and verifies the manifest's hash before
/// committing. Returns false without disturbing the installed image on any
/// failure - a half-written partition is never marked bootable.
bool installApplication(const Service::Discovery& discovery, const Manifest& manifest);

/// Caches the brand splash into LittleFS. Purely cosmetic and never fatal:
/// failure leaves the neutral splash in place.
bool cacheBrandAssets(const Service::Discovery& discovery);

/// Hands control to the installed application. Records a boot attempt first,
/// so an application that never reaches steady state is eventually recognised
/// as bad rather than retried forever. Does not return on success.
void bootApplication();

/// True when an application image is present and has not exhausted its boot
/// attempts.
bool haveBootableApplication();

}  // namespace Updater
