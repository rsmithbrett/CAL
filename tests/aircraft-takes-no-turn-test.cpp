// The aircraft card's place in the rotation: it takes a turn while it holds a
// plane and at no other time, and no screen is drawn to say there is no plane.
//
// App/Aircraft.cpp and App/CardManager.cpp are compiled as they ship and linked
// together, so what runs here is the card's own fetch, item count and draw under
// the scheduler's own poll(), history and policy code. Everything those two call
// outside themselves is defined at the bottom of this file, and each definition
// includes the shipped header, so a signature that drifts stops the compile.
//
// What the test controls:
//
//   - The clock. millis() answers a counter that pass() moves one second at a
//     time, calling CardManager::poll() after each step, so a dwell is counted
//     in whole seconds and nothing depends on how fast the host is.
//   - The service. Http::ready(), Http::beginRequest() and the client's status
//     and body are scripted, so fetchMine() parses a body the case chose.
//   - The panel. Each Display function a card or the scheduler calls appends one
//     line to a list naming the screen it would have drawn. "What is on screen"
//     is the last line of that list.
//   - Touch. A tap is queued and handed to the scheduler on its next poll.
//
// Two cards with no fetch function, "before" (order 1) and "after" (order 3),
// register through the real Cards::registerCard() and sit either side of the
// aircraft card (order 2), so "the rotation carried on" is something a case can
// read. Their item counts are variables the test sets.
//
// A refresh is forced by clearing the card's everFetched flag and polling once,
// which is what the scheduler's ten-minute interval coming due looks like to
// refreshOneDueCard(). Waiting the ten minutes out would move the rotation as
// well, and the cases need to say which card is up when the answer lands.
//
// What each case holds:
//
//   - Before its first fetch, and after a first fetch that got no answer, the
//     card reports no items and is never drawn.
//   - A clear sky leaves the two neighbours alternating. No aircraft screen, no
//     impression for the card, no placeholder screen, and a log line saying the
//     card is taking no turn.
//   - A plane puts the card in the rotation for its ordinary dwell, and a plane
//     inside the notable distance for the longer one.
//   - A plane leaving while its card is up puts the next card up at once with a
//     full dwell of its own, and reverse taps afterwards never reach the card.
//   - A fetch that got no answer keeps the plane. Every answered fetch without a
//     plane drops it, one row per way that can happen.
//   - With every card empty the scheduler's own placeholder is what shows, the
//     aircraft card goes up the moment a fetch returns a plane, and a reverse
//     tap then stays on it.
//   - Alone in a policy, the card is the only thing drawn while it holds a
//     plane, the placeholder shows when the plane leaves, and the card returns
//     with the next plane.
//   - Forward and reverse taps pass over the card once it has emptied.
//   - A plane leaving during a manual hold on its card moves on at once, and the
//     card that replaces it keeps an ordinary dwell rather than the hold. It is
//     recorded as a showing the rotation opened and is never marked as sought.
//   - With two announcements in force, the card that replaces the plane's
//     carries the announcement it would have carried had the plane's dwell run
//     out, so leaving a card is one step through the announcement queue.
//   - Asked to draw with no plane, the card draws nothing and logs that.
//
// What this cannot check: anything about pixels, since Display is a list of
// names here; heap, since tests/aircraft-stubs/ArduinoJson.h allocates from the
// host and applies no filter; TLS; either board's build, which only the arduino
// compile reaches; and the time a real fetch takes, since one returns at once
// here and the clock does not move during it.

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "Actions.h"
#include "Aircraft.h"
#include "Assets.h"
#include "CardManager.h"
#include "CardPolicyStore.h"
#include "Cards.h"
#include "ConfigRequest.h"
#include "Display.h"
#include "HeapRatchet.h"
#include "Http.h"
#include "Identity.h"
#include "Impressions.h"
#include "Log.h"
#include "Maintenance.h"
#include "Motion.h"
#include "Touch.h"

