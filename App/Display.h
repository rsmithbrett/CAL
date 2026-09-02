#pragma once

#include <Arduino.h>

/// Everything the App draws.
///
/// Unlike CAL's Display, this one renders actual product content (the weather
/// card) alongside the same boot-ladder status/failure screens CAL uses -
/// WifiJoin::joinStoredNetwork() calls showStatus exactly as CAL's own
/// Provisioning module does, so the two modules' expectations of Display's
/// surface deliberately match.
namespace Display {

void begin();

/// A single line of status with an optional detail line beneath it.
void showStatus(const String& headline, const String& detail = "");

/// A failure the household can act on.
void showFailure(const String& headline, const String& whatToDo);

/// The weather card itself. temperature/unit/shortForecast come from the
/// nearest forecast period; location is Home or Target's city/state,
/// whichever the caller resolved; updatedAt is a short human string ("Updated
/// 2 min ago") the caller computes, not a raw timestamp Display has to format.
void showWeatherCard(const String& location, int temperature, const String& unit,
                     const String& shortForecast, const String& updatedAt);

}  // namespace Display
