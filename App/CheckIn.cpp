#include "CheckIn.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <time.h>

#include "Actions.h"
#include "Config.h"
#include "Identity.h"
#include "Log.h"
#include "Tls.h"

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
  }
  Log::printf("[checkin] carrying %u pending action(s)", count);
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

  NetworkClientSecure client;
  if (!Tls::configure(client)) {
    Log::line("[checkin] TLS setup failed, skipping this check-in");
    return result;
  }

  HTTPClient http;
  const String url = String("https://") + Config::kServiceHost + kPath;
  if (!http.begin(client, url)) {
    Log::line("[checkin] could not begin request, skipping this check-in");
    return result;
  }
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

  String body;
  serializeJson(requestDoc, body);

  const int status = http.POST(body);
  if (status == 401) {
    result.secretRejected = true;
    http.end();
    Log::line("[checkin] rejected: device secret no longer valid (401)");
    return result;
  }
  if (status != 200) {
    http.end();
    Log::printf("[checkin] failed, http status=%d", status);
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
  result.acknowledged = responseDoc["acknowledged"] | false;
  result.updateAvailable = responseDoc["updateAvailable"] | false;
  result.debugStreamRequested = responseDoc["debugStreamRequested"] | false;
  result.utcOffsetMinutes = responseDoc["utcOffsetMinutes"] | 0;
  result.isDaytime = responseDoc["isDaytime"] | true;
  // `| -1` covers a JSON null and a field an older server never sends at all. Both
  // mean the same thing to this client - no sunrise or sunset to show - so neither
  // needs distinguishing here.
  result.sunriseMinutesUtc = responseDoc["sunriseMinutesUtc"] | -1;
  result.sunsetMinutesUtc = responseDoc["sunsetMinutesUtc"] | -1;
  const int intervalSeconds = responseDoc["checkInIntervalSeconds"] | 300;
  result.intervalMs = static_cast<uint32_t>(intervalSeconds) * 1000UL;

  parseCardPolicy(responseDoc["cardPolicy"], result.cardPolicy);
  parseCardActions(responseDoc["cardActions"], result);
  parseAcceptedActionIds(responseDoc["acceptedActionIds"], result);

  Log::printf(
      "[checkin] ok (acknowledged=%d updateAvailable=%d debugStream=%d intervalSeconds=%d "
      "utcOffsetMinutes=%d isDaytime=%d cardPolicy=%d cards=%u actions=%u accepted=%u)",
      result.acknowledged, result.updateAvailable, result.debugStreamRequested, intervalSeconds,
      result.utcOffsetMinutes, result.isDaytime, result.cardPolicy.present,
      result.cardPolicy.entryCount, result.cardActionCount, result.acceptedActionCount);
  return result;
}

}  // namespace CheckIn
