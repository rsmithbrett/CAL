#include "Aircraft.h"

#include <math.h>
#include <ArduinoJson.h>

#include <utility>  // std::move, for handing the fetched result to gLast without copying it

#include "Assets.h"
#include "Cards.h"
#include "Config.h"
#include "Display.h"
#include "Http.h"
#include "Identity.h"
#include "Log.h"
#include "ProviderStatus.h"

namespace Aircraft {
namespace {

constexpr const char* kPath = "/api/myaircraft/mine";

// The shortest schedule this card will take from the server. A figure below it
// is raised to it, so a misconfigured server cannot turn this card into a
// request loop: each fetch holds the one shared connection and can sit on a
// failing request for Config::kHttpTimeoutMs.
constexpr uint16_t kMinNextFetchSeconds = 30;

// The largest figure either of the two second counts can hold.
constexpr long kMaxSeconds = 65535;

// Identical shape and reasoning to Weather.cpp's parseRefusal - the same
// ContentProviderGate produces this 403 body for every device-facing content
// route, aircraft included (see ContentProviderGate.cs).
Result parseRefusal(const String& body) {
  Result result;
  result.status = Status::NetworkError;

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    result.message = "Cannot reach the aircraft service.";
    result.serviceUnreachable = true;
    return result;
  }

  const char* reason = doc["reason"] | "";
  const char* message = doc["message"] | "";
  result.message = strlen(message) > 0 ? String(message) : "This card is unavailable.";

  if (strcmp(reason, "device_not_activated") == 0) {
    result.status = Status::NotActivated;
  } else if (strcmp(reason, "content_provider_disabled") == 0) {
    result.status = Status::ProviderDisabled;
  }
  Log::printf("[aircraft] refused (%s): %s", reason, result.message.c_str());
  return result;
}

// How many seconds this plane is still worth showing. The server counts its own
// limit down against the age of the position and sends what is left; zero means
// the plane has had its time. An answer that carries no such field, which is
// every server older than the field, gives each plane kDefaultShowForSeconds
// instead, and so does a negative figure.
uint16_t secondsToShow(JsonVariantConst plane) {
  const int seconds = plane["showForSeconds"] | -1;
  if (seconds < 0) {
    return kDefaultShowForSeconds;
  }
  if (static_cast<long>(seconds) > kMaxSeconds) {
    return static_cast<uint16_t>(kMaxSeconds);
  }
  return static_cast<uint16_t>(seconds);
}

// How soon the server wants to be asked again, clamped to what the scheduler
// can act on. Zero is what an answer without the field reads as, and it leaves
// the card on the scheduler's fixed interval.
uint16_t scheduleFrom(int seconds) {
  if (seconds <= 0) {
    return 0;
  }
  if (seconds < kMinNextFetchSeconds) {
    return kMinNextFetchSeconds;
  }
  if (static_cast<long>(seconds) > kMaxSeconds) {
    return static_cast<uint16_t>(kMaxSeconds);
  }
  return static_cast<uint16_t>(seconds);
}

// The altitude as the log stream states it: a figure in feet, or that nobody
// reported one.
void describeAltitude(const Sighting& sighting, char* out, size_t size) {
  if (!sighting.altitudeReported) {
    snprintf(out, size, "not reported");
    return;
  }
  snprintf(out, size, "%dft", sighting.altitudeFeet);
}

}  // namespace

