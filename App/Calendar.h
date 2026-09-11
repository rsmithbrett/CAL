#pragma once

#include <Arduino.h>
#include <time.h>

/// The household's own upcoming calendar events - a server-fetched content
/// type sitting beside Listings.h and Aircraft.h as its own module, per
/// Weather.h's own remarks on why cards get sibling files rather than growing
/// into one another. Like them, Calendar.cpp registers its own card descriptor
/// with the scheduler (see Cards.h) from its own translation unit, so nothing
/// in App.ino names this card.
///
/// **This card exists because the server already advertises it and firmware
/// did not have it.** `KnownCards.cs` has carried a `"calendar"` entry for a
/// while - "Upcoming events from whichever calendars the account has connected
/// and made eligible to display - Google and Microsoft/Outlook 365 both, merged
/// into one list" - and an operator configured it on a real device, which
/// answered by logging `[cards] policy names unknown card 'calendar' -
/// ignored` on every check-in. That log line is the tolerance Cards.h's own
/// `CardSpec::id` remarks describe working exactly as designed (a server may
/// name a card firmware does not implement), and this module is the other half
/// of it arriving.
///
/// It registers as a **list** card in the literal sense, like Listings and
/// unlike Aircraft: the server returns several events, this module keeps up to
/// kMaxEvents of them, and the scheduler cycles through them one per dwell.
///
/// ---------------------------------------------------------------------------
/// PRIVACY, which is the constraint that shapes this module more than memory
/// does.
/// ---------------------------------------------------------------------------
///
/// The server's own `CalendarEventSummary` carries an explicit rule that this
/// firmware is bound by just as much as the server is:
///
///   "PRIVACY: every field here is personal content. Subject lines routinely
///   carry medical appointments, custody arrangements, legal matters. Nothing
///   in this type may ever be written to a log, an audit record, or an
///   exception message."
///
/// On this device the remote debug stream (Log.h) IS a log - it is POSTed to
/// the server, stored, and read later by an admin who is not necessarily the
/// household. So **no event title, event time, location or account name is
/// ever passed to Log::printf/verbose/line anywhere in Calendar.cpp**, not
/// even behind `Log::verbose`'s streaming-only gate. Everything this module
/// logs is a count, an HTTP status, or a fixed phrase. That is a deliberate
/// and deliberate-looking asymmetry with every other card in this tree
/// (Listings logs the address it drew, Aircraft the callsign), and it is the
/// reason the debug output here reads thinner than its siblings' - it is not
/// an oversight to be "fixed" by a later change.
///
/// The same rule shapes what is even asked for: the fetch's ArduinoJson filter
/// whitelists three fields per event and nothing else, so an event's location,
/// end time, attendees or body cannot reach this device's RAM even if a future
/// server starts sending them. Not holding them is the cheapest way to not
/// leak them - the same argument the server-side type already makes for not
/// requesting them from Graph.
namespace Calendar {

enum class Status {
  Ok,
  /// The fetch succeeded, a calendar is connected, and there is simply nothing
  /// upcoming - not an error, and one of this card's two ordinary resting
  /// states.
  Empty,
  /// The account has no calendar connected (or none made eligible to display).
  /// The other ordinary resting state, and the one most devices in the fleet
  /// will sit in forever - see the empty-state reasoning in Calendar.cpp.
  NotConnected,
  /// The server answered 404: it predates the calendar route entirely. A
  /// first-class value rather than folding into NetworkError because it is a
  /// permanent, uninteresting fact about that server, not a fault to chase -
  /// and this firmware will ship against exactly such a server (the endpoint
  /// is being built in parallel with this module). Same first-class-resting-
  /// state reasoning as Listings::Status::NotConfigured.
  NotImplemented,
  NotActivated,      // ContentGateRefusal.DeviceNotActivated
  ProviderDisabled,  // ContentGateRefusal.ProviderDisabled
  AuthError,         // the device's own secret was rejected
  NetworkError,      // couldn't reach the service, or the response made no sense
};

/// This card's registered id, and the `id` a cardPolicy entry must use to
/// schedule it. Exposed only so that the id appears exactly once in the
/// firmware, the same reason Tides.h and IssFlyover.h expose theirs. It must
/// stay exactly "calendar": that is the id KnownCards.cs already advertises and
/// the one an operator has already configured on a real device.
extern const char* const kCardId;

/// How many upcoming events this card keeps and cycles through, and how long
/// an event's title may be once stored. See the "BOUNDS" block at the top of
/// Calendar.cpp for the full derivation of both numbers - the short version is
/// that three events is what a 320x240 panel can show without becoming an
/// agenda nobody reads, and 48 characters is what fits beside a date/time
/// prefix at the larger of Display::showAnnouncementCard()'s two font tiers.
static constexpr uint8_t kMaxEvents = 3;
static constexpr uint8_t kMaxTitleLength = 48;

/// One upcoming event, trimmed to the three fields this card draws.
///
/// Fixed `char` buffer rather than an Arduino `String`, unlike
/// Listings::ListingInfo and Aircraft::Sighting, which both retain Strings.
/// This is a deliberate divergence, not an inconsistency: the retained store
/// lives for the life of the device and the largest contiguous 8-bit block on
/// this fleet has been measured as low as 4,596 bytes, so three
/// independently-allocated heap strings being freed and re-allocated on every
/// ten-minute refresh is churn in exactly the arena that cannot afford it.
/// A fixed buffer moves the cost to .bss - roughly 168 bytes for the whole
/// array, permanently, and never fragmenting anything. See Cards.h's own
/// remarks on kMaxAnnouncements for the same trade made for the same reason.
struct Event {
  /// Already truncated to kMaxTitleLength with a visible "..." if the server
  /// sent something longer, and already stripped of control characters - see
  /// copyTitle() in Calendar.cpp.
  char title[kMaxTitleLength + 1] = "";
  /// Absolute UTC epoch seconds, parsed from the wire's ISO-8601 instant. 0 is
  /// the "absent/unparseable" sentinel, the same convention
  /// CheckIn::Result::issNextPassRiseUtc uses and for the same reason: a real
  /// event is never exactly the Unix epoch.
  time_t startUtc = 0;
  /// True for a whole-day entry ("Anniversary"), which is drawn as a date with
  /// no clock time AND is deliberately not shifted by the device's UTC offset -
  /// see localDayIndexOf() in Calendar.cpp for why shifting an all-day event
  /// would move it to the wrong day for every western timezone.
  bool isAllDay = false;
};

struct Result {
  Status status = Status::NetworkError;
  uint8_t count = 0;
  Event events[kMaxEvents];
  /// Set on every non-Ok status. **Never contains event content** - it is a
  /// fixed phrase or a server-supplied refusal message, and it is the one
  /// string from this module that may reach a log. See this file's privacy
  /// remarks above.
  String message;
};

/// GETs /api/mycalendar/mine with the device's own secret and no device id
/// anywhere in the request - identical authentication to Listings::fetchMine()
/// and every other request the App makes (see MyWeatherEndpoints.cs's remarks
/// on why "mine" replaced an id-bearing route).
///
/// Only ever returns events that are still upcoming as of the moment of the
/// call, already capped at kMaxEvents. See the contract block at the top of
/// Calendar.cpp for the exact wire field names this assumes.
Result fetchMine();

}  // namespace Calendar
