// App/Sports.cpp's registrations and the three hook switches that bind them:
// which ids register at all, which of them take the table renderer, and which
// card's games each registered hook answers with.
//
// Sports.cpp is compiled as it ships. Cards, CardColumns, Display and Log are
// declarations that parse on a host once Arduino.h is stubbed, and the nine
// functions Sports.cpp calls into are defined at the bottom of this file. The
// Cards stub keeps every descriptor that registers, which is the only way to
// reach anything in Sports.cpp's anonymous namespace - isBoard(), registerOne(),
// listingIdFor(), describeFor() and frameCountFor() are all in there, and
// CardManager reaches them the same way this does, through CardSpec's hooks.
//
// WHY THIS FILE EXISTS. Registering sportsscores2 touched three things inside
// Sports.cpp - a second strcmp in isBoard(), a registerOne(4, ...) call, and a
// case 3 added to each of the three switches so their default arm could hand out
// index 4 - and a mutation sweep found all three invisible to all 24 ci/ runners.
// Each case below was written against a mutation and watched to fail before it
// was watched to pass.
//
// WHAT EACH CASE HOLDS, and the mutation it was measured against:
//
//   - itemCount: a board reports one item however many games it holds, and a
//     team or live card reports one per game. Deleting either strcmp from
//     isBoard() turns a scoreboard into a single-game card and this reports the
//     game count instead of 1.
//   - Registration: every id in the table registers. Dropping registerOne(4,
//     ...) leaves sportsscores2 out, which is what the server's payload for a
//     second league used to land on - nothing.
//   - Identity: each card's listingId, describe and draw hooks answer with THAT
//     card's games. Deleting `case 3:` from any of the three switches folds
//     sportslive onto the default arm, which hands out index 4's wrapper, and
//     the live card then reports and draws the second scoreboard's fixtures
//     under its own id. That fold is what the explicit case 3 was added to
//     prevent, and nothing else in the suite would have seen it.
//   - frameCountFor: a scoreboard is worth one impression per game sharing its
//     frame, and a single-game card one. Collapsing the board arm to 1 reports
//     one impression for four games.
//
// WHAT THIS CANNOT CHECK is the static_assert on kMaxCards at the top of that
// wrapper block. A static_assert is reached by a compiler and not by a test
// binary, and only the arduino build compiles that file today. What stands in
// for it here is stronger in the one way that matters: the id table below is
// held to Sports::kMaxCards by a static_assert of its own, so raising kMaxCards
// without extending that table stops this test compiling, and the identity cases
// then demand that the new index answer with its own games rather than the
// default arm's.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "CardColumns.h"
#include "Cards.h"
#include "Display.h"
#include "Log.h"
#include "Sports.h"