Result fetchMine() {
  Result result;

  if (!Http::ready()) {
    result.message = "Cannot verify the service's identity.";
    Log::line("[aircraft] TLS setup failed");
    return result;
  }

  const String url = String("https://") + Config::kServiceHost + kPath;
  if (!Http::beginRequest(url)) {
    result.message = "Cannot reach the aircraft service.";
    result.serviceUnreachable = true;
    Log::line("[aircraft] could not begin request");
    return result;
  }
  HTTPClient& http = Http::client();
  http.setTimeout(Config::kHttpTimeoutMs);
  http.addHeader("X-Device-Secret", Identity::deviceSecret());

  Log::verbose("[aircraft] GET %s", url.c_str());
  const int status = http.GET();
  Log::verbose("[aircraft] response status=%d", status);

  if (status == 401) {
    http.end();
    result.status = Status::AuthError;
    result.message = "This display is not signed in to an account. Set it up again from your account page.";
    Log::line("[aircraft] auth rejected (401)");
    return result;
  }

  if (status == 403) {
    const String body = http.getString();
    http.end();
    return parseRefusal(body);
  }

  // 404 is "we do not know where this display is", not a server that is down.
  // The route resolves a position from the display's owner, that owner's home
  // address, and the connecting address in turn, and answers 404 when all
  // three come to nothing - a display with no owner reaches this every time.
  // Named as its own state so the card stops reporting a working server as
  // unreachable, and so the sentence says the one thing somebody can act on.
  if (status == 404) {
    http.end();
    result.status = Status::NoPosition;
    result.message = "We do not know where this display is. Add a home address to the account that holds it.";
    Log::line("[aircraft] no position for this device (404)");
    return result;
  }

  // The service answered and could not help - it is reachable, so the card
  // declines to say anything about the sky rather than blaming the network.
  // Below 500 that is a refusal we have no better name for; at 500 and above
  // it is the server's own fault, and either way the reader can only wait.
  if (status >= 429) {
    http.end();
    result.status = Status::RefreshFailed;
    result.message = "The flight service is busy. This will catch up on its own.";
    Log::printf("[aircraft] service declined, http status=%d", status);
    return result;
  }

  if (status != 200) {
    http.end();
    result.message = "Cannot reach the aircraft service.";
    result.serviceUnreachable = true;
    Log::printf("[aircraft] unexpected http status=%d", status);
    return result;
  }

  // Only the fields this card actually draws are worth keeping in the filter
  // - same rationale as CYD-Dickey's Aircraft.cpp filtering adsb.lol's
  // couple-dozen raw fields down to five, just applied to this server's own
  // AircraftSighting shape. originName/destinationName are now whitelisted
  // alongside their codes - see Display::showAircraftCard()'s remarks for
  // why the card draws a name when one is on file and only falls back to the
  // bare code per side when it isn't.
  // Built once, on first fetch, and reused for the life of the device.
  //
  // A JsonDocument takes a pool block from the heap the moment it holds
  // anything - 1KB on this 32-bit target, ARDUINOJSON_POOL_CAPACITY being 128
  // slots - and this one was built, allocated and freed on every single fetch
  // to hold nothing but a handful of booleans. The keys are string literals,
  // so ArduinoJson links to them rather than copying, and the shape never
  // varies: this document has no per-fetch input at all. Keeping it resident
  // trades ~1KB of permanent heap for zero allocation churn on a device whose
  // scarce resource is contiguous blocks rather than total bytes - the same
  // trade Display.cpp's gFileBuffer and PNG decode scratch already make, for
  // the same measured reason.
  //
  // Function-local static, so it is constructed on first use rather than
  // during static init where the heap is in no state to be relied on.
  //
  // The filter is the read itself, not a note about it. A key missing from it
  // is dropped during deserialization and never reaches `doc`, so the parse
  // below cannot tell that key apart from one the server never sent and takes
  // its fallback every time. Adding a field to this card means adding it here
  // in the same edit.
  static const JsonDocument filter = [] {
    JsonDocument f;
    f["radiusMiles"] = true;
    f["notableDistanceMiles"] = true;
    // Top-level siblings of the array, not part of it, so each gets its own
    // entry. See the empty-list branch below for what `status` decides, and
    // cardFetch() for what the card does with the schedule.
    f["status"] = true;
    f["nextFetchSeconds"] = true;
    f["aircraft"][0]["callsign"] = true;
    f["aircraft"][0]["altitudeFeet"] = true;
    f["aircraft"][0]["speedKnots"] = true;
    f["aircraft"][0]["headingDegrees"] = true;
    f["aircraft"][0]["distanceMiles"] = true;
    f["aircraft"][0]["airlineCode"] = true;
    f["aircraft"][0]["airlineName"] = true;
    f["aircraft"][0]["airlineLogoAssetId"] = true;
    f["aircraft"][0]["originCode"] = true;
    f["aircraft"][0]["originName"] = true;
    f["aircraft"][0]["destinationCode"] = true;
    f["aircraft"][0]["destinationName"] = true;
    // The military four. Whitelisted like the rest: the filter keeps only the fields
    // this card draws, so a key missing from it is a key the parse below cannot see
    // however faithfully the server sends it.
    f["aircraft"][0]["isMilitary"] = true;
    f["aircraft"][0]["militaryBranch"] = true;
    f["aircraft"][0]["aircraftType"] = true;
    f["aircraft"][0]["registration"] = true;
    // How old the position is and how much of this plane's time is left. The
    // card counts the second one down itself between fetches and states the
    // first one on the glass.
    f["aircraft"][0]["positionAgeSeconds"] = true;
    f["aircraft"][0]["showForSeconds"] = true;
    return f;
  }();

  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    result.message = "The aircraft service sent something unreadable.";
    Log::line("[aircraft] response was not valid JSON");
    return result;
  }

  result.radiusMiles = doc["radiusMiles"] | 10.0;

  // Zero when the server does not send it, which cardIsNotable() reads as
  // "fall back to the old fifth-of-the-radius rule".
  result.notableDistanceMiles = doc["notableDistanceMiles"] | 0.0;

  // Read here rather than on the Ok path, so every answer the server gives -
  // an empty sky, a resting provider, a failed upstream refresh - carries the
  // schedule it wants to be asked on. Zero on an answer without the field, and
  // the card then stays on the scheduler's fixed interval.
  result.nextFetchSeconds = scheduleFrom(doc["nextFetchSeconds"] | 0);

  // THE SIGNAL, AND THE ONE FIELD THIS CARD READS TO GET IT. `ProviderStatus`
  // on the server - a closed vocabulary derived on the record, meant to be read
  // by a machine. See ProviderStatus.h for what each value means and why a card
  // reads this rather than the operator's sentence sitting next to it.
  //
  // WHY THERE IS NO `lastRefreshError` FALLBACK HERE, unlike Listings.cpp.
  // That card reads the old field's PRESENCE when `status` is absent, because
  // it already had correct behaviour built on that inference and deleting it
  // would regress a shipped card against a server predating the strip. This
  // card never read the field at all - it was on the wire and was never
  // whitelisted - so there is no behaviour to preserve, and adding the read now
  // would newly teach a card to treat an operator's prose as a protocol
  // element, in the very change that exists to stop doing that. It would also
  // pull that prose into the parsed document on a card that has never had it
  // there, which is leak surface bought for a purely transitional benefit.
  //
  // The consequence is stated plainly rather than buried: against a pre-strip
  // server this card behaves exactly as it does today, empty-sky claim and all.
  // It becomes correct the moment the server sends `status`, and draws nothing
  // worse in the meantime.
  const char* statusText = doc["status"] | "";
  const ProviderStatus::Value wire = ProviderStatus::parse(statusText);

  // The one derived fact. Everything downstream reads this boolean.
  bool refreshFailed = false;
  switch (wire) {
    case ProviderStatus::Value::Ok:
      refreshFailed = false;
      break;
    // "Nothing was attempted" is not "an attempt failed". The branch below
    // rests on NotConfigured before the aircraft array is ever consulted, so
    // this value never reaches a claim about the sky either way.
    case ProviderStatus::Value::NotConfigured:
      refreshFailed = false;
      break;
    case ProviderStatus::Value::Stale:
    case ProviderStatus::Value::Unavailable:
    case ProviderStatus::Value::Unrecognized:
      refreshFailed = true;
      break;
    // No status on this payload and, per the reasoning above, nothing else to
    // consult. Reads as "the refresh was fine", which is this card's behaviour
    // today and the only reading that does not start calling a genuinely empty
    // sky a failure on every server currently deployed.
    case ProviderStatus::Value::Absent:
      refreshFailed = false;
      break;
  }

  Log::verbose("[aircraft] status='%s' - %s", strlen(statusText) > 0 ? statusText : "(absent)",
               ProviderStatus::describe(wire));

  // A resting state, checked before the array is ever looked at - an account
  // with no provider on file should read as "not set up", never as "nothing
  // overhead". This card has no `isConfigured` of its own, so `status` is the
  // only thing that can say so.
  if (wire == ProviderStatus::Value::NotConfigured) {
    result.status = Status::NotConfigured;
    // A literal chosen for whoever this panel hangs in front of, never the
    // server's own words - the same contract Result::message carries on every
    // other card, and the one the listings card had to be taught after it put
    // a vendor's sign-up instructions on a kitchen wall.
    result.message = "Aircraft tracking is not set up for this home yet.";
    Log::printf("[aircraft] NOT CONFIGURED - resting; neither an empty sky nor a failed refresh, "
                "and the aircraft array was not consulted");
    return result;
  }

  JsonArrayConst aircraft = doc["aircraft"].as<JsonArrayConst>();
  if (aircraft.isNull() || aircraft.size() == 0) {
    // AN EMPTY LIST IS NOT ONE FACT, IT IS TWO - identical in shape to the
    // listings card's own empty-array split, and worse here in one respect: an
    // empty sky is PLAUSIBLE far more often than an empty housing market, so
    // the false version of this claim is much less likely to be questioned by
    // whoever reads it. "No aircraft within 10 mi right now" is a flat
    // statement about the sky, and until now this card made it whether or not
    // anybody had actually looked.
    //
    // Deliberately NOT serviceUnreachable. That flag means "this device could
    // not reach OUR server", which is what a declared maintenance window
    // explains; this is our server answering perfectly well about an upstream
    // feed it could not reach. None of the ProviderStatus values means our
    // server is unreachable - every one arrives on a well-formed 200 - so
    // nothing read out of that field may ever raise it. cardFetch() therefore
    // drops a plane it holds when this result arrives.
    if (refreshFailed) {
      result.status = Status::RefreshFailed;
      // Names the cause. No claim about the sky, and no count implied.
      result.message = "The flight data service is not answering right now.";
      Log::printf("[aircraft] empty list AND a failed refresh -> RefreshFailed; Empty NOT taken - "
                  "nothing here licenses a claim about the sky. serviceUnreachable stays false: "
                  "our server answered, the upstream feed did not");
      return result;
    }
    result.status = Status::Empty;
    char buffer[48];
    snprintf(buffer, sizeof(buffer), "No aircraft within %.0f mi right now.", result.radiusMiles);
    result.message = String(buffer);
    // The one branch on this card that makes a positive claim about the sky,
    // and until now the only one that logged nothing at all. It says so now,
    // and says what it relied on to be allowed to.
    Log::printf("[aircraft] empty list and a refresh not reported as failed -> Empty; "
                "RefreshFailed NOT taken - a real statement about the sky within %.0f mi",
                result.radiusMiles);
    return result;
  }

  // Sightings AND a failed refresh - `status: "Stale"`, the value that case
  // exists to name. The server is serving last-known-good rows while its
  // refresh fails behind them. Drawing real sightings beats drawing a warning,
  // exactly as the listings card decides for its own cached rows, so the screen
  // is left alone and only the stream is told.
  if (refreshFailed) {
    Log::printf("[aircraft] serving %u cached sighting(s) behind a failed refresh - drawing them "
                "rather than a warning, and NOT taking RefreshFailed",
                static_cast<unsigned>(aircraft.size()));
  }

  // The list arrives in distance order, and a military sighting outranks the
  // rest: the server keeps a place for the nearest military aircraft even when
  // the eight-aircraft cap would have dropped it, which is the whole reason one
  // ever reaches this device. This card features one plane at a time, and an
  // ordinary airliner is what it features every other minute of the day.
  //
  // A plane with no time left on it is passed over, so a list whose nearest
  // entry has had its time still draws the next one that has time left, and a
  // military plane outranks the others only while it has time of its own.
  JsonVariantConst plane = aircraft[0];
  uint16_t showForSeconds = 0;
  for (JsonVariantConst candidate : aircraft) {
    const uint16_t seconds = secondsToShow(candidate);
    if (seconds == 0) {
      continue;
    }
    if (showForSeconds == 0) {
      plane = candidate;
      showForSeconds = seconds;
    }
    if (candidate["isMilitary"].as<bool>()) {
      plane = candidate;
      showForSeconds = seconds;
      break;
    }
  }

  // Every plane on the list has had its time. Nothing here says the sky is
  // empty and nothing failed: the card takes no turn until a fetch brings back
  // a plane with time left on it.
  if (showForSeconds == 0) {
    result.status = Status::Expired;
    result.message = "No aircraft reported recently enough to show.";
    Log::printf("[aircraft] %u sighting(s) and none with time left -> Expired; the card takes no "
                "turn until a fetch returns one",
                static_cast<unsigned>(aircraft.size()));
    return result;
  }

  result.status = Status::Ok;
  result.nearest.showForSeconds = showForSeconds;

  // "UNKNOWN" is what the server sends where a military aircraft flies without
  // a flight id, and it reads exactly like a callsign where SCWTP33 sits on the
  // card. Empty instead, so the card falls back to the registration it already
  // has, or draws nothing, the way it does for every other missing string.
  const char* callsignText = plane["callsign"] | "";
  if (strcmp(callsignText, "UNKNOWN") == 0) {
    callsignText = "";
  }
  result.nearest.callsign = String(callsignText);

  // Null or absent means nobody reported an altitude. Zero feet is sea level,
  // so the flag carries the difference and the card draws "--" without it.
  result.nearest.altitudeReported = plane["altitudeFeet"].is<double>();
  result.nearest.altitudeFeet = result.nearest.altitudeReported ? (plane["altitudeFeet"] | 0) : 0;

  // How old the position was when the server answered. -1 for an answer that
  // carries no such field, and the last line on the card then dates the answer
  // rather than the position.
  const int positionAge = plane["positionAgeSeconds"] | -1;
  result.nearest.positionAgeSeconds = positionAge < 0 ? -1 : positionAge;

  // NaN rather than zero, because zero knots is a parked aeroplane and zero
  // degrees is due north. The server writes null for a figure nobody reported,
  // and NaN is the one double that fails every comparison, so
  // compassDirection()'s own [0,360) guard already refuses it and draws the
  // dash it was written to draw. Only speed needed a check of its own.
  result.nearest.speedKnots =
      plane["speedKnots"].is<double>() ? plane["speedKnots"].as<double>() : NAN;
  result.nearest.headingDegrees =
      plane["headingDegrees"].is<double>() ? plane["headingDegrees"].as<double>() : NAN;
  result.nearest.distanceMiles = plane["distanceMiles"] | 0.0;
  // `| ""` reads a JSON null exactly the same as a field an older server
  // never sends at all - both mean "nothing here" to this client, and the
  // contract deliberately writes null rather than omitting the key, so both
  // shapes have to land on the same empty String regardless.
  result.nearest.airlineCode = String((const char*)(plane["airlineCode"] | ""));
  result.nearest.airlineName = String((const char*)(plane["airlineName"] | ""));
  result.nearest.airlineLogoAssetId = String((const char*)(plane["airlineLogoAssetId"] | ""));
  result.nearest.originCode = String((const char*)(plane["originCode"] | ""));
  result.nearest.originName = String((const char*)(plane["originName"] | ""));
  result.nearest.destinationCode = String((const char*)(plane["destinationCode"] | ""));
  result.nearest.destinationName = String((const char*)(plane["destinationName"] | ""));
  // Absent on every civil aircraft and on every server old enough to predate the fields,
  // which land on the same empty String for the reason above. The flag reads false in
  // both cases, which is what "not a military sighting" means to this card.
  result.nearest.isMilitary = plane["isMilitary"].as<bool>();
  result.nearest.militaryBranch = String((const char*)(plane["militaryBranch"] | ""));
  result.nearest.aircraftType = String((const char*)(plane["aircraftType"] | ""));
  result.nearest.registration = String((const char*)(plane["registration"] | ""));

  // A lightly-summarized response rather than the raw body - same filtering
  // reasoning as Forecast::fetch()'s own verbose line: this endpoint's
  // JsonDocument filter above already exists to keep only the fields this
  // card draws in memory, and buffering the raw body just to log it would
  // undo that.
  char altitudeText[16];
  describeAltitude(result.nearest, altitudeText, sizeof(altitudeText));
  Log::verbose("[aircraft] response radiusMiles=%.0f callsign=%s alt=%s dist=%.1fmi showFor=%us "
               "positionAge=%lds nextFetch=%us",
               result.radiusMiles,
               result.nearest.callsign.length() > 0 ? result.nearest.callsign.c_str() : "(none)",
               altitudeText, result.nearest.distanceMiles,
               static_cast<unsigned>(result.nearest.showForSeconds),
               static_cast<long>(result.nearest.positionAgeSeconds),
               static_cast<unsigned>(result.nextFetchSeconds));

  return result;
}

