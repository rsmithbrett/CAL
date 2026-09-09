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

/// Which of three display styles a card draws once it takes its turn in the
/// rotation - orthogonal to `Kind` above, which only decides *when* that turn
/// comes. Mirrors the server's `CardPolicyEntry.Theme` (see that property's
/// own remarks for the full reasoning); this is the firmware half.
///
/// `Banner`/`BannerButton` both draw `CardSpec::text` - reused as-is, there is
/// no second content field - as a header strip across the top of the panel
/// (`Display::showBannerCard()`) instead of this card's own `draw()`. With no
/// text to show, `CardManager::drawCurrent()` falls back to `draw()` anyway,
/// which is exactly Full Screen with no code path of its own needed for it.
enum class Theme : uint8_t {
  /// Every card's own ordinary full-screen layout - the only theme that
  /// existed before this feature, and the default for every policy entry
  /// that omits Theme entirely (every policy saved before this feature
  /// existed).
  FullScreen,
  /// A header strip reminding a household of something - an emergency
  /// weather alert, an upcoming calendar event - drawn across the top of the
  /// panel instead of this card's own full-screen content.
  Banner,
  /// The same header strip, plus: pressing this card's own button (drawn and
  /// wired up exactly like any other card's button - see Actions.h) clears
  /// the reminder locally on this device, on top of whatever effect that
  /// button is otherwise bound to fire server-side. See
  /// CardManager::handleTap()'s `Touch::Hit::ActionButton` case for exactly
  /// what "clears" means and how a later policy change undoes it.
  BannerButton,
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

  // ---- Display theme and effectivity dates - see Theme's own remarks above
  // and CardPolicyEntry.EffectiveFromUtc/EffectiveToUtc on the server. Both
  // are rewritten wholesale on every policy exactly like every field above,
  // except `dismissedByButton`, which is deliberately NOT reset by an
  // unchanged policy - see CardManager::applyPolicy()'s own remarks on why.

  /// FullScreen unless a policy names this card with a recognised Theme
  /// value - see CardManager::applyPolicy().
  Theme theme = Theme::FullScreen;
  /// Epoch seconds (UTC). 0 means "no bound in this direction" - the same
  /// absent-means-unrestricted convention every other optional policy field
  /// on this struct already follows. Compared against time(nullptr) on every
  /// scheduling decision, not just once when the policy arrives - see
  /// CardManager::showable()'s isEffectiveNow().
  uint32_t effectiveFromUtc = 0;
  uint32_t effectiveToUtc = 0;

  /// Set by CardManager::handleTap() when this card's Theme is BannerButton
  /// and its own button is pressed - see that function's own remarks. RAM
  /// only, deliberately not persisted to NVS: a press that a reboot erases is
  /// the honest limit of a fire-and-forget, no-confirmation button press on
  /// firmware with no automated hardware tests, not a guarantee this file
  /// claims to make. Cleared again the moment applyPolicy() sees this same
  /// entry's `text` or `effectiveFromUtc` change - a new reminder, as far as
  /// this device can tell, must not stay suppressed by an old one's press.
  bool dismissedByButton = false;
};

// 28 registrations exist today: aircraft, clockdate, issflyover, listings,
// moonphase, sunmoon, tides and homevalue at one apiece (8), plus graphic,
// announcement, qrtext and forecast at five independently-configured
// instances each (20) - the multi-instance generalisation that widened
// Graphic's original three-instance precedent to every card type whose own
// descriptor carries a field an admin can set differently per instance
// (assetId/text/qrData/location). Aircraft, listings, tides, sunmoon,
// moonphase, clockdate, issflyover and homevalue deliberately did NOT get
// multiple instances - each either has no such field at all (sunmoon/
// moonphase/clockdate compute one fact for the device's own position/time,
// with nothing a second instance could be configured differently from) or
// fetches an unparameterised "mine" endpoint that would return
// byte-identical data to a second instance for the price of a second HTTP
// round trip (aircraft/listings) or is pushed unconditionally on every
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
static constexpr uint8_t kMaxCards = 28;

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
  /// Optional on the wire: "banner", "bannerbutton", or absent/anything else
  /// meaning Full Screen - see Cards::Theme and CardPolicyEntry.Theme on the
  /// server for the full tolerance rule. Parsed into a CardSpec::theme value
  /// by CardManager::applyPolicy(), not here - this struct only carries the
  /// raw string the same way `kind` does.
  String theme;
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

}  // namespace Cards
