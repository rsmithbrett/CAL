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
#include <esp_heap_caps.h>
#include <esp_system.h>

#include "Config.h"
#include "Display.h"
#include "Enrollment.h"
#include "Identity.h"
#include "Journal.h"
#include "Provisioning.h"
#include "Service.h"
#include "SelfInstall.h"
#include "Updater.h"

namespace {

/// Whether this boot should talk to the server at all.
///
/// A working device must not depend on the network to start. If an application
/// is installed and healthy, CAL hands over immediately and lets the
/// application decide when to check for updates - otherwise a service outage
/// becomes a fleet outage.
bool mustContactServer() {
  // Both inputs are read into locals and logged, rather than being short-
  // circuited inside the return. The whole point of this decision is that it
  // is the difference between "CAL touched the network" and "CAL did not", and
  // a reader of the journal has to be able to tell which input drove it -
  // `haveBootableApplication()` logs its own three reasons in turn.
  const bool updateRequested = Identity::updateRequested();
  const bool bootable = Updater::haveBootableApplication();
  const bool must = updateRequested || !bootable;
  Journal::printf("[boot] contact the server? updreq=%d bootableApp=%d -> %s",
                  updateRequested ? 1 : 0, bootable ? 1 : 0,
                  must ? "YES"
                       : "no - a healthy app is installed and nothing asked for an "
                         "update, so the network is not touched at all");
  return must;
}

/// The BOOT button - the same one already used to enter flash mode over USB,
/// so there is nothing new for anyone to learn. Holding it through power-on
/// forces re-provisioning without discarding whatever networks are already
/// remembered (see Identity::setProvisioningForced). This is the only
/// user-accessible recovery path for "this device is on the wrong network"
/// or "we moved it to a new house" - there is no touch UI and no menu, and
/// CAL must not need one to recover from a bad WiFi credential.
///
/// A second, longer tier on the same gesture erases the device's identity
/// (its secret) instead of its WiFi. Found live: a device whose server-side
/// secret was reset (an admin's "Allow re-registration", or a straight
/// RegenerateSecret) while the physical unit was already holding a
/// different, now-orphaned secret has no way back without this - the App's
/// own self-heal (CheckIn::Result::secretRejected ->
/// Loader::returnToLoaderForReprovisioning(), which already calls
/// Identity::clearSecret()) only fires from *inside a running App that is
/// new enough to have it*, so a unit stuck on old firmware, or one that
/// never gets far enough to attempt a check-in at all, had no recovery path
/// short of a full serial erase-and-reflash. Continuing past the WiFi
/// tier's release point to a second, longer hold gets there with nothing
/// but the same button everyone already knows to hold.
constexpr uint8_t kBootButtonPin = 0;
constexpr uint32_t kWifiResetHoldMs = 3000;
constexpr uint32_t kIdentityEraseHoldMs = 10000;

enum class BootHoldResult { None, WifiReset, IdentityErase };

BootHoldResult bootHoldRequested() {
  pinMode(kBootButtonPin, INPUT_PULLUP);
  if (digitalRead(kBootButtonPin) != LOW) {
    Journal::line("[boot] BOOT not held at power-on - no WiFi reset, no identity erase");
    return BootHoldResult::None;
  }

  Journal::printf("[boot] BOOT held at power-on - %lu ms more selects WiFi setup",
                  static_cast<unsigned long>(kWifiResetHoldMs));
  Display::showStatus("Keep holding BOOT to set up WiFi", "Release now to cancel");
  uint32_t deadline = millis() + kWifiResetHoldMs;
  while (millis() < deadline) {
    if (digitalRead(kBootButtonPin) != LOW) {
      // Released before the first tier completed - a stray press, not a request.
      Journal::line("[boot] BOOT released before the WiFi tier completed - read as a "
                    "stray press, nothing changed");
      return BootHoldResult::None;
    }
    delay(50);
  }

  // First tier reached. Announce the second tier and give the same
  // released-early-means-stop-here treatment, just with a further deadline
  // (measured from here, not from entry - the two tiers' own hold times stay
  // exactly kWifiResetHoldMs and kIdentityEraseHoldMs apart from each other
  // regardless of how long the first tier's own polling loop took) and a
  // message that says what continuing to hold now does.
  Journal::printf("[boot] WiFi tier reached - %lu ms more erases this device's identity",
                  static_cast<unsigned long>(kIdentityEraseHoldMs - kWifiResetHoldMs));
  Display::showStatus("Keep holding BOOT to erase this device's identity",
                       "Release now for WiFi setup instead");
  deadline = millis() + (kIdentityEraseHoldMs - kWifiResetHoldMs);
  while (millis() < deadline) {
    if (digitalRead(kBootButtonPin) != LOW) {
      Journal::line("[boot] BOOT released during the identity tier - WiFi reset "
                    "requested, the secret is kept");
      return BootHoldResult::WifiReset;
    }
    delay(50);
  }
  Journal::line("[boot] BOOT held through both tiers - identity erase requested");
  return BootHoldResult::IdentityErase;
}

/// Shows a terminal condition and stops.
///
/// Deliberately not rebooting in a loop. A unit repeating a failed boot every
/// few seconds is harder to diagnose than one sitting on a screen that says
/// what is wrong, and the message always names what to do about it.
[[noreturn]] void haltWithFailure(const String& headline, const String& whatToDo) {
  // Recorded before the screen is drawn, so the journal carries the terminal
  // state even if drawing it is what fails. The panel message is written for a
  // household and was never evidence - this line is the evidence.
  Journal::printf("[halt] %s | %s", headline.c_str(), whatToDo.c_str());
  Journal::line("[halt] CAL stops here and will not restart itself. Press 'd' for the "
                "whole journal.");
  Display::showFailure(headline, whatToDo);
  while (true) {
    // The only reason this loop is not a bare delay: an operator who plugs a
    // cable in AFTER finding the device on this screen needs a way to read the
    // journal that does not involve power-cycling it.
    Journal::poll();
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

  // Logged on CHANGE, not on every pass. This loop can legitimately run for
  // hours at a two-minute interval, and a line per attempt would fill the
  // boot's 4KB sector with nothing but "still waiting" - pushing out the
  // context somebody opened the journal to find. Same rule the App's
  // Graphic.cpp/SunMoon.cpp already follow.
  Enrollment::State lastState = Enrollment::State::Unknown;
  bool everLogged = false;

  Journal::line("[enroll] no secret held - waiting for an administrator to assign one");

  while (true) {
    const Enrollment::Result result = Enrollment::requestKey(discovery);

    if (result.state != lastState || !everLogged) {
      Journal::printf("[enroll] state=%s message='%s' next poll in %lu ms",
                      result.state == Enrollment::State::Issued    ? "issued"
                      : result.state == Enrollment::State::Pending ? "pending"
                      : result.state == Enrollment::State::Refused ? "refused"
                                                                   : "unknown/failed",
                      result.message.c_str(), static_cast<unsigned long>(waitMs));
      lastState = result.state;
      everLogged = true;
    }

    if (result.state == Enrollment::State::Issued) {
      Journal::line("[enroll] secret issued and stored - continuing the ladder");
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

    // Broken into short slices rather than one long delay(), purely so the
    // serial 'd' command still answers during a wait that can last hours. The
    // total is unchanged.
    for (uint32_t waited = 0; waited < waitMs; waited += 100) {
      Journal::poll();
      delay(100);
    }
    waitMs = waitMs * 2 > kMaxWaitMs ? kMaxWaitMs : waitMs * 2;
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);

  // Ahead of the display on purpose. A hang inside lcd.init() or a LittleFS
  // format is one of the things the journal exists to make visible, and it
  // cannot record that if it starts afterwards. Budgeted at well under 400 ms
  // against the two-second rule immediately below: 512 bytes of header reads,
  // one sector erase, and at most one sector printed to serial.
  Journal::begin();
  Journal::dumpLastBoot();

  Journal::printf("[boot] CAL starting: resetReason=%d freeHeap=%u largest8BitBlock=%u",
                  static_cast<int>(esp_reset_reason()),
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  if (!Journal::persistent()) {
    // Said out loud rather than inferred. Somebody reading a live serial
    // session has to know whether what they are watching will still be there
    // after they power-cycle the device.
    Journal::line("[boot] journal is SERIAL ONLY this boot - nothing here will survive a "
                  "restart");
  }

  // Something must appear within about two seconds of power being applied. A
  // display that stays dark is indistinguishable from a broken device and will
  // be unplugged.
  Display::begin();
  if (!Display::showBrandSplash()) {
    Journal::line("[display] no usable cached brand splash - drawing the neutral one");
    Display::showNeutralSplash();
  } else {
    Journal::line("[display] cached brand splash drawn");
  }

  Identity::begin();
  Journal::printf("[identity] mac=%s secret=%s networks=%u installedApp='%s' updreq=%d "
                  "bootAttempts=%u/%u",
                  Identity::macAddress().c_str(), Identity::hasSecret() ? "held" : "NONE",
                  static_cast<unsigned>(Identity::networkCount()),
                  Identity::installedAppVersion().c_str(),
                  Identity::updateRequested() ? 1 : 0,
                  static_cast<unsigned>(Identity::bootAttempts()),
                  static_cast<unsigned>(Identity::kMaxBootAttempts));

  // TWO ROLES, DECIDED BY WHERE THIS CODE IS EXECUTING FROM.
  //
  // A CAL running from ota_0 is a CANDIDATE - it arrived there the way a new App
  // does, downloaded by the CAL already running, and its only job is to copy
  // itself into factory and get out of the way. It does not serve cards, does not
  // download anything and does not hand over. See CAL_OTA_DESIGN.md section 10.
  //
  // This fork is placed AFTER Identity::begin() because the candidate reads the
  // size and hash phase 1 recorded, and BEFORE anything touching the network,
  // because the copy needs nothing but power. That is the design's central
  // safety property: the survivor of an interrupted update can always finish
  // without a network, TLS, or a card.
  if (SelfInstall::runningAsCandidate()) {
    Journal::line("[boot] this CAL is running from ota_0, so it is a candidate: its job is "
                  "to copy itself into factory, not to start an application");
    if (SelfInstall::applyCandidate()) {
      return;  // not reached - applyCandidate restarts on success
    }
    // Refused or failed, with otadata untouched. factory still holds whichever
    // CAL was there before, so a power cycle returns this device to normality -
    // which is why this halts rather than falling through into a ladder that
    // would try to install an App into the partition it is running from.
    haltWithFailure("Cannot finish update",
                    "Restart the device. If this persists, contact support.");
  }

  // Running from factory. If the previous boot was a candidate that finished,
  // ota_0 still holds a copy of CAL - and the ladder below would mistake it for
  // an installable App and hand over to it, producing another candidate and a
  // boot loop. Cleared here, where nothing is executing from ota_0.
  SelfInstall::cleanUpAfterCandidate();

  // Only takes effect on a boot that goes on to actually join WiFi itself -
  // see mustContactServer() below. A device that already has a working
  // application installed hands off to it immediately without CAL touching
  // WiFi at all, so setting the flag here does nothing observable until the
  // next boot where CAL is the one doing the joining (no app installed yet,
  // or an update was requested).
  //
  // setProvisioningForced, not clearNetworks: this used to clear the
  // remembered-networks list outright, which was the only thing that forced
  // the join-or-provision check below to open the portal at all
  // (joinStoredNetwork() fails immediately with nothing remembered - see
  // WifiJoin.cpp). That meant every use of this gesture discarded whatever
  // was already remembered before the portal added the one new network it
  // captures, so a unit provisioned at more than one site only ever answered
  // to the last one. See the README's "the 3-remembered-networks design had
  // no path to ever reach 2" for the fuller incident writeup.
  switch (bootHoldRequested()) {
    case BootHoldResult::IdentityErase:
      // Wipes the secret only - not the remembered networks, for the same
      // reason the WiFi tier below does not touch identity: a household
      // stuck on a bad secret almost certainly has a perfectly good WiFi
      // connection, and forcing them to redo that too would just be a second
      // unrelated recovery step bolted onto the one they actually needed.
      // Falls straight through into the ordinary boot below, which is what
      // sends this unit down awaitKeyAssignment() naturally once
      // Identity::hasSecret() reads false - no separate flag needed the way
      // setProvisioningForced() is for the WiFi tier.
      Identity::clearSecret();
      Journal::line("[boot] identity erased on request - the remembered networks were "
                    "deliberately left alone");
      Display::showStatus("Identity erased", "Re-registering...");
      delay(1000);
      break;
    case BootHoldResult::WifiReset:
      Identity::setProvisioningForced(true);
      Journal::line("[boot] provisioning forced for this boot - the secret and the "
                    "remembered networks were deliberately left alone");
      Display::showStatus("Set up WiFi", "Opening setup...");
      delay(1000);
      break;
    case BootHoldResult::None:
      break;
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

  // Consumed immediately, one-shot per this decision - see its own doc
  // comment in Identity.h. Short-circuits joinStoredNetwork() entirely when
  // set, so a forced request opens the portal right away rather than trying
  // (and possibly succeeding at rejoining) whatever is already remembered
  // first - that would make the BOOT-hold gesture look like a dead button
  // whenever the device is still in range of a network it already knows.
  const bool forced = Identity::provisioningForced();
  Identity::setProvisioningForced(false);

  if (forced) {
    Journal::line("[wifi] provisioning was forced - skipping the stored-network join "
                  "entirely and opening the portal");
  }
  if (forced || !Provisioning::joinStoredNetwork()) {
    // Repeated failure means the stored credentials are wrong or the network
    // is gone - retrying them indefinitely would look identical to an outage.
    Provisioning::run();
    if (!Provisioning::joinStoredNetwork()) {
      haltWithFailure("Could not join WiFi",
                      "Restart the device to set up the network again.");
    }
  }

  Journal::printf("[wifi] joined SSID=%s IP=%s RSSI=%d dBm channel=%d",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                  WiFi.channel());

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
    Journal::line("[boot] discovery failed - falling back to the installed app if there "
                  "is one, since an unreachable server is not a reason to refuse to start");
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
  } else {
    Journal::line("[enroll] secret already held - enrollment skipped");
  }

  // Cosmetic and never fatal - a failure here leaves the neutral splash.
  Updater::cacheBrandAssets(discovery);

  const Updater::Manifest manifest = Updater::fetchManifest(discovery);

  // A DEVICE WITH NOTHING TO RUN NEEDS AN INSTALL EVEN IF THE VERSION MATCHES.
  //
  // This used to be version inequality alone, and that made CAL refuse to
  // rescue the one device it exists to rescue. Observed on device 17 on
  // 2026-09-16, with ota_0 erased and nvs still recording the current version:
  //
  //   [updater] no bootable app: 'v2026.09.16.0003' has used 3 of 3 boot
  //             attempts without reporting itself healthy, treated as bad
  //   [boot] contact the server? updreq=0 bootableApp=0 -> YES
  //   [update] install? offered='v2026.09.16.0003' installed='v2026.09.16.0003' -> no
  //   [boot] nothing to hand over to ... showing the not-yet-activated screen
  //
  // CAL printed "there is no bootable app" and "no update needed" two lines
  // apart and acted on the second. The device was then stuck permanently: only
  // a USB cable, or a server changing the version it offers, could move it.
  // Recovering it took both.
  //
  // installedAppVersion() is what nvs RECORDS, not what the flash CONTAINS, and
  // the two part company exactly when recovery is needed - a half-written OTA, a
  // corrupted partition, a failed handover. Comparing records to records cannot
  // see that. haveBootableApplication() asks the flash.
  //
  // This matters most for the over-the-air CAL work in CAL_OTA_DESIGN.md, whose
  // entire safety argument is that the survivor of a failed update can retry.
  // The survivor could not retry, because it did not believe anything was wrong.
  const bool haveApp = Updater::haveBootableApplication();
  const bool needsInstall =
      manifest.ok && manifest.isConfigured &&
      (manifest.version != Identity::installedAppVersion() || !haveApp);

  // Four inputs, so the journal distinguishes "the manifest could not be fetched"
  // from "the server has no current build marked" from "the build the server
  // names is already installed" from "the versions match but there is nothing on
  // the flash to run". Those look identical from outside and want four different
  // investigations.
  //
  // KEPT SHORT ON PURPOSE. Log::printf formats into a fixed 256-byte scratch and
  // truncates past it. A first version of this line spelled out "(same version,
  // but nothing bootable is installed)" and was cut mid-sentence on device 17 -
  // the verdict survived and the explanation did not, which was luck rather than
  // design. "noApp" carries the same fact in five characters.
  Journal::printf("[update] install? manifestOk=%d isConfigured=%d bootableApp=%d "
                  "offered='%s' installed='%s' -> %s%s",
                  manifest.ok ? 1 : 0, manifest.isConfigured ? 1 : 0, haveApp ? 1 : 0,
                  manifest.version.c_str(), Identity::installedAppVersion().c_str(),
                  needsInstall ? "YES" : "no",
                  (needsInstall && manifest.version == Identity::installedAppVersion())
                      ? " (noApp)" : "");

  if (needsInstall) {
    if (!Updater::installApplication(discovery, manifest)) {
      Journal::line("[update] install FAILED - looking for anything still bootable to "
                    "fall back to");
      if (Updater::haveBootableApplication()) {
        Updater::bootApplication();
      }
      // Reaching here means haveBootableApplication() said no, which after a
      // failed install means the app partition was invalidated and there is
      // nothing to go back to. The install log above is the only record of how
      // far it got before that happened.
      Journal::line("[update] nothing bootable remains after the failed install - this "
                    "device is stopped until a human restarts it");
      haltWithFailure("Update failed",
                      "Restart the device to try again.");
    }
  } else if (manifest.ok) {
    // installApplication() clears updateRequested on the path that actually
    // installs something. This is the other successful path - the server was
    // reached and confirmed the running version is already current - and it
    // has to clear the flag too. Without this, an application that asked for
    // an update and got "nothing to do" would leave updateRequested set
    // forever, which makes mustContactServer() true on every future boot
    // even after a plain power cut - exactly the fleet-wide network
    // dependency this flag exists to avoid outside of a real update.
    Identity::setUpdateRequested(false);
    Journal::line("[update] server confirmed the installed version is current - the "
                  "update-requested flag has been cleared");
  }

  if (Updater::haveBootableApplication()) {
    Updater::bootApplication();
  }

  // No application installed and none available. The QR is the useful thing to
  // show: the server decides where it points - often the agent's own address,
  // which redirects onward to the service.
  if (discovery.qrUrl.length() > 0) {
    Journal::printf("[boot] nothing to hand over to - showing the server's QR (%s)",
                    discovery.qrUrl.c_str());
    Display::showQr(discovery.qrUrl,
                    discovery.qrCaption.length() > 0 ? discovery.qrCaption
                                                     : "Scan to get started",
                    "");
  } else {
    Journal::line("[boot] nothing to hand over to and discovery supplied no QR URL - "
                  "showing the not-yet-activated screen");
    Display::showStatus("Waiting for setup",
                        "This device is not yet activated.");
  }
}

void loop() {
  // CAL is a boot-time component. Once setup() has handed over, this is only
  // reached in the waiting states above, where there is nothing to poll for
  // until the household or the account holder acts.
  //
  // The journal poll is the exception: it is what lets somebody who plugged a
  // cable in after the fact press 'd' and read the whole journal, on a device
  // that is otherwise sitting on a QR code doing nothing.
  Journal::poll();
  delay(1000);
}
