# CAL — Client Application Loader

The first application flashed onto a Discover Around Me device, and the only
one that is never delivered over the air. CAL lives in the `factory` partition,
gets the unit onto a network, authenticates it with its device secret, installs
the real product application into the OTA partition, and hands over. It is a
loader and a recovery image, not a product.

The server this talks to lives in a separate repository
(`DiscoverAroundMe`, https://github.com/rsmithbrett/DiscoverAroundMe); its
`Firmware` domain is the catalog half — upload, checksum, mark-current, and the
device-authenticated manifest/binary endpoints CAL consumes.

## Why this is a separate project

The product application and the loader have opposite constraints, and putting
them in one binary means one of the two gets the wrong treatment.

The application changes constantly and must be replaceable remotely — a bug in
it costs a download. CAL cannot be replaced without physically recovering the
hardware and putting a USB cable into it, so a bug in CAL costs a truck roll or
a returned unit. That difference is what justifies the split: CAL is written to
do as little as possible and change as rarely as possible. It renders no cards,
holds no product logic, resolves no policy, and knows nothing about weather,
listings, calendars or content. It gets the device to the point where something
that *can* be updated takes over, and it stays behind as the fallback if that
ever fails.

Keeping it in its own repository rather than its own folder follows the same
reasoning. The two are versioned on completely different clocks and share no
build; a shared history would imply a coupling that does not exist.

## Hardware

An LCDWIKI **E32R28T**, from the "Cheap Yellow Display" family — ESP32-WROOM-32E,
**4 MB flash**, no PSRAM, an ILI9341 240×320 panel driven in landscape
(320×240), and an XPT2046 resistive touch controller. CAL does not use touch at
all; every one of its screens is either informational or a QR code, and the one
place a person has to type something (a WiFi password) deliberately happens on
their phone rather than on a resistive panel.

The display is brought up through LovyanGFX's **`LGFX_AUTODETECT`** rather than
a hand-written pin map. This is not laziness. Most published CYD pin maps
describe the Sunton boards, and the E32R28T's HSPI-style pinout differs from
them — a copied pin map produces a blank or scrambled panel, while autodetect
senses the board correctly. If autodetect is ever replaced with an explicit
configuration, it must be measured against this specific board and not against
the family name.

## Partition layout

`partitions_cal.csv`. Asymmetric on purpose, and this is the single decision the
whole project rests on.

The server-side README records the measurement that forced it: the product
application, as flashed, is **2,166,784 bytes**, while the largest app slot in a
conventional dual-slot OTA layout on 4 MB flash (`min_spiffs`) is **1,966,080
bytes** — about 196 KB short. That gap was treated as a hard ceiling, and it was,
*given the assumption that OTA requires two equal app partitions*. It does not.
It requires somewhere to write an image that is not the partition currently
executing. Splitting the flash unevenly satisfies that and removes the ceiling:

| Name | Type | SubType | Offset | Size | Bytes | Holds |
|---|---|---|---|---:|---:|---|
| `nvs` | data | nvs | `0x9000` | `0x5000` | 20,480 | Device secret, WiFi, installed version, flags |
| `otadata` | data | ota | `0xE000` | `0x2000` | 8,192 | Which app partition boots |
| `factory` | app | factory | `0x10000` | `0x130000` | 1,245,184 | **CAL** — never written by OTA |
| `ota_0` | app | ota_0 | `0x140000` | `0x280000` | 2,621,440 | The product application |
| `spiffs` | data | spiffs | `0x3C0000` | `0x30000` | 196,608 | Cached branding assets |
| `coredump` | data | coredump | `0x3F0000` | `0x10000` | 65,536 | |

That fills 4 MB exactly, with nothing left over. The application slot is now
**2,621,440 bytes against a 2,166,784-byte build** — roughly 455 KB of headroom
where there was previously a 196 KB deficit. The consequence worth stating
plainly: the previously proposed remedy of dropping the ESP32-A2DP Bluetooth
audio stack to reclaim 300–500 KB is **no longer required by the size ceiling**.
It may still be worth doing on its own merits, but it is no longer the price of
having OTA at all.

The `spiffs` label is what the ESP32 Arduino LittleFS library looks for by
default; the partition is formatted and used as LittleFS despite the name. That
is conventional, not an oversight, but it is the kind of detail that reads as a
bug six months later.

**The open risk is CAL's own size.** The layout works only if CAL compiles to
under 1,245,184 bytes, and CAL has not yet been measured — see Open questions.
Nothing here is proven on hardware.

## The boot ladder

`setup()` in `CAL.ino` is the whole of CAL's control flow, and
it is written to be read top to bottom.

1. **Display first, within about two seconds of power.** A dark screen is
   indistinguishable from a dead device and will be unplugged mid-setup. The
   cached brand splash is shown if one exists, otherwise a neutral one.
2. **Load identity from NVS.** No device secret means the unit was never
   provisioned. That is a manufacturing fault, not something a household can
   resolve, so CAL says exactly that and stops rather than starting a setup
   flow that cannot succeed.
3. **Decide whether to contact the server at all.** If a healthy application is
   installed and no update was requested, CAL hands over immediately — see
   below.
4. **Join WiFi** with stored credentials, retrying, and fall back to the
   captive-portal provisioning flow if that fails.
5. **Synchronise the clock over SNTP.** Mandatory, and mandatory *in this
   position*. The device has no RTC and boots believing it is 1970, so every
   certificate it is offered looks not-yet-valid and TLS fails. A result earlier
   than `Config::kEarliestPlausibleTime` is treated as a failure rather than an
   answer, because SNTP itself is unauthenticated. The failure is reported to the
   household as "cannot reach the internet", which is what they can act on — not
   as a certificate error.
6. **Fetch the discovery document**, cache brand assets (cosmetic, never fatal),
   compare the manifest against the installed version, install if they differ.
7. **Hand over**, or — if there is no application to hand over to — show the
   server-supplied QR and wait.

### CAL must not need the network to boot

Step 3 is the rule that matters most in the field. If an application is installed
and healthy, CAL hands control over **without contacting the server**, and lets
the application decide when to check for updates. The alternative — checking on
every boot before handing over — makes a service outage, an expired
certificate, or a DNS mistake into a fleet outage in which every device in every
household stops working at once. `mustContactServer()` returns true only when the
application asked for an update or when there is no bootable application to run.

Failures at every rung are terminal-with-a-message rather than
reboot-and-retry. A unit repeating a failed boot every few seconds is much
harder to diagnose than one sitting on a screen that names the problem, and the
screen always says what to do, never only what went wrong.

## Updates: reboot to the updater

With a single OTA slot, the application **cannot update itself** — it cannot
write the partition it is executing from. This is the direct cost of the
asymmetric layout, and it is paid deliberately.

The flow is:

1. The application discovers a new build is available and sets the
   `updreq` flag in NVS.
2. It reboots. The bootloader runs `factory`, so CAL comes up.
3. `mustContactServer()` sees the flag, so CAL joins WiFi, syncs time, fetches
   discovery and the manifest, and downloads the binary into `ota_0`.
4. The image is streamed in 1 KB chunks with a running SHA-256, and the hash is
   compared against the manifest **before** `Update.end()` commits it. A
   truncated or tampered download is discarded rather than marked bootable, so a
   half-written partition can never become the running application.
5. On success CAL records the installed version, clears the boot-attempt
   counter and the update flag, then sets the boot partition and restarts into
   the new application.

**The trade-off, stated honestly:** there is no instant A/B rollback to the
*previous version*, because there is no second slot holding it. What there is
instead is a permanent recovery image. A bad build cannot brick a unit — the
worst case is a device that falls back to CAL and re-downloads. For hardware
sitting in strangers' kitchens with no console and no service technician, an
image that always boots is worth more than the ability to revert one version.

### The anti-brick counter

Handing control to an application that crashes on startup would otherwise be
unrecoverable: the device would boot the bad image, crash, boot it again, and
never reach the code that could replace it.

`Updater::bootApplication()` therefore **increments an NVS boot-attempt counter
immediately before handing over**, and the application is expected to call the
clearing path once it reaches steady state. `haveBootableApplication()` returns
false once the counter reaches `Identity::kMaxBootAttempts` (3), at which point
CAL stops handing over, treats the installed application as bad, and
re-downloads it instead.

The ordering is the whole mechanism. The counter is written *before* the jump,
by the component that survives the crash, and cleared *after* success by the
component that only runs if things worked. A counter incremented after a
successful boot would prove nothing.

## Provisioning: how a device gets its secret

The Device Client Specification's §13 lists "the factory provisioning process by
which a device receives its first secret" as unspecified. This is the design
that fills that gap; it is designed, not yet built.

The one-time wired load is done from a **browser-based flasher** built on
esptool-js / WebSerial — not a native application. A browser page needs no
install, no code signing, no per-platform build and no update mechanism of its
own, which matters when the people running it are real-estate agents preparing a
handful of devices rather than a manufacturing line.

The sequence: the operator signs in to the flasher page, which authenticates to
the server; the server mints the device record and generates an **NVS partition
image containing that device's secret**; the flasher writes that image alongside
the CAL binary over USB. The secret therefore never lands in a file on the
provisioning PC — it goes from the server, through the page, onto the flash.

Two things about this are worth knowing before anyone tries it:

- **WebSerial requires a secure context** (HTTPS, or `localhost`). The flasher is
  consequently blocked on the same outstanding TLS work already listed as a
  blocking item on the server side. There is no way around this; it is a browser
  rule, not a configuration.
- **The CH340C USB-serial driver is the most likely first-contact failure.** On
  Windows the board frequently enumerates as an unknown device until the driver
  is installed, and the symptom a non-technical operator sees is simply that no
  port appears in the browser's picker. Whatever documentation ships with the
  flasher needs to lead with this rather than mention it at the end.

## Discovery: a name, and nothing else

Only two things about the outside world are compiled into CAL (`Config.h`): a
**DNS hostname** and a **well-known discovery path**. Endpoint paths, the QR
target, timeouts and branding all come from the discovery document fetched at
runtime, following §9 of the Device Client Specification.

The reason is that CAL cannot be changed. An IP address compiled into a factory
partition strands every unit in the field the day the service moves; a name can
be repointed. For the same reason, discovery returns *paths* rather than full
URLs — the host is already known, and accepting a host from the document would
be a way to redirect an entire fleet somewhere unintended. TLS uses the root
bundle rather than a pinned leaf certificate, because a pinned leaf turns a
routine ninety-day rotation into the simultaneous bricking of every device, and
CAL cannot be updated to trust a new one.

## Branding and the QR target

CAL is flashed **before** a device is assigned to a brand. The normal resting
state of a device is unassigned in an agent's pool, so there is no branding to
compile in and no way to know at flash time what it would be.

So CAL shows a neutral splash on first boot, and after it has authenticated it
caches server-supplied brand assets into LittleFS and shows those on subsequent
boots. Assets are stored as raw RGB565 at a fixed 240×120 — the server prepares
them for this exact panel, so the device carries no image decoder and no scaler,
and a wrong-sized file is discarded rather than rendered as noise. The splash is
streamed a row at a time (480 bytes) because a full-screen buffer would be
150 KB on a board with no PSRAM.

The QR code's target URL is server-supplied for the same reason. It may point at
an agent's own vanity address that redirects onward to the service, or at the
service directly. **The server decides; CAL renders what it is given** — and
always prints the URL as text underneath the code as well, because cameras fail
and a code nobody can type is a support call.

## WiFi

The device is enrolled at an agent's office and then carried to a household, so
it moves between networks as a matter of course. CAL remembers **the last three
networks it successfully joined**, most recent first, rather than only the
current one as the older CYD-Dickey firmware did — which required a by-hand
re-provision on every move. Three covers office, home and one spare without
turning NVS into a database.

Remembering is not enough on its own, because more than one of them may be in
range and the most recently used is not necessarily the one with usable signal.
So `joinStoredNetwork()` **scans first**, matches the remembered list against
what it can actually see, and tries them **strongest signal first** — taking the
strongest sighting where a mesh advertises the same name more than once. A
successful join promotes that network to the front of the list, so the ordering
reflects where the device lives rather than where it was last provisioned. If
nothing remembered turns up in the scan at all, CAL falls back to trying the
stored entries blind, which covers a network that is present but was missed.

Each entry lives in its own pair of NVS keys (`ssid0`..`ssid2`, `pass0`..`pass2`)
with a separate count, and `rememberNetwork` rebuilds all three slots rather
than shuffling in place — cheap at this size, and it avoids the class of
off-by-one bug an in-place move invites. Slots past the current count are left
behind in NVS but are unreachable, since every read is bounded by the count.

The provisioning portal itself offers **networks the device itself scanned**
rather than a free-text field, and says so on the page. ESP32 is 2.4 GHz only,
and a band-steering router advertises one SSID across both bands, so a network
plainly visible on the phone can be genuinely invisible to the device. Showing
the device's own view makes that legible instead of presenting as a wrong
password on a correct one. The captive-portal probe URLs for Android, iOS/macOS
and Windows are answered explicitly, because wildcard DNS alone lets a handset
declare the network dead and drop back to cellular partway through setup.

## Building

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32:PartitionScheme=min_spiffs" .
```

Toolchain this has been developed against:

| | Version |
|---|---|
| arduino-cli | 1.5.1 |
| `esp32:esp32` core | 3.3.11 |
| LovyanGFX | 1.2.28 |
| QRCode (ricmoo) | 0.0.1 — **vendored**, see below |
| ArduinoJson | 7.4.3 |

The QR generator is **vendored into the sketch** as `CalQr.c` / `CalQr.h` rather
than used as an installed library, and the rename is the point of the exercise.
A plain `#include <qrcode.h>` resolves to the ESP32 core's own `esp_qrcode`
header instead of ricmoo's library, and fails at compile time suggesting the
core's differently-named API. Renaming both the file and its include guard is
what makes the local copy win. The functions inside are untouched, are MIT
licensed, and do not clash with `esp_qrcode_*`. Keeping the copy in-tree also
means the QR code CAL renders is fixed for the service life of the factory
partition, which is the right property for a component that cannot be updated.

Note what that command does **not** do: it builds against the stock `min_spiffs`
scheme, not `partitions_cal.csv`. The custom table is written but not yet wired
into the build. A successful compile with `min_spiffs` therefore proves the code
builds and gives a size figure against a *1,966,080-byte* budget — it does not
demonstrate that CAL fits the 1,245,184-byte `factory` partition it is actually
destined for, which is a considerably tighter target.

## Open questions

- **CAL's compiled size is unmeasured.** The entire asymmetric layout depends on
  CAL fitting in 1,245,184 bytes, and no build has produced a number yet.
  LovyanGFX, WiFi, TLS, HTTPClient, LittleFS and the Update library are not
  small, and there is no headroom estimate worth quoting. This is the single
  open risk that could send the partition sizes back to the drawing board, and
  measuring it is in progress.
- **`partitions_cal.csv` is not wired into the build.** It needs a
  `boards.local.txt` entry or an equivalent custom-scheme registration before
  anything can be flashed with it. Until then the file documents an intention
  rather than describing a build.
- **`Provisioning::run()` never returns false.** Its contract says it gives up
  if the household abandons setup, but the implementation loops until
  credentials are accepted. There is no abandonment timeout.
- **No automated tests, at all.** There is no test harness in this repository
  and no obvious one to reach for: the code is inseparable from ESP32
  peripherals, NVS, WiFi and TLS, none of which have a usable stub here. The
  testing story for this firmware is genuinely unsolved, and the size
  measurement above is currently the only mechanical verification of anything.
  This is a real gap, not a deferred chore — the server side treats a test
  project as shipping alongside the logic it covers, and nothing equivalent
  exists here.
- **Nothing has run on hardware.** Every behaviour described in this document is
  designed and written; none of it is proven on a device.
- **The application's side of the contract does not exist yet.** Setting
  `updreq` and clearing the boot-attempt counter are CAL's expectations of an
  application that has not been written to meet them.
