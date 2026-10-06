#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace PiPolicy {

struct Entry {
  std::string id;
  std::string kind;
  int order = 0;
  int dwellSeconds = 0;
  int interleaveEvery = 0;
  int notableDwellSeconds = 0;
  int maxItems = 0;
  // Preserve optional server fields without inventing rendering support for them.
  nlohmann::json fields;
};

struct Snapshot {
  int defaultDwellSeconds = 0;
  int manualNavHoldSeconds = 0;
  std::vector<Entry> cards;
};

inline int number(const nlohmann::json& object, const char* name, int minimum, int maximum) {
  const auto field = object.find(name);
  if (field == object.end() || field->is_null()) return 0;
  if (!field->is_number_integer()) throw std::invalid_argument("invalid policy number");
  if (field->is_number_unsigned()) {
    const auto value = field->get<uint64_t>();
    if (value > static_cast<uint64_t>(maximum)) throw std::invalid_argument("invalid policy number");
    return static_cast<int>(value);
  }
  const auto value = field->get<int64_t>();
  if (value < minimum || value > maximum) throw std::invalid_argument("invalid policy number");
  return static_cast<int>(value);
}

inline std::string word(const nlohmann::json& object, const char* name, bool required) {
  const auto field = object.find(name);
  if (field == object.end() || field->is_null()) {
    if (required) throw std::invalid_argument("missing policy identifier");
    return {};
  }
  if (!field->is_string()) throw std::invalid_argument("invalid policy identifier");
  const auto value = field->get<std::string>();
  if ((required && value.empty()) || value.size() > 64 ||
      value.find_first_of("\r\n\t") != std::string::npos || value.find('\0') != std::string::npos)
    throw std::invalid_argument("invalid policy identifier");
  return value;
}

inline std::optional<Snapshot> decode(const std::string& body) {
  try {
    if (body.empty() || body.size() > 65536) throw std::invalid_argument("invalid response size");
    std::vector<std::set<std::string>> keys;
    auto callback = [&keys](int depth, nlohmann::json::parse_event_t event, nlohmann::json& value) {
      if (depth > 16) throw std::invalid_argument("response nesting exceeds limit");
      if (event == nlohmann::json::parse_event_t::object_start) keys.emplace_back();
      if (event == nlohmann::json::parse_event_t::key &&
          !keys.back().insert(value.get<std::string>()).second)
        throw std::invalid_argument("duplicate response key");
      if (event == nlohmann::json::parse_event_t::object_end) keys.pop_back();
      return true;
    };
    const auto root = nlohmann::json::parse(body, callback);
    if (!root.is_object() || !root.contains("acknowledged") ||
        !root["acknowledged"].is_boolean() || !root["acknowledged"].get<bool>())
      throw std::invalid_argument("response not acknowledged");
    const auto policy = root.find("cardPolicy");
    if (policy == root.end() || policy->is_null()) return std::nullopt;
    if (!policy->is_object() || !policy->contains("cards") || !(*policy)["cards"].is_array() ||
        (*policy)["cards"].size() > 64)
      throw std::invalid_argument("invalid policy cards");
    Snapshot result;
    result.defaultDwellSeconds = number(*policy, "defaultDwellSeconds", 0, 65535);
    result.manualNavHoldSeconds = number(*policy, "manualNavHoldSeconds", 0, 65535);
    std::set<std::string> ids;
    for (const auto& card : (*policy)["cards"]) {
      if (!card.is_object()) throw std::invalid_argument("invalid policy card");
      Entry entry;
      entry.id = word(card, "id", true);
      if (!ids.insert(entry.id).second) throw std::invalid_argument("duplicate policy card");
      entry.kind = word(card, "kind", false);
      entry.order = number(card, "order", -32768, 32767);
      entry.dwellSeconds = number(card, "dwellSeconds", 0, 65535);
      entry.interleaveEvery = number(card, "interleaveEvery", 0, 65535);
      entry.notableDwellSeconds = number(card, "notableDwellSeconds", 0, 65535);
      entry.maxItems = number(card, "maxItems", 0, 65535);
      entry.fields = card;
      result.cards.push_back(std::move(entry));
    }
    return result;
  } catch (const std::exception&) {
    // JSON-library diagnostics can quote input bytes. Keep those out of device logs.
    throw std::invalid_argument("invalid acknowledged check-in policy response");
  }
}

class State {
 public:
  bool apply(const std::string& body) {
    auto replacement = decode(body);
    if (!replacement) return false;
    current_ = std::move(replacement);
    return true;
  }
  const std::optional<Snapshot>& current() const { return current_; }
 private:
  std::optional<Snapshot> current_;
};

}  // namespace PiPolicy
