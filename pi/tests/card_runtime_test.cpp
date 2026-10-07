#include "CardRuntime.h"
#include <cassert>
#include <iostream>

int main() {
  PiCards::Runtime state;
  assert(!state.displayed());
  state.apply(R"({"acknowledged":true,"cardPolicy":{"defaultDwellSeconds":17,"cards":[{"id":"future-card"},{"id":"forecast","order":2,"dwellSeconds":3},{"id":"forecast2","order":1,"location":"TARGET"},{"id":"forecast3","order":3,"kind":"interstitial","interleaveEvery":2}]}})");
  assert(state.unknownCount == 1 && state.ready);
  assert(state.needs(false) && state.needs(true));
  assert(state.displayed() == &state.weather[1] && state.dwell() == 17);
  state.advance();
  assert(state.displayed() == &state.weather[0] && state.dwell() == 3);
  state.advance(); // strict interleave threshold: third computed card
  assert(state.displayed() == &state.weather[0] && state.dwell() == 17);
  state.advance();
  assert(state.displayed() == &state.weather[1]);
  assert(!state.apply(R"({"acknowledged":true,"cardPolicy":null})"));
  assert(state.needs(true));
  // A worker finishing an unchanged refresh must not rewind live rotation.
  auto refreshed = state;
  state.advance();
  const auto expected = state.displayed();
  state.acceptRefresh(refreshed);
  assert(state.displayed() == expected);
  try { state.apply(R"({"acknowledged":true,"cardPolicy":{"cards":[{"id":"forecast"},{"id":"forecast"}]}})"); assert(false); }
  catch (const std::invalid_argument&) {}
  assert(state.needs(true));
  state.apply(R"({"acknowledged":true,"cardPolicy":{"cards":[{"id":"forecast5","kind":"interstitial","location":"target"}]}})");
  assert(state.displayed() == &state.weather[1]); // singleton-only fallback
  state.advance(); assert(state.displayed() == &state.weather[1]);
  state.apply(R"({"acknowledged":true,"cardPolicy":{"cards":[]}})");
  assert(!state.displayed() && !state.needs(false) && !state.needs(true));
  const std::string forecast = R"({"requestedLocation":"home","city":"Annapolis","state":"MD","periods":[{"name":"Today","temperature":72,"temperatureUnit":"F","shortForecast":"Sunny"}]})";
  const auto parsed = PiCards::decodeForecast(forecast, false);
  assert(parsed.location == "Annapolis, MD" && parsed.periods[0].temperature == 72);
  assert(PiCards::decodeForecast(R"({"requestedLocation":"target","city":null,"periods":[]})",true).periods.empty());
  for (const auto& invalid : {forecast, std::string("{}"),
      std::string(R"({"requestedLocation":"target","periods":[],"periods":[]})"),
      std::string(R"({"requestedLocation":"target","periods":[{"name":"Today","temperature":500,"temperatureUnit":"F","shortForecast":"Sunny"}]})")}) {
    try { PiCards::decodeForecast(invalid, true); assert(false); }
    catch (const std::invalid_argument& e) { assert(std::string(e.what()) == "invalid forecast response"); }
  }
  state.weather[0] = parsed;
  state.revoke();
  assert(!state.ready && !state.displayed() && state.weather[0].periods.empty());
  std::cout << "card runtime: policy/order/dwell/interleave/unknown/empty/forecast bounds/revocation passed\n";
}
