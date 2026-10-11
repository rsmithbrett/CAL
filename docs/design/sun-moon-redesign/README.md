# Sun and moon cards — redesign

Four reference images in this folder, two per card:

| Card | Day | Night |
|---|---|---|
| Sun | `sunrise-sunset-day.svg` | `sunrise-sunset.svg` |
| Moon | `moon-forecast-day.svg` | `moon-forecast.svg` |

All four are 320×240, which is the panel exactly, so a coordinate in the SVG is
a coordinate on the device.

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

## Not verified on hardware

Checked by a clean compile and by reading, like every other card in
`Display.cpp`. The two things most worth looking at on a real panel are whether
the sunrise and sunset times stay legible where they cross the near ridge, and
whether the day palette's moon discs have enough contrast in daylight.
