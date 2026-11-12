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
`lcd.drawString(someServerString, x, y)` as the file stands, and the firmware has
no unit test harness, so it is checked the only way it can be, which is the same
way that script already checks that every public `Motion` entry point has a
caller.

The first gate, **amended 2026-11-03**:

> Every call of a method named `drawString` in `App/Display.cpp`, on any
> receiver, must either pass a string literal as its first argument, or sit on a
> line carrying the `LAYOUT-PRIMITIVE` sentinel comment. `LgfxCanvas`'s own
> hand-off to LovyanGFX is the one exception, and it appears exactly once. The
> panel object is named nowhere outside the driver block.

It read `lcd.drawString(` and nothing else until that date, which is a spelling
rather than a route. Adding `auto& panel = lcd;` and
`panel.drawString(whatToDo, 10, 200);` to `showFailure()` draws a server string
into no box, and the gate passed it with its census still reading five and no
figure printed.

The receiver can be called anything. Three things cannot be renamed away without
the gate saying so, and it now reads all three, prints each figure and floors
each one: the method name, because `Canvas` declares one text-drawing virtual;
the single hand-off inside `LgfxCanvas`; and the driver block, which is what
leaves `Canvas` as the only way a string reaches the glass from the rest of the
file. A pattern that stops matching now fails instead of finding nothing.
`TEST_PLAN.md` section 33 has the mutations and the figures.

A permit the compiler checks would be better, and it is not built: `drawString()`
private on `Canvas`, taking a tag type only the four sanctioned functions can
construct, so an unbounded draw fails to build under any receiver name. It would
not retire the gate, because `panelDevice` also carries LovyanGFX's own
`print()`, `setCursor()` and `drawCenterString()`, which a `drawString` permit
says nothing about and the driver-boundary reading is what holds. A second text
method added to `Canvas` is what neither the permit nor the gate would see.

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
line reserved **18px**, which is the face's `fontHeight()`, for a 9pt line
whose `yAdvance` is **22**, so it
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
with the audit's own numbers for all four strings below. `yAdvance` comes
out at 22 for 9pt and 29 for 12pt, which is the step the cards reserve;
`fontHeight()` is 18 and 23 for the same two faces.

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
number of its own is exactly `FreeSansBold9pt7b`'s `yAdvance`, where at
12pt it was several pixels short of the font's advance of 29. The layout has stopped
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
18, 23, 33 and 47 and whose `yAdvance` values are 22, 29, 42 and 56.
`layoutLine()` sizes its slot and each tier off `fontHeight()`; a card reserving
a line of its own reserves the advance. A tier is never taller than the face
above it on either measure, so a step down
can't reach anything below the line, which is what makes this step safe to
apply everywhere without asking.

`LGFXBase::fontHeight()` returns `_font_metrics.height`, which for an Adafruit
GFX face is the glyph bounding height across the range -
`lgfx_fonts.cpp`'s `GFXfont::getDefaultMetric()` sets it from the largest
above-baseline and below-baseline glyph offsets, and keeps `y_advance` as the
face's own `yAdvance` beside it. The two are never equal for these four faces,
and `fontHeight()` is the smaller.

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
runs on a `Sans9` face whose `fontHeight()` is 36 instead of 18, inside a 35px
row - `fontHeight()` multiplies by the text scale. That is doubled glyphs in
rows measured for single ones, which is a
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

> **Read 15a through 15j at a 26px score column and a 31px name floor.**
> Section 15k corrected those two constants to 28 and 29 on 2026-11-02 and the
> code carries the new figures. Rather than restate a dozen tables, the whole
> difference is this, and it is exact:
>
> - A one-line **name** column with scores is 2px narrower than any figure in
>   15a-15j: 83 is 81, 43 is 41, 36 is 34, 76 is 74, 63 is 61, 73 is 71, 71 is
>   69, 70 is 68, 111 is 109, 33 is 31.
> - `kBoardNameFloor` is 29 wherever a figure reads 31, including the name
>   columns that stand on it.
> - A **scoreless** name column, and every **status** column, is unchanged.
> - Every column **edge** is unchanged: `homeNameLeft` 157/129/119/109 and
>   `homeScoreRight` 294/238/218/198 all hold, because a name 2px narrower
>   beside a score 2px wider starts the home block in the same place.
> - The two scoreless ceilings are 216 and 136 where they read 212 and 132; the
>   two with scores, 160 and 80, are unchanged.
> - The four two-line ceilings are 137, 157, 217 and 237 where they read 135,
>   155, 215 and 235. Every two-line **name** figure is unchanged.
> - The separation stops being two gaps below 206px with scores where a figure
>   reads 202, and the columns cross below 194 where one reads 190. The
>   scoreless pair, 174 and 162, is unchanged.
> - A figure of 26 for the score column is 28, and `kBoardStackedScoreWidth`'s
>   20 is unchanged.
>
> 15k has the derivation, what each change costs, and the one name it newly
> cuts. The subsections whose own tables move carry a pointer to it as well.

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

**The 26 below is 28 now.** Section 15k measured two digits of `Sans12` at 28
where one of them is a `1`, so every "name" figure in this section's table that
has a score column in it is two pixels narrower than it reads here: 83 is 81 and
43 is 41. The scoreless rows are unaffected, because a scoreless row has no
score column. 15k has the trade.

```cpp
const int scoreWidth  = scoreless ? 0 : kBoardScoreWidth;    // 28
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
- A row with scores keeps the 81px and 41px name columns, so the next change to
  these constants cannot shrink them further with nothing firing. Those two read
  83 and 43 until 15k spent two pixels of each on the score column.
- The separation is at least `2 * kBoardColumnGap` in all four cases.
- The status column's right edge is inside the content band at both widths,
  which is where the literals put 248 on a 240px panel.
- The `kBoardMinNameWidth` floor is clear at both widths. It has to be: the floor
  clamps the name width and nothing clamps the edges derived from it, so below
  174px scoreless and 206px with scores the separation stops holding. No shipped
  panel is that narrow, which makes it latent rather than live.

**None of the fourteen will say anything when a third board arrives.** They
evaluate the literals 320 and 240 and nothing else, so a 272px panel compiles
them all clean whatever it does to the row. That claim was in this section and it
was wrong. The eighteen 15h adds read the same two literals and have the same
limit. `tests/card-columns-test.cpp` is what covers a width nobody has built:
it walks every width from 28 to 1024 in both states, so a third board's figure is
already measured before the board exists.

`boardColumns()` and the constants it reads live in `App/CardColumns.h`, where
the assertions live with them: fourteen here, eighteen more in 15h and nine more
in 15i, forty-one in all. The nine read the same two literals as the rest and
carry the same limit this section gives for the first fourteen. The header names no
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
  above a floor, which stops them shrinking. The host run holds them at 97, 81,
  57 and 41 exactly, so the table above is checked rather than re-derived. The
  two "with scores" figures were 83 and 43 before 15k.
- **Every width from 28 to 1024, both states.** The row's two ends are the same
  distance in from the glass, the two blocks are the same size and have a gap
  between them, the edges run left to right, the name column never goes under the
  floor, and the row never asks for more pixels than it has. The separation
  identity is checked as an identity at each of them, which is where the
  thirteenth pixel in 15c came from.
- **The boundaries around the floor, measured rather than named.** The separation
  stops being `2 * kBoardColumnGap` below 174 scoreless and 206 with scores. The
  first width whose name column is clear of the floor is 176 and 208, two wider
  in each case, because 49 pixels of leftover still split into a 24px name. The
  two columns actually meet at 162 and 194, and below those they're drawn through
  each other. None of those six figures is a thing a `static_assert` at 320 and
  240 could have reported. The three "with scores" figures were 202, 204 and 190
  before 15k widened the score column.
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
const BoardColumns columns = boardColumnsFor(kScreenW, scoreless, statusWidth);
```

`boardStatusWidthFor()` is three rules and no table:

- An empty status asks for nothing. Nothing is drawn there, and the two names
  split the lot.
- Otherwise, a status that fits the 56px column keeps the 56px column. `FINAL`,
  `PPD`, `LIVE` and every short period cost the names not one pixel, so a row
  with scores and a state word in it draws exactly as `v2026.10.31.0002` drew
  it, in all eight of its figures.
- Anything wider asks for its own measured width plus the 2px of slack, up to a
  ceiling.

The three read in that order and the first one owns zero, so the second is true
of every width from 1 to 56 and not of 0: an empty status also "fits" 56 and
still gets nothing.

`scoreless` is still an argument, and it still means both score strings are
empty. It no longer picks the status width. It picks the two score columns, and
through them the ceiling, because a row that isn't spending 56px on scores can
spend more of the row on its status. 52 is what that pair cost at the 26px
column; 15k has it at 28.

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
is 78 and `12:00 AM` is cut again, which is the regression 15a is about.

