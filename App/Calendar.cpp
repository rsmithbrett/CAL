#include "Calendar.h"

#include <ArduinoJson.h>
#include <stdio.h>   // sscanf/snprintf, used by parseIso8601Utc() and describeEvent()
#include <string.h>  // strlen/strcmp, used by copyTitle() and parseRefusal()
#include <time.h>

#include "Cards.h"
#include "Config.h"
#include "Display.h"
#include "Http.h"
#include "Identity.h"
#include "Log.h"

// =============================================================================
// THE WIRE CONTRACT THIS MODULE ASSUMES - READ THIS FIRST IF YOU ARE THE
// SERVER HALF.
// =============================================================================
//
// The device-authenticated calendar endpoint did not exist when this module
// was written; it is being built in parallel, following MyWeatherEndpoints'
// own `/mine` handler (RequireDeviceAuth, identity from X-Device-Secret only).
// Everything below is therefore an ASSUMPTION to be reconciled against that
// work, not an observed fact. Nothing here was tested against a live response.
//
//   ROUTE     GET https://{Config::kServiceHost}/api/mycalendar/mine
//             Header: X-Device-Secret: <this device's secret>
//             No query string, no device id on the URL - exactly like
//             /api/myaircraft/mine and /api/mylistings/mine.
//
//   200 body (every field optional; absence is tolerated everywhere):
//
//     {
//       "isConnected": true,
//       "events": [
//         { "startUtc": "2026-09-11T18:30:00Z",
//           "title":    "Dentist",
//           "isAllDay": false }
//       ]
//     }
//
//   FIELD BY FIELD, with what this firmware does when it is absent:
//
//     isConnected   bool. false means the account has no calendar connected,
//                   or none made eligible to display - a resting state, not an
//                   error. ABSENT IS READ AS true, so a server that never
//                   sends it still works; "no calendar" then arrives as an
//                   empty events array instead, which lands on the same
//                   silence (see the empty-state section below). Mirrors
//                   CalendarResult.IsConnected.
//
//     events        array. Absent, null or empty all mean "nothing upcoming".
//                   ASSUMED SORTED ASCENDING BY START TIME - this module does
//                   not sort, it takes the first kMaxEvents it can parse, the
//                   same way Listings.cpp relies on the server's own
//                   distance-ordering. If the server cannot promise ordering,
//                   say so and this module grows a three-element insertion
//                   sort; it is not a reason to send more events.
//
//     events[].startUtc
//                   ISO-8601 UTC instant, "2026-09-11T18:30:00Z". Parsed by
//                   the same sscanf shape CheckIn.cpp's parseIso8601Utc() uses
//                   for the ISS pass times, so any suffix (fractional seconds,
//                   "+00:00") is ignored rather than rejected. Absent,
//                   null or unparseable drops that one event and keeps the
//                   rest. Mirrors CalendarEventSummary.StartUtc.
//
//     events[].title
//                   string. **This firmware accepts "title" OR "subject",
//                   preferring "title".** The brief for this card said
//                   "title"; the server's existing CalendarEventSummary type
//                   calls the field `Subject` and would serialise camelCase as
//                   "subject". Rather than guess, both are read - it costs two
//                   filter entries and one strlen. WHICHEVER ONE THE SERVER
//                   ACTUALLY SENDS, THIS IS THE FIELD PAIR TO RECONCILE FIRST.
//                   Absent/empty/null draws the fixed phrase "(no title)"
//                   rather than dropping the event: a private or untitled
//                   block is still a real commitment at a real time.
//
//     events[].isAllDay
//                   bool, absent reads as false. Mirrors
//                   CalendarEventSummary.IsAllDay. For an all-day event
//                   startUtc is assumed to be MIDNIGHT ON THE EVENT'S OWN
//                   CALENDAR DAY expressed as UTC (i.e. "2026-09-12T00:00:00Z"
//                   for an event on the 12th), NOT midnight local converted to
//                   UTC. See localDayIndexOf() below for why that distinction
//                   decides whether a birthday shows on the right day in every
//                   western timezone - this is the assumption most likely to
//                   be silently wrong, and the one worth checking on a real
//                   device in a real timezone before believing this card.
//
//   FIELDS DELIBERATELY NOT REQUESTED, and which must not be added without
//   revisiting Calendar.h's privacy remarks: location, endUtc, attendees,
//   body, connectedAccount, calendar name, event id. The ArduinoJson filter
//   below whitelists exactly four keys, so any of these arriving is discarded
//   during parse and never reaches this device's RAM.
//
//   NO "how many events to send" KNOB is sent, matching every sibling route -
//   Listings.h records the same decision ("this is a display cap, not a
//   request parameter"). But note the asymmetry that creates: this device caps
//   what it STORES at kMaxEvents, while ArduinoJson still parses the whole
//   array into a JsonDocument first. **The server should send a handful, not a
//   month.** A response with two hundred events would not crash this card -
//   the filter keeps each event to three short fields - but it would take a
//   pool allocation this device has no business making. If unbounded responses
//   are possible, the endpoint should cap its own list; that is a one-line
//   server change and a much safer place for the bound than here.
//
//   STATUS CODES: 200 handled as above. 401 -> AuthError. 403 -> parsed as a
//   ContentProviderGate refusal body ({"reason","message"}), the same shape
//   every other device-facing content route produces - carried even though
//   KnownCards lists this card as RequiredContent.None today, because the
//   twenty lines cost nothing and a gate added later would otherwise read as
//   a generic network fault. 404 -> NotImplemented, the quiet "this server
//   predates the route" state. Anything else -> NetworkError.
//
// =============================================================================
// BOUNDS, and why these particular numbers.
// =============================================================================
//
//   kMaxEvents = 3.
//
//     The screen argument and the memory argument happen to agree, which is
//     the convenient part. On a 320x240 panel a calendar card showing more
//     than a few events is unreadable, and the scheduler shows ONE event per
//     dwell anyway (this is a list card - see the descriptor at the bottom), so
//     a fourth event is not a fourth line on a crowded screen, it is a fourth
//     ten-second slot the rotation has to spend before it gets back to
//     anything else. Three is "what's next, and what's after that" - the
//     question a wall display answers - rather than an agenda.
//
//     The memory side: this device runs with roughly 20KB of free 8-bit heap
//     in normal operation and the largest contiguous block has been measured
//     as low as 4,596 bytes (see Http.h's own field data, and Graphic.h on the
//     two devices whose TLS handshakes failed with SSL_ALLOC_FAILED). Three
//     Events at 49 bytes of title plus a time_t and a bool is roughly 168
//     bytes of .bss for the retained store - which Cards.h correctly points
//     out is "heap this device does not get back", but at that size it is
//     noise. What three actually buys is that nothing here ever needs a heap
//     block: no String is retained, no buffer is grown per fetch, and the only
//     transient allocation in the whole module is ArduinoJson's own pool
//     during the parse.
//
//   kMaxTitleLength = 48.
//
//     Set by what fits on the glass, not by bytes. Titles are drawn through
//     Display::showAnnouncementCard(), whose larger tier is FreeSansBold12pt7b
//     wrapped to five 24px lines across a 300px body - call it 23 characters
//     a line, so ~115 characters before it drops to the smaller, denser tier.
//     The date/time prefix this card prepends ("Tomorrow 09:00 - ") is up to
//     about 20 characters, so 48 leaves the whole card comfortably inside the
//     large tier with room for the wrap to break badly and still fit. A longer
//     bound would buy nothing readable and would start pushing ordinary events
//     down to the small font for no gain.
//
//     **Over-long titles are TRUNCATED with a visible "...", not dropped.**
//     That deliberately diverges from Cards.h's rule for policy `text` and
//     `qrData`, which are dropped rather than truncated on the grounds that
//     "silence is a more honest failure than a sentence chopped off mid-word".
//     That rule is right for those fields and wrong for this one. An admin's
//     notice can invert its meaning when clipped ("no school tomorrow" ->
//     "no school"); an event title clipped to "Dentist appt with Dr Marsha..."
//     is still instantly recognisable to the one household it belongs to, and
//     dropping it would silently lose them an appointment. The trailing "..."
//     is what keeps the truncation honest - it never reads as a complete
//     sentence. Three ASCII dots rather than an ellipsis glyph, for the same
//     reason IssFlyover.cpp spells out "deg" instead of using "\xB0": this
//     panel's bitmap fonts are not trusted to carry anything outside ASCII.
//
// =============================================================================
// THE EMPTY STATES, and why this card is silent where Aircraft and Listings
// are not.
// =============================================================================
//
// Graphic.h states the principle: a card with nothing to show reports zero
// items and stays out of the rotation entirely, because "there is nothing
// informative to say about a picture that isn't there, and a card reading 'no
// image configured' in a household's living room would be a worse outcome than
// one that simply never appears". Aircraft and Listings take the opposite
// route - their "nothing overhead within 10 mi" and "no homes for sale near
// Charlotte, NC right now" screens are real content, as Cards.h's ItemCountFn
// remarks allow.
//
// **This card follows Graphic, not Aircraft. It draws only real events, and is
// silent in every other state - including outright failures.** Three reasons,
// in increasing order of how much they decided it:
//
//   1. There is no fact in the absence. Aircraft's empty state says something
//      true about the world that changed since last time and will change
//      again: nothing is within the radius right now. "No calendar connected"
//      says nothing the household does not already know, cannot be acted on
//      from the sofa, and for most devices in this fleet is a PERMANENT
//      condition - the card would occupy a rotation slot with an unchanging
//      nag forever. That is precisely Graphic.h's argument.
//
//   2. Silence on failure is strictly better than an error card here, because
//      the fallback is real content. A failed fetch does NOT clear the events
//      already held (see cardFetch()) - a ten-minute network blip leaves
//      yesterday's answer on screen, which is more useful to a household than
//      "Could not load calendar", and the stale entries age themselves out of
//      isStillUpcoming() regardless of whether any fetch ever succeeds again.
//      Only an authoritative statement from the server (isConnected:false, or
//      an empty events array) clears the store, because only those two are the
//      server actually saying "there is nothing".
//
//   3. Silence on screen is NOT silence to the operator. Cards.h added StatusFn
//      for exactly this failure mode - a card sitting in one refused state for
//      twenty minutes while the debug stream says nothing - and cardStatus()
//      below reports every one of the eight states by name, re-asserted once
//      per check-in whether or not it changed. So "this device's calendar card
//      never appears" is a question with a one-line answer in the stream,
//      which is where an admin is looking anyway; it was never going to be
//      diagnosed from an amber card in someone's kitchen.
//
// A rejected fourth option, recorded because it is the obvious one: draw a
// muted "No calendar connected" via Display::showNoContent(). Rejected on (1)
// above, and separately because showNoContent() is documented as the screen
// for "no registered card has anything to draw AT ALL" - borrowing it for one
// card's resting state would make a genuinely empty device indistinguishable
// from a working one with an unconnected calendar.
//
// =============================================================================
// A NOTE ON THE DRAW SURFACE.
// =============================================================================
//
// This module draws through Display::showCalendarCard(), a purpose-built entry
// point beside showTidesCard()/showListingsCard(): its own CALENDAR banner in
// its own colour, and an item-number caption so a household mid-cycle can see
// there is another event behind this one.
//
// It was not always so. The first version of this file was written under a
// constraint not to touch Display.cpp/Display.h, and so borrowed
// showAnnouncementCard() - mechanically a good fit (two font tiers, five
// lines, word wrap, the theme's own ink and background) but stamped with the
// green NOTICE banner and with no "2 of 3" caption. Both of those are gone.
// showCalendarCard() reuses that same two-tier wrap verbatim, so the text
// layout describeEvent() below was tuned against is unchanged; only the banner
// and the caption are new.
//
// =============================================================================
// UNVERIFIED. This firmware has no automated tests and cannot get any. Nothing
// below has been compiled by the author (CI compiles every push) and nothing
// has run against a real response, because no real response exists yet.
// Correctness here comes from reading and from the reasoning above.
// =============================================================================

