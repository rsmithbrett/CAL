#include "LiveClient.h"
#include <cassert>
#include <iostream>
int main(int argc, char** argv) {
  assert(argc == 3);
  const auto credentials = PiInstallation::load(argv[2]);
  PiCards::Runtime state;
  assert(PiClient::refresh(state, argv[1], credentials, "test"));
  assert(state.displayed() && state.displayed()->periods.size() == 1);
  assert(PiClient::refresh(state, argv[1], credentials, "test"));
  assert(state.displayed()->stale && state.displayed()->periods.size() == 1);
  assert(!PiClient::refresh(state, argv[1], credentials, "test")); // malformed policy
  assert(state.displayed()->periods.size() == 1);
  assert(PiClient::refresh(state, argv[1], credentials, "test")); // provider denied
  assert(state.displayed()->periods.empty());
  assert(!PiClient::refresh(state, argv[1], credentials, "test")); // installation revoked
  assert(!state.ready && !state.displayed() && state.weather[0].periods.empty());
  assert(PiClient::refresh(state, argv[1], credentials, "test")); // reactivated with target
  assert(state.displayed() == &state.weather[1] && !state.displayed()->stale);
  std::cout << "refresh: null policy, transient failure, malformed replacement, provider denial, revocation and recovery passed\n";
}
