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
//   - A plane outliving its showForSeconds keeps its turn. Nothing on the
//     device drops a plane for being old, including through seventy fetches
//     that got no answer; what drops it is an answered fetch carrying none.
//   - A candidate with less than kMinimumTimeLeft is passed over when the
//     answer is read, so the plane drawn is the first clearing the floor and a
//     military sighting outranks the rest only while it clears the floor too.
//     Twenty seconds qualifies and nineteen does not. A list where no plane
//     clears it holds none of them.
//   - A plane carrying no limit at all clears the floor and stays.
//   - An altitude the server sent as null draws "--", and the literal
//     "UNKNOWN" where a callsign goes draws the registration.
//   - The card's last line states the age of the plane's position where the
//     server sent one, grown by the time since the answer, and dates the
//     answer where it did not. It climbs a unit as the figure grows: seconds,
//     minutes, hours, then days.
//   - The card asks again on the schedule the server sets, with a floor under
//     it, falls back to the fixed interval on an answer that carries none, and
//     gives the schedule up after three fetches in a row that got no answer.
//
// What this cannot check: anything about pixels, since Display is a list of
// names here; heap, since tests/aircraft-stubs/ArduinoJson.h allocates from the
// host and applies no filter; TLS; either board's build, which only the arduino
// compile reaches; and the time a real fetch takes, since one returns at once
// here and the clock does not move during it.
//
// The filter is the one gap with a gate of its own. The JSON stub accepts
// fetchMine()'s filter and applies none, so no case here can see a field the
// filter drops; ci/test-aircraft-takes-no-turn.sh greps for the three keys this
// file's cases depend on before it compiles anything.

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <initializer_list>
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

/// The altitude the aircraft card was drawn with, one entry per draw, oldest
/// first. "--" for the figure Display reads as "nobody reported one".
std::vector<std::string>& altitudes() {
  static std::vector<std::string> held;
  return held;
}

/// The card's last line, one entry per draw, oldest first.
std::vector<std::string>& ageLines() {
  static std::vector<std::string> held;
  return held;
}

std::string lastAltitude() { return altitudes().empty() ? std::string() : altitudes().back(); }

std::string lastAgeLine() { return ageLines().empty() ? std::string() : ageLines().back(); }

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
///
/// An hour of showForSeconds, so a case built on this one is about the card's
/// place in the rotation and never about the countdown running out underneath
/// it. The cases that are about the countdown use onePlaneWith() below.
std::string onePlane(const char* callsign, double distanceMiles, const char* status = "Ok") {
  char body[512];
  snprintf(body, sizeof(body),
           R"({"radiusMiles":10,"notableDistanceMiles":2,"status":"%s","aircraft":[)"
           R"({"callsign":"%s","altitudeFeet":31000,"speedKnots":440,"headingDegrees":270,)"
           R"("distanceMiles":%.1f,"airlineCode":"UAL","airlineName":"United",)"
           R"("originCode":"IAD","destinationCode":"DEN","registration":"N12345",)"
           R"("showForSeconds":3600}]})",
           status, callsign, distanceMiles);
  return body;
}

/// One aircraft with the fields a newer server adds. A negative showForSeconds
/// or positionAgeSeconds leaves that field off the wire altogether, which is
/// what a server older than the field sends; `altitude` goes on as written, so
/// "null" is the altitude nobody reported.
std::string onePlaneWith(const char* callsign, double distanceMiles, int showForSeconds,
                         int positionAgeSeconds, const char* altitude = "31000") {
  std::string extra;
  char field[64];
  if (showForSeconds >= 0) {
    snprintf(field, sizeof(field), R"(,"showForSeconds":%d)", showForSeconds);
    extra += field;
  }
  if (positionAgeSeconds >= 0) {
    snprintf(field, sizeof(field), R"(,"positionAgeSeconds":%d)", positionAgeSeconds);
    extra += field;
  }

  char body[640];
  snprintf(body, sizeof(body),
           R"({"radiusMiles":10,"notableDistanceMiles":2,"status":"Ok","aircraft":[)"
           R"({"callsign":"%s","altitudeFeet":%s,"speedKnots":440,"headingDegrees":270,)"
           R"("distanceMiles":%.1f,"airlineCode":"UAL","airlineName":"United",)"
           R"("originCode":"IAD","destinationCode":"DEN","registration":"N12345"%s}]})",
           callsign, altitude, distanceMiles, extra.c_str());
  return body;
}

