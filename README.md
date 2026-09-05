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

`partitions.csv`, at the repo root. Asymmetric on purpose, and this is the
single decision the whole project rests on.

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
| `factory` | app | factory | `0x10000` | `0x160000` | 1,441,792 | **CAL** — never written by OTA |
| `ota_0` | app | ota_0 | `0x170000` | `0x250000` | 2,424,832 | The product application |
| `spiffs` | data | spiffs | `0x3C0000` | `0x30000` | 196,608 | Cached branding assets |
| `coredump` | data | coredump | `0x3F0000` | `0x10000` | 65,536 | |

That fills 4 MB exactly, with nothing left over. `factory` was sized against a
measured CAL build (see below) rather than a guess, with room to grow; `ota_0`
is **2,424,832 bytes against a 2,166,784-byte build** — about 258 KB of headroom
where there was previously a 196 KB deficit. The consequence worth stating
plainly: the previously proposed remedy of dropping the ESP32-A2DP Bluetooth
audio stack to reclaim 300–500 KB is **no longer required by the size ceiling**.
It may still be worth doing on its own merits, but it is no longer the price of
having OTA at all.

The `spiffs` label is what the ESP32 Arduino LittleFS library looks for by
default; the partition is formatted and used as LittleFS despite the name. That
is conventional, not an oversight, but it is the kind of detail that reads as a
bug six months later.

**CAL's own size is measured, not guessed, and re-verified on every build.**
`ci/build-firmware.sh` compiles CAL, and the layout above is what actually gets
used - see *Building*, below, for how that was confirmed rather than assumed.
As of the last build: **1,318,891 bytes**, comfortably inside the 1,441,792-byte
`factory` partition. Re-measure before trusting these numbers if the firmware
grows meaningfully - `factory` cannot be resized for a unit already flashed.
Nothing here is proven on hardware yet - see Open questions.

**The App's own size, measured the same way.** As of the card-manager build:
**1,353,683 bytes of program storage and 55,356 bytes of globals** (leaving
272,324 bytes of the 327,680-byte DRAM for locals). That is **55.8% of the
2,424,832-byte `ota_0` partition**, with 1,071,149 bytes spare. Note that
`arduino-cli` reports this build as "68% of 1,966,080 bytes" - that is the
stock `min_spiffs` table's own app slot, not this project's layout, and it is
the wrong ceiling to read. `ota_0` above is the real one. The card manager,
actions, SD and PNG support together cost **+71,136 bytes** over the previous
build's 1,282,547, and **+1,752 bytes** of globals over its 53,604; nearly all
of that is the SD library and LovyanGFX's PNG decoder being linked in for the
first time, which was expected.

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

## The App firmware

`App/` is the product application the sections above refer to only from CAL's
side of the fence — the thing "the application" means throughout *Updates:
reboot to the updater*. It runs from `ota_0`, installed and started by CAL,
and reuses CAL's own `Identity.h`/`.cpp` and `Tls.h`/`.cpp` verbatim (same NVS
namespace, same TLS trust setup) plus a trimmed `Config.h`. Unlike CAL, it is
meant to run indefinitely — failures here retry rather than halt, because a
display that goes dark until someone finds a USB cable is a worse outcome for
a household than one that keeps trying. It renders a rotation of cards —
weather, aircraft overhead, a picture the server picks, sunrise/sunset, and a
standalone clock/date card — scheduled by a policy the server hands down on
check-in and navigable by touch (see *The card
manager*, below).

### The weather and aircraft cards, styled after CYD-Dickey

A separate, more mature project on similar hardware — `CYD-Dickey`, a
Discover Around Me build for the same ESP32/ILI9341 panel — already has a
working weather card and an aircraft-overhead card. `App/`'s own weather card
was restyled to match its actual look, and its aircraft card is new; CAL had
no aircraft rendering of any kind before this.

**The weather card** (`Display::showWeatherCard`) moved from a screen-centered
black card to CYD-Dickey's actual layout: a white background, a small
colour-banded "WEATHER" label in the top-left corner (`Display.cpp`'s
`drawCardBanner`, mirroring their `lcd.fillRect(0, 0, 110, 22, TFT_NAVY)` +
bold-white-label pattern from `drawWeatherCard()`), the temperature set
**left-aligned** in a bold sans GFX font rather than centered bitmap text, and
the short-forecast text below it left-margined instead of centered. The
hand-drawn degree ring from before (`drawTemperature`, formerly
`centeredTemperature`) stayed — CYD-Dickey sidesteps the missing-glyph problem
by never printing a degree sign at all (`"%.0fF\n"`), but CAL already had the
better fix, and regressing to their plain "72F" would be a downgrade dressed
up as alignment. `location` (Home or Target's city/state, which CYD-Dickey's
card has no equivalent field for) is kept.

#### Why the weather card was restyled a second time

That first pass copied CYD-Dickey's *elements* faithfully and was still
reported as worse than the original once it was seen on hardware. Reading the
two implementations side by side explains why, and it is not a detail either
version got wrong.

CYD-Dickey's card carries **five live readings and a five-day strip**:
temperature, condition, feels-like, humidity and wind, then a bottom band of
`M/D` plus high/low for five days (`drawWeatherCard()`, `WeatherInfo`,
`DailyForecast`). It fills the panel top to bottom, and its modest 12pt
temperature is the right size *because* four other readings and a forecast
band are competing with it for the same 320×240.

CAL's card has **three fields** — temperature, condition, location — because
that is all `/api/myweather/mine` sends. Copying a dense layout's type sizes
onto a third of its content produced the worst of both: roughly 70 vertical
pixels of content on a 240-pixel panel, everything below y≈120 blank, and
nothing set large enough to read from across a room. It was *small* and
*empty* at the same time.

So the second pass stopped imitating their density and spent the space
instead — fewer facts, set larger:

- **The temperature is now a hero number** at `FreeSansBold24pt7b` (35px
  digits) rather than sharing 12pt with everything else, with the hand-drawn
  degree ring scaled to match. `drawTemperature` takes the ring radius as a
  parameter now, because a fixed 5px ring that read as a degree mark beside a
  12pt numeral reads as a stray speck beside a 24pt one; at large radii it
  draws two concentric circles so the ring has a stroke weight in the same
  family as the bold digits beside it.
- **The two supporting lines are legible.** `location` and the "updated" line
  were both `Font0` — the 6×8 bitmap font — in muted grey. That is the exact
  combination this file already records as having *failed on real hardware*
  for the corner clock: about 3mm tall on a 2.8" panel, low contrast, and
  reported by the first person who saw it as simply not being there. The clock
  was fixed at the time; these two lines on the same card had the identical
  defect for the identical reason and were not. Both are now set in that same
  bold 9pt face, and `location` moved to its own line under the banner.
- **The condition phrase picks a size that fits rather than clipping.** NWS's
  `shortForecast` is a whole phrase ("Chance Showers And Thunderstorms then
  Partly Sunny"), not CYD-Dickey's one-or-two-word `weatherCodeDescription()`.
  It renders at 12pt while it lands in two lines or fewer and drops to 9pt and
  three lines when it does not (`wrappedLeftText` gained a `measureOnly` mode
  so the same wrap that draws is the one that measures). Truncating mid-phrase
  can invert the meaning of exactly the sentences worth reading — "…then
  Clearing".
- **The "updated" line is now true.** It was the hardcoded string `"Updated
  just now"` passed on *every* draw, including redraws of a forecast fetched
  twenty minutes earlier and including reverse navigation into card history,
  where it was false by construction. `Weather.cpp` now records `millis()` at
  each successful fetch and the card says "Updated 7 min ago". A freshness
  label that is always the same string is worse than none, because it reads as
  a live measurement and is not one. It is pinned to a fixed baseline rather
  than flowing under the condition block, so it does not jump around the card
  when the forecast wording changes length.

**What was deliberately not done:** the empty space was not padded with
invented content. There is no placeholder humidity, no fake wind reading and
no forecast strip drawn from data the device does not have.

#### What CYD-Dickey's weather card shows that this server does not send

This is the real reason the two cards cannot be made to match, and it is
**server-side, not firmware-side**.

*Not modelled server-side.* Feels-like, humidity and wind come from
Open-Meteo's `current` block in CYD-Dickey (`apparent_temperature`,
`relative_humidity_2m`, `wind_speed_10m`). DiscoverAroundMe's weather pipeline
is built on the US National Weather Service instead, and `WeatherPeriod` — the
server's own model — carries none of the three. The cost of adding them is not
uniform, and it is worth being exact rather than lumping them together:

- **Wind** is the cheap one. NWS's `/forecast` periods *do* carry `windSpeed`
  and `windDirection`; `NwsForecastClient.ForecastPeriod` simply does not map
  them. That is a field on an existing DTO, not a new integration.
- **Humidity and feels-like** are not in what this endpoint's periods provide.
  Getting them means a different NWS endpoint or a second provider — a data
  source change, not a display field.

None of this is done here, and none of it is faked on the card.

*Available on the server, deliberately withheld from the device.* The five-day
strip is the bigger loss and the more fixable one. `WeatherResult.Periods` is
a full NWS forecast — roughly a fortnight of periods, each with `Name`
("Tuesday", "Tuesday Night"), `IsDaytime`, `Temperature`, `ShortForecast` and
`StartTimeUtc`. `MyWeatherEndpoints.ForDeviceLocation` **trims that to
`Periods[0]`** before it goes over the wire, for a documented and entirely
sound reason: the untrimmed response measured 9,194 bytes for a real account,
and a real device failed to parse it, because ArduinoJson's DOM parser needs
roughly its own input size again in heap on a chip that has already spent most
of it on WiFi and TLS.