**The second of those two is why the floor is 29 now.** It's a derivation with
the score width inside it, and 15k corrected the score width to 28, which makes
29 the largest floor that still hands a start time its 80px. The four ceilings
above are then 216, 160, 136 and 80, and the clear of `kBoardMinNameWidth` is
5px rather than 7. 15c's
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
15g's 9pt ones to the pixel.

**"Before" is `v2026.11.01.0001`**, which is what the fleet draws. For the
thirteen rows **with scores** it is also `v2026.10.31.0002`, which drew those
identically; for the thirteen scoreless rows it is not, because 15b's own table
records `v2026.10.31.0002` charging a flat 56 there and drawing 109px names at
320 and 69 at 240. Those two figures come back in the "after" column, which is
what the last paragraph of this section is about.

**Read the thirteen "with scores" rows two pixels wide.** Section 15k widened
the score column from 26 to 28 and re-derived `kBoardNameFloor` from 31 to 29,
so every name in those rows is two narrower than it reads here and the three
that stand on the floor read 29 rather than 31. The thirteen scoreless rows and
every status column in the table are unchanged, and so is every edge: a name two
narrower beside a score two wider starts the home block in the same place. 15k
has the table and what it costs.

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

Section 15k moves the "with scores" rows of this table too. Read 83 as 81 and 43
as 41, the floor as 29 in all four rows, and each of the eight "with scores"
per-string figures two narrower, down to 29 where the old figure was 31. The
"scoreless" rows are the figures they are. The two widths at which a name first
stands on the floor do not move: 157px of status on the 320px panel and 77 on
the 240px one, because the score column and the floor each gave up two pixels
and the name takes half of both.

| panel | state | names while the status fits 56 | narrowest the ceiling allows | over the strings this document measures |
|---|---|---|---|---|
| 320 | with scores | 83, unchanged | 31 | 81 at a 57px status, 73 for `6TH INN`, 63 for `HALFTIME`, 61 for `OVERTIME` |
| 320 | scoreless | 109, up from 97 | 31 | 107, 99, 89, 87 for the same four |
| 240 | with scores | 43, unchanged | 31 | 41, 33, 31, 31 |
| 240 | scoreless | 69, up from 57 | 31 | 67, 59, 49, 47 |

The narrowest a name column gets is `kBoardNameFloor`, wherever the ceiling is
the one the names set. That was 31 when this was written and 15k re-derived it
to 29. On the 320px panel with scores that needs a status measuring 157px,
which is more than the 144 the widest eight characters in the glyph table make,
so nothing the server can send reaches it there. On the 240px panel with scores
anything from 77px up lands on it, which is most of the periods in 15g.

Below 216px with scores and 160px scoreless the ceiling is the
`kBoardStatusWidth` clamp instead, and the name floor stops being the floor: a
200px panel with scores draws 24px names, which is `kBoardMinNameWidth` and not
this section's figure. The scoreless width was 164 at the floor of 31 these
paragraphs were written against; 216 is unchanged, because the score column and
the floor moved by the same two pixels in opposite directions. No board is that narrow. The host test guards the equality
with `ceiling > kBoardStatusWidth` for exactly that reason.

The release note for `v2026.10.31.0002` was written about those name columns,
and its own example is a row with scores reading `Blackha...`; that row's status
is `FINAL`, so it draws at 83 exactly as it did, and only a game in progress
pays.

**`HALFTIME` is still cut on the 240px panel.** 94px needs a 96px column, the
ceiling there is 80, and reaching 96 would put the names at 24, which is
`kBoardMinNameWidth`, where the clamp engages and the separation falls under
15c's 12. Not an overlap: at the 28px score column 15k settled on, the home
block and the status column touch at a panel width of 234 and cross below it, so
a 240px panel asking for 96 draws a row that is wrong by 6px of air rather than
one drawn through itself. At the 26px column these paragraphs were written
against those two figures were 230 and 2px. That row
has 212px for two names, two scores and a status, and it hasn't got the pixels. The 320px panel holds every period the eight-character
buffer can carry.

**Three of the five scoreless states get names back, and the fourth can go
either way.** 15f called the 80px column on a `PPD` or an empty status pixels
spent for nothing. They are not spent any more, so a `PPD` or a `FINAL` with no
score sent draws 109px names on the 320px panel against the 97 it draws now,
and an empty status draws 137. That 109 is exactly what `v2026.10.31.0002`
shipped. The fifth state, a live game with no score
sent, is the one that pays: `LIVE` at 41px gets the 109 too, and a period over
78px goes below 97, down to 89 for `HALFTIME`.

**Four rows of one board no longer share a column grid, and section 15i is what
put them back on one.** The rest of this subsection is what 15h shipped and what
15i was written from; read it in the past tense. `boardColumnsFor()` was called
inside the row loop, and every edge but the status column's right one is derived
from the name width, so a row's own status string moved where the home name
started and where both score columns ended. On a 320px panel with scores:

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
live row has digits. A board of three finished games and one in progress drew
two ragged columns of numbers, and a mixed board is the ordinary case for
`sportsscores` rather than an edge.

The status column's own right edge was the same on every row, which is the edge
the status is aligned to, so the one column a reader scans down the right of the
card still lined up. Nothing else did, and nothing had looked at it. No property
here could catch it either: `boardColumnsFor()` takes one row's state, and the
host test held the four grids above as figures rather than asserting they agree.

**The four grids in the table above are what one board picks the widest status
of now**, rather than what one board draws at once. The figures stand as what a
row asks for on its own, and the host test has a block that does assert the
board's answer against them. What the board lands on is not always one of the
four, because `scoreless` became the board's question: 15i's photographed 240px
board lands on a 36px name while its four rows would each have asked for 43, 43,
43 and 62.

#### One more figure the row did not derive

`showSportsBoard()` drew both score columns at the `kBoardScoreWidth` constant
rather than at `columns.scoreWidth`, which is the same 26 on a row with scores
and 0 on a row without. On every row the fleet draws that is the same figure, so
nothing moves; what it was, was the one width in the loop that did not come from
the row, which is how a block laid out for no score column could still have 26px
of digits drawn into it, through a team name. It reads `columns.scoreWidth` now.
An empty score string draws nothing at either width, so a scoreless row is
unchanged.

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

Eighteen new assertions, at both shipped widths:

- **Four:** a status at the 74px `6TH INN` measures gets exactly 76, with scores
  and without. Equalities rather than `>=`, because `>=` at the two scoreless
  widths is satisfied by the 80px the shipped layout already handed those rows,
  so two of the four would have read as properties and bitten on nothing.
- **Two:** a status at the 78px widest start time gets a column that holds it on
  a row **with** scores, which is 15f's door stated as a property.
- **Two:** `FINAL` leaves the name column at 81 and 41, which is where a status
  that fits the 56px column leaves it. `PPD`'s 37 is in the host test's table
  rather than here, so there is one figure for the property and not two. Those
  two read 83 and 43 before 15k.
- **Four:** the ceilings, as 216, 160, 136 and 80. `>= kBoardStatusWidth` was
  what this bullet used to say and it is true for every input by
  `boardMaxStatusWidth()`'s own ternary, so it reported nothing about a panel
  width. The two scoreless figures read 212 and 132 before 15k re-derived the
  name floor.
- **Four:** at the ceiling both name columns stand exactly on `kBoardNameFloor`.
  The tightest of the set, since a pixel either way fails.
- **Two:** the 76 and the 80 again, written as literals. The four equalities
  above carry `kBoardStatusSlack` on both sides and move with it, so a slack of
  0 cleared all of them and only the host run read it.

All eighteen read the literals 320 and 240 and have the same limit 15d names for
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
  widths, one of them always odd and one, the ceiling, odd at every odd width the
  names set it at. That is what found that the thirteenth pixel follows the row's
  leftover rather than the panel's.
- The name floor holds at every width above the ceiling's own clamp, and the
  column never exceeds its ceiling.
- `HALFTIME` on a 240px panel with scores is cut, as an assertion. The gap above
  is a measured figure rather than a sentence, and a later change that widens
  that column has to come here and say so.

TEST_PLAN.md section 18e has the mutations.

### 15i. Four rows of one board are one table, 2026-11-01

A photograph of a portrait CYD running `v2026.10.30.0001` came back with four
things wrong on one NHL board. Two of them the geometry rewrite in `57df3d8`
already fixed, and the two it did not are the grid the four rows sit on and the
navigation chevron drawn across two of their cards. Those two are this section.

