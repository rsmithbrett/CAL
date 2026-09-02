// CAL - Client Application Loader
//
// The first application flashed to a device, and the only one that is not
// delivered over the air. It lives in the factory partition, is never
// overwritten by an update, and exists to get the unit onto a network,
// authenticate it, and install the real application into the OTA slot.
//
// Because it cannot be replaced without physically recovering the hardware, it
// does as little as possible: no card rendering, no product logic, no policy.
// It gets the device to the point where something that CAN be updated takes
// over, and it stays behind as the recovery image if that ever fails.

#include <WiFi.h>

#include "Config.h"
#include "Display.h"
#include "Enrollment.h"
#include "Identity.h"
#include "Provisioning.h"
#include "Service.h"
#include "Updater.h"

namespace {

/// Whether this boot should talk to the server at all.
///
/// A working device must not depend on the network to start. If an application
/// is installed and healthy, CAL hands over immediately and lets the
/// application decide when to check for updates - otherwise a service outage
/// becomes a fleet outage.
bool mustContactServer() {
  return Identity::updateRequested() || !Updater::haveBootableApplication();
}

/// The BOOT button - the same one already used to enter flash mode over USB,
/// so there is nothing new for anyone to learn. Holding it through power-on
/// clears remembered WiFi networks and forces re-provisioning. This is the
/// only user-accessible recovery path for "this device is on the wrong
/// network" or "we moved it to a new house" - there is no touch UI and no
/// menu, and CAL must not need one to recover from a bad WiFi credential.
constexpr uint8_t kBootButtonPin = 0;
constexpr uint32_t kWifiResetHoldMs = 3000;

bool wifiResetRequested() {
  pinMode(kBootButtonPin, INPUT_PULLUP);
  if (digitalRead(kBootButtonPin) != LOW) {
    return false;
  }

  Display::showStatus("Keep holding BOOT to reset WiFi", "Release now to cancel");
  const uint32_t deadline = millis() + kWifiResetHoldMs;
  while (millis() < deadline) {
    if (digitalRead(kBootButtonPin) != LOW) {
      // Released before the hold completed - a stray press, not a request.
      return false;
    }
    delay(50);
  }
  return true;
}

/// Shows a terminal condition and stops.
///
/// Deliberately not rebooting in a loop. A unit repeating a failed boot every
/// few seconds is harder to diagnose than one sitting on a screen that says
/// what is wrong, and the message always names what to do about it.
[[noreturn]] void haltWithFailure(const String& headline, const String& whatToDo) {
  Display::showFailure(headline, whatToDo);
  while (true) {
    delay(1000);
  }
}

/// Waits for an administrator to assign this unit a key.
///
/// The hardware address is shown on screen throughout, because that is what
/// the person doing the assigning has to match against. There is nothing the
/// household can do here and the screen says so plainly rather than presenting
/// a spinner that reads as a fault.
void awaitKeyAssignment(const Service::Discovery& discovery) {
  // Backs off from ten seconds to two minutes. A room full of units flashed
  // together would otherwise poll in lockstep forever, and this wait can
  // legitimately last hours - nobody is watching it.
  uint32_t waitMs = 10000;
  constexpr uint32_t kMaxWaitMs = 120000;

  while (true) {
    const Enrollment::Result result = Enrollment::requestKey(discovery);

    if (result.state == Enrollment::State::Issued) {
      return;
    }

    if (result.state == Enrollment::State::Refused) {
      // This hardware already holds a secret, so it is either a reflashed unit
      // or something claiming an address that is not its own. Either way an
      // administrator has to intervene; the device must not keep asking.
      haltWithFailure(result.message.length() > 0
                          ? result.message
                          : String("This device is already registered"),
                      "An administrator must re-issue its key.");
    }

    Display::showQr(Identity::macAddress(),
                    result.message.length() > 0 ? result.message
                                                : String("Waiting to be set up"),
                    Identity::macAddress());

    delay(waitMs);
    waitMs = waitMs * 2 > kMaxWaitMs ? kMaxWaitMs : waitMs * 2;
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);

  // Something must appear within about two seconds of power being applied. A
  // display that stays dark is indistinguishable from a broken device and will
  // be unplugged.
  Display::begin();
  if (!Display::showBrandSplash()) {
    Display::showNeutralSplash();
  }

  Identity::begin();

  // Only takes effect on a boot that goes on to actually join WiFi itself -
  // see mustContactServer() below. A device that already has a working
  // application installed hands off to it immediately without CAL touching
  // WiFi at all, so clearing the list here does nothing observable until
  // the next boot where CAL is the one doing the joining (no app installed
  // yet, or an update was requested). That covers today's real case - first
  // setup with the wrong network chosen - not "move an already-running
  // device to a new house," which is the application's own concern.
  if (wifiResetRequested()) {
    Identity::clearNetworks();
    Display::showStatus("WiFi reset", "Setting up again...");
    delay(1000);
  }

  // A unit holding no secret is newly flashed, not faulty. Every device is
  // written with the identical image; which device it is gets established
  // below, once it is on a network and can report its hardware address.
  if (!mustContactServer()) {
    Display::showStatus("Starting", Identity::installedAppVersion());
    Updater::bootApplication();
    // Only reached if handing over failed outright.
    haltWithFailure("Cannot start application",
                    "Restart the device. If this persists, contact support.");
  }

  if (!Provisioning::joinStoredNetwork()) {
    // Repeated failure means the stored credentials are wrong or the network
    // is gone - retrying them indefinitely would look identical to an outage.
    Provisioning::run();
    if (!Provisioning::joinStoredNetwork()) {
      haltWithFailure("Could not join WiFi",
                      "Restart the device to set up the network again.");
    }
  }

  // TEMPORARY diagnostic instrumentation for the first real-hardware test -
  // remove once discovery has succeeded at least once on real hardware.
  Serial.printf("[wifi] joined SSID=%s IP=%s RSSI=%d dBm channel=%d\n",
                WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
                WiFi.RSSI(), WiFi.channel());

  Display::showStatus("Checking the time", "Needed before a secure connection");
  if (!Service::synchroniseTime()) {
    // Attributed to the network rather than presented as a security error,
    // because that is what the household can act on.
    haltWithFailure("Cannot reach the internet",
                    "Check the network, then restart the device.");
  }

  Display::showStatus("Contacting service", Config::kServiceHost);
  const Service::Discovery discovery = Service::fetchDiscovery();
  if (!discovery.ok) {
    if (Updater::haveBootableApplication()) {
      // An unreachable server is not a reason to refuse to start when a
      // working application is already installed.
      Updater::bootApplication();
    }
    haltWithFailure("Cannot reach the service",
                    "Check the network, then restart the device.");
  }

  // Establish identity before anything that needs it. A unit with no secret
  // cannot fetch a manifest, so this gates the rest of the ladder.
  if (!Identity::hasSecret()) {
    awaitKeyAssignment(discovery);
  }

  // Cosmetic and never fatal - a failure here leaves the neutral splash.
  Updater::cacheBrandAssets(discovery);

  const Updater::Manifest manifest = Updater::fetchManifest(discovery);
  const bool needsInstall =
      manifest.ok && manifest.isConfigured &&
      manifest.version != Identity::installedAppVersion();

  if (needsInstall) {
    if (!Updater::installApplication(discovery, manifest)) {
      if (Updater::haveBootableApplication()) {
        Updater::bootApplication();
      }
      haltWithFailure("Update failed",
                      "Restart the device to try again.");
    }
  }

  if (Updater::haveBootableApplication()) {
    Updater::bootApplication();
  }

  // No application installed and none available. The QR is the useful thing to
  // show: the server decides where it points - often the agent's own address,
  // which redirects onward to the service.
  if (discovery.qrUrl.length() > 0) {
    Display::showQr(discovery.qrUrl,
                    discovery.qrCaption.length() > 0 ? discovery.qrCaption
                                                     : "Scan to get started",
                    "");
  } else {
    Display::showStatus("Waiting for setup",
                        "This device is not yet activated.");
  }
}

void loop() {
  // CAL is a boot-time component. Once setup() has handed over, this is only
  // reached in the waiting states above, where there is nothing to poll for
  // until the household or the account holder acts.
  delay(1000);
}
