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

## Four measures per card

| Measure | Recorded on | Means |
|---|---|---|
| **Shown** | Every device | The card was on the glass this long |
| **Seen** | Every device | A human was confirmed there while it was up |
| **Sought** | Every device | Somebody stopped on it, having navigated to it |
| **Acted on** | Every device | The card's action was pressed while it was up |

### Seen has two sources, and either one is proof

A human is confirmed present when **the motion sensor says so**, or when **somebody is
working the controls**. Both are evidence; neither depends on the other.

The second matters more than it first appears. **A card crossed during a rewind was seen
by a person, whether or not they stopped on it.** Somebody pressing back three times is
in front of the device for all three cards, looking at each one as it passes. Counting
only the card they landed on throws away two confirmed views and understates every card
that sits between two popular ones.

So any card drawn inside an active navigation window is Seen, destination or not. The
window opens on the first forward or back press and closes after the auto-advance timer
resumes — `CardManager.cpp` already suppresses that timer after a deliberate touch, so
the window it needs exists today and is the thing that defines "still navigating".

This also means **a unit with no motion sensor still reports Seen**, whenever somebody
touches the controls. The sensor widens the measure; it does not own it.

### Sought is narrower, deliberately

Sought is the card somebody navigated to **and stayed on** — the auto-advance timer
resumed while it was still up, or they pressed the action. A card flicked past is Seen
and not Sought.

Keeping these apart is the whole point: Seen answers "did a person look at this", Sought
answers "did a person want this". Collapsing them would let a card that is merely in the
way of a popular one look as wanted as its neighbour.

`advanceCard()` and `rewindCard()` are the manual path and are already distinct from
rotation, so both counters hang off a distinction the firmware makes today.

**Acted on** is the button on a card — the press that already reaches the press log.
Recording it here as a card attribute puts reach, attention, intent and response on one
row, so a marketing reader can see that a card was shown 400 times, seen 130, sought 22
and acted on 9, and know what each number means.

## Seen is a subset of Shown, always

Every showing is recorded on every device: how many times, and for how long. Seen is
carved out of that same time, so the two never need reconciling — one contains the other
by construction.

**Where a unit has no motion sensor, its Seen figure counts navigation only.** That is a
real number and it belongs in the report, but it is a floor rather than a measure: the
unit reports what it can prove, and people who stood and watched without touching
anything are not in it.

So the report distinguishes three things, and must not blend them:

| | |
|---|---|
| Seen, sensor unit | Presence or navigation. The fuller measure. |
| Seen, no sensor | Navigation only. A floor. |
| Sensor absent | Said out loud, so a low figure is read as a thinner measurement rather than a quiet screen. |

`/device` already reports whether a unit has a motion sensor, so the server can tell
these apart without asking the device anything new.

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
| Household photographs | The asset id of the photograph that drew |
| Announcement | The text, as a short hash of it |
| Home value | The address the estimate was computed for |
| Sports: my team, scoreboard, live game | The fixture, as its UTC start date with the two sides, or the two sides alone when the server sent no start time |
| Aircraft overhead | The aeroplane's registration, which belongs to the airframe for its life, falling back to the callsign where no registration arrived |
| Weather, tides, clock, sun, moon, ISS, calendar | The card is the content; no second key |

Authored content is keyed by a short hash, so changing a QR link or rewriting a
notice starts a new row rather than inheriting the old one's history. The intent
here was a hash the server could resolve back to the exact string it had at the
time; nothing stores that mapping, and nothing below builds one. What names the
content for a reader is the summary riding the same draw, which both the QR and the
announcement cards send.

**Home value and the five announcement slots were in the bottom row until
2026-11-03**, reporting a summary on every draw and no key, so every address a
display cycled through and every notice a tenant wrote collapsed into one period
row per quarter hour. The owner's call that day is that both owe a key, and the
two rows above are what they report now.

