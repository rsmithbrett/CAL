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

**Two CI gates, in `ci/build-firmware.sh`.** The compiler cannot catch a new
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

Both are deliberately greps and nothing cleverer, in the same spirit as the
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
   `Display.cpp` carries the corrected wrap; that file compiles clean, and the
   one symbol the change adds, `Journal::printf`, is defined in `Journal.cpp`
   in the same sketch.
2. The two CI gates in section 7, which fail the build on a new unbounded
   `drawString` and on any of the retired helper names reappearing.
3. **On hardware, and only on hardware:**
   - The sports card shows `Houston Astros` whole at the smaller tier, or
     `Houston Ast...` if it still will not fit, and never `Houston Astr`.
   - The away row reads `@ Houston Astros` and the home row has no marker.
   - A 21:40 Eastern game reads `21:40`, or `9:40 PM` in a 12-hour household,
     and not `01:40`. The debug stream carries
     `[sports] start time: utc=... offset=...min 12hour=... -> ...` on every
     draw, which is enough to check the conversion off the stream without
     standing in front of the panel.
   - The notice card's first line breaks after `rotation`, with `has` starting
     line two and no characters missing.

Point 3 is the one that needs a device. The first two are what this change can
prove on its own.