// ---------------------------------------------------------------------------
// The card descriptor. See the equivalent block at the bottom of Weather.cpp
// for why registration happens here rather than in App.ino.
//
// Registered as a list card that holds one plane at a time. MyAircraftService
// returns a distance-sorted list and fetchMine() keeps one entry off it, so
// cardItemCount() reports one while a plane is held and zero otherwise. A
// later change that keeps more than one needs cardItemCount() and cardDraw()
// to start reading an index, and nothing in the scheduler to change at all.
// ---------------------------------------------------------------------------
namespace {

Result gLast;
bool gEverFetched = false;

/// The logo's bytes when this device has no SD cache to hold them, and the id
/// they belong to.
///
/// A card-less device reaches the logo the way Graphic.cpp's cards already
/// reach their pictures: ensureCached() answers no, and the fetch path falls
/// back to Assets::fetchToRam(). This card needs only one of these rather than
/// Graphic.cpp's per-instance buffers, because there is one logo on screen at a
/// time and the aircraft card is a single descriptor - but it is a file-scope
/// static for the same reason theirs are, so the bytes survive from the fetch
/// that filled them to however many draws follow before the next refresh.
///
/// gLogoRamId is what the buffer actually holds, not what gLast wants. The two
/// diverge on the draw after a refresh that found a different airline: the
/// buffer still has the previous logo until the new fetch replaces it, and
/// drawing it then would put the wrong carrier's mark on the card.
Assets::RamAssetBuffer gLogoRam;
String gLogoRamId;

/// Set when the bytes in gLogoRam reached the decoder and no picture came out,
/// so the card stops asking for them.
///
/// Without it a logo that will not draw costs three attempts on every
/// appearance of this card, forever, and each one counts toward the
/// consecutive-draw-failure watchdog that restarts the device at seven. A
/// card-less device is the one that cannot recover from that on its own:
/// Assets.cpp's re-fetch-once rule reads the SD cache to decide and so
/// declines for a device that has none, and on a card-less device with no
/// picture cards this draw is the only thing in the binary that moves the
/// counter in either direction - so nothing would ever reset it either. One
/// attempt per set of bytes is the bargain Graphic.cpp strikes when it drops a
/// card whose picture will not decode.
///
/// Cleared when a fetch puts different bytes in the buffer, so a logo the
/// server re-encodes gets a fresh attempt rather than being written off for
/// the uptime.
bool gLogoRamUndrawable = false;

/// How many sets of bytes may fail to draw before this card stops trying for
/// the rest of the boot, and how many have.
///
/// **The mark above does not bound anything on its own**, which is the whole
/// reason this counter exists. It is keyed to what the buffer holds, and three
/// ordinary events empty or replace the buffer: a different airline overhead,
/// a fetch that fails partway, and Aircraft::releaseRamBuffer() on the
/// check-in failure path. Each of those clears the mark and buys three more
/// failed attempts - and the last is the worst of the three, because a device
/// short of contiguous heap is both the device whose check-ins fail and the
/// device whose draws refuse, so the two re-arm each other. Two airlines
/// overhead in half an hour reached seven failures and a restart with no fault
/// anywhere but this.
///
/// Two episodes of three attempts is six, which is what App.ino's limit
/// tolerates (it restarts past six, not at it). Never reset: a boot that has
/// already asked this question twice has its answer.
constexpr uint8_t kMaxLogoRamDrawFailures = 2;
uint8_t gLogoRamDrawFailures = 0;

/// Which logo a network fetch to RAM was last attempted for, as a hash, and
/// when - so one id costs at most one attempt per Config::kContentRefreshIntervalMs
/// however that attempt went.
///
/// A display with no SD card has nowhere to keep a logo, so the fetch path asks
/// the network for it on every refresh. That was one request per ten minutes
/// until this card started taking the server's own schedule, which is a minute
/// on today's server, and a few thousand bytes allocated and freed ten times as
/// often is exactly the churn that carves a heap into unusable holes. A
/// different airline overhead hashes differently and is fetched at once, which
/// is the case that matters.
///
/// A hash rather than the id, so this costs eight bytes rather than a String
/// whose buffer moves.
uint32_t gLogoRamAttemptHash = 0;
unsigned long gLogoRamAttemptMs = 0;

/// FNV-1a over an asset id. Collisions cost one skipped logo fetch for up to a
/// refresh interval, which is why a hash is enough here.
uint32_t hashOf(const String& text) {
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < text.length(); ++i) {
    hash ^= static_cast<uint8_t>(text[i]);
    hash *= 16777619UL;
  }
  return hash;
}

