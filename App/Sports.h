#pragma once

#include <Arduino.h>
#include <time.h>

/// Sports scores and fixtures, as cards - structurally the same
/// "check-in-pushed fact" shape as Tides.h, IssFlyover.h and HomeValue.h, and
/// for the same reason: everything drawn here arrives on the check-in response
/// the device already makes, and nothing is fetched from this file.
///
/// **This card fetches nothing.** The server holds the API key, spends the
/// daily request budget, resolves which team or league each card instance
/// follows, and sends back exactly the games that card should draw - already
/// filtered, ordered and capped. The device draws what it is given and makes no
/// selection of its own. See SPORTS_CARDS_DESIGN.md on the server side.
///
/// **FOUR CARD INSTANCES, NOT ELEVEN.** The server's catalog advertises eleven
/// sports ids (sportsteam1-5, sportsscores1-5, sportslive). This firmware
/// registers four of them, and that is a deliberate decision rather than an
/// unfinished one:
///
///   sportsteam    one followed team
///   sportsteam2   a second followed team
///   sportsscores  one league's games today
///   sportslive    appears only while a followed team is playing
///
/// Every registration costs a CardSpec and a PolicyEntry slot, and raising
/// Cards::kMaxCards to cover all eleven would add roughly 1.6 KB of .bss on a
/// board whose largest contiguous 8-bit block has been measured near the
/// 16,717-byte TLS floor. A device pushed under that floor cannot open a TLS
/// session, so it cannot check in, so it cannot be told an update exists -
/// which makes .bss spent on card slots nobody configured the most expensive
/// kind of unused memory this codebase has. A policy naming one of the other
/// seven is ignored with a log line, exactly as a policy naming "weather"
/// already is, and instances 3-5 become a later registration if anybody asks
/// for them.
///
/// **FIXED BUFFERS, NOT String.** Every other card here uses Arduino String
/// freely, and this one deliberately does not. Four cards x four games x two
/// team names is thirty-two String allocations churning on every check-in, on
/// the exact heap whose LARGEST CONTIGUOUS BLOCK - not its total free bytes -
/// is what decides whether TLS can open. Fragmenting that block is a worse
/// failure than using a little more of it, so the storage below is a flat,
/// preallocated array whose size never changes after boot: predictable, and
/// invisible to the fragmentation that binds.
///
/// **Absent is a real answer**, the same tolerance Tides.cpp, MoonPhase.cpp and
/// HomeValue.cpp already give. A team card with no game today, a league with an
/// empty schedule, and sportslive on the overwhelming majority of days all
/// report zero items and drop out of the rotation rather than drawing an empty
/// frame. For sportslive that is not an edge case, it is the card's whole
/// design: it exists to appear when something is happening and to be absent
/// otherwise.
///
/// **The server caps the games, and the device trusts the cap.** Four per card,
/// set by SportsOptions.MaxGamesPerScoreboard for a heap reason rather than a
/// layout one - eight games is roughly 1.5-2 KB of JSON and ArduinoJson needs
/// about the payload size again to parse it. kMaxGames below matches that cap;
/// anything beyond it is dropped on arrival with a log line rather than
/// silently truncated, because a server sending five when firmware stores four
/// is a contract drift somebody should see.
namespace Sports {

/// Longest team name stored. Names arrive short from the server ("Orioles",
/// not "Baltimore Orioles"), and a 320x240 screen cannot show more than this
/// beside a score anyway.
static constexpr uint8_t kMaxTeamNameLength = 20;

/// Longest progress string stored - "T7", "Q3", "78'", "HT". The provider's own
/// text, passed through unparsed by the server and undrawn-upon here.
static constexpr uint8_t kMaxPeriodLength = 8;

/// Longest sport token stored. The server sends API-Sports' own slugs, and
/// "american-football" at 17 is the longest of the five. Stored as the slug and
/// turned into a chip label when it is drawn, so the wire stays a token and a
/// sixth sport costs no new field.
static constexpr uint8_t kMaxSportLength = 17;

/// Longest competition name stored, matching the cap the server applies before
/// sending (CheckInGatewayService.MaxCompetitionLabelLength). 24 clears "UEFA
/// Champions League" at 21.
static constexpr uint8_t kMaxCompetitionLength = 24;

/// Games stored per card. Matches the server's own cap; see the class remarks.
static constexpr uint8_t kMaxGames = 4;

/// Card instances this firmware registers. Four, deliberately - see the class
/// remarks for why not eleven.
static constexpr uint8_t kMaxCards = 4;

/// What a game is doing. Mirrors the server's four states, which are themselves
/// a deliberate collapse of each provider's much longer vocabulary.
enum class State : uint8_t {
  /// Not started. The card shows a start time.
  Scheduled,
  /// In progress. The only state whose staleness somebody standing in front of
  /// the screen would notice.
  Live,
  /// Finished, by whatever route. The card shows a final score.
  Final,
  /// Postponed, cancelled or abandoned. Distinct from Scheduled because a start
  /// time beside a game that will not happen is worse than saying nothing, and
  /// from Final because there is no score.
  Postponed,
  /// The server sent a state string this firmware does not know. Treated as
  /// "draw the teams, claim nothing about timing" rather than guessed at - a
  /// wrong Live is the costliest guess, because it invites somebody to watch a
  /// score that will never move.
  Unknown,
};

/// One game as stored. Scores are int16_t with kNoScore for "not started",
/// because null and zero are different facts and a nil-nil draw is real.
static constexpr int16_t kNoScore = -1;

struct Game {
  time_t startsAtUtc = 0;
  State state = State::Unknown;
  char period[kMaxPeriodLength + 1] = "";
  char home[kMaxTeamNameLength + 1] = "";
  char away[kMaxTeamNameLength + 1] = "";
  int16_t homeScore = kNoScore;
  int16_t awayScore = kNoScore;
};

/// Replace one card instance's games with what the check-in just delivered.
///
/// Called from App.ino's performCheckIn() alongside Tides::setTimes() and
/// HomeValue::setValue(). Passing zero games is how a card is emptied, and is
/// the normal state for sportslive nearly all the time.
///
/// `cardId` must be one of the four registered ids; anything else is logged and
/// dropped, so a server advertising an instance this firmware does not register
/// costs a log line rather than a silent misfile onto the wrong card.
///
/// `staleSinceUtc` is epoch seconds for when the server last actually read the
/// provider, and **0 is the ordinary value and means "fresh"**. The server
/// sends `staleSinceUtc` only when it judges the answer old - a refresh that
/// was attempted, came back empty, and left a previous answer standing - and
/// omits the key entirely otherwise (SportsCardPayload.StaleSinceUtc, added
/// server-side 2026-09-27). Absence is therefore not a gap to go looking for;
/// it is the server saying this card is current.
///
/// **The server judges, not this device.** CARD_ABSENCE_AND_AGE_DESIGN.md
/// section 8 reverses section 5 on exactly this point: the server is the side
/// that knows the refresh interval the answer was meant to live inside, and a
/// staleness threshold compiled into firmware could only be changed by
/// shipping firmware. So there is no threshold constant here, no clock
/// arithmetic to decide staleness, and no second field carrying a flag - one
/// nullable timestamp, and its absence means fresh.
///
/// ONE TIMESTAMP PER CARD, not per game, also from section 8: the question a
/// reader has is whether the screen in front of them is current, not which of
/// four fixtures refreshed most recently.
/// `sport` is API-Sports' slug for the sport this card follows - "basketball",
/// "american-football" - and `competition` is the league's own display name,
/// "NFL" or "UEFA Champions League". Both are card-level for the same reason
/// the timestamp is: a card follows one league, and the question a reader has
/// is what they are looking at rather than which fixture carries which name.
///
/// Both default to empty, which is what a server predating SportsCardPayload's
/// `sport` and `competition` fields (added 2026-10-09) sends. Empty draws no
/// chip and no competition line rather than a placeholder, so the card renders
/// exactly as it did before those fields existed.
void setGames(const char* cardId, const Game* games, uint8_t count, time_t staleSinceUtc,
              const char* sport = "", const char* competition = "");

/// The chip text for a sport slug: "FOOTBALL" for american-football, "SOCCER"
/// for football, and the slug upper-cased for the other three. An unrecognised
/// slug returns empty, so a sport this firmware has never heard of draws no
/// chip instead of a lowercase hyphenated one.
///
/// The two football codes are the whole reason this mapping exists. API-Sports
/// calls soccer "football", and a chip reading FOOTBALL over a soccer score is
/// the one labelling mistake a viewer here would actually act on.
String sportChipLabel(const char* sport);

/// Drop everything. Called when a check-in reports no sports content at all, so
/// yesterday's fixtures cannot linger on screen past their day.
void clearAll();

}  // namespace Sports
