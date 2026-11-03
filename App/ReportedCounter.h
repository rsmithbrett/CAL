#pragma once

#include <stdint.h>

/// Counting occurrences into an 8-bit field that is reported rather than
/// compared against a cap.
///
/// A counter the firmware only ever tests against a small ceiling cannot
/// overflow: the test stops the increment long before the type does. A counter
/// whose value travels to the server is the other kind. Nothing bounds how many
/// times a card redraws a picture that will not decode, so the figure has to
/// stay readable past 255 occurrences, and `++` on a `uint8_t` does not: it
/// rolls to 0 and reports a smaller number than happened.
///
/// Holding at the ceiling is the answer rather than widening the field, because
/// the field is on the check-in request and the 6-month compatibility mandate
/// closed on 2026-09-22. 255 then means "255 or more", which is a floor on what
/// happened. A reader who needs the difference between 255 and 4,000 is asking
/// a question this field was never shaped to answer, and the log lines beside
/// each counter carry the per-cycle detail.
///
/// The wrapped reading is worse than imprecise in two specific places. At
/// exactly 256, 512 and so on the figure reads 0, and CheckIn.cpp reads a count
/// of 0 as "nothing to report": the id list is withheld along with it, and the
/// clear that a successful check-in performs is skipped, so the ids pin and the
/// next cycle opens on a count that is already wrong. Below that, every wrapped
/// value is a plausible small number, so there is nothing in the report to say
/// it is not the real one.
///
/// **This header names nothing from Arduino, ESP-IDF or the graphics library**,
/// which is what lets tests/reported-counter-test.cpp walk the whole range of a
/// `uint8_t` on the host. App/Assets.cpp holds the two counters, and that test
/// reads none of it: ci/check-asset-counters.sh is what checks the call sites.
namespace ReportedCounter {

/// The value a reported counter holds at. Equal to the maximum a `uint8_t`
/// carries, so the ceiling is the field's own limit rather than a figure chosen
/// beside it.
constexpr uint8_t kCeiling = UINT8_MAX;

/// Counts one occurrence into `count`, leaving it at kCeiling once it gets
/// there.
///
/// constexpr so tests/reported-counter-test.cpp can assert the walk past the
/// end of the range as a static_assert as well as at runtime: a widened
/// predicate then stops the compile on any toolchain, rather than only printing
/// a Failed! line under a runner.
constexpr void countOne(uint8_t& count) {
  if (count < kCeiling) {
    ++count;
  }
}

/// Whether `count` is at the ceiling, so a reader can say "or more" about the
/// figure it is about to report.
constexpr bool atCeiling(uint8_t count) { return count >= kCeiling; }

}  // namespace ReportedCounter