**What the photograph shows, and what `main` already does with it.** The device
is eight releases behind, so most of what is in the frame is the five literal
column edges `f2ae104` wrote and `57df3d8` deleted, on a panel they were never
measured for.

`homeScoreRight` was a flat 248 on a 240px panel, eight pixels past the glass.
A two-digit score at `Sans12` is 26px right-aligned on that, so it ran 221 to
248 and the glass cut it at 239; a one-digit score ran 235 to 248 and lost
better than half. That is the clipped digit on the top row. Which score it is
is the second thing the photograph gets to say and the code does not: away is
drawn first and home second, and `awayScoreRight` was 126, 114 pixels inside the
glass. The clipped digit is a **home** score.

`homeNameLeft` was 136 with an 86px name column, so the home name's box ran 136
to 222 while the scored row's status box ran 170 to 226 and its score box 221
to 248. Three boxes over the same pixels. `Hurricanes` measures 94 and does not
overrun its box, because `layoutLine()` has no face below `Sans9` and no second
line and falls through to an ellipsis at 86px, ending at 222. It is the boxes
that overlapped, not the string: `FINAL` right-aligned on 226 occupies 171 to
226 and the ellipsized name is drawn through it. `Capitals` measures 70, fits
its box whole and ends at 206, and `P1` at 22px right-aligned on 226 starts at
204, so those two met with nothing between them for the same reason.

All of that is the panel width written down as literals, and `57df3d8` replaced
the literals with widths derived from it: on the 240px panel with scores the row
now runs 14, 83, 89, 158, 226 with a 43px name column and 12 pixels of air
between the home block and the status. None of it reaches `main`.

**The grid is live and `main` makes it worse.** 15h's own "Four rows of one
board no longer share a column grid" wrote it down: `boardColumnsFor()` is
called inside the row loop, so a row's own status string moves where its home
name starts and where both of its score columns end. The photographed board is
`FINAL` at 55px, `P1` at 22 twice and a `7:00 PM` start time at 67 on a row with
no scores, and on `main` at 240px that is three rows on one grid and the fourth
on another:

| row | status | measured | score columns | status column | name | home name at | home score ends at |
|---|---|---|---|---|---|---|---|
| `Jets` / `Hurricanes` | `FINAL` | 55 | 26 | 56 | 43 | 89 | 158 |
| `Wild` / `Capitals` | `P1` | 22 | 26 | 56 | 43 | 89 | 158 |
| `Mammoth` / `Flyers` | `P1` | 22 | 26 | 56 | 43 | 89 | 158 |
| `Oilers` / `Rangers` | `7:00 PM` | 67 | 0 | 69 | 62 | 82 | 144 |

7px between two home-name edges and 14px between two home-score edges, on four
rows of one card. A board of mixed states is what `sportsscores` sends on an
ordinary afternoon, not an edge.

#### One grid, folded before the loop

`showSportsBoard()` reduces the rows to two figures and lays the whole board on
one set of edges:

```cpp
const bool scoreless = boardHasNoScores(rows, count);
const BoardColumns columns =
    boardColumnsForBoard(kScreenW, scoreless, widestBoardStatus(rows, count));
```

`boardHasNoScores()` is false the moment any row reports either side.
`widestBoardStatus()` selects `FontId::Sans9` and keeps the largest
`textWidth(rows[i].status)`, because `textWidth()` reads whichever face is
current and the status draws in that one. Both run under the `kMaxRows` clamp,
over the rows that will be drawn: a fifth row the card drops would otherwise
widen a column nothing draws into.

`boardColumnsForBoard()` is `boardColumnsFor()` over `boardStatusWidthFor()`,
the same two calls 15h put in the loop, under one name so the draw path has one
thing to call and `ci/check-board-columns-call.sh` has one thing to read.

**The widest string, not the widest column.** `boardStatusWidthFor()` never
falls as its measurement rises: 0 asks for 0, 1 through 56 ask for 56, and above
that it is the measurement plus the slack under a ceiling that doesn't depend on
the measurement at all. So the widest column any row of the board asks for is
the column its widest string asks for, and the fold can run over the
measurements and convert once. The host sweep asserts the monotonicity rather
than leaving it as the reason this paragraph gives.

**One row with a score gives every row a score column.** `scoreless` means no
row of the board reported either side, which is a different question from the
one 15h's `scoreless` asked about a row. A board with one game under way draws
26px of score column on all four rows, and the three scheduled rows draw nothing
into theirs, because an empty string draws nothing. That is what a column is: a
scheduled row's blank cell under three rows of digits is the table working, and
a scheduled row with 26 extra pixels of name is the table coming apart.

#### What it costs, as the figures

One long status now costs every name on the board, where it used to cost one.
The floor it can drive them to is the same `kBoardNameFloor` a single row
already reached, so no name goes anywhere a row couldn't already go; what
changed is how many rows go there together. The photographed board at 240px, and
the worst case a provider can send at both widths:

Two pixels wide on every row of it, for 15k's reason: the score column is 28 and
not the 26 these were worked out at, so 36 is 34, 43 is 41, 76 is 74, 63 is 61
and the two rows on the floor read 29. Every status column here is unchanged.

| panel | board | widest status | status column | name per row | against 15h's per-row names |
|---|---|---|---|---|---|
| 240 | the photograph | `7:00 PM`, 67 | 69 | 36 | 43, 43, 43, 62 |
| 240 | four `FINAL`s | 55 | 56 | 43 | 43 unchanged |
| 240 | one `HALFTIME` | 94 | 80, cut | 31 | 43, 43, 43, 31 |
| 320 | the photograph | `7:00 PM`, 67 | 69 | 76 | 83, 83, 83, 102 |
| 320 | one `HALFTIME` | 94 | 96 | 63 | 83, 83, 83, 63 |

The 240px row of the photograph is the trade stated plainly: three names go from
43 to 36 so that four rows of digits line up. `Hurricanes` is 94px and was
already cut at 43, so what those seven pixels buy is the column, not a word.

**`HALFTIME` on a 240px panel is still cut**, for 15h's reason and no new one:
the ceiling there is 80 because 96 would put the names at 24 and the clamp
engages. It now cuts on a board rather than on a row, which is the same string
in the same column.

**Nothing on the wire changes.** `SportsBoardRow` and `Sports::Game` are
byte-for-byte what they were, the fold reads two fields the row already carries,
and a device without this draws the same payload on per-row grids. The 6-month
compatibility gate is untouched because no field was added, removed or
reinterpreted.

#### The nav chevrons stood on the card

The same frame shows the right-hand chevron crossing the third row's rounded
card. `drawNavAffordances()` drew its left tip at x=5 and its right at
`kScreenW - 6`, and the chevron is 7px wide, so the two bodies occupied 5..12 and
`kScreenW - 13`..`kScreenW - 6`. The content band starts at `kCardMargin`, 10,
and ends at `kScreenW - kCardMargin - 1`, so both chevrons reached 3px into the
band at either end.

On most cards nothing is drawn at the very edge of the band and the overlap was
invisible. `showSportsBoard()` fills a rounded rect across the whole band on
every row, so on that card the chevron is drawn onto the fill, in `muted()`
rather than in the card's own ink, and reads as the row overrunning itself. The
chevrons are centred on `kScreenH / 2`, which is 160 in profile, so they are on
the same two rows of every board rather than on a selected one. The body runs
`centre` plus or minus `kChevronHalfHeight`, 148 to 172, and the rows are 100 to
151 and 156 to 207, so four pixels of it sat on the second row's card as well as
on the third's. In landscape the body is 108 to 132 against rows 79 to 110 and
114 to 145, which is three pixels on the second and the rest on the third.

The inset is a named figure now, and the relationship is what a build checks:

```cpp
constexpr int kChevronInset = 2;
static_assert(kChevronInset + kChevronWidth < kCardMargin,
              "the chevron body stands in the margin, clear of any card");
```

2 is the largest inset that keeps the whole 7px body inside a 10px margin with a
pixel to spare at the glass. The left body runs 2..9 and the right
`kScreenW - 10`..`kScreenW - 3`, which is 230..237 on the portrait panel,
symmetric about the two margins, and the card runs 10..229. The chevrons sit
3px further out than they did and nothing else moves. Whether 2px of glass is
far enough in to clear the bezel on a cased device is the one thing here that a
photograph has to answer.

TEST_PLAN.md section 18f has the mutations and what they caught.

---

### 15j. Two lines and a brace, so a 240px panel can spell the team, 2026-11-01

A photograph of a portrait CYD running `v2026.11.01.0004` came back with five
of eight team names cut to an ellipsis on one NHL board: `Jets 0 Hur... 7
FINAL`, `Wild 5 Ca... 1 FINAL`, `Ma... 1 Fly... 5 P3`, `Oilers Rang... 6:00 PM`.
`Ma...` and `Ca...` carry no team at all.

