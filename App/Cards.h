#pragma once

#include <Arduino.h>
#include <time.h>

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

/// Optional: this card's own one-line view of how its last data fetch went -
/// "ok, 6 aircraft", "refused: provider disabled for this account", "never
/// fetched". nullptr means "nothing to report", which is the right answer for
/// every card that fetches nothing (clockdate, announcement, qrtext).
///
/// **Why this exists.** Every fetch-driven card already logs its outcome, but
/// never on a cadence anyone watching can rely on, and the two families of
/// card are unhelpful in different ways:
///
///   The check-in-driven cards (Tides, IssFlyover, HomeValue, Graphic) log
///   only on a STATE CHANGE - their gLastLogged* dedup statics. A device
///   parked in one state for twenty minutes says nothing about it at all.
///
///   The fetching cards (Aircraft, Listings, Forecast) have no such statics
///   and log at fetch time instead - which sounds better until you notice
///   fetches are kContentRefreshIntervalMs apart, so "nothing for ten
///   minutes" is the normal, healthy case there too.
///
/// Either way an admin who starts watching mid-flight sees cards cycling in
/// rotation and has no way to tell a working fetch from a refused one without
/// log history from before they connected. Diagnosing two devices live cost
/// real time to exactly this.
///
/// So CardManager re-asserts every active card's status once per check-in
/// (see logProviderStatuses()), unconditionally, whether or not it changed.
/// One consolidated line rather than re-firing each module's own scattered
/// call sites, which keeps the dedup logging intact for what it is good at.
///
/// Returning String rather than const char* because most implementations
/// build the text from live values. It is called at most once per check-in per
/// card, on the stream-only path, so that allocation is not on any hot path.
using StatusFn = String (*)();

/// The longest asset id a policy entry can carry. Matches Assets::kMaxIdLength,
/// which is what actually validates one - Graphic.cpp static_asserts that the
/// two agree, so a divergence is a compile error rather than a silently
/// truncated id that quietly fetches nothing.
static constexpr uint8_t kMaxAssetIdLength = 48;

/// The longest announcement text a policy entry can carry, in characters.
/// Matches DiscoverAroundMe.AdminUI.CardPolicyEditing.MaxTextLength (280) on
/// the server, which is what actually enforces the bound - Announcement.cpp
/// static_asserts that the two agree, the same cross-check Graphic.cpp already
/// does for kMaxAssetIdLength against Assets::kMaxIdLength, so a future change
/// to either number is a compile error here rather than text that quietly
/// arrives truncated to whatever this buffer happened to hold.
static constexpr uint16_t kMaxTextLength = 280;

/// The longest QR payload a policy entry can carry, in characters. Matches
/// DiscoverAroundMe.AdminUI.CardPolicyEditing.MaxQrDataLength (100) on the server, which is
/// what actually enforces the bound - QrText.cpp static_asserts that the two agree, the
/// same cross-check Announcement.cpp already does for kMaxTextLength against
/// CardPolicyEditing.MaxTextLength. See that server-side constant's own remarks for how 100
/// was derived from QR version 6's own byte-mode capacity (134 characters at ECC LOW) - this
/// firmware-side number only sizes the fixed buffer the value is carried in once it
/// arrives, it does not re-derive the bound.
static constexpr uint16_t kMaxQrDataLength = 100;

/// The longest location choice a policy entry can carry, in characters.
/// CardPolicyEntry.Location on the server only ever sends "home", "target",
/// or omits the field entirely (see that property's own remarks), so this
/// only needs to be a few characters longer than "target" itself - sized with
/// headroom rather than exactly 6 for the same "tolerate rather than reject"
/// reasoning wantsTarget() in Forecast.cpp applies to the value once it
/// arrives: an unrecognised value here is treated as Home, not truncated into
/// a different unrecognised value.
static constexpr uint8_t kMaxLocationLength = 16;

/// The longest card id an announcement's target list can carry. Card ids are
/// short, fixed, firmware-defined strings ("issflyover" is the longest today at
/// 10), so this is sized with generous headroom rather than derived - an id
/// that would not fit is one this firmware does not implement anyway, and the
/// target simply never matches.
static constexpr uint8_t kMaxAnnouncementCardIdLength = 32;

