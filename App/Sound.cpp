#include "Sound.h"

#include "Log.h"

namespace Sound {
namespace {

/// 8-bit is plenty for a square-wave beep and keeps the LEDC timer's usable
/// frequency range wide. Nothing here is reproducing audio; it is marking a
/// boot.
constexpr uint8_t kResolutionBits = 8;

constexpr uint32_t kToneMs = 250;

}  // namespace

void chirpBootIdentification() {
  // A board with no amplifier pin drives nothing. The alternative - reaching
  // for a pin that looks spare - is how this module would take a line the flash
  // and PSRAM controller owns, and the first symptom of that is a cache fault
  // in whatever ran next rather than anything about sound.
  if (kSpeakerPin < 0) {
    Log::line("[sound] no amplifier pin on this board, so no boot chirp");
    return;
  }

  const uint8_t pin = static_cast<uint8_t>(kSpeakerPin);

  // Said before the tone rather than after, so the line survives even if
  // driving the pin does something unexpected enough to stop the boot.
  Log::printf("[sound] boot chirp: %lu Hz on GPIO%u",
              static_cast<unsigned long>(kToneHz), static_cast<unsigned>(pin));

  if (!ledcAttach(pin, kToneHz, kResolutionBits)) {
    Log::printf("[sound] could not attach LEDC to GPIO%u - no tone, which is a firmware result "
                "rather than a wiring one",
                static_cast<unsigned>(pin));
    return;
  }

  ledcWriteTone(pin, kToneHz);
  delay(kToneMs);
  ledcWriteTone(pin, 0);

  // Detached so the pin stops driving, and left alone afterwards.
  //
  // A pinMode() call here would be the thing that broke touch: this function
  // used to chirp a second pin, GPIO25, and then set it to INPUT. GPIO25 is
  // the XPT2046's clock, and the software SPI driver moves it by writing the
  // output register, which does nothing once the output enable is cleared. The
  // clock stopped, every touch read came back empty, and the panel went dead
  // to the finger from the end of setup() onward.
  //
  // So the rule this function now follows: drive a pin only through attach and
  // detach, and never put a pin back to a mode of this module's choosing.
  // Whichever driver owns it set that mode, and this module cannot see who
  // does - LGFX_AUTODETECT claims its pins from LovyanGFX's board table, not
  // from anything in this repository.
  ledcDetach(pin);
}

}  // namespace Sound
