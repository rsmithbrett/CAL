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

/// Reads back the UTC offset most recently given to setEnvironment() - the
/// same value drawClock()'s corner clock already uses on every card. A read
/// accessor rather than a second pushed copy (the way SunMoon::setTimes()
/// receives its own) because this file is already the one place tracking
/// "what does the App currently believe about local time right now"; a card
/// that wants that same belief (see ClockDate.cpp) should read it from here
/// rather than keep a third copy that could drift from the two that already
/// exist - App.ino's own lastUtcOffsetMinutes and this file's internal
/// gUtcOffsetMinutes. Defaults to 0 (UTC), matching setEnvironment()'s own
/// pre-first-check-in default.
int utcOffsetMinutes();

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

/// The aircraft-overhead card. Originally modeled on CYD-Dickey's
/// drawFeaturedAircraft() minus the parts that assumed data the server didn't
/// provide; the server now does (airline name, logo, route - see Aircraft.h's
/// updated remarks), so this draws them, with callsign staying on screen as a
/// secondary line rather than being displaced entirely.
///
/// airlineName empty means the server has no name for this callsign (older
/// firmware talking to a newer server never happens the other way, but the
/// reverse - this firmware against a server old enough to send nothing - is
/// exactly the 6-month compatibility case) - callsign is promoted back to the
/// headline in that case, which is this card's entire original behaviour.
///
/// originCode/destinationCode/originName/destinationName: each side prefers
/// its name and falls back to its own code independently - not as an
/// all-or-nothing pair - because hexdb can resolve a route's code without a
/// name for one particular airport (see AircraftSighting.cs's own remarks),
/// and a server old enough to predate the two name fields sends codes alone.
/// Empty destination (name and code both) with a non-empty origin draws a
/// one-sided "from X" line (a filed departure with no filed arrival, or a
/// route lookup partial); everything empty draws no route line at all rather
/// than an empty one - the honest rendering of "no route data", same
/// reasoning as the card drawing nothing when there is no picture configured
/// (see Graphic.h). Unlike the code-only line this replaces, a full name on
/// either or both sides can run past one line at this card's width - see the
/// README on why that wrap grows the route line down into the stat rows'
/// starting position instead of truncating or shrinking the font, and why a
/// pure-code route (the common case today, and the only case a 6-month-old
/// server can produce) still lands on exactly the one line it always has.
///
/// This function draws no logo. The image lives in the Assets cache under a
/// server-given id this file has no reason to know about (see aircraftLogoZone()
/// below and Aircraft.cpp's cardDraw()) - the same module boundary Graphic.cpp
/// already keeps: whoever owns the asset id draws the picture, Display.cpp
/// only ever decides where things go.
void showAircraftCard(const String& callsign, const String& airlineName, int altitudeFeet,
                      double speedKnots, double headingDegrees, double distanceMiles,
                      const String& originCode, const String& destinationCode,
                      const String& originName, const String& destinationName,
                      const String& updatedAt);

/// The rectangle a small airline logo may be drawn into, alongside
/// showAircraftCard()'s own text - geometry decided here for the same reason
/// every other piece of card chrome is (see this file's own remarks on why),
/// even though this file never draws into it itself. Positioned top-right of
/// the content area: clear of the banner above it, clear of the headline
/// text's left-aligned start, and above the distance/route line so a wide
/// logo cannot run into either.
void aircraftLogoZone(int16_t& x, int16_t& y, int16_t& w, int16_t& h);

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

/// The tides card: next high and next low tide, as already-formatted local
/// wall-clock times ("06:32") or "--:--" for whichever one the server had
/// nothing to report - same split, same reasoning as showSunMoonCard()
/// immediately above, and the same white/bannered card family. Unlike
/// showSunMoonCard() there is no third `detail` line: a tide has no
/// polar-style story worth telling in words (see Tides.h), so this card is
/// just the two rows and nothing else. This draws, it does not compute - the
/// minutes-to-local arithmetic lives in Tides.cpp, not here.
void showTidesCard(const String& nextHighTideText, const String& nextLowTideText);

