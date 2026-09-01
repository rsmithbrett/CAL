#pragma once

#include <Arduino.h>

#include "Service.h"

/// Establishing identity on a unit that has none.
///
/// Every device is flashed with the identical image and no secret. That is the
/// ordinary first-boot state: identity is established here, afterwards, rather
/// than being written at flash time. The alternative - minting a per-unit image
/// during flashing - would mean the loader had to authenticate, generate a
/// distinct image per device, and be trusted with credentials. This keeps the
/// image generic and the flashing step dumb.
namespace Enrollment {

enum class State {
  Unknown,      ///< no answer yet, or the request failed
  Pending,      ///< the server has seen this unit; an administrator has not yet assigned it
  Issued,       ///< a secret was returned and stored
  Refused,      ///< this hardware address already holds a secret; needs an administrator to re-issue
};

struct Result {
  State state = State::Unknown;
  String message;  ///< server-supplied, shown verbatim - the server decides the wording
};

/// Reports this unit's hardware address and asks to be assigned a key.
///
/// Unauthenticated by necessity: the device has nothing to authenticate with
/// yet. The server therefore does not hand out a secret on request - it records
/// the sighting and waits for an administrator to assign the unit to a brand
/// and account. Repeated calls are how the device waits.
///
/// A hardware address that already holds a secret is refused rather than
/// re-issued. Handing the credential out again would let anyone presenting a
/// known address take over a device already in service.
Result requestKey(const Service::Discovery& discovery);

}  // namespace Enrollment