namespace Calendar {

const char* const kCardId = "calendar";

namespace {

constexpr const char* kPath = "/api/mycalendar/mine";

/// How long after its start a timed event keeps showing. An event that began
/// twenty minutes ago is very often the single most relevant thing on this
/// screen - you are in it, or late for it - and dropping it the instant the
/// clock passes its start would blank the card at exactly the wrong moment.
///
/// A fixed hour rather than reading the event's real end time: endUtc is on the
/// server's own summary type and could trivially be sent, but requesting it
/// means holding a second personal timestamp per event for a marginal gain over
/// a constant (most events are an hour or less, and this card is answering
/// "what's next", not "am I still in this meeting"). Not holding a field is the
/// cheapest way to not leak it - see Calendar.h.
constexpr int32_t kTimedEventGraceSeconds = 60 * 60;

/// An event starting within this window earns the longer notableDwellSeconds
/// hold - the direct analogue of Aircraft.cpp giving a plane nearly overhead a
/// longer look. Ninety minutes is roughly "you should be thinking about
/// leaving", which is the moment this card is worth pausing on.
constexpr int32_t kNotableWithinSeconds = 90 * 60;

constexpr int32_t kSecondsPerDay = 24 * 60 * 60;

// ---------------------------------------------------------------------------
// Retained state.
//
// Separate from the Result fetchMine() returns, rather than the single `gLast`
// every other fetching card keeps, precisely because of the keep-on-failure
// rule in this file's empty-state notes: the events and the last status have
// different lifetimes here. A NetworkError updates gLastStatus and leaves
// gEvents exactly as it found them.
// ---------------------------------------------------------------------------

Event gEvents[kMaxEvents];
uint8_t gCount = 0;

Status gLastStatus = Status::NetworkError;
String gLastMessage;
bool gEverFetched = false;

/// Logged only on a change, the same "don't put the same line in the remote
/// debug stream every ten minutes forever" dedup Tides.cpp's gLastLoggedHighTide
/// and IssFlyover.cpp's gLastLoggedHadData use. Two values because a card that
/// goes 3 events -> 2 events has changed in a way worth one line, and a card
/// that goes Ok -> NetworkError has too.
int16_t gLastLoggedCount = -1;
Status gLastLoggedStatus = Status::NetworkError;
bool gHasLoggedOnce = false;

/// Days from the civil epoch (1970-01-01) to the given UTC calendar date -
/// Howard Hinnant's days_from_civil, copied verbatim from CheckIn.cpp's own
/// copy along with parseIso8601Utc() below.
///
/// Duplicated rather than shared, the same call this codebase already made
/// twice: Actions.cpp keeps its own nowAsIso8601Utc() rather than reaching into
/// CheckIn.cpp's, and Tides.cpp keeps its own toLocalMinutes() rather than
/// SunMoon.cpp's. Both helpers are file-local statics in an anonymous namespace
/// in CheckIn.cpp, so sharing them would mean a new header and a new
/// translation unit to hold two pure functions - and this module must not edit
/// CheckIn.* at all.
long daysFromCivil(int year, int month, int day) {
  year -= month <= 2 ? 1 : 0;
  const long era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
  const unsigned dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return era * 146097 + static_cast<long>(dayOfEra) - 719468;
}

/// Parses the server's "2026-09-11T18:30:00Z"-style ISO-8601 UTC instant into
/// epoch seconds. Returns 0 - this module's "absent" sentinel, matching
/// Event::startUtc's own convention - for a JSON null, a missing field, or
/// anything this sscanf() cannot read.
///
/// Deliberately tolerant rather than asserting, for the reason CheckIn.cpp
/// gives for its identical copy: a malformed instant is exactly as much "no
/// answer" to this device as a JSON null is. Note the sscanf ignores whatever
/// follows the seconds, so a trailing "Z", "+00:00" or ".123" all parse the
/// same - which is the tolerance that keeps this working against a server that
/// changes its serializer settings.
time_t parseIso8601Utc(const char* text) {
  if (text == nullptr || text[0] == '\0') {
    return 0;
  }
  int year, month, day, hour, minute, second;
  if (sscanf(text, "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6) {
    return 0;
  }
  const long days = daysFromCivil(year, month, day);
  return static_cast<time_t>(days) * 86400 + hour * 3600 + minute * 60 + second;
}

/// Whether this device's clock is good enough to do calendar arithmetic with.
///
/// Every other card here either does no date maths (Graphic, Announcement) or
/// draws whatever the clock says and lets a wrong answer be visibly wrong
/// (ClockDate). This card cannot: "Today" and "Tomorrow" are computed by
/// comparing day numbers, so an unsynchronised clock would not render a wrong
/// time, it would render a confidently wrong WORD, and a card that says
/// "Tomorrow 09:00" about something three days ago is worse than no card. The
/// same kEarliestPlausibleTime bound CAL and AppService already use to reject
/// an implausible clock decides it here too.
bool clockIsUsable() { return time(nullptr) >= Config::kEarliestPlausibleTime; }

/// Which local calendar day an instant falls on, as a day number since the
/// epoch - the unit "Today"/"Tomorrow" are decided in.
///
/// **All-day events are deliberately NOT shifted by the UTC offset, and timed
/// events are.** This is the subtlest thing in the file. A timed event at
/// 18:30Z is a real instant, and a household on UTC-5 should see 13:30 - so it
/// gets the same `instant + utcOffsetMinutes*60` shift ClockDate.cpp and
/// IssFlyover.cpp's localHhMm() already use. An all-day event is not an
/// instant at all; it is a DATE, which the wire can only carry by picking a
/// midnight, and the assumption recorded in the contract block above is that
/// the midnight picked is UTC midnight on the event's own day. Applying a
/// negative offset to 2026-09-12T00:00:00Z would produce 2026-09-11T19:00, and
/// the household's anniversary would show up a day early on every device west
/// of Greenwich. Leaving it unshifted is what keeps a date a date.
///
/// If the server turns out to send local-midnight-as-UTC instead (i.e.
/// 2026-09-12T05:00:00Z for the 12th in UTC-5), this function is where that is
/// fixed, by shifting all-day events like every other one - and the symptom
/// will be all-day events appearing one day LATE rather than early.
long localDayIndexOf(time_t instantUtc, bool isAllDay) {
  const time_t shifted =
      isAllDay ? instantUtc
               : instantUtc + static_cast<time_t>(Display::utcOffsetMinutes()) * 60;
  // Floor division, not truncation: C's / rounds toward zero, which would put
  // every pre-1970 instant on the wrong side. No real event is pre-1970, but
  // parseIso8601Utc() returning garbage for a malformed year could be, and a
  // silently wrong day number is exactly the kind of thing nobody finds on
  // firmware with no tests.
  const long seconds = static_cast<long>(shifted);
  return (seconds >= 0) ? (seconds / kSecondsPerDay)
                        : -(((-seconds) + kSecondsPerDay - 1) / kSecondsPerDay);
}

/// Today, in the same day-number unit localDayIndexOf() returns. Always the
/// shifted (local) day - "today" is a wall-clock question - which is exactly
/// what localDayIndexOf() does for a non-all-day instant, so this is that call
/// rather than a second copy of the same arithmetic that could drift from it.
long todayLocalDayIndex() { return localDayIndexOf(time(nullptr), /*isAllDay=*/false); }

/// Whether an event is still worth showing.
///
/// Timed events survive kTimedEventGraceSeconds past their start; all-day
/// events survive until their own local day is over, which is a day-number
/// comparison rather than an arithmetic one precisely because the all-day
/// instant is not shifted (see localDayIndexOf()). Doing this as a live check
/// rather than pruning the array on a timer is what lets a device that has not
/// fetched successfully for an hour still age its own stale events out
/// correctly - see the keep-on-failure rule in this file's empty-state notes.
bool isStillUpcoming(const Event& event, time_t now) {
  if (event.startUtc <= 0) {
    return false;
  }
  if (event.isAllDay) {
    return localDayIndexOf(event.startUtc, true) >= todayLocalDayIndex();
  }
  return now < event.startUtc + kTimedEventGraceSeconds;
}

/// How many of the retained events are still upcoming right now.
uint8_t upcomingCount() {
  if (!clockIsUsable()) {
    return 0;
  }
  const time_t now = time(nullptr);
  uint8_t upcoming = 0;
  for (uint8_t i = 0; i < gCount; ++i) {
    if (isStillUpcoming(gEvents[i], now)) {
      upcoming++;
    }
  }
  return upcoming;
}

/// Maps a scheduler item index (0..upcomingCount()-1) onto the matching slot in
/// gEvents, skipping ones that have aged out. Returns -1 when there is no such
/// event.
///
/// This indirection is load-bearing, not tidiness. Events age out of the middle
/// of the array, not just off the front - an all-day entry can outlive a timed
/// one that came after it - so an itemIndex handed straight to gEvents[] would
/// draw an expired event whenever an earlier slot had expired first. The
/// scheduler asks for "the Nth thing you can show", and this is what makes that
/// the question being answered.
int8_t slotOfNthUpcoming(uint16_t itemIndex) {
  if (!clockIsUsable()) {
    return -1;
  }
  const time_t now = time(nullptr);
  uint16_t seen = 0;
  for (uint8_t i = 0; i < gCount; ++i) {
    if (!isStillUpcoming(gEvents[i], now)) {
      continue;
    }
    if (seen == itemIndex) {
      return static_cast<int8_t>(i);
    }
    seen++;
  }
  return -1;
}

/// Copies a server-sent title into an Event's fixed buffer: truncated to
/// kMaxTitleLength with a visible "..." (see the BOUNDS block for why truncate
/// rather than drop), and with every control character replaced by a space.
///
/// The control-character scrub is not defensive decoration. Display.cpp's
/// wrappedLeftText() breaks lines on ' ' and nothing else - a '\n' or '\t' in a
/// title would not wrap, it would be handed to drawString() and rendered as
/// whatever box glyph this panel's bitmap font has for it, in the middle of a
/// household's appointment. Calendar subjects pasted out of email carry these
/// routinely.
void copyTitle(char* destination, const char* source) {
  destination[0] = '\0';
  if (source == nullptr) {
    return;
  }

  const size_t sourceLength = strlen(source);
  const bool truncating = sourceLength > kMaxTitleLength;
  // Three characters of the budget go to the "..." when truncating, so the
  // result is always exactly kMaxTitleLength or shorter.
  const size_t copyLength = truncating ? (kMaxTitleLength - 3) : sourceLength;

  size_t written = 0;
  for (size_t i = 0; i < copyLength; ++i) {
    const unsigned char c = static_cast<unsigned char>(source[i]);
    destination[written++] = (c < 0x20 || c == 0x7F) ? ' ' : source[i];
  }
  if (truncating) {
    destination[written++] = '.';
    destination[written++] = '.';
    destination[written++] = '.';
  }
  destination[written] = '\0';
}

/// The one line this card draws, built from an event - "Tomorrow 09:00 -
/// Dentist", "Today, all day - Anniversary", "Fri 18 Sep 14:30 - Site visit".
///
/// 24-hour times with no AM/PM, matching every other time this firmware puts on
/// a screen (ClockDate's big clock, Tides, SunMoon, IssFlyover, and Display's
/// own corner clock). "Today"/"Tomorrow" rather than a date for the two days a
/// household actually cares about, which is the same "say the useful thing, not
/// the precise thing" choice IssFlyover.cpp makes in using an 8-point compass
/// instead of a real azimuth.
///
/// A plain " - " separator rather than two spaces or a dash glyph: wrappedLeftText()
/// treats every space as a wrap candidate, so a single-space-delimited string
/// wraps cleanly at any point, and the separator stays inside ASCII for the same
/// font reason the "..." above does.
String describeEvent(const Event& event) {
  const long eventDay = localDayIndexOf(event.startUtc, event.isAllDay);
  const long today = todayLocalDayIndex();

  // gmtime_r on an already-shifted time_t, the same idiom ClockDate.cpp and
  // IssFlyover.cpp's localHhMm() use rather than reaching for localtime() and a
  // TZ this firmware never sets. For an all-day event the "shift" is zero, by
  // localDayIndexOf()'s own rule.
  const time_t shown =
      event.isAllDay ? event.startUtc
                     : event.startUtc + static_cast<time_t>(Display::utcOffsetMinutes()) * 60;
  struct tm shownTm;
  gmtime_r(&shown, &shownTm);

  char when[40];
  if (event.isAllDay) {
    if (eventDay == today) {
      snprintf(when, sizeof(when), "Today, all day");
    } else if (eventDay == today + 1) {
      snprintf(when, sizeof(when), "Tomorrow, all day");
    } else {
      char dateText[24];
      // "%a %d %b" -> "Fri 18 Sep". C-locale English, like ClockDate.cpp's own
      // strftime - nothing in this firmware is localised, by design or
      // otherwise.
      strftime(dateText, sizeof(dateText), "%a %d %b", &shownTm);
      snprintf(when, sizeof(when), "%s, all day", dateText);
    }
  } else {
    if (eventDay == today) {
      snprintf(when, sizeof(when), "Today %02d:%02d", shownTm.tm_hour, shownTm.tm_min);
    } else if (eventDay == today + 1) {
      snprintf(when, sizeof(when), "Tomorrow %02d:%02d", shownTm.tm_hour, shownTm.tm_min);
    } else {
      char dateText[24];
      strftime(dateText, sizeof(dateText), "%a %d %b", &shownTm);
      snprintf(when, sizeof(when), "%s %02d:%02d", dateText, shownTm.tm_hour, shownTm.tm_min);
    }
  }

  const char* title = (event.title[0] != '\0') ? event.title : "(no title)";
  return String(when) + " - " + title;
}

// Identical shape and reasoning to Aircraft.cpp's/Listings.cpp's parseRefusal -
// the same ContentProviderGate produces this 403 body for every device-facing
// content route (see ContentProviderGate.cs). Carried here even though
// KnownCards lists "calendar" as RequiredContent.None today: if a content
// provider is ever put in front of this route, a device running this firmware
// already reports the refusal by name instead of calling it a network fault.
//
// Note what is NOT logged: the server's message is logged, the household's
// events are not - but by construction a refusal body carries no event content
// at all, so this is the one log line in the module that can safely echo a
// server string.
Result parseRefusal(const String& body) {
  Result result;
  result.status = Status::NetworkError;

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    result.message = "Cannot reach the calendar service.";
    return result;
  }

  const char* reason = doc["reason"] | "";
  const char* message = doc["message"] | "";
  result.message = strlen(message) > 0 ? String(message) : "This card is unavailable.";

  if (strcmp(reason, "device_not_activated") == 0) {
    result.status = Status::NotActivated;
  } else if (strcmp(reason, "content_provider_disabled") == 0) {
    result.status = Status::ProviderDisabled;
  }
  Log::printf("[calendar] refused (%s): %s", reason, result.message.c_str());
  return result;
}

}  // namespace

Result fetchMine() {
  Result result;

  if (!Http::ready()) {
    result.message = "Cannot verify the service's identity.";
    Log::line("[calendar] TLS setup failed");
    return result;
  }

  const String url = String("https://") + Config::kServiceHost + kPath;
  if (!Http::beginRequest(url)) {
    result.message = "Cannot reach the calendar service.";
    Log::line("[calendar] could not begin request");
    return result;
  }
  HTTPClient& http = Http::client();
  http.setTimeout(Config::kHttpTimeoutMs);
  http.addHeader("X-Device-Secret", Identity::deviceSecret());

  Log::verbose("[calendar] GET %s", url.c_str());
  const int status = http.GET();
  Log::verbose("[calendar] response status=%d", status);

  if (status == 401) {
    http.end();
    result.status = Status::AuthError;
    result.message = "Cannot verify this device. Contact support.";
    Log::line("[calendar] auth rejected (401)");
    return result;
  }

  if (status == 403) {
    const String body = http.getString();
    http.end();
    return parseRefusal(body);
  }

  // 404 is expected, not exceptional: this firmware is being written against a
  // server that does not have the route yet, and MANDATE-wise the reverse case
  // (a device up to six months old talking to a newer server) is the one that
  // must keep working - so a device that ships with this card and meets an
  // older server has to sit quietly rather than declare a fault every ten
  // minutes. Given its own status value for the same reason Listings.cpp gives
  // NotConfigured one: it is a resting state somebody could act on (deploy the
  // server half), and reading it as a failure sends whoever is watching the
  // debug stream after the wrong bug.
  if (status == 404) {
    http.end();
    result.status = Status::NotImplemented;
    result.message = "This server has no calendar endpoint yet.";
    return result;
  }

  if (status != 200) {
    http.end();
    result.message = "Cannot reach the calendar service.";
    Log::printf("[calendar] unexpected http status=%d", status);
    return result;
  }

  // The filter is a PRIVACY control here as much as a memory one - see
  // Calendar.h. Four keys and nothing else, so an event's location, end time,
  // attendees or body cannot reach this device's RAM even if a future server
  // starts sending them, and the account's own email address (which a
  // CalendarResult-shaped response would carry as connectedAccount) never
  // arrives either.
  //
  // "title" and "subject" are both whitelisted deliberately - see the contract
  // block at the top of this file for the field-name ambiguity that costs two
  // entries to sidestep.
  //
  // A single index anywhere inside "events" applies to every element, not just
  // index 0 - ArduinoJson's own filter semantics, the same way Listings.cpp
  // relies on it.
  //
  // Built once, on first fetch, and kept resident for the life of the device:
  // a JsonDocument takes a 1KB pool block the moment it holds anything, this
  // one's contents never vary, and re-taking that block every fetch is pure
  // churn on a device whose scarce resource is contiguous blocks. Same trade,
  // same reason as Aircraft.cpp's and Listings.cpp's own filters. Function-local
  // static, so it is constructed on first use rather than during static init
  // where the heap is in no state to be relied on.
  static const JsonDocument filter = [] {
    JsonDocument f;
    f["isConnected"] = true;
    f["events"][0]["startUtc"] = true;
    f["events"][0]["title"] = true;
    f["events"][0]["subject"] = true;
    f["events"][0]["isAllDay"] = true;
    return f;
  }();

  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    result.message = "The calendar service sent something unreadable.";
    Log::line("[calendar] response was not valid JSON");
    return result;
  }