So the data exists and the device is not allowed to see it. Closing this gap
is a small, bounded server change — send the first *N* periods carrying only
`name`, `temperature` and `shortForecast`, which is a few hundred bytes rather
than nine thousand, nowhere near the ceiling that forced the trim — and then a
firmware change to render the strip. **Neither is done here**, and the card is
designed around the three fields that genuinely arrive rather than around a
strip that would have to be faked to appear. The aircraft card below applied
this same standard to its own missing airline/route data for a while, until
that gap was closed server-side — see "Airline name, logo, and route" below
for what changed once it was.

**The aircraft card** (`Display::showAircraftCard`, `App/Aircraft.h`/`.cpp`)
is new. It fetches `/api/myaircraft/mine` with the device's own secret — the
same authentication, refusal-body shape (`reason`/`message`), and
`NetworkClientSecure`/`HTTPClient`/`Tls::configure` idiom as `Weather.h`/`.cpp`
and `CheckIn.cpp` already use — and shows the nearest aircraft the server
reports (the response is already sorted by distance and pre-filtered to
airborne traffic within the owner's radius). Visually it borrows CYD-Dickey's
`drawFeaturedAircraft()` layout: a bold headline at top-left, then stat rows
below with a muted label on the left and the value right-justified against
the card's right margin, using the same truncate-and-right-justify technique
as their `drawTruncatedRight()` (`Display.cpp`'s `drawRightJustified`) so a
short value and a long one both end flush at the same edge. The 8-point
compass-direction lookup for `headingDegrees` is also a direct port of theirs.

**Airline name, logo, and route — added after the gap above was closed
server-side.** `AircraftSighting` now carries seven more fields, all optional:
`airlineCode`, `airlineName`, `airlineLogoAssetId`, `originCode`,
`destinationCode` (plus `originName`/`destinationName`, which this card reads
the wire contract for but does not parse — see below). The server resolves
these the same way CYD-Dickey's own `Airlines.h`/`Route.h` did, just fleet-wide
in one cache instead of twelve entries of per-device RAM lost on every reboot.

`airlineName` takes over the headline slot CYD-Dickey gives its logo/name and
this card previously gave the bare callsign; callsign drops to the secondary
line alongside distance instead of disappearing (`"UAL123 - 4.2 mi away"`).
Empty `airlineName` — a server old enough to predate the field entirely, the
6-month backward-compatibility case — falls straight back to callsign as the
headline, this card's entire original behaviour with nothing new to detect or
branch on.

A route line appears between the distance line and the stat rows when
`originCode` is present: `"KRDU -> KLGA"`, or `"from KRDU"` alone when no
destination was on file. Neither present draws no line at all, the same
"nothing configured, nothing shown" rule Graphic.cpp already follows for a
missing picture — this card does not fabricate a dash or a placeholder where
there is no data. **Codes only, never the full names the server also sends**:
two airport names plus everything else already on this card (headline,
distance, three stat rows, freshness) does not fit readably on a 320x240
panel, the identical "small and empty vs. too dense" tradeoff the missing
weather forecast strip above already documents. `originName`/`destinationName`
are consequently never parsed by `Aircraft.cpp` at all — a field the filter
does not whitelist is simply never seen, not wastefully decoded and discarded.

**The logo** is the first image this build draws that is *not* the whole
card. Every existing PNG draw here (`Assets::drawCached`/`drawFullScreen`,
`Graphic.cpp`'s picture card) fills the entire 320x240 panel; a small airline
logo alongside text needed a new primitive, `Display::drawPngFromSdInRect()`
and `Assets::drawCachedInRect()`, bounded to a caller-given rectangle rather
than clearing and filling the whole screen. The rectangle itself —
`Display::aircraftLogoZone()`, top-right of the content area — is decided in
`Display.cpp` for the same reason all of this build's card chrome geometry
lives there, even though `Display.cpp` never touches `Assets.h` itself:
`Aircraft.cpp` owns the asset id and calls `Assets::drawCachedInRect()`
directly from its own `cardDraw()`, the identical module boundary
`Graphic.cpp` already keeps. The logo is fetched (`Assets::ensureCached()`)
from `cardFetch()`, never from `cardDraw()` — the same fetch/draw split every
other card in this build follows, so stepping backwards through the rotation
is never a network operation.

This bounded-rectangle draw is **unverified on hardware more pointedly than
most of this codebase**. Every other PNG draw here fills the whole panel; this
is the first one that doesn't, and the auto-fit-within-a-box behaviour is read
from LovyanGFX's own `drawPngFile` parameters, not confirmed against an actual
decode of an actual logo on an actual panel.

Both cards also gained a "content status" treatment for their non-Ok states
(not yet activated, disabled for this owner, nothing currently in range, auth
or network trouble) — `Display::showWeatherStatus`/`showAircraftStatus` keep
these in the same white/bannered card family rather than routing them through
the black boot-ladder `showStatus()`/`showFailure()` screens, matching
CYD-Dickey's own split between `drawStatusMessage()`'s black Wi-Fi/menu
screens and `drawNoAircraftScreen()`/`drawNoListingsScreen()`'s white,
card-styled ones for content problems specifically. The boot-ladder screens
themselves (Wi-Fi joining, time sync, update-in-progress) are unchanged.

**The two-way toggle these cards used to live on is gone.** An earlier
version of this document called that out as "a known simplification":
`App.ino` held an `enum class CardKind { Weather, Aircraft }` and flipped
between them on the content-refresh timer, while CYD-Dickey's real scheduler
— independent interleave timers, touch-driven forward/back, a history buffer
so rewinding replays exactly what was shown — had nothing to attach to.
That is no longer true. Both cards are now ordinary registered descriptors
and the scheduler is real; see *The card manager* below.

### A corner clock and a day/night theme, both driven by check-in

`/api/checkin`'s response carries two more fields alongside
`checkInIntervalSeconds`/`updateAvailable`/`debugStreamRequested`:
`utcOffsetMinutes` (signed minutes to add to UTC for this device's correct
local time, DST already applied) and `isDaytime` (whether the Sun is up
right now at the device's location) — both recomputed by the server fresh
on every check-in from the device's own location, not looked up once and
cached or guessed at from a fixed schedule. `CheckIn::Result` parses both
the same way as every other check-in field, and `performCheckIn()` in
`App.ino` persists them into two more file-local statics
(`lastUtcOffsetMinutes`/`lastIsDaytime`, alongside the existing
`checkInIntervalMs`) on every *successful* check-in — not one-shot, so both
track the server's current answer rather than latching whatever the first
check-in ever said. `isDaytime` defaults to `true` before this run's first
check-in ever completes, matching the server's own fallback for a location
it can't yet resolve (`DeviceLocalTimeResult.Fallback` in the DiscoverAroundMe
repo) — there is nothing to persist across a reboot for a value the server
itself only guesses at until it knows better.

**`utcOffsetMinutes` gets one more copy than `isDaytime` does, because its gap
is worse.** A device that has just powered on — WiFi still joining, or SNTP
still failing and retrying every 10s in `synchroniseTime()` — can go through
several minutes of drawing cards before its first check-in of this boot ever
lands, and until this existed every one of those frames drew raw UTC as if it
were local time. `Identity::setLastUtcOffsetMinutes()` mirrors the offset to
NVS (key `utcoffmin`, same `"cal"` namespace as everything else in
`Identity.h`) on every successful check-in, right alongside the existing
in-RAM copy, and `setup()` reads it back with `Identity::
lastUtcOffsetMinutes()` before WiFi, time sync, or anything else in that
function has run. A device that has completed even one check-in in its life
therefore draws with that offset — stale by at most a day, never by more,
since check-ins keep refreshing it — from its very first frame, instead of
raw UTC while it waits. A device that has never completed a check-in — the
factory-fresh case, NVS empty — reads back `0` and behaves exactly as it did
before this existed: not a new failure mode, just a narrower one than before.

**The clock** (`Display.cpp`'s `drawClock()`) is `time(nullptr) +
utcOffsetMinutes * 60`, turned into wall-clock fields with `gmtime_r` and
formatted `HH:MM` — the same time_t-to-tm idiom `CheckIn.cpp`'s
`nowAsIso8601Utc()` already uses, just shifted by the offset instead of left
at UTC, and with no DST logic of its own since the server already folded
that into the offset. It's drawn bottom-right, small and muted, by every one
of the four card-family functions (`showWeatherCard`/`showAircraftCard` and
their `*Status` variants) — deliberately including the error states, since a
clock is chrome, not content, and shouldn't disappear just because a card
is showing a problem. It is **not** on its own per-second ticker: it
redraws only when the card underneath it redraws (a content refresh, a
forced update check, or now a touch tap), which was judged the right amount
of engineering for a small corner clock rather than building a partial-
redraw path that ticks independently of everything else on screen — stated
here plainly as a real trade-off, not an oversight.

**The theme** (`Display.cpp`'s `bg()`/`ink()`/`muted()`) is a wholesale
swap between two palettes that both already existed in this file before
this change, applied uniformly: "day" is exactly the white-background/
black-ink/grey-muted look the content-card family already had, and "night"
is exactly the black-background/white-ink look the boot-ladder screens
(`showStatus`/`showFailure`) already had. Rather than leaving that a fixed
split — cards always white, boot screens always black — every screen this
file draws now picks whichever pair `isDaytime` currently says, so the
*whole* App matches what a household would see out their own window at
that moment, not just the two content cards. The one deliberate exception
is the weather card's big temperature number, which is tinted with the
weather banner's own navy by day (it reads fine against white, and echoes
the banner colour) but falls back to the plain theme ink colour at night,
since that same navy would be nearly invisible against a black background
— the banner rect above it still carries the navy accent either way, so
nothing brand-identifying is actually lost. The colour-banded banners
themselves (`WEATHER` navy, `OVERHEAD` blue) and the amber warning colour
are deliberately **not** part of the swap — both already read fine against
either background, and giving them night variants too would be theme-
following for its own sake rather than solving a real legibility problem.

### Touch input: the XPT2046 controller, used for the first time

This board — 2.8" ILI9341 + XPT2046 resistive touch, the same "Cheap Yellow
Display" family as `CYD-Dickey` — has always had a touch controller; the App
simply never used it, wiring card advancement to a plain timer instead.
`App/Touch.h`/`.cpp` is a small, reusable, card-agnostic module: one physical
tap produces exactly one event, edge-detected the same way
`forceUpdateCheckRequested()` already debounces the BOOT button, polled once
per `loop()` iteration.

**The coordinate is no longer thrown away.** The first version of this file
returned a bare `bool` from `wasTapped()` — `Display::readTouchRaw()` was
already handing back `x` and `y` and they were discarded, so a tap anywhere
on the glass meant one single thing. `Touch::poll()` now classifies the tap
into a zone: an action button, the reverse (left edge) strip, the forward
(right edge) strip, or none. The edge strips are 16px wide and full height,
the same dimensions CYD-Dickey settled on for the same panel (`x < 16` /
`x > 304` in its own touch handler).

**Zone priority is fixed, not incidental:** action buttons are tested first,
edges second. The edge strips have no visible chrome of their own and run the
full height of the screen, so a button that happens to sit near an edge has to
win — the same ordering CYD-Dickey uses, where its corner menu buttons are
checked before its edge zones for exactly that reason. Geometry is decided by
`Display` (which is the only thing that knows this panel's layout and what
else is already drawn on it) and handed to `Touch::setActionZones()` after
every card draw, so the hit test and the drawing can never disagree about
where a button is, and a zone belonging to the previous card can never still
be live under the current one. Nothing in `Touch.h`'s shape assumes cards are
the only thing that will ever consume it: this file knows about rectangles and
screen edges, not about what advancing means.

`loop()`'s trailing `delay()` came down from 1000ms to 50ms as part of this.
Everything else in that loop is gated on its own `millis()` comparison and
does not care how often it is asked, but touch is sampled inside
`CardManager::poll()`, and sampling once a second misses most of a real tap —
a finger is on the glass for a fraction of that. That was already true when a
tap only advanced a card, where a missed tap costs nothing worse than tapping
again. It is much less acceptable now that a tap can be a button press whose
whole design is that the person gets no confirmation, and so would never learn
it had not registered.

The actual hardware read (`lcd.getTouch(&x, &y)`) lives in
`Display::readTouchRaw()`, not in `Touch.cpp` itself — `Display.cpp` already
owns the one `LGFX` instance for this panel (via `LGFX_AUTODETECT`, the same
autodetection CAL's own `Display.cpp` uses for the screen), and giving
`Touch.cpp` a second `LGFX_AUTODETECT` instance addressing the same physical
SPI bus/controller would risk re-initialising hardware `Display::begin()`
already brought up. `Touch.cpp` calls that accessor and adds only the
debounced event shape on top — no hand-rolled SPI or XPT2046 register
access anywhere in this codebase; LovyanGFX's autodetect wires up
calibration and reading for this board the same way it wires up the panel,
confirmed by `CYD-Dickey/TouchKeyboard.cpp` (`lcd.getTouch(&x, &y)`) already
working this way on the same hardware family.

**Stated plainly, because a compile can't prove this one:** nobody has
confirmed touch actually works on this exact ELEGOO board. The display
itself was separately confirmed compatible earlier — that confirmation
never exercised touch. This firmware has no automated tests at all (see
*Open questions*), and touch specifically has zero automated coverage
possible even in principle here — proving it out needs an actual finger on
actual glass, which as of this commit nobody has done. Treat the tap-to-
advance behaviour above as designed and compiled, not as verified.

Zones make that unverified assumption *load-bearing* in a way a bare
tap-anywhere never was. Before, an inverted or unscaled axis would have gone
completely unnoticed — every tap did the same thing. Now it would send a
"forward" tap backwards, or put a button's hit zone somewhere other than
where the button is drawn. Calibration is LovyanGFX's autodetect's job and
`CYD-Dickey/TouchKeyboard.cpp` relies on it working on the same hardware
family, but that is inherited confidence, not measurement.

### The card manager: a registry, not a toggle

`App/Cards.h` and `App/CardManager.h`/`.cpp` replace the two-way toggle
described above with the real scheduler, ported from CYD-Dickey's
`Screen::Ready` rotation — its `computeNextCard()`, `advanceCard()`,
`rewindCard()` and the long design comment above them.

**What a card is, now.** A card used to be smeared across three places: a
fetch module (`Weather.cpp`, `Aircraft.cpp`), a draw function in
`Display.cpp`, and the hardcoded `CardKind` toggle in `App.ino` that named
both of them. Adding a third card meant editing all three. A card is now a
`Cards::CardSpec` descriptor — an id, a kind, a fetch function, an
item-count function, a draw function, an optional "is this item notable"
predicate — that the card module registers itself at static-init time (see
the block at the bottom of `Weather.cpp` and `Aircraft.cpp`). `App.ino` does
not name a single card anywhere any more; it brings up the hardware and the
network, calls `CardManager::begin()`, and pumps `CardManager::poll()` once
per `loop()` iteration.

**Deliberately not CYD-Dickey's shape, in exactly one place.** That project
uses a hardcoded `enum class CardSlot { Base, Weather, Splash, Qr }` plus
three separately-named globals, `cardsSinceWeather` / `cardsSinceSplash` /
`cardsSinceQr`. A fifth card type there needs a new enum case, a new global,
a new `if` in `computeNextCard()` and a new `case` in
`drawDashboardScreen()`. Card types on this project are expected to keep
growing, so every one of those named globals is a per-descriptor struct field
here (`CardSpec::cardsSince`) and the enum is a registry index. Adding a card
type is registering a descriptor plus server-side config; the scheduler does
not change.

**Interstitials interleave, they do not take a slot.** Two kinds exist.
A `list` card is a variable-length collection whose items are cycled one at a
time. An `interstitial` is a singleton that shows *after every N other cards*
(`interleaveEvery`) rather than occupying one fixed position in a rotation.
That distinction is carried over deliberately and the reason is recorded
rather than guessed at: with a fixed slot, a device tracking a dozen list
items showed its singletons proportionally less often — CYD-Dickey's own note
says "with a dozen+ data cards, once every couple minutes, easy to miss
entirely." Every active card's counter ticks on every computed card, the
first to exceed its own `interleaveEvery` wins, and ties break on `order`,
lowest first. The counters are fully independent: an earlier CYD-Dickey
version forced two of its singletons into a fixed pair (QR always following
splash) and records having corrected that, so nothing here couples one
interstitial's cadence to another's.

**Forward and reverse share one history, not two code paths.** Every
genuinely-new card is recorded in a 24-entry ring buffer as it is first shown
(`CARD_HISTORY_CAP = 24` there, the same here). Rewinding walks the cursor
back and replays recorded entries exactly — including landing back on an
interstitial, not just on list items. Advancing after a rewind replays
*forward* through that same recorded stretch rather than recomputing:
recomputing could put a different card in a position the user just stepped
past, which would make "which card is where" depend on which direction you
happen to be travelling. Only once the cursor reaches the frontier does
advancing compute something new. The ring collapses to a single entry
whenever a card refetches, for the reason CYD-Dickey's `resetCardHistory()`
exists — a recorded item index must never be replayed against a list that no
longer holds the same items at the same positions — and deliberately leaves
the interleave counters alone, so a data refresh does not throw off the
singletons' cadence.

**Navigation never fetches.** `fetch()` and `draw()` are separate functions
on the descriptor and the scheduler only ever calls `fetch()` from its own
refresh timer, never from a navigation path. A rewind is a pure redraw of
state the card is already holding. If it refetched, stepping back could show
you something you never saw going forward, which defeats the entire point of
the history buffer. `CardManager::poll()` refreshes at most one due card per
call, so a refresh sweep never blocks `loop()` on several HTTP round trips
back to back.

**Manual navigation holds off the timer.** A deliberate tap suppresses
auto-advance for `manualNavHoldSeconds`, so a card someone picked on purpose
is not yanked away a second later. CYD-Dickey hardcodes this as
`MANUAL_NAV_HOLD_MS = 30000`; here 30 seconds is the built-in default and the
server can change it.

**Empty cards are skipped, blank cards are not shown.** A card reporting zero
items is passed over entirely. Note what does *not* count as empty: a card
holding an explanatory status — "no aircraft within 10 mi right now",
"weather is not showing yet" — reports one item, because that message is
content and has always been shown. In practice the skip fires for a card that
has not fetched yet, which is the ordinary state for the first second or two
after boot. If nothing at all has anything, `Display::showNoContent()` says
so rather than leaving the panel blank.

**The policy comes from the server, on the check-in that already exists.**
`CheckInResponse.cardPolicy` carries `defaultDwellSeconds`,
`manualNavHoldSeconds`, and a `cards` array of `{id, kind, order,
dwellSeconds, interleaveEvery?, notableDwellSeconds?, assetId?}`. Three rules
keep this safe in both directions of the six-month firmware
backward-compatibility mandate:

- **No policy means keep the one you have.** An omitted or empty `cardPolicy`
  is explicitly not "show nothing" — a server that cannot resolve a policy
  must never blank a screen that was working. A device that has never
  received one uses the built-in defaults compiled into each descriptor.
- **An unknown card id is ignored, not an error.** That is what lets the
  server add a card type before firmware supports it.
- **A policy that matches *nothing* leaves every card active.** The mirror
  image of the rule above: a newer server whose whole policy names card types
  an older device does not have would otherwise switch every card off and
  leave a dark screen. A card the policy *does* omit while naming others is
  taken out of the rotation, which is how the server turns a card off — the
  fallback only fires when nothing matched at all.

`notableDwellSeconds` is the generalised form of CYD-Dickey's
`aircraftOverheadSeconds = 20`: a longer hold for an item the card itself
considers more interesting. The aircraft card's `isNotable` uses their rule
too — 20% of the configured tracking radius with a 1-mile floor, so
"practically overhead" still means something whether the owner tracks 3 miles
or 30, rather than a hardcoded mile count.

`assetId` is the newest of these and the only one that is *content* rather
than scheduling: it names the picture a card should draw, and it exists so
that changing which picture a household sees is a config edit rather than a
firmware release. It is optional, absent from every card that draws no
picture, and empty is an ordinary state and not a fault — see *A picture as a
card*, below. The server side of it is being built in parallel with this
firmware, so nothing has ever sent one.

### Card buttons and the pending-action queue

`App/Actions.h`/`.cpp` adds buttons a card can draw and a queue of presses
waiting to be delivered. Two halves that only touch each other at the moment
of a press: the **definitions** (`CheckInResponse.cardActions`, held in RAM
because the server re-sends them every time, so persisting them would just be
another thing to fall out of sync) and the **pending queue** (NVS-backed,
because a press a power cycle eats is a press the household believes they
made).

**The device reports intent, never meaning.** `actionId` is opaque here. This
firmware knows `"im-ok"` was pressed; it does not know an email goes out, or
to whom. That binding is a database row in the server's `Commands` domain, so
changing who gets notified is a config change rather than a firmware release
across a fleet. `label` is drawn verbatim and never interpreted.

**Passive push, no confirmation, no new endpoint.** A press is recorded
locally, rides the next ordinary `/api/checkin` as `pendingActions`, and the
device's job ends there. There is no round trip and no "sent" state for the
user to wait on. Worst-case latency is one check-in interval — which the
server already controls, so tightening it is a config change, not a new
route. A single press fires the action; there is no confirm tap.

**`instanceId` exists for dedup and for nothing else.** It is
`"<device>:<monotonic counter>"`, the counter persisted in NVS and
incremented *before* the press is stored, so a power loss mid-write costs an
unused id rather than two presses sharing one. Its whole purpose is that if a
check-in succeeds but its response is lost, the device re-sends the same press
and the recipient is not notified twice. It is not user-visible
confirmation and nothing in the UI is built on it. The prefix is the device's
MAC address rather than its server-side device id, because nothing has ever
told a device its own id — `CheckInRequest` deliberately carries no
`DeviceId` at all — and the server, which already knows who is calling, only
needs the prefix to be stable and device-local.

**One-shot consume, running the other way.** The device keeps carrying a press
until `acceptedActionIds` in a response names its `instanceId`; only then does
it drop it from NVS. This is the same handshake shape as
`FirmwareUpdateForced`, in the opposite direction. The queue holds eight
presses; a press arriving at a full queue is dropped and logged rather than
evicting one already recorded, because a queue that deep means check-ins have
been failing for a long stretch and the *earliest* presses are the ones that
still describe what happened.

**The one thing drawn that the contract does not describe:** a pressed button
flashes and comes straight back (`Display::flashActionButton`). That
acknowledges the *press*, not the delivery — a button that does not visibly
react to a finger reads as a dead button, which is its own separate failure —
and it claims nothing about what the server did with it. Pressing also
triggers the same manual-nav hold a navigation tap does, so a card someone
just pressed a button on is not swapped out from under them.

Geometry is entirely the device's: a row of up to three buttons along the
bottom, from x=8 to x=312 and ending at y=220, clearing the corner clock's
bottom-right patch above it. It does *not* need to clear the 16px
reverse/forward edge strips on either side the way it looks like it should —
`Touch.cpp`'s `poll()` checks action-button zones before the edge strips, so
a tap landing inside a button rect is always a button press regardless of how
close it sits to the physical edge; real fingers found the original,
edge-clearing width too narrow to hit reliably. The server says what a
button is called and what it means; only the device knows its own panel.

**Reverse/forward taps flash too, the same way** (`Display::flashNavEdge`),
for the identical reason: an edge strip with no visible chrome of its own
gave a tap there no acknowledgment at all before rewind()/advance() ran.
The flash only fills up to `kButtonRowY`, not the panel's full height,
specifically because the edge strip's x-range overlaps the button row's —
Touch::poll()'s button-first priority means a tap actually lands on
Hit::Reverse/Hit::Forward there only where no button covers it, but a
flash filling the whole column would still paint over a button that *is*
drawn at that x within the button row, and painting over a button is only
harmless because the very next thing to happen is `drawCurrent()` redrawing
everything — except on the one path that skips that redraw entirely
(`rewind()` when there is no history to step back into), where the button
would otherwise stay visibly bitten into until some unrelated redraw fixed
it.

### Assets and the SD card

`App/SdStorage.h`/`.cpp` mounts the card (CS pin 5, the same one `CYD-Dickey`'s
`SdCard.cpp` uses on this board family — unlike the panel, a card slot's CS
line is just a wiring fact, not something autodetect can find). `App/Assets.h`
/`.cpp` is a cache of images on top of it, addressed by server-side asset id:
**SD hit → draw. SD miss → fetch → store → draw. Fetch failed → draw
nothing.** A miss must never block a card; a household staring at a frozen
screen while a PNG times out is a far worse outcome than a card with no
picture on it. Same storage approach as CYD-Dickey, which keeps its PNGs on SD
and addresses them by path (`splashImage = "/LRBH.PNG"`); the difference is
that the path is derived from a server id rather than typed in by a person, so
the server's catalog is the source of truth and a device populates its own
cache on demand.

Downloads stream straight to the card rather than through a `String` — an
asset is tens of kilobytes and this device has roughly 274KB of free heap, so
buffering the whole body first is exactly the allocation that would turn a
slightly-too-large image from slow into fatal. They are written to a `.part`
name and renamed on success, so an interrupted download cannot leave a
truncated file that every later lookup then treats as a cache hit.

**The device never validates image size.** The server normalises every image
at upload time — resized to a per-asset-type target, re-encoded, metadata
stripped — so what arrives here is already device-appropriate. A device with
274KB of heap must never be the thing that discovers an image was too big;
that discovery belongs at upload, where a person can see it.

Two small mechanical details are inherited from CYD-Dickey and both matter.
`SD.h` must be included *before* `LovyanGFX.hpp` in `Display.cpp`: LovyanGFX
detects SD image support by checking whether the SD library's own include
guard is already defined, and including it afterwards fails to compile with
`abstract type DataWrapperT<fs::SDFS>`. And `lcd.releasePngMemory()` is called
unconditionally after every PNG draw, including a failed one — LovyanGFX keeps
the decoder's buffers allocated on purpose for cheap repeat-draws, and
CYD-Dickey found that starving the memory its Bluetooth init needed
immediately afterwards.

**Storage is treated as effectively unlimited but measured.** The card is
user-upgradeable, and a cache that has to reason about eviction is a great
deal of machinery for a problem a larger card solves. "Unlimited" is only a
defensible position while somebody can see how full it is, so `Telemetry` now
reports `sdTotalBytes`, `sdUsedBytes` and `assetCount` alongside free heap —
same mechanism, same cadence, so storage pressure shows up fleet-wide on
`/diag/telemetry` before it shows up as a device that quietly stopped caching.
All three read zero on a device with no card in the slot, which is an ordinary
state and not a fault: nothing mounts, nothing caches, weather and aircraft
carry on untouched, and the graphic card below simply never appears.

**Two callers, and they ask different questions.**
`Assets::showBootSplash()` puts a cached `splash` asset up at boot if there is
one, the way CYD-Dickey's `showSplashScreen()` does, and silently does nothing
otherwise — deliberately without fetching, since boot is the one moment where
waiting on the network to draw a decoration is least defensible. The graphic
card (below) is the other, and it needs the fetch and the draw to be separate
calls, so `Assets` exposes both halves: `ensureCached()` / `drawFullScreen()`
may hit the network and belong on a fetch path, while `isCached()` /
`drawCached()` never touch it and are what a card's `draw()` is allowed to
call.

**Partially run now, on real hardware — and it found a real bug.** The fetch
path written above, `/api/assets/{id}`, was this firmware's guess at a route
that did not exist yet when it was written, and nobody went back to check it
once the server side actually shipped with a different shape:
`GET /api/assets/{id}/content` (see `AssetsEndpoints.cs`). A real device
reached the real server correctly — TLS, `X-Device-Secret`, the request
itself all worked — and got back a real, honest 404, because the path was
simply wrong. Caught from the device's own remote debug log
(`[assets] fetch of '...' failed, http status=404`), which is exactly the
diagnostic this domain exists to provide. Fixed to the real path.

What is still genuinely unverified is everything past a 200: no device has
decoded a real PNG or written one to its own SD card, because no fetch has
yet succeeded far enough to try. A clean compile plus one confirmed-real
404-to-be-fixed is the current state of verification, which is more than a
clean compile alone but well short of "this works."

### A picture as a card, chosen by the server - three of them

`App/Graphic.h`/`.cpp` puts an image into the rotation as a card of its own,
and provides **three independently-configured instances**: ids `graphic`,
`graphic2` and `graphic3`. Each registers its own `CardSpec` exactly the way
`Weather.cpp` and `Aircraft.cpp` register theirs, and adding all three
required **no change to the scheduler at all** — which is the property the
registry was built to have, and the first time anything has exercised that
claim for more than one descriptor from the same module. A household can
therefore rotate through up to three separately-chosen pictures - a seasonal
notice, a house rule, a QR code - rather than being limited to one.

The three instances share one implementation, not three copies of it:
`Graphic.cpp` factors the fetch/itemCount/draw logic into a single
`template <int N> struct Instance`, explicitly instantiated for `N = 1, 2, 3`
(the one piece that cannot be written generically - each instance's id - is
the only explicitly-specialized member). Each instantiation gets its own set
of static globals from the compiler, which is what gives `graphic`, `graphic2`
and `graphic3` fully independent state - one instance's cached asset going
stale has no effect on the other two - without hand-duplicating the module.
This was the only workable option given `Cards::CardSpec::fetch/itemCount/draw`
are raw function pointers with no per-instance context parameter (see
Cards.h): a single runtime class holding an id data member would have nowhere
to stash `this` for the scheduler to pass back in.

**None of the three has content of its own.** Weather and aircraft each own a
server route, a response shape and a status vocabulary. These own none of
that: what each one draws is whatever asset its own policy entry names.
`CardPolicyEntry.assetId` (optional, string) is carried through
`Cards::PolicyEntry` and `CardManager::applyPolicy()` onto that instance's own
descriptor as `Cards::CardSpec::assetId`, and each instance resolves it
independently through the `Assets` cache described above. That indirection is
the whole point: **changing which picture a household sees is a config edit,
not a firmware release.**
CYD-Dickey's nearest equivalents are its splash and QR cards, which are this
card with the image hardcoded (`splashImage = "/LRBH.PNG"`) and therefore need
a reflash to change.

`assetId` is a fixed 48-character buffer on the descriptor rather than a
`String`, and that is not an arbitrary choice. The registry is
constant-initialised precisely so it exists before any card module's
static initialiser runs; a `String` member would make it dynamically
initialised instead, and initialisation order across translation units is
undefined — the card that registered first would be writing into an array that
had not been constructed yet. An id longer than the buffer is **dropped, not
truncated**: a truncated id is a perfectly well-formed id for some *other*
asset, and showing the wrong picture is worse than showing none.

**Interstitial, not list - all three.** Each instance is one picture, not a
feed, and `interleaveEvery`'s "show after every N other cards" is the honest
description of how a picture should appear — on a cadence of its own, no
matter how many aircraft happen to be overhead. A list card would take one
fixed slot in the list sequence and be seen proportionally less often as that
sequence grows, which is the specific mistake recorded above as having been
corrected on a running CYD-Dickey device. All three share the same `order`
and `interleaveEvery` defaults: they are peers of the same kind of card, not a
priority chain, so there is no meaningful ranking to invent between three
pictures a household picks independently.

**Missing is the ordinary state, and it is silent, per instance.** With no
`assetId` in a given instance's policy entry — which is every instance on
every device until somebody sets one — that instance reports zero items and
the scheduler's existing empty-card skipping passes over it entirely. Same for
an asset that will not fetch and for one that will not decode. This is
deliberately the opposite of what weather and aircraft do, whose "not
activated" and "nothing overhead right now" states report one item because
those messages are real content worth a screen. There is nothing informative
to say about a picture that isn't there, and a card reading "no image
configured" in a household's living room is a worse outcome than a card that
simply never appears. A household that wants only one picture configures only
`graphic`; `graphic2` and `graphic3` then sit silent, exactly as `graphic`
alone used to for a device with no policy at all.

A decode failure is the one case an instance cannot see coming, since it is
only discovered inside `draw()`. It clears that instance's own ready flag, so
the card is out of the rotation by the next computed card and a corrupt asset
costs one dwell rather than reappearing every cycle - and only that one
instance is affected, not the other two. The cached file is deliberately
**not** deleted: a PNG that will not decode will not decode next time either,
and deleting it would turn a permanent failure into an HTTP fetch on every
refresh, forever — loud on the network and no better on screen.

**Fetch and draw stay strictly separate**, like every other card, for each
instance independently. `fetch()` calls `Assets::ensureCached()` (an SD stat
on a hit, one HTTP fetch on a miss) and is called only by the scheduler's
refresh timer; `draw()` calls `Assets::drawCached()`, which never reaches for
the network, so stepping backwards through the rotation is a pure redraw.
Both re-read the configured `assetId` off that instance's own descriptor
rather than caching it, which is what makes a policy change take effect
immediately: the moment the server names a different asset for a given
instance, the one that instance is holding stops counting as content and
stays uncounted until the next refresh has actually fetched the new one.
Without that, a device told to change a picture would keep showing the old
one for up to a full refresh interval.

The theme and the centring come for free — `Display::drawPngFromSd()` already
clears to the day/night background before decoding and centres the image on it
— and the corner clock and action buttons are drawn by `CardManager` after
every card's `draw()` returns, so there is nothing card-specific to do for
either.

### An announcement card: admin-typed text, chosen by the server

`App/Announcement.h`/`.cpp` is the text equivalent of the picture card just
above: the server picks an image for `graphic`, and picks *words* for
`announcement`. It registers a `CardSpec` with id `announcement` the same way
every other card module does, and needed no scheduler change either.

**No fetch at all, unlike every other card in this build.** A picture is an
id that has to be resolved through the `Assets` cache — an SD stat, maybe an
HTTP round trip, maybe a PNG decode. Text is not: `CardPolicyEntry.Text`
arrives already complete, inside the policy itself, on every check-in, so
there is nothing to cache and nothing that can fail to fetch or decode.
`Announcement.cpp` registers no `fetch` function at all (`spec.fetch` is left
`nullptr`), which `CardManager.cpp`'s scheduler already tolerates — it guards
every call site with `card.fetch == nullptr` for exactly this reason. The
"fetch" for this card, such as it is, is `CardManager::applyPolicy()` writing
straight onto the descriptor.

`Text` is carried through `Cards::PolicyEntry::text` and
`CardManager::applyPolicy()` onto `Cards::CardSpec::text` — a fixed
281-character buffer (`Cards::kMaxTextLength + 1`) for the same
constant-initialisation reason `assetId`'s buffer is fixed rather than a
`String` (see the picture-card section above): the registry exists before any
card module's static initialiser runs, and a `String` member would make it
dynamically initialised instead, racing that guarantee. An over-long value is
**dropped, not truncated**, mirroring `assetId`'s own rule: a notice cut off
mid-sentence on a household's screen is a worse outcome than one that simply
does not appear, and in practice this should never fire at all — the server's
policy editor already refuses to save anything past its own limit.

**`Cards::kMaxTextLength` (280) must equal
`DiscoverAroundMe.AdminUI.CardPolicyEditing.MaxTextLength` on the server.**
The two live in separate repositories with no shared build, so
`Announcement.cpp` carries a `static_assert` pinning this firmware's own
constant to the number both sides' comments agree on — it cannot catch the
server's number changing out from under it, but it does catch this side
drifting from what both comments say it must be. Change one number, you must
notice and change the other; that is the same discipline
`Cards::kMaxAssetIdLength`/`Assets::kMaxIdLength` already keep for the picture
card, just without a same-repository `static_assert` to enforce the
cross-repository half of it.

**Interstitial, not list**, for the same reason the picture card is: one
notice is not a feed. **Missing text is the ordinary state, and it is
silent** — with no `text` in the policy, which is every device until an admin
types one, the card reports zero items and is passed over entirely, the same
"no image configured" tolerance the picture card already has for words
instead of a picture.

Drawing is `Display::showAnnouncementCard()`, a new function alongside
`showWeatherCard()`/`showAircraftCard()`/`showSunMoonCard()` in the same
white/bannered card family. It reuses `wrappedLeftText()`'s existing
greedy word-wrap rather than inventing new text-layout code, and picks between
two size tiers the same way `showWeatherCard()` already does for its own
free-text `shortForecast` phrase: 12pt/5 lines if the whole notice fits there,
9pt/7 lines if it does not. Either tier clears the button row that starts at
y=190. A banner reading "NOTICE" in a new muted green
(`kAnnouncementBanner`) keeps it visually distinct from weather's navy,
aircraft's blue and sunrise/sunset's amber.

### A moon-phase card: the first illustrated card, not just text/data

`App/MoonPhase.h`/`.cpp` is structurally the sunrise/sunset card's twin —
same "fetches nothing, pushed in from the check-in path" shape as
`SunMoon.cpp` — but it is the first of a new family the product owner wants:
**graphical style cards**, illustrated rather than text/data. Every card
before this one draws numbers, prose, or a server-chosen picture; this one
draws something computed, as an actual picture.

**No fetch, same as `SunMoon.cpp`, for the same reason.** `moonPhase`,
`moonIlluminatedFraction` and `moonPhaseName` arrive on the check-in response
the device already makes (see the DiscoverAroundMe README's "The moon phase
card"), so `App.ino` pushes them into `MoonPhase::setPhase()` right alongside
`SunMoon::setTimes()` — one more call in the same spot, no new timer, no new
failure mode.

**A simpler absent case than `SunMoon.cpp`'s.** Sunrise and sunset can be
absent for three different reasons and `SunMoon.cpp` still shows a card
explaining which in words. The Moon's phase has only one absent case — a
device whose position has never resolved — and nothing useful to say about
it in words that "no card" doesn't already say, so `MoonPhase.cpp`'s
`cardItemCount()` reports zero items rather than inventing a message, the
same tolerance a picture card with no `assetId` chosen already gets.

**Drawing an actual disc from three primitives, not an image asset.**
`Display::showMoonPhaseCard()` (see that function's own long comment in
`Display.cpp` for the full derivation) uses a well-known technique for
faking a lunar-phase disc with nothing but `fillCircle`/`fillArc`/
`fillEllipse`:

1. Fill the whole disc `muted()` — start dark.
2. Fill the hemisphere currently facing the Sun `ink()`, as a half-disc
   wedge (`fillArc` from radius 0 to the disc's radius, sweeping 180
   degrees) rather than a `fillRect` — a rectangle's bounding-box corners
   poke past the round limb; a wedge's do not.
3. Overlay an ellipse, same centre and vertical radius as the disc,
   horizontal radius `radius * |1 - 2 * illuminatedFraction|`, to grow or
   shrink the lit area away from the exact-half case step 2 drew — filled
   `muted()` below 0.5 illuminated (eating back toward new moon) or `ink()`
   above it (growing toward full), skipped entirely at exactly 0.5 where it
   would have zero width.

**Waxing lights the right, waning lights the left — a stated
simplification, not a guess.** `phase < 0.5` (waxing, growing toward full)
picks the right half in step 2; `phase > 0.5` (waning) picks the left. This
is the Northern Hemisphere convention: a Southern Hemisphere household sees
its own sky mirrored left-right from what this draws. There is no
per-device hemisphere signal today to draw the correct picture for both, and
`Display.cpp`'s own comment says so rather than leaving it to be discovered
on a real device.

`phaseName` is captioned underneath the disc in bold, with an "N%
illuminated" line below that in muted grey — the picture is the point, but
the card is not purely an unlabeled image.

**Registered at `order` 4, interleaving every 7 cards** — grouped with
`sunmoon` and `announcement`, the other once-a-day-fact singletons, between
`sunmoon`'s 6 (sunrise/sunset earns a slightly shorter gap; it is worth
seeing about that often) and `announcement`'s 8 (a household notice, seen
more rarely). A fresh, illustrated card the product owner wants seen
regularly does not belong buried as rarely as a notice, but the Moon's phase
changes more slowly than sunrise/sunset does, so it does not need sunmoon's
own cadence either.

**`Cards::kMaxCards` moved from 8 to 10.** Eight registrations already
existed (`weather`, `aircraft`, `graphic` × 3, `sunmoon`, `announcement`,
`clockdate`) — exactly at the cap — so `moonphase` as a ninth would have hit
`registerCard()`'s silent-log-and-drop overflow path on firmware with no
automated tests to catch it. Sized to 10, one spare slot, rather than
exactly 9, so the next card type is a registration, not also a bump here.

**`kBannerHeight`/`kCardMargin`/font choices are unchanged** — this card
reuses the same white/bannered card family and layout primitives as every
card before it, in a new banner colour (`kMoonPhaseBanner`, deep indigo)
distinct from weather's navy, aircraft's blue, sunrise/sunset's amber and
announcement's green, since it is visually a new kind of card, not a
variation on an existing one.

### Deciding when to reboot to the updater

Three independent things can make the App call `Loader::requestUpdate()` —
which sets the `updreq` flag and reboots into CAL, as described above:

1. **Check-in (`CheckIn.h`/`.cpp`) — the fast path, and the primary
   mechanism.** `performCheckIn()` POSTs to `/api/checkin` every
   `checkInIntervalMs`, sending the device's current UTC timestamp (ISO8601,
   built with `gmtime_r`/`strftime`), its installed firmware version
   (`Identity::installedAppVersion()`), and battery/charging fields. This
   board has no battery — the ELEGOO/CYD is USB-powered — so
   `batteryPercent`/`charging` go over as fixed placeholders (`100`, `true`)
   rather than being omitted: `CheckInRequest` has no way to say "not
   applicable," and a battery-powered sibling board will want the real fields
   this same call already sends. The response's `checkInIntervalSeconds`
   becomes the next `checkInIntervalMs` — a fleet's polling cadence is the
   server's decision, not a constant baked into every device's firmware — and
   defaults to 5 minutes (matching `CheckInGatewayService`'s own
   `DefaultIntervalSeconds`) until the first real response replaces it. When
   the response's `updateAvailable` comes back true, `performCheckIn()` calls
   `Loader::requestUpdate()` directly. This is the path an admin's "Force
   update" button on the server actually reaches, and it's what makes a fresh
   build visible to a device within one check-in interval rather than within
   the hour.

   A check-in that fails for an ordinary reason (no network, a momentary
   server outage) changes nothing; the next attempt is still
   `checkInIntervalMs` away, same as a normal one. A check-in rejected with
   `401` is different and is not treated as ordinary: it means this device's
   own secret no longer authenticates, the case an admin's "Allow
   re-registration" or a secret regeneration produces on a device that is
   still mid-run rather than freshly booted. The App cannot re-enroll itself
   (only CAL can), so `CheckIn::Result::secretRejected` carries this specific
   case back to `performCheckIn()`, which calls
   `Loader::returnToLoaderForReprovisioning()` immediately rather than
   retrying the same dead secret forever. Before this existed, a device
   caught by a server-side re-registration mid-run would sit silently
   failing every `checkInIntervalMs` with no way back - invisible on its own
   screen, since a failed check-in draws nothing - until someone noticed and
   held its BOOT button for 3 seconds by hand.
2. **`AppUpdater::newerVersionAvailable()` — kept, explicitly as a slower
   fallback.** This is the same plain yes/no manifest check it always was,
   still run independently on `Config::kUpdateCheckIntervalMs` (1 hour).
   `App.ino` comments the call site to say so directly: check-in above is the
   fast path that actually reaches an admin's "Force update" button or a
   newly-current build; this hourly timer is belt-and-braces only, kept so an
   update can never be permanently missed if check-in itself were ever
   broken.
3. **A single BOOT-button press**, covered next.

### Two BOOT-button gestures, distinguished only by hold time

The App reads the same pin CAL itself uses (`kBootButtonPin`, GPIO0,
`INPUT_PULLUP`) for two different gestures — deliberately, so a household
never needs to know which binary is running to know how to fix a problem:

- **Hold for 3 seconds — reset stored WiFi.** `wifiResetRequested()` checks
  this once, at boot, before the button is read for anything else, and
  reboots into CAL via `Loader::returnToLoaderForReprovisioning()` if held.
  The App has no captive-portal flow of its own (see WifiJoin, below), so
  this is the only way out of "nothing remembered works."
- **A plain press during normal operation — check for an update now.**
  `forceUpdateCheckRequested()` edge-detects against the previous `loop()`
  iteration's reading (a static `wasPressed`), so it fires exactly once per
  physical press rather than repeatedly while held; a plain press during
  normal operation was otherwise unused and is free to mean this.
  `forceUpdateCheck()` shows "Checking for update," then either reboots into
  the updater with "Updating" or reports "Already up to date" and falls
  through to a normal weather refresh. Saying so explicitly on screen either
  way matters: a press that silently does nothing reads as a dead button, and
  without an explicit answer, "nothing happened" and "already checked,
  nothing new" look identical.

### Bug fix: the WiFi-reset hold only worked at boot, not when actually stuck

Found live on a physical device. `wifiResetRequested()`'s 3-second BOOT hold
was, before this fix, checked exactly once — in `setup()`, before
`ensureWifiConnected()` is ever called. A device that passed that one check
(WiFi looked fine, or the hold window was simply missed) and only lost its
ability to join WiFi *afterwards* — bad credentials, a router swap, whatever
— fell into `ensureWifiConnected()`'s retry loop, which showed "Could not
join WiFi… Hold BOOT for 3 seconds to set up WiFi again" and then called a
blind `delay(30000)` between attempts. That `delay()` never read the button
at all. A household holding BOOT for 3 seconds *while already stuck on that
exact screen* — precisely what it told them to do — did nothing, because the
gesture's only working instant had already passed back in `setup()`. The
only actual way out was a precisely-timed power-cycle-then-immediately-hold,
which the on-screen text never described and which is not something a
household member would discover on their own.

The fix keeps the same ~30-second pace between join attempts but polls for
the gesture throughout the wait instead of blocking blind: every 100ms it
checks the BOOT pin, and a press hands off to `wifiResetRequested()` itself
— reused as-is rather than reimplemented, so there is exactly one definition
of "was the 3-second hold actually completed" for both call sites to share,
not two subtly different ones drifting apart over time. A confirmed hold
calls `Loader::returnToLoaderForReprovisioning()` immediately, from inside
the retry loop, rather than waiting for the next `joinStoredNetwork()`
attempt to fail first. A press released early (before the 3 seconds
complete) leaves `wifiResetRequested()`'s own "Keep holding…" prompt on
screen, which `ensureWifiConnected()` explicitly redraws back to "Could not
join WiFi" afterward — so the screen never keeps telling someone to
"release now to cancel" a hold that already ended.

### A drawn degree symbol, because the font has none

LovyanGFX's built-in font used here is ASCII-only — no Unicode glyphs, no
extended-ASCII either — so a degree sign has nothing to look up, whether it's
attempted as a UTF-8 sequence or a raw `0xB0` byte. A real device showed
exactly the failure mode this predicts: a missing-glyph box where the degree
mark should be. `Display.cpp`'s `drawTemperature()` (formerly
`centeredTemperature`) works around this by composing the temperature display
by hand rather than as one string: `lcd.drawString` for the number and the
unit, and a small ring from `lcd.drawCircle()` — positioned near the
cap-height, like a real superscript degree mark — standing in for the
character the font can't render. `showWeatherCard()` calls this instead of
building `String(temperature) + "°" + unit`; putting a literal degree
character back into that string reintroduces the missing-glyph box.

The ring's radius is a **parameter**, not a constant, because it has to track
whatever font the caller selected: the fixed 5px ring that read as a degree
mark beside a 12pt numeral reads as a stray speck beside the 24pt one the
weather card now uses. At radii of 7 and above it draws two concentric
circles, so the ring carries a stroke weight in the same family as the bold
digits standing next to it — a one-pixel ring beside a 24pt bold numeral reads
as a rendering artefact, which is the one thing a hand-drawn glyph substitute
must never look like. The doubling is skipped at small radii, where a 2px
stroke would close the ring into a dot.

### WifiJoin, not Network

`App/WifiJoin.h`/`.cpp` is named that on purpose, not `Network.h`. A
sketch-local `Network.h` shadows the ESP32 Arduino core's own system header
of the same name, which `WiFi.h` depends on internally, and takes the build
down with missing-type errors (`network_event_handle_t`, `NetworkInterface`,
and similar) that have nothing to do with the file's own contents and nothing
obviously pointing at the real cause. Don't rename this file back, and don't
add a new sketch-local `Network.h` anywhere else in this project.

### Watching a device live: `Log.h`/`.cpp` and remote debug streaming

Forcing an update onto a device and then wanting to *watch it land* — without
finding the unit and plugging in a USB cable — is what `App/Log.h`/`.cpp`
exists for. Every module in `App/` (`App.ino`, `CheckIn.cpp`, `Weather.cpp`,
`Aircraft.cpp`, `WifiJoin.cpp`, `AppUpdater.cpp`, `AppService.cpp`,
`Loader.cpp`) calls `Log::line()`/`Log::printf()` instead of `Serial` directly,
so the same narrative — WiFi joining, check-in results, card refreshes, update
decisions — is available two ways at once rather than two logging paths
silently drifting apart. This is scoped to `App/` only; CAL's own root-level
logging (`CAL.ino` and its modules) is untouched by this pass; a
bootloader-side counterpart, if ever wanted, is a separate piece of work.

**Serial first, always.** Every `Log::line()`/`Log::printf()` call writes to
USB `Serial` unconditionally, before anything else happens. If the remote
stream is ever broken, disabled, or the server unreachable, someone with a
cable in hand still sees exactly what they would have seen before this module
existed — that is the fallback of last resort this was built not to regress,
not a nice-to-have.

**Discovery is check-in-driven, and deliberately not one-shot.** `/api/checkin`'s
response now carries `debugStreamRequested` alongside `acknowledged` /
`updateAvailable` / `checkInIntervalSeconds`, parsed into a new
`CheckIn::Result::debugStreamRequested` field the same way as the others.
Unlike the forced-update flag, `performCheckIn()` calls
`Log::setStreamingEnabled(result.debugStreamRequested)` on *every* successful
check-in, not just once — so streaming tracks an admin's toggle live, turning
on or off within one `checkInIntervalMs`, and there is no flag of its own
persisted to NVS. After a reboot, streaming is simply off until the next
check-in tells it otherwise (up to one interval away) — recovering the right
state automatically rather than needing to survive the reboot itself.

**Transport reuses the proven pattern, on a timer.** While streaming is on,
`Log::line()` also appends the same formatted line to an in-RAM buffer;
`Log::poll()`, called once per `loop()` iteration, batches it into a
`POST /api/debuglog` body (`{"lines": ["...", "..."]}`) using the identical
`NetworkClientSecure` / `HTTPClient` / `Tls::configure()` /
`X-Device-Secret` pattern `CheckIn.cpp` already established — no new HTTP
client, no WebSocket. `Log::flushNow()` forces an immediate, synchronous send
bypassing the timer; `Loader.cpp`'s `bootFactoryAndRestart()` — the single
choke point every reboot path (`requestUpdate()`,
`returnToLoaderForReprovisioning()`) already goes through — calls it right
before `esp_restart()`, so the line explaining *why* the device is about to go
dark actually reaches the server instead of being lost with the rest of RAM.

**The concrete numbers, and why:**

| Constant | Value | Reasoning |
|---|---|---|
| `kBatchIntervalMs` | 1000 ms | `loop()` already runs on roughly a 1-second cadence of its own (its closing `delay(1000)`), with no hardware timer or second task driving anything faster. A shorter timer would only be aspirational — `poll()` cannot be called any more often than `loop()` actually calls it. |
| `kMaxBatchLines` / `kMaxBatchBytes` | 40 lines / 4 KB | Caps a single `POST` body so one flush can't spike request latency or hold up `loop()` for longer than necessary; whatever doesn't fit waits for the next `poll()`. |
| `kMaxBufferedLines` / `kMaxBufferedBytes` | 200 lines / 16 KB | The buffer's actual memory ceiling, independent of and larger than the per-batch cap — this is what protects against unbounded growth if the network is down for a while. Generous for several minutes of this firmware's real log volume (roughly one line every few seconds, brief bursts during WiFi join/check-in/update) while staying a small, fixed slice of the ESP32's ~320 KB SRAM, and only ever paid while an admin has actually turned streaming on. |

Once either buffer cap is hit, the oldest line is dropped to make room for the
newest — oldest-first, never growing unbounded and never crashing — and the
next successful flush is prefixed with a `[N lines dropped]` marker line
summarizing exactly how many were lost. A failed `POST` leaves the buffer
untouched for a retry on the next `poll()`; only what the server actually
accepted (`HTTP 200`) is removed.

**Formatting.** `Log::printf()` renders into a fixed 256-byte stack scratch
buffer — deterministic, and consistent with this module running from
stack-tight retry loops throughout `App/` — rather than growing a heap buffer
on every call; output that would overflow it is kept and marked
`...(truncated)` rather than silently cut off mid-word.

**Log on change, not on every pass.** The 200-line buffer above is the reason
this is a rule rather than a preference: anything logged unconditionally from
a timer or a draw path emits the same line forever and pushes out exactly the
context someone opened the stream to find. `Graphic.cpp`'s `noteState()` and
`SunMoon.cpp`'s `gLastLoggedSunrise`/`gLastLoggedSunset` established the
pattern — say something the first time a condition becomes true and the first
time it stops, and nothing in between.

`Weather.cpp` now follows it throughout, and the cases it covers are worth
listing because every one of them used to be a **silent** degradation on a
device in someone's kitchen, with no on-screen tell and nothing in the stream:

- **A missing `temperature` field** defaulted to `0` and drew as a real
  reading of zero degrees — indistinguishable from a genuine freezing morning.
  It still draws that way (inventing a substitute reading would be worse), but
  it now says so, in those words.
- **A missing `shortForecast`** drew as an empty gap; **an unresolvable
  city/state/postal code** drew as no location line at all. Both are now
  reported when they start and when they recover.
- **Which address the reading is for.** `preferredForecast()` picks the
  owner's Target over their Home when one is set, and both render as an
  ordinary city name — so a device showing the "wrong" city has no visible
  tell whatsoever. The log line is the only place that distinction is ever
  recoverable from a deployed device.
- **A failed refresh discards a good forecast.** `cardFetch()` replaces the
  reading that was on the card with an error message, so one network blip
  turns a working weather card into "Could not load weather" until the next
  refresh ten minutes later. That is deliberate existing behaviour, not a bug
  fixed here — but it now logs the *age of what it threw away*, which is the
  kind of thing that is impossible to reconstruct after the fact.
- **What the card actually drew**, gated so it fires only when the content
  changed. Deliberately *excluding* the "Updated N min ago" string from the
  change comparison even though it is on screen: freshness ticks over on its
  own every minute, so folding it in would make every summary differ from the
  last and turn a change-gated line straight back into a per-draw one.

### CAL-side (bootloader) logging is explicitly out of scope here

This pass touches `App/` only. CAL's own root-level modules (`CAL.ino`,
`Provisioning.cpp`, `Updater.cpp`, and the rest) keep their existing `Serial`
calls untouched, and there is no `debugStreamRequested` handling, streaming
buffer, or `/api/debuglog` POST anywhere in CAL's own code. This is a
deliberate scoping decision, not an oversight: the motivating use case —
watching an *App* OTA update land and run — lives entirely in `App/`, and CAL
itself already has no equivalent "watch it happen" need of its own (it either
gets a device onto the network and hands over, or fails loudly on-screen at
the point of failure). A bootloader-side counterpart, should CAL ever want
one, is a separate piece of work — not a natural extension of this one, since
CAL's constraints (never updated over the air, must stay minimal) argue
against giving it any new remote-facing surface lightly.

### Telemetry: a heartbeat riding check-in's own clock

`App/Telemetry.h`/`.cpp` reports device health — uptime, WiFi RSSI, free
heap, CAL's own boot-attempt counter, and this device's own last check-in
outcome — to the server's `POST /api/telemetry` (see the DiscoverAroundMe
repo's README, "Telemetry: device health/diagnostics," for the canonical
wire contract this firmware implements; this section only covers the
device's own side of it). `Telemetry::report()` is called once per
successful check-in, from `performCheckIn()` in `App.ino`, right before the
`updateAvailable` reboot branch — a device about to reboot for an update
still leaves a fresh snapshot behind first. Deliberately has no timer of
its own: the server's own `/diag/telemetry` page flags a report stale past
three times the check-in gateway's default interval, a threshold that only
stays meaningful if a healthy device's telemetry refreshes close to every
check-in rather than on some slower, independent schedule that would trip
that alarm on its own. Best-effort and fire-and-forget — a failed report is
logged and dropped, not retried before the next check-in comes around; this
is diagnostics, not a control channel, and nothing downstream depends on it
succeeding.

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

- **WebSerial requires a secure context** (HTTPS, or `localhost`). This was
  blocked on the server's own TLS work, which has since gone live
  (`api.discoveraroundme.com` serves a valid Let's Encrypt certificate) - so
  this specific blocker is resolved. It is still a hard browser rule, not a
  configuration, for whatever flasher runs where.
- **The CH340C USB-serial driver is the most likely first-contact failure.** On
  Windows the board frequently enumerates as an unknown device until the driver
  is installed, and the symptom a non-technical operator sees is simply that no
  port appears in the browser's picker. Whatever documentation ships with the
  flasher needs to lead with this rather than mention it at the end.

**What exists today is narrower than what this section describes.** The server
now has a public, unauthenticated `/cal` page for the simplest case - a fresh
unit that only needs generic CAL firmware, no server-issued secret involved,
CH340 driver links leading the page as called for above, and a link out to
Espressif's own `esptool-js` for the actual WebSerial flash. It does not sign
anyone in, mint a device record, or generate an NVS secret image - the
operator-authenticated, secret-injecting flasher this section actually
describes for bulk provisioning is still designed, not built.

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
ci/build-firmware.sh
```

This is the entire build procedure - install the pinned toolchain, compile,
checksum - kept in one script rather than only in CI config so it runs
identically by hand or inside any CI provider. See **Shipping** below for how
a push to `main` turns this into a published release with no further action.

Toolchain, pinned in that script and matching what this has been measured
against:

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

**The custom `partitions.csv` at the repo root is what actually gets built.**
Naming a `PartitionScheme` on the `arduino-cli compile` command line (the
build script still passes `min_spiffs`, for no effect) does not matter: the
ESP32 Arduino core detects a `partitions.csv` sitting in the sketch root and
uses it in preference to any named scheme, silently. This was verified, not
assumed - by decoding the actual compiled `CAL.ino.partitions.bin` output
byte-for-byte (the `0xAA50`-magic partition-table binary format) rather than
trusting `arduino-cli`'s own build summary, which reports "% of X bytes"
against the *named* scheme's static memory map regardless of which table was
actually used, and will happily report against the wrong one.

## Shipping

Push to `main` and CI does the rest - no manual build, no manual release, no
version number to choose. See `VERSIONING.md` for the exact scheme
(`vYYYY.MM.DD.NNNN`, entirely date- and counter-derived) and
`.github/workflows/build-and-release.yml` for the pipeline, which is a thin
wrapper around `ci/build-firmware.sh` plus the two steps that are genuinely
GitHub-specific (finding the next same-day release number, publishing to a
GitHub Release) - so moving to a different CI provider later means replacing
a few lines of YAML, not re-deriving the build.

Every push publishes to the rolling **`latest`** release (same tag, same
download URLs, every time - this is what a device-setup page should link to)
and to a new, **permanent** `vYYYY.MM.DD.NNNN` release that is never reused.

**That second half silently did not happen at least once.** The two publish
steps run in sequence, and a failed step aborts the rest of the job by
default. `latest` replaces every asset on a tag that never changes name, and
that replace-in-place dance occasionally races against GitHub's own API - a
"delete this asset" call returning 404 for one already gone, observed live -
which failed the `latest` step and silently skipped the permanent, dated
release after it. The push still showed green-adjacent in the sense that
every binary really did build and upload to `latest` correctly; what never
happened was the numbered release nothing else can recreate after the fact,
and the server's own "Load from GitHub" picker only lists numbered releases,
not `latest` - so the fix for that exact push was reachable only by loading
`latest` by name, not by picking the newest number in the list. The `latest`
publish step now has `continue-on-error: true`, so a cosmetic race on the
disposable tag can no longer cost the permanent one.

## Open questions

- **`Provisioning::run()`'s abandonment timeout is untested on hardware.** The
  code now gives up and returns `false` after 15 minutes with nobody
  submitting credentials (`Config::kProvisioningAbandonTimeoutMs`), matching
  its documented contract - but "nothing has run on hardware" below applies
  to this exactly as much as everything else.
- **No automated tests, at all.** There is no test harness in this repository
  and no obvious one to reach for: the code is inseparable from ESP32
  peripherals, NVS, WiFi and TLS, none of which have a usable stub here. The
  testing story for this firmware is genuinely unsolved. This is a real gap,
  not a deferred chore — the server side treats a test project as shipping
  alongside the logic it covers, and nothing equivalent exists here.
- **Nothing has run on hardware.** Every behaviour described in this document
  is designed and written and, as of the build-to-ship pipeline above,
  mechanically reproducible - none of it is proven on a device.
- **Touch input (`App/Touch.h`/`.cpp`) is compiled but unverified.** The
  panel/screen on this exact ELEGOO board was separately confirmed working
  earlier, but that confirmation never exercised the XPT2046 touch
  controller, and nobody has put a finger on this glass yet. This is the one
  piece of firmware in this repository with literally zero possible
  automated coverage even in principle - a clean compile proves the code
  builds against LovyanGFX's touch API, nothing more. See "Touch input: the
  XPT2046 controller, used for the first time" above. Now that taps are
  classified into zones rather than meaning one single thing, a miscalibrated
  or inverted axis would send a "forward" tap backwards instead of going
  unnoticed - the unverified assumption became load-bearing.
- **The SD card slot does have a card in it, confirmed from a real device's
  own telemetry** (`sdTotalMB=7450`, a real capacity, not the zero a missing
  card reports) — narrower than previously written here. What is still
  unconfirmed: whether a PNG actually decodes off it, since no asset fetch
  has yet reached that stage (see "Partially run now, on real hardware"
  above) - the card mounts and reports real numbers, but nothing has been
  written to it or read back as an image yet.
- **The asset fetch endpoint does not exist yet.** `App/Assets.cpp` expects
  `GET /api/assets/{id}` with the usual `X-Device-Secret`, matching the shape
  of every other device route, but the server's `Assets` domain is still a
  stub. Nothing has ever answered that request, and no registered card
  consumes an asset yet - the only caller is the boot splash.
- **Card buttons and the pending-action queue are compiled, not exercised.**
  Nothing has pressed a button, written a press to NVS, carried one on a
  check-in, or watched `acceptedActionIds` clear one. The server-side
  `DeviceSimulator` (`/diag/devicesimulator`) is the only repeatable way to
  exercise the wire half of this without hardware; the firmware half has no
  equivalent and cannot get one.
- **The scheduler has no test, and the nearest substitute lives on the
  server.** `App/CardManager.cpp`'s interleaving, history ring and dwell
  logic are verified by a clean compile and by reading them against the
  CYD-Dickey code they were ported from. The closest thing to a test for the
  intended *behaviour* is the `DeviceSimulator`'s rendered preview of a
  policy's resulting rotation, which at least lets a policy change be
  sanity-checked before a device sees it.
- **The operator-authenticated, secret-injecting flasher is still just
  designed.** See *Provisioning: how a device gets its secret*, above - what
  exists today (the server's public `/cal` page) covers a fresh unit that
  only needs generic firmware, not bulk provisioning with per-device secrets.
