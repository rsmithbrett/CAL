#pragma once

#include "PolicyState.h"
#include "ScheduleCore.h"
#include <algorithm>
#include <array>
#include <cctype>

namespace PiCards {
struct Period { std::string name, unit, forecast; int temperature = 0; };
struct Forecast {
  std::string location;
  std::vector<Period> periods;
  bool stale = false;
  std::string status = "Waiting for weather";
};
inline std::string text(const nlohmann::json& object, const char* key, size_t limit) {
  const auto field = object.find(key);
  if (field == object.end() || field->is_null()) return {};
  if (!field->is_string()) throw std::invalid_argument("invalid forecast");
  auto value = field->get<std::string>();
  if (value.size() > limit || std::any_of(value.begin(), value.end(),
      [](unsigned char c) { return c < 32 || c == 127; }))
    throw std::invalid_argument("invalid forecast");
  return value;
}
inline Forecast decodeForecast(const std::string& body, bool target) {
  try {
    if (body.empty() || body.size() > 16384) throw std::invalid_argument("invalid forecast");
    // Reuse the bounded JSON/duplicate-key boundary, with an acknowledged wrapper.
    // Forecast is an optional response field so it survives that validation.
    const std::string wrapped = "{\"acknowledged\":true,\"forecast\":" + body + "}";
    PiPolicy::decode(wrapped);
    const auto root = nlohmann::json::parse(body);
    if (!root.is_object() || !root.contains("periods") || !root["periods"].is_array() ||
        root["periods"].size() > 10 || text(root, "requestedLocation", 6) != (target ? "target" : "home"))
      throw std::invalid_argument("invalid forecast");
    Forecast result;
    result.location = text(root, "city", 128);
    const auto state = text(root, "state", 64);
    if (!result.location.empty() && !state.empty()) result.location += ", " + state;
    if (result.location.empty()) result.location = text(root, "postalCode", 32);
    for (const auto& p : root["periods"]) {
      if (!p.is_object() || !p.contains("temperature") || !p["temperature"].is_number_integer())
        throw std::invalid_argument("invalid forecast");
      const auto temperature = PiPolicy::number(p, "temperature", -150, 150);
      if (temperature < -150 || temperature > 150) throw std::invalid_argument("invalid forecast");
      Period period{text(p, "name", 64), text(p, "temperatureUnit", 1),
                    text(p, "shortForecast", 256), static_cast<int>(temperature)};
      if ((period.unit != "F" && period.unit != "C") || period.name.empty() || period.forecast.empty())
        throw std::invalid_argument("invalid forecast");
      result.periods.push_back(std::move(period));
    }
    result.status = result.periods.empty() ? "No forecast available" : "Weather from your service";
    return result;
  } catch (const std::exception&) { throw std::invalid_argument("invalid forecast response"); }
}

struct Card {
  std::string id;
  bool active = false, interstitial = false, target = false;
  int order = 4, dwellSeconds = 10;
  uint16_t interleaveEvery = 0, cardsSince = 0;
};
class Runtime {
 public:
  Runtime() {
    for (size_t i = 0; i < cards.size(); ++i)
      cards[i].id = i == 0 ? "forecast" : "forecast" + std::to_string(i + 1);
  }
  bool apply(const std::string& response) {
    auto snapshot = PiPolicy::decode(response);
    if (!snapshot) return false;
    const auto signature = nlohmann::json::parse(response)["cardPolicy"].dump();
    if (ready && signature == policySignature) return false;
    auto replacement = Runtime();
    replacement.policySignature = signature;
    replacement.weather = weather;
    replacement.defaultDwell = snapshot->defaultDwellSeconds > 0 ? snapshot->defaultDwellSeconds : defaultDwell;
    for (const auto& entry : snapshot->cards) {
      auto found = std::find_if(replacement.cards.begin(), replacement.cards.end(),
          [&entry](const Card& c) { return c.id == entry.id; });
      if (found == replacement.cards.end()) { ++replacement.unknownCount; continue; }
      found->active = true;
      found->interstitial = entry.kind == "interstitial";
      found->order = entry.order;
      found->dwellSeconds = entry.dwellSeconds;
      found->interleaveEvery = entry.interleaveEvery;
      auto location = entry.fields.find("location");
      if (location != entry.fields.end() && location->is_string()) {
        auto value = location->get<std::string>();
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
        found->target = value == "target";
      }
    }
    replacement.status = "No supported cards assigned";
    replacement.ready = true;
    *this = std::move(replacement);
    advance();
    return true;
  }
  void acceptRefresh(Runtime replacement) {
    if (ready && replacement.ready && policySignature == replacement.policySignature) {
      weather = std::move(replacement.weather);
      status = std::move(replacement.status);
    } else *this = std::move(replacement);
  }
  void revoke() { *this = Runtime(); status = "Device access stopped"; }
  void advance() {
    auto eligible = [this](uint8_t i) { return cards[i].active && cards[i].interstitial; };
    auto earlier = [this](uint8_t a, uint8_t b) {
      return cards[a].order == cards[b].order ? a < b : cards[a].order < cards[b].order;
    };
    ScheduleCore::tickActive(cards.data(), cards.size());
    int8_t next = ScheduleCore::dueInterstitial(cards.data(), cards.size(), eligible, earlier);
    if (next < 0) {
      next = ScheduleCore::nextShowable(cards.size(), listCursor,
          [this](uint8_t i) { return cards[i].active && !cards[i].interstitial; }, earlier);
      if (next >= 0) listCursor = next;
      else next = ScheduleCore::nextShowable(cards.size(), current, eligible, earlier);
    }
    current = next;
    if (current >= 0 && cards[current].interstitial) cards[current].cardsSince = 0;
  }
  int dwell() const { return current < 0 || cards[current].dwellSeconds <= 0 ? defaultDwell : cards[current].dwellSeconds; }
  bool needs(bool target) const {
    return std::any_of(cards.begin(), cards.end(), [target](const Card& c) { return c.active && c.target == target; });
  }
  const Forecast* displayed() const { return current < 0 ? nullptr : &weather[cards[current].target ? 1 : 0]; }
  std::array<Card, 5> cards;
  std::array<Forecast, 2> weather;
  size_t unknownCount = 0;
  bool ready = false;
  std::string status = "Connecting to your service";
 private:
  int8_t current = -1, listCursor = -1;
  int defaultDwell = 10;
  std::string policySignature;
};
}  // namespace PiCards