  // Absent reads as true, so a server that never sends this field still works
  // and simply arrives at the empty-events case below instead - see the
  // contract block.
  const bool isConnected = doc["isConnected"] | true;
  if (!isConnected) {
    result.status = Status::NotConnected;
    result.message = "No calendar is connected to this account.";
    return result;
  }

  JsonArrayConst events = doc["events"].as<JsonArrayConst>();
  if (events.isNull() || events.size() == 0) {
    result.status = Status::Empty;
    result.message = "Nothing upcoming on the calendar.";
    return result;
  }

  // Filtering to "still upcoming" at PARSE time as well as at draw time is not
  // redundant. A server that sends the current day's events including ones that
  // already happened would otherwise spend this card's three slots on the
  // morning's finished meetings, and the draw-time check would then correctly
  // report zero items - a card that goes silent precisely because the server
  // sent it plenty of data. Dropping them here means the three slots hold three
  // events worth showing.
  const time_t now = time(nullptr);
  const bool clockUsable = clockIsUsable();
  uint16_t received = 0;
  for (JsonVariantConst entry : events) {
    received++;
    if (result.count >= kMaxEvents) {
      // Keep counting for the log line's "kept N of M", but stop storing.
      continue;
    }

    Event event;
    event.startUtc = parseIso8601Utc((const char*)(entry["startUtc"] | ""));
    if (event.startUtc <= 0) {
      // A malformed or absent instant drops that one event and keeps the rest,
      // the same tolerance parseIso8601Utc() itself applies - an event with no
      // time is not something this card can place on a day.
      continue;
    }
    event.isAllDay = entry["isAllDay"] | false;

    // "title" preferred, "subject" as the fallback - see the contract block.
    // The cast mirrors Aircraft.cpp's own `| ""` idiom, which reads a JSON null
    // exactly the same as a field the server never sends: both mean "nothing
    // here" to this client.
    const char* title = (const char*)(entry["title"] | "");
    if (title == nullptr || title[0] == '\0') {
      title = (const char*)(entry["subject"] | "");
    }
    copyTitle(event.title, title);

    // With no usable clock this device cannot tell an upcoming event from a
    // finished one, so it keeps them all and lets upcomingCount() report zero
    // until the clock syncs - rather than discarding real data on the strength
    // of a wrong "now".
    if (clockUsable && !isStillUpcoming(event, now)) {
      continue;
    }

    result.events[result.count++] = event;
  }