/// The longest announcement text this device holds. Shorter than
/// kMaxTextLength on purpose: this is a header strip across the top of a
/// 320x240 panel, not a full-screen card, and Display::showBannerCard() has
/// room for roughly three lines at 26px. Longer text arrives truncated rather
/// than rejected - a banner is a reminder, and most of a reminder beats none.
static constexpr uint16_t kMaxAnnouncementTextLength = 120;

/// The most currently-effective announcements this device tracks at once, and
/// the most cards one announcement may name explicitly. Bounds on fixed arrays
/// rather than a claim about the server, which may send more; the surplus is
/// dropped with a log line rather than growing the heap.
///
/// **These are sized against DRAM, not against what a server might want to
/// send.** Every slot costs its bytes twice - once in CardManager's live store
/// and once in CheckIn.cpp's parse buffer - and it costs them in .bss, which
/// is heap this device does not get back. That matters more here than the
/// numbers suggest: maxAllocHeap settles at 32,756 bytes once WiFi and TLS are
/// up (see the contiguity section in README.md), and App.ino's heap watchdog
/// restarts the device below 28,000, so a few kilobytes of static buffers is a
/// meaningful fraction of the margin between working and rebooting. Three
/// concurrent announcements is a generous ceiling for a household display -
/// they cycle, so a fourth would be waiting behind three others anyway - and
/// four explicit targets covers naming a handful of cards before the wildcard
/// (an empty target list) becomes the sensible way to say "all of them".
static constexpr uint8_t kMaxAnnouncements = 3;
static constexpr uint8_t kMaxAnnouncementTargets = 4;

