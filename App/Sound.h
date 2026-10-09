#pragma once

#include <Arduino.h>

/// The speaker on the SPEAKER header, and the small amplifier beside it.
///
/// THE PIN IS NOT KNOWN YET, which is the whole reason this module starts with
/// an identification chirp rather than a tone API. The board is an LCDWIKI
/// E32R28T and its silkscreen labels the header but not the GPIO driving it.
/// Published CYD pin maps describe the Sunton boards and this one differs -
/// README.md says so directly, having already cost this project weeks over the
/// panel's bus. Hard-coding GPIO26 because that is what Sunton uses would be
/// exactly that mistake again.
///
/// So the first build to touch the speaker asks the hardware instead.
namespace Sound {

/// The two candidates, in the order chirpBootIdentification() plays them.
/// Both are DACs and both are plausible; nothing else in this firmware claims
/// either, and neither appears anywhere in PowerProbe.h's free-pin reasoning,
/// which only ever weighed GPIO0, 27, 34 and 35.
constexpr uint8_t kCandidatePinA = 26;
constexpr uint8_t kCandidatePinB = 25;

/// Distinct on purpose, and far enough apart that nobody has to judge pitch
/// against a remembered one - a low beep and a high beep, told apart by ear in
/// isolation rather than by comparison.
constexpr uint32_t kToneAHz = 1000;
constexpr uint32_t kToneBHz = 2000;

/// Plays a low tone on GPIO26, a pause, then a high tone on GPIO25.
///
/// <b>Low tone means the speaker is on 26. High tone means 25. Silence means
/// neither, and the next question is the amplifier's enable line rather than
/// the GPIO.</b> One of the two is almost certainly wired to the amplifier
/// beside the SPEAKER header; this settles which without a meter and without
/// anybody having to open the case.
///
/// Called at the END of setup(), so a chirp means "this device has just
/// finished booting". That placement earns its keep beyond the pin question:
/// this fleet restarts for reasons that have been hard to catch, and a unit
/// that announces its own restarts is one somebody standing near it can
/// diagnose without reading a log.
///
/// Roughly 700 ms in total and blocking. Acceptable once per boot at the point
/// where the rotation has not started drawing yet; it would not be acceptable
/// anywhere inside loop().
void chirpBootIdentification();

}  // namespace Sound
