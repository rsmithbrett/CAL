# Boot button for WiFi, and impressions with a human present

Two features, requested 2026-10-02. They are independent and ship separately.

---

# 1. Hold BOOT at any point in startup to join a new WiFi

## What happens today

`CAL.ino`'s `bootHoldRequested()` samples GPIO 0 **once**, at power-on:

    pinMode(kBootButtonPin, INPUT_PULLUP);
    if (digitalRead(kBootButtonPin) != LOW) {
      return BootHoldResult::None;      // never looks again
    }

So the button only works if it is already down as the unit powers up. Press it a second
later and nothing is watching.

What a household has to do today when the WiFi has changed:

1. Power on.
2. Watch it try every remembered network — `Config::kWifiJoinAttempts` attempts each,
   `kWifiJoinTimeoutMs` per attempt, over up to three networks, then a second blind pass
   over all three.
3. Wait for it to give up and reboot.
4. Catch the next power-on with the button already held.

## What it should do

Holding BOOT for 3 seconds at **any** point between power-on and the App taking over
abandons the join ladder and opens the WiFi portal.

## Where the check goes

The ladder spends nearly all of its time in one place — the poll loop inside
`Provisioning.cpp`'s `attemptJoin()`:

    const uint32_t deadline = millis() + Config::kWifiJoinTimeoutMs;
    while (millis() < deadline) {
      if (WiFi.status() == WL_CONNECTED) { return true; }
      delay(250);                          // <- the whole wait happens here
    }

A hold detector polled from that loop covers every attempt on every network, which is
the entire window the household is staring at.

**Precedent, in this codebase:** `App/App.ino:567` already polls the same pin inside its
own startup loop, and `App.ino:541` records why — a household holding the button during
a long retry was being ignored. CAL has the same defect and gets the same shape.

## Shape

A small detector owned by CAL, polled rather than interrupt-driven, so it cannot fire
during flash writes:

- `BootButton::begin()` — sets the pin mode once.
- `BootButton::heldFor(uint32_t ms)` — true once the pin has been continuously LOW for
  that long. Resets its own timer on release, so a stray tap never accumulates.

`bootHoldRequested()` keeps the power-on gesture exactly as it is, including the
10-second second tier for identity erase. The new call site is `attemptJoin()`'s wait
loop, which returns a distinct result so `joinStoredNetwork()` stops laddering and the
caller opens the portal.

## Boundaries

- **The 10-second identity erase stays power-on only.** Erasing identity is the
  destructive tier; requiring a deliberate power-on gesture is the guard, and a reader
  holding the button to fix WiFi must not cross into it by holding too long.
- **Once the App is running, the button is the App's.** CAL's detector is torn down at
  handover. The App's existing `forceUpdateCheckRequested()` keeps the pin.
- **The screen says so.** The same "Keep holding BOOT to set up WiFi / Release now to
  cancel" prompt, drawn when the hold starts, cleared on release — `gPanelUsedForGesture`
  already exists for exactly this and is honored.

## How it is proven

- A unit with a wrong passphrase stored: power on, wait for the second attempt, hold 3
  seconds, portal appears.
- A stray tap during the ladder: the join continues.
- Hold 11 seconds during the ladder: WiFi portal, identity intact.
- Hold 11 seconds at power-on: identity erased, as today.

---

# 2. Impressions recorded while a human is present

## What is wanted

Count how many times each card was shown **while the motion sensor confirmed somebody
was there**, how long each showing lasted, and aggregate it by day, week, month and
year, so a marketing reader can be told what their screen actually earned.

## Two numbers for every card, always

**Every showing is recorded on every device.** How many times each card was shown, and
for how long. A unit with no motion sensor still reports all of it.

**Presence is a second measure layered on the first**, on units that have the sensor:
of that screen time, how much had somebody in front of it.

So a card carries two durations:

| Measure | Recorded on | Means |
|---|---|---|
| Shown | Every device | The card was on the glass for this long |
| Shown with somebody there | Devices with a motion sensor | Of that time, this much had presence |

A marketing reader gets reach from the first and attention from the second, and the two
never have to be reconciled because one is a subset of the other by construction.

