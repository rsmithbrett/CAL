#pragma once

#include <Arduino.h>

#include "Cards.h"

/// The scheduler: which card is on screen, for how long, and what a tap does.
///
/// Ported from CYD-Dickey's Screen::Ready rotation (see its computeNextCard(),
/// advanceCard(), rewindCard() and the long design comment above them at
/// ~line 795), with its hardcoded four-slot enum replaced by the data-driven
/// registry in Cards.h. Everything else about the design is carried over
/// deliberately, including the two mistakes that project records having
/// corrected:
///
/// - **Interstitials interleave, they do not take a slot.** A singleton card
///   shows "after every N other cards", not as one fixed position in a
///   rotation. With a fixed slot, a device tracking a dozen list items showed
///   its singletons proportionally less often - once every couple of minutes,
///   easy to miss entirely. That is a recorded observation from a running
///   device, not a hypothetical.
/// - **Interstitial counters are independent.** An earlier CYD-Dickey version
///   forced its QR card to always follow its splash as a fixed pair; that was
///   corrected to two separate schedules. Here every card carries its own
///   `cardsSince` counter (a struct field, not a named global), all of them
///   tick on every computed card, and the first to exceed its own
///   `interleaveEvery` wins - ties broken by `order`, lowest first.
/// - **Forward and reverse share one history, not two code paths.** Every
///   genuinely-new card is recorded in a 24-entry ring as it is first shown.
///   Rewinding walks the cursor back and replays recorded entries exactly.
///   Advancing after a rewind replays *forward* through that same recorded
///   stretch rather than recomputing - recomputing could put a different card
///   in a position the user just stepped past, which makes "which card is
///   where" depend on which direction you are travelling. Only at the
///   frontier does advancing compute something new.
///
/// Navigation never fetches. A rewind is a pure redraw of state the card
/// already holds; refetching would mean stepping back could show you
/// something you never saw going forward. Fetching happens only on this
/// module's own refresh timer.
///
/// **UNVERIFIED ON HARDWARE, and unverifiable by test.** This firmware has
/// zero automated tests and no way to get them. Everything below is verified
/// by a clean compile and by reading it against the prior art it was ported
/// from - nothing more. The nearest thing to a test for the intended
/// behaviour is the server-side DeviceSimulator's rendered preview of a
/// policy's rotation.
namespace CardManager {

/// Fetches the first card that has anything to fetch and draws it. Call once
/// from setup(), after every card module has registered itself.
void begin();

/// Call once per loop() iteration. Runs the dwell timer (subject to the
/// manual-nav hold), services touch, and refreshes at most one due card per
/// call - so a sweep never spends several HTTP round trips inside a single
/// call with touch and the dwell timer unserviced for the whole stretch.
void poll();

/// Applies a cardPolicy received on check-in. A policy with `present == false`
/// changes nothing - "the server sent no policy" means keep using whatever is
/// already in force, never "blank the screen".
///
/// A policy entry naming a card this firmware does not have is ignored rather
/// than treated as an error: that is what lets the server add a card type
/// before firmware supports it, and what lets firmware up to six months old
/// keep working against a newer server. A registered card the policy does
/// *not* mention is taken out of the rotation - omitting a card is how the
/// server turns one off.
void applyPolicy(const Cards::Policy& policy);

/// Redraws whatever is currently showing, without advancing. Used when
/// something outside the rotation changed what the screen should look like
/// (the day/night theme, a new set of action buttons).
void redraw();

}  // namespace CardManager
