#include "PolicyState.h"
#include <cassert>
#include <iostream>

int main() {
  PiPolicy::State state;
  const auto valid = R"({"acknowledged":true,"cardPolicy":{"defaultDwellSeconds":15,"manualNavHoldSeconds":30,"cards":[{"id":"forecast","kind":"interstitial","order":-2,"dwellSeconds":11,"interleaveEvery":3,"location":"target"},{"id":"future-card","kind":"list","order":2,"maxItems":3}]}})";
  assert(state.apply(valid));
  assert(state.current()->cards.size() == 2);
  assert(state.current()->cards[0].order == -2);
  assert(state.current()->cards[0].dwellSeconds == 11);
  assert(state.current()->cards[0].interleaveEvery == 3);
  assert(state.current()->cards[0].fields["location"] == "target");
  assert(state.current()->cards[1].id == "future-card");
  assert(state.current()->defaultDwellSeconds == 15);
  assert(!state.apply(R"({"acknowledged":true})"));
  assert(!state.apply(R"({"acknowledged":true,"cardPolicy":null})"));
  assert(state.current()->cards.size() == 2);

  auto refused = [&state](const std::string& input) {
    bool rejected = false;
    try { state.apply(input); }
    catch (const std::invalid_argument& error) {
      rejected = true;
      assert(std::string(error.what()) == "invalid acknowledged check-in policy response");
    }
    assert(rejected);
    assert(state.current()->cards.size() == 2);
    assert(state.current()->cards[0].id == "forecast");
  };
  for (const auto& input : {
      "", "{", R"({"acknowledged":false})", R"({"acknowledged":"true"})",
      R"({"acknowledged":true,"acknowledged":false})",
      R"({"acknowledged":true,"cardPolicy":[]})",
      R"({"acknowledged":true,"cardPolicy":{"cards":null}})",
      R"({"acknowledged":true,"cardPolicy":{"cards":[{"id":"forecast"},{"id":"forecast"}]}})",
      R"({"acknowledged":true,"cardPolicy":{"cards":[{"id":"forecast","id":"other"}]}})",
      R"({"acknowledged":true,"cardPolicy":{"cards":[{"id":3}]}})",
      R"({"acknowledged":true,"cardPolicy":{"cards":[{"id":""}]}})",
      R"({"acknowledged":true,"cardPolicy":{"cards":[{"id":"forecast","dwellSeconds":1.5}]}})",
      R"({"acknowledged":true,"cardPolicy":{"cards":[{"id":"forecast","dwellSeconds":-1}]}})",
      R"({"acknowledged":true,"cardPolicy":{"cards":[{"id":"forecast","order":32768}]}})",
      R"({"acknowledged":true,"cardPolicy":{"cards":[{"id":"forecast","dwellSeconds":18446744073709551615}]}})"}) refused(input);
  refused(std::string(65537, ' '));
  refused("{\"acknowledged\":true,\"ignored\":" + std::string(20, '[') + "0" + std::string(20, ']') + "}");
  auto excessive = nlohmann::json::parse(valid);
  for (int i = 0; i < 65; ++i) excessive["cardPolicy"]["cards"].push_back({{"id", std::to_string(i)}});
  refused(excessive.dump());
  assert(state.apply(R"({"acknowledged":true,"cardPolicy":{"cards":[]}})"));
  assert(state.current()->cards.empty());
  assert(state.apply(R"({"acknowledged":true,"cardPolicy":{"cards":[{"id":"aircraft","dwellSeconds":23}]}})"));
  assert(state.current()->cards.size() == 1 && state.current()->cards[0].id == "aircraft");
  std::cout << "policy state: typed response, atomic replacement, absent/empty semantics and invalid-input bounds passed\n";
}
