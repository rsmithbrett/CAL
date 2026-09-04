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

/// Updates the day/night theme and the corner-clock's UTC offset that every
/// screen below reads when it next draws - not retroactive to whatever is
/// already on screen. Called once from App.ino's performCheckIn() whenever
/// a check-in succeeds (see CheckIn::Result::utcOffsetMinutes/isDaytime),
/// so this file has exactly one place tracking "what does the App currently
/// believe about local time and daylight" rather than every draw call
/// taking both as parameters. Defaults (0 minutes, daytime) match App.ino's
/// own pre-first-check-in defaults, so the very first boot screens render
/// sensibly before any check-in has ever completed.
void setEnvironment(int utcOffsetMinutes, bool isDaytime);

/// The raw touch read beneath Touch.h/.cpp's debounced, event-style API.
/// Lives here, not in Touch.cpp, because this file already owns the one
/// LGFX instance for this panel (see `lcd` and begin() below) - a second
/// LGFX_AUTODETECT instance addressing the same physical SPI bus would risk
/// re-initialising hardware this file already brought up. Returns false
/// (x/y untouched) when nothing is currently touching the panel.
bool readTouchRaw(int32_t& x, int32_t& y);

/// A single line of status with an optional detail line beneath it.
void showStatus(const String& headline, const String& detail = "");

/// A failure the household can act on.
void showFailure(const String& headline, const String& whatToDo);

/// The weather card itself. temperature/unit/shortForecast come from the
/// nearest forecast period; location is Home or Target's city/state,
/// whichever the caller resolved; updatedAt is a short human string ("Updated
/// 2 min ago") the caller computes, not a raw timestamp Display has to format.
///
/// Styled after CYD-Dickey's drawWeatherCard() - colour-banded label in the
/// top-left corner, headline number set left-aligned in a bold sans face,
/// body copy left-margined below it - but deliberately NOT at their type
/// sizes. Their card fills the panel with six live readings and a five-day
/// strip; this one has three fields, because that is all the server sends.
/// Matching their sizes on a third of their content produced a card that was
/// both small and empty, so the hierarchy here is stretched to fit what is
/// actually available: the temperature is a 24pt hero rather than 12pt, and
/// the supporting lines are set in readable 9pt bold rather than the 6x8
/// bitmap grey they were. See showWeatherCard()'s own layout note in
/// Display.cpp, and the README on what the server would have to send for the
/// missing half of their card to be possible at all.
///
/// The degree mark stays hand-drawn (see drawTemperature in Display.cpp) -
/// CYD-Dickey sidesteps the glyph entirely by never printing one ("72F"), but
/// CAL already solved this the better way and regressing to their workaround
/// would be a downgrade, not alignment.
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

/// The sunrise/sunset card. Both time strings are already-formatted local
/// wall-clock ("06:32") - this draws, it does not compute, so the
/// minutes-to-local arithmetic lives in exactly one place (SunMoon.cpp)
/// rather than half of it here. Same white/bannered card family as weather
/// and aircraft.
///
/// `detail` is the single line under the two times: day length on an ordinary
/// day, and on a polar day or night the reason there is no time to show.
void showSunMoonCard(const String& sunriseText, const String& sunsetText, const String& detail);

/// Shown when no registered card has anything to draw at all - which is the
/// ordinary state for the first second or two after boot, before the first
/// fetch lands. Same white/bannered card family as the two status screens
/// above rather than the black boot ladder: nothing is wrong with the
/// device, it simply has no content yet.
void showNoContent(const String& headline, const String& detail);

// --- Card chrome: the controls drawn on top of whatever card is showing.
//
// Both of these are drawn by CardManager after a card's own draw function
// has finished, so they land on a completed card rather than being painted
// over by it. Their geometry is decided here, not by the server: only this
// file knows this panel's size and what else is already on it. The corner
// clock owns the bottom-right (see drawClock in Display.cpp) and the
// left/right edge strips belong to the reverse/forward touch zones (see
// Touch.h), so the button row sits clear of all three.

/// Up to Actions::kMaxButtonsPerCard buttons in a row along the bottom of the
/// card. Labels are drawn verbatim, truncated to fit - the server chose the
/// wording and this file does not second-guess it.
void drawActionButtons(const String* labels, uint8_t count);

/// The hit rectangle for button `index` of `count`, in the same layout
/// drawActionButtons() uses. Handed to Touch::setActionZones() so the hit
/// test and the drawing can never disagree about where a button is.
void actionButtonZone(uint8_t index, uint8_t count, int16_t& x, int16_t& y, int16_t& w,
                      int16_t& h);

/// Briefly redraws one button in its pressed colour and puts it straight
/// back. This acknowledges the *press* only. It deliberately says nothing
/// about delivery: a press is a passive push that rides the next ordinary
/// check-in, with no confirmation and nothing for the user to wait for (see
/// Actions.h). A button that does not visibly react to a finger reads as a
/// dead button, which is its own, separate failure.
void flashActionButton(uint8_t index, uint8_t count, const String& label);

/// Small chevrons at the left and right edges marking the reverse/forward
/// touch zones. CYD-Dickey leaves its equivalent zones completely invisible;
/// these are drawn because an invisible control on a household appliance is
/// only discoverable by accident. `canReverse` dims the left one when there
/// is no history to step back into, so the affordance never promises
/// something that will do nothing.
void drawNavAffordances(bool canReverse);

/// Draws a PNG from the SD card, scaled to fit and centred on the whole
/// panel. Clears to the theme background first, so a failed decode leaves a
/// clean screen rather than a half-painted one; returns false in that case so
/// the caller can put its own content back. Assets.cpp is the only caller -
/// the SD read lives behind this function because this file owns the one
/// LGFX instance for this panel, the same reason readTouchRaw() is here.
bool drawPngFromSd(const String& path);

}  // namespace Display

// A note on the day/night theme this file implements (see setEnvironment()
// above): "day" is exactly the white-background/black-ink/grey-muted look
// the card family already had (see kBgDay/kInkDay/kMutedDay in Display.cpp);
// "night" is the black-background/white-ink/lighter-grey look the
// boot-ladder screens (showStatus/showFailure) already had before this -
// applied uniformly, so the whole App matches what a household would see
// out their own window, not just the two content cards. The colour-banded
// banners (WEATHER navy, OVERHEAD blue) and the amber warning colour are
// deliberately NOT part of the swap - both already read fine against either
// background, and inventing night variants of them would be theme-following
// for its own sake rather than a real legibility need.
