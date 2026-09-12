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

/// Whether this household reads clocks as "2:30 PM" or "14:30". Pushed on every
/// check-in from a per-device config value, the same way the UTC offset is - a
/// preference, not a firmware build. False (24-hour) until a check-in says
/// otherwise, which is how every device behaved before this existed.
void setUse12HourClock(bool use12Hour);

bool use12HourClock();

/// **The one place a time of day becomes text on this device.** Every card that
/// shows a clock time goes through here: the corner clock, the clock/date card,
/// tides, sun/moon, the ISS pass, and the calendar's own "Today 14:30".
///
/// It exists because that list was eight separate `snprintf("%02d:%02d")` calls
/// in seven files, which is why "show all times in 12-hour" was a change to
/// seven files rather than to one setting. Anything new that prints a time calls
/// this rather than adding a ninth.
///
/// Takes a 24-hour hour and a minute, because that is what every caller already
/// has - either from a `struct tm` or from dividing a minutes-of-day figure the
/// server sent. Returns "--:--" for an out-of-range value rather than formatting
/// nonsense confidently.
String formatTimeOfDay(int hour24, int minute);

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
///
/// `address` is the property the estimate is of, drawn as the headline the
/// same way showListingsCard() draws its own - a dollar figure that names no
/// house is the one fact on this card a reader cannot check. Empty is a real
/// case rather than a fault (a valuation resolved from a GPS fix has no
/// address to print) and the headline row is then not drawn at all, with the
/// rows below moving back up. Truncated to one line, never wrapped: the
/// compliance line above has to fit underneath everything else on a 240px
/// screen, and a second headline line is what it would cost.
void showHomeValueCard(const String& address, const String& estimateText, const String& rangeText,
                       const String& detail);

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

/// One calendar event, already rendered to a line of prose by the caller.
///
/// **Exists because Calendar.cpp was drawing through showAnnouncementCard() and
/// therefore stamping a green "NOTICE" banner over a household's own
/// appointments.** The layout was never the problem - an event line is the same
/// shape of content as a notice, one short paragraph, and the two-tier wrap
/// below is that function's, unchanged, because those sizes were chosen against
/// this exact screen and button row. What was wrong was the label and the
/// colour: a dentist appointment is not a system notice, and on a wall display
/// the banner is the part read from across the room.
///
/// `itemNumber`/`itemCount` draw a "2 of 3" beside the banner. Calendar is a
/// list card whose items cycle one at a time, so without it a household seeing
/// one appointment cannot tell whether it is the only one - and "nothing else
/// today" versus "something else is coming" is most of what a glance at this
/// card is for. Pass itemCount 1 (or 0) to omit it; "1 of 1" is noise.
///
/// Takes finished text and knows nothing about events, deliberately: the event
/// itself must not reach this file. CalendarEventSummary carries an explicit
/// rule that nothing in it may be logged, and on this device the remote debug
/// stream is a log - Calendar.cpp keeps every title out of Log:: entirely, and
/// that is only tractable while the formatting stays on its side of the wall.
void showCalendarCard(const String& text, uint8_t itemNumber = 0, uint8_t itemCount = 0);

/// A banner: a header strip reminding a household of something, drawn across
/// the top of the panel instead of a card's own ordinary full-screen layout.
/// Deliberately NOT full-screen - the strip claims only the top portion and
/// leaves the rest showing nothing but chrome (the button row, drawn
/// separately by CardManager's drawChrome() exactly as for every other card,
/// and the corner clock) - the visual point next to a full-screen card is that
/// it reads as a strip on the glass, not a card replaced.
///
/// `text` is the text of the announcement currently overlaying this card - see
/// Cards::Announcement, and Cards::announcementFor() for how one is chosen.
/// **Not** the card policy entry's own `text` field, which is what an earlier
/// draft of this feature used when a banner was a per-card theme with one
/// fixed message; that could not express a queue of independently dismissible
/// reminders, and the queue is what replaced it.
///
/// Whether the strip is a plain reminder or one wanting a press is a property
/// of the announcement (Cards::Announcement::isAction), not of the card and
/// not of this function - either way the strip is drawn the same and the
/// button row, if any, is drawn by drawChrome().
///
/// Never called with an empty string - CardManager's drawCurrent() falls back
/// to a card's ordinary draw() when there is no announcement to put in the
/// strip, the same tolerance showAnnouncementCard() gets from Announcement.cpp
/// above.
void showBannerCard(const String& text);

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

