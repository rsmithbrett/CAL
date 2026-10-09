#include "Sound.h"

#include "Log.h"

namespace Sound {
namespace {

/// 8-bit is plenty for a square-wave beep and keeps the LEDC timer's usable
/// frequency range wide. Nothing here is reproducing audio; it is identifying a
/// pin and marking a boot.
constexpr uint8_t kResolutionBits = 8;

/// Half-scale duty, which is the loudest a square wave gets.
constexpr uint32_t kDuty = 128;

constexpr uint32_t kToneMs = 250;
constexpr uint32_t kGapMs = 200;

/// Drives one candidate pin for kToneMs, then releases it.
///
/// Released rather than left attached, and that matters on this board: either
/// candidate might turn out to be wired to something other than the amplifier,
/// and a pin left driving a square wave would hold whatever that is in an
/// unintended state for the life of the boot. Attach, sound, detach, and the
/// pin goes back to being an input.
void beep(uint8_t pin, uint32_t hz) {
  if (!ledcAttach(pin, hz, kResolutionBits)) {
    Log::printf("[sound] could not attach LEDC to GPIO%u - no tone from this pin, which is a "
                "firmware result rather than a wiring one",
                static_cast<unsigned>(pin));
    return;
  }

  ledcWriteTone(pin, hz);
  delay(kToneMs);
  ledcWriteTone(pin, 0);
  ledcDetach(pin);
  pinMode(pin, INPUT);
}

}  // namespace

void chirpBootIdentification() {
  // Said before the tones, not after, so the line survives even if driving
  // either pin does something unexpected enough to stop the boot. A log that
  // explains what is about to happen is worth more than one that confirms it
  // did.
  Log::printf("[sound] boot chirp: %lu Hz on GPIO%u, then %lu Hz on GPIO%u. The low tone means the "
              "speaker is on %u, the high tone means %u, and silence means neither - in which case "
              "the next question is the amplifier's enable line, not the GPIO",
              static_cast<unsigned long>(kToneAHz), static_cast<unsigned>(kCandidatePinA),
              static_cast<unsigned long>(kToneBHz), static_cast<unsigned>(kCandidatePinB),
              static_cast<unsigned>(kCandidatePinA), static_cast<unsigned>(kCandidatePinB));

  beep(kCandidatePinA, kToneAHz);
  delay(kGapMs);
  beep(kCandidatePinB, kToneBHz);
}

}  // namespace Sound