/// One entry in a list of sightings, in the order the server sends them.
struct Plane {
  const char* callsign;
  int showForSeconds;
  bool isMilitary;
};

/// A list of sightings, all five miles out, each with its own time left.
std::string planes(std::initializer_list<Plane> list) {
  std::string body = R"({"radiusMiles":10,"notableDistanceMiles":2,"status":"Ok","aircraft":[)";
  bool first = true;
  for (const Plane& plane : list) {
    char one[320];
    snprintf(one, sizeof(one),
             R"(%s{"callsign":"%s","altitudeFeet":31000,"speedKnots":440,)"
             R"("headingDegrees":270,"distanceMiles":5.0,"showForSeconds":%d,)"
             R"("isMilitary":%s,"registration":"N12345"})",
             first ? "" : ",", plane.callsign, plane.showForSeconds,
             plane.isMilitary ? "true" : "false");
    body += one;
    first = false;
  }
  body += "]}";
  return body;
}

/// The same body with a fetch schedule on it, which is a top-level field beside
/// the array rather than part of it.
std::string withSchedule(std::string body, int nextFetchSeconds) {
  char field[48];
  snprintf(field, sizeof(field), R"("nextFetchSeconds":%d,)", nextFetchSeconds);
  return body.insert(1, field);
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
  check(loggedSince(logMark, "could not reach the service - keeping"),
        "and logs that it kept it");

  serviceAnswers(400, "");
  logMark = logLines().size();
  refreshAircraft();
  check(planesHeld() == 1, "an HTTP status that is no answer of ours keeps the plane");
  check(loggedSince(logMark, "could not reach the service - keeping"),
        "and logs that it kept it");

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

void aPlaneWhoseTimeIsUpKeepsItsTurn() {
  beginCase("a plane whose time is up");

  settle();
  serviceAnswers(200, onePlaneWith("UAL900", 5.0, /*showForSeconds=*/30,
                                   /*positionAgeSeconds=*/-1));
  refreshAircraft();
  check(planesHeld() == 1, "the card holds the plane the fetch returned");
  check(passUntilArrives("aircraft card: UAL900", 60), "and it takes its turn");

  // Well past the thirty seconds the server had left on it, with no answer in
  // between: nothing on the device drops a plane for being old.
  const size_t logMark = logLines().size();
  pass(120);
  check(planesHeld() == 1, "it is still held four times its own showForSeconds later");
  check(aircraftStatus() == "ok, showing UAL900 at 5.0 mi",
        "its status still names the plane rather than an expiry");
  check(!loggedSince(logMark, "[aircraft] no plane held"),
        "and nothing in the stream says the card gave up its turn");

  const size_t mark = screens().size();
  check(passUntilArrives("aircraft card: UAL900", 60), "the card keeps coming round");
  check(drewSince(mark, "aircraft card: UAL900"), "drawing the same plane");
}

void aPlaneIsHeldThroughALongOutage() {
  beginCase("a plane carried through a long outage");

  settle();
  serviceAnswers(200, onePlaneWith("DAL910", 5.0, 40, /*positionAgeSeconds=*/10));
  refreshAircraft();
  const unsigned long answeredAtMs = gNowMs;
  check(planesHeld() == 1, "the card holds the plane");

  serviceIsUnreachable();
  for (int minute = 0; minute < 70; ++minute) {
    pass(60);
    refreshAircraft();
  }
  check(planesHeld() == 1, "seventy fetches that got no answer keep it");

  check(passUntilArrives("aircraft card: DAL910", 60), "the card still takes its turn");
  const unsigned long age = 10 + (gNowMs - answeredAtMs) / 1000UL;
  check(age >= 3600UL && age < 7200UL, "between one and two hours of position age has built up");
  check(lastAgeLine() == "Reported 1 hr ago",
        "and the last line states it in hours rather than counting minutes");

  // The answer that finally arrives is what replaces the plane, not a timer.
  const size_t logMark = logLines().size();
  serviceAnswers(200, kClearSky);
  refreshAircraft();
  check(planesHeld() == 0, "an answered fetch with no plane is what drops it");
  check(loggedSince(logMark, "[aircraft] no plane held"), "and the stream says so then");
}
void theFirstPlaneClearingTheFloorIsDrawn() {
  beginCase("a list whose nearest plane is nearly out of time");

  settle();
  serviceAnswers(200, planes({{"NEAR005", 5, false}, {"NEXT100", 100, false}}));
  refreshAircraft();
  check(planesHeld() == 1, "the card holds a plane");
  check(passUntilArrives("aircraft card: NEXT100", 60),
        "the plane drawn is the first one clearing the floor, not the nearest");
}

void theFloorsOwnBoundary() {
  beginCase("the floor's own boundary");

  settle();
  serviceAnswers(200, planes({{"AT020", 20, false}, {"LATER300", 300, false}}));
  refreshAircraft();
  check(passUntilArrives("aircraft card: AT020", 60),
        "a plane with exactly the floor's seconds left is taken");

  serviceAnswers(200, planes({{"UNDER19", 19, false}, {"LATER301", 301, false}}));
  refreshAircraft();
  check(passUntilArrives("aircraft card: LATER301", 60),
        "one second under the floor is passed over");
}

void aMilitaryPlaneClearingTheFloorOutranksTheRest() {
  beginCase("a military plane among the rest");

  settle();
  serviceAnswers(200, planes({{"CIV100", 100, false}, {"MIL200", 200, true}}));
  refreshAircraft();
  check(passUntilArrives("aircraft card: MIL200", 60),
        "a military plane clearing the floor takes the card over a nearer civil one");

  serviceAnswers(200, planes({{"CIV300", 300, false}, {"MIL005", 5, true}}));
  refreshAircraft();
  check(passUntilArrives("aircraft card: CIV300", 60),
        "a military plane under the floor does not outrank one over it");
}

void everyPlaneUnderTheFloorTakesTheCardOutOfTheRotation() {
  beginCase("every plane under the floor");

  settle();
  serviceAnswers(200, onePlaneWith("UAL940", 5.0, 3600, -1));
  refreshAircraft();
  check(planesHeld() == 1, "a plane is held to begin with");

  const size_t logMark = logLines().size();
  const size_t mark = screens().size();
  const size_t shown = impressions().size();
  serviceAnswers(200, planes({{"OLD001", 1, false}, {"OLD019", 19, true}}));
  refreshAircraft();

  check(planesHeld() == 0, "a list where no plane clears the floor holds none of them");
  check(aircraftStatus() == "ok, no plane with enough time left to show",
        "its status says so rather than claiming the sky is clear");
  check(loggedSince(logMark, "2 sighting(s) and none with 20s left"),
        "and the stream says how many it passed over and what the floor is");

  pass(30);
  check(!drewSince(mark, "aircraft"), "no aircraft screen is drawn");
  check(!showedSince(shown, "aircraft"), "no showing is opened for the card");
}

void aPlaneWithNoLimitClearsTheFloorAndStays() {
  beginCase("a plane the server set no limit on");

  settle();
  serviceAnswers(200, onePlaneWith("AAL950", 5.0, /*showForSeconds=*/-1,
                                   /*positionAgeSeconds=*/-1));
  refreshAircraft();
  check(planesHeld() == 1, "a plane with no limit of its own clears the floor and is held");
  check(passUntilArrives("aircraft card: AAL950", 60), "and takes its turn");

  pass(300);
  check(planesHeld() == 1, "and is still held five minutes later");
  check(lastAgeLine().rfind("Updated ", 0) == 0,
        "its last line dates the answer, the way it always has against such a server");
}

void anAltitudeNobodyReportedDrawsADash() {
  beginCase("an altitude nobody reported");

  settle();
  serviceAnswers(200, onePlaneWith("UAL960", 5.0, 3600, -1, "31000"));
  refreshAircraft();
  check(passUntilArrives("aircraft card: UAL960", 60), "the card goes up");
  check(lastAltitude() == "31000", "a reported altitude is drawn as a figure");

  serviceAnswers(200, onePlaneWith("UAL961", 5.0, 3600, -1, "null"));
  refreshAircraft();
  check(onScreen() == "aircraft card: UAL961", "the card redraws with the next plane");
  check(lastAltitude() == "--", "an altitude the server sent as null draws a dash");
}

void anUnknownCallsignFallsBackToTheRegistration() {
  beginCase("the literal UNKNOWN where a callsign goes");

  settle();
  serviceAnswers(200, onePlaneWith("UNKNOWN", 5.0, 3600, -1));
  refreshAircraft();
  check(passUntilArrives("aircraft card: N12345", 60),
        "the card draws the registration rather than the word UNKNOWN");
  check(aircraftStatus() == "ok, showing N12345 at 5.0 mi",
        "and the status line names it the same way");
  check(aircraft().describe(0) == "N12345 - IAD to DEN - 5.0 mi away",
        "and the press label names it rather than nothing at all");
}

void theLastLineStatesThePlanesAge() {
  beginCase("the age on the card's last line");

  settle();
  serviceAnswers(200, onePlaneWith("DAL970", 5.0, 3600, /*positionAgeSeconds=*/45));
  refreshAircraft();
  check(passUntilArrives("aircraft card: DAL970", 60), "the card goes up");

  // Fetched and drawn on the same tick, so the line is the age the server
  // measured and nothing else.
  serviceAnswers(200, onePlaneWith("DAL971", 5.0, 3600, 45));
  refreshAircraft();
  const unsigned long answeredAtMs = gNowMs;
  check(onScreen() == "aircraft card: DAL971", "the refresh redraws it in place");
  check(lastAgeLine() == "Reported 45 sec ago",
        "the line states the age of the position the server measured");

  check(passUntilArrives("before", 60), "the rotation moves on");
  check(passUntilArrives("aircraft card: DAL971", 60), "and comes back round to the card");
  const unsigned long age = 45 + (gNowMs - answeredAtMs) / 1000UL;
  check(age > 45, "time passed while the rotation went round");
  std::string wanted;
  if (age < 60) {
    wanted = "Reported " + std::to_string(age) + " sec ago";
  } else if (age < 120) {
    wanted = "Reported 1 min ago";
  } else {
    wanted = "Reported " + std::to_string(age / 60) + " min ago";
  }
  check(lastAgeLine() == wanted, "and the figure has grown by the time since the answer");

  serviceAnswers(200, onePlaneWith("DAL972", 5.0, 3600, /*positionAgeSeconds=*/-1));
  refreshAircraft();
  check(onScreen() == "aircraft card: DAL972", "the card redraws with a plane carrying no age");
  check(lastAgeLine() == "Updated just now",
        "with no age from the server the line dates the answer instead");

  // The ladder's upper rungs, read straight off the server's own figure so the
  // clock has nothing to do with them.
  serviceAnswers(200, onePlaneWith("DAL973", 5.0, 3600, /*positionAgeSeconds=*/7200));
  refreshAircraft();
  check(lastAgeLine() == "Reported 2 hr ago", "two hours of age reads in hours, not in minutes");

  serviceAnswers(200, onePlaneWith("DAL974", 5.0, 3600, /*positionAgeSeconds=*/3600));
  refreshAircraft();
  check(lastAgeLine() == "Reported 1 hr ago", "and one hour reads without a figure in front of it");

  serviceAnswers(200, onePlaneWith("DAL975", 5.0, 3600, /*positionAgeSeconds=*/180000));
  refreshAircraft();
  check(lastAgeLine() == "Reported 2 days ago", "past two days the line reads in days");

  serviceAnswers(200, onePlaneWith("DAL977", 5.0, 3600, /*positionAgeSeconds=*/90000));
  refreshAircraft();
  check(lastAgeLine() == "Reported 1 day ago", "and the first day reads in the singular");

  serviceAnswers(200, onePlaneWith("DAL976", 5.0, 3600, /*positionAgeSeconds=*/3540));
  refreshAircraft();
  check(lastAgeLine() == "Reported 59 min ago", "a minute under the hour still reads in minutes");
}

void theServerSetsTheFetchCadence() {
  beginCase("the server's own fetch schedule");

  settle();
  serviceAnswers(200, withSchedule(onePlaneWith("UAL980", 5.0, 3600, -1), 60));
  refreshAircraft();
  check(aircraft().refreshSeconds == 60, "the card takes the schedule off the answer");

  const uint32_t fetchedAtMs = aircraft().lastFetchMs;
  pass(59);
  check(aircraft().lastFetchMs == fetchedAtMs, "the card is not asked again before the schedule");
  pass(1);
  check(aircraft().lastFetchMs != fetchedAtMs,
        "and is asked again as soon as the schedule is up, not ten minutes later");

  serviceAnswers(200, withSchedule(onePlaneWith("UAL981", 5.0, 3600, -1), 5));
  refreshAircraft();
  check(aircraft().refreshSeconds == 30, "a schedule under the floor is raised to the floor");
}

void noScheduleKeepsTheFixedInterval() {
  beginCase("an answer carrying no schedule");

  settle();
  serviceAnswers(200, withSchedule(onePlaneWith("UAL985", 5.0, 3600, -1), 60));
  refreshAircraft();
  check(aircraft().refreshSeconds == 60, "the card is on the server's schedule to begin with");

  serviceAnswers(200, onePlaneWith("UAL986", 5.0, 3600, -1));
  refreshAircraft();
  check(aircraft().refreshSeconds == 0,
        "an answer with no schedule on it puts the card back on the fixed interval");

  const uint32_t fetchedAtMs = aircraft().lastFetchMs;
  pass(120);
  check(aircraft().lastFetchMs == fetchedAtMs, "and it is not asked again two minutes later");
}

void anUnansweredFetchGivesUpTheScheduleAfterTwoTries() {
  beginCase("the schedule through a service that stops answering");

  settle();
  serviceAnswers(200, withSchedule(onePlaneWith("UAL990", 5.0, 3600, -1), 60));
  refreshAircraft();
  check(aircraft().refreshSeconds == 60, "the card is on the server's schedule");

  serviceIsUnreachable();
  refreshAircraft();
  check(aircraft().refreshSeconds == 60, "one fetch that got no answer keeps it");
  refreshAircraft();
  check(aircraft().refreshSeconds == 60, "so does a second");
  refreshAircraft();
  check(aircraft().refreshSeconds == 0, "a third puts the card back on the fixed interval");

  serviceAnswers(200, withSchedule(onePlaneWith("UAL991", 5.0, 3600, -1), 60));
  refreshAircraft();
  check(aircraft().refreshSeconds == 60, "an answer puts it back on the server's schedule");
}

void nothingWasEverDrawnWithoutAPlane() {
  beginCase("the whole run");

  check(drawsSince(0, "aircraft card: ") > 0, "the card was drawn during the run");
  check(drawsSince(0, "aircraft card: ") == altitudes().size(),
        "every aircraft screen in the whole run came from a draw that held a plane");
  check(drawsSince(0, "aircraft card: ") == ageLines().size(),
        "and every one of them carried a line dating what it showed");
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

void showAircraftCard(const String& callsign, const String&, int altitudeFeet, double, double,
                      double, const String&, const String&, const String&, const String&,
                      const char* updatedAt, const String&, bool) {
  screens().push_back(std::string("aircraft card: ") + callsign.c_str());
  altitudes().push_back(altitudeFeet == kAltitudeNotReported ? std::string("--")
                                                             : std::to_string(altitudeFeet));
  ageLines().push_back(updatedAt == nullptr ? std::string() : std::string(updatedAt));
}

void aircraftLogoZone(int16_t& x, int16_t& y, int16_t& w, int16_t& h) {
  x = 0;
  y = 0;
  w = 0;
  h = 0;
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
  aPlaneWhoseTimeIsUpKeepsItsTurn();
  aPlaneIsHeldThroughALongOutage();
  theFirstPlaneClearingTheFloorIsDrawn();
  theFloorsOwnBoundary();
  aMilitaryPlaneClearingTheFloorOutranksTheRest();
  everyPlaneUnderTheFloorTakesTheCardOutOfTheRotation();
  aPlaneWithNoLimitClearsTheFloorAndStays();
  anAltitudeNobodyReportedDrawsADash();
  anUnknownCallsignFallsBackToTheRegistration();
  theLastLineStatesThePlanesAge();
  theServerSetsTheFetchCadence();
  noScheduleKeepsTheFixedInterval();
  anUnansweredFetchGivesUpTheScheduleAfterTwoTries();
  nothingWasEverDrawnWithoutAPlane();

  if (gFailures != 0) {
    std::printf("Failed: %d check(s) over the aircraft card's place in the rotation\n", gFailures);
    return 1;
  }

  std::printf("PASS: aircraft card takes no turn without a plane, %d cases, %d checks\n", gCases,
              gChecks);
  return 0;
}
