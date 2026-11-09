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
/// **FIVE CARD INSTANCES, NOT FOUR AND NOT ELEVEN.** The server's catalog
/// advertises eleven sports ids (sportsteam1-5, sportsscores1-5, sportslive).
/// This firmware registers five of them, and the count is a decision each time
/// it moves rather than a gap waiting to be filled:
///
///   sportsteam     one followed team
///   sportsteam2    a second followed team
///   sportsscores   one league's games today
///   sportsscores2  a second league's games today
///   sportslive     appears only while a followed team is playing
///
/// **sportsscores2 is here because one league was the whole limit.** An account
/// could follow exactly one, and this file was the reason. The server already
/// resolves all five scoreboards (SportsCardConfig lists sportsscores through
/// sportsscores5, and MySportsService iterates every one), so a second league
/// needs nothing added to the wire and nothing new parsed - it needed a second id
/// registered here for the payload to land on instead of being dropped with a log
/// line. Registered 2026-11-08.
///
/// **Registering it does not by itself offer it.** The server decides what a
/// household can pick, and three places there still say nothing draws this id:
/// KnownCards.cs carries FirmwareRegisters: false for sportsscores2 and the same
/// four ids in its FirmwareRegisteredIdSnapshot, and the household page's
/// ScoreboardSlots lists only sportsscores. Until those move, the only route to a
/// configured second league is a raw config write through /diag/sports. They move
/// after this firmware ships, in that order - a server offering a league no device
/// draws is the failure that flag exists to prevent.
///
/// **WHAT THE FIFTH CARD ACTUALLY COST, measured rather than estimated.** 832
/// bytes of static RAM on the ESP32-2432S028: the App's reported global variables
/// went 115,524 -> 116,356 across two arduino-cli builds of this sketch, one
/// either side of this change, and flash went 1,591,787 -> 1,592,939. Roughly 290
/// of the 832 is the CardSlot below; the rest is one Cards::CardSpec in
/// CardManager's registry array, which carries assetId, text, qrData and location
/// buffers and is the larger half.
///
/// That is three times what the arithmetic over this file alone predicts, because
/// the CardSpec is not in this file. It also corrects the figure this note used to
/// carry: covering all eleven ids is about 5 KB of static RAM, not the 1.6 KB
/// written here before anybody built it both ways.
///
/// **WHAT THE HEAP HEADROOM IS, AND WHAT NOBODY CAN ESTABLISH.** The reason .bss
/// is weighed this carefully is that a device short of contiguous internal 8-bit
/// heap cannot open a TLS session, so it cannot check in, so it cannot be told an
/// update exists. Four things about the figures, because three of them have been
/// misread here before:
///
///   - **16,717 twice over is the stock-core answer, not a constant.** Http.h
///     derives it and says so: CI builds stock, where IN and OUT are both 16,717
///     and a session holds 33,434. The core overlay on this workstation sets
///     ASYMMETRIC_CONTENT_LEN, giving 16,717 in and 4,429 out for 21,146. The two
///     binaries are 12,288 bytes apart on this question.
///   - **One contiguous run is not the whole gate.** TlsRoots.h records that chain
///     verification turns on TOTAL free internal 8-bit heap, with device 17
///     verifying successfully at largest8 = 14,324 - under the single-buffer
///     figure. Above 16,717 the first buffer can be found; whether the session
///     completes is a second question.
///   - **A boot reading is a ceiling.** BootDiag::recordBootHeap() runs at the top
///     of setup() before the display, the SD mount, WiFi, TLS or the splash decode
///     have taken anything, and App.ino gives that as the reason: measuring later
///     would understate what the boot started with. So
///     bootLargestFreeBlock8BitInternalBytes is this boot's high-water mark by
///     construction. The owner read 59,380 off the fleet on 2026-11-08; the
///     figures recorded in this tree for device 17 are 65,524 and 90,100, so the
///     reading is per-device and per-build and none of them is a floor.
///   - **No instrument reports a low-water largest block.** HeapRatchet observes
///     at phase BOUNDARIES and declines to sample mid-phase on purpose, so
///     heapPhaseDrawBytes is a signed net across two quiescent readings and its
///     own floor is explicitly not the minimum ever seen.
///     largestFreeBlock8BitInternalBytes is a spot reading taken while telemetry
///     POSTs, which is quiescent too. restartLargestFreeBlockBytes is the one
///     non-quiescent figure on the wire - the largest block as the previous boot
///     decided to restart, read 34,804 at essentially every one of 31 restarts -
///     but it is a single instant at a decision, absent on a clean power-on, and
///     says nothing about a device that has never restarted. Nothing takes a
///     minimum over samples, on the device or on the server.
///
/// So the worst case through a draw is not knowable from what is instrumented
/// today, and this step is sized for that: one card, the cheapest one that answers
/// the question the owner asked. All five leagues wants kMaxCards 8 here, three
/// more registrations, and Cards::kMaxCards at 39 - about 2.5 KB more static RAM -
/// and that should wait for an instrument that reports a trough rather than a
/// boundary.
///
/// A policy naming one of the other six is ignored with a log line, exactly as a
/// policy naming "weather" already is, and instances 3-5 become a later
/// registration if anybody asks for them.
///
/// **FIXED BUFFERS, NOT String.** Every other card here uses Arduino String
/// freely, and this one deliberately does not. Five cards x four games x two
/// team names is forty String allocations churning on every check-in, on
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

/// Card instances this firmware registers. Five, deliberately - see the class
/// remarks for why not four and why not eleven. Cards::kMaxCards has to carry
/// one descriptor per registration, so a change here is a change there.
static constexpr uint8_t kMaxCards = 5;

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
///
/// **A stored score is zero or above, or it is kNoScore.** CheckIn.cpp stores
/// kNoScore for everything else, and names in the stream what it was sent. -1 is
/// this sentinel and the rest of the negative range joins it because none of the
/// five sports sportChipLabel() maps scores below zero, and a rule that drew -2
/// while blanking -1 would answer two ways about one range. A wire value above
/// INT16_MAX is rejected there too, because the narrowing to int16_t is modular
/// and 65536 would otherwise arrive as a nil-nil draw.
/// CardColumns::boardScoreIsDrawable() is the predicate every reader of these
/// two fields goes through, and TEXT_LAYOUT_DESIGN.md section 15m is the
/// decision.
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
/// `cardId` must be one of the five registered ids; anything else is logged and
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
///
/// Returns a string literal or the empty string, so a caller that asks on every
/// frame copies nothing onto the heap. The draw path's small repeated
/// allocations are the ones that cost this board its largest contiguous block;
/// see HeapRatchet.h.
const char* sportChipLabel(const char* sport);

/// Drop everything. Called when a check-in reports no sports content at all, so
/// yesterday's fixtures cannot linger on screen past their day.
void clearAll();

}  // namespace Sports