namespace {

// --- the registry Sports.cpp registers into ---------------------------------

/// Every descriptor that registered, in a function-local static so nothing
/// depends on the order translation units initialise in. Sports.cpp registers at
/// static-init time, before main() runs.
///
/// Reserved up front and never grown past that, because findCard() hands back a
/// pointer into this and a reallocation would invalidate one.
std::vector<Cards::CardSpec>& registry() {
  static std::vector<Cards::CardSpec>* held = [] {
    auto* fresh = new std::vector<Cards::CardSpec>();
    fresh->reserve(32);
    return fresh;
  }();
  return *held;
}

Cards::CardSpec* findCard(const char* id) {
  for (auto& spec : registry()) {
    if (std::strcmp(spec.id, id) == 0) {
      return &spec;
    }
  }
  return nullptr;
}

Cards::CardSpec& card(const char* id) {
  Cards::CardSpec* const spec = findCard(id);
  assert(spec != nullptr && "no card registered under that id");
  return *spec;
}

// --- what the panel was asked to draw ---------------------------------------

/// The last showSportsBoard() call, so a board draw can be read back. Teams
/// rather than pixels: what these cases are about is WHICH card's games reached
/// the renderer.
struct BoardDraw {
  bool happened = false;
  uint8_t rowCount = 0;
  std::vector<std::string> awayNames;
  std::string competition;
};

/// The last showSportsCard() call, for the three registrations that draw one
/// game at a time.
struct CardDraw {
  bool happened = false;
  std::string awayName;
  std::string homeName;
  std::string competition;
};

BoardDraw& lastBoard() {
  static BoardDraw held;
  return held;
}

CardDraw& lastCard() {
  static CardDraw held;
  return held;
}

void forgetDraws() {
  lastBoard() = BoardDraw();
  lastCard() = CardDraw();
}

// --- the ids, in the order Sports.cpp's own table holds them -----------------

/// The five registrations, index for index with kCardIds in Sports.cpp. That
/// file's table is in an anonymous namespace, so this is a second copy of it and
/// the assert below is what keeps the two from drifting: a sixth id with
/// kMaxCards raised to 6 stops this test compiling until this table grows too,
/// and the identity cases then require the new index to answer for itself.
///
/// The seven added on 2026-11-09 are appended rather than placed beside their
/// own kind, for the reason Sports.cpp's own kCardIds comment gives: index 3 is
/// sportslive and three call sites there name that number. This table must
/// match that one entry for entry, in order.
const char* const kIds[] = {
    "sportsteam",    "sportsteam2",   "sportsscores",  "sportslive",
    "sportsteam3",   "sportsteam4",   "sportsteam5",   "sportsscores2",
    "sportsscores3", "sportsscores4", "sportsscores5",
};

constexpr size_t kIdCount = sizeof(kIds) / sizeof(*kIds);

static_assert(kIdCount == Sports::kMaxCards,
              "Sports::kMaxCards moved and this table did not. Every case below walks this "
              "table, so a registration missing from it is a registration nothing here checks.");

const char* const kTeamOne = kIds[0];
const char* const kTeamTwo = kIds[1];
const char* const kBoardOne = kIds[2];
const char* const kLive = kIds[3];
const char* const kTeamThree = kIds[4];
const char* const kBoardTwo = kIds[7];

// --- building a payload ------------------------------------------------------

/// One game, the way a check-in delivers it. startsAtUtc stays 0 throughout,
/// which is the server not sending one: listingIdCardAt() then keys on the two
/// sides alone, so a key is "away@home" and names the card it came from without
/// any date arithmetic in the expectation.
Sports::Game game(const char* away, const char* home, int16_t awayScore, int16_t homeScore,
                  Sports::State state) {
  Sports::Game made;
  std::strncpy(made.away, away, Sports::kMaxTeamNameLength);
  made.away[Sports::kMaxTeamNameLength] = '\0';
  std::strncpy(made.home, home, Sports::kMaxTeamNameLength);
  made.home[Sports::kMaxTeamNameLength] = '\0';
  made.awayScore = awayScore;
  made.homeScore = homeScore;
  made.state = state;
  made.startsAtUtc = 0;
  return made;
}

/// What a check-in hands setGames(). staleSinceUtc stays 0, which is the
/// ordinary payload and means the server considers the card current, so no age
/// line is drawn and describeAnswerAge() is never reached.
void setCard(const char* id, const std::vector<Sports::Game>& games, const char* sport,
             const char* competition) {
  Sports::setGames(id, games.data(), static_cast<uint8_t>(games.size()), 0, sport, competition);
}

/// Five cards, each holding its own teams and its own number of games, so an
/// answer that came from the wrong index names a team that card never held.
///
/// The counts differ on purpose. sportslive holds three games and sportsscores2
/// holds two, which is what makes a sportslive hook folded onto index 4 report 2
/// where 1 is right - with equal counts that fold would read as correct.
void populateEveryCard() {
  Sports::clearAll();
  forgetDraws();

  setCard(kTeamOne,
          {game("Orioles", "Yankees", 4, 2, Sports::State::Final),
           game("Orioles", "Red Sox", 1, 3, Sports::State::Final),
           game("Orioles", "Rays", 7, 7, Sports::State::Final)},
          "baseball", "MLB");

  setCard(kTeamTwo, {game("Ravens", "Steelers", 24, 17, Sports::State::Final)},
          "american-football", "NFL");

  setCard(kBoardOne,
          {game("Celtics", "Knicks", 101, 98, Sports::State::Final),
           game("Lakers", "Nets", 88, 95, Sports::State::Final),
           game("Heat", "Bulls", 110, 104, Sports::State::Final)},
          "basketball", "NBA");

  setCard(kLive,
          {game("Capitals", "Penguins", 2, 1, Sports::State::Live),
           game("Rangers", "Devils", 0, 0, Sports::State::Live),
           game("Bruins", "Sabres", 3, 2, Sports::State::Live)},
          "hockey", "NHL");

  setCard(kBoardTwo,
          {game("Arsenal", "Chelsea", 2, 2, Sports::State::Final),
           game("Spurs", "Everton", 1, 0, Sports::State::Final)},
          "football", "Premier League");

  // The six added on 2026-11-09, so every registered index holds games of its
  // own. Distinct sides throughout: noTwoCardsReportTheSameKey() is the case
  // that catches an index folding onto another's slot, and two cards sharing a
  // fixture would make that fold invisible.
  setCard(kTeamThree, {game("Mariners", "Cubs", 6, 4, Sports::State::Final)},
          "baseball", "MLB");
  setCard(kIds[5], {game("Padres", "Giants", 5, 3, Sports::State::Final)},
          "baseball", "MLB");
  setCard(kIds[6], {game("Jets", "Bills", 10, 31, Sports::State::Final)},
          "american-football", "NFL");
  setCard(kIds[8],
          {game("Suns", "Kings", 112, 109, Sports::State::Final),
           game("Jazz", "Thunder", 95, 99, Sports::State::Final)},
          "basketball", "NBA");
  setCard(kIds[9],
          {game("Flames", "Oilers", 4, 2, Sports::State::Final),
           game("Canucks", "Kraken", 1, 2, Sports::State::Final)},
          "hockey", "NHL");
  setCard(kIds[10],
          {game("Inter", "Milan", 0, 1, Sports::State::Final),
           game("Roma", "Lazio", 2, 2, Sports::State::Final)},
          "football", "Serie A");
}

// --- reading an answer back through the descriptor ---------------------------

uint16_t itemCountOf(const char* id) {
  Cards::CardSpec& spec = card(id);
  assert(spec.itemCount != nullptr && "that card registers no itemCount");
  return spec.itemCount();
}

std::string keyOf(const char* id, uint16_t item) {
  Cards::CardSpec& spec = card(id);
  assert(spec.listingId != nullptr && "that card registers no listingId resolver");
  return spec.listingId(item);
}

std::string describeOf(const char* id, uint16_t item) {
  Cards::CardSpec& spec = card(id);
  assert(spec.describe != nullptr && "that card registers no describe");
  return spec.describe(item);
}

uint8_t frameCountOf(const char* id, uint16_t item) {
  Cards::CardSpec& spec = card(id);
  assert(spec.frameCountFor != nullptr && "that card registers no frameCountFor");
  return spec.frameCountFor(item);
}

void drawOf(const char* id, uint16_t item) {
  Cards::CardSpec& spec = card(id);
  assert(spec.draw != nullptr && "that card registers no draw");
  forgetDraws();
  spec.draw(item);
}

// --- every id registers ------------------------------------------------------

/// The case the second league shipped failing. Before registerOne(4, ...) landed,
/// setGames("sportsscores2", ...) found no index, logged a line and dropped the
/// payload, so a household following a second league saw nothing and the server
/// had no way to tell.
void everyIdInTheTableRegisters() {
  for (size_t i = 0; i < kIdCount; ++i) {
    assert(findCard(kIds[i]) != nullptr &&
           "an id in Sports.cpp's table has no registration, so a payload naming it is dropped");
  }
  std::cout << "  all " << kIdCount << " sports ids register\n";
}

/// The count as well as the names, because the two fail differently: a
/// registerOne() call handed the wrong index registers a duplicate id and leaves
/// another unclaimed, which the loop above would pass on if the duplicate
/// happened to answer.
void asManyCardsRegisterAsThereAreIds() {
  size_t found = 0;
  for (auto& spec : registry()) {
    for (size_t i = 0; i < kIdCount; ++i) {
      if (std::strcmp(spec.id, kIds[i]) == 0) {
        ++found;
        break;
      }
    }
  }
  assert(found == kIdCount && "the number of registered sports cards is not the number of ids");
  assert(registry().size() == kIdCount && "something other than Sports.cpp registered here");
  std::cout << "  " << found << " registrations for " << kIdCount << " ids\n";
}

/// Each registration carries all four hooks. A descriptor missing listingId
/// would be refused by registerCard() under KeyPolicy::Resolves; one missing
/// frameCountFor registers happily and reports one impression for a frame of
/// four games.
void everyRegistrationCarriesItsHooks() {
  for (size_t i = 0; i < kIdCount; ++i) {
    Cards::CardSpec& spec = card(kIds[i]);
    assert(spec.itemCount != nullptr && "a sports card registers no itemCount");
    assert(spec.draw != nullptr && "a sports card registers no draw");
    assert(spec.listingId != nullptr && "a sports card registers no listingId");
    assert(spec.describe != nullptr && "a sports card registers no describe");
    assert(spec.frameCountFor != nullptr && "a sports card registers no frameCountFor");
  }
  std::cout << "  every registration carries itemCount, draw, listingId, describe and "
               "frameCountFor\n";
}

// --- isBoard(), through the item count --------------------------------------

/// BOTH scoreboards report one item however many games they hold, because the
/// board draws the whole league in one frame and the rotation would otherwise
/// give the same four games four dwells.
///
/// Deleting the "sportsscores" strcmp makes this 3; deleting the "sportsscores2"
/// one makes the second assert 2.
void bothScoreboardsReportOneItem() {
  populateEveryCard();
  assert(itemCountOf(kBoardOne) == 1 &&
         "sportsscores does not take the table renderer, so its games get a dwell each");
  assert(itemCountOf(kBoardTwo) == 1 &&
         "sportsscores2 does not take the table renderer, so its games get a dwell each");
  std::cout << "  both scoreboards report one item for a frame of games\n";
}

/// The team cards keep the single-game renderer: one item per game, one dwell
/// each. This is the other half of isBoard() - a strcmp widened to a prefix
/// match, or an arm that answered true for everything, would turn these into 1.
void theTeamCardsReportOneItemAGame() {
  populateEveryCard();
  assert(itemCountOf(kTeamOne) == 3 && "sportsteam holds three games and should get three dwells");
  assert(itemCountOf(kTeamTwo) == 1 && "sportsteam2 holds one game");
  std::cout << "  the team cards report one item a game\n";
}

/// sportslive is not a board either. Its count comes from countLive() rather
/// than count3 - zero unless something is playing - so this holds both that the
/// gate opens for a live game and that the card past it counts games rather than
/// frames.
void theLiveCardReportsOneItemAGame() {
  populateEveryCard();
  assert(itemCountOf(kLive) == 3 &&
         "sportslive holds three live games and should get three dwells");
  std::cout << "  the live card reports one item a game\n";
}

/// sportslive drops out of the rotation when nothing is on, which is the card's
/// whole design and its state on most days. Final games are not live games.
void theLiveCardReportsNothingWithNoGameInProgress() {
  Sports::clearAll();
  setCard(kLive, {game("Capitals", "Penguins", 4, 1, Sports::State::Final)}, "hockey", "NHL");
  assert(itemCountOf(kLive) == 0 &&
         "sportslive reported an item with nothing in progress, so it draws that nothing is on");
  std::cout << "  the live card reports nothing with no game in progress\n";
}

/// An empty board reports no items rather than one empty frame. The `count > 0`
/// arm of itemCountFor() is the only thing holding this.
void anEmptyScoreboardReportsNoItems() {
  Sports::clearAll();
  assert(itemCountOf(kBoardOne) == 0 && "an empty sportsscores reported an item to draw");
  assert(itemCountOf(kBoardTwo) == 0 && "an empty sportsscores2 reported an item to draw");
  std::cout << "  an empty scoreboard reports no items\n";
}

// --- the three switches answer for their own index ---------------------------

/// The key every card reports is a key for its own fixture.
///
/// This is the case that bites when `case 3:` leaves listingIdFor(): sportslive
/// takes the default arm, gets listingId4, and reports Arsenal@Chelsea - the
/// second scoreboard's game - under its own id. Every impression of the live
/// card would then group with the second league's.
void eachCardReportsAKeyForItsOwnGame() {
  populateEveryCard();
  assert(keyOf(kTeamOne, 0) == "Orioles@Yankees" && "sportsteam reported another card's fixture");
  assert(keyOf(kTeamTwo, 0) == "Ravens@Steelers" && "sportsteam2 reported another card's fixture");
  assert(keyOf(kBoardOne, 0) == "Celtics@Knicks" &&
         "sportsscores reported another card's fixture");
  assert(keyOf(kLive, 0) == "Capitals@Penguins" &&
         "sportslive reported another card's fixture - its case has left listingIdFor() and it "
         "is answering from the default arm");
  assert(keyOf(kBoardTwo, 0) == "Arsenal@Chelsea" &&
         "sportsscores2 reported another card's fixture");
  std::cout << "  each card reports a key for its own game\n";
}

/// Every key differs from every other, which is the property an impression row
/// rests on. Two cards reporting one key is how a fold onto the default arm
/// looks from the server: one league's games counted twice and another's not at
/// all.
void noTwoCardsReportTheSameKey() {
  populateEveryCard();
  for (size_t i = 0; i < kIdCount; ++i) {
    for (size_t j = i + 1; j < kIdCount; ++j) {
      const std::string left = keyOf(kIds[i], 0);
      const std::string right = keyOf(kIds[j], 0);
      assert(!left.empty() && "a card reported no key for a game it holds");
      assert(left != right && "two sports cards report the same key, so one is answering for the "
                              "other's index");
    }
  }
  std::cout << "  no two cards report the same key\n";
}

/// The same question of describeFor(). The label is what a report shows beside
/// the key, so a fold here reads last night's wrong result back at somebody.
///
/// Away first with the "@" on the host, which is what the panel and the verbose
/// log both do.
void eachCardDescribesItsOwnGame() {
  populateEveryCard();
  assert(describeOf(kTeamOne, 0) == "Orioles 4 @ Yankees 2 (FINAL)" &&
         "sportsteam described another card's game");
  assert(describeOf(kTeamTwo, 0) == "Ravens 24 @ Steelers 17 (FINAL)" &&
         "sportsteam2 described another card's game");
  assert(describeOf(kBoardOne, 0) == "Celtics 101 @ Knicks 98 (FINAL)" &&
         "sportsscores described another card's game");
  assert(describeOf(kLive, 0) == "Capitals 2 @ Penguins 1 (LIVE)" &&
         "sportslive described another card's game - its case has left describeFor() and it is "
         "answering from the default arm");
  assert(describeOf(kBoardTwo, 0) == "Arsenal 2 @ Chelsea 2 (FINAL)" &&
         "sportsscores2 described another card's game");
  std::cout << "  each card describes its own game\n";
}

/// An item past what a card holds reports nothing rather than another index's
/// game or a read past the stored set. sportsteam2 holds one game, so item 1 is
/// one past it.
void anItemPastACardReportsNothing() {
  populateEveryCard();
  assert(keyOf(kTeamTwo, 1).empty() && "an item past sportsteam2's games reported a key");
  assert(describeOf(kTeamTwo, 1).empty() && "an item past sportsteam2's games was described");
  assert(keyOf(kBoardTwo, 2).empty() && "an item past sportsscores2's games reported a key");
  std::cout << "  an item past a card reports nothing\n";
}

/// A card with no games reports no key, which is what keeps sportslive out of
/// the impression stream on the days nothing is on.
void anEmptyCardReportsNoKey() {
  Sports::clearAll();
  for (size_t i = 0; i < kIdCount; ++i) {
    assert(keyOf(kIds[i], 0).empty() && "an empty sports card reported a key");
  }
  std::cout << "  an empty card reports no key\n";
}

/// What reached the panel, for the draw hooks. Each registration binds its index
/// at its own call site rather than through a switch, and this is what says so:
/// a board draw carries that board's rows, and a single-game draw that card's
/// teams.
void eachCardDrawsItsOwnGames() {
  populateEveryCard();

  drawOf(kBoardOne, 0);
  assert(lastBoard().happened && "sportsscores did not reach the table renderer");
  assert(lastBoard().rowCount == 3 && "sportsscores drew a different number of rows than it holds");
  assert(lastBoard().awayNames[0] == "Celtics" && "sportsscores drew another card's games");
  assert(lastBoard().competition == "NBA" && "sportsscores drew another card's league");

  drawOf(kBoardTwo, 0);
  assert(lastBoard().happened && "sportsscores2 did not reach the table renderer");
  assert(lastBoard().rowCount == 2 &&
         "sportsscores2 drew a different number of rows than it holds");
  assert(lastBoard().awayNames[0] == "Arsenal" && "sportsscores2 drew another card's games");
  assert(lastBoard().competition == "Premier League" && "sportsscores2 drew another card's league");

  drawOf(kTeamOne, 0);
  assert(lastCard().happened && "sportsteam did not reach the single-game renderer");
  assert(lastCard().awayName == "Orioles" && "sportsteam drew another card's game");

  drawOf(kLive, 0);
  assert(lastCard().happened && "sportslive did not reach the single-game renderer");
  assert(lastCard().awayName == "Capitals" && "sportslive drew another card's game");
  assert(lastCard().competition == "NHL" && "sportslive drew another card's league");

  std::cout << "  each card draws its own games\n";
}

/// A scoreboard goes to the table renderer and a team card does not. The same
/// isBoard() answer the item count rests on, read at the other end: a card that
/// got it wrong would draw one game per dwell while reporting four impressions
/// for the frame.
void onlyTheScoreboardsReachTheTableRenderer() {
  populateEveryCard();

  drawOf(kBoardOne, 0);
  assert(lastBoard().happened && !lastCard().happened &&
         "sportsscores drew a single game instead of its table");

  drawOf(kBoardTwo, 0);
  assert(lastBoard().happened && !lastCard().happened &&
         "sportsscores2 drew a single game instead of its table");

  drawOf(kTeamOne, 0);
  assert(lastCard().happened && !lastBoard().happened &&
         "sportsteam drew a table instead of one game");

  drawOf(kTeamTwo, 0);
  assert(lastCard().happened && !lastBoard().happened &&
         "sportsteam2 drew a table instead of one game");

  drawOf(kLive, 0);
  assert(lastCard().happened && !lastBoard().happened &&
         "sportslive drew a table instead of one game");

  std::cout << "  only the scoreboards reach the table renderer\n";
}

// --- frameCountFor() ---------------------------------------------------------

/// A scoreboard's showing is worth one impression per game sharing the frame,
/// so its frame count is the games it holds. Collapsing that arm to 1 reports
/// one impression for four games and nobody can say which game was looked at.
void aScoreboardReportsAFramePerGame() {
  populateEveryCard();
  assert(frameCountOf(kBoardOne, 0) == 3 && "sportsscores holds three games and framed otherwise");
  assert(frameCountOf(kBoardTwo, 0) == 2 && "sportsscores2 holds two games and framed otherwise");
  std::cout << "  a scoreboard reports a frame per game\n";
}

/// Four games is the cap at both ends - the server's own and kMaxGames here - so
/// a board holding the maximum frames four.
///
/// The `slot.count > kMaxGames` comparison in frameCountCardAt() cannot be
/// reached through setGames(), which caps the stored count first and logs the
/// drift. So what this pins is the cap's effect: a payload of six games frames
/// four, not six. The comparison itself is defensive and stays unreached.
void aScoreboardFramesNoMoreThanTheGamesItStores() {
  Sports::clearAll();
  setCard(kBoardTwo,
          {game("Arsenal", "Chelsea", 1, 0, Sports::State::Final),
           game("Spurs", "Everton", 2, 2, Sports::State::Final),
           game("Villa", "Wolves", 0, 1, Sports::State::Final),
           game("Fulham", "Brentford", 3, 1, Sports::State::Final)},
          "football", "Premier League");
  assert(frameCountOf(kBoardTwo, 0) == Sports::kMaxGames &&
         "a full scoreboard did not frame every game it holds");

  Sports::clearAll();
  setCard(kBoardOne,
          {game("Celtics", "Knicks", 1, 0, Sports::State::Final),
           game("Lakers", "Nets", 2, 2, Sports::State::Final),
           game("Heat", "Bulls", 0, 1, Sports::State::Final),
           game("Suns", "Jazz", 3, 1, Sports::State::Final),
           game("Bucks", "Pacers", 4, 4, Sports::State::Final),
           game("Kings", "Magic", 5, 2, Sports::State::Final)},
          "basketball", "NBA");
  assert(frameCountOf(kBoardOne, 0) == Sports::kMaxGames &&
         "a six-game payload framed more than the four games this firmware stores");
  std::cout << "  a scoreboard frames no more than the games it stores\n";
}

/// A single-game card frames one, which is the answer that makes CardManager's
/// frame loop do nothing.
///
/// This is the case that bites when `case 3:` leaves frameCountFor(): sportslive
/// takes frameCount4, index 4 is a board, and the live card frames the second
/// scoreboard's two games. The counts in populateEveryCard() differ for exactly
/// this reason.
void aSingleGameCardReportsOneFrame() {
  populateEveryCard();
  assert(frameCountOf(kTeamOne, 0) == 1 && "sportsteam framed more than the one game it draws");
  assert(frameCountOf(kTeamTwo, 0) == 1 && "sportsteam2 framed more than the one game it draws");
  assert(frameCountOf(kLive, 0) == 1 &&
         "sportslive framed more than one game - its case has left frameCountFor() and it is "
         "answering from the default arm, which is a scoreboard");
  std::cout << "  a single-game card reports one frame\n";
}

/// An empty board frames nothing. It reports no items either, so it never
/// draws - this holds the two answers consistent rather than describing a
/// reachable screen.
void anEmptyScoreboardReportsNoFrames() {
  Sports::clearAll();
  assert(frameCountOf(kBoardOne, 0) == 0 && "an empty sportsscores framed a game");
  assert(frameCountOf(kBoardTwo, 0) == 0 && "an empty sportsscores2 framed a game");
  std::cout << "  an empty scoreboard reports no frames\n";
}

// --- the id a payload names --------------------------------------------------

/// A payload for an id this firmware does not register changes nothing. The
/// second league's payload used to take this path, which is why it drew nothing
/// and why registering the id was the whole fix.
///
/// sportsstandings is the id used here since 2026-11-09, when all eleven sports
/// ids became registered and sportsscores3 stopped being an example of one that
/// is not. It is a real id - SPORTS_CARDS_DESIGN.md section 4.4 proposes it and
/// nothing builds it - so this case keeps testing the path a server advertising
/// something this firmware has never heard of actually takes.
void aPayloadForAnUnregisteredIdChangesNothing() {
  populateEveryCard();
  const std::vector<Sports::Game> games = {game("Dodgers", "Giants", 5, 3, Sports::State::Final)};
  Sports::setGames("sportsstandings", games.data(), 1, 0, "baseball", "MLB");

  assert(findCard("sportsstandings") == nullptr && "sportsstandings is registered after all");
  for (size_t i = 0; i < kIdCount; ++i) {
    assert(keyOf(kIds[i], 0).find("Dodgers") == std::string::npos &&
           "a payload for an unregistered id landed on a registered card");
  }
  std::cout << "  a payload for an unregistered id changes nothing\n";
}

/// clearAll() empties every index, so yesterday's fixtures cannot sit on screen
/// past their day. Walked over the id table rather than the first card, because
/// a loop bounded at four would leave the fifth card holding last night's games.
void clearingEmptiesEveryCard() {
  populateEveryCard();
  Sports::clearAll();
  for (size_t i = 0; i < kIdCount; ++i) {
    assert(itemCountOf(kIds[i]) == 0 && "a card still reported items after clearAll()");
    assert(keyOf(kIds[i], 0).empty() && "a card still reported a key after clearAll()");
  }
  std::cout << "  clearing empties every card\n";
}

}  // namespace