/// millis() when gLast last became an Ok result - same field, same reasoning,
/// same fix as Weather.cpp's gLastOkMs: "Updated just now" was previously
/// hardcoded on every draw, including a redraw of a sighting fetched minutes
/// earlier and reverse navigation into card history, where it was false by
/// construction.
unsigned long gLastOkMs = 0;

/// How many fetches in a row got no answer, and how many of those keep the
/// server's own schedule before the card falls back to the scheduler's fixed
/// interval. A server that has stopped answering should not be asked every
/// minute for the rest of the uptime: each attempt holds the one shared
/// connection and can sit on a dead socket for Config::kHttpTimeoutMs.
constexpr uint8_t kUnansweredBeforeSlowdown = 2;
uint8_t gUnansweredInARow = 0;

/// Unsigned subtraction, correct across the millis() rollover at ~49 days -
/// identical to Weather.cpp's describeFreshness(), duplicated rather than
/// shared because the two cards' Result types are unrelated and a shared
/// helper would need a third file just to hold one function used twice.
String describeFreshness(unsigned long fetchedAtMs) {
  const unsigned long ageMinutes = (millis() - fetchedAtMs) / 60000UL;
  if (ageMinutes == 0) {
    return "Updated just now";
  }
  if (ageMinutes == 1) {
    return "Updated 1 min ago";
  }
  return String("Updated ") + ageMinutes + " min ago";
}

