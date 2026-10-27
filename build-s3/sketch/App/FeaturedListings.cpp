#line 1 "C:\\Users\\Administrator\\Documents\\CAL\\App\\FeaturedListings.cpp"
#include "FeaturedListings.h"

#include <ArduinoJson.h>

#include <utility>  // std::move, for handing the fetched result to gLast without copying it

#include "Assets.h"
#include "Cards.h"
#include "Config.h"
#include "Display.h"
#include "Http.h"
#include "Identity.h"
#include "Log.h"
#include "Maintenance.h"

namespace FeaturedListings {
namespace {

constexpr const char* kPath = "/api/featuredlistings/mine";

// The picture route is keyed on the CONTENT KEY, which is the only identifier this device
// holds and which the server minted for this association. The image key beside it is what the
// picture is cached under, not what it is fetched by - the two differ because the cache id has
// to change when the picture is replaced and the content key does not.
constexpr const char* kImagePathPrefix = "/api/featuredlistings/image/";

// The same ContentProviderGate 403 body every device-facing content route
// produces - identical shape and reasoning to Listings.cpp's own parseRefusal.
Result parseRefusal(const String& body) {
  Result result;
  result.status = Status::NetworkError;

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    result.message = "Cannot reach the listings service.";
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

  Log::printf("[featuredlistings] refused (%s): %s", reason, result.message.c_str());
  return result;
}

}  // namespace

Result fetch() {
  Result result;

  if (!Http::ready()) {
    result.message = "Cannot verify the service's identity.";
    Log::line("[featuredlistings] TLS setup failed");
    return result;
  }

  const String url = String("https://") + Config::kServiceHost + kPath;
  if (!Http::beginRequest(url)) {
    result.message = "Cannot reach the listings service.";
    result.serviceUnreachable = true;
    Log::line("[featuredlistings] could not begin request");
    return result;
  }

  HTTPClient& http = Http::client();
  http.setTimeout(Config::kHttpTimeoutMs);
  http.addHeader("X-Device-Secret", Identity::deviceSecret());

  Log::verbose("[featuredlistings] GET %s", url.c_str());
  const int status = http.GET();
  Log::verbose("[featuredlistings] response status=%d", status);

  if (status == 401) {
    http.end();
    result.status = Status::AuthError;
    result.message = "Cannot verify this device. Contact support.";
    Log::line("[featuredlistings] auth rejected (401)");
    return result;
  }

  if (status == 403) {
    const String body = http.getString();
    http.end();
    return parseRefusal(body);
  }

  if (status == 404) {
    // No owner on this display, so nothing here is personal to it. Not an
    // error and not worth a message: the card simply does not apply, and the
    // scheduler drops it from the rotation.
    http.end();
    result.status = Status::NotConfigured;
    return result;
  }

  if (status != 200) {
    http.end();
    result.message = "Cannot reach the listings service.";
    result.serviceUnreachable = true;
    Log::printf("[featuredlistings] unexpected http status=%d", status);
    return result;
  }

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, http.getStream());
  http.end();

  if (error) {
    result.message = "Could not read the reply.";
    Log::printf("[featuredlistings] json parse failed: %s", error.c_str());
    return result;
  }

  if (!(doc["isConfigured"] | false)) {
    result.status = Status::NotConfigured;
    return result;
  }

  JsonArrayConst cards = doc["cards"].as<JsonArrayConst>();

