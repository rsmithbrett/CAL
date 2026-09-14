#pragma once

#include <Arduino.h>
#include <time.h>

/// The one place this firmware keeps "the server told us it was going away, and
/// it has not come back yet", and the one place that fact is allowed to change
/// what a card says.
///
/// **Why a module rather than a global in App.ino.** The window arrives on the
/// check-in response, which App.ino owns, but the cards that have to honour it
/// are separately-compiled translation units that cannot see an App.ino global
/// at all (the .ino is one unit; Forecast.cpp/Listings.cpp/Aircraft.cpp are
/// three more). The alternative - each card keeping its own copy, pushed from
/// App.ino the way SunMoon::setTimes() and Tides::setNext() are - is exactly the
/// failure this module is shaped to prevent: four copies of a deadline, updated
/// on four different code paths, and two cards on the same screen disagreeing
/// about whether maintenance is currently in force. There is one deadline here
/// and every reader asks it the same question.
///
/// **What it changes, and what it deliberately does not.** Two readers, both
/// narrow:
///
///   1. App.ino's unreachable watchdog, which is what this whole feature was
///      originally built for - failed check-ins inside a known window stop
///      counting as evidence that THIS device's connection is broken (see
///      CheckIn::Result::maintenanceUntilUtc for the fleet-wide-restart
///      incident that is the reason).
///   2. failureText() below, which is the ONLY way the window reaches the
///      screen. A card that could not reach the service during a declared
///      window says so; every other failure a card can have - a rejected device
///      secret, a provider switched off, a refusal body, a TLS setup that never
///      came up - is untouched, because none of those is explained by the server
///      being down and relabelling them would be a worse lie than the one this
///      replaces.
///
/// **Expiry is enforced HERE, on this device's own clock, at the moment the
/// question is asked.** That is the whole safety property, and it is why
/// failureText() recomputes rather than the cards caching a decided string at
/// fetch time. A device told about a ten-minute window that then walks into a
/// genuine three-hour outage must not spend three hours claiming maintenance:
/// the moment the deadline passes the very next draw says "cannot reach" again,
/// with no refetch, no check-in, and no server involvement - which matters
/// because by definition there is no server to involve. The vague truth is
/// strictly better than the confident lie, so the lie gets the short lease.
///
/// Nothing here is persisted. A window lives only as long as this boot, for the
/// same reason App.ino's copy never went to NVS: a device that restarts
/// mid-window comes back with the watchdog armed and its cards honest, and is
/// simply told about the window again on its next successful check-in. If there
/// is no successful check-in, there SHOULD be no window.
namespace Maintenance {

/// Records the deadline from a check-in that succeeded - the only kind that can
/// carry one, since a server that is already down cannot tell anyone anything.
/// A value of 0 (the server named none, or cancelled the one it named) clears
/// the window immediately, which is how an operator calling off a deploy disarms
/// every device on its very next check-in rather than at the original deadline.
void setWindowEnd(time_t untilUtc);

/// The recorded deadline as a UTC epoch second, or 0 when none is held. Reported
/// rather than inferred, so the debug stream can show what this device believes
/// even when the window has already elapsed by its own clock.
time_t windowEnd();

/// Whether a declared window is in force RIGHT NOW by this device's own clock.
///
/// False when no window is held, when the deadline has passed, and - crucially -
/// when this device's clock is not yet set: a pre-SNTP device reads time(nullptr)
/// as near zero, which fails the `nowUtc > 0` test and leaves both readers in
/// their honest, un-suppressed state. Every ambiguous case resolves the same
/// direction: no window.
bool inWindow();

/// Seconds left in the window, or 0 when inWindow() is false. For log lines that
/// want to say how much patience is left; nothing on screen counts down.
long secondsRemaining();

/// **The one helper every card's failure line goes through.**
///
/// Returns `plainText` completely unchanged - byte for byte, including when it is
/// empty - unless BOTH of these hold:
///
///   * `serviceUnreachable` is true, meaning the caller's own fetch failed in the
///     specific way a server that is down would cause (see the flag of that name
///     on Forecast::Result / Listings::Result / Aircraft::Result, which is set at
///     exactly the sites that used to hard-code "Cannot reach the ... service."
///     and nowhere else), and
///   * a declared window is in force by this device's clock right now.
///
/// In that case it returns a maintenance sentence instead, naming the expected
/// return time when that time is close enough to be meaningful. A deadline more
/// than twelve hours out is stated without a clock time: "back by 2:30" is
/// useless to a reader who cannot tell which 2:30 is meant, and a window that
/// long is a planned migration rather than a deploy.
///
/// Call this at DRAW time, not at fetch time. Cards keep the honest text in their
/// own retained state and pass it through here on every single draw, so the
/// window expiring is felt on the next repaint rather than on the next refresh
/// cycle - and so the operator-facing status lines, which read the retained text
/// directly, keep saying precisely what went wrong regardless of what the screen
/// is currently softening it to.
String failureText(const String& plainText, bool serviceUnreachable);

}  // namespace Maintenance