A home-value draw is about one property and the check-in response carries the
address it was valued for — `homeValueAddress`, which `HomeValueResult.SubjectAddress`
fills. That string is the key. There is no subject id on the wire to prefer over it.
Two quarter-hours of the same address group together on it, which is the whole gain;
the figure keeps riding the summary, so an estimate that moves does not start a new
row for the same house.

An announcement holds its text and nothing else. The policy entry carries `Id`,
`Kind`, `Order`, `DwellSeconds`, `Text` and the asset, QR and location fields, and
none of them identifies a notice, so the text is the only thing that can. The text
runs to 280 characters and a content key is bounded at 64 on both sides of the wire,
so the key is a 64-bit FNV-1a digest of the exact text, written as sixteen lowercase
hex characters. Six distinct announcement texts have drawn in production, 74 to 77
characters each, read on 2026-11-03 at 11:30 UTC, so sending the first 64
characters would have been cut on every draw rather than exceptionally. That is
the measured half. Collapsing two notices sharing a prefix into one row is what a
prefix key allows, not something these six do: they diverge by character 14, and a
64-character prefix tells all six apart. The words themselves keep riding the
summary, which is what a reader sees.

**Both resolvers answer only for a draw that put the content up.** The announcement
one returns nothing when the instance's text is empty, which is the one condition
its `draw()` tests before drawing the no-content state. The home-value one returns
nothing until the card holds both a real estimate and an address, which is stricter
than its own draw: a valuation resolved from a position fix puts a figure up with no
headline and reports no key, so for that card a keyed row is a row that drew and the
reverse does not follow. That is `PersonalPhotos.cpp`'s rule applied as far as each
card's state allows.

The address key carries the 64-character bound rather than dodging it. A content key
is cut at 64 by `Impressions::beginShowing` and again by the server's rollup, the
subject address allows 300, and two properties agreeing for 64 characters would
group as one row. The longest address in the valuation cache on 2026-11-03 is 55
characters and has no estimate, so it never draws a key; the longest with one is 42.
A readable address is worth more than collision-proofing a case no household has
produced, and a digest is the fix if one ever does.

**The live sports card reported no key until 2026-11-03.** `App/Sports.cpp`
registers five cards. Four go through `registerOne()`, which assigns the
resolver, the describe hook and the frame count from the shared
`listingIdFor()`, `describeFor()` and `frameCountFor()` switches; the other one,
`sportslive`, is built by hand so that it can carry its own item count and its
notable dwell, and that descriptor assigned none of the three. So the team cards
and the scoreboard reported a key on every draw and the live card out of the same
file reported nothing on 907 of them, with 161 period rows holding nothing,
counted on 2026-11-03. It now assigns all three.

The `default` arms of those switches used to answer for index 3, which is what
made the hand-built descriptor look as though it had inherited them.
`sportsscores2` took index 4 on 2026-11-08, so index 3 has a case of its own and
index 4 is what the default arm hands out. `Sports.cpp` carries a `static_assert`
on the registration count beside those switches, so a sixth id cannot fold onto
the last arm and draw index 4's games under its own id, with every id, count and
frame figure reading as correct. That is the shape the live card was in for 907
draws.

What makes the key mean something was on the glass is the card's own item count.
`countLive()` returns zero unless `anyLive(3)` finds a game in progress, so the
card drops out of the rotation rather than drawing a card that says nothing is
on, and `listingIdCardAt()` returns nothing for an index past the stored games or
for a game with neither side named. That is the same rule `PersonalPhotos.cpp`
applies, reached through the item count instead of a per-item drawability test.

### The decision is an argument

Every card module hands its descriptor to `Cards::registerCard()`, and since
2026-11-03 that call takes a second argument naming what the card's draws
report: `Cards::registerCard(spec, Cards::KeyPolicy::Resolves)`.

