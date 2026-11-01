# Text layout: one routine, and why every card has to go through it

Two photographs taken off real hardware started this. Both show a 320x240 panel
losing characters at the right edge, and the interesting part is that they are
**two different pieces of code making the same mistake in two different ways**.
A per-card fix for either one would have left the other standing, and would have
left eight more call sites that have not been photographed yet.

So this document settles a single text layout routine, what it guarantees, how a
card tells it where it may draw, what it does when the text still will not fit,
and what stops the next card from going back to the ad-hoc way.

Related reading: README.md's *The content budget: a card knows whether a button
will cover it* for the vertical half of the same problem, and `App/Http.h`'s
`kTlsRecordBufferBytes` remarks (plus `docs/DEVICE_MEMORY.md` in the server
repository, which is where the measured figures live) for why an allocation on
the draw path is a defect rather than a style preference.

---

## 1. The two photographs

**The sports card.** The second team row read `Houston Astr`. The word stops at
the right edge of the name column with no ellipsis and nothing on the panel
saying anything was lost. A household reading it sees a team whose name is
`Houston Astr`.

**The notice card.** Three lines:

```
Test device: this rotation h
one card of every type the
catalog knows.
```

Line one is cut inside `has` and the `as` is not anywhere else on the card. The
next line starts at `one`. So the wrap did not merely look bad, it put a string
on the panel that was wider than the panel and let the driver clip the overflow
into nothing.

Those are the visible symptoms. Underneath they are two separate bugs.

### 1a. The sports card bug: truncation with no indication

`drawTruncatedLeft()` shortens by one character at a time until the measured
width fits, then draws. That is correct about the width and wrong about the
honesty: the result is indistinguishable from a team that is genuinely called
`Houston Astr`. Same code shape in `drawRightJustified()` and in
`drawOneButton()`, which each carry their own copy of the loop.

### 1b. The notice card bug: the greedy wrap breaks at the wrong space

`wrappedLeftText()` and `wrappedCenteredText()` both walk the string, remember
the most recent space in `lastSpace`, and on an overflow break the line at
`lastSpace`. The defect is the order of two statements:

```cpp
if (isSpace) {
  lastSpace = i;                                   // (1) i is recorded as a break point
}
...
const String candidate = text.substring(lineStart, i);
if (lcd.textWidth(candidate) <= maxWidth) { ... }  // (2) candidate does NOT fit
const int breakAt = (lastSpace > lineStart) ? lastSpace : i;
lcd.drawString(text.substring(lineStart, breakAt), ...);
```

When the overflow is discovered at a space, step (1) has already set
`lastSpace == i`, so `breakAt == i`, so the line drawn is exactly the candidate
that was just measured and rejected. The measurement happens, the answer is
ignored, and the too-wide line goes on the panel to be clipped by the hardware.
`Test device: this rotation has` is 30 characters of which 28 fit, and the `as`
was drawn past x=320.

This is worth naming precisely because the function's own comment says it
measures with real font metrics, and it does. Measuring is not the missing
piece. Acting on the measurement is.

**The same function, with the same statement order, is in CAL's own root
`Display.cpp`** (its `wrappedCenteredText`, used by `showStatus()`,
`showFailure()` and the provisioning QR screen). CAL's copy has never been
photographed failing, but it is the same code and it draws server-supplied
refusal messages, so it is fixed in the same pass. CAL does not get the whole
routine. It gets the break-point correction, the ellipsis, the mid-word break
for a word wider than the screen, and the same fixed stack buffer in place of
`String`, because CAL has one call site shape and importing a layout module into
the factory partition buys nothing it can use. Its one log line goes through
`Journal::printf`, which survives the reboot that Serial does not.

---

## 2. Survey: every place `App/` draws a string into a bounded region

Nothing outside `App/Display.cpp` draws text at all. `grep` for `drawString`,
`textWidth` and `.print(` across `App/` finds `Assets.cpp` writing to a file and
one comment in `Calendar.cpp`, and no other renderer. So the whole problem is
inside one file, which is the good news.

Inside that file, before this change, there were **five distinct approaches**:

| # | Approach | Where | What it does today | Failure mode |
|---|---|---|---|---|
| 1 | Greedy word wrap, measured | `wrappedCenteredText`, `wrappedLeftText` | Wraps on `' '`, measures with `lcd.textWidth` | Breaks at the overflowing space (1b), so a too-wide line is drawn and clipped. Runs out of `maxLines` and drops the remainder with no mark and no log |
| 2 | Truncate one character at a time | `drawTruncatedLeft`, `drawRightJustified` | Shortens until measured width fits | No ellipsis (1a). Takes `String` by value and calls `substring()` in a loop, so one draw of a long name is a dozen heap allocations |
| 3 | The same loop again, inline | `drawOneButton` | Identical shortening loop, written out a third time | Same as 2, and drifts from 2 independently |
| 4 | Truncate by character count | `shortDayLabel` | `name.substring(0, 3)` | Assumes 3 glyphs fit a 60px column at whatever font is current. No measurement at all |
| 5 | No bound whatsoever | 20 raw `lcd.drawString()` calls | Draws at a datum and hopes | Silent clipping for anything server supplied |

Approach 5 is the largest group and the one most worth listing by call site,
because "unbounded" is easy to read as "short enough". The ones carrying data
this firmware does not control:

| Call site | String | Why it is not safe today |
|---|---|---|
| `showSportsCard` score | `homeScore` / `awayScore` | Provider text, right aligned against the margin, nothing stops it colliding with the name column |
| `showSportsCard` status | `status` | The provider's own progress wording, drawn raw at `kCardMargin, 172` |
| `showAircraftCard` | `distanceBuf`, `updatedAt` | `updatedAt` is server formatted |
| `showListingsCard` | `updatedAt`, `distanceBuf` | same |
| `showCalendarCard` | `"N of M"` built with `String` concatenation | allocates on the draw path |
| `showForecastCard` | `shortDayLabel(...)`, `tempBuffer` | column bounded, never measured against the column |
| `showClockDate` | `timeText` | measured once, and the only response available is a font size drop |
| `showQrTextCard` | `caption` | drawn through wrap 1, so inherits 1b |
| `drawClock` | `clockText` | bounded by construction, but by convention rather than by code |
| `drawCardBanner` | `label` | all callers pass literals, and nothing says they must |

The remaining raw calls draw compile-time literals (`"Altitude"`, `"Speed"`,
`"Next high"`, `"Beds"`). Those are bounded because the programmer can see the
string, which is a real argument and is exactly the argument the gate in section
7 encodes. They go through the routine anyway, with a declared width, because
the width is the part nobody had written down: every stat row was a label drawn
from the left margin with no bound at all and a value right-aligned in a 150px
column, and the two were kept apart by the labels happening to be short. The
label column is now reserved explicitly as `rightX - rowValueWidth - margin`,
and on the sun/moon and tides cards as the gap before the icon column, so the
arithmetic is in the code rather than in a comment.

---

## 3. The routine

One function, private to `App/Display.cpp`, in the anonymous namespace beside
the panel object it draws on. It is not a separate translation unit because the
`LGFX lcd` instance is file local and handing a reference to it across a module
boundary would widen the surface that can put pixels on the panel for no gain;
the survey above proves there is no caller outside this file.

```cpp
enum class Align : uint8_t { Left, Centre, Right };

// A card declares the rectangle of panel it is willing to spend on a string,
// and how many lines of it.
struct TextBox {
  int16_t x;           // left edge for Left, centre for Centre, right edge for Right
  int16_t y;           // top of the first line
  int16_t width;       // pixels the text may occupy, measured, never assumed
  int16_t lineHeight;  // step from one line's top to the next
  uint8_t maxLines;    // the line budget
  Align align;
};

struct TextResult {
  uint8_t lines;      // lines drawn, or that would be drawn under measureOnly
  bool ellipsized;    // true when "..." went on the panel
  int16_t bottom;     // y just past the last line, for stacking the next block
};

TextResult layoutText(const char* text, const TextBox& box, uint32_t colour,
                      const char* what,
                      uint32_t background = kUseCardBackground,
                      bool measureOnly = false);

TextResult layoutText(const String& text, ...);  // same, via c_str(), no copy
```

Three things sit on top of it, all in the same anonymous namespace:

```cpp
// The one-line shape, which is most of this file: a stat value, a team name,
// an address, a button label. One line means the "lines ran out" rule applies
// immediately, so anything too wide ellipsizes. lineHeight comes from
// lcd.fontHeight() so the returned `bottom` is not a lie.
TextResult layoutLine(const char* text, int x, int y, int width, uint32_t colour,
                      const char* what, Align align = Align::Left,
                      uint32_t background = kUseCardBackground);
TextResult layoutLine(const String& text, ...);   // same, via c_str()

// How many lines of lineHeight fit between topY and contentBottom().
uint8_t linesToBudget(int topY, int lineHeight);

// The prose body the notice and calendar cards share: 12pt, dropping to 9pt
// when the 12pt pass would ellipsize. Section 4 has why this is one function.
TextResult drawProseBody(const String& text, const char* what);
```

Both `const char*` and `const String&` overloads exist throughout for one
reason: half the call sites hand over an `snprintf`'d stack buffer precisely so
that nothing allocates, and a single `String` entry point would build a
temporary on every one of them.

`what` is a short name for the debug stream, `"sports.away"` or
`"notice.body"`. It exists so a line in the log says which string on which card
lost characters. This follows `noteContentOverrun()`, which already takes a card
name for the same reason.

The `String` overloads exist because every public function in `Display.h` takes
`const String&` and this change does not touch that contract. They call
`.c_str()` and allocate nothing.

Logging splits the way `Log.h`'s own remarks ask it to. An ellipsis, a hard
break and an empty draw are "exactly what a card drew this cycle", which that
header names as the `Log::verbose` case, so they are verbose and cost nothing
on a device nobody is streaming. A box narrower than one glyph, a box with a
zero line budget and a call that drew no lines at all are layout mistakes rather
than long strings, so they take `Log::printf` and reach Serial whether or not
anyone turned streaming on.

### Guarantees

1. **Word boundaries, measured.** A line breaks at the last whitespace
   **strictly before** the word that overflows. That is bug 1b, stated as a
   property instead of as a fix.
2. **Nothing is drawn wider than `box.width`.** Every line is measured in the
   font the caller selected before it is drawn, and no line goes out unmeasured.
3. **Nothing disappears without a mark.** Text that does not fit is wrapped,
   or hard broken, or ellipsized. There is no fourth outcome.
4. **Nothing disappears without a log line.** An ellipsis, a hard break and an
   empty draw each narrate themselves to the debug stream.
5. **No heap allocation.** Not one, on any path.

### Whitespace

Any byte `<= 0x20` is a break opportunity, not just `' '`. A `'\n'` or a
`'\t'` in an admin's announcement is otherwise handed to `drawString()` and
rendered as whatever box glyph this ASCII-only font has for it, in the middle of
a household's notice. `Calendar.cpp` already scrubs control characters on the way
in for exactly this reason and says so; doing it here as well means the next card
to take free text does not have to remember. Any such byte that ends up interior
to a drawn line is copied out as `' '`.

---

## 4. How a card declares its region and its line budget

Three things, and the card has to mean all three.

**The rectangle.** `x`, `y`, `width`. Width is in pixels, and it is the card's
job to reserve competing space first. The sports card already does this
correctly and its comment says why: the score column is subtracted before the
name column is sized, so a long club name can never push a score off the edge.
That reasoning survives unchanged; only what happens at the edge of the reserved
column changes.

**The line height.** The step between lines. Passed rather than derived from
`lcd.fontHeight()` because the cards already choose their own leading (24px for
the notice body at 12pt, 18px at 9pt, `kRouteLineHeight` at 18 for the aircraft
route) and those numbers were tuned against the fixed y positions around them.

**The line budget.** `maxLines`. A card may write a constant, and most do,
because the y positions below the block are fixed. A card whose block is the
last thing on the panel should instead ask:

```cpp
uint8_t linesToBudget(int topY, int lineHeight);   // (contentBottom() - topY) / lineHeight
```

which reads `contentBottom()`, so it returns fewer lines when a button row is
about to be painted over the bottom 60px. This is the join between this document
and README.md's content budget: the budget says how far down the card may draw,
`linesToBudget()` turns that into the number this routine wants, and the card
stops hardcoding a count that was measured for one of the two chrome
configurations.

