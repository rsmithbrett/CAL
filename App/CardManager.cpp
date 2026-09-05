#include "CardManager.h"

#include "Actions.h"
#include "Config.h"
#include "Display.h"
#include "Log.h"
#include "Touch.h"

// ---------------------------------------------------------------------------
// The registry declared in Cards.h lives here rather than in a Cards.cpp of
// its own: the registry and the scheduler that walks it are one concept, and
// splitting them would leave a translation unit holding nothing but an array.
//
// Both objects below are constant-initialised (every member of CardSpec has a
// literal default), so they are ready before any other translation unit's
// static initialisers run. That matters: each card module registers itself
// from a static initialiser (see the kRegistered idiom at the bottom of
// Weather.cpp and Aircraft.cpp), and static initialisation order across
// translation units is otherwise undefined.
// ---------------------------------------------------------------------------
namespace {
Cards::CardSpec gCards[Cards::kMaxCards];
uint8_t gCardCount = 0;
}  // namespace

namespace Cards {

bool registerCard(const CardSpec& spec) {
  if (gCardCount >= kMaxCards) {
    // Logged rather than silently dropped. On firmware with no tests, a card
    // that quietly never appears is close to undiagnosable.
    Log::printf("[cards] registry full - card '%s' was NOT registered", spec.id);
    return false;
  }
  gCards[gCardCount++] = spec;
  return true;
}

uint8_t count() { return gCardCount; }

CardSpec& at(uint8_t index) { return gCards[index]; }

int8_t indexOf(const char* id) {
  if (id == nullptr) {
    return -1;
  }
  for (uint8_t i = 0; i < gCardCount; ++i) {
    if (strcmp(gCards[i].id, id) == 0) {
      return static_cast<int8_t>(i);
    }
  }
  return -1;
}

}  // namespace Cards

