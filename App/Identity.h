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

/// Forgets the stored secret, so the next boot's `!hasSecret()` check is true
/// again and CAL re-enrolls via Enrollment/RegisterViaMacAddress instead of
/// presenting a value the server has already rejected forever. See
/// App/Loader.cpp's `returnToLoaderForReprovisioning()` - the only caller -
/// for why a rejected secret needs this and a rejected WiFi network does not.
void clearSecret();

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

/// One-shot, same shape and lifecycle as updateRequested below: set before a
/// reboot into CAL, read (and consumed) once by the code deciding whether to
/// open Provisioning::run() unconditionally, false again afterward.
///
/// Exists so the BOOT-hold "reset WiFi" gesture can force the captive portal
/// open without calling clearNetworks() to do it - forcing via network count
/// meant every reprovisioning wiped everything already remembered before
/// adding the one new network the portal captures, which is why a unit set
/// up at more than one site only ever answered to the last one. See the
/// README's "the 3-remembered-networks design had no path to ever reach 2"
/// for the incident this fixes.
bool provisioningForced();
void setProvisioningForced(bool forced);

/// The application version currently installed in ota_0, as reported by the
/// manifest that installed it. Empty means nothing is installed yet.
String installedAppVersion();

/// Which CAL is in factory, as CAL recorded it in the shared "cal" NVS namespace.
/// Read only - the App never installs a CAL and never writes this. Empty means the
/// CAL predates recording itself, which is every device until the USB pass, and that
/// emptiness is reported rather than hidden.
String installedCalVersion();
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

/// The most recent `CheckIn::Result::utcOffsetMinutes` a successful check-in
/// ever handed back, persisted so a device that hasn't completed one yet this
/// boot - just powered on, WiFi still joining, first check-in still seconds
/// away - has a real (if possibly a day stale) offset for the corner clock
/// and day/night logic instead of drawing raw UTC until it does. Updated on
/// every successful check-in, not just the first, the same "always current"
/// treatment App.ino's own lastUtcOffsetMinutes already gives it in RAM - this
/// is that same value's copy that survives a reboot.
///
/// 0 (UTC) on a device that has never completed a check-in, matching
/// CheckIn::Result's own default - a freshly flashed unit gets exactly the
/// behaviour it already had before this existed, not a new failure mode.
///
/// App-only key: CAL never reads or writes it, so - unlike every other member
/// of this file - it does not need mirroring in CAL's own copy of
/// Identity.h/.cpp despite the "kept byte-identical" note at the top. It's a
/// new key, not a change to one CAL also uses.
int lastUtcOffsetMinutes();
void setLastUtcOffsetMinutes(int minutes);

/// How many times in a row this App has restarted ITSELF to recover - the
/// heap watchdog and the unreachability watchdog, and nothing else. Not
/// bootAttempts() (CAL's "is the installed app bootable at all" counter,
/// cleared the moment WiFi comes up) and not totalBoots() (every start ever,
/// including power cuts, OTAs and reprovisions, never cleared).
///
/// **Exists so the App can stay quiet about a self-heal without being able to
/// hide a device that is not healing.** App.ino suppresses the boot-progress
/// screens on a restart the firmware asked for - nobody plugged anything in,
/// so narrating the WiFi/time ladder turns an invisible recovery into a
/// visible fault, repeatedly. That is right for the first few and wrong
/// forever: a device restarting every 13-27 minutes (measured across the
/// fleet on 2026-09-11, one unit 24 times in five hours) would then never
/// tell anybody anything, and the household's only evidence would be a
/// picture that blinks. This counter is what puts a floor under that - see
/// kMaxSilentSelfRestarts in App.ino for the budget and what happens when it
/// is spent.
///
/// Has to live in NVS rather than RTC memory for exactly the reason BootDiag.h
/// documents at length for the restart cause: every restart on this device
/// goes App -> CAL -> App, two software resets with a different binary in
/// between, and RTC_NOINIT does not survive that hop. Measured there, not
/// assumed here.
///
/// Saturates at 255 rather than wrapping, same reasoning as totalBoots(): a
/// counter that rolled to 0 would read as a perfectly healthy device, which is
/// the one thing it must never be able to say about a unit that has restarted
/// itself 256 times.
///
/// App-only key, like lastUtcOffsetMinutes() above - CAL never reads or writes
/// it, so it needs no mirroring into CAL's own copy of this file.
uint8_t consecutiveSelfRestarts();

/// Counted immediately before a deliberate, self-inflicted restart - the two
/// watchdogs in App.ino and nowhere else. Deliberately NOT called for
/// RestartCause::Ota, ::Reprovision or ::SelfTest: those are restarts somebody
/// asked for (an admin, a household holding BOOT, a self-test build request),
/// not evidence of a device trying and failing to fix itself, and counting
/// them would spend the silence budget on boots that were never meant to be
/// silent in the first place.
void recordSelfRestart();

/// Called once the device has demonstrated the restart actually worked - a
/// long stretch of uptime with no further self-restart. See
/// kSelfRestartRecoveredUptimeMs in App.ino for why "worked" is measured as
/// uptime rather than as a successful check-in or a successful draw: the two
/// watchdogs fire on different symptoms, and a device can pass either one's
/// idea of healthy while still cycling on the other's.
void clearSelfRestarts();

/// How many times in a row this device has restarted itself to get back to the
/// server WITHOUT a successful check-in in between. The exponent behind the
/// self-restart backoff - see selfRestartFloorMs() in App.ino for the shape and
/// the cap.
///
/// **Why this is not consecutiveSelfRestarts() above, which is the obvious
/// candidate and the wrong one.** That counter answers "should this boot narrate
/// itself", and its clearing rule is deliberately uptime-based: an hour with no
/// further restart, whether or not the device ever reached the server. Both
/// halves of that are wrong for a backoff. An hour of uptime is not evidence
/// that a restart FIXED anything on a device whose whole complaint is that it
/// cannot be reached - and worse, once the backoff grows past that hour the
/// budget would clear itself moments before every restart it was meant to
/// delay, capping the backoff at the narration rule's horizon by accident. The
/// two counters also want opposite reset events: narration wants "no more
/// restarts", backoff wants "the device got through to the server", which is
/// the only thing that actually proves the run is over.
///
/// Stepped only by the two watchdogs that share the restart floor - the
/// connection watchdog and the response-OOM watchdog. NOT by the draw-failure
/// heap watchdog, which has no floor of its own and is not part of that
/// hazard's accounting.
///
/// Saturates at 255, same reasoning as the two counters above; the floor is
/// clamped long before then, so any value past the clamp is equivalent.
///
/// App-only key, like lastUtcOffsetMinutes() - CAL never reads or writes it, so
/// it needs no mirroring into CAL's own copy of this file.
uint8_t selfRestartBackoffSteps();

/// Counted immediately before a connection or response-OOM self-restart, beside
/// Identity::recordSelfRestart(), so the NEXT boot knows how many attempts this
/// run has already spent. It has to be persistent for the same reason
/// BootDiag's restart cause is: the restart is the event being counted, and
/// nothing in RAM survives it.
void recordSelfRestartBackoffStep();

/// Called on the first successful check-in after a run of self-restarts, and
/// nowhere else. A completed check-in - TLS up, secret accepted, whole response
/// parsed - is the only observation that proves the condition both watchdogs
/// fire on has actually ended.
void clearSelfRestartBackoff();

void begin();

}  // namespace Identity