/// One message to overlay on a card, as it arrives on the check-in response.
///
/// **Effectivity is not on this struct, and that is deliberate.** The server
/// sends only the announcements effective right now for this device and not
/// already dismissed by whoever owns it - see the server's
/// AnnouncementsService.GetEffectiveForDeviceAsync, which filters on both
/// before serialising. So this device needs no date arithmetic, no clock
/// comparison, and no dismissal history beyond the current session: an
/// announcement's presence in the array IS the statement that it should be
/// showing. That keeps the one rule which has to agree between two codebases
/// living in exactly one of them.
struct Announcement {
  /// The announcement's own id, as text. Used only to tell one from another
  /// across check-ins, so a banner this device just dismissed is not
  /// resurrected by the very next response - the server cannot know about the
  /// press until that press reaches it, so it will still be listing the
  /// announcement for one more round.
  char id[37] = "";
  char text[kMaxAnnouncementTextLength + 1] = "";
  /// True when this announcement wants a press to satisfy it. **This, not the
  /// card, is what makes a banner a "Banner Button"**: whether a reminder needs
  /// acknowledging is a property of the reminder, not of whatever card it
  /// happens to be sitting on.
  bool isAction = false;
  /// The action to record when that button is pressed. Rides the existing
  /// PendingActions path exactly like any other card button (see Actions.h),
  /// which is why dismissal needed no new wire route of its own. Empty
  /// whenever isAction is false.
  char actionId[kMaxAnnouncementCardIdLength + 1] = "";
  /// Which cards this announcement may appear on. **Empty means every card
  /// whose policy allows banners**, not none - a wildcard, matching the
  /// server's own TargetCardIds contract.
  char targetCardIds[kMaxAnnouncementTargets][kMaxAnnouncementCardIdLength + 1] = {};
  uint8_t targetCount = 0;
  /// Set when this device's own press satisfied it, so the banner stops
  /// drawing immediately instead of lingering until the next check-in. RAM
  /// only, deliberately not persisted: a press a reboot erases is the honest
  /// limit of a fire-and-forget button on firmware with no automated hardware
  /// tests. The server holds the durable record - it writes the dismissal when
  /// the press arrives and stops sending the announcement after that.
  bool dismissedLocally = false;
};

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
  /// Optional - see StatusFn. nullptr for every card with no fetch of its own.
  StatusFn status = nullptr;

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

  /// Which asset this card draws, for the cards that draw one - empty for
  /// every card that does not, which is most of them. It lives on the
  /// descriptor rather than inside one card's module because it arrives on
  /// the same policy entry as `order` and `dwellSeconds`: **changing which
  /// picture a household sees is a config edit, not a firmware release.**
  ///
  /// A fixed buffer rather than a String, deliberately. The registry these
  /// descriptors live in is constant-initialised so that it exists before any
  /// card module's static initialiser runs (see the top of CardManager.cpp);
  /// a String member would make it dynamically initialised instead, and the
  /// registration order across translation units is undefined.
  char assetId[kMaxAssetIdLength + 1] = "";

  /// The announcement text this card draws, for the one card that draws text
  /// instead of a picture - empty for every other card, which is most of
  /// them. Same fixed-buffer reasoning as `assetId` immediately above: this
  /// struct must stay constant-initialisable, and a String field would make
  /// it dynamically initialised instead, racing every card module's own
  /// static-init registration. See CardManager::applyPolicy() for how this is
  /// populated from a policy's `text` field, and Announcement.cpp for the
  /// card that reads it.
  char text[kMaxTextLength + 1] = "";

  /// The QR payload this card encodes - a URL or short data string - for the one card that
  /// draws a scannable code. Empty for every other card, which is most of them. Same
  /// fixed-buffer reasoning as `assetId`/`text` above: this struct must stay
  /// constant-initialisable, and a String field would make it dynamically initialised
  /// instead, racing every card module's own static-init registration. See
  /// CardManager::applyPolicy() for how this is populated from a policy's `qrData` field,
  /// and QrText.cpp for the card that reads it. Unlike `text` (this same card's optional
  /// caption), an empty value here means the card has nothing to encode at all and reports
  /// zero items - see QrText.h's own remarks on why QrData, not Text, is this card's
  /// required content.
  char qrData[kMaxQrDataLength + 1] = "";

  /// Which of the owner's addresses this card should show - "home" or
  /// "target" - for the one card that draws a single-location result and
  /// needs to be told which. Empty for every other card, which is most of
  /// them: this arrives on the same policy entry as
  /// `order`/`dwellSeconds` for the same reason `assetId`/`text`/`qrData` do
  /// (see CardPolicyEntry.Location on the server), and a card that does not
  /// consult it must ignore it exactly the way an unrelated card ignores a
  /// stray assetId. Same fixed-buffer reasoning as assetId/text/qrData above:
  /// this struct must stay constant-initialisable. Raw and unvalidated -
  /// Forecast.cpp's wantsTarget() is what actually interprets it, treating
  /// anything other than exactly "target" (case-insensitive) as Home, the
  /// same tolerant default GET /api/myweather/forecast itself applies.
  char location[kMaxLocationLength + 1] = "";

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

  // ---- Banner eligibility and this card's own scheduling window. Both are
  // rewritten wholesale on every policy exactly like every field above - there
  // is no field down here that survives a policy any more. There used to be:
  // a `dismissedByButton` flag that applyPolicy() deliberately did not reset,
  // because a per-card banner had no id and "is this the same reminder" could
  // only be guessed at by diffing text. Announcements carry ids, so the
  // dismissal moved to the announcement and the guess is gone - see
  // Cards::Announcement::dismissedLocally.

  /// Whether this card may be overlaid by a banner announcement.
  ///
  /// **Eligibility, not a promise.** True only means this card is a candidate
  /// to carry one of whatever announcements are currently effective and
  /// targeting it. A card with this set and nothing due draws exactly as it
  /// would with it false - its own ordinary full-screen content, never a blank
  /// space or a stuck "waiting for a banner" state.
  ///
  /// This replaced a per-card `Theme` field (FullScreen/Banner/BannerButton)
  /// which was the first draft of this feature. The reason it could not stay
  /// is worth keeping: a theme on the card could express exactly one fixed
  /// message per card, so it had nowhere to put a queue of independently
  /// dismissible announcements, and nothing to say about which of several
  /// currently-effective ones a card should show. Whether a banner is a plain
  /// reminder or one with a button is also not a property of the CARD - it
  /// belongs to the announcement, which knows whether it wants a press (see
  /// Cards::Announcement::isAction). The card only ever says "banners are
  /// allowed here".
  bool allowBanner = false;
  /// Epoch seconds (UTC). 0 means "no bound in this direction" - the same
  /// absent-means-unrestricted convention every other optional policy field
  /// on this struct already follows. Compared against time(nullptr) on every
  /// scheduling decision, not just once when the policy arrives - see
  /// CardManager::showable()'s isEffectiveNow().
  uint32_t effectiveFromUtc = 0;
  uint32_t effectiveToUtc = 0;

};

