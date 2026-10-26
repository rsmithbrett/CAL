# Sun and moon cards — redesign

Four reference images in this folder, two per card:

| Card | Day | Night |
|---|---|---|
| Sun | `sunrise-sunset-day.svg` | `sunrise-sunset.svg` |
| Moon | `moon-forecast-day.svg` | `moon-forecast.svg` |

All four are 320×240, which is the panel exactly, so a coordinate in the SVG is
a coordinate on the device — **on a panel lying flat**. A panel standing on its
end is 240×320 and the moon card draws something else there; see "The moon card
on a turned panel" below.

## Why there are two of each

Every other card in this firmware draws through `bg()`, `ink()` and `muted()`,
which swap on the `isDaytime` flag the check-in response already carries. These
two are full-bleed artwork and would have ignored that — leaving a dark
rectangle sitting among white cards every afternoon, which reads as a fault
rather than a design.

**The geometry is identical between each pair.** Same ridgelines, same text
positions, same type sizes, same disc centres and radii. Only the palette moves,
and the sun's height. That is what lets one draw routine take a palette
parameter instead of two layouts that drift apart the first time one is edited.

The moon's day variant inverts the disc — lit portion dark, shadow light — which
is how a moon looks against a bright sky, so the phase shapes still read.

## What the device already has, and what it works out

Everything the sun card draws arrives on the check-in response:
`sunriseMinutesUtc`, `sunsetMinutesUtc`, `utcOffsetMinutes`. The moon card's
hero needs `moonPhase`, `moonIlluminatedFraction` and `moonPhaseName`, which
also arrive.

**The four upcoming phases are computed on the device.** Nothing on the response
carries phase dates, and nothing needs to: each quarter is a fixed point in a
cycle of known mean length, so "how long until the next full moon" is a
subtraction from the phase already in hand. A server-side answer would have cost
a protocol field, a migration and a simulator edit for arithmetic the device can
do in a loop.

They are sorted into the order they actually occur. The reference image shows a
waxing gibbous, whose next four are full, last quarter, new, first quarter — a
hardcoded order would be wrong for three quarters of the month.

## Drawing notes, and why each choice was forced

**Gradients are one `drawFastHLine` per row, not a sprite.** A full-screen
16-bit sprite is 153,600 bytes. The largest contiguous block these devices
report is a fraction of that even when healthy, and the whole of 2026-10-09 went
on the heap fragmentation that makes large allocations fail. 240 line calls
allocate nothing.

**Ridgelines are filled per column under the quadratic** rather than flattened
to a polygon — no vertex buffer, and no seams on a curve this shallow.

**The sun's glow is three flat rings, not a radial fill.** A true radial is
per-pixel work across a third of the panel for something indistinguishable at
this size once the sky is behind it.

**One `drawMoonDisc` draws all five discs** — the hero and the four upcoming.
Five copies of terminator arithmetic would be five places for the lit side to
end up on the wrong edge, and the quarters are the case that catches it: first
quarter waxes right, last quarter left, so passing the current phase's direction
to every disc draws both the same way round.

## The moon card on a turned panel

**The disc goes over the name, and the four upcoming phases come off.** Both the
disc-beside-the-name and the four-across row are landscape compositions, and
neither survives a 240px panel. Measured against the vendored glyph table:

| Line | Column it was given | What it needs | Before |
|---|---|---|---|
| `MOON \| TODAY` | 122px | 137px at 9pt | ellipsized |
| Phase name | 132px | 199px at 12pt for "Waning Crescent" | ellipsized |
| `X% illuminated` | 132px | 146px at 9pt for "100% illuminated" | wrapped to two lines |
| Each upcoming column | 55px | 68px at 9pt for "First qtr" | all four ellipsized |

So the narrow card is the disc, the phase name, how much of it is lit, and the
date — centred, each on its own full-width row. The widest of them,
"Waning Crescent" at 199px, fits the 220px band with room over.

**The upcoming phases are dropped rather than stacked into a column.** Four
rows of "in 12d" is a lot of card for arithmetic the device works out rather
than something a household asked this panel for, and the space buys a disc
nearly three times the area of the landscape hero. Landscape keeps them: the
row fits there, and nothing about it reads badly in the shot.

**The date is new, and it replaces the word TODAY.** The heading said
`MOON | TODAY` and the card never showed a date. A heading saying TODAY beside a
line saying Saturday, October 25 says the same thing twice, so the narrow
heading is just `MOON`. The date is `strftime("%A, %B %d")` off the same local
time the card's own corner clock reads, which is the format and the source
`showClockDate()` already uses, so the two cards cannot name different days.
Landscape keeps `MOON | TODAY` and shows no date.

## Every separator on these two cards is ASCII

The vendored `FreeSansBold` tables carry 0x20 to 0x7E and nothing else, and
`LGFXBase::text_width()` substitutes a space for any code outside the face —
**per byte**, so a UTF-8 character leaves as many blanks as it has bytes.

Three of these shipped. `MOON · TODAY` and `SUN · TODAY` used U+00B7, two bytes,
and reached the glass as `MOON  TODAY` and `SUN  TODAY` with a hole where the
dot belongs. The sun card's two labels used U+2191 and U+2193, three bytes each,
and drew as three blanks: the sunrise label sat indented from the 18pt time
under it, and the sunset label, being right-aligned, kept its blanks on the side
nobody sees. The sports card's `Q3 · 7:42` is the fourth and has the same cause.

