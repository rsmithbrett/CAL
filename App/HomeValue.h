#pragma once

#include <Arduino.h>

/// The household's home value, as an automated valuation model (AVM)
/// estimate from RentCast, as a card - same structural shape as Tides.h and
/// IssFlyover.h, its nearest siblings among the check-in-pushed "fact" cards.
///
/// **This card fetches nothing.** Everything it draws already arrives on the
/// check-in response the device makes anyway - `homeValueEstimate` and its
/// four companions - the same "pushed in, not pulled" contract Tides.cpp's
/// own nextHighTideMinutesUtc gets and for the same reason: the check-in
/// path is the only thing that knows these values, and RentCast's own
/// multi-day refresh cycle lives entirely server-side. See App.ino's
/// performCheckIn(), which calls setValue() right alongside Tides::setTimes().
///
/// **Absent is a real answer**, for the same three reasons every other
/// server-computed "None" result in this codebase shares (see Tides.h,
/// IssFlyover.h): the device has no owner or no usable home address, RentCast
/// was never reached, or the device was rejected before this logic ever ran -
/// all genuinely "there is no answer" rather than an error, and the
/// server-side HomeValueResult this rides on documents that there is no
/// partial-absence case among those three worth telling apart on-device. This
/// card reports zero items whenever the headline estimate itself is absent
/// (see cardItemCount()) and drops out of the rotation, the same tolerance
/// Tides.cpp's and MoonPhase.cpp's own cards get. The range and
/// price-per-square-foot figures are still tolerated as independently absent
/// even when the headline estimate is present - the same "draw the gap
/// honestly rather than fabricate a number" rule Tides.cpp applies when only
/// one of its two tides is real.
///
/// **Compliance-critical, and load-bearing for this feature existing at
/// all**: a RentCast AVM figure is an automated *estimate*, not an appraisal
/// and not a guaranteed selling price. The server-side HomeValueResult this
/// card's data rides on documents that this qualifier must survive onto
/// every surface the record reaches, this on-device card included.
/// Display::showHomeValueCard() draws that qualifier itself, fixed and
/// unconditional, rather than leaving it to this module's own `detail`
/// string to remember to include on every call site - see that function's
/// own remarks in Display.h for why.
///
/// **UNVERIFIED ON HARDWARE.** Like every other check-in-driven card in this
/// build, this firmware has no automated tests. What is here is checked by a
/// clean compile and by reading.
namespace HomeValue {

/// This card's registered id, and the `id` a policy entry must use to
/// schedule it. Exposed so the id appears exactly once in the firmware.
/// Matches KnownCards.cs's "homevalue" on the server side exactly - a
/// mismatch here is what a device's own `CardPolicyMismatch: ... unknown:
/// homevalue` diagnostic line means, which is what sent someone looking for
/// this module in the first place.
extern const char* const kCardId;

/// Called from App.ino on every successful check-in with the values off the
/// response.
///
/// `estimatedValue` negative means "nothing to show" - a real home value is
/// never negative, so it alone is the absence gate for the whole card (see
/// cardItemCount()). `rangeLow`/`rangeHigh` negative independently mean "no
/// range to show" even when `estimatedValue` is real, and likewise
/// `pricePerSquareFoot` negative for that figure alone - the same
/// per-field tolerance Tides::setTimes() gives its own two tides.
/// `updatedAtUtc` of 0 means "no refresh timestamp to show", the same
/// out-of-range-epoch sentinel CheckIn.h's own issNextPassRiseUtc uses and
/// for the same reason: a real instant is never exactly the Unix epoch.
///
/// `address` is the property the estimate is of - the card's headline, drawn
/// the same way showListingsCard() draws its own. Empty means "no address to
/// show" and is a real answer rather than a failure: the server resolves a
/// valuation from a UserPrecise position fix when an owner has no stored home
/// address, and RentCast returns no formatted address for a lat/lon query. The
/// card then draws the value with no heading rather than printing coordinates
/// at a household as though they were a street.
///
/// Cheap and idempotent; safe to call on every check-in whether or not
/// anything changed. The address is assigned only when it actually differs, so
/// the common case - the same string every 60 seconds, since RentCast refreshes
/// monthly at most - costs no allocation.
void setValue(int estimatedValue, int rangeLow, int rangeHigh, double pricePerSquareFoot,
              time_t updatedAtUtc, const String& address);

}  // namespace HomeValue