// 29 registrations exist today: aircraft, calendar, clockdate, issflyover,
// listings, moonphase, sunmoon, tides and homevalue at one apiece (9), plus graphic,
// announcement, qrtext and forecast at five independently-configured
// instances each (20) - the multi-instance generalisation that widened
// Graphic's original three-instance precedent to every card type whose own
// descriptor carries a field an admin can set differently per instance
// (assetId/text/qrData/location). Aircraft, calendar, listings, tides,
// sunmoon, moonphase, clockdate, issflyover and homevalue deliberately did NOT
// get multiple instances - each either has no such field at all (sunmoon/
// moonphase/clockdate compute one fact for the device's own position/time,
// with nothing a second instance could be configured differently from) or
// fetches an unparameterised "mine" endpoint that would return
// byte-identical data to a second instance for the price of a second HTTP
// round trip (aircraft/listings/calendar - and for calendar there is a second
// reason on top: a second instance would mean a second copy of the household's
// private event titles resident in RAM, which Calendar.h's privacy remarks
// rule out on their own) or is pushed unconditionally on every
// check-in with nothing to distinguish a second copy (tides/issflyover/
// homevalue - see HomeValue.h's own remarks for that card specifically). See
// Graphic.h/Announcement.h/QrText.h/Forecast.h's own remarks for the full
// accounting, and Forecast.h specifically for why it - alone among the
// fetch-driven cards - got multi-instance treatment anyway (its Location
// field is a real per-instance axis the other four lack).
//
// registerCard() only logs and drops a card past the cap rather than
// crashing, which is a silent-until-noticed failure on firmware with no
// automated tests. Sized with one spare slot rather than exactly 27 when
// that was the count, precisely so a card like homevalue could be added as
// a registration and nothing else - which is exactly what just happened.
// That spare slot is now spent: the next new card type past this one will
// need this bound raised alongside its own registration.
//
// And that is exactly what happened next. 29 registrations exist as of the
// "calendar" card (see Calendar.cpp), which is the 29th - the server's
// KnownCards.cs had advertised that id for a while and a real device was
// logging "[cards] policy names unknown card 'calendar' - ignored" on every
// check-in until firmware caught up. Raised to 29 rather than to 32-with-slack
// on purpose: each unused slot is a whole CardSpec (several hundred bytes of
// .bss apiece, given the assetId/text/qrData/location buffers on it), and this
// file's own remarks on kMaxAnnouncements explain why .bss is not free on a
// board whose heap watchdog restarts it below 28,000 bytes. Tracking the real
// count exactly means the next new card type needs this line touched again -
// which is the point: it is a one-line edit, and it is far better than paying
// for three empty descriptors forever so that edit can be skipped.
static constexpr uint8_t kMaxCards = 29;

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

// Was a fixed 8 - a leftover from before this project grew past 8 registered
// card types, and never bumped alongside kMaxCards as new ones were added.
// The real bug this caused: a device's own /diag stream logged "cardPolicy
// has more than 8 cards - the rest are ignored" for any account with more
// than 8 cards configured, silently dropping whichever cards the server
// happened to list past position 8 in the JSON array - before this firmware
// ever got the chance to accept or reject them by id. Tied to kMaxCards now
// so the two can never drift apart again: a policy can name at most one entry
// per registered card, so the registry's own cap is the right bound here too.
static constexpr uint8_t kMaxPolicyCards = kMaxCards;

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
  /// Optional on the wire, and empty for the cards that draw no picture. One
  /// longer than kMaxAssetIdLength is dropped rather than truncated when it
  /// reaches the descriptor - a truncated id is a perfectly well-formed id
  /// for some *other* asset, so the card shows nothing instead of showing
  /// the wrong thing. See CardManager::applyPolicy().
  String assetId;
  /// Optional on the wire, and empty for every card that draws no text - which
  /// is every card except the announcement one. Unlike assetId, an over-long
  /// value here has no "well-formed but wrong" failure mode to avoid (there is
  /// no catalog to look up a truncated id in), but it is still dropped rather
  /// than truncated for the same reason CardManager::applyPolicy() gives for
  /// assetId: silence is a more honest failure than a sentence chopped off
  /// mid-word on a household's screen.
  String text;
  /// Optional on the wire, and empty for every card that draws no QR code - which is every
  /// card except the QR one. Same "dropped rather than truncated when over-long" rule as
  /// text immediately above, for the same reason: a QR payload chopped off mid-string is
  /// not a shorter version of the same code, it is a different (and likely useless) one, so
  /// showing nothing is more honest than showing the wrong code.
  String qrData;
  /// Optional on the wire, and empty for every card that draws no single
  /// location's data - which is every card except the forecast one. "home",
  /// "target", or absent/anything else, meaning Home - see
  /// Cards::CardSpec::location and CardPolicyEntry.Location on the server for
  /// the full tolerance rule. Dropped rather than truncated when over-long,
  /// the same rule text/qrData already follow above, though in practice this
  /// value is always short enough that the bound never fires against a real
  /// server.
  String location;
  /// Optional on the wire, false when absent - which is every policy saved
  /// before this feature existed. Mirrors CardPolicyEntry.AllowBanner on the
  /// server; see Cards::CardSpec::allowBanner for what it does and does not
  /// promise, and for why it replaced a per-card `theme` string.
  bool allowBanner = false;
  /// Already converted from the wire's ISO-8601 instant to epoch seconds by
  /// CheckIn.cpp's own parseIso8601Utc() at parse time - unlike every other
  /// field on this struct, there is no reason to carry the raw string only to
  /// re-parse it in CardManager::applyPolicy(), since nothing else needs the
  /// unparsed form. 0 for an absent field, the same sentinel
  /// Cards::CardSpec::effectiveFromUtc uses for "no bound".
  time_t effectiveFromUtc = 0;
  time_t effectiveToUtc = 0;
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