Nothing is broken. 15i's grid is doing exactly what it says: four text columns
share the 212px a 240px panel has, the widest status on that board is the
`6:00 PM` start time at 67px rather than `FINAL`'s 55, the column it lands on
is 69, and what the two names split afterwards is 36px each. `Hurricanes` is
94. The row has the pixels it has.

The device in the frame is a release behind and draws the per-row grids 15i
replaced, which is why `Wild` and `Oilers` are whole in it. On `main` the same
board puts every row on the one 36px column, so seven of those eight names are
cut rather than five. 15i names that trade and takes it, to line the digits up.

The row also has height nobody is spending. `kRowHeight` is 56 in profile
against 35 in landscape, because the profile panel has 224px between the
heading and the age line and only four rows to put in it. The row draws one
22px line into all 56. Width is the binding constraint and height is the one
going spare, so the row turns one into the other: two lines, one team per line,
joined by a left brace that says they are one game.

#### What the row holds, per line

One name and one score, which is the whole of the width win. A one-line row
spends its 212px on two names, two scores and a status; a two-line row spends
it on a brace, one name, one score and a status, and the single name keeps
everything the second one was taking.

On the photographed board at 240px, whose widest status is the 67px start time:

| | brace | name | score | status |
|---|---|---|---|---|
| one line, on `main` | - | 36 each | 26 each | 69 |
| two lines | 8 | **97** | 20 | 69 |

`Jets` 36, `Hurricanes` 94, `Wild` 38, `Capitals` 70, `Mammoth` 86, `Flyers`
53, `Oilers` 51 and `Rangers` 72 all measure under 97. Every name on that
photograph is drawn whole, where `main` cuts seven of the eight. The edges are
brace 14..21, name 28..124, score right edge 145, status 157..226, and 12
pixels of separation, which is 15c's `2 * kBoardColumnGap` exactly and with no
leftover pixel to carry: one name rather than two means nothing is split in
half, so the parity term 15c had to chase doesn't arise.

A board whose statuses are all state words keeps the 56px column and gives the
name 110 instead, which is the `FINAL` row of the table further down.

#### The brace is drawn, because no face holds one this tall

LovyanGFX's `{` in `FreeSansBold9pt7b` is 4px wide and 17px tall, and 12pt's is
6 by 23 against a 44px block. A glyph can't span two lines, and scaling one
with `setTextSize()` would scale its stroke with it and put a text scale on the
panel that section 14's contract then has to put back.

So it's drawn, from the primitives `drawNavAffordances()` already uses for the
chevrons: a vertical spine broken in the middle, a nose pointing left out of
the break, and a short tip turning right at each end.

```
    __
   |
  <
   |__
```

`kBoardBraceWidth` is 8 and the geometry inside it is fixed: the nose is the
leftmost pixel, the spine stands 4px right of it, and the two tips run 3px
right of the spine. Width is 8 at any height.

Height isn't a constant. The brace runs from the top of the first line's slot
to the bottom of the second's, both of which come from `kRowHeight`, so a panel
that gives the row more or fewer pixels gets a brace that spans whatever the
two lines end up being. Nothing in it is measured off a glyph table, because
nothing in it is a glyph.

8px is thin for a 44px span, and that is a choice. A brace drawn in proportion
would be about 18 wide and would cost the name column ten of the pixels this
whole section exists to find. It reads as a bracket with a pinch in the middle,
which is what it has to do.

#### Where the one value for the pair goes

`FINAL`, `P3` and `6:00 PM` describe the game, not a team. There are two lines
and one of each of those, so the status stays in the column it's in, right edge
on `statusRight` the same as landscape, and sits vertically centred between the
two lines rather than on either one.

Right, because that is the one column a reader scans down the side of the card,
and 15h is explicit that it's the column that stayed aligned when nothing else
did. Moving it would cost the scan to buy nothing. Centred, because a status on
line one reads as the away team's and a status on line two as the home team's,
and it is neither.

The status slot is `Sans9`, 22px tall, in a 44px block, so its top is 11px
below the block's top line, which is `top + 15` on the profile panel.

#### The score face drops a tier, and that is what buys the fourth game

This is the cost, and it is the only one.

The profile row is 56px and the card inside it is 52. Two `Sans9` slots are 22
each: 4px of padding, 22, 22, 4px of padding, 52 exactly. Two `Sans12` slots
are 29 each, which is 58, and 58 does not go into 52. The score cannot stay at
`Sans12` and be on its own team's line.

The alternative is three rows instead of four. 224px over three gives a 74px
row and a 70px card, which holds two `Sans12` lines with room over. It also
drops a game off a board the server sends four of, which is a product change
nobody asked for, so it isn't what this does. `kMaxRows` is still 4 and the
board still shows four games.

So in profile the score draws in `Sans9` with the name. Two digits measure 20px
there against 26 at `Sans12`, measured off the vendored glyph table by 9b's
method, and `kBoardStackedScoreWidth` is that 20. Landscape keeps `Sans12`, and
`kBoardScoreWidth` is not touched here.

**The 26 was wrong and section 15k is where it was corrected.** Two digits of
`Sans12` are 28 where one of them is a `1`, so `kBoardScoreWidth` is 28 and the
one-line names in this section's "before" column are two pixels narrower than
it says. The 20 stands: every digit of `Sans9` advances 10, so a two-line score
column of 20 holds any two of them.

Reversing the trade is one constant and one face: a `kMaxRows` of 3 and a score
drawn at `Sans12` gives big digits on three games. The figures above are what
it would cost to look at.

#### Landscape is one line, and nothing in it moves

35px holds one 22px slot and not two. The landscape board keeps
`boardColumnsForBoard()`, `kBoardScoreWidth`, `Sans12` scores and every figure
15h and 15i wrote down, and the assertions in `App/CardColumns.h` that read
them are unchanged.

Unchanged by *this* section. Section 15k widened `kBoardScoreWidth` to 28 and
moved the one-line name figures in 15h and 15i by two pixels. It moved two
things below as well, because it re-derived `kBoardNameFloor` from 31 to 29:
the four two-line ceilings each rose by 2, to 137, 157, 217 and 237. Every
two-line **name** figure below is unchanged, because no string a provider sends
reaches a two-line ceiling.

`showSportsBoard()` branches once, on `kScreenH > kScreenW`, which is the
branch it already makes for `kRowHeight`. What the branch picks is the grid,
the two line positions, the score face and whether a brace is drawn; the five
`layoutLine()` calls underneath are one set, not two. The profile row puts both
names at the same left edge and both scores at the same right edge, so
`BoardColumns` holds the profile grid without a field added to it:
`awayNameLeft == homeNameLeft` and `awayScoreRight == homeScoreRight`, and the
two lines differ in y alone.

#### A scheduled game, in the two-line form

15i's rule is unchanged and it reads better vertically. `scoreless` is still
the board's question: one game under way gives every row a score column, and a
scheduled row draws nothing into it. In the two-line form that is a blank cell
at the end of each of the pair's two lines, under the digits of the games that
have started, with the start time centred against the brace. The pair is there
and the numbers haven't happened.

A board where no row reported a score releases the column on every row, and the
name goes to 106 at 240px with the widest start time in the status. 186 is what
it reaches when the status column goes too, which is a board whose rows sent no
status at all. `App/CardColumns.h` asserts both.

#### What a profile row gets, at both widths

Measured off the vendored `FreeSansBold9pt7b` table by 9b's method, which
reproduces all twenty figures this document and 15g, 15h and 15i already carry
to the pixel: `FINAL` 55, `PPD` 37, `LIVE` 41, `P1` 22, `6TH INN` 74, `4TH QTR`
79, `3RD QTR` 80, `HALFTIME` 94, `OVERTIME` 97, `12:00 AM` 78, `7:30 PM` 67,
`7:00 PM` 67, `2ND P...` 66, `Hurricanes` 94, `Capitals` 70 and `Automated
estimate, not an appraisal.` 323 at 9pt, and `Sunrise` 89, `Sunset` 81,
`Est. value` 112, `$9999K - $99999K` 200 at 12pt.

"Before" is the one-line grid of `v2026.11.01.0005`, which is built and not
registered. The 320 column is what a 320px panel in profile would draw, not
what the landscape board draws; no such panel is on the fleet, and the host
sweep is what covers it.

