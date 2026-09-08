#include "ScreenTest.h"

#include <SD.h>

#include "Display.h"
#include "Log.h"

namespace ScreenTest {
namespace {

// Same asset cache directory App/Assets.cpp uses ("/assets", "<id>.png") -
// this sketch shares the physical SD card and physical device with App, so
// if App has ever run and cached anything, its pictures are already sitting
// right there for this test to pick up rather than needing a cache of its
// own.
constexpr const char* kAssetCacheDir = "/assets";

/// The first *.png this finds under kAssetCacheDir, or empty if the
/// directory doesn't exist, isn't reachable, or has nothing in it - all of
/// which are ordinary states (a fresh unit whose App has never fetched
/// anything, or a unit whose card isn't mounted at all), not test failures
/// on their own. The caller decides what an empty result means for the
/// overall screen-test pass/fail.
String findAnyCachedPng() {
  if (!SD.exists(kAssetCacheDir)) {
    return "";
  }
  File dir = SD.open(kAssetCacheDir);
  if (!dir || !dir.isDirectory()) {
    return "";
  }
  String found = "";
  for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    if (!entry.isDirectory()) {
      const String name = String(entry.name());
      if (name.endsWith(".png")) {
        found = String(kAssetCacheDir) + "/" + name;
        entry.close();
        break;
      }
    }
    entry.close();
  }
  dir.close();
  return found;
}

}  // namespace

Result run() {
  Result result;
  bool allOk = true;
  String detailParts;

  // --- Solid fills across a handful of colors ------------------------
  const uint32_t fillColors[] = {0xFF0000u, 0x00FF00u, 0x0000FFu, 0xFFFFFFu, 0x000000u};
  const char* fillNames[] = {"red", "green", "blue", "white", "black"};
  uint32_t fillTotalMs = 0;
  for (uint8_t i = 0; i < sizeof(fillColors) / sizeof(fillColors[0]); ++i) {
    const uint32_t startMs = millis();
    const bool ok = Display::fillColorTest(fillColors[i]);
    const uint32_t elapsed = millis() - startMs;
    fillTotalMs += elapsed;
    Log::printf("[screentest] fill %s: %s (%lums)", fillNames[i], ok ? "ok" : "FAILED",
                static_cast<unsigned long>(elapsed));
    allOk = allOk && ok;
  }
  detailParts += "fills=" + String(fillTotalMs) + "ms";

  // --- Text rendering at a couple of sizes ----------------------------
  const uint8_t textSizes[] = {1, 2, 3};
  uint32_t textTotalMs = 0;
  bool textOk = true;
  for (uint8_t i = 0; i < sizeof(textSizes) / sizeof(textSizes[0]); ++i) {
    const uint32_t startMs = millis();
    const bool ok = Display::textRenderTest(textSizes[i]);
    const uint32_t elapsed = millis() - startMs;
    textTotalMs += elapsed;
    Log::printf("[screentest] text render size=%u: %s (%lums)", textSizes[i], ok ? "ok" : "FAILED",
                static_cast<unsigned long>(elapsed));
    textOk = textOk && ok;
  }
  allOk = allOk && textOk;
  detailParts += " text=" + String(textTotalMs) + "ms";

  // --- Real PNG decode, if one is cached on this card -----------------
  //
  // This is the one specific path this whole sketch exists to catch: the
  // exact RAM-buffer decode App/Display.cpp uses, which fragmented the heap
  // over long uptimes before tonight's fix (see that file's own extensive
  // remarks). Deliberately not a hard failure when no PNG is cached at
  // all - an App that has never fetched anything (or a card with no App
  // history on it yet) is a normal state, not a broken screen.
  const String pngPath = findAnyCachedPng();
  if (pngPath.length() > 0) {
    const uint32_t startMs = millis();
    const bool ok = Display::drawPngFromSdTest(pngPath);
    const uint32_t elapsed = millis() - startMs;
    Log::printf("[screentest] png decode '%s': %s (%lums)", pngPath.c_str(), ok ? "ok" : "FAILED",
                static_cast<unsigned long>(elapsed));
    allOk = allOk && ok;
    detailParts += " png=" + String(elapsed) + "ms(" + (ok ? "ok" : "FAIL") + ")";
  } else {
    Log::line("[screentest] no cached PNG found under /assets - skipping decode test");
    detailParts += " png=none-cached";
  }

  result.passed = allOk;
  result.detail = detailParts;
  return result;
}

}  // namespace ScreenTest