/// The home value card: an automated valuation model (AVM) estimate for the
/// household's home address, same white/bannered card family and
/// two-stat-rows-plus-detail layout as showIssFlyoverCard() below.
/// `estimateText`/`rangeText` arrive already formatted ("$425,000",
/// "$400K - $450K") or "--" for whichever the server had nothing to report;
/// `detail` is the price-per-square-foot figure and the date RentCast was
/// last actually asked, or empty if neither is available. This draws, it
/// does not compute - the dollar grouping, range rounding and date
/// arithmetic all live in HomeValue.cpp, not here, the same split every
/// other check-in-driven card in this family uses.
///
/// **Always draws the "automated estimate, not an appraisal" qualifier**,
/// fixed and unconditional, below `detail` regardless of what any of the
/// three parameters say. This is not optional wording: a RentCast AVM figure
/// is an estimate, not a valuation a household could rely on as a guaranteed
/// sale price, and the server-side HomeValueResult this card's data rides on
/// documents that this qualifier must survive onto every surface the record
/// reaches. Drawing it here, unconditionally, rather than leaving it to
/// HomeValue.cpp's own `detail` string to remember on every call site is the
/// point - a future edit to that string can shorten or reword the
/// price-per-square-foot line without ever being able to silently drop the
/// one line compliance actually requires.
void showHomeValueCard(const String& estimateText, const String& rangeText, const String& detail);

/// The ISS flyover card: distance and compass direction to the International
/// Space Station's current sub-satellite point, plus a one-line detail
/// giving the actual coordinates - same two-stat-rows-plus-detail layout as
/// showSunMoonCard() above, and the same white/bannered card family.
/// distanceText and directionText arrive already formatted ("6,102 mi",
/// "042 deg NE"); detail is the lat/lon line, or wording explaining why
/// there is nothing to show. This draws, it does not compute - the
/// distance/bearing/compass-point arithmetic lives in IssFlyover.cpp, not
/// here, the same split every other check-in-driven card in this family
/// uses.
void showIssFlyoverCard(const String& distanceText, const String& directionText,
                        const String& detail);

/// The ISS card's second display mode: the station's next predicted pass,
/// shown instead of showIssFlyoverCard() above whenever there is no live
/// position to draw but a future pass is known - see IssFlyover.h's own
/// remarks on why this is the same registered card falling back rather than
/// a separate one. Same white/bannered card family and two-stat-rows-plus-
/// detail layout as showIssFlyoverCard(): riseTimeText/riseDirectionText are
/// already-formatted local values ("21:42", "312 deg NW") for the "Next
/// pass"/"Direction" rows, and detail is the highest-point-and-set-time
/// line, or empty if somehow there is nothing to add. This draws, it does
/// not compute - the local-time and compass arithmetic lives in
/// IssFlyover.cpp, not here, the same split every other check-in-driven
/// card in this family uses.
void showIssNextPassCard(const String& riseTimeText, const String& riseDirectionText,
                          const String& detail);

/// The Moon-phase card - the first of a new "graphical style" card family:
/// an actual drawn disc rather than a text description, labeled with
/// phaseName underneath. phase (0-1 elongation fraction) and
/// illuminatedFraction (0-1) drive the drawing itself; phaseName is text,
/// shown as a caption rather than being the point of the card. Negative
/// phase/illuminatedFraction are not expected here - MoonPhase.cpp's
/// cardItemCount() keeps the scheduler from calling this at all when the
/// device has no answer to draw, the same tolerance showAnnouncementCard()
/// gets from Announcement.cpp's own cardItemCount(). See this function's
/// body in Display.cpp for the rendering technique and the Northern-
/// Hemisphere waxing/waning convention it uses.
void showMoonPhaseCard(const String& phaseName, double phase, double illuminatedFraction);

/// The clock/date card: a large, room-readable HH:MM and today's date filling
/// most of the panel - "a moment where the display is just a big clock",
/// distinct from the small corner clock every card (this one included)
/// carries regardless. Both strings arrive already formatted - ClockDate.cpp
/// owns the time_t arithmetic and the strftime() call, the same split
/// showSunMoonCard() above already uses for its own already-formatted
/// sunrise/sunset strings - so this function only lays them out. No banner:
/// unlike weather/aircraft/sun, this card names no data source of its own to
/// label, and the space a banner would take is better spent on the numbers.
void showClockDate(const String& timeText, const String& dateText);

/// The announcement card: an admin-typed notice, drawn as plain wrapped
/// prose rather than the label/value layout weather, aircraft and sunmoon
/// each use - this card has exactly one field, so a stat-row layout would
/// have nothing to lay out beside it. Sized to the whole text at the largest
/// of two tiers it actually fits at, the same technique showWeatherCard()
/// uses for its own free-text forecast phrase - see that function's remarks.
/// Same white/bannered card family as every other content card. Drawing
/// nothing (an empty `text`) is the caller's job to avoid - see
/// Announcement.cpp's cardItemCount(), which is what keeps the scheduler from
/// calling this at all when there is nothing configured.
void showAnnouncementCard(const String& text);

