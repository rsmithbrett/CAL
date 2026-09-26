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

  // The quieter logging tier every card here uses: runs once per dwell rather
  // than once per check-in, states exactly what is on screen, and is a no-op
  // unless remote streaming is on for this device. It is the only way to
  // reconstruct this card's content without standing in front of the hardware.
  Log::verbose("[sports] on screen: card=%s %s %s - %s %s (%s)", kCardIds[index],
               game.home, homeScore.c_str(), awayScore.c_str(), game.away, status.c_str());

  Display::showSportsCard(game.home, homeScore, game.away, awayScore, status,
                          itemIndex + 1, slot.count);
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

bool registerOne(uint8_t index, Cards::ItemCountFn itemCount, Cards::DrawFn draw,
                 int16_t order, uint16_t dwellSeconds) {
  Cards::CardSpec spec;
  spec.id = kCardIds[index];
  spec.kind = Cards::Kind::List;
  spec.itemCount = itemCount;
  spec.draw = draw;
  spec.order = order;
  spec.dwellSeconds = dwellSeconds;
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

void setGames(const char* cardId, const Game* games, uint8_t count) {
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

  Log::printf("[sports] card '%s' now holds %u game(s)", cardId, static_cast<unsigned>(count));
}

void clearAll() {
  for (uint8_t i = 0; i < kMaxCards; ++i) { gCards[i].count = 0; }
  Log::printf("[sports] cleared all cards");
}

}  // namespace Sports