namespace CardManager {
namespace {

/// Where the rotation currently is. `card` is a registry index; -1 means
/// nothing is showing at all (boot, or every registered card empty).
struct Position {
  int8_t card = -1;
  uint16_t item = 0;
};

Position gCurrent;

/// The position within the *list* cards specifically, kept separately from
/// gCurrent so an interstitial firing does not lose the reader's place in the
/// list sequence. This is exactly why CYD-Dickey keeps `baseCardIndex`
/// separate from `currentSlot`.
Position gListCursor;

/// Forward/reverse history. Every genuinely-new card is recorded here as it
/// is first shown; rewinding replays entries exactly rather than running the
/// scheduler backwards. 24 entries, same as CYD-Dickey's CARD_HISTORY_CAP -
/// deep enough to step back through a couple of minutes of rotation, small
/// enough to be a fixed array.
constexpr uint8_t kHistoryCap = 24;
Position gHistory[kHistoryCap];
uint8_t gHistoryCount = 0;
uint8_t gHistoryCursor = 0;

/// Policy, with built-in defaults that hold until the first cardPolicy ever
/// arrives. 12 seconds matches the spec's own example default; 30 seconds of
/// manual hold matches CYD-Dickey's MANUAL_NAV_HOLD_MS, which was arrived at
/// on a running device.
uint16_t gDefaultDwellSeconds = 12;
uint32_t gManualNavHoldMs = 30000UL;

uint32_t gLastSwitchMs = 0;
uint32_t gManualHoldUntilMs = 0;

/// The buttons currently drawn, in the same order as the touch zones handed
/// to Touch::setActionZones() - so a Hit::ActionButton's index addresses this
/// array directly.
Actions::Definition gButtons[Actions::kMaxButtonsPerCard];
uint8_t gButtonCount = 0;

/// Round-robin start point for the refresh sweep, so one card whose fetch
/// keeps coming due first cannot starve the others.
uint8_t gRefreshScan = 0;

/// True when this card is active and actually has something to draw right
/// now. A card with zero items is passed over entirely rather than put on
/// screen blank. Note that a card holding an explanatory status ("no aircraft
/// within 10 mi", "weather is not activated") reports one item, not zero -
/// that message is content, and only a card with genuinely nothing to say
/// (typically one that has never fetched) is skipped.
bool showable(uint8_t index) {
  const Cards::CardSpec& card = gCards[index];
  return card.active && card.itemCount != nullptr && card.draw != nullptr &&
         card.itemCount() > 0;
}

/// Total ordering over the registry: `order` first, registration index as the
/// tie-break so the ordering is always strict and never depends on scan
/// direction.
bool earlier(uint8_t a, uint8_t b) {
  if (gCards[a].order != gCards[b].order) {
    return gCards[a].order < gCards[b].order;
  }
  return a < b;
}

int8_t firstShowable(Cards::Kind kind) {
  int8_t best = -1;
  for (uint8_t i = 0; i < gCardCount; ++i) {
    if (gCards[i].kind != kind || !showable(i)) {
      continue;
    }
    if (best < 0 || earlier(i, static_cast<uint8_t>(best))) {
      best = static_cast<int8_t>(i);
    }
  }
  return best;
}

/// The next showable card of `kind` strictly after `after` in the ordering
/// above, wrapping around to the first. `after` < 0 starts from the top.
int8_t nextShowable(Cards::Kind kind, int8_t after) {
  if (after < 0) {
    return firstShowable(kind);
  }
  int8_t best = -1;
  for (uint8_t i = 0; i < gCardCount; ++i) {
    if (gCards[i].kind != kind || !showable(i)) {
      continue;
    }
    if (!earlier(static_cast<uint8_t>(after), i)) {
      continue;
    }
    if (best < 0 || earlier(i, static_cast<uint8_t>(best))) {
      best = static_cast<int8_t>(i);
    }
  }
  return best >= 0 ? best : firstShowable(kind);
}

/// Which interstitial, if any, has waited long enough. Every registered
/// card's counter has already been ticked by the caller; the first to exceed
/// its own interleaveEvery wins, ties broken by `order`.
///
/// The counters are fully independent by design. CYD-Dickey originally forced
/// two of its singletons into a fixed pair (QR always following splash) and
/// records having corrected that to two separate schedules - so nothing here
/// couples one interstitial's cadence to another's.
int8_t dueInterstitial() {
  int8_t best = -1;
  for (uint8_t i = 0; i < gCardCount; ++i) {
    const Cards::CardSpec& card = gCards[i];
    if (card.kind != Cards::Kind::Interstitial || card.interleaveEvery == 0) {
      continue;
    }
    if (!showable(i)) {
      continue;
    }
    if (card.cardsSince <= card.interleaveEvery) {
      continue;
    }
    if (best < 0 || earlier(i, static_cast<uint8_t>(best))) {
      best = static_cast<int8_t>(i);
    }
  }
  return best;
}

/// Computes a genuinely new next card. Only ever called from advance() once
/// the history cursor is at the frontier - anywhere behind the frontier,
/// advancing replays what was actually shown instead.
Position computeNext() {
  // Every active card's counter ticks on every computed card, including the
  // one that ends up being an interstitial; whichever type's interval is
  // reached first is what shows and the others just wait one more tick.
  for (uint8_t i = 0; i < gCardCount; ++i) {
    if (gCards[i].active && gCards[i].cardsSince < 0xFFFF) {
      gCards[i].cardsSince++;
    }
  }

  const int8_t interstitial = dueInterstitial();
  if (interstitial >= 0) {
    gCards[interstitial].cardsSince = 0;
    return Position{interstitial, 0};
  }

  // Otherwise the next item in the list sequence: the next item within the
  // current list card, then the next list card, wrapping.
  const int8_t current = gListCursor.card;
  if (current >= 0 && showable(static_cast<uint8_t>(current)) &&
      gCards[current].kind == Cards::Kind::List) {
    const uint16_t total = gCards[current].itemCount();
    if (static_cast<uint32_t>(gListCursor.item) + 1 < total) {
      gListCursor.item++;
      return gListCursor;
    }
    const int8_t next = nextShowable(Cards::Kind::List, current);
    if (next >= 0) {
      gListCursor = Position{next, 0};
      return gListCursor;
    }
  } else {
    const int8_t first = firstShowable(Cards::Kind::List);
    if (first >= 0) {
      gListCursor = Position{first, 0};
      return gListCursor;
    }
  }

  // No list card has anything to show. Rotate through the interstitials
  // instead, so a device configured with singletons only - or one whose list
  // cards are all empty right now - still cycles rather than freezing on one
  // card. Their interleave counters are reset as they show, so the cadence
  // picks up correctly the moment a list card has data again.
  const int8_t onlySingletons = nextShowable(Cards::Kind::Interstitial, gCurrent.card);
  if (onlySingletons >= 0) {
    gCards[onlySingletons].cardsSince = 0;
    return Position{onlySingletons, 0};
  }

  return Position();
}

void pushHistory(const Position& position) {
  if (gHistoryCount < kHistoryCap) {
    gHistory[gHistoryCount] = position;
    gHistoryCount++;
    gHistoryCursor = gHistoryCount - 1;
    return;
  }
  for (uint8_t i = 1; i < kHistoryCap; ++i) {
    gHistory[i - 1] = gHistory[i];
  }
  gHistory[kHistoryCap - 1] = position;
  gHistoryCursor = kHistoryCap - 1;
}

/// Collapses history back to a single entry. Called whenever fresh data makes
/// older recorded positions meaningless - a recorded item index could
/// otherwise be replayed against a list that no longer holds the same items
/// at the same positions. Same reason CYD-Dickey's resetCardHistory() exists,
/// and it deliberately leaves the interleave counters alone so a data refresh
/// does not throw off the singletons' cadence.
void resetHistory(const Position& position) {
  gHistory[0] = position;
  gHistoryCount = 1;
  gHistoryCursor = 0;
}

/// Adopts whatever the history cursor now points at. A list entry also
/// restores the list cursor, so stepping forward off the end of a rewound
/// stretch resumes the sequence from the right place.
void applyHistory() {
  gCurrent = gHistory[gHistoryCursor];
  if (gCurrent.card >= 0 && gCards[gCurrent.card].kind == Cards::Kind::List) {
    gListCursor = gCurrent;
  }
}

uint32_t dwellMs() {
  if (gCurrent.card < 0) {
    return static_cast<uint32_t>(gDefaultDwellSeconds) * 1000UL;
  }
  const Cards::CardSpec& card = gCards[gCurrent.card];
  uint16_t seconds = card.dwellSeconds > 0 ? card.dwellSeconds : gDefaultDwellSeconds;
  // The conditional dwell override: an item the card itself considers more
  // interesting gets a longer hold. Generalises CYD-Dickey's
  // aircraftOverheadSeconds, which gives a plane nearly directly overhead 20
  // seconds where an ordinary one gets 8.
  if (card.notableDwellSeconds > 0 && card.isNotable != nullptr &&
      card.isNotable(gCurrent.item)) {
    seconds = card.notableDwellSeconds;
  }
  return static_cast<uint32_t>(seconds) * 1000UL;
}

/// Draws the buttons the current card should show, registers their hit zones
/// with Touch, and adds the forward/reverse affordances. Called after the
/// card itself has drawn, so it lands on top of a finished card rather than
/// being painted over by it.
void drawChrome(const Cards::CardSpec& card) {
  gButtonCount = Actions::forCard(card.id, gButtons, Actions::kMaxButtonsPerCard);

  String labels[Actions::kMaxButtonsPerCard];
  for (uint8_t i = 0; i < gButtonCount; ++i) {
    labels[i] = gButtons[i].label;
  }
  Display::drawActionButtons(labels, gButtonCount);

  Touch::Rect zones[Actions::kMaxButtonsPerCard];
  for (uint8_t i = 0; i < gButtonCount; ++i) {
    Display::actionButtonZone(i, gButtonCount, zones[i].x, zones[i].y, zones[i].w, zones[i].h);
  }
  // Always set, even at zero, so a zone belonging to the previous card can
  // never still be live under the current one.
  Touch::setActionZones(zones, gButtonCount);

  Display::drawNavAffordances(/*canReverse=*/gHistoryCursor > 0);
}

void drawCurrent() {
  if (gCurrent.card < 0 || gCurrent.card >= static_cast<int8_t>(gCardCount) ||
      !showable(static_cast<uint8_t>(gCurrent.card))) {
    gButtonCount = 0;
    Touch::setActionZones(nullptr, 0);
    Display::showNoContent("Nothing to show yet",
                           "Waiting for the first update from the server.");
    return;
  }

  Cards::CardSpec& card = gCards[gCurrent.card];
  const uint16_t total = card.itemCount();
  if (gCurrent.item >= total) {
    gCurrent.item = 0;
  }
  card.draw(gCurrent.item);
  drawChrome(card);
}

void show(const Position& position) {
  gCurrent = position;
  gLastSwitchMs = millis();
  drawCurrent();
}

void advance() {
  // Behind the frontier: replay the card that was actually shown here rather
  // than recomputing. Recomputing could put a different card in a position
  // the user has already stepped past, which would make "which card is where"
  // depend on which direction they happen to be travelling.
  if (static_cast<uint16_t>(gHistoryCursor) + 1 < gHistoryCount) {
    gHistoryCursor++;
    applyHistory();
    gLastSwitchMs = millis();
    drawCurrent();
    return;
  }

  const Position next = computeNext();
  pushHistory(next);
  applyHistory();
  gLastSwitchMs = millis();
  drawCurrent();
}

void rewind() {
  if (gHistoryCursor == 0) {
    return;
  }
  gHistoryCursor--;
  applyHistory();
  gLastSwitchMs = millis();
  drawCurrent();
}

/// Suppresses the auto-advance timer for a while after any deliberate touch.
/// A card someone picked on purpose must not be yanked away after the ordinary
/// dwell - they get a longer, fixed look at it before automatic cycling
/// resumes. Server-configurable here (manualNavHoldSeconds) where CYD-Dickey
/// hardcodes it as MANUAL_NAV_HOLD_MS.
void holdOffAutoAdvance() {
  if (gManualNavHoldMs > 0) {
    gManualHoldUntilMs = millis() + gManualNavHoldMs;
  }
}

void handleTap(const Touch::Tap& tap) {
  switch (tap.hit) {
    case Touch::Hit::ActionButton: {
      if (tap.actionIndex >= gButtonCount) {
        return;
      }
      const Actions::Definition& pressed = gButtons[tap.actionIndex];
      Log::printf("[cards] action button pressed: card=%s actionId=%s",
                  pressed.cardId.c_str(), pressed.actionId.c_str());
      Actions::recordPress(pressed);
      // Acknowledges the *press*, not the delivery. The contract is
      // deliberate about there being no "sent" state and no round trip - the
      // press rides the next ordinary check-in and the user waits for
      // nothing - but a button that does not visibly react to a finger reads
      // as a dead button, which is its own failure. This flashes the button
      // and puts it straight back; it claims nothing about what the server
      // did with it.
      Display::flashActionButton(tap.actionIndex, gButtonCount, pressed.label);
      // A card someone just pressed a button on should not be yanked away a
      // second later, same as a manual navigation.
      holdOffAutoAdvance();
      return;
    }
    case Touch::Hit::Reverse:
      Log::line("[cards] reverse tap");
      // Same reasoning as the ActionButton case above: the edge strip has no
      // chrome of its own, so without this the tap produced no visible
      // reaction at all, registered or not. Flash first, then act, so the
      // acknowledgment isn't delayed by whatever rewind() draws next.
      Display::flashNavEdge(/*isForward=*/false, /*canReverse=*/gHistoryCursor > 0);
      rewind();
      holdOffAutoAdvance();
      return;
    case Touch::Hit::Forward:
      Log::line("[cards] forward tap");
      Display::flashNavEdge(/*isForward=*/true, /*canReverse=*/gHistoryCursor > 0);
      advance();
      holdOffAutoAdvance();
      return;
    case Touch::Hit::None:
    default:
      return;
  }
}

void fetchCard(uint8_t index) {
  Cards::CardSpec& card = gCards[index];
  if (card.fetch == nullptr) {
    return;
  }
  card.fetch();
  card.lastFetchMs = millis();
  card.everFetched = true;

  // Recorded positions can no longer be trusted to mean the same items, so
  // collapse the ring to wherever we are now.
  resetHistory(gCurrent);

  if (gCurrent.card < 0) {
    // Nothing was on screen (boot, or everything empty until now) - put the
    // first thing we have up immediately rather than waiting out a dwell.
    show(computeNext());
    return;
  }
  if (gCurrent.card == static_cast<int8_t>(index)) {
    // Fresh data for the card actually on screen: redraw it in place. Not an
    // advance - a refresh must never make the rotation skip a card.
    drawCurrent();
  }
}

/// At most one card per call. A sweep that fetched everything due at once
/// would sit inside a single poll() for several HTTP round trips with touch
/// and the dwell timer unserviced the whole time; one per call means the loop
/// gets a turn between each. Cheap on the overwhelming majority of calls -
/// nothing is due, so this is a scan of at most eight structs.
void refreshOneDueCard() {
  const uint32_t now = millis();
  for (uint8_t attempt = 0; attempt < gCardCount; ++attempt) {
    const uint8_t index = (gRefreshScan + attempt) % gCardCount;
    Cards::CardSpec& card = gCards[index];
    if (!card.active || card.fetch == nullptr) {
      continue;
    }
    const bool due = !card.everFetched ||
                     (now - card.lastFetchMs) >= Config::kContentRefreshIntervalMs;
    if (!due) {
      continue;
    }
    gRefreshScan = (index + 1) % gCardCount;
    fetchCard(index);
    return;
  }
}

}  // namespace

void begin() {
  Actions::begin();

  if (gCardCount == 0) {
    // Cannot happen with the cards this build registers, but a registry that
    // silently ended up empty would otherwise look exactly like a server
    // problem. Said out loud instead.
    Log::line("[cards] NO CARDS REGISTERED - the rotation will be empty");
    Display::showNoContent("Nothing to show", "This build has no cards registered.");
    return;
  }

  Log::printf("[cards] %u card(s) registered", gCardCount);
  gCurrent = Position();
  gListCursor = Position();
  resetHistory(gCurrent);

  // Fetch one card so something real is on screen quickly; poll()'s refresh
  // sweep fills the rest in over the following seconds.
  for (uint8_t i = 0; i < gCardCount; ++i) {
    const uint8_t index = static_cast<uint8_t>(i);
    if (gCards[index].active && gCards[index].fetch != nullptr) {
      gRefreshScan = (index + 1) % gCardCount;
      fetchCard(index);
      return;
    }
  }
}

void poll() {
  Touch::Tap tap;
  if (Touch::poll(tap)) {
    handleTap(tap);
  }

  const uint32_t now = millis();
  // Signed difference rather than a plain `now >= gManualHoldUntilMs`, so a
  // hold set moments before millis() wraps at ~49.7 days does not read as
  // "hold forever".
  const bool holdExpired = static_cast<int32_t>(now - gManualHoldUntilMs) >= 0;
  if (holdExpired && (now - gLastSwitchMs) >= dwellMs()) {
    advance();
  }

  refreshOneDueCard();
}

void applyPolicy(const Cards::Policy& policy) {
  if (!policy.present) {
    // "The server sent no policy" means keep using whatever is already in
    // force. Explicitly not "show nothing" - a server that cannot resolve a
    // policy must never blank a screen that was working.
    return;
  }

  if (policy.defaultDwellSeconds > 0) {
    gDefaultDwellSeconds = static_cast<uint16_t>(policy.defaultDwellSeconds);
  }
  if (policy.manualNavHoldSeconds > 0) {
    gManualNavHoldMs = static_cast<uint32_t>(policy.manualNavHoldSeconds) * 1000UL;
  }

  for (uint8_t i = 0; i < gCardCount; ++i) {
    gCards[i].active = false;
  }

  uint8_t matched = 0;
  for (uint8_t e = 0; e < policy.entryCount; ++e) {
    const Cards::PolicyEntry& entry = policy.entries[e];
    const int8_t index = Cards::indexOf(entry.id.c_str());
    if (index < 0) {
      // Ignored, not an error. This is what lets the server add a card type
      // before firmware supports it, and what lets firmware up to six months
      // old keep working against a newer server.
      Log::printf("[cards] policy names unknown card '%s' - ignored", entry.id.c_str());
      continue;
    }

    Cards::CardSpec& card = gCards[index];
    card.active = true;
    matched++;
    if (entry.kind == "list") {
      card.kind = Cards::Kind::List;
    } else if (entry.kind == "interstitial") {
      card.kind = Cards::Kind::Interstitial;
    }
    card.order = static_cast<int16_t>(entry.order);
    card.dwellSeconds = static_cast<uint16_t>(entry.dwellSeconds > 0 ? entry.dwellSeconds : 0);
    card.interleaveEvery =
        static_cast<uint16_t>(entry.interleaveEvery > 0 ? entry.interleaveEvery : 0);
    card.notableDwellSeconds =
        static_cast<uint16_t>(entry.notableDwellSeconds > 0 ? entry.notableDwellSeconds : 0);

    // The picture this card draws, for the cards that draw one. Rewritten on
    // every policy - including back to empty, which is how the server takes a
    // picture away again. An over-long id is dropped rather than truncated:
    // a truncated id is a well-formed id for some *other* asset, and showing
    // the wrong picture is worse than showing none (see Cards.h).
    card.assetId[0] = '\0';
    if (entry.assetId.length() > Cards::kMaxAssetIdLength) {
      Log::printf("[cards] policy assetId for '%s' is too long (%u chars) - ignored",
                  entry.id.c_str(), static_cast<unsigned>(entry.assetId.length()));
    } else if (entry.assetId.length() > 0) {
      strncpy(card.assetId, entry.assetId.c_str(), Cards::kMaxAssetIdLength);
      card.assetId[Cards::kMaxAssetIdLength] = '\0';
    }

    // The words this card draws, for the announcement card - rewritten on
    // every policy exactly like assetId above, including back to empty,
    // which is how an admin takes an announcement down again. Dropped rather
    // than truncated when over-long, for the same reason: a notice chopped
    // off mid-sentence on a household's screen is worse than one that simply
    // does not appear, and the editor already refuses to save anything past
    // this length (CardPolicyEditing.MaxTextLength), so this should only ever
    // fire against a hand-crafted or future-server value.
    card.text[0] = '\0';
    if (entry.text.length() > Cards::kMaxTextLength) {
      Log::printf("[cards] policy text for '%s' is too long (%u chars) - ignored",
                  entry.id.c_str(), static_cast<unsigned>(entry.text.length()));
    } else if (entry.text.length() > 0) {
      strncpy(card.text, entry.text.c_str(), Cards::kMaxTextLength);
      card.text[Cards::kMaxTextLength] = '\0';
    }

    // The QR payload this card encodes, for the QR card - rewritten on every
    // policy exactly like text above, including back to empty, which is how
    // an admin takes a QR code down again. Dropped rather than truncated when
    // over-long, for the same reason assetId is: a QR payload chopped off
    // mid-string is not a shorter version of the same code, it is a
    // different one, and the editor already refuses to save anything past
    // this length (CardPolicyEditing.MaxQrDataLength), so this should only
    // ever fire against a hand-crafted or future-server value.
    card.qrData[0] = '\0';
    if (entry.qrData.length() > Cards::kMaxQrDataLength) {
      Log::printf("[cards] policy qrData for '%s' is too long (%u chars) - ignored",
                  entry.id.c_str(), static_cast<unsigned>(entry.qrData.length()));
    } else if (entry.qrData.length() > 0) {
      strncpy(card.qrData, entry.qrData.c_str(), Cards::kMaxQrDataLength);
      card.qrData[Cards::kMaxQrDataLength] = '\0';
    }
  }

  if (matched == 0) {
    // The mirror image of the unknown-card case above: a newer server whose
    // entire policy names card types this firmware does not have would
    // otherwise switch every card off and leave a blank screen. Falling back
    // to "everything stays on" keeps an old device useful rather than dark.
    Log::line("[cards] policy matched no known card - keeping every card active");
    for (uint8_t i = 0; i < gCardCount; ++i) {
      gCards[i].active = true;
    }
  }

  Log::printf("[cards] policy applied (%u of %u entries known, defaultDwell=%us hold=%lus)",
              matched, policy.entryCount, gDefaultDwellSeconds,
              static_cast<unsigned long>(gManualNavHoldMs / 1000UL));

  resetHistory(gCurrent);
  if (gCurrent.card < 0 || !showable(static_cast<uint8_t>(gCurrent.card))) {
    // Whatever was showing is no longer in the rotation - move on rather than
    // leaving a card the server just switched off on screen.
    show(computeNext());
  } else {
    drawCurrent();
  }
}

void redraw() { drawCurrent(); }

}  // namespace CardManager