  if (result.count == 0) {
    result.status = Status::Empty;
    result.message = "Nothing upcoming on the calendar.";
  } else {
    result.status = Status::Ok;
  }

  // COUNTS ONLY. No title, no time, no location, no account - see the privacy
  // remarks in Calendar.h. "kept N of M" is the one genuinely useful diagnostic
  // this card can emit without leaking its own content: it distinguishes "the
  // server sent nothing" from "the server sent twelve and they had all already
  // happened" from "the server sent three and we kept three", which is the
  // whole space of questions an admin would ask here.
  Log::verbose("[calendar] response kept %u of %u event(s)", static_cast<unsigned>(result.count),
               static_cast<unsigned>(received));

  return result;
}

// ---------------------------------------------------------------------------
// The card descriptor. See the equivalent block at the bottom of
// Aircraft.cpp/Listings.cpp for why registration happens here rather than in
// App.ino - the registry exists so that adding a card type is registering a
// descriptor from its own translation unit, with no edit to the scheduler at
// all. This card is the first one added since Cards.h's kMaxCards spare slot
// was spent, so that bound moved from 28 to 29 alongside this registration,
// exactly as that constant's own remarks said the next new card would have to.
// ---------------------------------------------------------------------------
namespace {

/// Re-asserted once per check-in - see Cards.h's StatusFn, and this file's
/// empty-state notes for why this function carries more weight here than on any
/// other card: the screen is silent in seven of these eight states, so this line
/// is the ONLY way to tell a working-but-empty calendar from a refused one from
/// a server that has no route.
///
/// Reports counts and state names only. Never an event title, time or the
/// connected account - see Calendar.h.
String cardStatus() {
  if (!gEverFetched) {
    return "never fetched";
  }
  if (!clockIsUsable()) {
    // Worth its own line ahead of the status switch: with an unsynchronised
    // clock this card reports zero items no matter how good the last fetch was,
    // and "ok, 3 upcoming event(s)" beside a card nobody can see would send an
    // admin looking in the wrong place entirely.
    // String(...) first, not a bare literal: Arduino's WString has no
    // `const char* + String` operator, only String-on-the-left.
    return String("clock not synchronised - holding ") + gCount + " event(s), showing none";
  }
  switch (gLastStatus) {
    case Status::Ok:
      return String("ok, ") + upcomingCount() + " upcoming event(s)";
    case Status::Empty:
      return "ok, nothing upcoming";
    case Status::NotConnected:
      return "resting: no calendar connected to this account";
    case Status::NotImplemented:
      return "resting: this server has no calendar endpoint yet";
    case Status::NotActivated:
      return "refused: device not activated";
    case Status::ProviderDisabled:
      return "refused: provider disabled";
    case Status::AuthError:
      return "refused: device secret rejected";
    case Status::NetworkError:
      // Names how many stale events are still being shown, because that is the
      // difference between "this card is broken" and "this card is riding out a
      // blip on data it already had" - see the keep-on-failure rule.
      return String("network error (still showing ") + upcomingCount() + " held event(s)): " +
             gLastMessage;
  }
  return "unknown";
}

void cardFetch() {
  const Result fresh = fetchMine();
  gEverFetched = true;
  gLastStatus = fresh.status;
  gLastMessage = fresh.message;

  // The keep-on-failure rule, which is the whole reason this module keeps its
  // retained events separately from the last Result. Only the server actually
  // SAYING there is nothing clears the store; a failure to ask leaves what we
  // had, and isStillUpcoming() ages it out on its own schedule regardless.
  if (fresh.status == Status::Ok) {
    for (uint8_t i = 0; i < fresh.count; ++i) {
      gEvents[i] = fresh.events[i];
    }
    gCount = fresh.count;
  } else if (fresh.status == Status::Empty || fresh.status == Status::NotConnected) {
    gCount = 0;
  }

  // Change-only, the same dedup Tides.cpp and IssFlyover.cpp use: cardFetch()
  // runs every kContentRefreshIntervalMs (10 minutes) forever, and an
  // unconditional line here would be the same sentence in the remote debug
  // stream six times an hour for the life of the device. Cards.h's
  // logProviderStatuses() is what re-asserts the current state on a cadence;
  // this is only for the moment it changed.
  const int16_t freshCount = static_cast<int16_t>(gCount);
  if (!gHasLoggedOnce || freshCount != gLastLoggedCount || fresh.status != gLastLoggedStatus) {
    gHasLoggedOnce = true;
    gLastLoggedCount = freshCount;
    gLastLoggedStatus = fresh.status;
    // Counts and a fixed phrase only - see Calendar.h.
    Log::printf("[calendar] card updated: %s, holding %u event(s)", cardStatus().c_str(),
                static_cast<unsigned>(gCount));
  }
}

/// The number of events still upcoming, and zero for absolutely everything
/// else - no "not connected" screen, no "nothing upcoming" screen, no error
/// screen. See this file's empty-state block for the full argument; the short
/// version is Graphic.h's: a card with nothing to show reports zero items and
/// stays out of the rotation rather than putting an unactionable message in a
/// household's living room, and Cards.h's StatusFn is what keeps that silence
/// from also being silence to whoever is debugging.
uint16_t cardItemCount() { return upcomingCount(); }

/// An event starting within kNotableWithinSeconds earns the longer dwell - the
/// direct analogue of Aircraft.cpp giving a plane nearly overhead a longer
/// look, and Listings.cpp giving a just-listed house one. All-day events are
/// never notable: "sometime today" has no urgency to signal, and treating one
/// as imminent for the whole day would make the longer dwell meaningless on
/// exactly the days it should mean something.
bool cardIsNotable(uint16_t itemIndex) {
  const int8_t slot = slotOfNthUpcoming(itemIndex);
  if (slot < 0) {
    return false;
  }
  const Event& event = gEvents[slot];
  if (event.isAllDay) {
    return false;
  }
  const time_t now = time(nullptr);
  return event.startUtc > now && (event.startUtc - now) <= kNotableWithinSeconds;
}

void cardDraw(uint16_t itemIndex) {
  int8_t slot = slotOfNthUpcoming(itemIndex);
  if (slot < 0) {
    // cardItemCount() is what keeps the scheduler from calling this with
    // nothing to draw, the same tolerance showAnnouncementCard() already gets
    // from Announcement.cpp's own cardItemCount(). Reaching here means an event
    // aged out between the count and the draw - a real race on a card whose
    // items expire on a clock rather than on a fetch - so fall back to whatever
    // is still showable rather than drawing a blank card.
    slot = slotOfNthUpcoming(0);
    if (slot < 0) {
      return;
    }
  }

  // What is on screen this draw, not what the last fetch found - the two
  // diverge across a rewind, where this runs again with no fetch behind it.
  // COUNTS AND FLAGS ONLY: the index, the total and whether it is an all-day
  // entry. Not the title, not the time. See Calendar.h - the remote debug
  // stream is a log, and this is the module whose content may never reach one.
  Log::verbose("[calendar] drawing item %u/%u (allDay=%s)",
               static_cast<unsigned>(itemIndex) + 1, static_cast<unsigned>(upcomingCount()),
               gEvents[slot].isAllDay ? "yes" : "no");

  // itemIndex is the scheduler's 0-based position; the caption is 1-based, and
  // upcomingCount() rather than a stored total because the count is read at
  // draw time for the same reason the slot is - an event can age out between
  // the two, and "2 of 2" is right where "2 of 3" would be a lie.
  Display::showCalendarCard(describeEvent(gEvents[slot]),
                            static_cast<uint8_t>(itemIndex + 1),
                            static_cast<uint8_t>(upcomingCount()));
}

// A *list* card, like Listings and unlike every interstitial: the server
// returns several events, cardItemCount() reports the real number and
// cardDraw() reads the index it is given, so the scheduler cycles through them
// one per dwell with no scheduler change of any kind. Registering it as an
// interstitial would be claiming it is one fixed fact, which it is not.
//
// order = 1, ahead of aircraft (2), listings (3) and the five forecast
// instances (4) among list cards. The household's own schedule is the most
// personally relevant thing this device can show and the thing with the
// shortest useful life - an appointment is worth seeing before it happens,
// where a house listing and a forecast are worth seeing whenever. 0 is left
// unclaimed as CardSpec::order's own "nobody said" default rather than taken
// here, so a card that never sets an order still sorts distinctly. Every value
// in this block is a built-in default that holds only until the first
// cardPolicy from the server replaces it wholesale.
//
// dwellSeconds = 10 matches listings and forecast - one date, one time and a
// short title is about as much as a listing's address and price, and reads in
// about as long from across a room. notableDwellSeconds = 18 likewise mirrors
// listings' own, for an event about to start.
[[maybe_unused]] const bool kRegistered = [] {
  Cards::CardSpec spec;
  spec.id = kCardId;
  spec.kind = Cards::Kind::List;
  spec.fetch = cardFetch;
  spec.itemCount = cardItemCount;
  spec.draw = cardDraw;
  spec.isNotable = cardIsNotable;
  spec.status = cardStatus;
  spec.order = 1;
  spec.dwellSeconds = 10;
  spec.notableDwellSeconds = 18;
  return Cards::registerCard(spec);
}();

}  // namespace

}  // namespace Calendar