// --- the nine definitions Sports.cpp links against --------------------------

namespace Cards {

bool registerCard(const CardSpec& spec, KeyPolicy keyPolicy) {
  // The check production's registerCard() applies, reached through the same
  // predicate, so a module compiled here that states a policy its descriptor
  // does not satisfy stops this test rather than a device. All five sports
  // registrations state Resolves and all five assign a listingId.
  assert(keyPolicyHolds(spec, keyPolicy));
  if (registry().size() >= registry().capacity()) {
    return false;
  }
  registry().push_back(spec);
  return true;
}

}  // namespace Cards

namespace Display {

int utcOffsetMinutes() { return -300; }

bool use12HourClock() { return false; }

String formatTimeOfDay(int hour24, int minute) {
  char rendered[8];
  snprintf(rendered, sizeof(rendered), "%d:%02d", hour24, minute);
  return String(rendered);
}

String describeAnswerAge(time_t observedAtUtc) {
  // Never reached: every payload above carries staleSinceUtc 0, which is the
  // server saying the card is current, and staleAgeText() returns before asking.
  (void)observedAtUtc;
  return String();
}

void showSportsCard(const char* homeName, const String& homeScore, const char* awayName,
                    const String& awayScore, const String& status, uint16_t itemNumber,
                    uint16_t itemCount, const String& ageText, const char* sportLabel,
                    const char* competitionLabel, const char* progressLabel,
                    const char* clockLabel, bool isLive) {
  (void)homeScore;
  (void)awayScore;
  (void)status;
  (void)itemNumber;
  (void)itemCount;
  (void)ageText;
  (void)sportLabel;
  (void)progressLabel;
  (void)clockLabel;
  (void)isLive;

  CardDraw& held = lastCard();
  held.happened = true;
  held.awayName = awayName != nullptr ? awayName : "";
  held.homeName = homeName != nullptr ? homeName : "";
  held.competition = competitionLabel != nullptr ? competitionLabel : "";
}

void showSportsBoard(const SportsBoardRow* rows, uint8_t count, const String& ageText,
                     const char* sportLabel, const char* competitionLabel) {
  (void)ageText;
  (void)sportLabel;

  BoardDraw& held = lastBoard();
  held.happened = true;
  held.rowCount = count;
  held.awayNames.clear();
  for (uint8_t i = 0; i < count; ++i) {
    held.awayNames.push_back(rows[i].awayName);
  }
  held.competition = competitionLabel != nullptr ? competitionLabel : "";
}

}  // namespace Display