A card is free to ignore `linesToBudget()` and pass a constant. It is not free to
pass a budget it did not think about, because `maxLines` has no default.

**The notice and calendar cards are the ones that stopped writing constants.**
Both had the same eighteen lines: measure at 12pt with a five line cap, use
12pt when the measurement came back at five lines or fewer, otherwise drop to
9pt with a seven line cap. They are now one `drawProseBody()`, and merging them
exposed a third defect nobody had photographed:

> `wrappedLeftText()` stopped drawing at `maxLines`, so asking it for the line
> count **with a five line cap** and then testing `lines <= 5` was a test that
> could not fail. **The 9pt fallback tier was unreachable code.** A notice long
> enough to need it was cut off at five 12pt lines, with the denser size it was
> supposed to drop to never once being tried.

What the card wants to know is whether the text survived, which is what
`TextResult::ellipsized` answers and what a line count never could. And the
budgets are `linesToBudget()` now rather than 5 and 7, because those two
numbers were measured against `kButtonRowY` back when the card had no way to
ask whether a button row was coming. The 7 by 18px tier is recorded in
README.md as landing 2px inside the button gap; asking the budget gets 6 lines
when a button is bound and 10 when none is, instead of one compromise that is
slightly wrong in both directions.

---

## 5. What happens when it still does not fit

Three cases, in the order the routine meets them.

**The line has a break opportunity.** Break at the last whitespace before the
overflowing word. Ordinary wrapping, nothing is logged, this is the common path.

**A single word does not fit on a line of its own, and lines remain.** Hard
break it at the last character that fits and continue the rest on the next line.
Not an ellipsis: a 44-character URL under a QR code is better as two ugly lines
than as one line with its tail deleted, and rule 3 above says nothing may be
dropped while there is still somewhere to put it. Logged with `Log::verbose`,
since it is per draw and the log API's own remarks name "exactly what a card
drew this cycle" as the verbose case.

**The lines run out with text remaining.** Ellipsize the last line: back
characters off until the line plus `"..."` fits the box, then draw it with the
`"..."` attached. Logged with `Log::verbose`, with the count of characters that
did not make it, so the stream says how much was lost rather than only that
something was.

Those last two meet at `maxLines == 1`, which is the single-line case the sports
card, the listings address and the button labels all want. A team name that does
not fit its column runs out of lines immediately, so it ellipsizes. `Houston
Astros` becomes `Houston Ast...`, which is a household reading a name that was
shortened rather than a household reading the wrong name.

### The ellipsis is three ASCII periods

`"..."`, not `U+2026`. Every GFX font in this binary is ASCII only. This file
already hand-draws a circle for the degree sign rather than reach for `°`, and
the aircraft card writes `->` rather than an arrow, both with comments saying
why. A single-glyph ellipsis would render as a box, which would be a worse lie
than the truncation it was added to disclose.

### Degenerate boxes

A box with a zero line budget or no width at all cannot be drawn into. That is a
layout mistake rather than a long string, so it takes `Log::printf` (always on
Serial) instead of `Log::verbose`, draws nothing, and returns zero lines. A box
narrower than one glyph draws that glyph anyway and says so loudly: looping
forever on a character that will never fit is worse than one overhanging mark,
and the loud line is what gets the box fixed. An empty or null string draws
nothing and says so at verbose. "A card that silently drew nothing" is a shape
of bug this project has paid for more than once, so the absence of ink gets a
line in the stream the same as the presence of it.

---

## 6. Heap: why the routine is written the way it is

`heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)` is the number that decides
whether this device can do anything, and `mbedtls_ssl_setup()` needs
**16,717 contiguous bytes twice** (`App/Http.h`'s `kTlsRecordBufferBytes`, and
the server repository's `docs/DEVICE_MEMORY.md`). A device
below that floor cannot open a TLS session, so it cannot check in, cannot report
telemetry, and cannot be told that the firmware update fixing it is waiting.
Fragmenting the largest block is therefore worse than using more of it, which is
the reasoning `App/Sports.h` already gives for storing thirty-two team names in
flat fixed buffers rather than in `String`.

The old helpers did the opposite. `drawTruncatedLeft(String text, ...)` takes its
argument **by value**, so every call copies the string onto the heap, then calls
`substring()` inside a loop, allocating and freeing a fresh `String` per
character removed. `wrappedLeftText` allocates a `String` per candidate
measurement and another per line drawn. The notice card measures at one size and
then draws at another, so it runs the whole allocation storm twice per draw.

The routine allocates nothing:

- The line under construction lives in a **fixed 128-byte stack buffer**. 128 is
  above any line this panel can hold: the narrowest glyphs in the binary are
  `Font0` at size 1, six pixels wide, so 320px is at most 53 characters, and the
  GFX faces the cards use are all wider than that. A word too long for the
  buffer falls into the same hard-break path as a word too wide for the box, so
  it is broken and carried rather than silently cut, and the hard break is
  logged.
- Measurement is `lcd.textWidth()` on that buffer, once per word rather than
  once per character, so a line costs about as many measurements as it has
  words.
- The `String` overload calls `.c_str()`. Nothing is copied, nothing is
  constructed, and the `const String&` public API in `Display.h` is untouched.
- Several call sites stopped building their text with `String` at all while
  they were being converted: the aircraft card's three stat values were
  `String(altitudeFeet) + " ft"` and friends, the calendar counter was
  `String(n) + " of " + String(m)`, the moon card wrapped an existing `char`
  buffer in a `String` to pass it, and the listings square footage built one to
  hold a number or a dash. All are `snprintf` into a stack buffer now. That is
  eight fewer heap allocations per rotation on the cards that had them.

Stack rather than heap is the whole point: 128 bytes of stack are not part of the
contiguous 8-bit pool `mbedtls_calloc` draws from, so this routine cannot move a
device toward the TLS floor no matter how often a card redraws.

---

## 7. How a caller that wants the old behavior is prevented from having it

Three mechanisms, in increasing order of how much they can actually stop.

**The old helpers are deleted, not deprecated.** `drawTruncatedLeft`,
`drawRightJustified`, `wrappedLeftText`, `wrappedCenteredText` and
`drawOneButton`'s inline truncation loop are gone from the file. A deprecated
helper with a comment saying not to use it is a helper that gets used, and these
four were themselves the result of the same shortening loop being written out
three separate times. There is no name left to call.

**`maxLines` has no default.** A caller cannot get "as many lines as it takes"
by omitting an argument. Declaring the budget is the price of drawing text.

**Three CI gates, in `ci/build-firmware.sh`.** The compiler cannot catch a new
`lcd.drawString(someServerString, x, y)`, and the firmware has no unit test
harness, so it is checked the only way it can be, which is the same way that
script already checks that every public `Motion` entry point has a caller.

The first gate:

> Every `lcd.drawString(` in `App/Display.cpp` must either pass a string literal
> as its first argument, or sit on a line carrying the `LAYOUT-PRIMITIVE`
> sentinel comment.

A literal is bounded because the programmer can see it. The sentinel marks four
places (five call sites, since `drawTemperature` draws twice), each with a
comment beside it saying why:

| Sentinel site | Why it cannot be a `TextBox` |
|---|---|
| `layoutText()`'s own draw | It is the routine |
| `drawClock()` | Needs `bottom_right` so the baseline pins 4px off the panel edge inside a 16px band; a top-anchored box cannot express it. The string is `formatTimeOfDay()`'s own, bounded to eight characters |
| `showClockDate()`'s hero | Needs `middle_center` at y=100, and already measures and gives up a whole font size rather than clipping. It now also logs if even the smaller size will not fit |
| `drawTemperature()` | Advances a cursor across a number, a hand-drawn degree ring and a unit. It is not filling a box, and its inputs are a formatted `int` and a one-character unit |

The second gate refuses the four retired names by grep, so bringing one back
fails the build rather than quietly reinstating the defect.

The third gate is `ci/check-text-state.sh`, which holds every draw function to
returning a text scale of 1. It arrived later than these two and section 14 has
the defect it was written for.

All three are deliberately greps and nothing cleverer, in the same spirit as the
Motion gate: they do not know whether the text will fit, they know the
difference between "went through the routine" and "did not", which is the
failure that actually happened and the one worth a gate.

---

## 8. What this does not change

- **No wire contract moves.** `CheckInResponse` and every field on it are under
  the six month backward compatibility rule that closed on 2026-09-22. This work
  needs no new field, optional or otherwise. The strings were always long enough;
  what was wrong was what the panel did with them.
- **`Display.h`'s public signatures are untouched.** Every `show...Card()` still
  takes `const String&`. Cards, `CardManager` and `App.ino` do not change.
- **No card's fixed y positions move.** The routine wraps and ellipsizes inside
  the boxes the cards already use. A card that wants a different box is a
  separate change with its own measurement.
- **`shortDayLabel()` keeps its three-character abbreviation** because "Wed" is
  the wording wanted, rather than a width fallback. It now goes through the
  routine as well, so a locale or font change that makes three characters too
  wide for a 60px column ellipsizes instead of overprinting its neighbour.

---

## 8a. What the sports card got, beyond the layout

Three changes landed on this card in the same pass, because it is the card the
photograph came from and because two of them are about what the text *says*
rather than how wide it is.

**The start time was UTC wearing a local label.** `Sports.cpp`'s
`startTimeText()` called `localtime_r(&game.startsAtUtc, &local)`.
`AppService.cpp` calls `configTime(0, 0, ...)`, so the C library's timezone
offset on this device is zero by design and `localtime_r` is `gmtime_r` under a
misleading name. A 21:40 Eastern baseball game was photographed reading 01:40,
which is worse than an obviously broken value because 01:40 is a time somebody
will believe. It now adds `Display::utcOffsetMinutes() * 60` and calls
`gmtime_r`, which is exactly what `ClockDate.cpp` and `Calendar.cpp` already do.

**It ignored the household's 12-or-24-hour setting**, because it formatted
`%02d:%02d` itself. It goes through `Display::formatTimeOfDay()` now, for the
reason `ClockDate.cpp`'s own comment gives: the clock card would be the first
place anybody noticed that setting not applying, and a game start time is the
second.

`startsAtUtc == 0` is reachable, not defensive. `CheckIn.cpp`'s
`parseIso8601Utc()` returns 0 for a missing, null or unparseable value and a
scheduled game can carry any of the three. `--:--` stays, and it is not a third
convention: `formatTimeOfDay()` renders `--:--` itself for a time it cannot
state, in 12-hour households as well as 24-hour ones.

**The away team is prefixed with `@`.** Two stacked names said nothing about
which was at home. `@ Houston Astros` is the convention every American
scoreboard uses. Only the away row is marked, because a home team with no
marker is the home team.

**That paragraph was wrong about which row, and it shipped.** `@ X` is read
"at X", so the marker names the **host**, not the visitor. Marking the away
row therefore stated that the away team was hosting — photographed on device
23 as `Yankees / @ Orioles` on a day the Yankees hosted, which is the exact
inversion of the truth and reads as a completely ordinary scoreline. Nothing
about it looks wrong, which is why it survived a photograph, a review, and
this document.

**Corrected 2026-09-27, and the correction took the row order with it.** Two
variants were available. Moving the marker onto the home row alone is one line
and still reads wrong, because no scoreboard or ticker writes the host first.
So the rows swapped as well: **away on top unmarked, home underneath carrying
the `@`**, which is the vertical form American scoreboards use —

```
  NYY  5
@ BAL  3
```

— and that same photographed game now renders `Orioles / @ Yankees`.

The part that belongs to this document is what makes the swap dangerous to do
by halves. **The row index picks the position and the flag picks the team.**
Name, score and marker are all selected by `isAway`; `y` is selected by `row`.
Swapping one of the four and not the others puts a score against the wrong
team, and a scoreline with the numbers transposed looks exactly as plausible
as a correct one — strictly worse than the bug it came from, because there is
then nothing on the panel that looks off at all.

The fix is firmware, not server. The marker exists only in `App/Display.cpp`,
and swapping the two names server-side instead was refused: it works today and
can never be undone, because the wire contract documents what those fields
mean, the device's own `[sports] on screen:` line would start lying, and every
firmware flashed afterwards would carry the inversion forever. That log line
was flipped to read away-first in the same change, since it is what anybody
verifying the fix actually reads — nobody is standing in front of device 23.

The part that belongs to this document: **the `@` is joined to the name before
anything is measured.** Added after the fit was computed it would push the name
one glyph further into being cut, and the marker itself could end up being what
the ellipsis ate, so the layout would have created the defect it exists to fix.
The join goes into a 96-byte stack buffer against a 20-character wire cap
(`Sports.h`'s `kMaxTeamNameLength`), which is not a guess at what fits the panel
but room for anything a caller could pass, and a caller that somehow exceeds it
says so on the stream.

**The team name is two-tier now, like the notice body.** The `@` costs real
width on a column that was already too narrow for "Houston Astros" at 18pt, so
the row measures at 18pt and drops to 12pt when the whole name will not survive.
The score stays at 18pt: this card's own comment says the score is what a reader
across the room is after, so the name is the one that gives up size. The
ellipsis is still there for a name that fits neither, which makes this a way of
needing the ellipsis less often rather than a way of avoiding it.

**Three-tier as of 2026-09-27, the third at 9pt.** Two tiers were not enough:
the 12pt tier still ellipsizes anything from "@ New York Yankees" (247px)
upward against a 236px column, and long names are not the rare case. Three
separate routes produce one — football is returned untouched by design, an
unrecognised market returns the full name, and the nickname-collision branch
returns market and nickname together — and the first of those is the common
one, not the third.

Reapportioning the row was measured and rejected before a third tier was
added. The score column has **7 real pixels** of slack against a shortfall
that runs from 9 to 64 pixels depending on the name, so widening the name
column buys back the narrowest case and nothing else.

At 9pt every realistic name clears the column: the widest, `@ Tampa Bay
Buccaneer` at the 20-character wire cap, measures 217px and leaves 19. **The
9pt tier fires only after 12pt has been measured and rejected**, which is the
whole point of a ladder — a name that fits 12pt must never be drawn at 9pt,
because this card's argument is that the score reads from across the room and
9pt is a third the height of the top tier. Dropping two tiers is announced on
the stream rather than left to be noticed. Whether 9pt is legible enough at
room distance is the one part of this that needs glass; see section 9.

---

## 9. Verification

**There is no unit test harness in `App/`**, and this change does not invent
one. `App/` is an Arduino sketch compiled for an ESP32 and every function
touched here either draws on a real panel or reads a real clock; there is no
host build, no test runner and no seam to stand a fake `LGFX` behind. Saying so
is more useful than a test that only proves the code compiles.

So the checks are:

1. `arduino-cli compile` for `App`, clean, with the binary under the 2,097,152
   byte `ota_0` partition. As built on 2026-09-25: **1,510,864 bytes**, no
   warnings, against 1,508,976 before this change, so the routine and every
   converted call site together cost 1,888 bytes of flash and left 586,288
   bytes of headroom. CAL is rebuilt in the same pass because its own
   `Display.cpp` carries the corrected wrap: **1,349,664 bytes**, also no
   warnings, against the 1,703,936-byte (`0x1A0000`) factory partition, so
   354,272 bytes of headroom there. The one symbol that change adds,
   `Journal::printf`, is defined in `Journal.cpp` in the same sketch and
   `CAL.ino` already links against it.
2. The two CI gates in section 7, which fail the build on a new unbounded
   `drawString` and on any of the retired helper names reappearing.
3. **On hardware, and only on hardware:**
   - The sports card shows `Houston Astros` whole at the smaller tier, or
     `Houston Ast...` if it still will not fit, and never `Houston Astr`.
   - The away row reads `@ Houston Astros` and the home row has no marker.
     **Superseded 2026-09-27** — see section 8a. The away row now reads
     `Houston Astros` with no marker and sits on top; the **home** row carries
     the `@` and sits underneath.
   - A 21:40 Eastern game reads `21:40`, or `9:40 PM` in a 12-hour household,
     and not `01:40`. The debug stream carries
     `[sports] start time: utc=... offset=...min 12hour=... -> ...` on every
     draw, which is enough to check the conversion off the stream without
     standing in front of the panel.
   - The notice card's first line breaks after `rotation`, with `has` starting
     line two and no characters missing.

Point 3 is the one that needs a device. The first two are what this change can
prove on its own.

---

### 9a. The 2026-09-27 card-audit build

A second pass over the same routine, from `CARD_AUDIT_2026_09_27.md`. What is
new here as a *method* is that the widths below are **measured, not
estimated**: `LGFXBase::text_width()` was reimplemented against the vendored
LovyanGFX glyph tables, and the reimplementation reproduces **all eleven**
width figures previously recorded by hand in this repository's own source
comments — `Updated just now` at 150px, `Updated 365 days ago` at 190,
`99.9 mi away` at 109, `HALFTIME` at 124, `311 deg NW` at 140 and the rest —
exactly, to the pixel. It is still a model of LovyanGFX, and one flashed
device converts every figure below from modelled to observed.

**The content budget moved inside `layoutText()`.** `setContentBudget()` had
existed since the home value card lost its compliance line under a button
row, and exactly one card of fourteen consulted it. Rather than teach the
other thirteen, one comparison now runs where every bounded string already
passes — which is only possible *because* of section 7's gate: the rule that
every non-literal string in `App/Display.cpp` goes through this routine is
what makes this a genuine choke point rather than a place most strings visit.
A gate written to stop a sixth truncation helper turns out to be what makes
the geometry measurable.

It names the **box**, not the card — `[display] listings.onmarket drew to
y=184, past its 154 budget` — because `noteContentOverrun()`'s card name says
which card to go and read, where the `what` string says which row to move.
`Log::printf`, not `verbose`, since it only fires when something is wrong.

One exemption: `drawOneButton()` draws its label through the same routine at
roughly y=181 *by design*, so a file-static flag is set around
`drawActionButtons()`. Nothing else in the file draws chrome text through
`layoutText()` — the corner clock and the clock card's hero are
`LAYOUT-PRIMITIVE` and never reach it.

Two false positives were found and fixed rather than tolerated, because a
check that fires on a card that is not broken is a check people scroll past:
a stray `gContentBottom = 200` left at the end of `showSportsCard()`, and
`CardManager`'s early return to `showNoContent()` leaking the previous card's
budget. A third was subtler and is worth recording: the home value compliance
line reserved **18px** for a 9pt line whose `fontHeight()` is **22**, so it
had always reported a bottom four pixels past `contentBottom()`. It looked
fine on glass because a 9pt line's ink is shorter than its advance. The
reservation is now 22 — reserving what the font advances rather than what the
ink happens to occupy.

**Widths settled in this pass**, all against the measured model:

| element | before | after |
|---|---|---|
| sports name | 2 tiers, 12pt floor; `@ Tampa Bay Buccaneer` cut at 292px | 3 tiers, 9pt floor at 217px of 236 — 19px spare |
| `homevalue.range` | 150px shared column; **shortest possible range already 11px over** | own 80/210 split; fits to a $10M low / $100M high |
| `homevalue.estimate` | 150px shared column | own 116/174 split (label measures 112) |
| `listings.address` | one 12pt line, 300px; **0 of 118 live addresses fit** | two 9pt lines, 523px of 600 at the widest |
| `listings` "Listed N days" | `Listed 3234 days` | `On market 8.8 years`, label 89px of 150 |

Two of those are worth stating as findings rather than as fixes. The home
value range's **shortest** producible value, `$400K - $450K`, is 161px in a
150px column — so that row had been drawing `$400K -...`, a range with no
upper bound, since it was written. And the listings address did not fit for
**any** of the 118 live listings; dropping `, STATE ZIP` server-side was
measured before being rejected, since 92 of the 118 still overflow at 12pt
without it.

The listings card also gained the reserve-first tight layout
`showHomeValueCard()` already had. Four stat rows plus a footer do not fit
above y=154, and y=154 is the floor on **every device in this fleet** whenever
an action is bound, because all nine share one account and the `Request Tour`
binding is live. With a button bound the card now seats two stat rows at a
22px pitch and drops the other two with a line naming each; without one it
seats all four at 26px and lands within a pixel of where it always did.

**As built, 2026-09-27.** `ci/build-firmware.sh` end to end, **no warnings and
no errors**, all five gates green before either compile started.

| image | before | after | delta | real ceiling | headroom |
|---|---|---|---|---|---|
| `App.ino.bin` | 1,515,808 | **1,517,776** | +1,968 | 2,097,152 (`ota_0`) | 579,376 (72% used) |
| `CAL.ino.bin` | 1,349,664 | **1,349,664** | 0 | 1,703,936 (`factory`) | 354,272 (79% used) |

Both memory lines, as arduino-cli printed them — and note that its "Maximum is
1966080" is the FQBN's generic `min_spiffs` scheme, not either real ceiling
above, which is the whole reason the script re-checks against `partitions.csv`
itself:

```
CAL: Sketch uses 1349523 bytes (68%) of program storage space. Maximum is 1966080 bytes.
     Global variables use 52124 bytes (15%) of dynamic memory, leaving 275556 bytes for
     local variables. Maximum is 327680 bytes.
App: Sketch uses 1517619 bytes (77%) of program storage space. Maximum is 1966080 bytes.
     Global variables use 81980 bytes (25%) of dynamic memory, leaving 245700 bytes for
     local variables. Maximum is 327680 bytes.
```

CAL is byte-for-byte the same **size** because nothing under `App/` is compiled
into it and its own sources were not touched; its image hash still moves,
because the ESP32 app descriptor embeds a compile timestamp. The bootloader and
partition images hash identically to the previous build, which is the check
that nothing structural moved.

The binding ceiling remains the **field** one, not the table in
`partitions.csv`: CAL against the 1,441,792-byte factory partition the fleet
is actually running is at **93%**, with 92,128 bytes spare. That number did
not move in this pass and no change here touches CAL, but it is the one to
watch.

**Still needs a device, and nothing here can prove it:**

- **Whether 9pt is legible across a room.** This is the open question from the
  audit's section 7 and it is a judgement, not a measurement: the sports
  card's whole argument is that the score reads from a distance, and a 9pt
  name is a third the height of the 18pt tier. The alternative is an
  ellipsis, and the ellipsis is still the backstop below 9pt.
- **That two 9pt address lines look like a headline** rather than like body
  text that lost its heading.
- **Every MODELLED width above.** One flashed device with a bound button
  settles all of them at once, together with the budget check: the stream
  should fall silent on the cards that now fit, and name a box and a y on the
  ones that still do not.
- **That the `@` is now on the row a household reads as the host.** The
  arithmetic is not in question; the convention is, and it was wrong once
  already in exactly this file.

---

### 9b. The 2026-09-28 pass: the four remaining truncations

Section 9a left four strings measured and unfixed. This is those four, plus
one redraw bug that came in from a photograph rather than from the audit.

**The measurements were redone, not inherited.** `LGFXBase::text_width()` was
reimplemented a fourth time, independently of the three the audit used, by
reading `LGFXBase.cpp:2184` and the vendored `FreeSansBold9pt7b.h` /
`FreeSansBold12pt7b.h` glyph tables directly. At `size_x == 1` every `>>16` in
that routine is the identity, so the whole of it is: substitute `0x20` for any
code outside the face, then per character `right = left + max(xAdvance, width
+ xOffset)` and `left += xAdvance`, with the one special case that a first
glyph with a negative `xOffset` starts both cursors at `-xOffset`.

It reproduces **every** figure already written into this repository by hand -
`Est. value` at 112px, `Sunrise` at 89, `Sunset` at 81, `$400K - $450K` at
161, the whole all-ones range table from 167 to 223, `$9999K - $99999K` at
200, and the widest live listings address at 523 - to the pixel, and it agrees
with the audit's own numbers for all four strings below. `fontHeight()` comes
out at 22 for 9pt and 29 for 12pt, which is what the cards already assume.

That is a fourth agreeing model of LovyanGFX. It is still a model.

#### `homevalue.compliance` - two lines

`Automated estimate, not an appraisal.` is **323px** at 9pt in a 300px box, so
it has been drawing as `Automated estimate, not an...` - the one line on the
card that `Display.h` says may not go missing, missing the word it exists to
say. There is no smaller face to drop to; `Font0` is the 6x8 bitmap that
`drawClock()`'s own remarks record as unreadable on this panel.

A shorter sentence was measured before being rejected, not instead of:

| wording | width | verdict |
|---|---|---|
| `Automated estimate, not an appraisal.` | 323 | over |
| `Automated estimate, not an appraisal` | 319 | over |
| `Automated estimate. Not appraised.` | 306 | over |
| `Automated estimate, not appraised.` | 303 | over |
| `Automated estimate, not appraisal.` | 297 | fits by 3px, reads as a typo |
| `Automated value, not an appraisal.` | 296 | fits by 4px, "value" is weaker |
| `Model estimate, not an appraisal.` | 282 | drops "automated" |
| `Auto estimate, not an appraisal.` | 270 | reads like car insurance |
| `Estimate only, not an appraisal.` | 268 | drops "automated" |

Nothing that keeps both halves of the claim fits with margin worth having.
`HomeValueModels.cs` calls the distinction "a compliance requirement from the
product owner, **not wording**", so the sentence *could* have moved - but a
second line costs 22px on a card that can find 22px, and rewording a legal
line to save pixels is a trade worth not making while it is avoidable. It
wraps to `Automated estimate, not an` (235px) and `appraisal.` (83px), and
cannot vary, because it is a literal.

The 22px came out of the detail block, which now has room for one line
instead of two. That costs nothing measurable: `$275/sq ft - updated Sep 25`
is 232px and the deliberately absurd `$11,111/sq ft - updated Sep 11` is 256,
both inside one 300px line. The second detail line was headroom for a string
`HomeValue.cpp` does not build.

#### `homevalue.address` - two 9pt lines, and the block below moves 7px

Both live addresses overflow: `300 Eatons Landing Dr, Annapolis, MD 21401` is
**518px** at 12pt and **384px** at 9pt, `104 Virginia Ave, Edgewater, MD
21037` is **449** and **332**. One line does not work at either size, so this
takes the listings card's answer unchanged - two 9pt lines at an 18px step -
rather than inventing a second way to lay out a postal address.

Two 9pt lines advance 36 against the single 12pt line's 29, so the cost is
**7px** and everything below moves down by exactly that: `firstRowY` 62 to 69,
`secondRowY` 100 to 107, the detail block 138 to 145. The pitch and the gaps
are unchanged, which is the point - there were only 3px of slack at 62, so
absorbing it was never on the table.

**With a button bound the address is dropped entirely**, which is this card's
own long-standing sentence ("the only element here that a reader can do
without") acted on rather than only written down. The arithmetic forces it: a
154px floor, a compliance line now reserving 44, and 26 + 36 + 29 + 29 reaches
120 against a compliance line that must start at 110. The tight path loses the
refresh date for the same reason. The card then draws the shape it already
draws for a household with no stored address.

#### `aircraft.route` - shortened on the device

The cause is server-side and is worth stating precisely, because the fix that
matters is not in this repository. `AirportNameFormatter.Shorten` strips a
**trailing** `" International Airport"`. Every name long enough to matter does
not end there:

```
Minneapolis-Saint Paul International Airport (Wold-Chamberlain Field), MN
Charleston International Airport / Charleston Air Force Base, SC
Athens International Airport (Eleftherios Venizelos Airport), GR
```

so the rule fires on the short names and on none of the long ones.

Measured over the **948 distinct origin/destination display pairs** the route
cache can currently produce (1,394 cached routes, less the three-leg ones
`FlightRouteService.SplitRoute` refuses, joined against the 255-row airport
name cache with the ICAO code as the fallback the card actually uses):

| | widest route line | overrun a 2 x 300px box |
|---|---|---|
| before | 1,077px | 110 of 948 |
| after | 710px | 54 of 948 |

`shortenAirportName()` in `App/Display.cpp` does what the server's own two
rules would do if they could reach: split off a one-to-three character
`", XX"` suffix, drop one parenthetical, drop an alternate name after `" / "`,
then `" International Airport"` to `" Intl"` **anywhere** and a bare trailing
`" Airport"` entirely, and put the suffix back. The 54 that remain are names
with nothing left to strip - `Ronald Reagan Washington National, VA`,
`Hartsfield-Jackson Atlanta Intl, GA` - and they now lose a word off a
destination rather than the destination.

**A third line was measured and refused.** The stat rows and the freshness
line already reach y=234 against a 220 budget whenever the route takes two
lines, so an 18px third line would put this card 32px under the corner clock.
That existing 14px overrun is fixed in the same pass by closing the stat-row
pitch from 30 to 25 **only** when the route took two lines, which lands the
freshness line at 197..219 - one pixel inside. The one-line case, which is
what every device draws today, keeps 30 and does not move at all.

**What the server should still do**, and this is the half that belongs
upstream: apply `Shorten`'s two rules to the whole string rather than to its
tail, and drop a parenthetical and a `" / "` alternate before formatting. Then
the device pays neither the bytes nor the per-draw work, and the raw name is
still available in `provider_airport_name_cache` for a diagnostic. The device
rule stays regardless - it is the only half that can ship without the wire
moving, and a card that fits whatever it is sent is the better shape anyway.

#### `qr.caption` - 9pt

At 12pt the 300px box held about twenty-seven ordinary characters. The seeded
`Scan for our latest listings` is **304px**, `Book a tour of this property` is
**320**, `Scan me for more information` is **346**. At 9pt the same three are
**226**, **237** and **256**.

This card's own comment already says the caption is "supplementary, not the
point" - the code is the point - so the font is the thing to spend. **Nothing
else on the card moves**: the 22px step this layout was already carrying as a
number of its own is exactly `FreeSansBold9pt7b`'s `fontHeight()`, where at
12pt it was several pixels short of the font's advance. The layout has stopped
disagreeing with the face drawn into it.

Not a guarantee, and deliberately not dressed as one: `CardSpec::text` holds
280 characters and an operator can type a paragraph. A long caption still
ellipsizes and `layoutText()` still says so.

#### And one redraw bug, from a photograph

`CardManager::rewind()` at cursor 0 was the one navigation path that returned
without repainting. `Display::flashNavEdge()` erases a 106px edge strip as
press feedback and redraws only the chevron, so that path left a blank column
down the side of the card until the next auto-advance. `flashNavEdge()`'s own
comment had predicted exactly this path and exactly this consequence. It is
one `drawCurrent()` call. Not a layout change, and recorded here because it
came out of the same photograph pass as the rest.

**As built, 2026-09-28.** All five gates in `ci/build-firmware.sh` green, run
by hand before the compile started; **no warnings and no errors** anywhere in
a 467-line verbose log, checked by reading it rather than by the exit code.

| image | before | after | delta | real ceiling | headroom |
|---|---|---|---|---|---|
| `App.ino.bin` | 1,517,776 | **1,518,128** | +352 | 2,097,152 (`ota_0`) | 579,024 (72% used) |
| `CAL.ino.bin` | 1,349,664 | 1,349,664 | 0 | 1,703,936 (`factory`) | 354,272 (79% used) |

Both memory lines, as arduino-cli printed them — and its "Maximum is 1966080"
is the FQBN's generic `min_spiffs` scheme, not either real ceiling above,
which is why the script re-checks against `partitions.csv` itself:

```
App: Sketch uses 1517971 bytes (77%) of program storage space. Maximum is 1966080 bytes.
     Global variables use 81980 bytes (25%) of dynamic memory, leaving 245700 bytes for
     local variables. Maximum is 327680 bytes.
```

**Globals did not move at all** — 81,980 before and after. That is the number
worth watching on this fleet, and it says what these changes are: the whole
+352 bytes is flash, mostly `shortenAirportName()`'s code and the string
literals it matches against. The 256-byte route buffer is stack, not `.bss`,
and it replaces three heap `String` temporaries per draw of the aircraft
card, so the largest-contiguous-block figure that decides whether TLS can
open should move the right way rather than the wrong one.

CAL was **not** rebuilt: nothing under `App/` is compiled into it and no
root-level source changed in this pass, so its 2026-09-27 artifact is the
current one and all three of its ceiling checks were run against it. The
binding ceiling remains the **field** factory partition of 1,441,792 bytes
that the fleet is actually running, where CAL sits at 93% with 92,128 spare.
The two sketches' `partitions.bin` images are byte-identical, which is the
check that the table did not move.

The four ceiling checks, run by hand:

```
CAL (factory)                            1349664 / 1703936 (79%)  spare 354272  PASS
App (ota_0)                              1518128 / 2097152 (72%)  spare 579024  PASS
CAL (must also fit ota_0, OTA staging)   1349664 / 2097152 (64%)  spare 747488  PASS
CAL (field factory, pre-re-table)        1349664 / 1441792 (93%)  spare  92128  PASS
```

**Still needs a device:**

- **That two 9pt lines still read as a headline** on the home value card, the
  same open question the listings card left.
- **That the compliance line on two lines still reads as one sentence** rather
  than as a line and an orphan.
- **That `Minneapolis-Saint Paul Intl, MN` is recognisably the same airport**
  as what the server sent. The arithmetic is not in question; whether a
  household reads the shortened name as the right place is.
- **That the aircraft card's two-line-route mode at a 25px pitch still reads
  as three rows**, and not as a crowded block.
- **The aircraft card under a bound button is still broken and was left so.**
  Its freshness line lands at y=194 against a 154 floor whenever an action is
  bound, which is the audit's own step 13 - a layout that needs redesign, not
  a shifted y. The budget check reports it, correctly, and nothing here
  silences it.

---

## 10. One fitting rule instead of fifteen judgements, 2026-10-16

Fifteen labels truncate in the fleet's own logs. Counted over the window the
owner pulled:

| label | times | label | times |
|---|---|---|---|
| `board.status` | 135 | `sunmoon.sunset` | 35 |
| `listings.distance` | 99 | `tides.low` | 34 |
| `listings.updated` | 99 | `moon.heading` | 34 |
| `board.home` | 73 | `moon.illuminated` | 34 |
| `moon.upcoming.name` | 68 | `moon.phase` | 34 |
| `forecast.condition` | 65 | `tides.high` | 34 |
| `sunmoon.sunrise` | 35 | `featuredlistings.title` | 21 |
| | | `board.age` | 21 |

Fifteen separate column-width arguments is fifteen chances to measure one of
them against a panel it isn't drawn on, which is how `listings.distance` got
30px on device 17 after being measured correctly at 110. So this is one rule in
`layoutLine()` that every call site inherits, the way the budget check in
section 3 is one comparison for all fourteen cards.

**The rule.** Try the caller's own face on one line. Then each smaller face in
the same family on one line. Then two lines, if the caller declared room for a
second and the string has somewhere to break. Ellipsize only when all of those
have failed, and ellipsize at the caller's own face so the row still reads as a
row.

Nothing here stops drawing the ellipsis. Section 5's third case is unchanged
and so is its reasoning: the mark goes inside the box on purpose, and taking it
away without making the text fit would turn a loss a household can see into one
it can't.

### 10a. The ladder is the four faces already in the binary

`FreeSansBold9pt7b`, `12pt`, `18pt` and `24pt`, whose `fontHeight()` values are
22, 29, 42 and 56. A tier is never taller than the face above it, so a step down
can't reach anything below the line, which is what makes this step safe to
apply everywhere without asking.

Nothing follows the 9pt face. Section 9b already settled that question for
`homevalue.compliance`: `Font0` is the 6x8 bitmap `drawClock()` records as
unreadable at arm's length, and text that has dropped to 9pt and still doesn't
fit has run out of family.

A single line that lands on a smaller face is centred in the slot the caller
asked for, and `TextResult::bottom` still reports the slot rather than the
shorter line, so anything stacking underneath is unaffected. A caller outside
the family, `Font0` or a face scaled with `setTextSize`, gets no ladder and
today's behaviour exactly.

### 10b. A second line needs room, and room is the caller's to declare

`layoutLine()` knows `x`, `y`, `width` and the font. It doesn't know what the
card will draw at `y + 44`, and on this panel the answer is usually
"something". Measured against the current code:

| label | its line box | what comes next | room |
|---|---|---|---|
| `sunmoon.sunrise` | 170..212 | detail at 202 | none, overlaps by 10 |
| `moon.phase` | 56..85 | illuminated at 78 | none, overlaps by 7 |
| `moon.upcoming.name` | 184..206 | the day count at 202 | none, overlaps by 4 |
| `moon.illuminated` | 78..96 | the next-event line at 100 | none |
| `board.home` | top+9..top+31 | the next row at top+35 | none landscape, 21 profile |
| `listings.distance` | the footer row | the content floor | none |
| `tides.high` | top+28..top+70 | the row's own bottom | none |

A `layoutLine()` wrapping on its own judgement would draw three of those
through the element below, which is worse than the ellipsis it replaced. So
vertical room is a new last argument, `roomBelow`, in pixels, defaulting to
zero. Zero means a one-line slot, which is what all 88 call sites mean today,
so no site changes behaviour until its geometry has been measured and written
down. Width already works exactly this way and nobody expects it to be guessed.

`moon.illuminated` was the one site with measured room, and it was wired. It no
longer is: the 22px under it went to the next moonrise or moonset at y=100 (see
section 12), and the room there was only ever spent on a profile panel the card
now lays out differently for. Every site keeps a one-line slot. That is a
smaller result than fifteen fixes and it is the size the panel actually has.

### 10c. A word with nowhere to break is not wrapped, it's sawn in half

`Avalanche` is 90px of 9pt bold in an 86px column. Given two lines,
`layoutText()`'s section 5 rule hard-breaks it into `Avalanch` and `e`, because
a single word that doesn't fit has no break opportunity and rule 3 says nothing
may be dropped while there's somewhere to put it. That rule is right for a URL
under a QR code and wrong for a team name.

So `TextResult` gains `hardBroken`, set alongside `ellipsized`, and the
two-line attempt is accepted only when the string wrapped at a space. `100%
illuminated` takes the second line. `Avalanche` and `FINAL` keep the ellipsis.
The flag costs nothing, because the hard-break count was already being tracked
for the log line that reports it.

### 10d. What the rule reaches, measured

`LGFXBase::text_width()` at `size_x == 1`, reimplemented from the glyph tables
the way section 9b describes, reproducing its figures exactly: `Est. value` at
112, `Sunrise` at 89 and `$400K - $450K` at 161 at 12pt, and `Automated
estimate, not an appraisal.` at 323 and `Scan me for more information` at 256
at 9pt.

Most of these fifteen truncate on the profile panel rather than the landscape
one, which is what the ladder is good at, because profile is where a column
shrinks under a face that didn't:

| string | face, width | box | drops to |
|---|---|---|---|
| `12:51 PM` as `sunmoon.sunset` | 18pt, 149 | 110 profile | 12pt at 104, fits |
| `11:38 AM` as `tides.high` | 18pt, 149 | 120 profile | 12pt at 105, fits |
| `Patchy Fog then Sunny` | 12pt, 269 | 234 landscape | 9pt at 200, fits |
| `Waning Crescent` as `moon.phase` | 12pt, 199 | 132 profile | 9pt at 147, still over |
| `100% illuminated` | 9pt, 146 | 132 profile | two lines, fits |
| `Avalanche` as `board.home` | 9pt, 90 | 86 | nothing below 9pt, ellipsis |
| `Updated 365 days ago` | 9pt, 190 | 139 profile | no room, ellipsis |

`board.status`'s 135 is the largest single figure and the ladder doesn't touch
it, because it is 9pt already. `SPORTS_CARDS_DESIGN.md` section 11 is what
addressed that one, by giving the column 84px instead of 56 on a row whose
scores are blank: `10:00 PM` is 77 and `12:00 AM` is 78. A live row's `6TH INN`
was 74 against 56 and was cut.

**Both of those figures have moved since, and the 84 in that other
repository's document is two revisions behind.** Section 15a re-measured the
start-time column to 78 plus 2, and section 15h took the width off the status
string instead, so `6TH INN` gets 76 and there is no fixed figure in that column
to be behind. The two sentences above stand as where the period's truncation was
first written down, which is what 15g cites them for.

### 10e. What this leaves

Seven of the fifteen still ellipsize, and they are the ones already at 9pt in a
column with nothing under it: `listings.distance`, `listings.updated`,
`board.home`, `board.age`, `moon.upcoming.name`, `moon.heading`, and
`featuredlistings.title`, which goes through `layoutText()` with its own box
rather than through `layoutLine()` and so inherits nothing here.

There is one lever left and it isn't in this change. `FreeSans9pt7b` is the
same family at the same size in regular weight, and the library already carries
it. It runs 6 to 11 percent narrower:

| string | bold | regular | its box |
|---|---|---|---|
| `Avalanche` | 90 | 84 | 86, fits |
| `First Qtr` | 71 | 65 | 55 profile, no |
| `100% illuminated` | 146 | 137 | 132 profile, no |
| `Updated 365 days ago` | 190 | 181 | 139 profile, no |

It buys `board.home` and little else, it puts a weight change in the middle of
a row, and it adds a face to a binary already at 79% of its partition. Adding
it is one line in the tier table whenever somebody wants to make that trade. It
is not being made here.

Nothing here has been on hardware. The arithmetic is a fifth agreeing model of
LovyanGFX and still a model. What a device has to settle is whether a 12pt
sunrise beside a 9pt label still reads as the hero of its row, and whether
`100%` over `illuminated` reads as one fact or two.

## 11. Three of these were a layout, not a fitting problem, 2026-10-25

`moon.heading`, `moon.phase`, `moon.illuminated` and `moon.upcoming.name` were
all in the list above because the moon card put them in columns measured
against a 320px panel: a heading beside a clock, a 132px text column beside a
disc, and four 55px columns across the lower half. No fitting rule rescues a
68px string in a 55px column. The card lays out differently on a narrow panel
now — see `docs/design/sun-moon-redesign` — and on that panel these four each
get the full 220px band and none of them ellipsizes. The entries above still
describe the landscape card, where they still fit as recorded.

**And one of these was never a fitting problem at all.** The forecast card's
location row and its hero temperature collided: a 9pt line inks 18 rows, so a
location at y=28 inked down to y=45, and the hero icon started at 36 and the
24pt digits at 42. `forecast.location` never ellipsized on a landscape panel
and never reported anything; it simply printed through the number. "Severna
Park, MD" is 152px and reaches x=162, and the digits start at x=76.

A fitting ladder cannot see that, because nothing overflowed. What sees it is a
picture of the card, which is what `DAM_PANEL_SHOTS=1 dotnet test
tests/DiscoverAroundMe.DeviceSimulator.Tests` now writes for this card and the
moon card at both panel sizes. The hero is measured off the header's own ink
now: `kHeroInkTop = kLocationY + 18`.

## 12. The row under `moon.illuminated` went to a line, 2026-10-25

The landscape moon card now draws the next moonrise or moonset at y=100, in the
22px that section 10b measured as `moon.illuminated`'s room below. Nothing was
lost: the room was reserved for a second line of "100% illuminated" that a
320px panel never needs, because the column there is 212px and the string's
longest form is 146. It was measured when this code drew both panel sizes from
these constants, and the card has taken a separate branch for a 240px panel
since (`docs/design/sun-moon-redesign`), where that line gets the full content
width and does not wrap either.

So `moon.illuminated` goes back to a one-line slot, `moon.nextevent` takes
100..118, and `NEXT MAJOR PHASES` stays at 122 with the four upcoming phases
below it untouched. The widest string the new label draws is
`No moonrise or set`, at roughly 170px against the 212 it is given.
`No moonrise or moonset` was the first wording and it ellipsized in that
column, which the card's own shot caught.

## 13. The forecast header, the rule through the descenders, 2026-10-25

Two defects on one card, found by eye in the same pass and solved together
because neither has room to move without the other.

### 13a. The squeeze was costed against a floor one panel does not have

`contentIsTight()` answers "a button row is bound to this card", and the
forecast card read that as "rearrange the header": the freshness stamp moves up
onto the location row and the rule rises six pixels, which buys the compressed
strip 28px against a 154px floor. 154 is `kButtonRowY() - 6` on the 240px-high
landscape panel.

A panel standing on its end has a floor of **234**, and its strip is rows rather
than columns — rows that divide whatever the rule leaves. There is nothing for
the squeeze to buy there, and it was charging the location row 138 of the 220px
band to buy it: 41px left, so the shot showed `An...` where a town belongs.

So the squeeze is `tight && !stackedStrip` rather than `tight`. Measured on the
panel that now keeps its header whole: rule at 125, first row at 133, floor 234,
four rows of 25px against a 9pt line that inks 18.

### 13b. And where the squeeze is right, the row still did not fit

Landscape with a button bound leaves the location 121px after the stamp
reserves 179, and every town on this fleet is wider than that — `Annapolis, MD`
125, `Severna Park, MD` 152, `Fort Washington, MD` 182. The pair wants 361 of a
300px row and the card has no other 61 pixels.

`Updated ` is 73 of the stamp's 171 and is the one part of the row that says
nothing a reader standing in front of a town name and a time needs, so it is
what comes off. 288 of 300, with 12 over. On its own row the stamp keeps the
word, because that row has 300px for 171.

The row is then split by measuring both rather than by a fixed reservation.
Only a long town meeting a stale stamp runs it out, and then the stamp gives
first down to an 80px floor — the same direction the 2026-09-27 cap took, for
the same reason.

| panel, action bound | location box before | after | widest town |
|---|---|---|---|
| 320x240 | 121px | 198px | 182px, fits |
| 240x320 | 41px | 156px | 182px, Fort Washington still over |

Fort Washington on a turned panel with a button bound is the one case left, and
it ellipsizes at the same 156px the turned panel has always given the location
row without a button bound. That is the `kDayNightWidth` reservation, not the
squeeze.

### 13c. The rule had been drawn through the freshness line since the card was written

Both orientations, with no button bound, which is nine of nine fielded devices.
The stack is a 12pt condition phrase at y=80 inking to 103, a 9pt freshness line
at y=104 inking 104..121, and a strip label at 122. The rule at 118 crosses the
`p` and the `g` of `Updated 59 min ago`. There is no slack between those three
rows to move either of them into, which is why this needed 13a rather than a
shifted y.

**The slack is underneath.** The landscape strip bottoms at 199 against a 220
floor. So the rule goes to 125 — three clear of the freshness ink — and the
strip follows it down by eight: label 122 to 130, icon centre 155 to 163,
temperature 178 to 186, bottoming at 208 with twelve to spare. The strip's own
internal pitch does not move, so its three rows keep the spacing they were
drawn with. The stacked strip takes the same rule and divides what is left.

Squeezed, the rule stays at 112: the freshness line is up on the location row,
104..121 is empty, and 112 is the first line clearing the condition phrase.

### 13d. The faces are ASCII and four strings were not

`LGFXBase::text_width()` substitutes 0x20 for any code outside the vendored
tables, per byte, so a UTF-8 character draws as many blanks as it has bytes.
`SUN · TODAY`, `MOON · TODAY` and the sports card's `Q3 · 7:42` each lost their
U+00B7 to two spaces; the sun card's `↑ SUNRISE` and `↓ SUNSET` each lost three.
Every one of them is a vertical bar or nothing now — see
`docs/design/sun-moon-redesign/README.md` for the full count and the pictures.

**What this does not have is a check.** Nothing in either repository fails when
a non-ASCII byte reaches a face that cannot draw it; four of these shipped, and
the only reason they are fixed is that somebody looked at a photograph of the
moon card. A source scan over the string literals in `Display.cpp` would catch
the next one and has not been written.

---

## 14. The panel's text state belongs to whoever changed it, 2026-11-01

Everything above this section is about a string against a box. This one is about
the three pieces of panel state the box is drawn in, and about the fact that
LovyanGFX keeps all three until something sets them again. A card that leaves
one of them changed hands it to the next, unrelated screen.

### 14a. What the file already did, for two of the three

`restoreDefaultFont()` put the panel's own face and a top-left datum back at the
end of every card function, and its comment named the dependents: `showStatus()`
and `showFailure()` set no face of their own and draw in whatever is selected.
`layoutText()` does the same for the datum it sets, and says why - a leaked
`top_right` would range the next thing somebody adds to the right rather than
failing where it could be seen.

Text scale had neither half. Nothing restored it, and most draw functions set
`setTextSize(1)` at the top as though nothing would. Three functions select a
scale of 2:

| function | sets 2 at | returns at |
|---|---|---|
| `showStatus()` | the headline | **2, when `detail` is empty** |
| `showFailure()` | the headline | 1, set for `whatToDo` |
| `showClockDate()` | the hero | 1, set for the date line |

`showStatus()`'s second `setTextSize(1)` sits inside `if (detail.length() > 0)`,
and `detail` defaults to `""` in `Display.h`. Four call sites in `App.ino` pass
it empty, and `forceUpdateCheck()` is the one that reaches a card: it draws
`showStatus("Already up to date", "")`, waits 1,500 ms, and calls
`CardManager::redraw()`. Whichever card was showing is redrawn with a scale of 2
still selected.

Three draw functions set no scale at all and so inherit that one:

| function | what it depends on | when it bites |
|---|---|---|
| `showBannerCard()` | the strip text's scale | always |
| `showSportsCard()` | the competition label, then the rows | when the server sends no sport, so no pill is drawn first |
| `showSportsBoard()` | the competition label, then the four rows | the same |

`drawSportsPill()` sets 1 before drawing its own label, which is why the two
sports screens are only wrong when the pill is absent. `showAnnouncementCard()`
and `showQrTextCard()` look like they belong on that list and do not:
`drawCardBanner()` runs first on both and sets 1. `drawActionButtons()` is
covered the same way by `drawOneButton()`.

On the board the effect is geometric rather than cosmetic. `kSansTiers` is read
by `FontId`, so `sansTierIndex()` cannot see a scale, and `layoutLine()`'s ladder
runs on a `Sans9` face whose `fontHeight()` is 44 instead of 22, inside a 35px
row. That is doubled glyphs in rows measured for single ones, which is a
scoreboard with its columns wrong.

### 14b. The contract

**A draw function leaves the panel's text state as it found it: the panel's own
face, a top-left datum, and a scale of 1.** `restoreDefaultFont()` is now
`restoreDefaultTextState()` and puts back all three, so the thirty call sites
that already asked for the first two get the third without moving. The two boot
screens, which call no card helper, call it themselves.

Restoring rather than requiring every function to set its own scale is the half
this file already had a mechanism for, and it is what makes the three inheritors
above correct rather than lucky. Text color is the one piece of state that works
the other way and needs nothing: `layoutText()` sets it on every call, and the
five raw `drawString()` sites set their own.

CAL's root `Display.cpp` has none of this to fix. Every string there goes through
`centeredText()`, `wrappedCenteredText()` or a screen's own `button`/`label`
lambda, and each of those sets color, scale and datum before it draws.

### 14c. The gate

`ci/check-text-state.sh`, called from `ci/build-firmware.sh` beside the two
greps in section 7:

> No function in `App/Display.cpp` may return with a text scale other than 1
> selected.

That is the property. The script enforces it with two rules, neither of which
is a control-flow model, and the wording above is earned only by having both.

**Rule 1, the restore is not inside a branch.** A function that sets a scale
other than 1 must put 1 back, by `lcd.setTextSize(1);` or
`restoreDefaultTextState();`, at the function's own two-space indentation,
after the last place it set the other scale. `showStatus()` did restore the
scale, on the path where `detail` was non-empty, and a gate that accepted a
`setTextSize(1)` anywhere in the body would have passed it. Two-space
indentation is what "not inside a branch" looks like in this file.

**Rule 2, no early return while a scale is selected.** Reading a function top to
bottom, no `return` may stand between a line that selects a scale other than 1
and the next line that puts 1 back.

Rule 1 on its own is weaker than the property, and that gap was measured rather
than reasoned about: it compares two line numbers, so a function that sets 2,
takes an early return, and restores at the bottom has a restore on the larger
line number and passes. That shape is the original `showStatus()` defect in a
different arrangement, and it is a path out with scale 2 selected. Rule 2 is
what closes it.

The file passes rule 2 unchanged, and why is worth saying rather than leaving at
"it passes". Three functions select a scale other than 1 - `showStatus()`,
`showFailure()` and `showClockDate()` - and none of the three contains a
`return` at all, so each reaches its restore on every path.
`showMoonPhaseCard()` has the shape that looks like the interesting case, a
`restoreDefaultTextState()` at `App/Display.cpp:3965` directly above an early
`return`, but it never selects a scale other than 1, so rule 2's flag is never
set inside it.

**Both rules were proven to bite.** Rule 1 by deleting the restore from
`showStatus()` and confirming the script names that function and exits 1. Rule 2
by putting `if (detail.length() == 0) return;` into `showStatus()` above the
restore, which rule 1 passes and rule 2 names. TEST_PLAN.md section 17a records
the branches and the runs.

It is a grep and two `awk`s, nothing cleverer, for the reason section 7 gives:
it does not know what the panel looks like, it knows the difference between a
function that put the scale back and one that did not.

### 14d. What this does not cover

- **Nothing here has been on glass.** The defect was read out of the source and
  the fix is a source change; no device has been flashed and no photograph of a
  scoreboard taken before or after. The owner's photograph of wrong columns is
  consistent with this and is not proof of it, since the geometry fix that
  shipped separately is also in that frame.
- **Neither rule models control flow, and a branch defeats rule 2.** A restore
  inside an `if` clears rule 2's flag for every `return` after it, so

  ```cpp
  lcd.setTextSize(2);
  if (a) { restoreDefaultTextState(); }
  if (b) return;                        // reachable with 2 selected when !a
  lcd.setTextSize(1);
  ```

  passes both rules and is wrong. Rule 1 passes it too, because the restore at
  the bottom is on the larger line number. Catching it needs the branch
  structure, and an `awk` over 6,700 lines is the wrong tool for that. No
  function in the file has this shape today.
- **The host tests cannot reach this.** There are four host targets now -
  `ci/test-recovery-menu.sh`, `ci/test-loader-order.sh`,
  `ci/test-config-request.sh` and `ci/test-card-columns.sh` - and none of them
  compiles `Display.cpp`. `Display.cpp` names LovyanGFX in one block and could
  be driven through its own `Canvas` interface by a fake, which would turn
  section 14a's table into assertions. That has not been written. Section 15d
  records what did come out of that file into a header a host compiler can
  build.
- **The face and the datum are not gated**, only the scale. The private helpers
  that make up a card - `drawCardBanner()`, `drawProseBody()`, `drawSportsPill()`,
  `drawTideRow()`, `drawOneButton()` - select a face mid-card by design, so a
  gate on those would need an allowlist of its own and would fire on code that
  is right.

---

## 15. The scoreboard's status column, and the width it lost shipping, 2026-11-01

`showSportsBoard()`. Four rows, each one two team names, two scores and a state.
The state is `FINAL` or a period on a row that has scores and a start time on the
one row that does not, and those two are not the same width.

### 15a. What shipped, and what it cost

Until `57df3d8` the five column edges were literals offset from `kCardMargin`,
and the status width was computed per state:

```cpp
const int statusWidth =
    scoreless ? statusRight - (homeScoreRight - kScoreWidth) : kStatusWidth;
```

That expression existed for a measured reason, in the comment beside it: a start
time is about 72px of `FreeSansBold9pt7b` against the 56px the column holds on a
row with scores, so a scoreless row took the home score's column and the gap
beside it, 84px.

**The 72 is an estimate, it is 6px low, and section 10d of this document already
had the real figure.** 10d reads "`10:00 PM` is 77 and `12:00 AM` is 78", from
the audit that built section 9b's method. Section 15 was written against the
comment's "about 72px" instead, three hundred lines below a measurement of the
same string in the same face.

Re-measured here off the vendored `FreeSansBold9pt7b` glyph table by 9b's
method, to confirm 10d rather than to replace it: the widest string
`formatTimeOfDay()` can produce is 78px, which is a two-digit hour with `AM`
rather than `PM`, because `A` advances 13 and `P` advances 12. The method
reproduces every 12pt figure written into this document - `Sunrise` 89, `Sunset`
81, `Est. value` 112, `$400K - $450K` 161, `$9999K - $99999K` 200 - to the
pixel. That is a fifth agreeing model of `LGFXBase::text_width()`, and it is
still a model.

The 80 this change picked clears 78, so nothing is broken by the estimate. What
changed is the arithmetic behind it: `kBoardStartTimeWidth` is now
`kBoardStartTimeTextWidth + kBoardStartTimeSlack`, 78 plus 2, and the slack is
2px on a measured figure rather than the 8 an estimate was carrying.

`57df3d8` replaced the literals with widths derived from the panel, which the
240px panel genuinely needed: `homeScoreRight` was a flat 248 there, eight pixels
past the glass, and the status width that subtracted it came out at 4. In the
same diff the per-state status width became `const int statusWidth =
kStatusWidth;` and the 52px the two empty score columns gave up went to the names
instead. On the landscape panel the scoreless status column went from 84 to 56.

78 into 56 does not go, and nothing catches it. The status draws at
`FontId::Sans9`, which is the last entry in `kSansTiers`, so `layoutLine()`'s
ladder has no smaller face to drop to; `roomBelow` is 0, so it cannot take a
second line either. It falls through to `layoutText()` in the 56px box it was
asked for and ellipsizes, which is `10:00...` on the glass. That is the state the
replaced expression was written to fix, and it shipped in `v2026.10.31.0002`.

A second figure moved in the same diff. A row that **has** scores had an 86px
name column at every panel width; derived, it is 83 on the 320px panel and 43 on
the 240px one. The 240px figure is not a like-for-like loss, since that panel's
old layout drew off the glass, but the 320px one is: the release note's own
example, the Blackhawks reading as `Blackha...`, is a row with scores, and that
row lost three pixels on the panel the note describes.

### 15b. One scale, and the scores as its only input

`boardColumns(screenW, scoreless)` returns the five edges and the three widths.
`scoreless` is `row.awayScore[0] == '\0' && row.homeScore[0] == '\0'`, and it is
the only input the layout takes beyond the panel width, because it is where the
pixels come from: two empty score columns release what the status column spends.

**It is not the same thing as "the status holds a start time", and this section
said it was.** Section 15f has the two directions that come apart.

**Section 15h is what the draw path does now**, and the status widths in this
section's table are no longer the ones a row gets: the column asks for what its
own string measures and `scoreless` decides only the score columns and the
ceiling. The two-argument `boardColumns(screenW, scoreless)` below is still in
the header, still returns the figures in this table, and is what the original
fourteen assertions and the host test read, so the table stays as the state's
default width. 15h has the measured before-and-after.

```cpp
const int scoreWidth  = scoreless ? 0 : kBoardScoreWidth;    // 26
const int statusWidth = scoreless ? kBoardStartTimeWidth     // 80
                                  : kBoardStatusWidth;       // 56
const int fixedWidth  = 2 * scoreWidth + statusWidth + 3 * kBoardColumnGap;
const int shared      = (rowRight - rowLeft - fixedWidth) / 2;
```

The status width is **charged** to `fixedWidth` as well as drawn. That is the line
that was missing: `57df3d8` charged a flat 56 and then let the names spend the
52px the score columns released, so a scoreless row's names and its status column
were both laying claim to the same pixels.

80 rather than 78 because two pixels is the slack on a measured figure, not
headroom for a longer string. 15a has where the 78 comes from.

Measured at both panel widths, against both versions it replaces:

| panel | state | column | before `57df3d8` | `v2026.10.31.0002` | this change |
|---|---|---|---|---|---|
| 320 | scoreless | name | 86 | 109 | 97 |
| 320 | scoreless | status | 84 | **56** | 80 |
| 320 | scored | name | 86 | 83 | 83 |
| 320 | scored | status | 56 | 56 | 56 |
| 240 | scoreless | name | 86, off the glass | 69 | 57 |
| 240 | scoreless | status | **4** | 56 | 80 |
| 240 | scored | name | 86, off the glass | 43 | 43 |
| 240 | scored | status | 56 | 56 | 56 |

### 15c. Two trades, named rather than buried

**A row with scores keeps 83px names, not the 86 it had before `57df3d8`.** Those
three pixels, and nine more, are the separation between the home score's right
edge and the status column's left edge. Substituting `shared` back gives
`homeScoreRight = rowRight - statusWidth - 2 * kBoardColumnGap`, so that
separation is 12px for any status width, score width and panel width, plus the
one pixel the even split can't divide when there is one. It lands in this gap
because nothing else claims it.

**Which pixel that is depends on the status width, not on the panel width, and
this section said it was the panel's.** The leftover is
`rowRight - rowLeft - fixedWidth`, so the parity that reaches the gap is that
expression's, and the panel-width phrasing was only true while every status
width in the file was even: 80 and 56 are, and both shipped panels are, so the
two parities agreed everywhere anybody had looked. Section 15h hands the column
a width measured off a string, and `6TH INN` at 76 and `4TH QTR` at 81 are not
both even. The identity the host test checks is
`2 * kBoardColumnGap + (rowRight - rowLeft - fixedWidth) % 2`, which reduces to
the panel's own parity at the two default widths and is what the four
`static_assert`s on the separation read as a floor rather than as an equality.

The old literal layout spent 2px there: a right-aligned `7` two pixels from a
right-aligned `FINAL` reads as one token. Charging three gaps rather than two
would hand the names back exactly the 86 they had and leave 6px between those two
columns, and 83 with 12px of air is the better row.

**A scoreless row on the 240px panel keeps 57px names, down from 69.** That is
what the status column's 24 extra pixels cost, split two ways. A name ellipsized
to `Lightn...` is still the team; a start time ellipsized to `10:00...` is not a
time. The 320px panel pays 12 of the same pixels, 109 to 97, and 97 still clears
the 86 that shipped before the rewrite.

### 15d. The properties a build checks

`boardColumns()` is `constexpr`, so the figures above are not a table somebody
has to re-derive. Fourteen `static_assert`s hold them at both shipped panel
widths, in every build, on both boards:

- The scoreless status column is at least `kBoardStartTimeTextWidth`, at 320 and
  at 240. This is the live regression, stated as a property.
- The scored status column is at least `kBoardStateWordWidth`, the 55px `FINAL`
  measures, at both widths.
- A row with scores keeps the 83px and 43px name columns `v2026.10.31.0002`
  shipped, so the next change to these constants cannot quietly shrink them
  further.
- The separation is at least `2 * kBoardColumnGap` in all four cases.
- The status column's right edge is inside the content band at both widths,
  which is where the literals put 248 on a 240px panel.
- The `kBoardMinNameWidth` floor is clear at both widths. It has to be: the floor
  clamps the name width and nothing clamps the edges derived from it, so below
  174px scoreless and 202px with scores the separation stops holding. No shipped
  panel is that narrow, which makes it latent rather than live.

**None of the fourteen will say anything when a third board arrives.** They
evaluate the literals 320 and 240 and nothing else, so a 272px panel compiles
them all clean whatever it does to the row. That claim was in this section and it
was wrong. The sixteen 15h adds read the same two literals and have the same
limit. `tests/card-columns-test.cpp` is what covers a width nobody has built:
it walks every width from 28 to 1024 in both states, so a third board's figure is
already measured before the board exists.

`boardColumns()` and the constants it reads live in `App/CardColumns.h`, where
the assertions live with them, thirty of them since 15h. The header names no
graphics library, no font and no Arduino type, which is the whole reason it is
a header: `Display.cpp` pulls in LovyanGFX and a board pin map, so while these
figures were in that file they could only be compiled on the toolchain for one
of the two boards.
`kCardMargin` moved with them, because it is the figure the row is derived from
and a second copy of it is the thing that drifts.

### 15e. And the properties a host run checks

`ci/test-card-columns.sh` compiles that header with `g++` and runs
`tests/card-columns-test.cpp`. It is not a replacement for the assertions, which
run in every board build and need no host compiler. It is the four things an
assertion at a fixed width can't say:

- **The table in 15b, as equalities.** The assertions hold the name columns at or
  above what `v2026.10.31.0002` shipped, which stops them shrinking. The host run
  holds them at 97, 83, 57 and 43 exactly, so the table above is checked rather
  than re-derived.
- **Every width from 28 to 1024, both states.** The row's two ends are the same
  distance in from the glass, the two blocks are the same size and have a gap
  between them, the edges run left to right, the name column never goes under the
  floor, and the row never asks for more pixels than it has. The separation
  identity is checked as an identity at each of them, which is where the
  thirteenth pixel in 15c came from.
- **The boundaries around the floor, measured rather than named.** The separation
  stops being `2 * kBoardColumnGap` below 174 scoreless and 202 with scores. The
  first width whose name column is clear of the floor is 176 and 204, two wider
  in each case, because 49 pixels of leftover still split into a 24px name. The
  two columns actually meet at 162 and 190, and below those they're drawn through
  each other. None of those six figures is a thing a `static_assert` at 320 and
  240 could have reported.
- **Whether a column keeps the slack it is written to carry.** The assertion
  holds the start-time column at the 78px the widest start time measures. The
  host run holds it at 78 plus the 2px of slack, which is the difference between
  a column sized against a measurement and one trimmed down to it: a status
  width of 79 clears the assertion and fails here.

TEST_PLAN.md section 18 has the mutations both halves were proven against.

### 15f. `scoreless` is not "the status holds a start time"

The layout keys on whether both score strings are empty. `Sports.h` defaults
both scores to `kNoScore` and `writeScore()` writes `""` for that, so a state
the server sends no score with is scoreless whatever its status holds. Only one
of the five wants an 80px column:

| state | status | measured | column it got | column it gets, since 15h |
|---|---|---|---|---|
| Scheduled | a start time | up to 78 | 80, which is right | 80 |
| Postponed | `PPD` | 37 | 80, and both names pay 12px each on the 320 panel | 56 |
| Unknown | `""` | 0 | 80, for nothing | 0 |
| Final with no score sent | `FINAL` | 55 | 80 | 56 |
| Live with no score sent | `LIVE` or a period | 41 to 112 | 80 | 56, or the period plus 2 |

The status widths above are measured. Which of those states actually arrives
without scores is the server's choice and nothing here reads it; Postponed and
Unknown are the two that plainly have no score to send.

And the other direction, which costs more. `Sports.cpp`'s `writeScore()` writes
each side independently from its own `kNoScore` sentinel, so a scheduled game
with one side's score reported is **not** scoreless. Its status is still a start
time, and it is handed the 56px column, which is the regression 15a is about,
arriving through a different door. Nothing has been seen doing this, and it needs
only a provider that reports a 0 for a game that has not started.

**What it should key on is the status string's own state.** Two shapes would do
it. One: `SportsBoardRow` carries the flag, set by `Sports.cpp` where the state
enum still exists, and `boardColumns()` takes that instead of `scoreless`. The
pixels would still come from the empty score columns, so a row would have to
report both. Two: the status column asks for what its own string measures,
clamped between `kBoardStateWordWidth` and `kBoardStartTimeWidth`, which needs a
`textWidth()` call on the draw path and takes the figure out of a `constexpr`
and out of reach of both the assertions and the host test.

**The second is written, in section 15h**, and three things about that
description turned out differently. The clamp's floor is `kBoardStatusWidth`,
56, not `kBoardStateWordWidth`'s 55, because a string that fits the column the
fleet already draws should not move it; an empty status is a third case below
both and gets nothing. The ceiling is not `kBoardStartTimeWidth` either: a
ceiling of 80 would still cut `HALFTIME`, which is the half of the defect 15g is
about, so it is derived from a name floor instead. And the figure did not leave
`constexpr`. Only the `textWidth()` call is on the draw path, and what it hands
over is one int, so `boardStatusWidthFor()` and the layout it feeds are both
still read by the assertions and by the host test. 15h has why the flag shape
was not taken and what the measured column does to both name columns.

**Four of the five rows above move**, and three of the four improve. A postponed
row's `PPD` and a finished row's `FINAL` ask for 56 rather than taking 80, which
hands the 320px panel's names back the 109 they had before the start-time column
was widened; a row whose state the server did not send asks for nothing at all,
because nothing is drawn there, and its names get the whole row. The fifth, a
live row with no score sent, is the one that can go either way: `LIVE` at 41
gives the names 109, and a period over 78px takes them below the 97 they have
now. The other direction in the paragraph above - a start time on a row that
reported one score - gets its 80px at both panel widths.

### 15g. The 56px column does not hold a period, and a live row draws one

**Section 10d already said this**, in one sentence at the end of its table: "A
live row's `6TH INN` was 74 against 56 and was cut." Section 15 then went on
to describe the 56px column as the width that holds `FINAL` or a period. The
figures, re-measured on the same glyph table by 15a's method:

| string | measured | fits 56 |
|---|---|---|
| `PPD` | 37 | yes |
| `LIVE` | 41 | yes |
| `90+3'` | 46 | yes |
| `FINAL` | 55 | yes, by 1px |
| `6TH INN` | 74 | no |
| `4TH QTR` | 79 | no |
| `3RD QTR` | 80 | no |
| `HALFTIME` | 94 | no |
| `2ND PERIOD` | 112 | no, and cannot arrive - see below |

A live game's status is `game.period`, the provider's own progress text, and
`SportsBoardRow::kMaxStatus` holds 15 characters of it. A live game normally has
scores, so it takes the 56px column, and the status draws at `FontId::Sans9`
with `roomBelow` 0, so there is no smaller face and no second line and it
ellipsizes. `6TH
INN` reads as `6TH...` on the glass. That is the same failure 15a is about, on
the other branch of the same ternary, and `6TH INN` is the example this document
and `App/Display.h:470` both use for what the field holds.

**It was live, and section 15h is the fix.** Widening the scored column takes
pixels off two name columns that a release note was written about, and the width
it needs depends on a string the server sends rather than on anything in this
repository, so it is the measure-and-clamp shape in 15f rather than a new
constant. The pass that wrote this section added tests and changed nothing the
panel draws: `App/CardColumns.h` said the 56 was sized against a state word and
not against a period, `tests/card-columns-test.cpp` held it at or above the 55px
`FINAL` measures, and no assertion claimed it held a period. 15h is where the
column stops being 56 at all, and the table above is the set of strings it is
measured against.

**`2ND PERIOD` is in the table above and cannot reach the panel.** It is ten
characters. `Sports::kMaxPeriodLength` is 8, and `CheckIn.cpp` copies the
provider's period in through `copyBoundedMarked()`, which writes `...` into the
tail of what it keeps. So a server sending `2ND PERIOD` puts `2ND P...` in the
row, and that measures 66px, which fits the 56px column no better but is a
different figure and a different string. The widest period the storage can hold
is eight characters: `HALFTIME` fills it exactly at 94px, `OVERTIME` is 97, and
the widest eight characters the glyph table has at all is `@@@@@@@@` at 144.
Section 15h is sized against the names rather than against any of those.

### 15h. The status column asks for what its own string measures, 2026-11-01

Both doors in 15f and 15g are the same door: the column's width is picked from
the row's score strings, and what has to fit it is the status string. So the
status string is what it's picked from now.

```cpp
const int statusWidth = boardStatusWidthFor(kScreenW, scoreless, lcd.textWidth(row.status));
const BoardColumns columns = boardColumns(kScreenW, scoreless, statusWidth);
```

`boardStatusWidthFor()` is three rules and no table:

- An empty status asks for nothing. Nothing is drawn there, and the two names
  split the lot.
- A status that already fits the 56px column keeps the 56px column. `FINAL`,
  `PPD`, `LIVE` and every short period cost the names not one pixel, so a scored
  row looks exactly as `v2026.10.31.0002` drew it unless its status didn't fit.
- Anything wider asks for its own measured width plus the 2px of slack, up to a
  ceiling.

`scoreless` is still an argument, and it still means both score strings are
empty. It no longer picks the status width. It picks the two score columns, and
through them the ceiling, because a row that isn't spending 52px on scores can
spend more of the row on its status.

#### The ceiling is a name floor, and the name floor is a judgement

`kBoardNameFloor` is 31, and the ceiling is whatever status width leaves both
name columns at or above it:

```cpp
rowSpan - 2 * scoreWidth - 3 * kBoardColumnGap - 2 * kBoardNameFloor
```

| panel | state | ceiling |
|---|---|---|
| 320 | scoreless | 212 |
| 320 | with scores | 160 |
| 240 | scoreless | 132 |
| 240 | with scores | 80 |

**31 is a choice about which column pays, not a measurement, and it's the one
figure in this section that isn't measured.** Two things fix it. It's 7px clear
of `kBoardMinNameWidth`, where the layout's own clamp engages and 15c's
separation stops holding. And it's the largest floor that still lets the 240px
panel hand a start time the 80px a start time measures: at 32 the ceiling there
is 78 and `12:00 AM` is cut again, which is the regression 15a is about. 15c's
reason for spending names on the status applies to both doors. A name
ellipsized to `Lightn...` is still the team. A start time ellipsized to
`10:00...` is not a time, and `6TH...` is not a period.

Nothing above the floor bounds the column, which means no figure in the file
guesses at the provider's vocabulary. The widest eight characters the glyph
table holds is 144px and the floor is what stops it, rather than a constant
somebody picked by reading a list of sports.

#### Why not the flag on `SportsBoardRow`

Shape one in 15f sets a flag in `Sports.cpp`, where the state enum still exists,
and the layout keys on that. It fixes 15f's door and not 15g's: a live game with
scores has no spare score columns, so a flag saying "this status is a period"
can only move pixels off the names, and how many it should move depends on which
period the provider sent. The flag would have to carry a width, which is the
measurement again, taken one tier further from the glyph table. It also adds a
field to a struct `Sports.cpp` and `Display.cpp` share, and the measured shape
adds nothing to either.

Nothing on the wire changes. `textWidth()` reads a string the row already
carries, and `Sports::Game` and `SportsBoardRow` are both byte-for-byte what
they were, which is the cost the flag shape would have paid and this one does
not. What does change across the fleet during a rollout is that the same payload
lays out differently on either side of it: a longer period widens the column on
a device that has this and ellipsizes on one that has not.

#### What the measurement depends on

`LGFXBase::text_width()` multiplies its answer by the panel's current text scale,
so a row measured under a scale of 2 would ask for twice the pixels. The ceiling
caps what it can get, so the row still draws inside the glass, but the names
would pay for nothing. This is the first thing in this file where that scale
changes a layout rather than only a glyph size, and section 14 is what stands
behind it: 14b is the contract that every draw function puts 1 back, and 14d is
clear that `ci/check-text-state.sh` reads indentation and early returns rather
than control flow, so it is a check and not a proof. What the gate does not read
at all is the face, so `showSportsBoard()` selects `FontId::Sans9` before
measuring rather than relying on what the previous row left behind.

#### What a row draws, before and after

Measured off the vendored `FreeSansBold9pt7b` glyph table by 15a's method,
which reproduces all five of this document's 12pt figures and all eight of
15g's 9pt ones to the pixel. "Before" is `v2026.10.31.0002` and
`v2026.11.01.0001`, which draw these rows identically.

| panel | state | status | measured | name before | status before | name after | status after |
|---|---|---|---|---|---|---|---|
| 320 | with scores | nothing sent | 0 | 83 | 56 | 111 | 0 |
| 320 | with scores | `PPD` | 37 | 83 | 56 | 83 | 56 |
| 320 | with scores | `FINAL` | 55 | 83 | 56 | 83 | 56 |
| 320 | with scores | `7:30 PM` | 67 | 83 | 56 cut | **76** | 69 |
| 320 | with scores | `6TH INN` | 74 | 83 | 56 cut | **73** | 76 |
| 320 | with scores | `12:00 AM` | 78 | 83 | 56 cut | **71** | 80 |
| 320 | with scores | `3RD QTR` | 80 | 83 | 56 cut | **70** | 82 |
| 320 | with scores | `HALFTIME` | 94 | 83 | 56 cut | **63** | 96 |
| 320 | scoreless | nothing sent | 0 | 97 | 80 | 137 | 0 |
| 320 | scoreless | `PPD` | 37 | 97 | 80 | 109 | 56 |
| 320 | scoreless | `12:00 AM` | 78 | 97 | 80 | 97 | 80 |
| 320 | scoreless | `3RD QTR` | 80 | 97 | 80 | **96** | 82 |
| 320 | scoreless | `HALFTIME` | 94 | 97 | 80 cut | **89** | 96 |
| 240 | with scores | nothing sent | 0 | 43 | 56 | 71 | 0 |
| 240 | with scores | `PPD` | 37 | 43 | 56 | 43 | 56 |
| 240 | with scores | `FINAL` | 55 | 43 | 56 | 43 | 56 |
| 240 | with scores | `7:30 PM` | 67 | 43 | 56 cut | **36** | 69 |
| 240 | with scores | `6TH INN` | 74 | 43 | 56 cut | **33** | 76 |
| 240 | with scores | `12:00 AM` | 78 | 43 | 56 cut | **31** | 80 |
| 240 | with scores | `3RD QTR` | 80 | 43 | 56 cut | **31** | 80 |
| 240 | with scores | `HALFTIME` | 94 | 43 | 56 cut | **31** | 80 cut |
| 240 | scoreless | nothing sent | 0 | 57 | 80 | 97 | 0 |
| 240 | scoreless | `PPD` | 37 | 57 | 80 | 69 | 56 |
| 240 | scoreless | `12:00 AM` | 78 | 57 | 80 | 57 | 80 |
| 240 | scoreless | `3RD QTR` | 80 | 57 | 80 | **56** | 82 |
| 240 | scoreless | `HALFTIME` | 94 | 57 | 80 cut | **49** | 96 |

Every figure in bold is a name column narrower than the row drew before. Every
one of them bought a status column that holds its string, except the last row
with scores on the 240px panel, where the 12 pixels bought a column that still
cuts `HALFTIME`. The rows in that panel's `12:00 AM`, `3RD QTR` and `HALFTIME`
lines all read 31 because 31 is the floor and 80 is the ceiling it sets.

#### What a reader loses, said rather than left to be found

**A name column goes below what `v2026.10.31.0002` draws on any row whose
status didn't fit its column.** The rule, rather than a range: the names split
whatever the row has left, so a name column is
`(rowBudget - statusWidth) / 2`, and `statusWidth` is the string's own width
plus 2 once it passes 56. The bounds that follow:

| panel | state | names while the status fits 56 | narrowest the ceiling allows | over the strings this document measures |
|---|---|---|---|---|
| 320 | with scores | 83, unchanged | 31 | 81 at a 57px status, 73 for `6TH INN`, 63 for `HALFTIME`, 61 for `OVERTIME` |
| 320 | scoreless | 109, up from 97 | 31 | 107, 99, 89, 87 for the same four |
| 240 | with scores | 43, unchanged | 31 | 41, 33, 31, 31 |
| 240 | scoreless | 69, up from 57 | 31 | 67, 59, 49, 47 |

The narrowest a name column can get is 31 at every panel width and in both
states, because that is what the ceiling is derived from, and reaching it needs a
status measuring 158px on a 320px row with scores - more than the 144 the widest
eight characters in the glyph table make. So 31 is the floor the arithmetic
holds and not a width anything the server can send will produce on the landscape
panel; on the 240px panel with scores, anything from 78px up lands on it.

The release note for `v2026.10.31.0002` was written about those name columns,
and its own example is a row with scores reading `Blackha...`; that row's status
is `FINAL`, so it draws at 83 exactly as it did, and only a game in progress
pays.

**`HALFTIME` is still cut on the 240px panel.** 94px needs a 96px column, the
ceiling there is 80, and reaching 96 would put the names at 23, under
`kBoardMinNameWidth`, where the clamp engages and the home block walks into the
status column. That row has 212px for two names, two scores and a status, and it
hasn't got the pixels. The 320px panel holds every period the eight-character
buffer can carry.

**Three of the five scoreless states get names back, and the fourth can go
either way.** 15f called the 80px column on a `PPD` or an empty status pixels
spent for nothing. They are not spent any more, so a `PPD` or a `FINAL` with no
score sent draws 109px names on the 320px panel against the 97 it draws now,
and an empty status draws 137. That 109 is exactly what `v2026.10.31.0002`
shipped, and above the 86 before it. The fifth state, a live game with no score
sent, is the one that pays: `LIVE` at 41px gets the 109 too, and a period over
78px goes below 97, down to 89 for `HALFTIME`.

**Four rows of one board no longer share a column grid.** `boardColumnsFor()` is
called inside the row loop, and every edge but the status column's right one is
derived from the name width, so a row's own status string moves where the home
name starts and where both score columns end. On a 320px panel with scores:

| a row whose status is | status column | name | home name starts at | home score ends at |
|---|---|---|---|---|
| nothing | 0 | 111 | 157 | 294 |
| `FINAL` | 56 | 83 | 129 | 238 |
| `6TH INN` | 76 | 73 | 119 | 218 |
| `HALFTIME` | 96 | 63 | 109 | 198 |

48px between the first and last home-name edge, 96px between the home-score
edges, on four rows of one card. **This was already true across `scoreless` and
it was invisible**, because the rows it separated were the rows with no score
digits to misalign: a fixture's 97px names and a finished game's 83 put their
score columns in different places, and the fixture drew nothing in either. A
live row has digits. A board of three finished games and one in progress now
draws two ragged columns of numbers, and a mixed board is the ordinary case for
`sportsscores` rather than an edge.

The status column's own right edge is the same on every row, which is the edge
the status is aligned to, so the one column a reader scans down the right of the
card still lines up. Nothing else does, and nothing has looked at it. A property
cannot catch it either: `boardColumnsFor()` takes one row's state, and the host
test holds the four grids above as figures rather than asserting they agree.

#### What a build and a host run check

`boardColumnsFor(screenW, scoreless, statusWidth)` and `boardStatusWidthFor()`
are both `constexpr`. Only the `textWidth()` call is on the draw path, and what
it hands over is one int. `boardColumns(screenW, scoreless)` still returns the
state's default width and is what all fourteen existing assertions and every
existing case in `tests/card-columns-test.cpp` read, so none of the figures in
15b through 15e moved; 15c's parity paragraph and 15d's assertion count are the
prose that did.

`boardColumnsFor` is a separate name rather than a two-argument overload's
sibling. An int converts to bool with no diagnostic, so
`boardColumns(240, measuredWidth)` written by somebody reaching for the measured
form would compile, discard the measurement and return the scoreless layout, and
every assertion here and every case in the host test would pass, because none of
them reads the draw path.

Sixteen new assertions, at both shipped widths:

- **Four:** a status at the 74px `6TH INN` measures gets exactly 76, with scores
  and without. Equalities rather than `>=`, because `>=` at the two scoreless
  widths is satisfied by the 80px the shipped layout already handed those rows,
  so two of the four would have read as properties and bitten on nothing.
- **Two:** a status at the 78px widest start time gets a column that holds it on
  a row **with** scores, which is 15f's door stated as a property.
- **Two:** `FINAL` leaves the name column at the 83 and 43 the fleet draws.
  `PPD`'s 37 is in the host test's table rather than here, so there is one figure
  for the property and not two.
- **Four:** the ceilings, as the 212, 160, 132 and 80 in the table above.
  `>= kBoardStatusWidth` was what this bullet used to say and it is true for
  every input by `boardMaxStatusWidth()`'s own ternary, so it reported nothing
  about a panel width.
- **Four:** at the ceiling both name columns stand exactly on `kBoardNameFloor`.
  The tightest of the set, since a pixel either way fails.

All sixteen read the literals 320 and 240 and have the same limit 15d names for
the first fourteen: a third board compiles them clean.

And in the host run, which walks 28 to 1024 in both states twice:

- The three rules, as the boundaries between them: 55 asks for 56, 56 asks for
  56, 57 asks for 59, and 0 asks for 0. The third rule reads as an ordered
  ladder: an empty status is the first case, so "a status that fits 56 keeps 56"
  is true for every width from 1 to 56 and not at 0.
- All twenty-six rows of the table above as equalities: twenty-two in one table
  of measured widths and the four "nothing sent" rows on their own. And the four
  grids in the per-row table, as the figures they land on.
- The generalized separation identity in 15c, at every width and at eight status
  widths, one of them always odd and one odd at every odd panel width, which is
  what found that the thirteenth pixel follows the row's leftover rather than the
  panel's.
- The name floor holds at every width above the ceiling's own clamp, and the
  column never exceeds its ceiling.
- `HALFTIME` on a 240px panel with scores is cut, as an assertion. The gap above
  is a measured figure rather than a sentence, and a later change that widens
  that column has to come here and say so.

TEST_PLAN.md section 18e has the mutations.