| panel | board | widest status | name before | status before | name after | status after |
|---|---|---|---|---|---|---|
| 240 | with scores | nothing sent | 71 | 0 | 166 | 0 |
| 240 | with scores | `PPD` 37 | 43 | 56 | **110** | 56 |
| 240 | with scores | `FINAL` 55 | 43 | 56 | **110** | 56 |
| 240 | with scores | `7:30 PM` 67 | 36 | 69 | **97** | 69 |
| 240 | with scores | `6TH INN` 74 | 33 | 76 | **90** | 76 |
| 240 | with scores | `12:00 AM` 78 | 31 | 80 | **86** | 80 |
| 240 | with scores | `3RD QTR` 80 | 31 | 80 | **84** | 82 |
| 240 | with scores | `HALFTIME` 94 | 31 | 80 cut | **70** | **96** |
| 240 | with scores | `OVERTIME` 97 | 31 | 80 cut | **67** | **99** |
| 240 | scoreless | `12:00 AM` 78 | 57 | 80 | 106 | 80 |
| 240 | scoreless | `HALFTIME` 94 | 49 | 96 | 90 | 96 |
| 320 | with scores | `FINAL` 55 | 83 | 56 | 190 | 56 |
| 320 | with scores | `HALFTIME` 94 | 63 | 96 | 150 | 96 |
| 320 | scoreless | `HALFTIME` 94 | 89 | 96 | 170 | 96 |

`HALFTIME` is no longer cut on the 240px panel. 15h and 15i both record it as
the one string the profile board could not hold: 94px needs 96, the ceiling
there was 80, and 96 would have put two name columns at 24 where
`kBoardMinNameWidth` clamps. There is one name column now, so the ceiling is

```
rowSpan - kBoardBraceWidth - scoreWidth - 3 * kBoardColumnGap - kBoardNameFloor
```

with one `kBoardNameFloor` subtracted instead of two, which is 137 at 240 with
scores and 157 scoreless. 96 fits under both. `OVERTIME` at 97 fits too, and so
does every eight-character string the glyph table can make up to 135px. Those
three figures were 135, 155 and 133 at the 31px name floor this section was
written against; section 15k re-derived the floor to 29 and they moved with it.
Above 135 the status takes the ceiling and the name stands on the floor: the
widest eight characters the glyph table makes is 144, so the strings between
those two are reachable in principle and no period a provider sends is one.

#### What a reader loses, said rather than left to be found

The profile score digits are a tier smaller, `Sans12` to `Sans9`, on the
profile panel only. That is what the fourth game cost.

A three-digit score is still cut, and by the same margin it is today: 30px of
`Sans9` into a 20px column, where landscape puts 40px of `Sans12` into 28.
Carried over unchanged rather than fixed or worsened, and a basketball board is
where it shows. Section 15k measured that pair: the landscape figures read 39
into 26 here, and `100` is 40 against a column that is now 28. 15k also traced
what the row actually draws, which is `...` with no digits in it rather than a
cut number.

And one thing the measurement turned up that is **not** this change and was on
the fleet when this section was written. `kBoardScoreWidth` was 26 and its own
remark called that two digits of `Sans12`, which is true of nine digits out of
ten: every digit in `FreeSansBold12pt7b` has an `xAdvance` of 13 except `1`,
which has 14. So `11` measures 28 and `10` measures 27, both over the 26px
column, and `layoutLine()` dropped them to the next face down and drew them at
`Sans9`. A score of 11, 10 or 21 on a one-line row was a tier smaller than the
score beside it. It was not cut and it was not wrong, it was inconsistent, and
a hockey or baseball board hits it most nights. Fixing it moves
`kBoardScoreWidth`, which every table in 15b, 15h and 15i is built on, so it
was its own change and not this one: **section 15k is that change.** The column
is 28 there, and eighteen of the ninety two-digit scores were affected rather
than the four this paragraph listed - a single `1` measures 14 and always fitted
26.

The two-line row does not have it. Every digit in `FreeSansBold9pt7b` has an
`xAdvance` of 10, so every two-digit score measures exactly 20 and
`kBoardStackedScoreWidth` is exactly 20.

A name over 97px is still ellipsized on the photographed board's status, and
over 110 on a board of state words. `Golden Knights` is 134 and `Diamondbacks`
128, so they draw as `Golden Kni...` and `Diamondba...` on either. The
20-character `SportsBoardRow::kMaxName` can hold a
string the column can't, and no panel width makes that go away; what changed is
how much of the league is above the line rather than below it.

A tall row with two lines in it is a denser card: four games, eight team names
and four statuses on a 240px panel, where the same card held four names whole
and four stubs. Whether that reads as a scoreboard or as a wall is the thing a
photograph has to answer and nothing here can.

Nothing on the wire changes. `SportsBoardRow` and `Sports::Game` are
byte-for-byte what they were. No field was added, removed or reinterpreted, so
the 6-month compatibility gate is untouched, and a device without this draws
the same payload one line to a row.

`App/CardColumns.h` is the arithmetic, `tests/card-columns-test.cpp` sweeps it,
`ci/check-board-columns-call.sh` holds the draw path's arguments, and
TEST_PLAN.md section 18h has the mutations.

### 15k. Two widths that were measured wrong, 2026-11-02

Both of these came out of reading glyph tables and the two routines that read
them. Neither has been photographed, and the end of this section says what that
leaves open.

#### A two-digit score with a `1` in it is drawn a tier small

`1` is the only digit in `FreeSansBold12pt7b` whose `xAdvance` is 14. Every
other digit advances 13, and `kBoardScoreWidth` was 26, which is two of those
13s and nothing over:

| score | `Sans12` | against a 26px column |
|---|---|---|
| `00` | 26 | fits, with nothing to spare |
| `99` | 26 | fits, with nothing to spare |
| `10` | 27 | over |
| `21` | 27 | over |
| `11` | 28 | over |

Eighteen of the ninety two-digit scores measure over 26: `10` through `19`, and
`21`, `31`, `41`, `51`, `61`, `71`, `81` and `91`. Not one of the ninety
measures over 28.

What a score wider than its column gets is `layoutLine()`'s ladder, working
exactly as section 10 writes it. The string doesn't fit one line of the caller's
`Sans12`, so the ladder steps down the family and `Sans9` fits, which is 22px of
face against 29. A game at 10-7 draws its `10` smaller than the `7` beside it,
on one row, and the only report is the tier drop the ladder logs under
`verbose()`, where a drop is the ordinary case for a long team name.

**The single digit is fine.** `1` on its own is 14px and sits inside the column,
so a game at 1-0 draws both digits at `Sans12`. It takes two digits.

The column is 28 now, which is what `11` measures, and
`kBoardWidestTwoDigitScoreWidth` carries that 28 beside it so a build reads the
two against each other rather than against a comment.

**Three digits are drawn wrong and this does not fix it.** `100` is 40px at
`Sans12` and 30 at `Sans9`, over the old column, over the new one and over every
tier the ladder has, so `layoutLine()` falls through to `layoutText()` at the
caller's own face with a one-line budget. There it hard-breaks to two characters,
finds there is more text and no line left, and runs the ellipsis loop: `10...`
is 45px and `1...` is 32, both over the 28px column, so `take` reaches 0 and the
row draws `...` with no digits in it at all. Not a cut number, nothing. The
report is `Log::verbose`, the line about a word wider than the box being broken
mid-word, so it is in the remote stream only when streaming is on for that
device.

`SportsBoardRow::kMaxScore` holds eight characters and its own remark says a
score is three digits at the outside, so a basketball board draws `...` for a
side over 99. Closing that is a column width and a face rather than a constant,
and it is written down here rather than changed.

#### Where the two pixels come from

Two pixels on each of the two score columns is four off the row, and the names
split what the row has left, so each name column loses two. Section 15b's table
and the "with scores" half of 15h's are the figures, and the "scoreless" half of
both is untouched because a scoreless row has no score column to widen.

| panel | state | name before | name after |
|---|---|---|---|
| 320 | with scores | 83 | 81 |
| 320 | scoreless | 97 | 97 |
| 240 | with scores | 43 | 41 |
| 240 | scoreless | 57 | 57 |

What doesn't move is where the second block starts and ends. `homeNameLeft` is
`rowLeft + nameWidth + scoreWidth + kBoardColumnGap`, so a name two narrower and
a score two wider land it in the same place: all four of 15h's ragged-grid name
edges, 157, 129, 119 and 109, and all four of its home-score edges, 294, 238,
218 and 198, are the figures they were. So is the separation, which is two
column gaps plus the row's leftover pixel at every width.

**The clearest thing the two pixels cost is `Jets`.** On 15i's photographed
240px board the one-line name column goes from 36 to 34, and `Jets` measures 36.
It was the one name of those eight the landscape row still spelled, and the
landscape row now draws `Jet...` as well. The two-line row's 97 holds all eight,
so the profile panel this board was photographed on is unaffected; a 240px board
in landscape would lose the last name it had. `tests/card-columns-test.cpp`
counts the eight rather than asserting a win, which is what turned this up.

