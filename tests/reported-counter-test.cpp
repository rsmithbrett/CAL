// App/ReportedCounter.h: counting occurrences into an 8-bit field that travels
// to the server, where `++` on a uint8_t rolls over and reports a smaller
// number than happened.
//
// The header is compiled as it ships and names only <stdint.h>, so there are no
// stubs. A device can only ever sample its own figure, and the range that
// matters is the whole of a uint8_t plus the step past its end, which is what
// this walks.
//
// App/Assets.cpp holds the two counters this rule exists for. Nothing here
// reads that file: whether its increments run through countOne() is
// ci/check-asset-counters.sh's business, and TEST_PLAN.md section 21 says so.
//
// Every case is asserted twice, as a static_assert over a constexpr walk and
// again at runtime, so a widened predicate stops the compile on any toolchain
// and also prints a Failed! line under a runner.

#include <cassert>
#include <cstdio>

#include "ReportedCounter.h"

namespace {

// kMaxReportedDecodeFailures in Assets.cpp. Repeated rather than included
// because Assets.h pulls in Arduino; this is the id-list cap, and it is here
// only to show what the count says that the list cannot.
constexpr uint8_t kIdListCap = 4;

/// The value countOne() leaves behind after `times` occurrences, from zero.
/// constexpr so the walk below can be a static_assert as well as an assertion.
constexpr uint8_t after(unsigned times) {
  uint8_t count = 0;
  for (unsigned i = 0; i < times; ++i) {
    ReportedCounter::countOne(count);
  }
  return count;
}

/// What a bare `++` would have left after the same walk - the shipped shape,
/// written out so the two answers can be compared rather than described.
constexpr uint8_t afterWrapping(unsigned times) {
  uint8_t count = 0;
  for (unsigned i = 0; i < times; ++i) {
    ++count;
  }
  return count;
}

// The counting range, where the two shapes agree.
static_assert(after(0) == 0);
static_assert(after(1) == 1);
static_assert(after(kIdListCap) == kIdListCap);
static_assert(after(5) == 5);
static_assert(after(254) == 254);
static_assert(after(255) == 255);

// The ceiling, where they stop agreeing. 256 is the occurrence that matters
// most: CheckIn.cpp reads a count of 0 as "nothing to report", so a wrapped
// figure withholds the id list and skips the clear a successful check-in
// performs.
static_assert(after(256) == 255);
static_assert(afterWrapping(256) == 0);
static_assert(after(256) != afterWrapping(256));

// And it keeps holding, rather than holding once and then moving.
static_assert(after(257) == 255);
static_assert(after(511) == 255);
static_assert(after(512) == 255);
static_assert(after(1000) == 255);

// The wrapped figure is a plausible small number at every step past the end,
// which is why there is nothing in a report to say it is not the real one.
static_assert(afterWrapping(257) == 1);
static_assert(afterWrapping(260) == kIdListCap);
static_assert(afterWrapping(511) == 255);

// The ceiling is the field's own limit, not a figure chosen beside it: a lower
// one would withhold counts the 8-bit field has room for.
static_assert(ReportedCounter::kCeiling == UINT8_MAX);
static_assert(ReportedCounter::kCeiling == 255);

// atCeiling() is what lets a caller say "or more" about the figure it reports.
static_assert(!ReportedCounter::atCeiling(0));
static_assert(!ReportedCounter::atCeiling(254));
static_assert(ReportedCounter::atCeiling(255));

/// Counting from a value already at the ceiling, which is the state a long
/// reporting cycle leaves behind and the state the next occurrence arrives in.
constexpr uint8_t fromCeiling(unsigned times) {
  uint8_t count = ReportedCounter::kCeiling;
  for (unsigned i = 0; i < times; ++i) {
    ReportedCounter::countOne(count);
  }
  return count;
}

static_assert(fromCeiling(1) == 255);
static_assert(fromCeiling(50) == 255);

}  // namespace

int main() {
  // The same walk at runtime. A constexpr-only check passes a toolchain that
  // folded it and says nothing about the code that ships in the binary.
  assert(after(0) == 0);
  assert(after(1) == 1);
  assert(after(kIdListCap) == kIdListCap);
  assert(after(254) == 254);
  assert(after(255) == 255);

  // The step past the end, and the figure CheckIn.cpp would read as "nothing to
  // report".
  assert(after(256) == 255);
  assert(after(256) != 0);
  assert(after(256) != afterWrapping(256));

  assert(after(257) == 255);
  assert(after(512) == 255);
  assert(after(1000) == 255);

  // Every occurrence inside the range is still counted. A ceiling that bit
  // early would be the id-list cap again by another name, and the count would
  // stop separating four bad assets from one card failing all day.
  for (unsigned n = 0; n <= ReportedCounter::kCeiling; ++n) {
    assert(after(n) == static_cast<uint8_t>(n));
  }

  // And nothing past it moves.
  for (unsigned n = ReportedCounter::kCeiling; n <= ReportedCounter::kCeiling + 64; ++n) {
    assert(after(n) == ReportedCounter::kCeiling);
  }

  assert(fromCeiling(1) == ReportedCounter::kCeiling);
  assert(fromCeiling(50) == ReportedCounter::kCeiling);

  assert(!ReportedCounter::atCeiling(0));
  assert(!ReportedCounter::atCeiling(ReportedCounter::kCeiling - 1));
  assert(ReportedCounter::atCeiling(ReportedCounter::kCeiling));

  printf("PASS: reported counter - every occurrence to 255 counted, and the 64 past it hold\n");
  return 0;
}