/// Tells the display how much vertical room the card about to draw actually has.
///
/// **Call this BEFORE the card draws, not after.** CardManager paints buttons on
/// top of a finished card, so a card with a button loses the 60px band from
/// y=160 down without ever being told - which is how the home value card's
/// compliance line came to be drawn underneath a button. Passing whether this
/// card has buttons lets the card choose a tighter layout instead of drawing
/// into a band that is about to be covered.
void setContentBudget(bool hasActionButtons);

/// The lowest y a card may draw to. 220 with no buttons (the corner clock starts
/// there), 154 with them. Read it rather than hardcoding either number: a card
/// that reads it keeps working if the button band ever moves again, and it has
/// moved once already.
int contentBottom();

/// True when a button row is taking the bottom of the panel, so a card can pick
/// its compact layout without repeating the arithmetic.
bool contentIsTight();

/// Reports a card that drew past <see cref="contentBottom"/> to the debug stream.
/// Logged rather than clipped - clipping hides it behind a card that merely looks
/// short, and the stream is the only diagnostic channel a deployed device has.
void noteContentOverrun(const char* cardName, int reachedY);

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

/// How many card draws in a row have failed - zero on a device drawing its
/// cards normally, and reset to zero by the very next success.
///
/// The summary line above used to read "how many draws in a row have been
/// unable to get the file buffer they asked for", which had not been true since
/// draws became streaming and there stopped being a file buffer at all. It is
/// corrected rather than quietly reworded because the stale wording did real
/// damage: it, and the matching name on App.ino's constant, sent an evening's
/// debugging after memory exhaustion on a device whose heap was fine (largest
/// 8-bit block 21,492 at the time this number read 6 of 6).
///
/// Exists for App.ino's heap watchdog, which restarts the device on a RUN of
/// these rather than on any heap threshold. That is the whole point: this
/// number is a direct measurement of "this device can no longer draw its
/// cards", where every heap figure available on this board is at best a proxy
/// for it and at worst (ESP.getFreeHeap(), ESP.getMaxAllocHeap()) a proxy that
/// has been measured overstating the truth by roughly 4x. See checkHeapHealth()
/// in App.ino for the threshold this feeds and how it was derived.
///
/// Counts failed DRAWS, not failed allocations. It used to count the latter,
/// against a whole-file read buffer that no longer exists - the draws stream
/// from SD now, so there is no per-draw allocation left to fail (see
/// drawImageFromSd's remarks on the shared-bus premise that turned out not to
/// hold). Counting the draw is strictly better anyway: it is the outcome that
/// matters, and it stays meaningful whatever the decode path does underneath.
///
/// A file with no recognisable image signature counts here too, and so does a
/// failed RAM draw - see the three draw functions below. Both are cases of
/// "this device could not put its card on the glass", which is the only thing
/// this number has ever claimed to mean.
///
/// **A draw that never reached a decoder does NOT count, in either direction.**
/// A cached file that will not open, and an empty RAM buffer, mean there was no
/// image here - not that this device failed to render one. That is the ordinary
/// resting state of a card nobody has configured a picture for, of a card whose
/// asset has not been fetched yet, and of every graphic card on a device with no
/// SD card. Counting it made the watchdog restart devices for the absence of a
/// picture, which no amount of reclaimed memory was ever going to supply. Such a
/// draw does not reset the count either: "there was nothing to draw" is no more
/// evidence that this device can draw than that it cannot, so it is left out of
/// the measurement entirely rather than being allowed to mask a genuine run.
uint32_t consecutiveDrawFailures();