#### The name floor is the same derivation re-run

`kBoardNameFloor` was 31 because 31 is "the largest floor that still leaves the
240px panel room to hand a start time the 80px a start time measures". That
sentence is a derivation with the score width inside it. Re-run at 28 it gives
29, and the floor is 29 now.

Keeping 31 was the other option and it costs a property. The 240px ceiling would
go to 76, the widest start time asks for 80, and the assertion in
`App/CardColumns.h` that a scheduled row which reported one score still holds a
start time would have had to come out. Re-deriving the floor keeps both ceilings
on rows with scores exactly where they were:

| ceiling | before | after |
|---|---|---|
| 320, with scores | 160 | 160 |
| 240, with scores | 80 | 80 |
| 320, scoreless | 212 | 216 |
| 240, scoreless | 132 | 136 |
| 320, two-line, with scores | 215 | 217 |
| 240, two-line, with scores | 135 | 137 |
| 320, two-line, scoreless | 235 | 237 |
| 240, two-line, scoreless | 155 | 157 |

The scoreless and two-line ceilings rise because a floor is subtracted from each
of them and nothing else in them moved.

**No period a provider sends reaches a two-line ceiling, which is not the same
as nothing reaching one.** The 240px two-line ceiling is 137, so a status
measuring over 135 lands on it, and 15h's own figure for the widest eight
characters the `Sans9` table can make is 144 - eight `@` are exactly that. So
the band from 136 to 144 is reachable by a string the 8-character
`Sports::kMaxPeriodLength` can hold, and no period any provider sends is in it:
`OVERTIME`, the widest real one, is 97. Every name figure in 15j is the figure
it was for the strings 15j tabulates, and a status in that band would put a
two-line name on the floor, which moved from 31 to 29.

What the floor costs is two pixels on a name already standing on it, which on
the 240px panel is a row with scores and a status over 77px. Those rows read 29
where they read 31. The widths where the floor first engages move with the score
column: the separation stops being two gaps below 206px with scores rather than
202, and the two columns cross below 194 rather than 190. Both are far under
every panel the fleet has, and the scoreless pair, 174 and 162, is unchanged.

#### The two-line row was already right

Every digit of `FreeSansBold9pt7b` advances 10, `1` with the rest of them, so
the widest two digits at that face are 20 and `kBoardStackedScoreWidth` is 20
exactly. A `11` on a profile row has always fitted. The defect is the landscape
one-line row's alone, and `a525c3b` dropping the stacked score to `Sans9` is
what kept it out of the two-line form rather than anything anybody checked.

#### A status carrying a control byte is drawn wider than it was measured

Two routines disagree about a byte below 0x20, and the board reads one of them
and draws through the other.

`LGFXBase::text_width()` skips such a byte. Its UTF-8 loop advances over
anything under 0x20 before it reaches a glyph table, so the byte contributes no
pixels at all, and a string that ends in one ends the measurement there.

`copySlice()` in `App/Display.cpp` turns it into a space, which is what section
3's whitespace rule asks for: a `\n` in an admin's announcement would otherwise
reach `drawString()` and render as whatever box glyph an ASCII-only face has.
The space it becomes advances 5px at `Sans9`.

So each interior control byte in a status is charged nothing when the column is
sized and 5px when the string goes on the panel. On a 320px board with scores, a
period arriving as `2ND<0x01>QTR`:

| | measured | status column | name | what the row draws |
|---|---|---|---|---|
| before | 76 | 78 | 70 | `2ND...` at 49px |
| after | 81 | 83 | 67 | `2ND QTR` at 81px |

The before row is the whole of it. `widestBoardStatus()` measured 76, so the
board laid out a 78px column; `layoutLine()`'s own overflow test read the same
76 and found it inside 78, so no tier was tried; and `layoutText()` then built
the line through `copySlice()`, measured the 81px it had made and ellipsized to
fit. Three measurements of one string, two of the bytes that arrived and one of
the bytes that get drawn.

A leading or trailing control byte puts no ink on the panel. `layoutText()`
skips leading break space before it starts a line and ends a line at a word
boundary, so trailing break space is never in the slice it draws, and
`text_width()` skips both. `FINAL<0x01>` draws 55. It takes a byte with glyphs
on both sides to cost anything.

**Which is why the measurement trims both ends, and the first version of it did
not.** Substituting a space for every byte at or below 0x20 and measuring the
result charges 5px for a trailing one, so `FINAL<0x01>` measured 60 against the
55 it draws. That is the same class of error pointing the other way, and on the
one path where the number is not measured again before it is spent: a board
whose widest status was `FINAL<0x01>` took a 62px column instead of 56 and 3px
off each of its names, for a string the header asserts costs the names nothing.
A status that is nothing but a control byte measured 5 and took a 56px column to
draw no ink in. Three reviews of the first version found it, two of them
independently.

So `drawnTextWidth()` strips leading and trailing break space before it copies,
which is exactly what `layoutText()` drops, and substitutes inside what is
left, which is exactly what `copySlice()` keeps. An interior run of break space
is preserved byte for byte in both, so `2ND  QTR` with two spaces measures 86
on both paths and `2ND<0x01>QTR` measures 81 on both.

#### What the board measures now

One function, `drawnTextWidth()`, which copies a string through the same
control-byte substitution `copySlice()` applies and measures that. Both places
that were reading the raw bytes read it: `widestBoardStatus()`, which sizes the
column, and `layoutLine()`'s overflow test, which decides whether to try a
smaller face. A string with no control byte in it measures what it always
measured, so nothing else on any card moves.

It sits beside `copySlice()` and takes the substitution from the same
`isBreakSpace()` predicate, because two copies of "a control byte becomes a
space" is the shape the defect had in the first place. A string longer than the
`kLayoutLineBytes` buffer falls back to the raw measurement, which cannot change
an answer: 128 characters of the narrowest face in the binary is past any box
this panel has, so the comparison the caller is making is already true.

#### What is measured and what is assumed

The widths are measured. Every figure in this section comes off the vendored
glyph tables by 9b's method, and that reimplementation reproduces all
twenty-seven figures this repository already carries, `FINAL` 55, `HALFTIME` 94,
`Hurricanes` 94, `Est. value` 112 and the rest, to the pixel, before it was
trusted on a digit.

The score defect is on the fleet. Any board with a side on 10 through 19 draws
it, which is most hockey and football boards and nearly every basketball one.

**A status with a control byte in it has never been reported.** The route is
open: `period` arrives through `copyBoundedMarked()` in `App/CheckIn.cpp`, which
bounds the length and marks a cut and does not scrub control characters, where
`Calendar.cpp` scrubs its own free text and says so. So the arrival is a thing
the wire allows rather than a thing a device has sent, and the two widths are
what was established before anything was changed.

**Nothing on the wire changes.** `SportsBoardRow`, `Sports::Game` and the
check-in payload are byte-for-byte what they were. No field was added, removed
or reinterpreted, so the 6-month compatibility gate is untouched, and a device
without this draws the same payload with the same two defects.

**Neither has been on glass.** The score column is a constant and two tables of
arithmetic a host run checks; the control byte is a measurement a text gate
checks. What a 28px column looks like beside a 41px name on the 240px panel, and
whether the two pixels each name gave up can be seen, is what a photograph has
to answer.

`App/CardColumns.h` is the arithmetic, `tests/card-columns-test.cpp` sweeps it,
`ci/check-board-columns-call.sh` holds the draw path's arguments, and
TEST_PLAN.md section 18i has the mutations.

### 15l. A three-digit score draws, 2026-11-02

15k left this open and recorded what it was: `100` is 40px of `Sans12` and 30 of
`Sans9`, over every score column and past every tier the fitting ladder has, so
`layoutLine()` falls through to `layoutText()` with a one-line budget and the
ellipsis loop backs off to `...` with no digits in it. A basketball board shows
`...` wherever a side is over 99, which is most rows of most games.
`SportsBoardRow::kMaxScore` is 8 and its own remark says three digits are
expected, so this is a card not drawing what the wire sends it.

#### What three digits measure

Off the vendored glyph tables by 9b's method, the reimplementation reproducing
all twenty-seven of this repository's figures first:

| digits | widest at `Sans12` | widest at `Sans9` |
|---|---|---|
| one | `1`, 14 | 10 |
| two | `11`, 28 | 20 |
| three | `111`, 42 | `100`, 30 |
| four | `1111`, 56 | 40 |

**Every three-digit score is exactly 30px at `Sans9`.** All ten digits advance 10
in that face, so all 900 of them are 30 and a 30px column holds the lot. At
`Sans12` the `1` that 15k is about spreads them from 39 to 42, and 27 of the 900
are over a 40px column.