/// Whether a plane is held and still has time left on it.
///
/// Each plane arrives with the seconds the server has left on it, counted from
/// the answer this card read it out of, and that count runs down here between
/// fetches - including through fetches that got no answer, which keep the plane
/// rather than replacing it. At zero the status becomes Expired and stays there
/// until a fetch returns a plane, so the countdown is read once and cannot
/// un-read itself as millis() rolls over.
///
/// Everything that asks whether this card has anything to show goes through
/// here: the item count the scheduler reads, the draw, the press label, the
/// listing key and the status line.
bool holdsPlane() {
  if (gLast.status != Status::Ok) {
    return false;
  }
  if ((millis() - gLastOkMs) / 1000UL <
      static_cast<unsigned long>(gLast.nearest.showForSeconds)) {
    return true;
  }
  gLast.status = Status::Expired;
  Log::printf("[aircraft] the plane held has had its %u seconds - the card takes no turn until a "
              "fetch returns one",
              static_cast<unsigned>(gLast.nearest.showForSeconds));
  return false;
}

/// What the plane held is called in the log stream: the flight, the tail number
/// where no flight id arrived, or neither.
String heldIdentifier() {
  if (gLast.nearest.callsign.length() > 0) {
    return gLast.nearest.callsign;
  }
  if (gLast.nearest.registration.length() > 0) {
    return gLast.nearest.registration;
  }
  return "an aircraft";
}

/// The last line on the card: how old the fact above it is.
///
/// With a positionAgeSeconds the line dates the position - what the server
/// measured, plus the time since this device read the answer - because the
/// altitude, speed, heading and distance above it all describe that position.
/// Without one it dates the answer instead, counting from the last fetch that
/// returned a plane, which is what this card has always drawn.
///
/// Writes into the caller's buffer rather than returning a String, because this
/// runs on every draw.
void describeAge(char* out, size_t size) {
  const unsigned long sinceFetch = (millis() - gLastOkMs) / 1000UL;
  if (gLast.nearest.positionAgeSeconds < 0) {
    const unsigned long minutes = sinceFetch / 60UL;
    if (minutes == 0) {
      snprintf(out, size, "Updated just now");
    } else if (minutes == 1) {
      snprintf(out, size, "Updated 1 min ago");
    } else {
      snprintf(out, size, "Updated %lu min ago", minutes);
    }
    return;
  }
  const unsigned long seconds =
      static_cast<unsigned long>(gLast.nearest.positionAgeSeconds) + sinceFetch;
  if (seconds == 0) {
    snprintf(out, size, "Reported just now");
  } else if (seconds < 60UL) {
    snprintf(out, size, "Reported %lu sec ago", seconds);
  } else if (seconds < 120UL) {
    snprintf(out, size, "Reported 1 min ago");
  } else {
    snprintf(out, size, "Reported %lu min ago", seconds / 60UL);
  }
}

/// Hands the server's schedule to the scheduler, which asks this card again
/// that many seconds after the fetch that carried it. Zero leaves the card on
/// the scheduler's fixed interval, which is what every answer without the field
/// reads as.
void applySchedule(uint16_t seconds) {
  const int8_t index = Cards::indexOf("aircraft");
  if (index < 0) {
    return;
  }
  Cards::at(static_cast<uint8_t>(index)).refreshSeconds = seconds;
}

/// This card's current fetch state, re-asserted to the debug stream once per
/// check-in - see Cards.h's StatusFn for why change-only logging was not
/// enough. Names the refusal reason rather than just "not ok": "which of the
/// six ways can this be failing" is the entire question an admin is asking.
String cardStatus() {
  if (!gEverFetched) {
    return "never fetched";
  }
  // Asked first, because asking is what runs the countdown down: a plane that
  // has had its time reads as Expired on this line rather than at the next
  // fetch.
  if (holdsPlane()) {
    return String("ok, showing ") + heldIdentifier() + " at " +
           String(gLast.nearest.distanceMiles, 1) + " mi";
  }
  switch (gLast.status) {
    case Status::Ok:
      // holdsPlane() answered no, so this is no longer the status.
      break;
    case Status::Empty:
      return String("ok, nothing within ") + String(gLast.radiusMiles, 0) + " mi";
    // The sky was reported and what came back was too old to show. An answer,
    // like Empty, and not a claim that the sky is clear.
    case Status::Expired:
      return "ok, no plane with time left to show";
    // Distinct from Empty on purpose, and this line is where an admin sees the
    // difference: "nothing within 10 mi" is an answer, "could not check" is the
    // absence of one. Carries no reason - that is an operator diagnostic and
    // lives on the server's own /diag/providers, never on a device. See
    // Listings::Result::message for the full argument.
    case Status::RefreshFailed:
      return "upstream refresh failed (reason is on the server, not the device)";
    case Status::NotConfigured:
      return "resting: no aircraft provider on file";
    case Status::NoPosition:
      return "resting: no position for this device";
    case Status::NotActivated:
      return "refused: device not activated";
    case Status::ProviderDisabled:
      return "refused: provider disabled";
    case Status::AuthError:
      return "refused: device secret rejected";
    case Status::NetworkError:
      return String("network error: ") + gLast.message;
  }
  return "unknown";
}