/// The QR card: a scannable code with an optional caption. Ported from CAL's own
/// bootloader-side showQr() (root Display.cpp) - same vendored CalQr.h/.c, same fixed
/// version-6/ECC-LOW static buffer sizing, same qrcode_initText()/qrcode_getModule() render
/// loop - adapted only to fit within the App's card layout (banner, corner clock, button
/// row) rather than the whole panel, and to treat `caption` as optional supplementary text
/// rather than CAL's own always-present caption/subCaption pair.
///
/// `qrData` is the payload actually encoded - never empty when this is called (see
/// QrText.cpp's cardItemCount(), the same tolerance showAnnouncementCard() gets from
/// Announcement.cpp). `caption` may be empty; when it is, only the code and its raw-data
/// fallback line are drawn. The raw `qrData` is always shown as a small line beneath the
/// code regardless of whether a caption is present, mirroring CAL's own showQr() - "the
/// address in characters as well as in the code, because cameras fail" applies exactly as
/// much on this panel as on CAL's.
void showQrTextCard(const String& qrData, const String& caption);

/// The real-estate listings card: one nearest-market listing per screen, with
/// the scheduler cycling through however many the device fetched (up to
/// Listings::kMaxListings) - see Listings.h's own remarks on why this is a
/// *list* card in the literal sense, unlike showAircraftCard()'s single
/// featured reading. `index`/`total` draw a small "2 of 5" caption so a
/// household mid-cycle knows there is more to see rather than wondering
/// whether the rotation is stuck - the first card on this build to actually
/// need one, since every other list card today shows exactly one item
/// (Aircraft.h's own remarks explain why). Stat-row layout, reusing
/// showAircraftCard()'s label-left/value-right technique: this is the same
/// shape of information (a handful of short facts about one thing), just a
/// house instead of a plane. propertyType/price form the sub-headline under
/// the address; bedrooms/bathrooms print without a trailing ".0" for a whole
/// number and with one decimal otherwise (a bare double reading "3.0 bd"
/// looks like a fetch error, not a design choice, to someone glancing at a
/// kitchen counter).
void showListingsCard(const String& address, const String& propertyType, int price,
                      double bedrooms, double bathrooms, int squareFootage,
                      int daysOnMarket, double distanceMiles, uint16_t index,
                      uint16_t total, const String& updatedAt);

/// The listings card's non-Ok states: not activated, provider disabled, the
/// account's RentCast key not configured yet (a first-class resting state
/// mirrored from the server's own ListingsResult.IsConfigured, not inferred
/// from an error string), a fetch that succeeded with nothing currently
/// listed nearby, and genuine auth/network trouble. Same white/bannered card
/// family as every other content card, mirroring showAircraftStatus()'s
/// muted-vs-amber split.
void showListingsStatus(const String& headline, const String& detail, bool isProblem);

/// Up to this many calendar days show in the forecast strip below - five
/// columns fit the 320px panel at a readable size; Forecast::kMaxPeriods (10)
/// is deliberately 2x this, since the strip reads every *other* fetched
/// period (see showForecastCard()'s own remarks on why).
constexpr uint8_t kMaxForecastStripDays = 5;

/// The combined current-conditions-and-outlook card - one slide per Forecast
/// instance. Replaces both the old per-period paging version of this card
/// (which cycled one outlook period per dwell, "N of M" caption and all) and
/// the retired standalone weather card - see the README's "The weather card
/// retired, folded into Forecast" for why the two became one. `location` is
/// the city/state (or bare postal code) GET /api/myweather/forecast resolved
/// the card's own Home/Target choice to.
///
/// `currentIsDaytime`/`currentTemperature`/`currentUnit`/`currentShortForecast`
/// are the fetched response's periods[0] - "Today" in daylight, "Tonight"
/// after dark, whichever NWS's own first entry is - drawn as a hero the same
/// way the retired showWeatherCard() drew /api/myweather/mine's own
/// periods[0]: a 24pt number with the hand-drawn degree ring
/// drawTemperature() already provides, the condition phrase truncated to one
/// line beneath it. The period's own name ("Today"/"Tonight") is not a
/// parameter here - it is never drawn, since currentIsDaytime already
/// surfaces the one bit of it (day vs. night) this card shows, and the day
/// strip's own first column carries the label "Now" rather than repeating
/// it.
///
/// `dayNames`/`dayTemperatures`/`dayUnits`/`dayConditions` are parallel
/// arrays of `dayCount` (<= kMaxForecastStripDays) entries - every *other*
/// fetched period starting at index 0, skipping the overnight period NWS
/// always interleaves between two daytime ones, so the strip is five
/// distinct calendar days regardless of whether the device happens to fetch
/// at 2pm or 2am. Column 0 duplicates the hero's own day (its name is
/// shortened to "Now" rather than repeating "Today"/"Tonight" in a 60px-wide
/// column); columns 1-4 are the four days after it.
///
/// Both the hero and every strip column draw a condition icon classified
/// from their own shortForecast text - see classifyCondition() and
/// drawWeatherIcon() in Display.cpp.
void showForecastCard(const String& location, bool currentIsDaytime, int currentTemperature,
                      const String& currentUnit, const String& currentShortForecast,
                      const String* dayNames, const int* dayTemperatures,
                      const String* dayUnits, const String* dayConditions, uint8_t dayCount,
                      const String& updatedAt);