namespace {

int gFailures = 0;
int gCases = 0;
int gChecks = 0;
const char* gCase = "";

void beginCase(const char* name) {
  gCase = name;
  ++gCases;
}

void check(bool condition, const char* what) {
  ++gChecks;
  if (!condition) {
    std::printf("Failed: %s [%s]\n", what, gCase);
    ++gFailures;
  }
}

/// check() for a row of a table, so the line names which row it was.
void checkFor(const char* row, bool condition, const char* what) {
  ++gChecks;
  if (!condition) {
    std::printf("Failed: %s - %s [%s]\n", what, row, gCase);
    ++gFailures;
  }
}

// --- the clock --------------------------------------------------------------

/// Starts above zero so a subtraction from it is never the whole answer, and
/// stays far below 2^31 ms for the length of the run.
unsigned long gNowMs = 60000;

// --- the panel --------------------------------------------------------------

/// One line per screen drawn, oldest first. Function-local so it exists before
/// any static initializer that could reach it.
std::vector<std::string>& screens() {
  static std::vector<std::string> held;
  return held;
}

std::vector<std::string>& logLines() {
  static std::vector<std::string> held;
  return held;
}

/// The card id of every showing the scheduler opened, oldest first.
std::vector<std::string>& impressions() {
  static std::vector<std::string> held;
  return held;
}

/// How each of those showings arrived, index for index with impressions().
std::vector<Impressions::Arrival>& arrivals() {
  static std::vector<Impressions::Arrival> held;
  return held;
}

/// How many times the scheduler marked the card on screen as sought.
int gSought = 0;

std::deque<Touch::Hit>& pendingTaps() {
  static std::deque<Touch::Hit> held;
  return held;
}

bool startsWith(const std::string& text, const char* prefix) {
  return text.rfind(prefix, 0) == 0;
}

std::string onScreen() { return screens().empty() ? std::string() : screens().back(); }

bool drewSince(size_t mark, const char* prefix) {
  for (size_t i = mark; i < screens().size(); ++i) {
    if (startsWith(screens()[i], prefix)) {
      return true;
    }
  }
  return false;
}

size_t drawsSince(size_t mark, const char* prefix) {
  size_t found = 0;
  for (size_t i = mark; i < screens().size(); ++i) {
    if (startsWith(screens()[i], prefix)) {
      ++found;
    }
  }
  return found;
}

bool loggedSince(size_t mark, const char* fragment) {
  for (size_t i = mark; i < logLines().size(); ++i) {
    if (logLines()[i].find(fragment) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool showedSince(size_t mark, const char* cardId) {
  for (size_t i = mark; i < impressions().size(); ++i) {
    if (impressions()[i] == cardId) {
      return true;
    }
  }
  return false;
}

bool neighbourIsOnScreen() { return onScreen() == "before" || onScreen() == "after"; }

/// True when the showing opened last is `cardId`'s and the rotation opened it.
bool rotationLastOpened(const char* cardId) {
  return !impressions().empty() && impressions().back() == cardId &&
         arrivals().back() == Impressions::Arrival::Rotation;
}

// --- the service ------------------------------------------------------------

enum class Service {
  Answers,      // the request is made and the scripted status and body come back
  Unreachable,  // the request cannot begin
  TlsNotReady,  // the secure client was never set up
};

Service gService = Service::Answers;

HTTPClient& scriptedClient() {
  static HTTPClient held;
  return held;
}

void serviceAnswers(int status, const std::string& body) {
  gService = Service::Answers;
  scriptedClient().status = status;
  scriptedClient().body = body;
}

void serviceIsUnreachable() { gService = Service::Unreachable; }

void tlsIsNotReady() { gService = Service::TlsNotReady; }

const char* const kClearSky =
    R"({"radiusMiles":10,"notableDistanceMiles":2,"status":"Ok","aircraft":[]})";

const char* const kStaleAndEmpty =
    R"({"radiusMiles":10,"notableDistanceMiles":2,"status":"Stale","aircraft":[]})";

const char* const kUnavailableAndEmpty =
    R"({"radiusMiles":10,"notableDistanceMiles":2,"status":"Unavailable","aircraft":[]})";

const char* const kNotConfigured = R"({"radiusMiles":10,"status":"NotConfigured"})";

const char* const kNotActivated =
    R"({"reason":"device_not_activated","message":"Activate this display first."})";

const char* const kProviderDisabled =
    R"({"reason":"content_provider_disabled","message":"Aircraft is turned off."})";

const char* const kUnnamedRefusal = R"({"reason":"something_new","message":"Not today."})";

/// One aircraft `distanceMiles` away. The service says anything inside 2 miles
/// is notable, so 5.0 is an ordinary sighting and 1.0 a notable one.
std::string onePlane(const char* callsign, double distanceMiles, const char* status = "Ok") {
  char body[512];
  snprintf(body, sizeof(body),
           R"({"radiusMiles":10,"notableDistanceMiles":2,"status":"%s","aircraft":[)"
           R"({"callsign":"%s","altitudeFeet":31000,"speedKnots":440,"headingDegrees":270,)"
           R"("distanceMiles":%.1f,"airlineCode":"UAL","airlineName":"United",)"
           R"("originCode":"IAD","destinationCode":"DEN","registration":"N12345"}]})",
           status, callsign, distanceMiles);
  return body;
}

// --- the neighbours ---------------------------------------------------------

uint16_t gBeforeItems = 1;
uint16_t gAfterItems = 1;

uint16_t beforeItemCount() { return gBeforeItems; }
uint16_t afterItemCount() { return gAfterItems; }
void beforeDraw(uint16_t) { screens().push_back("before"); }
void afterDraw(uint16_t) { screens().push_back("after"); }

constexpr int kDwellSeconds = 8;
constexpr int kNotableDwellSeconds = 20;
constexpr int kDefaultDwellSeconds = 12;
constexpr int kManualHoldSeconds = 30;

bool registerNeighbour(const char* id, int16_t order, Cards::ItemCountFn itemCount,
                       Cards::DrawFn draw) {
  Cards::CardSpec spec;
  spec.id = id;
  spec.kind = Cards::Kind::List;
  spec.itemCount = itemCount;
  spec.draw = draw;
  spec.order = order;
  spec.dwellSeconds = kDwellSeconds;
  return Cards::registerCard(spec, Cards::KeyPolicy::Keyless);
}

// --- the aircraft card, through the hooks the scheduler uses -----------------

int8_t gAircraft = -1;

Cards::CardSpec& aircraft() { return Cards::at(static_cast<uint8_t>(gAircraft)); }

uint16_t planesHeld() { return aircraft().itemCount(); }

std::string aircraftStatus() { return aircraft().status(); }

bool aircraftIsNotable() { return aircraft().isNotable(0); }

// --- driving the scheduler ---------------------------------------------------

void tick() {
  gNowMs += 1000;
  CardManager::poll();
}

void pass(int seconds) {
  for (int i = 0; i < seconds; ++i) {
    tick();
  }
}

/// Runs until a screen starting with `prefix` goes up that was not up the second
/// before, so a dwell counted from the return is counted from the arrival.
bool passUntilArrives(const char* prefix, int within) {
  for (int i = 0; i < within; ++i) {
    const bool wasUp = startsWith(onScreen(), prefix);
    tick();
    if (!wasUp && startsWith(onScreen(), prefix)) {
      return true;
    }
  }
  return false;
}

/// How many seconds pass before something else is on screen, or -1.
int secondsUntilScreenChanges(int limit) {
  const std::string start = onScreen();
  for (int second = 1; second <= limit; ++second) {
    tick();
    if (onScreen() != start) {
      return second;
    }
  }
  return -1;
}

/// The refresh interval coming due for the aircraft card, with the clock held.
void refreshAircraft() {
  aircraft().everFetched = false;
  CardManager::poll();
}

void tap(Touch::Hit hit) {
  pendingTaps().push_back(hit);
  CardManager::poll();
}

Cards::Policy& policyStorage() {
  static Cards::Policy held;
  return held;
}

void addEntry(Cards::Policy& policy, const char* id, int order, int notableDwellSeconds,
              bool allowBanner) {
  Cards::PolicyEntry& entry = policy.entries[policy.entryCount++];
  entry.id = id;
  entry.kind = "list";
  entry.order = order;
  entry.dwellSeconds = kDwellSeconds;
  entry.notableDwellSeconds = notableDwellSeconds;
  entry.allowBanner = allowBanner;
}

/// The aircraft card never carries a banner here, so a plane is always what its
/// card draws. `neighboursCarryBanners` lets the two beside it carry one.
void applyPolicy(bool withNeighbours, bool neighboursCarryBanners = false) {
  Cards::Policy& policy = policyStorage();
  policy = Cards::Policy();
  policy.present = true;
  policy.defaultDwellSeconds = kDefaultDwellSeconds;
  policy.manualNavHoldSeconds = kManualHoldSeconds;
  if (withNeighbours) {
    addEntry(policy, "before", 1, 0, neighboursCarryBanners);
  }
  addEntry(policy, "aircraft", 2, kNotableDwellSeconds, false);
  if (withNeighbours) {
    addEntry(policy, "after", 3, 0, neighboursCarryBanners);
  }
  CardManager::applyPolicy(policy);
}

/// The three-card policy, history starting again from whatever is up, and long
/// enough for a manual hold left by the case before to run out.
void settle() {
  applyPolicy(/*withNeighbours=*/true);
  pass(kManualHoldSeconds + 1);
}

// --- the cases ---------------------------------------------------------------

void reportsNothingBeforeItsFirstFetch() {
  beginCase("before the first fetch");

  check(planesHeld() == 0, "the card reports no items before anything has been fetched");
  check(aircraftStatus() == "never fetched", "its status says it has never fetched");
}

void aFirstFetchWithNoAnswerTakesNoTurn() {
  beginCase("a first fetch that got no answer");

  serviceIsUnreachable();
  const size_t logMark = logLines().size();
  CardManager::begin();
  applyPolicy(/*withNeighbours=*/true);

  check(planesHeld() == 0, "the card reports no items when its first fetch got no answer");
  check(aircraftStatus() == "network error: Cannot reach the aircraft service.",
        "its status names the failure for the log");
  check(loggedSince(logMark, "[aircraft] no plane held"),
        "the fetch logs that no plane is held and the card takes no turn");

  const size_t mark = screens().size();
  const size_t shown = impressions().size();
  pass(24);
  check(!drewSince(mark, "aircraft"), "no aircraft screen is drawn");
  check(!showedSince(shown, "aircraft"), "no showing is opened for the card");
  check(!drewSince(mark, "no content"), "the placeholder is not drawn while other cards have content");
}

void aClearSkyTakesNoTurn() {
  beginCase("a clear sky");

  serviceAnswers(200, kClearSky);
  const size_t logMark = logLines().size();
  refreshAircraft();

  check(planesHeld() == 0, "the card reports no items for a clear sky");
  check(aircraftStatus() == "ok, nothing within 10 mi", "its status says the sky is clear");
  check(loggedSince(logMark, "[aircraft] no plane held"),
        "the fetch logs that no plane is held and the card takes no turn");

  const size_t mark = screens().size();
  const size_t shown = impressions().size();
  pass(60);
  check(!drewSince(mark, "aircraft"), "no aircraft screen is drawn across a minute of rotation");
  check(!showedSince(shown, "aircraft"), "no showing is opened for the card");
  check(!drewSince(mark, "no content"), "the placeholder is not drawn in the card's place");
  check(drawsSince(mark, "before") >= 3 && drawsSince(mark, "after") >= 3,
        "the two neighbours keep alternating");

  // The scheduler never asks, because it checks the item count first. This is
  // the card's own draw function called the way a caller that did not check
  // would call it.
  const size_t drawMark = screens().size();
  const size_t drawLogMark = logLines().size();
  aircraft().draw(0);
  check(screens().size() == drawMark, "asked to draw with no plane, the card draws nothing");
  check(loggedSince(drawLogMark, "[aircraft] draw reached with no plane held"),
        "and logs that it was asked");
}

void aPlaneArrivingPutsTheCardInTheRotation() {
  beginCase("a plane arrives");

  serviceAnswers(200, onePlane("UAL123", 5.0));
  refreshAircraft();

  check(planesHeld() == 1, "the card reports one item while it holds a plane");
  check(passUntilArrives("aircraft card: UAL123", 30), "the card goes up on its turn");
  check(secondsUntilScreenChanges(60) == kDwellSeconds,
        "an ordinary sighting holds the screen for the card's ordinary dwell");
  check(onScreen() == "after", "the rotation carries on to the card after it");
}

void aClosePlaneTakesTheNotableDwell() {
  beginCase("a plane inside the notable distance");

  serviceAnswers(200, onePlane("UAL123", 1.0));
  refreshAircraft();

  check(aircraftIsNotable(), "a plane inside the notable distance is notable");
  check(passUntilArrives("aircraft card: UAL123", 40), "the card goes up on its turn");
  check(secondsUntilScreenChanges(60) == kNotableDwellSeconds,
        "a notable sighting holds the screen for the notable dwell");
}

void aPlaneLeavingWhileItsCardIsUpMovesOn() {
  beginCase("the plane leaves while its card is up");

  check(passUntilArrives("aircraft card:", 60), "the card comes round again");
  pass(3);

  serviceAnswers(200, kClearSky);
  const size_t mark = screens().size();
  const size_t logMark = logLines().size();
  refreshAircraft();

  check(onScreen() == "after", "the card after it goes up at once");
  check(rotationLastOpened("after"),
        "the card that replaced it is recorded as a showing the rotation opened");
  check(!drewSince(mark, "aircraft"), "no aircraft screen is drawn for the empty sky");
  check(!drewSince(mark, "no content"), "the placeholder is not drawn over the rotation");
  check(loggedSince(logMark, "'aircraft' has nothing to show after its refresh"),
        "the scheduler logs that it left the card");
  check(planesHeld() == 0, "the card reports no items");
  check(!aircraftIsNotable(), "a card with no plane is not notable");
  check(secondsUntilScreenChanges(60) == kDwellSeconds,
        "the card that replaced it gets a full dwell of its own");
  check(onScreen() == "before", "and the rotation carries on from there");

  for (int i = 0; i < 4; ++i) {
    tap(Touch::Hit::Reverse);
    check(neighbourIsOnScreen(), "a reverse tap lands on a neighbour, never on the emptied card");
  }
}

void anUnansweredFetchKeepsThePlane() {
  beginCase("a fetch that got no answer");

  settle();
  serviceAnswers(200, onePlane("DAL456", 5.0));
  refreshAircraft();
  check(planesHeld() == 1, "a plane is held to begin with");

  serviceIsUnreachable();
  size_t logMark = logLines().size();
  refreshAircraft();
  check(planesHeld() == 1, "a request that cannot begin keeps the plane");
  check(loggedSince(logMark, "keeping the last good result"), "and logs that it kept it");

  serviceAnswers(400, "");
  logMark = logLines().size();
  refreshAircraft();
  check(planesHeld() == 1, "an HTTP status that is no answer of ours keeps the plane");
  check(loggedSince(logMark, "keeping the last good result"), "and logs that it kept it");

  serviceAnswers(403, "<html>a proxy's page</html>");
  refreshAircraft();
  check(planesHeld() == 1, "a 403 whose body is not ours keeps the plane");

  serviceAnswers(200, onePlane("DAL456", 5.0, "Stale"));
  refreshAircraft();
  check(planesHeld() == 1, "a plane served behind a stale upstream is still a plane");

  check(passUntilArrives("aircraft card: DAL456", 40), "the kept plane still takes its turn");
}

struct Drop {
  const char* what;
  bool tlsReady;
  int httpStatus;
  const char* body;
  const char* statusText;
};

const Drop kDrops[] = {
    {"a clear sky", true, 200, kClearSky, "ok, nothing within 10 mi"},
    {"an empty list behind a stale upstream", true, 200, kStaleAndEmpty,
     "upstream refresh failed (reason is on the server, not the device)"},
    {"an empty list behind an unavailable upstream", true, 200, kUnavailableAndEmpty,
     "upstream refresh failed (reason is on the server, not the device)"},
    {"HTTP 429", true, 429, "", "upstream refresh failed (reason is on the server, not the device)"},
    {"HTTP 503", true, 503, "", "upstream refresh failed (reason is on the server, not the device)"},
    {"no provider on file", true, 200, kNotConfigured, "resting: no aircraft provider on file"},
    {"no position for the display (404)", true, 404, "", "resting: no position for this device"},
    {"a display that is not activated", true, 403, kNotActivated, "refused: device not activated"},
    {"a provider turned off", true, 403, kProviderDisabled, "refused: provider disabled"},
    {"a refusal with a reason this build does not know", true, 403, kUnnamedRefusal,
     "network error: Not today."},
    {"a rejected secret (401)", true, 401, "", "refused: device secret rejected"},
    {"a body that is not JSON", true, 200, "not json",
     "network error: The aircraft service sent something unreadable."},
    {"a secure client that was never set up", false, 0, "",
     "network error: Cannot verify the service's identity."},
};

void anAnsweredFetchWithNoPlaneDropsIt() {
  beginCase("an answered fetch with no plane");

  for (const Drop& drop : kDrops) {
    serviceAnswers(200, onePlane("DAL456", 5.0));
    refreshAircraft();
    checkFor(drop.what, planesHeld() == 1, "a plane is held to begin with");

    if (drop.tlsReady) {
      serviceAnswers(drop.httpStatus, drop.body);
    } else {
      tlsIsNotReady();
    }
    const size_t mark = screens().size();
    const size_t shown = impressions().size();
    const size_t logMark = logLines().size();
    refreshAircraft();

    checkFor(drop.what, planesHeld() == 0, "the card reports no items");
    checkFor(drop.what, aircraftStatus() == drop.statusText, "its status names the reason");
    checkFor(drop.what, loggedSince(logMark, "[aircraft] no plane held"),
             "the fetch logs that no plane is held");

    pass(24);
    checkFor(drop.what, !drewSince(mark, "aircraft"), "no aircraft screen is drawn");
    checkFor(drop.what, !showedSince(shown, "aircraft"), "no showing is opened for the card");
    checkFor(drop.what, !drewSince(mark, "no content"), "the placeholder is not drawn");
  }
}

void everyCardEmptyShowsThePlaceholder() {
  beginCase("every card empty");

  settle();
  serviceAnswers(200, kClearSky);
  refreshAircraft();
  gBeforeItems = 0;
  gAfterItems = 0;

  const size_t mark = screens().size();
  pass(kDwellSeconds + 5);
  check(startsWith(onScreen(), "no content"),
        "with nothing to show anywhere, the scheduler's placeholder is on screen");
  pass(30);
  check(!drewSince(mark, "aircraft"), "no aircraft screen is drawn while everything is empty");

  serviceAnswers(200, onePlane("SWA789", 5.0));
  refreshAircraft();
  check(onScreen() == "aircraft card: SWA789",
        "the card goes up the moment a fetch returns a plane");

  tap(Touch::Hit::Reverse);
  check(onScreen() == "aircraft card: SWA789",
        "a reverse tap stays on the card rather than stepping back to the placeholder");

  gBeforeItems = 1;
  gAfterItems = 1;
}

void aloneInAPolicy() {
  beginCase("the only card in a policy");

  applyPolicy(/*withNeighbours=*/false);
  pass(kManualHoldSeconds + 1);

  size_t mark = screens().size();
  pass(40);
  check(drawsSince(mark, "aircraft card: SWA789") >= 3 &&
            drawsSince(mark, "aircraft card: SWA789") == screens().size() - mark,
        "with a plane, the card is the only thing drawn");

  serviceAnswers(200, kClearSky);
  mark = screens().size();
  refreshAircraft();
  check(startsWith(onScreen(), "no content"),
        "when the plane leaves, the placeholder goes up at once");
  pass(40);
  check(!drewSince(mark, "aircraft"), "no aircraft screen is drawn while there is no plane");

  serviceAnswers(200, onePlane("SWA790", 5.0));
  refreshAircraft();
  check(onScreen() == "aircraft card: SWA790", "the card returns with the next plane");

  tap(Touch::Hit::Reverse);
  check(onScreen() == "aircraft card: SWA790",
        "a reverse tap stays on the card rather than stepping back to the placeholder");
  pass(kManualHoldSeconds + 1);
}

void tapsPassOverTheEmptiedCard() {
  beginCase("taps after the card has emptied");

  applyPolicy(/*withNeighbours=*/true);
  check(passUntilArrives("aircraft card:", 60), "the card takes a turn with its plane");
  check(passUntilArrives("after", 60), "and the rotation moves past it");

  serviceAnswers(200, kClearSky);
  refreshAircraft();

  const size_t mark = screens().size();
  for (int i = 0; i < 3; ++i) {
    tap(Touch::Hit::Reverse);
  }
  for (int i = 0; i < 5; ++i) {
    tap(Touch::Hit::Forward);
  }
  check(screens().size() > mark, "the taps drew something");
  check(!drewSince(mark, "aircraft"), "no tap in either direction lands on the emptied card");
  check(!drewSince(mark, "no content"), "and none lands on the placeholder");
}

void aPlaneLeavingDuringAManualHoldMovesOn() {
  beginCase("the plane leaves during a manual hold");

  settle();
  serviceAnswers(200, onePlane("JBU321", 5.0));
  refreshAircraft();
  check(passUntilArrives("aircraft card: JBU321", 60), "the card takes its turn");
  check(passUntilArrives("after", 60), "and the rotation moves past it");

  tap(Touch::Hit::Reverse);
  check(onScreen() == "aircraft card: JBU321", "a reverse tap steps back to the plane");
  pass(5);

  serviceAnswers(200, kClearSky);
  const size_t mark = screens().size();
  const int sought = gSought;
  refreshAircraft();
  check(onScreen() == "after", "the card that followed it goes up at once");
  check(rotationLastOpened("after"),
        "the replacement is recorded as a showing the rotation opened, not one somebody sought");
  check(!drewSince(mark, "aircraft"), "no aircraft screen is drawn for the empty sky");
  check(secondsUntilScreenChanges(60) == kDwellSeconds,
        "the replacement keeps its ordinary dwell, not the hold that was on the aircraft card");
  pass(kManualHoldSeconds);
  check(gSought == sought,
        "no card is marked as sought once the hold's own time has passed");
}

/// The last banner drawn before screen `index`, or an empty string.
std::string bannerBefore(size_t index) {
  for (size_t i = index; i > 0; --i) {
    if (startsWith(screens()[i - 1], "banner: ")) {
      return screens()[i - 1];
    }
  }
  return std::string();
}

void anAnnouncementKeepsItsTurnWhenThePlaneLeaves() {
  beginCase("an announcement when the plane leaves");

  applyPolicy(/*withNeighbours=*/true, /*neighboursCarryBanners=*/true);
  pass(kManualHoldSeconds + 1);

  // Two announcements for every card that allows a banner. The queue moves one
  // place for each card that goes up, so across before, aircraft, after the
  // two neighbours carry the same one: the plane's card uses up the step in
  // between without drawing it.
  Cards::Announcement notices[2];
  snprintf(notices[0].id, sizeof(notices[0].id), "first");
  snprintf(notices[0].text, sizeof(notices[0].text), "first notice");
  snprintf(notices[1].id, sizeof(notices[1].id), "second");
  snprintf(notices[1].text, sizeof(notices[1].text), "second notice");
  Cards::setAnnouncements(notices, 2);
  // A lap of the two neighbours, so each has gone up once with a banner.
  pass(2 * kDwellSeconds + 1);

  serviceAnswers(200, onePlane("AAL654", 5.0));
  refreshAircraft();
  check(passUntilArrives("aircraft card: AAL654", 60), "the card takes its turn with its plane");
  const std::string carriedByBefore = bannerBefore(screens().size());
  check(!carriedByBefore.empty(), "the card before it carried an announcement");
  pass(3);

  serviceAnswers(200, kClearSky);
  refreshAircraft();
  check(startsWith(onScreen(), "banner: "), "the card that replaces it carries an announcement");
  check(rotationLastOpened("after"), "and that card is the one after it in the order");
  check(onScreen() == carriedByBefore,
        "it is the announcement that card would have carried had the plane's dwell run out");

  Cards::setAnnouncements(nullptr, 0);
  settle();
}

void noStatusScreenWasEverDrawn() {
  beginCase("the whole run");

  check(!drewSince(0, "aircraft status"), "the aircraft status screen was never drawn");
}

}  // namespace

// --- everything the two shipped files call outside themselves -----------------

unsigned long millis() { return gNowMs; }

void pinMode(uint8_t, uint8_t) {}

/// The action button reads released for the whole run.
int digitalRead(uint8_t) { return HIGH; }

namespace Log {

void printf(const char* format, ...) {
  char line[512];
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(line, sizeof(line), format, arguments);
  va_end(arguments);
  logLines().push_back(line);
}

void verbose(const char* format, ...) {
  char line[512];
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(line, sizeof(line), format, arguments);
  va_end(arguments);
  logLines().push_back(line);
}

void line(const String& text) { logLines().push_back(text); }

void line(const char* text) { logLines().push_back(text == nullptr ? "" : text); }

bool streamingEnabled() { return false; }

}  // namespace Log

namespace Http {

bool ready() { return gService != Service::TlsNotReady; }

bool beginRequest(const String&) { return gService == Service::Answers; }

HTTPClient& client() { return scriptedClient(); }

}  // namespace Http

namespace Identity {

String deviceSecret() { return String("a-test-secret"); }

void noteHasPictureCards(bool) {}

}  // namespace Identity

namespace Maintenance {

String failureText(const String& plainText, bool) { return plainText; }

}  // namespace Maintenance

namespace Assets {

// No body in this test names a logo, so none of these is reached.
bool ensureCached(const String&) { return false; }
bool isCached(const String&) { return false; }
bool drawCachedInRect(const String&, int32_t, int32_t, int32_t, int32_t) { return false; }
bool fetchToRam(const String&, RamAssetBuffer&) { return false; }
void releaseRamBuffer(RamAssetBuffer&) {}
bool drawRamInRect(const String&, const RamAssetBuffer&, int32_t, int32_t, int32_t, int32_t) {
  return false;
}

}  // namespace Assets

namespace Display {

void showAircraftCard(const String& callsign, const String&, int, double, double, double,
                      const String&, const String&, const String&, const String&, const String&,
                      const String&, bool) {
  screens().push_back(std::string("aircraft card: ") + callsign.c_str());
}

void aircraftLogoZone(int16_t& x, int16_t& y, int16_t& w, int16_t& h) {
  x = 0;
  y = 0;
  w = 0;
  h = 0;
}

/// Kept so a build of Aircraft.cpp that still draws a status screen links, and
/// the case that looks for one can say so.
void showAircraftStatus(const String& headline, const String&, bool) {
  screens().push_back(std::string("aircraft status: ") + headline.c_str());
}

void showNoContent(const String& headline, const String&) {
  screens().push_back(std::string("no content: ") + headline.c_str());
}

void showBannerCard(const String& text) {
  screens().push_back(std::string("banner: ") + text.c_str());
}

void drawActionButtons(const String*, uint8_t) {}

void setContentBudget(bool) {}

int contentBottom() { return 240; }

void actionButtonZone(uint8_t, uint8_t, int16_t& x, int16_t& y, int16_t& w, int16_t& h) {
  x = 0;
  y = 0;
  w = 0;
  h = 0;
}

void showButtonPressConfirmation() {}

void drawNavAffordances(bool) {}

void flashNavEdge(bool, bool) {}

}  // namespace Display

namespace HeapRatchet {

Scope::Scope(Phase phase, const char* subject)
    : previousPhase_(phase), previousSubject_(subject) {}

Scope::~Scope() {
  (void)previousPhase_;
  (void)previousSubject_;
}

}  // namespace HeapRatchet

namespace Impressions {

void beginShowing(const char* cardId, const char*, const char*, Arrival arrival) {
  impressions().push_back(cardId == nullptr ? "" : cardId);
  arrivals().push_back(arrival);
}

void addFrameMember(const char*, const char*) {}

void endShowing() {}

void setPresent(bool) {}

void markSought() { ++gSought; }

}  // namespace Impressions

namespace ConfigRequest {

void noteTap(uint32_t, int32_t, int32_t) {}

}  // namespace ConfigRequest

namespace CardPolicyStore {

uint16_t storedBytes() { return 0; }

bool load(Cards::Policy&) { return false; }

void save(const Cards::Policy&) {}

}  // namespace CardPolicyStore

namespace Touch {

void setActionZones(const Rect*, uint8_t) {}

bool poll(Tap& tap) {
  if (pendingTaps().empty()) {
    return false;
  }
  tap = Tap();
  tap.hit = pendingTaps().front();
  pendingTaps().pop_front();
  return true;
}

}  // namespace Touch

namespace Motion {

bool noteTouchAndShouldSwallow() { return false; }

BacklightState state() { return BacklightState::Active; }

}  // namespace Motion

namespace Actions {

void begin() {}

uint8_t forCard(const char*, Definition*, uint8_t) { return 0; }

bool recordPress(const Definition&, const String&, const String&) { return true; }

}  // namespace Actions

int main() {
  gAircraft = Cards::indexOf("aircraft");
  if (gAircraft < 0) {
    std::printf("Failed: App/Aircraft.cpp did not register a card called 'aircraft'\n");
    return 1;
  }
  if (!registerNeighbour("before", 1, beforeItemCount, beforeDraw) ||
      !registerNeighbour("after", 3, afterItemCount, afterDraw)) {
    std::printf("Failed: the two neighbour cards did not register\n");
    return 1;
  }

  reportsNothingBeforeItsFirstFetch();
  aFirstFetchWithNoAnswerTakesNoTurn();
  aClearSkyTakesNoTurn();
  aPlaneArrivingPutsTheCardInTheRotation();
  aClosePlaneTakesTheNotableDwell();
  aPlaneLeavingWhileItsCardIsUpMovesOn();
  anUnansweredFetchKeepsThePlane();
  anAnsweredFetchWithNoPlaneDropsIt();
  everyCardEmptyShowsThePlaceholder();
  aloneInAPolicy();
  tapsPassOverTheEmptiedCard();
  aPlaneLeavingDuringAManualHoldMovesOn();
  anAnnouncementKeepsItsTurnWhenThePlaneLeaves();
  noStatusScreenWasEverDrawn();

  if (gFailures != 0) {
    std::printf("Failed: %d check(s) over the aircraft card's place in the rotation\n", gFailures);
    return 1;
  }

  std::printf("PASS: aircraft card takes no turn without a plane, %d cases, %d checks\n", gCases,
              gChecks);
  return 0;
}