#### The four shapes, costed

The name column is at 81 on the 320px landscape row and 41 on the 240px one, and
the 240px portrait row's single name is 97. So "take it out of the names again"
is costed against a floor of 29 that two of those are already near.

| shape | 320 one-line name | 240 one-line name | 240 two-line name | what else |
|---|---|---|---|---|
| today, `...` drawn | 81 | 41 | 97 | no digits at all |
| **A** 42px column, keep `Sans12` | 67 | **27** | 87 | 240 one-line ceiling collapses to 56, start time cut to 56 |
| **B** 30px column, `Sans9` scores | **79** | **39** | **87** | 240 one-line start time cut to 76 |
| **C** per-row rather than per-board | 79 or 81 | 39 or 41 | 87 or 97 | a `105` drawn smaller than the `98` beside it |
| **D** drop the leading digit | 81 | 41 | 97 | `105` reads as `05`, which is a different score |

**A is the one that cannot be paid for.** 14px off each name takes the 240px
one-line row to 27, under the 29 floor the status ceiling is derived from, and
the ceiling there falls to the 56px clamp, which cuts a start time by 24px. 15k
moved that floor to keep exactly this property.

**D is not a trade, it is a wrong number.** A score column that can show `05` for
105 is worse than one that shows `...`: the ellipsis says a thing is missing, and
`05` says the Rockets lost.

**C reintroduces the defect 15k just fixed.** 15k's whole finding is that a `10`
drawn a tier below the `7` beside it is wrong, and deciding the face per row, or
per score, puts a small `105` next to a full-size `98` on the same card. The
principle 15k established is that a row is internally consistent, so the face is
the board's question and not a row's.

**So B, with the face as a board-wide decision.** The score column is 30 and the
scores draw at `Sans9` when any score on the board needs three digits, and
nothing changes when none does. Two digits at `Sans9` are 20 and three are 30,
so dropping the face is what makes the column cost 2px instead of 14.

#### What it costs, and only when it engages

| | two-digit board | three-digit board |
|---|---|---|
| 320 one-line name, board of `FINAL`s | 81 | 79 |
| 240 one-line name, board of `FINAL`s | 41 | 39 |
| 240 two-line name, board of `FINAL`s | 110 | 100 |
| 240 two-line name, photographed board | 97 | 87 |
| 320 two-line name, photographed board | 177 | 167 |
| one-line ceiling, 320 / 240 | 160 / 80 | 156 / 76 |
| two-line ceiling, 320 / 240 | 217 / 137 | 207 / 127 |
| score column, one-line / two-line | 28 / 20 | 30 / 30 |

**A board with no three-digit score on it is untouched, in every figure.** That
is the whole of why this is affordable: an NHL, NFL, MLB or soccer board never
reaches the state, so every figure 15a through 15k carries is the figure that
board still draws. Nothing the owner has photographed changes.

**The 2px on the one-line row is the same 2px 15k spent**, and for the same
reason: the names split what the row has left. The 10px on the two-line row is
larger because that row has one name rather than two, so it pays the whole
column rather than half of it.

**Every column edge is unchanged wherever the three states give the status column
the same width.** `nameWidth + scoreWidth` is
`(rowSpan - statusWidth - 3 * kBoardColumnGap) / 2`, which has no score width in
it, and the doubled score width is even so the halving truncates the same way. So
`homeNameLeft` is 129 and `homeScoreRight` 238 on a 320px board of `FINAL`s
whether the score column is 0, 28 or 30, and the separation is two gaps plus the
row's leftover pixel at all three.

**The qualifier is load-bearing, because the status width is itself
state-dependent at the ceiling.** A wider score column means a lower ceiling, so
once a board's widest status measures more than the narrowest of the three
ceilings allows, the three states hand their status columns different widths and
the edges part with them. The first measurement at which that happens:

| row | parts above a status measuring | widest a provider can send |
|---|---|---|
| 320 one-line | 155 | 144 |
| 240 one-line | 75 | 144 |
| 240 two-line | 126 | 144 |

144 is the widest eight characters the `Sans9` table can make, from 15h, and
`Sports::kMaxPeriodLength` is 8. So on the 320px landscape board the edges cannot
part at all, and on the two fielded shapes nothing a provider sends reaches the
band: `OVERTIME`, the widest real period, is 97. The 240px one-line row is the
exception and `6TH INN` at 74 is a pixel under its boundary - which is arithmetic
rather than a panel, because the 240px panel is portrait and draws two lines.

So the claim a reader should carry is the scoped one: a three-digit board draws
on the same grid as a two-digit one for every string a provider sends, and the
digits are the only thing that moved. `tests/card-columns-test.cpp` sweeps the
identity over width and status measurement together and finds that boundary
rather than asserting the unscoped version at one status.

#### What a reader loses, said rather than left to be found

**A basketball board's scores are the size of its team names.** On a one-line row
the score has drawn at `Sans12` against `Sans9` names since the card was written,
which is 29px of face against 22, and that is the emphasis a scoreboard puts on
the number. A three-digit board gives that up: the score drops to the names' own
face and the row reads flatter. The two-line portrait row already did this in 15j
and the owner has photographed it; the landscape row has not been seen this way.

**`Hurricanes` is cut on a portrait board that has a three-digit score.** The
240px two-line name goes 97 to 87 and `Hurricanes` is 94. No hockey board reaches
the state, so that pairing cannot arise on the board 15j photographed; what it
means is that the 97 that section won is 87 for basketball. Of the 30 NBA
nicknames, 27 fit 87 against 28 at 97, so exactly one crosses: `Mavericks` at 88.
`Timberwolves` at 121 and `Trail Blazers` at 108 were cut at 97 already. On a
board of `FINAL`s rather than one carrying a start time the name is 100, and
those two are the only ones cut.

**A 240px one-line board with three-digit scores cuts the widest start time.**
The ceiling there falls from 80 to 76 and `12:00 AM` is 78. The 240px panel on
the fleet is portrait and draws the two-line form, so no fielded board has this
shape, and 15k's note about the one-line 240 row being arithmetic rather than a
panel applies here too. It is a real loss at a width nobody builds, recorded
rather than hidden: the assertion that a scheduled row holds its start time is
asserted for the two-digit state and stated as lost for the three-digit one.

**A two-digit negative score dropped the whole board's face. Closed by 15m,
2026-11-02.** The fold keys on the longest score STRING, so `-10` was three
characters and put the board in the three-digit state: 30px columns, `Sans9`
scores, 2px off each one-line name or 10 off the two-line one, for a board whose
largest number is 10. 15m settles it as a question about the data instead of a
column width: none of the five registered sports scores below zero, `-1` is
already `Sports::kNoScore`, and a rule that drew `-2` and blanked `-1` would give
two answers about one range. The whole negative range now stores as `kNoScore`
and draws an empty cell, so no signed string reaches this fold and `strlen`
equals the digit count for everything that does. 15m also measures what the two
alternatives cost: `-10` is 35px at `Sans12` against a 28px two-digit column, so
a fold counting digits would have handed it a column it is over and the ladder
would have drawn it a tier below the digits beside it, which is this section's
own option C.

**The single-game card still draws a three-digit score at `Sans18`.** This
section is about `showSportsBoard()`. `showSportsCard()` has its own 76px score
column and draws at `Sans18`, so on one rotation the same 105 is 56px of face on
`sportsteam` and 22px on `sportsscores`. The consistency rule this section
applies is within a row and within a board; across the two cards of the family
there isn't one, and nothing here gives them one. No `Sans18` digit figure exists
anywhere in this repository, so whether three of them fit that 76px column is
unmeasured - the fitting ladder would drop them to `Sans12` at 42px before
reaching an ellipsis either way.

**Four digits still ellipsize, and draw `1...`.** 40px of `Sans9` against the
30px column, past every tier again, so `layoutText()` backs characters off until
the line plus the mark fits. At `Sans9` that lands one digit in: `1000` draws
`1...` at 22px and `32767` draws `3...`. Not the bare `...` a three-digit score
drew before this section, which is what the 28px `Sans12` column backed off to,
and that difference is the only thing the wider column buys a four-digit score.
No sport this card carries sends one: basketball tops out in the 100s, cricket
innings totals are three digits, and `kMaxScore`'s eight characters are a buffer
bound rather than an expectation. Asserted as a gap the way three digits were,
so closing it has to come through that assertion.

#### The board's score state is one value with three cases

`scoreless` was a `bool` and the question is now three-valued: no scores, two
digits, three digits. Two bools would admit a fourth case that means nothing,
scoreless and three-digit at once, and would let a caller swap them, which is the
hazard the remark beside `boardColumns()` already records about an `int`
converting to `bool` without a diagnostic. So it is an `enum class`:

```cpp
enum class BoardScores : uint8_t { None, TwoDigit, ThreeDigit };
```

An `enum class` converts from nothing, so a forgotten or swapped argument stops
the compile rather than drawing a wrong board. `boardScoreWidth()` reads it and
owns the policy, which keeps the figures in this header where
`ci/check-board-columns-call.sh` can go on forbidding the draw path from naming a
constant.

`boardHasNoScores()` becomes `boardScoreState()`, one fold over the rows
returning one of the three, beside the fold that measures the widest status.
`showSportsBoard()` reads it twice: once for the grid and once for the face.

`App/CardColumns.h` is the arithmetic, `tests/card-columns-test.cpp` sweeps all
three states at every width, `ci/check-board-columns-call.sh` holds the fold and
the face, and TEST_PLAN.md section 18j has the mutations.

---

### 15m. A negative score is not a scoreline, 2026-11-02

15l left one case written down rather than fixed, and this closes it as a
decision about the data rather than as a fourth column width.

`boardScoreState()` folds over the score STRINGS and keys on `strlen`, so `-10`
is three characters and puts the whole board in `BoardScores::ThreeDigit`: 30px
score columns, `Sans9` scores on every row, 2px off each one-line name or 10 off
each two-line one. A board whose largest number is 10 pays what a board carrying
a 105 pays, and every other score on it drops a face with it.

#### What a minus sign measures

Off the vendored glyph tables by 9b's method. The reimplementation reproduces
all twenty-four figures this repository already carries by hand - `Est. value`
112, `Sunrise` 89, `Sunset` 81, `$400K - $450K` 161, `$9999K - $99999K` 200,
`FINAL` 55, `6TH INN` 74, `4TH QTR` 79, `HALFTIME` 94, `OVERTIME` 97,
`2ND P...` 66, `2ND PERIOD` 112, the 78px widest start time, `6:00 PM` and
`7:30 PM` at 67, `11` at 28 and `1` at 14 in `Sans12`, `100` at 40 and 30,
`11` at 20 in `Sans9`, `1000` at 40, `1...` and `3...` at 22, `...` at 12 - to
the pixel, and `yAdvance` at 22 and 29, with `fontHeight()` at 18 and 23.

`-` advances 8 in `Sans12` and 6 in `Sans9`. So:

| score | characters | `Sans12` | `Sans9` |
|---|---|---|---|
| `-1` to `-9` | 2 | 21 to 22 | 16 |
| `-10` to `-99` | 3 | 34 to 36 | 26 |
| `-100` to `-999` | 4 | 47 to 50 | 36 |
| `-1000` to `-9999` | 5 | 60 to 64 | 46 |

#### Counting digits instead of characters moves the defect, it does not fix it

The obvious reading is that the fold should have counted digits, because 10 is a
two-digit number. Measure it and what you get is 15k's defect instead of this
one.

`kBoardScoreWidth` is 28, which is `11` at `Sans12`. `-10` at `Sans12` is 35. A
fold that called `-10` two-digit would hand the board a 28px column and the score
would be 7px over it, so `layoutLine()` steps down `kSansTiers` from the caller's
face: `Sans12` measures over and is skipped, `Sans9` is 26 and its 22px block
sits inside the 29px slot, and the score draws **whole, at `Sans9`, one tier
below the digits beside it**. No ellipsis. That is exactly the defect 15k is
about and 15l's option C rejects, a `105` drawn smaller than the `98` next to it,
and it is why the face is the board's and not a score's.

So the alternative swaps a face drop every row of the board pays together for one
score drawn a tier small on its own. The string is what has to fit the column,
which is why the fold is written the way it is, and `boardScoreState()`'s own
remark says so: a score arrives as text and what has to fit the column is the
text.

#### A fourth state costs more than the face drop on one row shape and less on the other

The other reading is a `BoardScores::NegativeTwoDigit`. Sized at 36px, the widest
`-10` to `-99` measures at `Sans12`, it costs each one-line name 8px against the
face drop's 2, on a 240px row whose name is already 41. Four times, and the
figure is right.

It does not hold on the two-line row, and that is the row the fleet's 240px panel
draws. That row has one score column, not two, and it is already `Sans9`, so a
fourth state would be sized at 26 rather than 36 and would cost its one name 6px
against the face drop's 10. On the shape most of the fleet is looking at, the
alternative this section rejects is the cheaper one. Computed through
`boardStackedColumnsFor()`: the two-line name is 110 at a 20px column, 104 at 26,
100 at 30.

So the pixels do not decide it, and saying they do would be the kind of
justification that reads as settled and is not. What decides it is the next
section: a fourth state threads a fourth case through nineteen signatures and
`scoreStateName()`'s switch, to buy a column for a number no sport this card
carries can produce.

#### So the negative range is not a score

The five sports `sportChipLabel()` maps are american-football, football,
basketball, baseball and hockey. None of them scores below zero. A negative
reaches the panel through the wire, where `SportsCardPayload`'s score is `int?`
end to end and nothing bounds it, rather than through a provider.

And the stored representation already answers for part of that range.
`Sports::kNoScore` is `-1`, so a server sending `-1` is sending "no score" and
gets an empty cell today. A rule that drew `-2` and blanked `-1` would give two
answers about one range, and which answer you got would depend on a sentinel
value picked for a different reason. Separating them would take a second wire
field saying which `-1` was meant. The compatibility gate closed 2026-09-22 and
allows additive optional fields, so that field is available rather than
impossible - it is just a field, a server change and a parse branch bought for a
case no league produces, which is a worse trade than the one below.

So the whole negative range is "no score". `CheckIn.cpp` stores
`Sports::kNoScore` for anything under zero, and for anything above `INT16_MAX`
as well, because the narrowing into `int16_t` is modular and 65536 would
otherwise arrive as a nil-nil draw. It names the value it was sent in the
stream. Both renderers read one predicate for what a drawable score is rather
than each restating the sentinel, which is what keeps their behavior defined for
a value this parse never produces.

#### What the board draws, and what the fold now rests on

Every score string the board can hold is decimal digits with no sign, so
`strlen` and the digit count are the same number and the fold is right by that
invariant rather than by accident. `boardScoreIsDrawable()` in
`App/CardColumns.h` is the predicate, it is `constexpr`, and
`tests/card-columns-test.cpp` sweeps the whole `int16_t` range against it and
asserts the invariant across every score the formatter can write.
`ci/check-score-guard.sh` holds the three lines that call it, because a predicate
nothing calls is a predicate that passes its own test.

#### What a reader loses, said rather than left to be found

**A negative score draws the same empty cell a game that has not started draws.**
That is the cost, and it is not recoverable on glass: the cell holds no mark, and
15l's own reasoning rules out inventing one, because a glyph standing for "the
server sent a figure this card will not draw" is a claim about the server that
the panel has no room to qualify. What carries it instead is the stream, which
is a deployed device's only diagnostic channel: the check-in that parsed the
value logs the teams and the number it dropped, once per check-in rather than
once per draw.

**Nothing a provider sends changes.** For all five sports every score is zero or
above, so this moves no pixel on any board the fleet is drawing. The scores that
change are the ones the wire can carry and no league produces.

**One layout does move, on the single-game card.** `showSportsCard()` decides
whether to reserve its 76px score column from `homeScore.length() > 0 ||
awayScore.length() > 0`. A game whose two scores were both negative used to give
two non-empty strings and keep the column; both are empty now, so the card draws
the shape a game that has not started draws and the two club names take the whole
panel. That is the right shape for a figure the card refuses to draw, and it is
the one layout in the family this section moves.

**The impression summary stops carrying the number too.** `describeCardAt()`
builds its line through the same `scoreText()`, so what the device reports as
having been on screen goes from `Penguins -5 @ Capitals` to `Penguins @
Capitals`. The field is free text and no schema moves. It matches what the panel
drew, which is the point of reading one predicate rather than two.

**The log has a ceiling and this can reach it.** Four cards of four games with
both sides negative is 32 lines in one check-in, and `Log`'s pending ring holds
exactly 32, so that check-in's other buffered lines are evicted. It needs a
server sending negatives on all sixteen games. Serial still carries everything;
what is lost is the remote stream's other content for that interval.

**A score of zero still draws.** `0` is 10px at `Sans9` and 13 at `Sans12`, and a
nil-nil draw is a real scoreline, which is the whole reason `kNoScore` is not 0
and the reason the predicate is `>= 0` rather than `> 0`.

**Four and five digit scores are untouched.** They are still `BoardScores::ThreeDigit`
and still ellipsize to `1...` at 22px, which 15l costed and left open. This
section narrows what can reach that case; it does not close it.