**Presence is absent, not zero, where there is no sensor.** A zero would be a claim that
nobody was ever there. The screen time on that unit is still a real number and still
counts; only the presence column is unknown. `/device` already reports whether a unit
has a motion sensor, so the server can tell "none present" from "cannot tell" without
asking the device anything new.

## It has to be WHICH card, not which kind of card

"QR cards were shown 340 times" tells a marketing reader nothing. "Your open-house QR
was shown 340 times" is the number they are paying for. So the recording key is the
card **and what was on it**.

Two kinds of content, and they behave differently:

**Authored content** changes when the agent changes it — a QR link, a picture, an
announcement. The slot id (`qrtext3`, `graphic2`) is stable, but the thing in it is not.
Counting by slot alone silently credits a new link with the old one's impressions.

**Live content** changes on its own — the listings card walks a feed. `Listings.cpp`'s
`cardItemCount()` and the `PerItem` basis in `KnownCards.h` mean firmware already gives
each listing its own dwell, so the device knows which property is on the glass. **If the
house listing changes, that matters**: 123 Oak Street's impressions belong to 123 Oak
Street, and they stay there after the listing sells and leaves the feed.

### The content key, per card family

| Card | What identifies the content |
|---|---|
| Real-estate listings | The listing itself, per item, for its own dwell |
| QR code + text | The link |
| Graphic / picture | The asset id already on the wire as `AssetId` |
| Announcement | The text |
| Sports: my team, scoreboard | The team or league |
| Weather, tides, clock, sun, moon, aircraft, ISS, home value, calendar | The card is the content; no second key |

Authored content is keyed by a short hash the server can resolve back to the exact
string or asset it had at the time, so a report can name it — and so changing a QR link
starts a new row rather than inheriting the old one's history.

## What the device records

Per showing:

| Field | Why |
|---|---|
| Card id | The slot. Known-card ids already on the wire. |
| Content key | Which listing, which link, which picture. Absent where the card is its own content. |
| Duration shown | The baseline measure, on every device. |
| Duration with presence | Omitted entirely on a unit with no sensor. |

Rolled up on the device into a small fixed buffer, keyed by card **and content**, and
uploaded at check-in. The device holds totals, not a log of showings: a per-showing log
grows without bound between check-ins and has no reader.

**The buffer is bounded and says when it overflowed.** A listings feed can walk more
properties between check-ins than a fixed buffer holds. When it fills, the device keeps
counting into an "other" bucket and reports that it did, so a report can say "and 40
further listings" rather than quietly dropping them.

## The wire, and the rule that constrains it

**The 6-month firmware compatibility gate CLOSED on 2026-09-22.** Clients have devices
in the field, `IsProduction` is true, and the only permitted change is **additive
optional fields**. So:

- A new optional block on the existing check-in payload. Absent from older firmware,
  and absent is distinct from empty.
- No existing field changes meaning, type or cardinality.
- `CheckInResponse` and the device simulator move together — a new response field needs
  three simulator edits in the same pass or WebTests fails.

## The server side

- Store per device, per card, per hour. Hourly is the smallest bucket worth keeping and
  rolls up cleanly into day, week, month and year without storing four tables.
- Aggregate by display, by account, and by brand, since a marketing reader asks "what
  did my fleet earn", not "what did display 12 earn".
- **Screen time totals cover every display.** Every unit reports it, so the fleet number
  is the whole fleet.
- **Presence totals cover the displays that can measure it**, and the report says how
  many displays those were. "4,200 minutes of screen time, 1,310 of it with somebody
  present, measured across 7 of your 9 displays" is a sentence a marketing reader can
  act on. A single blended figure over a fleet where two units cannot measure is the
  thing to avoid.

## What it is NOT storing

No identity, no face, no count of distinct people. The sensor reports presence, and
presence is all that is recorded. Worth stating because "impressions, with a human
confirmed" invites the assumption that the device recognizes people. It does not.

## Scope, honestly

Feature 1 is one firmware file plus a header, and it is testable on a bench unit
tonight.

Feature 2 spans the firmware, the check-in contract, the database, the aggregation and
a marketing-facing report. It is not a one-sitting change, and the compatibility gate
means the wire half has to be right first time.