  for (JsonObjectConst card : cards) {
    if (result.count >= kMaxCards) {
      // The server caps at five too. Hitting this means the two disagree,
      // which is worth saying out loud rather than silently truncating - a
      // card a household never sees because of a cap nobody remembered is
      // exactly the kind of thing that goes unnoticed for months.
      Log::printf("[featuredlistings] server sent more than %u cards, drawing %u",
                  static_cast<unsigned>(kMaxCards), static_cast<unsigned>(kMaxCards));
      break;
    }

    Card& out = result.cards[result.count];

    out.contentKey = card["contentKey"] | "";
    out.title = card["title"] | "";
    out.headline = card["headline"] | "";
    out.subhead = card["subhead"] | "";

    // Negative when the source stated none. The server sends null rather than
    // zero for exactly this reason, and turning that into 0 here would undo it.
    out.price = card["price"].isNull() ? -1 : (card["price"] | -1);

    // What that price replaced, read the same way and for the same reason. A
    // member that is absent reads as null here too, so a response from a server
    // that predates the field lands on -1 and the second line is not drawn.
    out.previousPrice =
        card["previousPrice"].isNull() ? -1 : (card["previousPrice"] | -1);

    strlcpy(out.imageKey, card["imageKey"] | "", sizeof(out.imageKey));

    out.factCount = 0;
    for (JsonObjectConst fact : card["facts"].as<JsonArrayConst>()) {
      if (out.factCount >= kMaxFacts) {
        break;
      }

      // Copied into the fixed buffers rather than assigned - see FeaturedListings.h for
      // why these two are not Strings. Anything over the cap is truncated here
      // and said out loud: the server caps the same figure, so a long one means
      // the two ends disagree, and silently clipping a fact nobody can read
      // would hide that for as long as it took somebody to photograph a card.
      const char* label = fact["label"] | "";
      const char* value = fact["value"] | "";

      if (strlen(label) > kMaxFactText || strlen(value) > kMaxFactText) {
        Log::printf("[featuredlistings] a fact arrived longer than %u chars ('%s'/'%s') - truncated",
                    static_cast<unsigned>(kMaxFactText), label, value);
      }

      Fact& into = out.facts[out.factCount];
      strlcpy(into.label, label, sizeof(into.label));
      strlcpy(into.value, value, sizeof(into.value));
      out.factCount++;
    }

    // A card with no key cannot be measured and should not be drawn: its
    // screen time would be credited to nothing, which is worse than one fewer
    // card. Dropped loudly, because it means the two ends disagree.
    if (out.contentKey.length() == 0) {
      Log::printf("[featuredlistings] a card arrived with no contentKey ('%s') - dropped, because its "
                  "screen time could not be attributed to anything",
                  out.headline.c_str());
      continue;
    }

    result.count++;
  }

  result.status = result.count > 0 ? Status::Ok : Status::Empty;

  Log::printf("[featuredlistings] %u propert%s for this household", static_cast<unsigned>(result.count),
              result.count == 1 ? "y" : "ies");

  return result;
}

