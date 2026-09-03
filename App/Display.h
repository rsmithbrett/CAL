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
///
/// Styled after CYD-Dickey's drawWeatherCard(): a white card with a small
/// colour-banded label in the top-left corner (theirs says "WEATHER" on
/// navy), the headline number set left-aligned in a bold sans font rather
/// than centered bitmap text, and body copy left-margined below it instead
/// of centered. The degree mark stays hand-drawn (see drawTemperature in
/// Display.cpp) - CYD-Dickey sidesteps the glyph entirely by never printing
/// one ("72F"), but CAL already solved this the better way and regressing to
/// their workaround would be a downgrade, not alignment.
void showWeatherCard(const String& location, int temperature, const String& unit,
                     const String& shortForecast, const String& updatedAt);

/// The weather card's non-Ok states (not activated, provider disabled, no
/// address on file, network trouble). Kept in the same white/bannered card
/// family as showWeatherCard() rather than routed through the black
/// boot-ladder showStatus()/showFailure() above - CYD-Dickey makes this same
/// split (drawStatusMessage's black Wi-Fi/menu screens vs. drawNoAircraftScreen/
/// drawNoListingsScreen's white, card-styled ones for content problems).
/// isProblem picks the headline colour: false (not activated/disabled - a
/// resting state, nothing wrong with the device) reads muted grey; true
/// (auth/network trouble) reads the same amber as showFailure()'s headline.
void showWeatherStatus(const String& headline, const String& detail, bool isProblem);

/// The aircraft-overhead card - new; CAL's App had no equivalent before this.
/// Modeled on CYD-Dickey's drawFeaturedAircraft(), minus the parts that
/// assume data DiscoverAroundMe's server doesn't provide (airline name/logo,
/// route/airport lookups - see Aircraft.h's own remarks): callsign stands in
/// as the card's headline where CYD-Dickey puts the airline logo or name,
/// and the stat rows below (altitude/speed/heading) reuse their
/// truncate-and-right-justify layout for the values.
void showAircraftCard(const String& callsign, int altitudeFeet, double speedKnots,
                      double headingDegrees, double distanceMiles, const String& updatedAt);

/// The aircraft card's non-Ok states, including "fetch succeeded, nothing is
/// currently overhead" (not an error - see Aircraft::Status::Empty) - same
/// white/bannered card family as showAircraftCard(), mirroring
/// showWeatherStatus()'s split from the boot-ladder screens.
void showAircraftStatus(const String& headline, const String& detail, bool isProblem);

}  // namespace Display