void cardFetch() {
  // A fetch that could not reach the service does not erase what is on the
  // glass. gLast holds the last result and gLastOkMs the instant it was last
  // Ok, and the card's own last line has been dating that answer all along, so
  // keeping it is a matter of not throwing it away.
  //
  // Only for serviceUnreachable, and that distinction is the whole safety of
  // it. The flag means the request never got an answer - no TLS session, no
  // route, a timeout - so the last answer is still the best account of the
  // world available. Every other failure is the server answering: a refusal, a
  // disabled provider, a device not activated, an empty sky. Those are real
  // changes of state, and holding content over them would keep a card on screen
  // after an operator turned it off. Those replace the cache.
  //
  // The plane kept this way keeps its own countdown. It was given its seconds
  // by the answer it arrived on, gLastOkMs still marks that answer, and
  // holdsPlane() measures against it - so a server that stops answering costs
  // the card its turn as soon as the plane it is holding runs out, not when the
  // next answer finally arrives.
  Result fresh = fetchMine();
  gEverFetched = true;

  // What the server wants to be asked on, handed to the scheduler whether or
  // not this answer carried a plane. An unanswered request keeps the schedule
  // for a couple of tries and then gives it up, so a server that has gone away
  // is asked on the fixed interval rather than every minute.
  if (fresh.serviceUnreachable) {
    if (gUnansweredInARow < kUnansweredBeforeSlowdown) {
      ++gUnansweredInARow;
    } else {
      applySchedule(0);
    }
  } else {
    gUnansweredInARow = 0;
    applySchedule(fresh.nextFetchSeconds);
  }

  if (fresh.status != Status::Ok && fresh.serviceUnreachable && holdsPlane()) {
    Log::printf("[aircraft] could not reach the service - keeping %s and the card's turn in the "
                "rotation (%s)",
                heldIdentifier().c_str(), describeFreshness(gLastOkMs).c_str());
    return;
  }

  // Moved, not copied. The keep-the-cache check above needs a named local, and
  // assigning one without the move deep-copies every String in the result. On a
  // display whose failure mode is contiguous heap, that is allocation churn for
  // nothing.
  gLast = std::move(fresh);
  if (gLast.status == Status::Ok) {
    gLastOkMs = millis();

    // Belongs on the fetch path, not draw: ensureCached() fetches on a miss,
    // and Cards.h's whole fetch/draw split exists so stepping backwards
    // through the rotation is never a network operation - see Graphic.cpp's
    // own remarks on the same split. A blank airlineLogoAssetId is the
    // ordinary case (no logo on file for this row yet) and skips the call
    // entirely rather than asking Assets to cache an empty id.
    if (gLast.nearest.airlineLogoAssetId.length() > 0) {
      const String& logoId = gLast.nearest.airlineLogoAssetId;
      const bool logoReady = Assets::ensureCached(logoId);
      Log::printf("[aircraft] logo %s for '%s': %s", logoReady ? "cached" : "unavailable",
                  gLast.nearest.airlineCode.c_str(), logoId.c_str());

      if (logoReady) {
        // An SD cache answered, so anything the buffer is holding is a logo
        // this device no longer needs - hand it back rather than carrying it
        // for the uptime. Assets.h's releaseRamBuffer() sets out why a held
        // buffer is worth reclaiming: a new TLS session needs 16,717 bytes
        // contiguous twice, and the blocks this fleet reports are not large
        // enough for both once a picture is sitting in one of them.
        if (gLogoRam.data != nullptr) {
          Assets::releaseRamBuffer(gLogoRam);
          gLogoRamId = "";
          gLogoRamUndrawable = false;
        }
      } else {
        // The SD path answered no. On the two card-less displays in this fleet
        // that is permanent and no retry of the SD side will change it, so the
        // bytes come straight to RAM over the network this fetch already
        // reached the server on - the same fallback Graphic.cpp's cards have
        // had since fetchToRam() arrived, which the logo was never wired to.
        //
        // Re-fetched rather than kept once it is in hand, which is what
        // Graphic.cpp's fetch() does with its own pictures and for the same
        // reason: a RAM buffer is the one cache nothing can invalidate.
        // ensureCached() answers "already here" on the file existing, so a
        // device with a card cannot notice the server re-encoding an asset
        // either - but that device at least has a cached file for Assets.cpp to
        // drop. Holding these bytes for the uptime would make a card-less
        // device the only one a re-encode could never reach short of a restart.
        //
        // Once per refresh interval per id, not once per fetch, now that this
        // card can be asked as often as the server says - see
        // gLogoRamAttemptMs. A logo is a few thousand bytes against a picture
        // card's ten to twenty, and the picture cards are still on the
        // ten-minute interval.
        const uint32_t logoHash = hashOf(logoId);
        const bool askedRecently =
            gLogoRamAttemptMs != 0 && gLogoRamAttemptHash == logoHash &&
            (millis() - gLogoRamAttemptMs) < Config::kContentRefreshIntervalMs;
        if (askedRecently) {
          Log::verbose("[aircraft] logo '%s' was asked for over the network within the refresh "
                       "interval - not asking again this fetch",
                       logoId.c_str());
        } else {
          gLogoRamAttemptHash = logoHash;
          gLogoRamAttemptMs = millis();
          Log::printf("[aircraft] no SD cache for logo '%s' - fetching it straight to RAM",
                      logoId.c_str());
          const size_t heldBefore = gLogoRam.size;
          if (Assets::fetchToRam(logoId, gLogoRam)) {
            // A different id, or the same id with a different number of bytes,
            // means these are not the bytes that failed to draw last time. Size
            // is a weak test for "different" and it is the only one available
            // here; the alternative is writing off a re-encoded logo until the
            // device restarts.
            //
            // Bounded by kMaxLogoRamDrawFailures, because the test above is
            // keyed to the buffer and the buffer is emptied by things that are
            // not an answer to "will these bytes draw" - see that constant.
            if ((gLogoRamId != logoId || gLogoRam.size != heldBefore) &&
                gLogoRamDrawFailures < kMaxLogoRamDrawFailures) {
              gLogoRamUndrawable = false;
            }
            gLogoRamId = logoId;
            Log::printf("[aircraft] logo for '%s' is showing straight from RAM (%u bytes) - "
                        "no SD card available",
                        gLast.nearest.airlineCode.c_str(), static_cast<unsigned>(gLogoRam.size));
          } else {
            // fetchToRam() can leave a partial write behind, so what the buffer
            // holds is no longer any id's bytes - see Assets.h on why the caller
            // has to forget that itself.
            gLogoRamId = "";
            if (gLogoRamDrawFailures < kMaxLogoRamDrawFailures) {
              gLogoRamUndrawable = false;
            }
            Log::printf("[aircraft] logo '%s' could not be fetched to RAM either - no logo this "
                        "cycle",
                        logoId.c_str());
          }
        }
      }
    }

    // Logged the same way it's drawn: per-side name-with-code-fallback, not
    // an all-or-nothing switch, so this line tells you exactly what
    // showAircraftCard() is about to put on screen rather than a summary
    // that could disagree with it. See that function's own remarks for why
    // the fallback is per-side.
    const String originSummary =
        gLast.nearest.originName.length() > 0 ? gLast.nearest.originName : gLast.nearest.originCode;
    const String destinationSummary = gLast.nearest.destinationName.length() > 0
        ? gLast.nearest.destinationName
        : gLast.nearest.destinationCode;
    String routeSummary = "none on file";
    if (originSummary.length() > 0) {
      routeSummary = destinationSummary.length() > 0 ? (originSummary + "->" + destinationSummary)
                                                       : ("from " + originSummary + " only");
    }
    char altitudeText[16];
    describeAltitude(gLast.nearest, altitudeText, sizeof(altitudeText));
    Log::printf(
        "[aircraft] card updated: %s (%s) alt=%s speed=%.0fkts heading=%.0f dist=%.1fmi route=%s "
        "showing for %us",
        heldIdentifier().c_str(),
        gLast.nearest.airlineName.length() > 0 ? gLast.nearest.airlineName.c_str() : "no airline match",
        altitudeText, gLast.nearest.speedKnots, gLast.nearest.headingDegrees,
        gLast.nearest.distanceMiles, routeSummary.c_str(),
        static_cast<unsigned>(gLast.nearest.showForSeconds));
  } else {
    // Printed rather than verbose: the stream is the only place a deployed
    // display says why its aircraft card is missing from the rotation, and
    // cardStatus() names which of the statuses it is.
    Log::printf("[aircraft] no plane held (%s) - the card takes no turn until a fetch "
                "returns one",
                cardStatus().c_str());
  }
}