/// Replaces the set of announcements currently in force, wholesale, from a
/// check-in response.
///
/// Wholesale rather than merged, for the same reason applyPolicy() rewrites
/// every card field: the response is a complete statement of what should be
/// showing, so anything absent from it has stopped being effective, been
/// dismissed elsewhere, or been deleted - and all three mean "stop drawing
/// it". A merge would leave a withdrawn announcement on screen forever.
///
/// One exception is carried across: an announcement still listed by the server
/// that this device already dismissed locally stays dismissed. The server
/// cannot know about a press until that press reaches it on the next check-in,
/// so it will legitimately still be listing the announcement, and matching on
/// id is what stops the banner flickering back for one cycle.
void setAnnouncements(const Announcement* announcements, uint8_t count);

/// The announcement this card should currently be overlaid with, or nullptr
/// for none - which is the ordinary case and means the card draws its own
/// content. See CardManager::drawCurrent() for the caller.
///
/// **A pure read.** Calling it twice for the same card returns the same
/// announcement, which matters because drawCurrent() runs on more than just a
/// rotation step - an in-place refresh, the redraw after a button press, and
/// the end of applyPolicy() all reach it. An earlier version advanced the
/// cycling cursor here, which meant any of those could swap the banner out
/// from under someone mid-sentence.
const Announcement* announcementFor(const CardSpec& card);

/// Moves the cycling cursor on by one, so the next card to carry a banner
/// starts looking from the following announcement.
///
/// Called from the rotation step only - see CardManager::advance(). Cycling
/// when several announcements target the same card was the explicit product
/// decision for overlapping ones ("cycle through them like a list card"), and
/// tying it to the rotation rather than to the draw is what makes each one
/// readable for a full dwell instead of for however long until the next
/// incidental redraw.
///
/// One device-wide cursor rather than one per card: a household reads one
/// screen at a time, and a per-card cursor would mean a card revisited after
/// twenty others resumes mid-list rather than showing what is most current.
void advanceAnnouncementCursor();

/// Marks the announcement a press just satisfied as dismissed on this device,
/// so it stops drawing at once instead of lingering until the server has
/// heard. Returns false when the id names nothing currently held.
bool dismissAnnouncement(const char* announcementId);

/// Re-asserts every active card's current fetch status to the debug log, in
/// one consolidated line, whether or not anything changed since last time.
///
/// Called once per successful check-in from App.ino. See StatusFn above for
/// why this exists at all: the per-module logging is deliberately
/// change-only, which leaves a live stream silent about a device that has been
/// sitting in the same refused state for twenty minutes - the case an admin is
/// most likely to be watching for.
///
/// Costs nothing when nobody is streaming: it is a Log::verbose line, which
/// returns before formatting anything if streaming is off, and it checks that
/// first before even walking the registry.
void logProviderStatuses();

}  // namespace Cards