namespace Log {

void printf(const char* format, ...) { (void)format; }

void verbose(const char* format, ...) { (void)format; }

}  // namespace Log

int main() {
  everyIdInTheTableRegisters();
  asManyCardsRegisterAsThereAreIds();
  everyRegistrationCarriesItsHooks();

  bothScoreboardsReportOneItem();
  theTeamCardsReportOneItemAGame();
  theLiveCardReportsOneItemAGame();
  theLiveCardReportsNothingWithNoGameInProgress();
  anEmptyScoreboardReportsNoItems();

  eachCardReportsAKeyForItsOwnGame();
  noTwoCardsReportTheSameKey();
  eachCardDescribesItsOwnGame();
  anItemPastACardReportsNothing();
  anEmptyCardReportsNoKey();
  eachCardDrawsItsOwnGames();
  onlyTheScoreboardsReachTheTableRenderer();

  aScoreboardReportsAFramePerGame();
  aScoreboardFramesNoMoreThanTheGamesItStores();
  aSingleGameCardReportsOneFrame();
  anEmptyScoreboardReportsNoFrames();

  aPayloadForAnUnregisteredIdChangesNothing();
  clearingEmptiesEveryCard();

  std::cout << "PASS: sports card registration and hook switches (21 cases)\n";
  return 0;
}