/// One item while a plane is held and has time left on it, and none at any
/// other time: before the first fetch, over a clear sky, for every refusal and
/// failure, and once the plane held has had its seconds. The scheduler passes
/// over a card reporting zero, which is what keeps "nothing overhead" from
/// taking a turn in the rotation.
uint16_t cardItemCount() { return holdsPlane() ? 1 : 0; }

/// A plane nearly directly overhead earns the longer dwell.
///
/// The server says how close that is, in notableDistanceMiles. It used to be
/// derived here as a fifth of the search radius - CYD-Dickey's rule - and that
/// made one number answer two questions. On 2026-10-23 the server's radius
/// went from 10 miles to 25 for an unrelated reason, this threshold moved from
/// 2 miles to 5 with it, and the card started taking the twenty-second dwell
/// far more often on every display, which lengthened the whole rotation. How
/// far to look and how close is remarkable are separate questions.
///
/// The old rule is the fallback, so a device talking to a server that does not
/// send the field behaves exactly as it did. The 1-mile floor stays on both
/// paths: a very small value must not make every sighting notable.
bool cardIsNotable(uint16_t) {
  if (!holdsPlane()) {
    return false;
  }
  const double notable = gLast.notableDistanceMiles > 0
                             ? gLast.notableDistanceMiles
                             : gLast.radiusMiles * 0.2;
  return gLast.nearest.distanceMiles <= max(1.0, notable);
}

void cardDraw(uint16_t) {
  if (holdsPlane()) {
    // What is actually on screen this draw, not just what the last fetch
    // found - the two can diverge across a rewind, where this runs again
    // with no fresh fetch behind it. cardFetch()'s own printf() summary
    // above only fires on a successful fetch, not on every draw.
    char altitudeText[16];
    describeAltitude(gLast.nearest, altitudeText, sizeof(altitudeText));
    Log::verbose(
        "[aircraft] drawing: %s alt=%s speed=%.0fkts heading=%.0f dist=%.1fmi logo=%s",
        heldIdentifier().c_str(), altitudeText, gLast.nearest.speedKnots,
        gLast.nearest.headingDegrees, gLast.nearest.distanceMiles,
        gLast.nearest.airlineLogoAssetId.length() > 0 ? gLast.nearest.airlineLogoAssetId.c_str()
                                                        : "(none)");
    // THE OPERATOR'S NAME SLOT CARRIES THE SERVICE, because that is what it is for: the
    // line under the callsign says who is flying this aircraft, and for a military
    // sighting that is "U.S. Navy" rather than an airline. The airframe joins it when the
    // provider named one, since "U.S. Navy - MH-60 Seahawk" is the whole answer a
    // household wants and the slot already goes through layoutText, which bounds it.
    //
    // No new parameter and no geometry change. A military sighting has no filed route, so
    // the two airport lines are empty and the card has the room.
    // THE AIRFRAME IS THE HEADLINE, THE SERVICE SITS UNDER IT, AND BOTH SHOW.
    //
    // These two shared the headline as "Military - P-8 Poseidon" and the half
    // worth reading was cut off - photographed 2026-10-08. Putting the type on
    // the identifier line instead showed one or the other, never both. So the
    // type takes the headline, because what the aircraft IS is the more
    // interesting fact, and the service drops into the space a filed route
    // would occupy. A military sighting has no route, so that space is free.
    //
    // Where no type is known the service keeps the headline, which is the card
    // as it reads today for an aircraft hexdb has nothing on file for.
    String operatorName = gLast.nearest.airlineName;
    String noRouteLine;

    if (gLast.nearest.isMilitary && gLast.nearest.militaryBranch.length() > 0) {
      if (gLast.nearest.aircraftType.length() > 0) {
        operatorName = gLast.nearest.aircraftType;
        noRouteLine = gLast.nearest.militaryBranch;
      } else {
        operatorName = gLast.nearest.militaryBranch;
      }
    }

    // The flight, where one was filed, else the airframe. Military traffic
    // routinely files no callsign, which used to arrive as the literal
    // "UNKNOWN" and read like one. The type is no longer a fallback here: it
    // has the headline.
    String identifier = gLast.nearest.callsign;
    if (identifier.length() == 0) {
      identifier = gLast.nearest.registration;
    }

    // Whether a logo is going into the zone, decided before the card is drawn
    // because the headline's width depends on it. Either source counts: the SD
    // cache holding this id, or the RAM buffer holding bytes that have not
    // already failed to draw. Asking isCached() costs a stat and no network -
    // the same question drawCachedInRect() is about to ask itself.
    const bool hasLogo =
        gLast.nearest.airlineLogoAssetId.length() > 0 &&
        (Assets::isCached(gLast.nearest.airlineLogoAssetId) ||
         (gLogoRamId == gLast.nearest.airlineLogoAssetId && !gLogoRamUndrawable));

    // The last line, built into a stack buffer because this is the draw path.
    char ageLine[32];
    describeAge(ageLine, sizeof(ageLine));

    Display::showAircraftCard(identifier, operatorName,
                              gLast.nearest.altitudeReported
                                  ? gLast.nearest.altitudeFeet
                                  : Display::kAltitudeNotReported,
                              gLast.nearest.speedKnots,
                              gLast.nearest.headingDegrees, gLast.nearest.distanceMiles,
                              gLast.nearest.originCode, gLast.nearest.destinationCode,
                              gLast.nearest.originName, gLast.nearest.destinationName,
                              ageLine, noRouteLine, hasLogo);

    // Drawn after showAircraftCard(), not by it - same module boundary
    // Graphic.cpp already keeps with Display.cpp: whoever holds the asset id
    // draws the picture, Display.cpp only ever decides where things go (see
    // aircraftLogoZone()). Never fetches - this is the draw path, and a
    // logo that hasn't finished caching this cycle simply doesn't appear
    // this cycle rather than blocking the card on the network.
    if (gLast.nearest.airlineLogoAssetId.length() > 0) {
      int16_t x, y, w, h;
      Display::aircraftLogoZone(x, y, w, h);
      // The SD cache first, then the RAM buffer the fetch path fills when
      // there is no cache to read. Which one holds the bytes decides which
      // draw reads them: drawCachedInRect() opens a file, drawRamInRect()
      // reads the buffer, and using the wrong one for the wrong source is the
      // same mistake Graphic.cpp's own gFromRam flag exists to prevent.
      //
      // The id is compared, not just the pointer: a buffer left over from the
      // airline that was overhead last refresh would otherwise be drawn as
      // this one's.
      if (!Assets::drawCachedInRect(gLast.nearest.airlineLogoAssetId, x, y, w, h) &&
          gLogoRamId == gLast.nearest.airlineLogoAssetId && !gLogoRamUndrawable) {
        if (!Assets::drawRamInRect(gLast.nearest.airlineLogoAssetId, gLogoRam, x, y, w, h)) {
          // Asked once per set of bytes, and at most kMaxLogoRamDrawFailures
          // times a boot - see those two for why asking again on every
          // appearance of this card would restart the device rather than
          // eventually produce a picture.
          gLogoRamUndrawable = true;
          if (gLogoRamDrawFailures < kMaxLogoRamDrawFailures) {
            ++gLogoRamDrawFailures;
          }
          Log::printf("[aircraft] logo '%s' is in RAM and will not draw (%u of %u attempts this "
                      "boot) - leaving the space empty",
                      gLast.nearest.airlineLogoAssetId.c_str(),
                      static_cast<unsigned>(gLogoRamDrawFailures),
                      static_cast<unsigned>(kMaxLogoRamDrawFailures));
        }
      }
    }
    return;
  }

  // No plane is held. cardItemCount() reports zero then and the scheduler does
  // not draw a card reporting zero, so this is reached only by a caller that
  // did not ask first. Nothing is drawn and the panel stays as it was.
  Log::verbose("[aircraft] draw reached with no plane held (%s) - nothing drawn",
               cardStatus().c_str());
}