So the dot is a vertical bar, which is in the face at every size these cards
use and reads as the same field separator, and the arrows are gone — `SUNRISE`
and `SUNSET` name the direction without them, and the labels now line up with
their own times. `panel-moonphase-*-landscape.png` and `panel-sun-*.png` are
where this is visible.

**Measured up from the content floor, not down from the top.** The next-event
line sits 18px above whatever `contentBottom()` says, the date 22 above that, the
lit line 22 above that and the name 28 above that; the disc takes what is left
between the heading and the name, capped at a 72px radius so it keeps a clear
margin either side of the glass. An action bound to the card lowers the floor by
80px and the disc gives up the pixels, rather than the bottom line being pushed
off the glass.

Photographed at both sizes, four phases each, with and without an action bound:
`docs/ia/screens/panel-moonphase-*.png` in the server repository, written by
`DAM_PANEL_SHOTS=1 dotnet test tests/DiscoverAroundMe.DeviceSimulator.Tests`.

The sun card is photographed the same way now — `panel-sun-*.png`, day, night
and the no-times case, both sizes, with and without an action bound — by a
`SunMoonPanel` renderer that did not exist before. The shots also paint the
action button row, which they did not: the band a bound action reserves used to
photograph as empty glass, so a card that stopped short of its floor looked
like a card with room to spare.

**The sun card beside it does not have this problem.** Every string on it is
given a column helper rather than a landscape constant — `kWidthBeside`,
`kHalfColumnWidth`, `kContentWidth` — so each one narrows with the panel.
"SUN | TODAY" measures about 119px against the 122px the heading gets on a 240px
panel, the labels have room, and the one line that outgrows its column is the
18pt time: "6:47 AM" is 130px against a 110px half column, so the fitting ladder
draws it at 12pt instead.

**That one is now a photograph rather than a measurement, and it stays left
alone.** `panel-sun-day-portrait.png` shows the 12pt pair: both times are still
twice the height of the `SUNRISE` and `SUNSET` labels above them and of the
daylight line below, they are the largest type on the lower half of the card,
and nothing is cut. A 12pt hero is smaller than an 18pt one and still the hero
of its row, which is what the ladder exists to produce. The log says so on
every draw — `1 line(s) of 23px, where one of 33px would have ellipsized`.

**Two other things that picture shows, and neither is fixed here.** The card's
whole composition is landscape constants — ridges at 144..179, labels at 150,
times at 170, the daylight line at 202 — so on a 240x320 panel it finishes at
y=240 and leaves the bottom 80 pixels an empty field of near ridge. And with an
action bound it draws straight through the button row: the card never reads
`contentBottom()`, so `panel-sun-day-landscape-action.png` shows both times and
the daylight line covered by the button. The second is the one that costs a
reader a fact.

**The daylight line ellipsizes in the no-times case, on both panels.** When the
server sends no sunrise and no sunset, `SunMoon.cpp` passes "No sunrise or
sunset today for this location." — 9pt, past 300px in landscape and past 220 in
profile, so it draws as "No sunrise or sunset today for th..." and the card
stops saying which location it means. Visible in `panel-sun-notimes-*.png`.

## The next moonrise or moonset

**One line, and it is whichever of the two comes next.** Not both times. The card
already carries the phase, how much of it is lit and the date; a pair of labelled
times is four more pieces of type on a card whose subject is a picture, and the
question somebody standing in front of it asks is "when does it come up",
singular.

The server works out which event is next and sends it as
`nextMoonEventMinutesUtc` plus `nextMoonEventKind` — see the server repository's
README under "The next moonrise or moonset" for why the choice is made there
rather than here. This card does no selection and no clock comparison of its own.
It turns the minute into local wall-clock with `gUtcOffsetMinutes`, the way every
other time on the card already does, and prints `Moonrise 8:14 PM` or
`Moonset 6:02 AM`.

**Three states, three different drawings.** A kind of `rise` or `set` with a
minute draws the time. A kind of `none` draws `No moonrise or set`, which is
the server saying it looked and there is no event to name. Absent fields draw
nothing at all: no line, and no gap where a line would be, so the card is pixel
for pixel what it was before this change on a device whose position the server
cannot resolve. A dash would have collapsed those last two into one appearance,
which is the mistake this project has made before with cards holding nothing.

**Portrait takes it at the bottom of the stack, under the date.** The stack is
still measured up from `contentBottom()`, so the new line is the one 18px above
the floor and the date, the lit line and the name each move up 22. On a 240×320
panel with no action bound the space left for the disc is still larger than the
72px cap, so the disc does not change at all. With an action bound the floor
comes up 80px and the disc gives up the pixels: 49px of radius against the 60 it
had.

**Landscape takes it at y=100, in the column beside the disc.** That row was
free. The lit line sits at 78 and the `NEXT MAJOR PHASES` label at 122, and the
22px between them were reserved for a second line of "100% illuminated" that a
landscape panel never wraps — the column there is 212px and the longest form of
that string is 146. The reservation dates from before the narrow branch above
existed, when this code drew both panels from these constants. `Moonrise 8:14 PM`
is about 130px at 9pt, so it fits the column with room, and the lit line now gets
a single-line budget instead of the two it could not use.

So the four upcoming phases stay on landscape. They were the thing at risk if
this had needed a row of its own.

## Not verified on hardware

Checked by a clean compile and by reading, like every other card in
`Display.cpp`. The two things most worth looking at on a real panel are whether
the sunrise and sunset times stay legible where they cross the near ridge, and
whether the day palette's moon discs have enough contrast in daylight.