Three values, one per family in the table above. `Resolves` says the module
assigns `CardSpec::listingId` and its own answer names what drew.
`PolicyContent` says the key arrives on the card policy, so `noteShowing()`'s
`assetId` and `qrData` fallbacks answer for it and a resolver would repeat what
is already in front of it. `Keyless` says there is nothing to name, which is the
bottom row.

The argument has no default, so a registration that states nothing does not
compile. That holds whatever the descriptor is called, however it was declared,
and wherever its file sits, because the registry is the one function every card
goes through to become a card.

What the argument states and what the descriptor holds are two separate claims,
and only the first one the compiler can see. `registerCard()` checks the second
when the registration runs: `Resolves` with a null `listingId` is refused, and
so is `PolicyContent` or `Keyless` with one assigned. Refused means the card is
not stored and a log line names it, so it stays out of the rotation rather than
drawing and reporting nothing. Registration happens at static-init over a
descriptor filled in field by field, so there is no constant expression for a
`static_assert` to read; the rule lives in `Cards::keyPolicyHolds()` in
`Cards.h`, and `ci/test-key-policy.sh` drives it on a host through every
combination of the three policies against a resolver present and absent.

`ci/check-content-key-resolvers.sh` carried the per-card decision as three lists
of filenames until this change, and a text scan over C++ is what let
`sportslive` hide between a keyed neighbour and a hand-built descriptor. The
lists are retired. What is left in that script is the part no compiler can
reach: the fallback order `noteShowing()` applies, that the resolved key reaches
`Impressions::beginShowing()`, that `CheckIn.cpp` sends it under the name the
rollup reads, and that `registerCard()` still calls `keyPolicyHolds()` on the
way in. Those stay text checks because `CardManager.cpp` names a graphics
library and does not compile on a host.

### Which card owes which policy

The argument makes every registration state a policy. It does not say whether
the policy stated is the right one for that card. `Keyless` on a card that walks
a feed is self-consistent: it states there is nothing to name, it assigns no
resolver, `keyPolicyHolds()` agrees with it, and every gate passes. That is the
shape `sportslive` was for 907 draws. No compiler can read it, because which
cards owe a key is a judgement about what a card puts on the glass rather than a
property of the code that puts it there.

So the judgement is written down per card id, in
`ci/check-card-key-decisions.sh`, as a census of all 36 ids this firmware
registers against the policy each one owes. The table higher up is the
reasoning. The census is the same decision in a form a runner can compare
against `App/`, and it is keyed on the card rather than on the file, which is
where the retired lists went wrong.

The gate reads `App/` rather than trusting the census, and compares the two both
ways:

- Every card id `App/` defines has to appear in the census. A new card is a new
  `kCardId` constant or a new entry in `Sports.cpp`'s `kCardIds[]`, so until
  somebody writes down what it owes the gate names the id and fails. Adding a
  card as `Keyless` is no way past that, because the failure is about an id the
  census does not name and not about the policy stated for it.
- Every census id has to be defined by the module the census names against it. A
  card renamed or retired leaves a row describing nothing, and the gate fails on
  that row rather than grading a shorter list.
- Every module calling `Cards::registerCard()` has to appear in the census, so a
  new module registering under ids this gate cannot read fails as well.
- For each module, the set of policies its registrations state has to equal the
  set the census owes for the cards that module registers. Flipping
  `Sports.cpp`'s live registration to `Keyless` leaves the module stating
  `Keyless` and `Resolves` where the census owes `Resolves` alone, and the gate
  names the module, both sets, and the ids the census read them from.

Both sides have to move together, which is the point of keeping them apart.
Editing the census alone fails the comparison, editing a call site alone fails
the same comparison, and the two edits land in different files where a review
reads them as one decision.

The grading is per module rather than per descriptor, and it rests on an
invariant the gate checks instead of assuming: the cards a module registers owe
one policy between them. All sixteen modules do today. A module whose cards owed
two would let the set comparison pass on a swap, so the gate fails on the first
census module naming more than one policy and says the grading has to become per
descriptor before that module can land.