/// Which aircraft was overhead when somebody pressed - see Cards::DescribeFn.
///
/// The flight or the tail number, the route and the distance. The callsign
/// identifies the flight to anyone
/// who looks it up afterwards; the route is what makes it mean something to a
/// reader who will not. Distance is included here, unlike on the listings card,
/// because for an aircraft it is the point of the press - "there was a plane two
/// miles away" is the observation, where a listing's distance from the device is
/// incidental to the house.
///
/// Names fall back to codes per side independently, matching how the card itself
/// draws its route: a flight with one known airport name and one unknown reads
/// better half-resolved than not at all.
/// Which aircraft a showing was of, as a key an impression report can group on.
///
/// The registration rather than the callsign, because the two answer different
/// questions and only one is stable: a tail number belongs to an airframe for
/// its life, while a callsign belongs to a flight and the same aeroplane
/// carries a different one tomorrow. "Which aircraft has this display shown"
/// wants the first, and the callsign is already on the label for anyone reading
/// a single row.
///
/// Falls back to the callsign when no registration arrived, on the same
/// reasoning as the listings card's MLS fallback: a less stable identifier
/// still names the thing, and no key at all names nothing.
String cardListingId(uint16_t) {
  if (!holdsPlane()) {
    return String();
  }

  if (gLast.nearest.registration.length() > 0) {
    return gLast.nearest.registration;
  }

  return gLast.nearest.callsign;
}

String cardDescribe(uint16_t) {
  if (!holdsPlane()) {
    return String();
  }

  // The flight where one was filed, the tail number where none was - the same
  // fallback the card's own headline takes, so a military aircraft flying
  // without a flight id still names something here. Nothing at all when neither
  // arrived.
  String summary = gLast.nearest.callsign.length() > 0 ? gLast.nearest.callsign
                                                       : gLast.nearest.registration;
  if (summary.length() == 0) {
    return String();
  }

  const String origin =
      gLast.nearest.originName.length() > 0 ? gLast.nearest.originName : gLast.nearest.originCode;
  const String destination = gLast.nearest.destinationName.length() > 0
                                 ? gLast.nearest.destinationName
                                 : gLast.nearest.destinationCode;

  if (origin.length() > 0 || destination.length() > 0) {
    summary += " - " + (origin.length() > 0 ? origin : String("?")) + " to " +
               (destination.length() > 0 ? destination : String("?"));
  }
  summary += " - " + String(gLast.nearest.distanceMiles, 1) + " mi away";
  return summary;
}

[[maybe_unused]] const bool kRegistered = [] {
  Cards::CardSpec spec;
  spec.id = "aircraft";
  spec.kind = Cards::Kind::List;
  spec.fetch = cardFetch;
  spec.itemCount = cardItemCount;
  spec.describe = cardDescribe;
  spec.listingId = cardListingId;
  spec.draw = cardDraw;
  spec.isNotable = cardIsNotable;
  spec.status = cardStatus;
  spec.order = 2;
  spec.dwellSeconds = 8;
  spec.notableDwellSeconds = 20;
  return Cards::registerCard(spec, Cards::KeyPolicy::Resolves);
}();

}  // namespace

void releaseRamBuffer() {
  // The id goes with the bytes, and it is cleared whether or not there was a
  // buffer to free. Assets.h's releaseRamBuffer() zeroes size and capacity but
  // cannot reach a caller's own idea of what the buffer holds, and cardDraw()
  // reads gLogoRamId to decide whether to draw from it - so clearing it is the
  // part that must not be skipped by the guard.
  //
  // gLogoRamDrawFailures is deliberately NOT reset here. This runs on the
  // check-in failure path, and a device short of contiguous heap is both the
  // device whose check-ins fail and the device whose draws refuse; resetting
  // the budget here would let those two re-arm each other into a restart.
  if (gLogoRam.data != nullptr) {
    Assets::releaseRamBuffer(gLogoRam);
  }
  gLogoRamId = "";
}

}  // namespace Aircraft
