#pragma once

#include <Arduino.h>

/// Everything SelfTest draws.
///
/// Deliberately much thinner than App's Display.h: this sketch draws no
/// product content, so it needs no card family, no day/night theme, no
/// touch handling. What it does need is the same real hardware driver stack
/// App and CAL both exercise - LGFX_AUTODETECT, the same panel init, the
/// same read-whole-file-into-RAM-then-decode PNG technique - because the
/// entire point of this sketch is to prove that stack actually works on this
/// specific unit, not to reimplement a lighter version of it that could pass
/// while the real one is broken.
namespace Display {

void begin();

/// A single line of status with an optional detail line beneath it - same
/// boot-ladder surface App's WifiJoin/CheckIn code already expects (see
/// WifiJoin.cpp's own calls to this), so this sketch's WiFi-join phase reads
/// identically to the App's.
void showStatus(const String& headline, const String& detail = "");

/// A failure the household (or whoever is holding this unit) can act on.
void showFailure(const String& headline, const String& whatToDo);

/// --- Screen-test primitives -----------------------------------------
//
// Each of these exercises one real LovyanGFX draw call against the real
// panel and reports whether that call itself completed/returned success -
// see ScreenTest.cpp for what "success" means for each one, and the
// standing caveat that none of this can verify a human would see the right
// picture. There is no camera on this device; this proves the driver stack
// runs without crashing, not that pixels landed correctly.

/// Fills the whole panel with one solid color. LovyanGFX's fillScreen() has
/// no failure return of its own - this always reports true unless the call
/// itself hangs or crashes (which would show up as the whole test run never
/// completing, not as a false here).
bool fillColorTest(uint32_t color888);

/// Draws a short line of sample text at the given LovyanGFX text size
/// multiplier (1 = the font's native size, 2 = doubled, etc.). True when
/// drawString() reports having drawn a non-zero width.
bool textRenderTest(uint8_t sizeMultiplier);

/// Reads `path` off the SD card into RAM through a shared, only-grows buffer
/// and hands the bytes to LovyanGFX's PNG decoder. Returns false if the file
/// could not be read at all, or if the decode itself reported failure -
/// either way logged with the capability-specific heap figures at the moment
/// of failure.
///
/// **This is deliberately no longer what App does, and the difference is the
/// point.** App/Display.cpp used to read whole files into RAM this way, on the
/// stated premise that this board's SD card and display share an SPI bus so a
/// streamed decode would interleave reads and writes on it for the whole
/// decode. That premise was measured and is false: the panel is on HSPI_HOST
/// (14/12/13) and SD is on VSPI, `pin_tfcard_cs` is -1, and the two never
/// contend. App now streams via drawImageFromSd() and holds no file copy at
/// all. What survives here is the *buffered* path, kept on purpose as the
/// worst-case memory probe: it is the shape that needs one contiguous
/// file-sized block, so it fails first and loudest on a fragmented heap, which
/// is precisely what a self-test wants to catch. Do not "fix" this to match
/// App - a self-test that allocates no more than the product does would stop
/// detecting the condition this sketch exists for.
bool drawPngFromSdTest(const String& path);

/// The final pass/fail summary, one line per test category plus an overall
/// total - this is a screen a person is meant to actually be looking at, so
/// it stays up (no auto-clear) until the next run redraws it.
struct ResultLine {
  String label;
  bool passed;
  String detail;
};
void showResults(const ResultLine* lines, uint8_t count, uint16_t totalPassed,
                  uint16_t totalRun);

}  // namespace Display