namespace {

Result gLast;
bool gEverFetched = false;
unsigned long gLastOkMs = 0;

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

void cardFetch() {
  Result fresh = fetch();
  gEverFetched = true;

  // A fetch that could not reach the service does not erase what is on the
  // glass - the same rule Forecast, Listings and Aircraft follow, and for the
  // same reason: no answer came back, so the last answer is still the best
  // available account of the world. Every other failure is the server
  // ANSWERING - a refusal, a disabled provider, a device not activated - and
  // those are real changes of state that must replace what is shown.
  if (fresh.status != Status::Ok && fresh.serviceUnreachable && gLast.status == Status::Ok) {
    Log::printf("[featuredlistings] could not reach the service - keeping the last good result on "
                "screen rather than drawing an error over it (%s)",
                describeFreshness(gLastOkMs).c_str());
    return;
  }

  // MOVED, not copied. This used to be gLast = fetch(), assigning straight
  // from the returned temporary, which the compiler moves. Binding to a local
  // first - which the keep-the-cache check above needs - turns that into a deep
  // copy of every String in the result, and a const local makes the move
  // impossible. On a display whose whole failure mode is contiguous heap, that is
  // allocation churn introduced by a fix for a different problem.
  gLast = std::move(fresh);

  if (gLast.status == Status::Ok) {
    gLastOkMs = millis();
    Log::printf("[featuredlistings] card updated: %u propert%s, first '%s'",
                static_cast<unsigned>(gLast.count), gLast.count == 1 ? "y" : "ies",
                gLast.cards[0].headline.c_str());
  }
}

uint16_t cardItemCount() {
  if (!gEverFetched) {
    return 0;
  }

  if (gLast.status == Status::Ok) {
    return gLast.count;
  }

  // No owner on this display, or an owner no agent has matched to anything. Zero
  // items, so the scheduler skips the card rather than spending a dwell on it -
  // the one state where the "a message is content too" tolerance Listings and
  // Weather give their own is wrong here. This card is personal by definition, so
  // on a display it does not belong to there is nothing to say and nobody to say
  // it to: a counter display in a shop would be telling passers-by about a house
  // hunt that is not theirs and never will be.
  if (gLast.status == Status::NotConfigured) {
    Log::verbose("[featuredlistings] nothing matched to this display - taking no turn");
    return 0;
  }

  // Every other outcome is the server ANSWERING - not activated, provider
  // disabled, or a failure - and those are worth a slide, because they are states
  // an owner or an operator needs told rather than silence to diagnose.
  return 1;
}

/// What the impression carries back. The server minted it; this returns it
/// unchanged, which is the entire join between a card on a wall and every
/// number measured about that listing.
String cardListingId(uint16_t itemIndex) {
  if (gLast.status != Status::Ok || itemIndex >= gLast.count) {
    return String();
  }

  return gLast.cards[itemIndex].contentKey;
}

String cardDescribe(uint16_t itemIndex) {
  if (gLast.status != Status::Ok || itemIndex >= gLast.count) {
    return String();
  }

  const Card& card = gLast.cards[itemIndex];
  String summary = card.headline;

  if (card.price > 0) {
    summary += " - $" + String(card.price);
  }

  for (uint8_t i = 0; i < card.factCount; ++i) {
    summary += " - ";
    summary += card.facts[i].label;
    summary += " ";
    summary += card.facts[i].value;
  }

  return summary;
}

void cardDraw(uint16_t itemIndex) {
  if (gLast.status == Status::Ok && itemIndex < gLast.count) {
    const Card& card = gLast.cards[itemIndex];

    Display::FactLine lines[kMaxFacts];
    for (uint8_t i = 0; i < card.factCount; ++i) {
      lines[i].label = card.facts[i].label;
      lines[i].value = card.facts[i].value;
    }

    // Cached on the card, not held in RAM. The download streams through a 512-byte stack
    // buffer and the decoder reads the file block by block, so a picture costs essentially
    // nothing of the contiguous heap this board's TLS session needs - see
    // Assets::ensureCachedFrom. A failure here is drawn as a card with no picture rather
    // than as an error: the address and the price are what the household came for.
    const bool hasPicture =
        card.imageKey[0] != '\0'
        && Assets::ensureCachedFrom(String(kImagePathPrefix) + card.contentKey, card.imageKey);

    if (card.imageKey[0] != '\0' && !hasPicture) {
      Log::printf("[featuredlistings] picture '%s' is not on the card, drawing without it",
                  card.imageKey);
    }

    Display::showFeaturedListingCard(card.title, card.headline, card.subhead, card.price,
                              card.previousPrice, lines, card.factCount,
                              describeFreshness(gLastOkMs),
                              hasPicture ? card.imageKey : "");
    return;
  }

  const bool isRestingState =
      gLast.status == Status::Empty || gLast.status == Status::NotConfigured
      || gLast.status == Status::NotActivated || gLast.status == Status::ProviderDisabled;

  const String headline = gLast.status == Status::Empty
                              ? "Nothing new from your agent"
                              : isRestingState ? "Your house hunt is not set up yet"
                                               : "Could not load your house hunt";

  // Resolved on this draw rather than when the fetch failed - see Maintenance.h:
  // a declared maintenance window elapsing is felt on the next repaint instead
  // of waiting for a refresh that may never succeed.
  const String detail = Maintenance::failureText(gLast.message, gLast.serviceUnreachable);

  Display::showListingsStatus(headline, detail, /*isProblem=*/!isRestingState);
}

[[maybe_unused]] const bool kRegistered = [] {
  Cards::CardSpec spec;
  spec.id = "featuredlistings";
  spec.kind = Cards::Kind::List;
  spec.fetch = cardFetch;
  spec.itemCount = cardItemCount;
  spec.describe = cardDescribe;
  spec.listingId = cardListingId;
  spec.draw = cardDraw;
  spec.order = 4;

  // A FALLBACK, not this card's dwell. CardManager::applyPolicy() overwrites
  // this with the policy entry's own seconds, and with 0 - which resolves to
  // the policy's default - when the entry says nothing. Since a card only
  // draws when a policy names it, the number below applies in exactly one
  // case: a policy that matched no card this firmware knows, where every card
  // is switched back on and keeps what it registered.
  //
  // 14 because a household's own property is worth reading rather than
  // glancing at, and five of them turn over in under a minute at ten. Set the
  // real figure per card on the policy.
  spec.dwellSeconds = 14;

  return Cards::registerCard(spec);
}();

}  // namespace

}  // namespace FeaturedListings
