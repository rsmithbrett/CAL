#include "Sports.h"

#include <string.h>

#include "Cards.h"
#include "Display.h"
#include "Log.h"

namespace Sports {
namespace {

/// The four ids this firmware registers, in rotation order. Indexed by the same
/// number as gCards below - the two arrays are parallel and must stay so, which
/// is why the ids live here once rather than being repeated at each
/// registration.
const char* const kCardIds[kMaxCards] = {
    "sportsteam",
    "sportsteam2",
    "sportsscores",
    "sportslive",
};

/// One card's worth of games. Flat and preallocated: see Sports.h on why this
/// is not a vector of Strings.
struct CardSlot {
  Game games[kMaxGames];
  uint8_t count = 0;

  /// When the server last read the provider, or 0 for "the server did not flag
  /// this card as old", which is the ordinary state - see setGames() in
  /// Sports.h. Four bytes per card instance, sixteen across the four: the
  /// entire .bss cost of this feature on a board whose .bss budget is the
  /// reason there are four instances here and not eleven.
  time_t staleSinceUtc = 0;

  /// What this card follows, as the server sent it: API-Sports' sport slug and
  /// the league's display name. 43 bytes a card, 172 across the four, on the
  /// same .bss budget the comment above is counting against.
  char sport[kMaxSportLength + 1] = "";
  char competition[kMaxCompetitionLength + 1] = "";
};

CardSlot gCards[kMaxCards];

/// Index of a registered card id, or -1. Linear over four entries, which is
/// cheaper than any map and far cheaper than the String comparisons a map would
/// need on this platform.
int8_t indexOf(const char* cardId) {
  if (cardId == nullptr) { return -1; }
  for (uint8_t i = 0; i < kMaxCards; ++i) {
    if (strcmp(kCardIds[i], cardId) == 0) { return static_cast<int8_t>(i); }
  }
  return -1;
}

/// Copy with a hard bound and a guaranteed terminator. strncpy does not
/// terminate when the source fills the buffer, which is the classic way a team
/// name runs into whatever sits after it in .bss.
void copyBounded(char* destination, const char* source, size_t capacity) {
  if (source == nullptr) { destination[0] = '\0'; return; }
  strncpy(destination, source, capacity);
  destination[capacity] = '\0';
}

/// What the card says when a game has not started. Deliberately a clock time
/// and not a countdown: a countdown is wrong the moment the rotation moves on,
/// and this card may sit unredrawn for minutes.
///
/// **THIS RENDERED UTC AND CALLED IT LOCAL.** It used to be
/// `localtime_r(&game.startsAtUtc, &local)`, which on this device returns UTC:
/// AppService.cpp calls `configTime(0, 0, ...)`, so the C library's timezone
/// offset is zero by design and `localtime_r` here is `gmtime_r` under a
/// misleading name. A 21:40 Eastern baseball game was photographed on the card
/// reading 01:40, which is worse than an obviously broken value, because 01:40
/// is a time somebody will believe.
///
/// The offset is applied by hand instead, the way every other card that shows
/// a time already does it: ClockDate.cpp's `time(nullptr) +
/// Display::utcOffsetMinutes() * 60` followed by `gmtime_r`, and Calendar.cpp
/// twice. The offset arrives on the check-in response and is handed to
/// Display::setEnvironment(), so it is already here for the asking.
///
/// The household's 12-or-24-hour preference is honoured through
/// Display::formatTimeOfDay() for the reason ClockDate.cpp's own comment
/// gives: the clock card would be the first place anybody noticed that setting
/// not being applied, and a game start time is the second.
String startTimeText(const Game& game) {
  // Reachable rather than defensive: CheckIn.cpp's parseIso8601Utc() returns 0
  // for a missing, null or unparseable startsAtUtc and a scheduled game can
  // carry any of the three. "--:--" is what Display::formatTimeOfDay() itself
  // renders for a time it cannot state, in 12-hour households as well as
  // 24-hour ones, so this is the firmware's existing "no usable time" wording
  // rather than a third convention of this card's own.
  if (game.startsAtUtc == 0) { return String("--:--"); }

  const int offsetMinutes = Display::utcOffsetMinutes();
  const time_t localInstant = game.startsAtUtc + static_cast<time_t>(offsetMinutes) * 60;
  struct tm local;
  gmtime_r(&localInstant, &local);
  const String rendered = Display::formatTimeOfDay(local.tm_hour, local.tm_min);

  // Every input and the output. A start time is a value a household checks
  // against the outside world, and being quietly wrong by five hours is the
  // failure this line exists to make reconstructible from the debug stream
  // without standing in front of the panel.
  Log::verbose("[sports] start time: utc=%ld offset=%dmin 12hour=%s -> %s",
               static_cast<long>(game.startsAtUtc), offsetMinutes,
               Display::use12HourClock() ? "yes" : "no", rendered.c_str());
  return rendered;
}

/// The short status a card draws beside a game.
String stateText(const Game& game) {
  switch (game.state) {
    case State::Live:
      // The provider's own progress text when there is one - "T7", "78'" - and
      // a bare "LIVE" when there is not. Never invented: a period this firmware
      // computed would be a guess about a sport it does not model.
      return game.period[0] != '\0' ? String(game.period) : String("LIVE");
    case State::Final:     return String("FINAL");
    case State::Postponed: return String("PPD");
    case State::Scheduled: return startTimeText(game);
    case State::Unknown:
    default:
      // Says nothing rather than guessing. An unknown state with a start time
      // drawn beside it would claim the game has not started, which is exactly
      // the claim we cannot make.
      return String("");
  }
}

/// A score, or an empty string before play starts. kNoScore and 0 are different
/// facts and a nil-nil draw is real, so this cannot collapse them.
String scoreText(int16_t score) {
  return score == kNoScore ? String("") : String(score);
}

/// The age line, or an empty string for "draw nothing", which is what this
/// returns nearly always.
///
/// **Absent means fresh** and is the ordinary path: the server sends
/// `staleSinceUtc` only when it judges the answer old (see setGames() in
/// Sports.h), so a card that is current gets no line, no reserved space and no
/// wording at all. A label that appears on every card is a label nobody reads;
/// one that appears rarely is the one that gets noticed on the evening it
/// matters. CARD_ABSENCE_AND_AGE_DESIGN.md section 8.
///
/// The words come from Display::describeAnswerAge(), which the listings card
/// also uses, so the two cards cannot end up describing age differently - the
/// point of putting that function in Display.h rather than writing a second
/// one here.
///
/// **The sub-minute suppression is a contradiction guard, not a rounding
/// choice.** describeAnswerAge() says "Updated just now" for anything under a
/// minute, which is correct as an age and absurd beside a staleness flag: the
/// server would be saying "I could not confirm this" over a line saying it was
/// confirmed seconds ago. Section 3 of the same design says a contradiction is
/// not drawn, so this draws nothing and puts the oddity in the stream instead,
/// where somebody can go and look at why the server flagged it.
String staleAgeText(const CardSlot& slot) {
  if (slot.staleSinceUtc == 0) {
    return String();
  }

  // Empty here means the device could not honestly compute an age at all - no
  // clock yet, or a timestamp in the future. describeAnswerAge() has already
  // said which in the stream, so this only records what the card did about it.
  const String age = Display::describeAnswerAge(slot.staleSinceUtc);
  if (age.length() == 0) {
    Log::verbose("[sports] card is flagged stale but no age can be computed - drawing the card "
                 "unmarked rather than an age this device cannot stand behind");
    return String();
  }

  const time_t now = time(nullptr);
  if (now - slot.staleSinceUtc < 60) {
    Log::printf("[sports] server flagged this card STALE but its own read was %ld s ago - not "
                "drawing an age, because 'Updated just now' beside a staleness flag is a "
                "contradiction. Worth looking at why the server called it stale",
                static_cast<long>(now - slot.staleSinceUtc));
    return String();
  }

  return age;
}

uint16_t itemCountFor(uint8_t index) {
  return gCards[index].count;
}

/// Draw one game of one card. Shared by all four registrations; the card
/// instance is bound by the wrapper functions below, because CardSpec's DrawFn
/// is a plain function pointer with no user data and four tiny wrappers are
/// cheaper than a std::function each.
void drawCardAt(uint8_t index, uint16_t itemIndex) {
  const CardSlot& slot = gCards[index];
  if (slot.count == 0) { return; }
  if (itemIndex >= slot.count) { itemIndex = 0; }

  const Game& game = slot.games[itemIndex];

  const String status = stateText(game);
  const String homeScore = scoreText(game.homeScore);
  const String awayScore = scoreText(game.awayScore);
  const String age = staleAgeText(slot);

  // The quieter logging tier every card here uses: runs once per dwell rather
  // than once per check-in, states exactly what is on screen, and is a no-op
  // unless remote streaming is on for this device. It is the only way to
  // reconstruct this card's content without standing in front of the hardware.
  // The age is named either way, and the empty case says WHY it is empty
  // rather than printing nothing: "no age line" on its own would be
  // indistinguishable from an age line that was meant to appear and did not.
  //
  // AWAY FIRST, AND THE "@" ON THE HOST, matching what the panel now draws -
  // see Display::showSportsCard()'s own remarks on why the old arrangement
  // stated that the visitor was hosting. This line is what anybody verifying
  // that fix will actually read, because nobody is standing in front of
  // device 23; a stream still reading home-first would have them checking the
  // fix against the bug's own ordering and concluding it had not landed.
  Log::verbose("[sports] on screen: card=%s %s %s - @ %s %s (%s) [%s]", kCardIds[index],
               game.away, awayScore.c_str(), game.home, homeScore.c_str(), status.c_str(),
               age.length() > 0 ? age.c_str() : "no age line - server says this card is current");

  // The period is the design's progressLabel - "Q3", "6TH INN", "78'" - passed
  // straight through without interpretation, which is what keeps the renderer
  // free of any branch on sport.
  //
  // No clock. Nothing on this device carries one: the wire has no clockLabel
  // field yet, and deriving a time from the start would be exactly the
  // fabricated clock the design forbids by name.
  //
  // Live is a flag rather than something the renderer infers from the status
  // word, because that word is prose and would make the badge depend on the
  // reader's language.
  const String sportLabel = sportChipLabel(slot.sport);

  // Both say what is on, so both are logged: a card drawing no chip because the
  // server sent no sport and one drawing none because the slug is unrecognised
  // are the same picture from the front of the display.
  Log::verbose("[sports] heading: card=%s sport='%s' chip='%s' competition='%s'",
               kCardIds[index], slot.sport,
               sportLabel.length() > 0 ? sportLabel.c_str() : "none - no sport sent, or a slug this build does not map",
               slot.competition[0] != '\0' ? slot.competition : "none - server sent no league name for this card");

  Display::showSportsCard(game.home, homeScore, game.away, awayScore, status,
                          itemIndex + 1, slot.count, age,
                          sportLabel, slot.competition,
                          /*progressLabel=*/game.period,
                          /*clockLabel=*/"",
                          /*isLive=*/game.state == State::Live);
}

// Four wrappers, one per registration. CardSpec::DrawFn and ItemCountFn are
// bare function pointers, so the instance has to be bound at compile time.
void draw0(uint16_t i) { drawCardAt(0, i); }
void draw1(uint16_t i) { drawCardAt(1, i); }
void draw2(uint16_t i) { drawCardAt(2, i); }
void draw3(uint16_t i) { drawCardAt(3, i); }

uint16_t count0() { return itemCountFor(0); }
uint16_t count1() { return itemCountFor(1); }
uint16_t count2() { return itemCountFor(2); }
uint16_t count3() { return itemCountFor(3); }

/// True while any game on this card is in progress. Used only by sportslive,
/// which exists to be present exactly then.
bool anyLive(uint8_t index) {
  const CardSlot& slot = gCards[index];
  for (uint8_t i = 0; i < slot.count; ++i) {
    if (slot.games[i].state == State::Live) { return true; }
  }
  return false;
}

/// sportslive reports zero items unless something is actually live, so it drops
/// out of the rotation rather than drawing a card that says nothing is on. This
/// is the card's design, not a failure path - see Sports.h.
uint16_t countLive() {
  return anyLive(3) ? itemCountFor(3) : 0;
}

/// A live game is worth a longer look than a fixture list. Same mechanism
/// Aircraft uses for an aeroplane nearly overhead.
bool notableLive(uint16_t) { return anyLive(3); }

/// Which game a showing was of, as a key an impression report can group on.
///
/// Composed here rather than carried from the server, because the payload has
/// no game id to carry: SportsCardPayload sends teams, scores, state, period
/// and a start time, and the matching struct in Sports.h holds exactly those.
/// The start instant and the two sides name a fixture between them, and all
/// three already arrive, so the key costs nothing on the wire and every device
/// showing the same game reports the same key.
///
/// UTC, deliberately. A game at 8pm Eastern falls on the next UTC day, so this
/// does not read as the date on the card - but two devices in different zones
/// showing one game have to agree, and the instant is the only thing about a
/// fixture that every device sees identically.
///
/// A start time of 0 means the server did not send one. The teams still name
/// the fixture, so the key keeps them and drops the date rather than returning
/// nothing: a key shared by both legs of a double-header is a better answer
/// than no key at all.
String listingIdCardAt(uint8_t index, uint16_t itemIndex) {
  const CardSlot& slot = gCards[index];
  if (slot.count == 0 || itemIndex >= slot.count) { return String(); }

  const Game& game = slot.games[itemIndex];
  if (game.away[0] == '\0' && game.home[0] == '\0') { return String(); }

  char key[72];
  if (game.startsAtUtc != 0) {
    struct tm utc;
    gmtime_r(&game.startsAtUtc, &utc);
    snprintf(key, sizeof(key), "%04d-%02d-%02d:%s@%s",
             utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, game.away, game.home);
  } else {
    snprintf(key, sizeof(key), "%s@%s", game.away, game.home);
  }

  return String(key);
}

/// The same line the verbose log writes, as the label a report shows beside
/// the key. Away first with the "@" on the host, matching both the panel and
/// drawCardAt()'s log - a report that reversed them would have somebody
/// reading last night's result backwards.
String describeCardAt(uint8_t index, uint16_t itemIndex) {
  const CardSlot& slot = gCards[index];
  if (slot.count == 0 || itemIndex >= slot.count) { return String(); }

  const Game& game = slot.games[itemIndex];

  String text = String(game.away);
  const String awayScore = scoreText(game.awayScore);
  if (awayScore.length() > 0) { text += " " + awayScore; }

  text += " @ ";
  text += game.home;
  const String homeScore = scoreText(game.homeScore);
  if (homeScore.length() > 0) { text += " " + homeScore; }

  const String status = stateText(game);
  if (status.length() > 0) { text += " (" + status + ")"; }

  return text;
}

// Four wrappers each, for the same reason draw0..draw3 exist: CardSpec's hooks
// are bare function pointers with no user data to bind the instance through.
String listingId0(uint16_t i) { return listingIdCardAt(0, i); }
String listingId1(uint16_t i) { return listingIdCardAt(1, i); }
String listingId2(uint16_t i) { return listingIdCardAt(2, i); }
String listingId3(uint16_t i) { return listingIdCardAt(3, i); }

String describe0(uint16_t i) { return describeCardAt(0, i); }
String describe1(uint16_t i) { return describeCardAt(1, i); }
String describe2(uint16_t i) { return describeCardAt(2, i); }
String describe3(uint16_t i) { return describeCardAt(3, i); }

Cards::ListingIdFn listingIdFor(uint8_t index) {
  switch (index) {
    case 0: return listingId0;
    case 1: return listingId1;
    case 2: return listingId2;
    default: return listingId3;
  }
}

Cards::DescribeFn describeFor(uint8_t index) {
  switch (index) {
    case 0: return describe0;
    case 1: return describe1;
    case 2: return describe2;
    default: return describe3;
  }
}

bool registerOne(uint8_t index, Cards::ItemCountFn itemCount, Cards::DrawFn draw,
                 int16_t order, uint16_t dwellSeconds) {
  Cards::CardSpec spec;
  spec.id = kCardIds[index];
  spec.kind = Cards::Kind::List;
  spec.itemCount = itemCount;
  spec.draw = draw;
  spec.order = order;
  spec.dwellSeconds = dwellSeconds;
  spec.listingId = listingIdFor(index);
  spec.describe = describeFor(index);
  return Cards::registerCard(spec);
}

/// Self-registering at static-init time, the same way every other card module
/// here does it - see Cards.h on registerCard(). No begin() to call and
/// therefore no call site to forget, which matters for a card whose absence
/// would otherwise look exactly like a card with no games.
[[maybe_unused]] const bool kRegistered = [] {
  // Orders sit after the household's own information (clock, forecast, tides)
  // and before the marketing cards, which is where somebody would expect
  // something they chose to follow. Dwell is 10s for a team and 14s for the
  // scoreboard, which has more to read.
  const bool a = registerOne(0, count0, draw0, 12, 10);
  const bool b = registerOne(1, count1, draw1, 13, 10);
  const bool c = registerOne(2, count2, draw2, 14, 14);

  // sportslive registers its own item count, which is zero unless something is
  // playing, and marks itself notable while it is. Early in the order: if a
  // followed team is on, that is what the rotation should lead with.
  Cards::CardSpec live;
  live.id = kCardIds[3];
  live.kind = Cards::Kind::List;
  live.itemCount = countLive;
  live.draw = draw3;
  live.isNotable = notableLive;
  live.order = 2;
  live.dwellSeconds = 12;
  live.notableDwellSeconds = 20;
  const bool d = Cards::registerCard(live);

  return a && b && c && d;
}();

}  // namespace

void setGames(const char* cardId, const Game* games, uint8_t count, time_t staleSinceUtc,
              const char* sport, const char* competition) {
  const int8_t index = indexOf(cardId);
  if (index < 0) {
    // A policy or payload naming an instance this firmware does not register.
    // Logged rather than dropped silently, because it means the server's
    // catalog and this file have drifted - which is a thing somebody should
    // find out from a log line and not from a card that never appears.
    Log::printf("[sports] payload names unregistered card '%s' - ignored",
              cardId != nullptr ? cardId : "(null)");
    return;
  }

  if (count > kMaxGames) {
    // The server caps at four for a heap reason. More than four arriving means
    // the two caps have drifted apart, which is worth a line rather than a
    // quiet truncation.
    Log::printf("[sports] card '%s' sent %u games, storing %u", cardId,
              static_cast<unsigned>(count), static_cast<unsigned>(kMaxGames));
    count = kMaxGames;
  }

  CardSlot& slot = gCards[index];
  for (uint8_t i = 0; i < count; ++i) {
    slot.games[i] = games[i];
    // Re-terminate defensively: these came across a wire and the sender's
    // bounds are not this firmware's to trust.
    slot.games[i].period[kMaxPeriodLength] = '\0';
    slot.games[i].home[kMaxTeamNameLength] = '\0';
    slot.games[i].away[kMaxTeamNameLength] = '\0';
  }
  slot.count = count;
  slot.staleSinceUtc = staleSinceUtc;

  // Bounded, and terminated by hand: strncpy writes no terminator when the
  // source fills the buffer, and both of these are the sender's strings rather
  // than this firmware's. A server sending a longer league name than its own cap
  // gets its name cut rather than this slot's neighbour overwritten.
  strncpy(slot.sport, sport != nullptr ? sport : "", kMaxSportLength);
  slot.sport[kMaxSportLength] = '\0';
  strncpy(slot.competition, competition != nullptr ? competition : "", kMaxCompetitionLength);
  slot.competition[kMaxCompetitionLength] = '\0';

  // One line, both branches named, on a path that runs once per check-in. The
  // fresh branch is said out loud rather than left as silence because on this
  // field absence IS the answer - "the server did not flag this" and "the
  // firmware never looked" are otherwise identical from outside, and that is
  // the same mistake the telemetry calVersion narration exists to prevent.
  // Deliberately folded into the existing line rather than added as a second
  // one: debug streaming was switched off fleet-wide after a heap fault, and
  // the way back on is not to have grown the stream while it was off.
  Log::printf("[sports] card '%s' now holds %u game(s); %s", cardId,
              static_cast<unsigned>(count),
              staleSinceUtc == 0
                  ? "no staleSinceUtc on the payload, so the server considers it current and no "
                    "age will be drawn"
                  : "server sent staleSinceUtc - this card will show how old its answer is");
}

String sportChipLabel(const char* sport) {
  if (sport == nullptr || sport[0] == '\0') { return String(); }

  // The two football codes, named first and separately. API-Sports calls soccer
  // "football", so matching a prefix or a substring here would put FOOTBALL over
  // a soccer score - which is the one label on this card a viewer would act on.
  // Whole-token comparison, both ways round.
  if (strcmp(sport, "american-football") == 0) { return String("FOOTBALL"); }
  if (strcmp(sport, "football") == 0)          { return String("SOCCER"); }
  if (strcmp(sport, "basketball") == 0)        { return String("BASKETBALL"); }
  if (strcmp(sport, "baseball") == 0)          { return String("BASEBALL"); }
  if (strcmp(sport, "hockey") == 0)            { return String("HOCKEY"); }

  // A slug this build has never seen. Empty rather than the raw token: the chip
  // is small caps on a pill, and "rugby-sevens" rendered into it reads as a bug
  // where no chip reads as a card that simply does not name its sport.
  Log::printf("[sports] unmapped sport slug '%s' - drawing no chip", sport);
  return String();
}

void clearAll() {
  // The age goes with the games. Leaving a staleSinceUtc behind on an emptied
  // slot would let yesterday's staleness reappear on tomorrow's fixtures the
  // moment a card is repopulated by a payload that carries no timestamp -
  // which, since absence means fresh, is the ordinary payload.
  for (uint8_t i = 0; i < kMaxCards; ++i) {
    gCards[i].count = 0;
    gCards[i].staleSinceUtc = 0;

    // The heading goes with them, for the reason above applied to the other two
    // card-level fields: a repopulated slot would otherwise carry the league it
    // used to follow.
    gCards[i].sport[0] = '\0';
    gCards[i].competition[0] = '\0';
  }
  Log::printf("[sports] cleared all cards");
}

}  // namespace Sports
