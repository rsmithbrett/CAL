# What the reporting actually needs

A review of the impression data model against the thing it exists for: telling an agent
or a brand what their screen earned, in terms they can put in front of a client.

The design so far records four measures per card per hour. That is the raw material. It
is not yet a report, and several things a report cannot be written without are missing.

---

## 1. The number means nothing without a denominator

"Your QR card was shown 400 times" is unusable. Four hundred out of what?

A fleet of two displays and a fleet of twenty produce wildly different totals for
identical performance. So does a device that was unplugged for a week. So does a month
with 31 days.

**What has to be stored: device-hours.** Per device, per hour, whether the device was
powered and showing cards at all. Without it there is no rate, and without a rate there
is no comparison — between months, between displays, or between agents.

This also fixes the commonest false alarm: a display that was off for four days produces
a drop that looks like falling interest and is actually an unplugged cable.

## 2. Cards compete with each other, and the report will lie about it

The rotation is finite. Add a sixth card and the other five each lose a sixth of their
showings. An agent who adds a listing card and sees their QR card "decline" has been
told something false by arithmetic.

**What has to be stored: how many cards were in rotation, per device, per hour.** Then a
card's showings can be expressed as a share of the rotation it was competing in, and
adding content stops looking like losing ground.

This is not a nicety. It is the difference between a report that survives a client
asking "why did this go down" and one that does not.

## 3. Duration and count are not interchangeable

A card at a 30-second dwell accumulates twice the screen time of one at 15 seconds with
the same number of showings. Ranking cards by duration therefore ranks them by their
configured dwell.

**What has to be stored: the dwell in force.** It already travels in `CardPolicyEntry`
as `DwellSeconds`, and `NotableDwellSeconds` for list cards that hold longer on an
interesting item — so a listings card's time is not even uniform within itself.

The report should lead with counts and treat duration as secondary, and must never rank
by duration without saying dwell differs.

## 4. Sessions are the unit advertising actually buys

This is the biggest gap.

Fifty card-views to one person over one evening is not fifty impressions. In every
advertising model that matters it is **one reach and fifty frequency**, and conflating
them overstates performance by an order of magnitude. An agent who repeats "four hundred
impressions" to a client on that basis will eventually be caught out, and the product
will be what let them down.

The device cannot identify people and should not try. It can do something nearly as
useful: **bound a visit.** A continuous run of presence — sensor asserted, or the
controls being worked, with no gap longer than a few minutes — is one visit. Everything
drawn inside it is that visit's views.

**What has to be stored: visits, and views per visit.** Then:

- *Reach* is visits.
- *Frequency* is views divided by visits.
- *Attention* is time per visit.

A household of three is still one visit when they walk past together, and that is the
honest reading of what the sensor saw. Say so in the report rather than implying people.

Without this the product can report activity. With it, it can report **audience**, and
that is the difference between a feature and the reason the device exists.

## 5. Time of day is the whole value of an hourly bucket

Hourly storage is already specified, and the report must use it rather than only summing
it. An impression at 07:30 and one at 03:00 are not worth the same, and an agent
deciding when to run a campaign needs the shape of the day.

**What the report needs: hour-of-day and day-of-week profiles**, per display and per
fleet. The data supports this for free; only the report has to be written to show it.

It also answers a question the agent cannot otherwise ask: *when should my announcement
be up?*

## 6. Content has to stay nameable after it is gone

A report covering last quarter will name a listing that has sold, a QR link that was
replaced, a picture that was deleted. Resolving the content key against today's records
produces blanks exactly where the history is most interesting.

**What has to be stored: the human label, captured when the content was first seen.**
"Open house — 123 Oak St" alongside the key, frozen. Changing the content starts a new
key and a new label; deleting the asset leaves the label intact.

Otherwise the thirteen-month retention is thirteen months of rows nobody can read.

## 7. Where the display is, as it was then

A display moves. It gets renamed. It gets reassigned to another agent in the same
brand.

**What has to be stored: the display's name and its owning account, as at the hour.** A
report that resolves ownership at read time silently re-attributes last March's results
to whoever holds the device now, which is wrong for both parties.

## 8. The funnel has a last step and it already exists

Shown → Seen → Sought → Acted on is four of five. The fifth is what the action produced,
and the press log already records it.

**What the report needs: the join.** A QR scanned, a button pressed, an email sent — tied
back to the card and content that prompted it. Everything needed is already stored on
both sides; nothing links them today.

That join is what turns "your card was seen 130 times" into "your card was seen 130
times and produced 9 enquiries", which is the only sentence in this document a client
will actually care about.

## 9. It has to leave the building

An agent reports to a client; a brand reports to its agents. A number that can only be
seen by signing in is a number that does not get used.

**What the report needs: an export, and a figure that survives being quoted.** CSV for
the agent's own analysis, and a dated summary they can attach. `/activity` already
exports presses; this should sit beside it rather than inventing a second idiom.

## 10. Say what was measured, every time

Three things routinely make a figure smaller without anything being wrong: a display
with no motion sensor, a display that was offline, and a card added partway through the
period.

**Every report carries its own caveats inline** — "across 7 of your 9 displays", "card
added 12 March", "display 3 offline 4 days". A footnote nobody reads is how a number
gets quoted out of context, and the first time that happens in front of a client is the
last time they trust the product.

---

## What this adds to the record

Beyond the four measures already specified:

| Needed | Where it comes from |
|---|---|
| Device-hours powered and showing | Device, per hour |
| Cards in rotation | Device, per hour |
| Dwell in force | Already in `CardPolicyEntry` |
| Visits, and views per visit | Device bounds the visit; server stores both |
| Content label at first sight | Server, frozen on first upload of a new key |
| Display name and owner, as at the hour | Server |
| Press log join | Both sides exist; the link does not |

## What I would build first

**Visits.** Everything else sharpens a number; visits change what the number *is*. Reach
and frequency are the vocabulary the buyer already uses, and the device can bound a
visit honestly without identifying anybody.

**Then device-hours**, because without it nothing can be compared to anything.

The rest improves a report that already says something true.