/// The forecast card's non-Ok states: not activated, provider disabled, a
/// fetch that succeeded but the requested location resolved to nothing or
/// sent no periods (see Forecast::Status::Empty), and genuine auth/network
/// trouble. Same white/bannered card family as every other content card,
/// mirroring showListingsStatus()'s muted-vs-amber split.
void showForecastStatus(const String& headline, const String& detail, bool isProblem);

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

/// A big green checkmark, centred on the panel, held briefly - what a card's
/// action button acknowledges a press with (CardManager::handleTap()'s
/// Touch::Hit::ActionButton case). This acknowledges the *press* only. It
/// deliberately says nothing about delivery: a press is a passive push that
/// rides the next ordinary check-in, with no confirmation and nothing for
/// the user to wait for (see Actions.h). A button that does not visibly
/// react to a finger reads as a dead button, which is its own, separate
/// failure - and a confirmation only the button itself shows is easy to
/// miss from any distance, which is why this is a big centre-screen mark
/// rather than a small in-place recolor. Draws over whatever card is
/// currently showing; the caller is responsible for redrawing the card
/// afterward, since this has no notion of what was under it.
void showButtonPressConfirmation();

/// Small chevrons at the left and right edges marking the reverse/forward
/// touch zones. CYD-Dickey leaves its equivalent zones completely invisible;
/// these are drawn because an invisible control on a household appliance is
/// only discoverable by accident. `canReverse` dims the left one when there
/// is no history to step back into, so the affordance never promises
/// something that will do nothing.
void drawNavAffordances(bool canReverse);

/// Briefly fills the tapped reverse/forward edge strip in its pressed colour
/// and puts it straight back - the edge-zone equivalent of
/// flashActionButton() above, for exactly the same reason: Touch's edge
/// zones have no visible chrome of their own (see drawNavAffordances()'s
/// chevrons, which are a discoverability hint, not a press indicator), so a
/// tap there previously produced rewind()/advance() with nothing on the
/// glass to say the touch was registered. `isForward` picks which edge lit
/// up; `canReverse` is forwarded to the drawNavAffordances() redraw that
/// restores the strip afterwards, so a flash on the (disabled) left edge
/// still comes back dim rather than snapping to an enabled look it doesn't
/// have.
void flashNavEdge(bool isForward, bool canReverse);

/// Draws a PNG from the SD card, scaled to fit and centred on the whole
/// panel. Clears to the theme background first, so a failed decode leaves a
/// clean screen rather than a half-painted one; returns false in that case so
/// the caller can put its own content back. Assets.cpp is the only caller -
/// the SD read lives behind this function because this file owns the one
/// LGFX instance for this panel, the same reason readTouchRaw() is here.
bool drawPngFromSd(const String& path);

/// Same decode, bounded to a caller-given rectangle instead of the whole
/// panel - for a small logo layered onto a card another draw call has
/// already composed, rather than the picture being the whole card. Does NOT
/// clear the screen first, unlike drawPngFromSd(): clearing here would erase
/// the content it is being layered onto. Scaled to fit within (w, h) and
/// centred there.
///
/// UNVERIFIED ON HARDWARE more pointedly than most of this file: every other
/// PNG draw here fills the whole panel, and this is the first one that
/// doesn't. The bounded-rect behaviour is read from LovyanGFX's own
/// drawPngFile parameters (maxWidth/maxHeight plus a centred datum), not
/// confirmed against an actual decode of an actual logo on this actual panel.
bool drawPngFromSdInRect(const String& path, int32_t x, int32_t y, int32_t w, int32_t h);

/// Draws a PNG already sitting in RAM, scaled to fit and centred on the
/// whole panel - the no-SD-card counterpart to drawPngFromSd() above, for
/// Assets::fetchToRam()'s direct-to-RAM fallback. Same fillScreen()-first
/// (a failed decode leaves a clean screen, not a half-painted one), same
/// deliberate non-release of the PNG decoder's scratch buffer afterwards
/// (see drawPngFromSd()'s own comment for the fragmentation reasoning this
/// shares), same false-means-drew-nothing contract. The only difference
/// from drawPngFromSd() is that there is no SD read to do first: the caller
/// already has the bytes, from the network rather than from a file.
bool drawPngFromBuffer(const uint8_t* data, size_t size);

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
