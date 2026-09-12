#include "CardManager.h"

#include "Actions.h"
#include "Config.h"
#include "Display.h"
#include "HeapRatchet.h"
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

namespace {

Cards::Announcement gAnnouncements[Cards::kMaxAnnouncements];
uint8_t gAnnouncementCount = 0;

/// Rotates which of several matching announcements a card shows - see
/// announcementFor(). One counter for the whole device rather than one per
/// card: a household reads one screen at a time, and a per-card cursor would
/// mean a card revisited after twenty others resumes mid-list instead of
/// showing what is most current.
uint8_t gAnnouncementCursor = 0;

/// Whether this announcement is allowed on this card. An empty target list is
/// a wildcard - see Announcement::targetCardIds.
bool targetsCard(const Cards::Announcement& announcement, const char* cardId) {
  if (announcement.targetCount == 0) {
    return true;
  }
  for (uint8_t i = 0; i < announcement.targetCount; ++i) {
    if (strcmp(announcement.targetCardIds[i], cardId) == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

void setAnnouncements(const Cards::Announcement* announcements, uint8_t count) {
  // Clamped defensively rather than reported: the only caller is
  // CheckIn.cpp's parser, which already bounds its own staging buffer and logs
  // when the server offered more than it kept, so a count over the ceiling
  // here would mean a second caller appeared without reading either.
  const uint8_t incoming = count > Cards::kMaxAnnouncements ? Cards::kMaxAnnouncements : count;

  // Which ids this device had already dismissed, copied out BY VALUE before
  // the store is touched.
  //
  // By value, not by pointer, and not by copying whole Announcements. Pointers
  // into gAnnouncements would dangle the moment the loop below overwrote the
  // slot they pointed at, silently comparing against whatever had just been
  // written there instead of against the old id. Copying whole Announcements
  // instead would put another kMaxAnnouncements-sized struct on the stack
  // while CheckIn::Result is still live on it - the exact cost that struct's
  // own remarks explain it moved to a file-static buffer to avoid. Three short
  // id strings is a hundred-odd bytes and neither problem.
  char dismissedIds[Cards::kMaxAnnouncements][sizeof(Cards::Announcement::id)] = {};
  uint8_t dismissedCount = 0;
  for (uint8_t i = 0; i < gAnnouncementCount; ++i) {
    if (gAnnouncements[i].dismissedLocally) {
      strncpy(dismissedIds[dismissedCount], gAnnouncements[i].id,
              sizeof(dismissedIds[dismissedCount]) - 1);
      dismissedIds[dismissedCount][sizeof(dismissedIds[dismissedCount]) - 1] = '\0';
      dismissedCount++;
    }
  }

  for (uint8_t i = 0; i < incoming; ++i) {
    gAnnouncements[i] = announcements[i];

    // Carry a local dismissal across, matched on id - see setAnnouncements'
    // own remarks in Cards.h. Without this, the check-in immediately after a
    // press puts the banner straight back on screen, because the server has
    // not heard about the press yet.
    gAnnouncements[i].dismissedLocally = false;
    for (uint8_t j = 0; j < dismissedCount; ++j) {
      if (strcmp(dismissedIds[j], gAnnouncements[i].id) == 0) {
        gAnnouncements[i].dismissedLocally = true;
        break;
      }
    }
  }

  for (uint8_t i = incoming; i < Cards::kMaxAnnouncements; ++i) {
    gAnnouncements[i] = Cards::Announcement{};
  }

  const bool changed = gAnnouncementCount != incoming;
  gAnnouncementCount = incoming;
  if (gAnnouncementCursor >= gAnnouncementCount) {
    gAnnouncementCursor = 0;
  }

  if (changed || incoming > 0) {
    Log::printf("[banner] %u announcement(s) in force", static_cast<unsigned>(gAnnouncementCount));
  }
}

const Cards::Announcement* announcementFor(const Cards::CardSpec& card) {
  if (!card.allowBanner || gAnnouncementCount == 0) {
    return nullptr;
  }

  // Walks the whole array starting at the cursor, so cycling is fair across
  // however many match this particular card without needing a per-card index.
  for (uint8_t offset = 0; offset < gAnnouncementCount; ++offset) {
    const uint8_t index = (gAnnouncementCursor + offset) % gAnnouncementCount;
    const Cards::Announcement& candidate = gAnnouncements[index];
    if (candidate.dismissedLocally || strlen(candidate.text) == 0) {
      continue;
    }
    if (!targetsCard(candidate, card.id)) {
      continue;
    }
    return &candidate;
  }

  return nullptr;
}

void advanceAnnouncementCursor() {
  if (gAnnouncementCount == 0) {
    return;
  }
  gAnnouncementCursor = static_cast<uint8_t>((gAnnouncementCursor + 1) % gAnnouncementCount);
}

bool dismissAnnouncement(const char* announcementId) {
  if (announcementId == nullptr || strlen(announcementId) == 0) {
    return false;
  }
  for (uint8_t i = 0; i < gAnnouncementCount; ++i) {
    if (strcmp(gAnnouncements[i].id, announcementId) == 0) {
      gAnnouncements[i].dismissedLocally = true;
      return true;
    }
  }
  return false;
}

void logProviderStatuses() {
  // Checked before anything else, including walking the registry: with
  // streaming off this must cost nothing at all, and every status()
  // implementation builds a String.
  if (!Log::streamingEnabled()) {
    return;
  }

  String summary;
  uint8_t reported = 0;

  for (uint8_t i = 0; i < gCardCount; ++i) {
    const CardSpec& card = gCards[i];
    // Inactive cards are skipped deliberately - a card the policy turned off
    // has no current provider state worth asserting, and listing a dozen
    // switched-off cards every check-in would bury the few that matter.
    if (!card.active || card.status == nullptr) {
      continue;
    }

    if (reported > 0) {
      summary += ", ";
    }
    summary += card.id;
    summary += '=';
    summary += card.status();
    ++reported;
  }

  if (reported == 0) {
    // Said out loud rather than skipped: "no fetch-driven cards are active" is
    // itself a diagnosis, and a silent check-in would look identical to this
    // whole feature being broken.
    Log::verbose("[providers] no active fetch-driven cards to report");
    return;
  }

  Log::verbose("[providers] %s", summary.c_str());
}

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

/// Whether a real, present policy response has ever been applied this boot.
/// Gates poll() below - see its own remarks. False from boot until the
/// first successful check-in that actually carries a cardPolicy; never
/// reset afterward, since every following boot gets its own fresh instance
/// of this same wait.
bool gPolicyEverApplied = false;

/// When begin() was called, so poll()'s own wait for a real policy has a
/// bound - see kMaxWaitForPolicyMs below.
uint32_t gBeginAtMs = 0;

/// The mirror image of kFirstCheckInMaxJitterMs and the heap-health
/// watchdog's own 3-minute grace period in App.ino: this is how long poll()
/// will hold the boot "Loading" screen waiting for a real policy before
/// giving up and showing the wide, unfiltered default-active set anyway.
/// Matched to the watchdog's own grace period on purpose - if a policy has
/// not arrived by then, something is wrong with connectivity, not merely
/// slow, and continuing to show nothing is worse than showing everything.
constexpr uint32_t kMaxWaitForPolicyMs = 3UL * 60UL * 1000UL;

/// The last-applied policy's match result - see CardManager.h's
/// lastPolicyKnownCount()/lastPolicyTotalCount()/lastPolicyUnknownIds() for
/// why this exists: App.ino's check-in path reports these back to the
/// server, which is the only way "the policy silently dropped an entry"
/// becomes something a server-side audit can see instead of only ever
/// existing in the remote debug stream for however long someone happens to
/// be watching it.
uint8_t gLastPolicyKnownCount = 0;
uint8_t gLastPolicyTotalCount = 0;
String gLastPolicyUnknownIds;
constexpr uint8_t kMaxReportedUnknownIds = 4;

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

/// The announcement currently drawn as a banner, or nullptr when the card on
/// screen is drawing its own content. Set on every draw by drawCurrent().
///
/// Points into the announcement store rather than copying, which is safe for
/// exactly one reason worth writing down: the store is only ever rewritten by
/// setAnnouncements() on a check-in, and a check-in cannot interleave with a
/// draw on this single-threaded firmware. It is re-read on the next draw
/// regardless, so a stale pointer can never survive one.
///
/// Needed because a button press has to know WHICH announcement it satisfied,
/// and the press arrives from the touch handler long after the draw decided.
const Cards::Announcement* gBannerOnScreen = nullptr;

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
/// Whether `card`'s effectivity window (if any) includes right now - the
/// firmware half of CardPolicyEntry.EffectiveFromUtc/EffectiveToUtc on the
/// server. Both 0 (the ordinary case: no policy has ever set either field)
/// means always effective, the ordinary "absent means no restriction"
/// tolerance every other optional policy field on CardSpec already follows.
/// Checked on every scheduling decision via showable() below, not just once
/// when the policy arrives, so a window opening or closing while a policy is
/// already in force takes effect without waiting for a fresh check-in.
bool isEffectiveNow(const Cards::CardSpec& card) {
  if (card.effectiveFromUtc == 0 && card.effectiveToUtc == 0) {
    return true;
  }
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  if (card.effectiveFromUtc != 0 && now < card.effectiveFromUtc) {
    return false;
  }
  // Half-open [from, to): `now == effectiveToUtc` is already past the window,
  // matching CardPolicyEditing.Validate's own "to <= from is never effective
  // at all" rule on the server for the degenerate case where the two are
  // equal.
  if (card.effectiveToUtc != 0 && now >= card.effectiveToUtc) {
    return false;
  }
  return true;
}

bool showable(uint8_t index) {
  const Cards::CardSpec& card = gCards[index];
  return card.active && card.itemCount != nullptr && card.draw != nullptr &&
         isEffectiveNow(card) && card.itemCount() > 0;
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
  // gButtonCount was resolved by drawCurrent() BEFORE the card drew, so the card
  // could size itself against the room these leave. Deliberately not recomputed
  // here: two Actions::forCard() calls around a draw could disagree if a policy
  // arrived in between, and the card would then have budgeted for one number of
  // buttons while a different number got painted over it.
  (void)card;

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
  // The draw phase, opened at the very top so it covers every exit including
  // the no-content one below - see HeapRatchet::Scope on why an enter/leave
  // pair was rejected for exactly this shape of function.
  //
  // This is the one choke point every card's draw passes through, the same
  // property the "[cards] showing" line further down relies on, which is what
  // makes one Scope here cover every card module including ones added after
  // this line was written. What it bills to Draw: LovyanGFX's decoders and
  // sprites, drawRgb565FromSd()'s band buffer, every per-card draw String, and
  // drawChrome()'s dozen small String allocate/free pairs below - it copies up
  // to three Actions::Definitions (three Strings each) and then builds three
  // more Strings for the labels, on EVERY draw, which is precisely the kind of
  // small repeated churn that carves a heap into unusable holes without leaking
  // a byte.
  //
  // The subject is resolved before the guard clause rather than after, so a
  // draw that bails still names something. "none" is a real state here - a
  // device waiting for its first policy - not an error.
  const char* const subject =
      (gCurrent.card >= 0 && gCurrent.card < static_cast<int8_t>(gCardCount))
          ? gCards[gCurrent.card].id
          : "none";
  const HeapRatchet::Scope scope(HeapRatchet::Phase::Draw, subject);

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
  // The one choke point every card's draw passes through, regardless of kind
  // or which module it lives in - a single line here, rather than one add per
  // card file, is what makes "is card X actually reaching the screen"
  // answerable from the remote debug stream for every card, including ones
  // added after this line was written. See the standing verbose-logging
  // mandate: the stream is the only diagnostic channel a deployed device has.
  Log::printf("[cards] showing '%s' (item %u/%u)", card.id,
              static_cast<unsigned>(gCurrent.item) + 1, static_cast<unsigned>(total));

  // A banner-eligible card carrying an effective announcement draws that
  // announcement in a header strip instead of its own full-screen content.
  // Everything else - no eligibility, nothing due, everything due already
  // dismissed - falls through to the card's ordinary draw(), which is what
  // makes allowBanner eligibility rather than a promise: a card with it set
  // and nothing to show looks exactly like a card without it.
  // Resolve this card's buttons BEFORE it draws, and tell Display how much room
  // that leaves it. drawChrome() below paints them on top of a finished card, so
  // a card laying itself out against the full panel silently loses the 60px band
  // they cover - which is how the home value card's compliance line came to be
  // drawn underneath a button. Same call on both branches: an announcement
  // banner gets a button row too when its action is bound.
  gButtonCount = Actions::forCard(card.id, gButtons, Actions::kMaxButtonsPerCard);
  Display::setContentBudget(gButtonCount > 0);
  if (gButtonCount > 0) {
    Log::verbose("[cards] '%s' draws with %u button(s), so content stops at y=%d", card.id,
                 static_cast<unsigned>(gButtonCount), Display::contentBottom());
  }

  const Cards::Announcement* banner = Cards::announcementFor(card);
  if (banner != nullptr) {
    Log::printf("[banner] '%s' showing announcement %s as a %s", card.id, banner->id,
                banner->isAction ? "banner button" : "banner");
    gBannerOnScreen = banner;
    Display::showBannerCard(String(banner->text));
  } else {
    gBannerOnScreen = nullptr;
    card.draw(gCurrent.item);
  }
  drawChrome(card);
}

void show(const Position& position) {
  gCurrent = position;
  gLastSwitchMs = millis();
  drawCurrent();
}

void advance() {
  // One rotation step, one step through the announcement queue - so a banner
  // gets a full dwell to be read rather than however long until the next
  // incidental redraw. See Cards::advanceAnnouncementCursor().
  Cards::advanceAnnouncementCursor();

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

      // Ask the card what it is showing, right now, before anything can rotate.
      // This is the only moment the answer exists: the check-in carrying this
      // press may be minutes away, the card will have moved on, and the server
      // is never told which item was up. A card with no describe() has nothing
      // worth naming (a clock, a splash) and sends empty, which is a real value
      // rather than a failure - see Cards::DescribeFn.
      String onScreen;
      if (gCurrent.card >= 0 && gCards[gCurrent.card].describe != nullptr) {
        onScreen = gCards[gCurrent.card].describe(gCurrent.item);
      }
      Actions::recordPress(pressed, onScreen);

      // A Banner Button's whole reason for existing: pressing it satisfies the
      // announcement, on top of - not instead of - whatever effect the press
      // above just queued (including none at all, for a button whose
      // server-side binding is DeviceActionEffects.Ignore and exists purely to
      // clear). The durable record is the server's: it maps this press back to
      // the announcement and writes the dismissal against whichever entity
      // owns this device, so it stops sending it. This local flag only covers
      // the gap until that press actually arrives on the next check-in -
      // without it the banner would sit there looking unpressed.
      //
      // Dismissed against the announcement on screen rather than the card,
      // because the announcement is what was satisfied. The same reminder can
      // be showing on several banner-eligible cards, and pressing it once
      // means it is done everywhere - which is what keying on the announcement
      // gets and what keying on the card would not.
      bool dismissed = false;
      if (gBannerOnScreen != nullptr && gBannerOnScreen->isAction) {
        dismissed = Cards::dismissAnnouncement(gBannerOnScreen->id);
        if (dismissed) {
          Log::printf("[banner] announcement %s dismissed locally (button press on '%s')",
                      gBannerOnScreen->id, pressed.cardId.c_str());
        }
        gBannerOnScreen = nullptr;
      }

      // Acknowledges the *press*, not the delivery. The contract is
      // deliberate about there being no "sent" state and no round trip - the
      // press rides the next ordinary check-in and the user waits for
      // nothing - but a button that does not visibly react to a finger reads
      // as a dead button, which is its own failure. A big centre-screen
      // checkmark, not the smaller in-place flashActionButton() this used to
      // call - the user's explicit ask was a confirmation that reads clearly
      // from across the room, not just at the button itself - and it claims
      // nothing about what the server did with the press, same as that
      // smaller flash never did.
      Display::showButtonPressConfirmation();
      if (dismissed) {
        // The card is still perfectly showable - only the announcement that
        // was overlaying it has gone - but redrawing that card the instant its
        // banner was dismissed would put the reminder's own card back up
        // wearing its ordinary content, which reads as "the press did
        // something confusing" rather than "the reminder is dealt with". Move
        // on to whatever is next instead, the same as a manual forward tap.
        //
        // This used to be true in the stronger sense: dismissal set a per-card
        // flag that showable() tested, so the card genuinely dropped out of
        // the rotation. It no longer does, and that is the point - dismissing
        // a reminder should never cost a household a card.
        resetHistory(gCurrent);
        show(computeNext());
      } else {
        // drawCurrent() puts the actual card (and its buttons) back, since
        // the checkmark was drawn over the whole panel, not just the one
        // button rect.
        drawCurrent();
      }
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

  // Opened AFTER the nothing-to-fetch return, so a scan over cards with no
  // fetch function does not open and close a phase for each of them - that
  // would be two heap walks per card per sweep to measure nothing happening.
  //
  // What this bills to Fetch, in one Scope covering every provider rather than
  // a Scope per provider file: the TLS request, ArduinoJson's variant pools
  // (1,024 bytes apiece on this 32-bit target, and the observed steps are
  // multiples of 2,048 - see the ranked hypotheses in HeapRatchet.h), the HTTP
  // body Strings on the refusal paths, and the long-lived Result Strings each
  // provider builds. One Scope because the providers are the same code written
  // several times - Forecast, Aircraft and Listings are structurally identical
  // down to the filter idiom - so instrumenting them individually would be an
  // edit per provider to learn one thing. The subject names which card it was,
  // which is what narrows it afterwards.
  //
  // card.fetch() alone is inside the phase; everything below it is ordinary
  // bookkeeping, and the drawCurrent()/show() calls at the end open their own
  // Draw scope so a refresh of the visible card does not bill its redraw here.
  {
    const HeapRatchet::Scope scope(HeapRatchet::Phase::Fetch, card.id);
    card.fetch();
  }

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

void pollTouch() {
  Touch::Tap tap;
  if (Touch::poll(tap)) {
    handleTap(tap);
  }
}

void begin() {
  Actions::begin();
  gBeginAtMs = millis();

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
  // Hold the boot splash/"Loading" screen (see App.ino's setup()) rather
  // than starting the rotation on every registered card's own true-by-
  // default active state - see CardSpec::active's default and
  // gPolicyEverApplied's own remarks. Found live: a household whose real
  // policy names a handful of cards (say, one forecast instance out of the
  // five registered) would otherwise cycle through every unconfigured
  // instance too for however long the first check-in takes, which reads as
  // "showing the wrong cards" even though it always self-corrected once
  // that check-in landed. Bounded by kMaxWaitForPolicyMs so a device that
  // genuinely cannot reach the server does not sit on "Loading" forever.
  if (!gPolicyEverApplied && (millis() - gBeginAtMs) < kMaxWaitForPolicyMs) {
    return;
  }

  pollTouch();

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

  // A present policy - even one that names nothing, or nothing this build
  // recognises - is still the real, resolved answer poll() is holding the
  // boot screen for. See gPolicyEverApplied's own remarks.
  gPolicyEverApplied = true;

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
  gLastPolicyUnknownIds = "";
  uint8_t unknownReported = 0;
  for (uint8_t e = 0; e < policy.entryCount; ++e) {
    const Cards::PolicyEntry& entry = policy.entries[e];
    const int8_t index = Cards::indexOf(entry.id.c_str());
    if (index < 0) {
      // Ignored, not an error. This is what lets the server add a card type
      // before firmware supports it, and what lets firmware up to six months
      // old keep working against a newer server.
      Log::printf("[cards] policy names unknown card '%s' - ignored", entry.id.c_str());
      if (unknownReported < kMaxReportedUnknownIds) {
        if (gLastPolicyUnknownIds.length() > 0) {
          gLastPolicyUnknownIds += ",";
        }
        gLastPolicyUnknownIds += entry.id;
        unknownReported++;
      }
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

    // Which of the owner's addresses to show, for the forecast card -
    // rewritten on every policy exactly like assetId/text/qrData above,
    // including back to empty (which reads as Home - see
    // Cards::CardSpec::location). Dropped rather than truncated when
    // over-long, the same rule those three already follow, though in
    // practice "home"/"target" never come close to this bound.
    card.location[0] = '\0';
    if (entry.location.length() > Cards::kMaxLocationLength) {
      Log::printf("[cards] policy location for '%s' is too long (%u chars) - ignored",
                  entry.id.c_str(), static_cast<unsigned>(entry.location.length()));
    } else if (entry.location.length() > 0) {
      strncpy(card.location, entry.location.c_str(), Cards::kMaxLocationLength);
      card.location[Cards::kMaxLocationLength] = '\0';
    }

    // Whether this card may carry a banner - rewritten on every policy
    // exactly like every field above, including back to false, which is how an
    // admin switches banners off for a card. Absent means false, which is
    // every policy saved before this feature existed.
    //
    // No dismissal bookkeeping happens here any more. It used to: a per-card
    // Banner Button flag had to be un-dismissed whenever the server sent
    // different text for the same entry, because "this reminder" and "this
    // card" were the same thing and there was no way to tell a new reminder
    // from the old one still being shown. Announcements have their own ids, so
    // that whole guess is gone - a dismissal is keyed to the announcement it
    // satisfied, and a genuinely new announcement is simply a different id.
    card.allowBanner = entry.allowBanner;

    // The effectivity window - rewritten on every policy exactly like every
    // field above, including back to 0 (no bound), which is how an admin
    // removes a window they set previously. Already epoch seconds by the
    // time it reaches here - see Cards::PolicyEntry::effectiveFromUtc's own
    // remarks on why the ISO-8601 parsing happens once, in CheckIn.cpp, and
    // not here.
    //
    // Note this is the CARD's own scheduling window - whether the card appears
    // in the rotation at all - and has nothing to do with an announcement's
    // effectivity, which the server evaluates before sending (see
    // Cards::Announcement).
    card.effectiveFromUtc = static_cast<uint32_t>(entry.effectiveFromUtc);
    card.effectiveToUtc = static_cast<uint32_t>(entry.effectiveToUtc);
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
  gLastPolicyKnownCount = matched;
  gLastPolicyTotalCount = policy.entryCount;

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

uint8_t lastPolicyKnownCount() { return gLastPolicyKnownCount; }

uint8_t lastPolicyTotalCount() { return gLastPolicyTotalCount; }

String lastPolicyUnknownIds() { return gLastPolicyUnknownIds; }

}  // namespace CardManager
