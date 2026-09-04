#pragma once

#include <Arduino.h>

// Kept byte-identical to CAL's own Identity.h/.cpp deliberately: this is the same
// NVS namespace ("cal") read and written by both binaries, so a change to a key
// name or layout on one side and not the other silently corrupts the other's
// reads. Diff against ../Identity.h/.cpp before editing either copy.

/// The device's persistent identity and the state CAL needs across reboots.
///
/// All of it lives in NVS rather than in the application partition, because the
/// application partition is overwritten by every update and this must survive
/// that. The device secret in particular is written once at provisioning time -
/// the loader flashes an NVS image containing it - and is never regenerated on
/// the device.
namespace Identity {

/// Presented in the X-Device-Secret header on every request.
///
/// Empty on a freshly flashed unit, and that is the ordinary first-boot state
/// rather than a fault. Every device receives the identical image; identity is
/// established afterwards, by CAL reporting its hardware address and an
/// administrator assigning it a key. See Enrollment.
String deviceSecret();

bool hasSecret();

void saveSecret(const String& secret);

/// The hardware address, formatted as the server expects it. This is what
/// identifies an unprovisioned unit, since it has nothing else to offer.
String macAddress();

/// Remembered networks, most recently joined first.
///
/// More than one on purpose. A device is enrolled at an agent's office and then
/// carried to a household, and a unit that remembers only the network it is
/// currently on has to be re-provisioned by hand every time it moves. Three is
/// enough to cover office, home and one spare without turning NVS into a
/// database.
static constexpr uint8_t kMaxNetworks = 3;

struct Network {
  String ssid;
  String password;
};

/// Index 0 is the most recently joined.
uint8_t networkCount();
Network network(uint8_t index);

/// Records a successful join. An SSID already known is moved to the front and
/// its password refreshed rather than duplicated; the least recently used entry
/// is dropped once the list is full.
void rememberNetwork(const String& ssid, const String& password);

void clearNetworks();

bool hasAnyNetwork();

/// The application version currently installed in ota_0, as reported by the
/// manifest that installed it. Empty means nothing is installed yet.
String installedAppVersion();
void setInstalledAppVersion(const String& version);

/// Set by the application to ask CAL to perform an update on next boot. The
/// application cannot write its own partition, so this flag plus a reboot is
/// how it hands the job over.
bool updateRequested();
void setUpdateRequested(bool requested);

/// Incremented by CAL immediately before handing control to the application,
/// and cleared by the application once it reaches steady state. A value above
/// the threshold means the installed application is not surviving boot, and CAL
/// should treat it as bad rather than handing over again.
uint8_t bootAttempts();
void recordBootAttempt();
void clearBootAttempts();
static constexpr uint8_t kMaxBootAttempts = 3;

/// How many times this App has started, ever. Deliberately NOT bootAttempts():
/// that counter is cleared the moment the App reaches a network, which happens
/// before the first telemetry report is sent, so it always reads 0 by the time
/// anything reports it and is dead as a fleet health signal. This one is never
/// cleared, so "is this device rebooting when it shouldn't be" is answerable
/// from a single report instead of by watching uptime across several.
///
/// Saturates rather than wrapping: a counter that rolled over to 0 would read
/// as a freshly provisioned device. At one boot a minute that is millennia
/// away, so this is about being explicit rather than an expected case.
uint32_t totalBoots();
void recordBoot();

void begin();

}  // namespace Identity