Nothing about the device changed. `registerCard()` still checks the descriptor
against the policy its own call stated and reads no census, so a census this
gate is wrong about fails a runner and cannot take a card off a display.

## What the device records

Per showing:

| Field | Why |
|---|---|
| Card id | The slot. Known-card ids already on the wire. |
| Content key | Which listing, which link, which picture. Absent where the card is its own content. |
| Times shown, duration shown | The baseline, on every device. |
| Duration with presence | Omitted entirely on a unit with no sensor. |
| Times sought | Reached by forward or back rather than by rotation. |
| Times acted on | The card's action pressed while it was up. |

**Superseded on 2026-10-08. The device reports every showing, not totals.**

This said the device rolls up into a fixed buffer keyed by card and content and uploads
totals, because a per-showing log grows without bound between check-ins and had no
reader. The owner asked for the other shape on 2026-10-08, and it now has a reader: the
server keeps every draw for ninety days and rolls it into quarter-hour rows kept for
thirteen months. `CARD_IMPRESSIONS_DESIGN.md` in the server repository is the current
design, and the server half of it is built and deployed. The paragraph above is kept
rather than deleted so nobody builds from it by accident.

What changes for firmware: buffer one entry per showing rather than a total per card.
Each carries its own instance id, start, dwell, shuffle flag, presence milliseconds,
sought flag, and content as named attributes.

What does not change: the buffer is still bounded, it still has to say when it
overflowed, and the entries still ride the ordinary check-in rather than an endpoint of
their own.

**The buffer is bounded and says when it overflowed.** A listings feed can walk more
properties between check-ins than a fixed buffer holds. When it fills, the device keeps
counting into an "other" bucket and reports that it did, so a report can say "and 40
further listings" rather than dropping them with nothing on screen to say so.

Worth recording why the newer shape is also the better one, so this is not read as
instruction overriding judgement. Totals keyed per card lose the time within the
check-in interval. A display reporting every half hour could never produce the
quarter-hour shape of a day, which is what answers *when should my announcement be up*.
From per-showing entries the server can compute any grain it is later asked for; from
totals it cannot recover one nobody thought to ask for in advance.

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

### Two tiers: every draw for ninety days, quarter hours for thirteen months

**Superseded on 2026-10-08.** This said the hour was the smallest thing stored and
nothing finer was kept. Both halves changed.

`card_impressions` holds one row per showing and is pruned at ninety days.
`card_impression_periods` holds one row per device, card and content key per **quarter
hour**, kept thirteen months, and `device_periods` holds the denominators once per
display per quarter hour. Day, week, month and year are sums over the quarter-hour
rows, so there is still one table per grain and no chance of them disagreeing.

Fifteen minutes rather than an hour because an impression at 07:30 and one at 03:00 are
not worth the same, and an agent deciding when to run a campaign needs the shape of the
day.

The ordering between the tiers is the part that can lose data, and the server enforces
it: the pruner never deletes a draw past the point the rollup has reached, and a rollup
that has never run means nothing is deleted at all.

### Thirteen months, trailing

Thirteen rather than twelve so this October can be compared with last October. A reader
asking "is the screen doing better than a year ago" needs both endpoints, and twelve
months loses the far one the day they ask.

Rows older than thirteen months are deleted on a schedule, oldest hour first.

### Purging on request

Two shapes, because the two reasons differ:

| Purge | Removes | For |
|---|---|---|
| By card type | Every row for a card family across the scope — all QR codes, all announcements | A card type being retired, or counted wrongly and not worth keeping |
| By specific card | One card and one content key — this QR link, this listing, this picture | A campaign that should not be reported on, or content removed at somebody's request |

A purge is scoped to one organization and recorded in the audit log with who asked and
what it covered. Deleting somebody's measurement history is a write worth being able to
account for later.

Totals are recomputed rather than adjusted. Subtracting a purged figure from a stored
roll-up leaves a total that no longer matches its own rows, and that divergence is
invisible until somebody checks.
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
