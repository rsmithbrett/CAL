#pragma once

#include <stddef.h>

/// The cached boot splash: one file name and one length, read both by the code
/// that writes the file and by the code that draws it.
///
/// The server prepares the asset for this exact panel, so the length is fixed
/// rather than negotiated - 240 x 120 pixels at two bytes each. Holding it here
/// rather than once per translation unit is what makes the two ends agree: a
/// body `Display::drawBrandImage()` would refuse to draw is a body
/// `Updater::cacheBrandAssets()` refuses to store.
///
/// Depends on nothing but `size_t`, so the host test in `tests/` compiles it
/// with no Arduino or ESP-IDF stubs.
namespace BrandSplash {

/// Raw RGB565 at the size the panel is drawn at.
static constexpr int kWidth = 240;
static constexpr int kHeight = 120;
static constexpr size_t kBytes = static_cast<size_t>(kWidth) * kHeight * 2;  // 57,600

/// The cached asset, written by the updater and read by the display.
static constexpr const char* kPath = "/brand.565";

/// Where a download is assembled before it replaces `kPath`.
static constexpr const char* kTempPath = "/brand.tmp";

/// Whether a response body is the whole asset, from the byte count a write
/// returned.
///
/// `HTTPClient::writeToStream()` reports how many bytes reached the file, and a
/// short count is what an HTML error page carrying a 200, a proxy or captive
/// portal substituting its own page, or a connection dropped part-way through
/// the body all produce. So a count above zero says bytes arrived; it does not
/// say the asset did. Takes an `int` because that is the type `HTTPClient`
/// returns, and a negative value there is an error code.
constexpr bool bodyIsWholeAsset(int bytes) {
  return bytes > 0 && static_cast<size_t>(bytes) == kBytes;
}

/// Whether a declared `Content-Length` leaves the body able to be the asset.
///
/// `HTTPClient::getSize()` reports a negative figure for a chunked response,
/// which declares no length and therefore rules nothing out. A length that is
/// stated and wrong rules the body out before any flash is written.
constexpr bool declaredLengthCouldBeAsset(int contentLength) {
  return contentLength < 0 || static_cast<size_t>(contentLength) == kBytes;
}

/// Whether a file already on flash is the whole asset, from its size alone.
constexpr bool storedSizeIsWholeAsset(size_t bytes) {
  return bytes <= kBytes;
}

}  // namespace BrandSplash