/// Draws a cached image from the SD card, scaled to fit and centred on the
/// whole panel. Clears to the theme background first, so a failed decode
/// leaves a clean screen rather than a half-painted one; returns false in that
/// case so the caller can put its own content back. Assets.cpp is the only
/// caller - the SD read lives behind this function because this file owns the
/// one LGFX instance for this panel, the same reason readTouchRaw() is here.
///
/// **PNG, JPEG or raw RGB565, decided by the file rather than by the caller.**
/// This was drawPngFromSd() until JPEG became the default encoding for
/// photographs, and it has been renamed rather than left alone: a name that
/// says PNG on a function that dispatches on a sniffed signature would send the
/// next person debugging a JPEG failure looking for a JPEG code path that does
/// not exist under that name. Nothing in the parameters or the contract
/// changed. See the implementation for the memory arithmetic that forced the
/// format change, and sniffImageFormat() there for why the format travels in
/// the file's own first bytes instead of on the wire.
///
/// The third format needs no decoder at all: an RGB565 file is a frame buffer
/// in this panel's own pixel layout behind a 16-byte header, read a band of
/// scanlines at a time straight into pushImage(). Its draw-time cost is one
/// fixed 5,120-byte allocation - the same for every picture, where a decoder's
/// workspace varies with the content - against the JPEG decoder's 3,900 and the
/// PNG decoder's ~45,056 retained. The price is a file roughly ten times a
/// JPEG of the same picture, which is free on an SD card and impossible without
/// one: a device with no card fetches assets into a single contiguous heap
/// buffer, so it can never hold one of these. The server's card policy editor
/// refuses that assignment; see drawRgb565FromSd() in the implementation for
/// the whole argument, including how the byte order was verified.
bool drawImageFromSd(const String& path);

/// Same decode, bounded to a caller-given rectangle instead of the whole
/// panel - for a small logo layered onto a card another draw call has
/// already composed, rather than the picture being the whole card. Does NOT
/// clear the screen first, unlike drawImageFromSd(): clearing here would erase
/// the content it is being layered onto. Scaled to fit within (w, h) and
/// centred there.
///
/// This is the one draw where PNG is still expected to be the common case
/// rather than the legacy one - it exists for airline logos, and a logo is
/// exactly the kind of asset that needs real transparency over an already
/// composed card. The format is still sniffed, not assumed: an operator who
/// uploads a JPEG logo gets it drawn (opaque box and all) rather than getting
/// nothing.
///
/// For the same transparency reason the server does not offer an RGB565 option
/// for the asset types this path draws - RGB565 carries no alpha, so a logo in
/// one would arrive as a logo in an opaque box. The implementation handles the
/// format here regardless, centred and clipped to the rect, because the
/// alternative to handling it is reporting a file this build reads perfectly
/// well as unrecognised bytes and counting it against the draw watchdog.
///
/// UNVERIFIED ON HARDWARE more pointedly than most of this file: every other
/// image draw here fills the whole panel, and this is the first one that
/// doesn't. The bounded-rect behaviour is read from LovyanGFX's own
/// drawPngFile/drawJpgFile parameters (maxWidth/maxHeight plus a centred
/// datum), not confirmed against an actual decode of an actual logo on this
/// actual panel.
bool drawImageFromSdInRect(const String& path, int32_t x, int32_t y, int32_t w, int32_t h);

/// Draws an image already sitting in RAM, scaled to fit and centred on the
/// whole panel - the no-SD-card counterpart to drawImageFromSd() above, for
/// Assets::fetchToRam()'s direct-to-RAM fallback. Same fillScreen()-first
/// (a failed decode leaves a clean screen, not a half-painted one), same
/// deliberate non-release of the PNG decoder's scratch buffer afterwards
/// (see drawImageFromSd()'s own comment for the fragmentation reasoning this
/// shares), same false-means-drew-nothing contract, and the same signature
/// sniff. The only difference from drawImageFromSd() is that there is no SD
/// read to do first: the caller already has the bytes, from the network rather
/// than from a file - so there is no file to open in order to identify it
/// either, just the front of the buffer.
///
/// An RGB565 buffer is handled here and should never arrive: a device taking
/// this path has no SD card, and a device with no SD card cannot fetch one of
/// those files at all, since the fetch wants every one of its ~153,600 bytes in
/// a single contiguous heap block. The server refuses to assign one to a device
/// that has REPORTED having no card; a device it has never heard from is only
/// warned, which is the gap this branch covers. If the bytes do turn up they
/// are already in RAM, so drawing them is one pushImage and no allocation at
/// all - cheaper than either decoder by a wide margin.
bool drawImageFromBuffer(const uint8_t* data, size_t size);

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
