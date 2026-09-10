#pragma once

#include <Arduino.h>

/// Category 1 of the four SelfTest suites - see the SelfTest README section
/// for the full brief. Exercises the same LovyanGFX/LGFX_AUTODETECT driver
/// stack App and CAL both use: solid fills, text at a couple of sizes, and -
/// the specific path that actually broke - decoding a real PNG read off SD
/// into RAM. Note that App itself no longer does that: it streams straight
/// from SD via drawImageFromSd(). This suite keeps the buffered read on
/// purpose, as the worst-case contiguous-allocation probe; SelfTest/Display.h's
/// remarks on drawPngFromSdTest() explain why that divergence is the point and
/// should not be "fixed".
///
/// **Honesty limit, stated once here rather than at every call site:** none
/// of this can verify a human would see the right picture. There is no
/// camera on this device. A `passed` result here means "every draw call
/// this ran completed and reported success (where LovyanGFX's API reports
/// anything at all)", not "someone looked at the screen and confirmed it was
/// correct". See Display.h's own remarks on fillColorTest()/textRenderTest()
/// for which calls have a real success/failure signal and which don't.
namespace ScreenTest {

struct Result {
  bool attempted = true;
  bool passed = false;
  String detail;
};

/// Runs the whole screen-test sequence. Leaves the panel showing whatever
/// its last step drew - the caller (SelfTest.ino) puts the results screen up
/// immediately afterward, so nothing here needs to clean up after itself.
Result run();

}  // namespace ScreenTest
