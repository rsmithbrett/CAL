#pragma once

#include <Arduino.h>

/// A standalone clock/date card: HH:MM and today's date, large enough to fill
/// most of the panel - "a moment where the display is just a big clock", the
/// way a bedside or kitchen wall clock would look. Distinct from the small
/// bottom-right corner clock every other card already carries (see
/// Display.cpp's drawClock()) - that one is chrome, present so a glance at
/// any card also tells the time; this one *is* the content.
///
/// **This card fetches nothing and needs no policy to have something to
/// show**, unlike every other card registered so far. Weather and aircraft
/// each own a server route; Graphic needs an assetId before it counts as
/// having content; even SunMoon, which also fetches nothing of its own,
/// needs its first check-in to have landed before it has a sunrise/sunset to
/// draw. A clock has no such gap: the moment this device has booted,
/// `time(nullptr)` is a real answer (UTC, synchronised over SNTP before this
/// card's fetch/draw are ever reached - see AppService::synchroniseTime()),
/// and the local-time offset defaults to 0 (UTC) until the first check-in
/// narrows it - exactly the corner clock's own pre-check-in behaviour. So
/// cardItemCount() below always reports one item; there is no "not
/// configured" state to report zero for.
///
/// **Local time is computed here, not in Display.cpp.** The formula -
/// `time(nullptr) + utcOffsetMinutes * 60` - is the one CheckIn.h documents
/// and Display.cpp's own drawClock() already uses; this module reuses it
/// rather than inventing a second timezone calculation, reading the offset
/// back out via Display::utcOffsetMinutes() (see that accessor's own remarks
/// for why a read accessor exists rather than a second pushed copy).
/// Display.cpp only ever receives the already-formatted strings to lay out -
/// the same fetch/draw split SunMoon.cpp and Display::showSunMoonCard()
/// already use for sunrise/sunset.
///
/// **No seconds field.** This card, like every other card in this build, is
/// drawn once per dwell and then left alone until the scheduler's refresh
/// timer or a navigation event redraws it (see Cards.h's fetch/draw split) -
/// there is no per-second ticker here any more than there is for the corner
/// clock (see drawClock()'s own remarks on why one was judged not worth
/// building). A seconds field would freeze the instant this card was drawn
/// and read as wrong for the rest of its dwell on screen, which is a worse
/// look for a clock than simply not showing seconds at all.
///
/// **UNVERIFIED ON HARDWARE**, like every other card in this build: checked
/// by a clean compile and by reading, not by an automated test - this
/// firmware has none.
namespace ClockDate {

/// This card's registered id, and the `id` a policy entry must use to
/// schedule it. Exposed so the id appears exactly once in the firmware.
extern const char* const kCardId;

}  // namespace ClockDate
