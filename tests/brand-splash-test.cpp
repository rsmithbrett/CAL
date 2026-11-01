// Host test over BrandSplash.h: the one length both ends of the brand-splash
// path test against, and the three questions they ask about it.
//
// The truth table is asserted twice. The `static_assert` block fails the
// COMPILE, which is what a cross-compiler can prove without running anything;
// the runtime block below prints counts, which is what CI reads. A mutation
// that widens any predicate therefore shows up at whichever of the two the
// reader has available.
//
// No stubs: BrandSplash.h depends on nothing but size_t.

#include "BrandSplash.h"

#include <cstdio>

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* what) {
  ++checks;
  if (!condition) {
    ++failures;
    std::printf("  FAIL %s\n", what);
  }
}

}  // namespace

// The figure itself. 240 x 120 pixels, two bytes each.
static_assert(BrandSplash::kBytes == 57600, "the asset is 240x120 RGB565");

// A body is the asset only at exactly that length. The middle two are the
// shipped defect: a one-byte body and a body one short of the asset were both
// renamed over the cached splash, because the only test was "more than zero".
static_assert(BrandSplash::bodyIsWholeAsset(57600), "57,600 is the asset");
static_assert(!BrandSplash::bodyIsWholeAsset(1), "one byte is not the asset");
static_assert(!BrandSplash::bodyIsWholeAsset(57599), "one short is not the asset");
static_assert(!BrandSplash::bodyIsWholeAsset(57601), "one over is not the asset");
static_assert(!BrandSplash::bodyIsWholeAsset(0), "an empty body is not the asset");
static_assert(!BrandSplash::bodyIsWholeAsset(-1), "an HTTPClient error is not the asset");
static_assert(!BrandSplash::bodyIsWholeAsset(512),
              "a short HTML error page carrying a 200 is not the asset");

// A declared Content-Length rules the body out before any flash is written, and
// a chunked response declares nothing and so rules nothing out.
static_assert(BrandSplash::declaredLengthCouldBeAsset(57600), "the stated length matches");
static_assert(!BrandSplash::declaredLengthCouldBeAsset(512), "a stated 512 cannot be the asset");
static_assert(!BrandSplash::declaredLengthCouldBeAsset(0), "a stated 0 cannot be the asset");
static_assert(BrandSplash::declaredLengthCouldBeAsset(-1), "a chunked response states no length");

// A file already on flash, judged from its size. This is what decides whether a
// device that has taken a bad body keeps it or unlinks it.
static_assert(BrandSplash::storedSizeIsWholeAsset(57600), "a 57,600-byte file is drawable");
static_assert(!BrandSplash::storedSizeIsWholeAsset(0), "an empty file is not drawable");
static_assert(!BrandSplash::storedSizeIsWholeAsset(512), "a 512-byte file is not drawable");
static_assert(!BrandSplash::storedSizeIsWholeAsset(115200), "a doubled file is not drawable");

// The two names do not collide, and neither is empty.
static_assert(BrandSplash::kPath[0] == '/', "the asset path is absolute");
static_assert(BrandSplash::kTempPath[0] == '/', "the temporary path is absolute");

int main() {
  std::printf("==> Brand splash length\n");

  check(BrandSplash::kBytes == 57600, "kBytes is 57,600");
  check(BrandSplash::kWidth * BrandSplash::kHeight * 2 == 57600,
        "kBytes is the product of the drawn dimensions");

  check(BrandSplash::bodyIsWholeAsset(57600), "a 57,600-byte body is accepted");
  check(!BrandSplash::bodyIsWholeAsset(1), "a one-byte body is refused");
  check(!BrandSplash::bodyIsWholeAsset(512), "a 512-byte error page is refused");
  check(!BrandSplash::bodyIsWholeAsset(57599), "a body one byte short is refused");
  check(!BrandSplash::bodyIsWholeAsset(57601), "a body one byte over is refused");
  check(!BrandSplash::bodyIsWholeAsset(0), "an empty body is refused");
  check(!BrandSplash::bodyIsWholeAsset(-1), "a negative count is refused");

  check(BrandSplash::declaredLengthCouldBeAsset(57600), "a stated 57,600 proceeds");
  check(BrandSplash::declaredLengthCouldBeAsset(-1), "a chunked response proceeds");
  check(!BrandSplash::declaredLengthCouldBeAsset(512), "a stated 512 stops before the write");
  check(!BrandSplash::declaredLengthCouldBeAsset(0), "a stated 0 stops before the write");

  check(BrandSplash::storedSizeIsWholeAsset(57600), "a 57,600-byte stored file is kept");
  check(!BrandSplash::storedSizeIsWholeAsset(512), "a 512-byte stored file is not kept");
  check(!BrandSplash::storedSizeIsWholeAsset(0), "an empty stored file is not kept");

  if (failures == 0) {
    std::printf("Passed! %d checks, 0 failed\n", checks);
    return 0;
  }
  std::printf("Failed! %d checks, %d failed\n", checks, failures);
  return 1;
}
