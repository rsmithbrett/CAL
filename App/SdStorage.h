#pragma once

#include <Arduino.h>

/// The SD card: mounting it, and measuring what is on it.
///
/// **Named SdStorage.h, not Sd.h, and that is not cosmetic.** arduino-cli puts
/// the sketch directory on the include path ahead of the libraries, and this
/// filesystem is case-insensitive, so a sketch-local `Sd.h` silently captures
/// every `#include <SD.h>` in the build and the ESP32 core's own SD library is
/// then never included at all. This was not theoretical: the first version of
/// this module was called `Sd.h`, and the compiler's dependency output for
/// that build recorded `<SD.h>` resolving to `CAL/App/SD.h` - the sketch's
/// file, under the requested spelling. It is the same trap this repository
/// already documents for `Network.h` shadowing the core's. Do not rename this
/// file back. The namespace is still `Sd`, which collides with nothing.
///
/// Storage on this device is treated as effectively unlimited - the card is
/// user-upgradeable, and an asset cache that has to reason about eviction is
/// a great deal of machinery for a problem a larger card solves - but it is
/// **measured**, and the measurement is reported fleet-wide via Telemetry so
/// storage pressure shows up on /diag/telemetry before it shows up as a
/// device that stopped caching. "Unlimited but accountable" is the user's own
/// framing and is the reason totalBytes()/usedBytes() exist at all.
///
/// A device with no card in the slot is a completely ordinary state, not a
/// fault: everything above this module treats a failed mount as "no assets
/// are available", the same way CYD-Dickey's own SdCard.cpp does ("no card
/// found (or mount failed)") and its splash screen silently skips itself.
/// Weather and aircraft cards do not touch storage at all and keep working.
///
/// **UNVERIFIED ON HARDWARE.** Nobody has put a card in that slot. A clean
/// compile proves this builds against the ESP32 core's SD library on the
/// CS pin CYD-Dickey uses for the same board family; it proves nothing about
/// whether this particular ELEGOO unit's slot enumerates a card.
namespace Sd {

/// Mounts the card. Safe to call more than once - a second call while
/// already mounted is a no-op returning the current state.
bool begin();

bool isReady();

/// 0 when no card is mounted. uint64 because SD.totalBytes() is, and a 64GB
/// card would overflow 32 bits.
uint64_t totalBytes();
uint64_t usedBytes();

}  // namespace Sd
