#pragma once
#include "CardRuntime.h"
#include "CheckInTransport.h"
#include <ctime>

namespace PiClient {
inline std::string heartbeat(const std::string& version) {
  if (version.empty() || version.size() > 64 ||
      version.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != std::string::npos)
    throw std::invalid_argument("invalid package version");
  const auto now = std::time(nullptr);
  std::tm utc{};
  char timestamp[21];
  if (!gmtime_r(&now, &utc) || !std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc))
    throw std::runtime_error("UTC clock unavailable");
  return nlohmann::json{{"deviceUtcTimestamp",timestamp},{"firmwareVersion",version}}.dump();
}
// Runs off the SDL thread. A check-in always precedes content access; a denial
// clears all display state. No raw payloads, IDs, URLs, or credentials in logs.
inline bool refresh(PiCards::Runtime& state, const std::string& origin,
                    const PiInstallation::Credentials& credentials, const std::string& version) {
  try {
    auto reply = PiCheckIn::post(origin, credentials, heartbeat(version));
    if (reply.status == 401 || reply.status == 403) { state.revoke(); return false; }
    if (reply.status != 200) throw std::runtime_error("check-in unavailable");
    state.apply(reply.body);
    for (int i = 0; i < 2; ++i) {
      if (!state.needs(i == 1)) { state.weather[i] = {}; continue; }
      auto& forecast = state.weather[i];
      try {
        auto response = PiCheckIn::forecast(origin, credentials, i == 1);
        if (response.status == 401) { state.revoke(); return false; }
        if (response.status == 403) {
          forecast = {};
          forecast.status = "Weather is unavailable for this device";
        } else if (response.status == 200) forecast = PiCards::decodeForecast(response.body, i == 1);
        else throw std::runtime_error("weather unavailable");
      } catch (const std::exception&) {
        forecast.stale = true;
        forecast.status = forecast.periods.empty() ? "Weather unavailable - retrying" : "Last received weather - reconnecting";
      }
    }
    return true;
  } catch (const std::exception&) {
    state.status = "Service unavailable - retrying";
    for (auto& forecast : state.weather) {
      forecast.stale = true;
      forecast.status = forecast.periods.empty() ? "Weather unavailable - retrying" : "Last received weather - reconnecting";
    }
    return false;
  }
}
}  // namespace PiClient
