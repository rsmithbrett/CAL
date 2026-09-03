#pragma once

#include <Arduino.h>

/// What a card *is*, as data - the descriptor every card module hands to the
/// scheduler, and the registry those descriptors live in.
///
/// This file exists because a card used to be smeared across three places: a
/// fetch module (Weather.cpp, Aircraft.cpp), a draw function in Display.cpp,
/// and a hardcoded `enum class CardKind { Weather, Aircraft }` toggle in
/// App.ino that named both of them. Adding a third card meant editing all
/// three, and the toggle in particular could only ever express "two cards,
/// alternating". A registry of descriptors replaces the toggle outright:
/// adding a card type is registering a descriptor, with no edit to the
/// scheduler at all.
///
/// This is deliberately NOT how CYD-Dickey does it. That project (the prior
/// art this scheduler is otherwise ported from - see CardManager.cpp) uses a
/// hardcoded `enum class CardSlot { Base, Weather, Splash, Qr }` alongside
/// three separately-named globals `cardsSinceWeather`/`cardsSinceSplash`/
/// `cardsSinceQr`. That works for exactly the four card types it has and
/// requires a new enum case, a new global, a new `if` in computeNextCard()
/// and a new `case` in drawDashboardScreen() for a fifth. Card types on this
/// project are expected to keep growing, so every one of those named globals
/// becomes a per-descriptor struct field here (see `cardsSince` below) and
/// the enum becomes a registry index.
///
/// **fetch() and draw() are separate on purpose, and must stay that way.**
/// Reverse navigation is a pure redraw of state the card is already holding -
/// stepping back to the card you just saw must show you *that* card, not a
/// fresh network fetch that might return something different. The scheduler
/// therefore never calls fetch() from a navigation path; only from its own
/// refresh timer. See CardManager.h.
namespace Cards {

enum class Kind : uint8_t {
  /// A variable-length collection whose items are cycled one at a time, each
  /// item getting its own dwell (aircraft overhead, calendar events).
  List,
  /// A singleton that interleaves *after every N other cards* rather than
  /// taking a fixed slot in the rotation. The distinction is load-bearing -
  /// see CardManager.cpp's computeNext() for why a fixed slot is wrong.
  Interstitial,
};

/// Refresh this card's retained state from the server. Called only by the
/// scheduler's refresh timer, never by a navigation path.
using FetchFn = void (*)();

/// How many items this card can draw *right now*. 0 means the card has
/// nothing to show and the scheduler passes over it entirely rather than
/// putting a blank screen in the rotation. A card holding an explanatory
/// status ("no aircraft within 10 mi", "weather is not activated") is NOT
/// empty - that message is content.
using ItemCountFn = uint16_t (*)();

/// Draw item `itemIndex` from retained state. Must not fetch.
using DrawFn = void (*)(uint16_t itemIndex);

/// Optional: true when this particular item deserves the longer
/// `notableDwellSeconds` hold instead of the ordinary one. nullptr means
/// "never notable". Generalises CYD-Dickey's `aircraftOverheadSeconds`,
/// which gives a plane nearly directly overhead a longer look than one at
/// the edge of the radius.
using NotableFn = bool (*)(uint16_t itemIndex);

struct CardSpec {
  /// Matches the `id` the server uses in cardPolicy/cardActions. An id the
  /// server sends that no descriptor here claims is ignored, not an error -
  /// that is what lets the server add a card type before firmware supports
  /// it, and lets firmware up to six months old keep working.
  const char* id = "";

  Kind kind = Kind::List;
  FetchFn fetch = nullptr;
  ItemCountFn itemCount = nullptr;
  DrawFn draw = nullptr;
  NotableFn isNotable = nullptr;

  // ---- Policy. Built-in defaults until a cardPolicy arrives on check-in,
  // then replaced wholesale by whatever the server said (see
  // CardManager::applyPolicy).

  /// Rotation position among list cards, and the tie-break when two
  /// interstitials come due on the same tick - lowest wins.
  int16_t order = 0;
  /// 0 means "use the policy's defaultDwellSeconds".
  uint16_t dwellSeconds = 0;
  /// 0 means "no override"; list cards only.
  uint16_t notableDwellSeconds = 0;
  /// Show after every N other cards. 0 means never interleaves;
  /// interstitials only.
  uint16_t interleaveEvery = 0;
  /// False when a received policy did not mention this card - the server's
  /// way of turning a card off. True until the first policy ever arrives, so
  /// a device that has never checked in still shows something.
  bool active = true;

  // ---- Per-card scheduling state. Each of these is a struct field
  // precisely because CYD-Dickey's equivalents are named globals, one set
  // per card type.

  /// Cards shown since this one last was - the interleave counter. Every
  /// registered card's counter ticks on every computed card; whichever
  /// interstitial exceeds its own interleaveEvery first is what shows.
  uint16_t cardsSince = 0;
  /// millis() of the last completed fetch. Drives the refresh timer only.
  uint32_t lastFetchMs = 0;
  bool everFetched = false;
};

static constexpr uint8_t kMaxCards = 8;

/// Called from each card module's own translation unit at static-init time
/// (see the `kRegistered` idiom at the bottom of Weather.cpp/Aircraft.cpp),
/// so App.ino never names a specific card. Returns false, and logs, if the
/// registry is full - a silently-dropped card would be very hard to notice
/// on firmware with no tests.
bool registerCard(const CardSpec& spec);

uint8_t count();
CardSpec& at(uint8_t index);
/// -1 when no registered card claims that id.
int8_t indexOf(const char* id);

// ---- The cardPolicy wire shape, in the form CheckIn.cpp parses into and
// CardManager::applyPolicy() consumes. Lives here rather than in CheckIn.h
// so CardManager does not have to include the check-in module to be told
// what its own policy is.

static constexpr uint8_t kMaxPolicyCards = 8;

struct PolicyEntry {
  String id;
  /// "list" or "interstitial". Anything else leaves the card's built-in kind
  /// alone rather than failing.
  String kind;
  int order = 0;
  int dwellSeconds = 0;
  /// Interstitials only; 0 when the server omitted it.
  int interleaveEvery = 0;
  /// List cards only; 0 when the server omitted it.
  int notableDwellSeconds = 0;
};

struct Policy {
  /// False for "the server sent no cardPolicy this time", which means keep
  /// using whatever policy is already in force - explicitly not "show
  /// nothing". A server that cannot resolve a policy never blanks a screen
  /// that was working.
  bool present = false;
  int defaultDwellSeconds = 0;
  int manualNavHoldSeconds = 0;
  uint8_t entryCount = 0;
  PolicyEntry entries[kMaxPolicyCards];
};

}  // namespace Cards
