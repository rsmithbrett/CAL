#include "CheckIn.h"

#include <ArduinoJson.h>
#include <time.h>

#include "Actions.h"
#include "Assets.h"
#include "CardManager.h"
#include "Config.h"
#include "Http.h"
#include "Identity.h"
#include "Log.h"

namespace CheckIn {
namespace {

constexpr const char* kPath = "/api/checkin";

String nowAsIso8601Utc() {
  time_t now = time(nullptr);
  struct tm utc;
  gmtime_r(&now, &utc);
  char buffer[21];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return String(buffer);
}

/// Days from the civil epoch (1970-01-01) to the given UTC calendar date -
/// Howard Hinnant's well-known days_from_civil algorithm. Used by
/// parseIso8601Utc() below instead of reaching for timegm()/mktime(): both
/// depend on libc/TZ behavior that varies by platform, where this is a pure
/// integer calculation with no timezone concept to get wrong - exactly what
/// is needed here since every field this parses is already UTC.
long daysFromCivil(int year, int month, int day) {
  year -= month <= 2 ? 1 : 0;
  const long era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
  const unsigned dayOfYear =
      (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return era * 146097 + static_cast<long>(dayOfEra) - 719468;
}

/// Parses the server's "2026-09-08T21:42:00Z"-style ISO-8601 UTC instant
/// into epoch seconds, for the ISS next-pass fields below - see CheckIn.h's
/// own remarks on why these are absolute instants rather than the
/// minutes-into-today shape sunrise/tide fields use. Returns 0 (this
/// firmware's "absent" sentinel for these fields) for a JSON null, a missing
/// field, or anything this sscanf() cannot parse - deliberately tolerant
/// rather than asserting, since a malformed instant is exactly as much "no
/// answer" to this device as a JSON null is.
time_t parseIso8601Utc(const char* text) {
  if (text == nullptr || text[0] == '\0') {
    return 0;
  }
  int year, month, day, hour, minute, second;
  if (sscanf(text, "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6) {
    return 0;
  }
  const long days = daysFromCivil(year, month, day);
  return static_cast<time_t>(days) * 86400 + hour * 3600 + minute * 60 + second;
}

/// Rides the ordinary check-in rather than getting an endpoint of its own -
/// that is the whole shape of the feature. A press is a passive push: it is
/// recorded locally, carried along on the next heartbeat, and the device's
/// job ends there. Worst-case latency is one check-in interval, which the
/// server already controls, so tightening it is a config change rather than
/// a new route.
///
/// The field is omitted entirely when there is nothing pending, so an
/// ordinary check-in body is byte-identical to what firmware predating this
/// sent.
void addPendingActions(JsonDocument& requestDoc) {
  Actions::Pending pending[Actions::kMaxPending];
  const uint8_t count = Actions::pendingSnapshot(pending, Actions::kMaxPending);
  if (count == 0) {
    return;
  }

  JsonArray array = requestDoc["pendingActions"].to<JsonArray>();
  for (uint8_t i = 0; i < count; ++i) {
    JsonObject entry = array.add<JsonObject>();
    entry["actionId"] = pending[i].actionId;
    entry["instanceId"] = pending[i].instanceId;
    entry["pressedAtUtc"] = pending[i].pressedAtUtc;
    // Omitted entirely when there is nothing to say, rather than sent as "".
    // This request is built with ArduinoJson, which needs roughly the payload's
    // size again in heap to serialise it, and a key nobody will read is heap
    // spent for nothing on the device that can least afford it. The server
    // treats a missing field and an empty one identically - see
    // PendingDeviceAction.OnScreenSummary.
    if (pending[i].onScreenSummary.length() > 0) {
      entry["onScreenSummary"] = pending[i].onScreenSummary;
    }
  }
  Log::printf("[checkin] carrying %u pending action(s)", count);
}

/// Turns the response's ISO-8601 `maintenanceUntilUtc` into an epoch second, or
/// 0 for absent, null, malformed, or already past.
///
/// Hand-parsed rather than handed to strptime() because the ESP32 newlib build
/// does not provide it, and timegm() is likewise absent - so the conversion uses
/// mktime(), which is correct here only because this device's timezone IS UTC:
/// AppService.cpp calls configTime(0, 0, ...) with a zero GMT offset and a zero
/// DST offset, so local time and UTC are the same clock. If that ever changes,
/// this function breaks silently by the size of the new offset. Only the shape the
/// server emits is accepted: "YYYY-MM-DDTHH:MM:SS" with an optional fractional
/// part and a trailing Z or offset, all of which the server always writes as UTC.
/// Anything else yields 0, which means "no window" - the safe direction, since a
/// window that fails to parse costs a restart during a deploy, while one wrongly
/// accepted suppresses the watchdog on a device that really is unreachable.
time_t parseMaintenanceUntil(JsonVariantConst value) {
  if (value.isNull()) {
    return 0;
  }
  const char* text = value.as<const char*>();
  if (text == nullptr) {
    return 0;
  }

  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  if (sscanf(text, "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day, &hour, &minute, &second) != 6) {
    Log::printf("[checkin] maintenanceUntilUtc '%s' is not an instant this build can read - ignored", text);
    return 0;
  }

  struct tm parts = {};
  parts.tm_year = year - 1900;
  parts.tm_mon = month - 1;
  parts.tm_mday = day;
  parts.tm_hour = hour;
  parts.tm_min = minute;
  parts.tm_sec = second;
  parts.tm_isdst = 0;

  const time_t until = mktime(&parts);
  if (until <= 0 || until < Config::kEarliestPlausibleTime) {
    return 0;
  }
  return until;
}

/// Every field here is optional on the wire. A server that predates them -
/// or one that simply has nothing to say this time - leaves `present` false
/// and the counts at zero, and the device carries on with whatever it
/// already had. That is the firmware half of the standing six-month
/// backward-compatibility mandate: new response fields must never be
/// required.
void parseCardPolicy(JsonVariantConst source, Cards::Policy& policy) {
  if (source.isNull()) {
    return;
  }
  policy.present = true;
  policy.defaultDwellSeconds = source["defaultDwellSeconds"] | 0;
  policy.manualNavHoldSeconds = source["manualNavHoldSeconds"] | 0;

  JsonArrayConst cards = source["cards"].as<JsonArrayConst>();
  if (cards.isNull()) {
    return;
  }
  for (JsonVariantConst card : cards) {
    if (policy.entryCount >= Cards::kMaxPolicyCards) {
      Log::printf("[checkin] cardPolicy has more than %u cards - the rest are ignored",
                  Cards::kMaxPolicyCards);
      break;
    }
    Cards::PolicyEntry& entry = policy.entries[policy.entryCount++];
    entry.id = String(card["id"] | "");
    entry.kind = String(card["kind"] | "");
    entry.order = card["order"] | 0;
    entry.dwellSeconds = card["dwellSeconds"] | 0;
    entry.interleaveEvery = card["interleaveEvery"] | 0;
    entry.notableDwellSeconds = card["notableDwellSeconds"] | 0;
    // Optional, and absent from every card that draws no picture. Empty is
    // the ordinary case, not a fault: the card it names simply reports
    // itself as having nothing to show and the scheduler passes over it.
    entry.assetId = String(card["assetId"] | "");
    // Same optionality as assetId immediately above, for the announcement
    // card's text instead of a picture. Absent (or blank) is the ordinary
    // case for every card that is not an announcement, and for an
    // announcement card nobody has typed anything into yet.
    entry.text = String(card["text"] | "");
    // Same optionality again, for the QR card's payload. Absent (or blank) is
    // the ordinary case for every card that does not draw a QR code, and for
    // a QR card nobody has entered data into yet.
    entry.qrData = String(card["qrData"] | "");
    // Same optionality again, for the one card that shows a single owner
    // location and needs to be told which - "home" or "target", absent
    // meaning Home. Every other card must ignore this field entirely, the
    // same tolerance already given to a stray assetId/text/qrData landing on
    // a card that draws none of those.
    entry.location = String(card["location"] | "");
    // Optional again: whether this card may be overlaid by a banner
    // announcement. Absent means false, which is every policy saved before the
    // feature existed. See Cards::CardSpec::allowBanner for why this is
    // eligibility rather than a promise, and for the per-card `theme` string it
    // replaced.
    entry.allowBanner = card["allowBanner"] | false;
    // These two arrive as ISO-8601 UTC instants, same shape as the ISS
    // next-pass fields below - parsed straight to epoch seconds here rather
    // than carried as strings, since nothing downstream needs the unparsed
    // form. A JSON null, a missing field, or anything parseIso8601Utc() can't
    // read all come back 0 - this struct's own "no bound" sentinel, the same
    // absent-means-unrestricted tolerance every other optional card-policy
    // field already gets.
    entry.effectiveFromUtc = parseIso8601Utc(card["effectiveFromUtc"] | "");
    entry.effectiveToUtc = parseIso8601Utc(card["effectiveToUtc"] | "");
  }
}

void parseCardActions(JsonVariantConst source, Result& result) {
  JsonArrayConst actions = source.as<JsonArrayConst>();
  if (actions.isNull()) {
    return;
  }
  for (JsonVariantConst action : actions) {
    if (result.cardActionCount >= Actions::kMaxDefinitions) {
      break;
    }
    Actions::Definition& definition = result.cardActions[result.cardActionCount++];
    definition.cardId = String(action["cardId"] | "");
    definition.actionId = String(action["actionId"] | "");
    definition.label = String(action["label"] | "");
  }
}

/// Copies a JSON string into a fixed char buffer, truncating rather than
/// dropping. Announcement text and ids are display/matching values with no
/// "well-formed but wrong" failure mode - unlike an asset id, where a
/// truncated value is a perfectly valid id for some *other* asset and so has
/// to be dropped instead.
void copyBounded(const char* source, char* destination, size_t capacity) {
  if (source == nullptr || capacity == 0) {
    destination[0] = '\0';
    return;
  }
  strncpy(destination, source, capacity - 1);
  destination[capacity - 1] = '\0';
}

/// Where parsed announcements actually live - see CheckIn::Result's own
/// remarks on why this is file-static rather than an inline array on that
/// struct. Overwritten by every perform() call, which is safe because the one
/// consumer (App.ino's performCheckIn) reads it immediately and this firmware
/// is single-threaded.
Cards::Announcement gAnnouncementBuffer[Cards::kMaxAnnouncements];

void parseAnnouncements(JsonVariantConst source, Result& result) {
  result.announcements = gAnnouncementBuffer;
  result.announcementCount = 0;

  // Cleared even when the response carries no announcements array at all, so
  // a stale set from the previous check-in can never be re-applied.
  for (uint8_t i = 0; i < Cards::kMaxAnnouncements; ++i) {
    gAnnouncementBuffer[i] = Cards::Announcement{};
  }

  JsonArrayConst announcements = source.as<JsonArrayConst>();
  if (announcements.isNull()) {
    return;
  }
  uint8_t offered = 0;
  for (JsonVariantConst item : announcements) {
    offered++;
    if (result.announcementCount >= Cards::kMaxAnnouncements) {
      // Counted and reported below rather than dropped in silence. This
      // codebase's rule is that a card which quietly never appears is close to
      // undiagnosable on firmware with no tests (see registerCard's own
      // remarks), and a banner nobody can explain the absence of is the same
      // failure. The bound itself is deliberate - see Cards::kMaxAnnouncements
      // on why it is sized against DRAM.
      continue;
    }

    Cards::Announcement& announcement = gAnnouncementBuffer[result.announcementCount];

    copyBounded(item["id"] | "", announcement.id, sizeof(announcement.id));
    copyBounded(item["text"] | "", announcement.text, sizeof(announcement.text));
    announcement.isAction = item["isAction"] | false;
    copyBounded(item["actionId"] | "", announcement.actionId, sizeof(announcement.actionId));

    // Empty or absent stays empty, which is the wildcard - "every card whose
    // policy allows banners" - not "no cards". See
    // Cards::Announcement::targetCardIds.
    announcement.targetCount = 0;
    JsonArrayConst targets = item["targetCardIds"].as<JsonArrayConst>();
    if (!targets.isNull()) {
      for (JsonVariantConst target : targets) {
        if (announcement.targetCount >= Cards::kMaxAnnouncementTargets) {
          break;
        }
        const char* cardId = target.as<const char*>();
        if (cardId == nullptr || strlen(cardId) == 0) {
          continue;
        }
        copyBounded(cardId, announcement.targetCardIds[announcement.targetCount],
                    Cards::kMaxAnnouncementCardIdLength + 1);
        announcement.targetCount++;
      }
    }

    // An announcement with no id cannot be told apart from another one across
    // check-ins, and one with no text has nothing to draw - neither is worth
    // carrying, and both would occupy a slot a usable announcement could have
    // had. Dropped rather than repaired: this device cannot invent either
    // field, and a silent drop with the count left short is exactly what the
    // "hold the first N" bound below already does.
    if (strlen(announcement.id) == 0 || strlen(announcement.text) == 0) {
      Log::line("[banner] dropped an announcement with no id or no text");
      announcement = Cards::Announcement{};
      continue;
    }

    // Local dismissal is never taken from the wire - the server has no idea
    // what this device has shown - so it always starts false here and is
    // carried across by Cards::setAnnouncements() matching on id.
    announcement.dismissedLocally = false;
    result.announcementCount++;
  }

  if (offered > result.announcementCount) {
    Log::printf("[banner] server offered %u announcement(s), holding %u",
                static_cast<unsigned>(offered),
                static_cast<unsigned>(result.announcementCount));
  }
}

void parseAcceptedActionIds(JsonVariantConst source, Result& result) {
  JsonArrayConst ids = source.as<JsonArrayConst>();
  if (ids.isNull()) {
    return;
  }
  for (JsonVariantConst id : ids) {
    if (result.acceptedActionCount >= Actions::kMaxPending) {
      break;
    }
    const char* text = id.as<const char*>();
    if (text == nullptr) {
      continue;
    }
    result.acceptedActionIds[result.acceptedActionCount++] = String(text);
  }
}

}  // namespace

Result perform() {
  Result result;

  if (!Http::ready()) {
    Log::line("[checkin] TLS setup failed, skipping this check-in");
    return result;
  }

  const String url = String("https://") + Config::kServiceHost + kPath;
  if (!Http::beginRequest(url)) {
    Log::line("[checkin] could not begin request, skipping this check-in");
    return result;
  }
  HTTPClient& http = Http::client();
  http.setTimeout(Config::kHttpTimeoutMs);
  http.addHeader("X-Device-Secret", Identity::deviceSecret());
  http.addHeader("Content-Type", "application/json");

  JsonDocument requestDoc;
  requestDoc["deviceUtcTimestamp"] = nowAsIso8601Utc();
  requestDoc["firmwareVersion"] = Identity::installedAppVersion();
  // No battery on this board - see CheckIn.h's own remarks.
  requestDoc["batteryPercent"] = 100;
  requestDoc["charging"] = true;
  addPendingActions(requestDoc);

  // Reports how the *previous* policy this device received actually turned
  // out - the same "N of M entries known" applyPolicy() already logs to the
  // remote debug stream, ridden along on the very next check-in instead of
  // only ever existing there for however long someone happens to be
  // watching. Omitted entirely (rather than sent as 0/0) before this device
  // has ever applied a policy at all, so the server can tell "never
  // configured" apart from "configured with zero entries known".
  if (CardManager::lastPolicyTotalCount() > 0) {
    requestDoc["cardPolicyKnownCount"] = CardManager::lastPolicyKnownCount();
    requestDoc["cardPolicyTotalCount"] = CardManager::lastPolicyTotalCount();
    const String unknownIds = CardManager::lastPolicyUnknownIds();
    if (unknownIds.length() > 0) {
      requestDoc["cardPolicyUnknownIds"] = unknownIds;
    }
  }

  // Reports any asset that downloaded and SHA-256-verified fine but still
  // would not decode as a picture - see Assets.h's own remarks. Omitted
  // entirely when nothing has failed, the same "absent, not zero" contract
  // cardPolicyKnownCount above uses. Cleared only once this request actually
  // succeeds (see below), so a failed check-in never loses the report.
  if (Assets::decodeFailureCount() > 0) {
    requestDoc["assetDecodeFailureCount"] = Assets::decodeFailureCount();
    requestDoc["assetDecodeFailureIds"] = Assets::decodeFailureIds();
  }

  String body;
  serializeJson(requestDoc, body);

  // Every outgoing check-in, verbatim - the always-on summary line at the
  // bottom of this function only ever reports the fields this firmware
  // itself parsed back out of the response, never the raw exchange. With
  // streaming off this costs nothing beyond Log::verbose()'s own early
  // return.
  Log::verbose("[checkin] POST %s body=%s", url.c_str(), body.c_str());

  const int status = http.POST(body);
  Log::verbose("[checkin] response status=%d", status);
  if (status == 401) {
    result.secretRejected = true;
    http.end();
    Log::line("[checkin] rejected: device secret no longer valid (401)");
    return result;
  }
  if (status != 200) {
    http.end();
    Log::printf("[checkin] failed, http status=%d", status);
    // A negative status is HTTPClient's catch-all for everything that failed
    // before a response existed - DNS, TCP connect and the TLS handshake all
    // report as -1. Http::diagnoseFailure() walks those layers and says which
    // one actually broke. Only for negatives: a real status (401, 500, ...)
    // means the server was reached and answered, which needs no archaeology.
    if (status < 0) {
      Http::diagnoseFailure("checkin");
    }
    return result;
  }

  JsonDocument responseDoc;
  const DeserializationError err = deserializeJson(responseDoc, http.getStream());
  http.end();
  if (err) {
    Log::line("[checkin] response was not valid JSON");
    return result;
  }

  result.ok = true;

  // The raw response, re-serialized from what was just parsed rather than
  // captured while streaming off http.getStream() above - that stream is
  // consumed once, straight into responseDoc, precisely so this device never
  // has to hold the whole body as a second, separate String at the same time
  // it holds the parsed JsonDocument. Guarded on streamingEnabled() rather
  // than left to Log::verbose()'s own internal gate, since building this
  // String at all is the cost worth avoiding on every ordinary check-in, not
  // just the verbose() call that would follow it.
  if (Log::streamingEnabled()) {
    String rawResponse;
    serializeJson(responseDoc, rawResponse);
    Log::verbose("[checkin] response body=%s", rawResponse.c_str());
  }

  // The round trip actually completed, so whatever decode failures were
  // just reported above are now the server's problem to have seen - clear
  // them so a persistent failure is reported once per occurrence rather
  // than resending the same ids on every check-in forever. Symmetric with
  // Actions::clearAccepted() below: both only drop what this exact
  // successful response confirms was received.
  if (Assets::decodeFailureCount() > 0) {
    Assets::clearDecodeFailures();
  }

  result.acknowledged = responseDoc["acknowledged"] | false;
  result.updateAvailable = responseDoc["updateAvailable"] | false;
  result.debugStreamRequested = responseDoc["debugStreamRequested"] | false;
  result.sdReformatRequested = responseDoc["sdReformatRequested"] | false;
  // An ISO-8601 instant on the wire, converted to an epoch second here so the
  // watchdog compares two integers rather than parsing a string on every health
  // check. The server has already dropped a window that elapsed by ITS clock;
  // this device checks again against its own, because the two can disagree and a
  // suppressed watchdog on a genuinely unreachable device is the one harm this
  // feature could do. See CheckIn::Result::maintenanceUntilUtc.
  result.maintenanceUntilUtc = parseMaintenanceUntil(responseDoc["maintenanceUntilUtc"]);
  result.utcOffsetMinutes = responseDoc["utcOffsetMinutes"] | 0;
  result.isDaytime = responseDoc["isDaytime"] | true;
  // `| -1` covers a JSON null and a field an older server never sends at all. Both
  // mean the same thing to this client - no sunrise or sunset to show - so neither
  // needs distinguishing here.
  result.sunriseMinutesUtc = responseDoc["sunriseMinutesUtc"] | -1;
  result.sunsetMinutesUtc = responseDoc["sunsetMinutesUtc"] | -1;
  // `| -1.0` covers a JSON null and a field an older server never sends at all, the
  // same reasoning as sunriseMinutesUtc/sunsetMinutesUtc above - both mean "nothing
  // to show" here, since unlike sunrise/sunset there is no separate polar-style
  // absent case to distinguish for the Moon's phase.
  result.moonPhase = responseDoc["moonPhase"] | -1.0;
  result.moonIlluminatedFraction = responseDoc["moonIlluminatedFraction"] | -1.0;
  result.moonPhaseName = String(responseDoc["moonPhaseName"] | "");
  // `| -1` covers a JSON null and a field an older server never sends at all, the
  // same reasoning as sunriseMinutesUtc/sunsetMinutesUtc above - both mean "no
  // tide to show" here.
  result.nextHighTideMinutesUtc = responseDoc["nextHighTideMinutesUtc"] | -1;
  result.nextLowTideMinutesUtc = responseDoc["nextLowTideMinutesUtc"] | -1;
  // `| -999.0`/`| -1.0` cover a JSON null and a field an older server never sends at
  // all, the same reasoning as nextHighTideMinutesUtc/nextLowTideMinutesUtc above -
  // all mean "no ISS position to show" here.
  result.issLatitude = responseDoc["issLatitude"] | -999.0;
  result.issLongitude = responseDoc["issLongitude"] | -999.0;
  result.issDistanceMiles = responseDoc["issDistanceMiles"] | -1.0;
  result.issBearingDegrees = responseDoc["issBearingDegrees"] | -1.0;
  // A different feature from the live position just above - the station's
  // next predicted pass. `| ""` then parseIso8601Utc() covers a JSON null and
  // a field an older server never sends at all the same way `| -1.0` does
  // for the plain-number fields; parseIso8601Utc() itself returns 0 (this
  // struct's "absent" sentinel for a time_t) for both. See CheckIn.h's own
  // remarks on issNextPassRiseUtc for the full reasoning.
  result.issNextPassRiseUtc = parseIso8601Utc(responseDoc["issNextPassRiseUtc"] | "");
  result.issNextPassRiseAzimuthDegrees = responseDoc["issNextPassRiseAzimuthDegrees"] | -1.0;
  result.issNextPassMaxElevationUtc = parseIso8601Utc(responseDoc["issNextPassMaxElevationUtc"] | "");
  result.issNextPassMaxElevationDegrees = responseDoc["issNextPassMaxElevationDegrees"] | -1.0;
  result.issNextPassMaxElevationAzimuthDegrees =
      responseDoc["issNextPassMaxElevationAzimuthDegrees"] | -1.0;
  result.issNextPassSetUtc = parseIso8601Utc(responseDoc["issNextPassSetUtc"] | "");
  result.issNextPassSetAzimuthDegrees = responseDoc["issNextPassSetAzimuthDegrees"] | -1.0;
  // `| -1`/`| -1.0` cover a JSON null and a field an older server never sends
  // at all, the same reasoning as every other optional numeric field above -
  // all mean "no home value estimate to show" here. `| ""` then
  // parseIso8601Utc() for the refresh timestamp is the same "both null and
  // missing become 0" treatment issNextPassRiseUtc gets above. See
  // CheckIn.h's own homeValueEstimate remarks.
  result.homeValueEstimate = responseDoc["homeValueEstimate"] | -1;
  result.homeValueRangeLow = responseDoc["homeValueRangeLow"] | -1;
  result.homeValueRangeHigh = responseDoc["homeValueRangeHigh"] | -1;
  result.homeValuePricePerSquareFoot = responseDoc["homeValuePricePerSquareFoot"] | -1.0;
  result.homeValueUpdatedAtUtc = parseIso8601Utc(responseDoc["homeValueUpdatedAtUtc"] | "");
  // `| ""` covers a JSON null, and a server predating this field that never
  // sends it at all - same reasoning as moonPhaseName below. Both mean "no
  // address to put at the top of the card" here, which is a real answer: a
  // valuation the server resolved from a position fix rather than a stored
  // address has none. See CheckIn.h's own homeValueAddress remarks.
  result.homeValueAddress = String(responseDoc["homeValueAddress"] | "");
  // `| ""` covers a JSON null and a field an older server never sends at all, the
  // same reasoning as moonPhaseName above - both mean "no splash configured" here.
  // See CheckIn.h's own splashAssetId remarks for what this device does with it.
  result.splashAssetId = String(responseDoc["splashAssetId"] | "");
  const int intervalSeconds = responseDoc["checkInIntervalSeconds"] | 300;
  result.intervalMs = static_cast<uint32_t>(intervalSeconds) * 1000UL;

  parseCardPolicy(responseDoc["cardPolicy"], result.cardPolicy);
  parseCardActions(responseDoc["cardActions"], result);
  parseAcceptedActionIds(responseDoc["acceptedActionIds"], result);
  parseAnnouncements(responseDoc["announcements"], result);

  Log::printf(
      "[checkin] ok (acknowledged=%d updateAvailable=%d debugStream=%d sdReformat=%d "
      "intervalSeconds=%d utcOffsetMinutes=%d isDaytime=%d cardPolicy=%d cards=%u actions=%u "
      "accepted=%u splashAssetId=%s)",
      result.acknowledged, result.updateAvailable, result.debugStreamRequested,
      result.sdReformatRequested, intervalSeconds, result.utcOffsetMinutes, result.isDaytime,
      result.cardPolicy.present, result.cardPolicy.entryCount, result.cardActionCount,
      result.acceptedActionCount,
      result.splashAssetId.length() > 0 ? result.splashAssetId.c_str() : "(none)");
  return result;
}

}  // namespace CheckIn
