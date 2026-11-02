// App/DecodeFailureReport.h: what a display tells the server about pictures it
// could not draw, and the counting defect that shipped.
//
// The header names String and nothing else, so it compiles against a stub here
// and against Arduino's String on the device. Assets.cpp holds it as a file-scope
// instance and is otherwise unchanged in how it is read.
//
// What each case is here for is TEST_PLAN.md section 21. The design is this
// repository's README, "The report says why now, not just which".

#include <cassert>
#include <iostream>
#include <string>

#include "DecodeFailureReport.h"

namespace {

using DecodeFailure::Reason;
using DecodeFailure::Report;

int gFailures = 0;

void check(bool condition, const std::string& what) {
  if (!condition) {
    std::cout << "  FAILED: " << what << "\n";
    ++gFailures;
  }
}

/// wireName() answers nullptr for an unclassified reason, which is the contract
/// CheckIn.cpp omits the member on. Read through this rather than constructed
/// straight into a std::string, so a build that answers nullptr where a name was
/// expected fails a check instead of throwing out of the middle of the run.
std::string name(Reason reason) {
  const char* wire = DecodeFailure::wireName(reason);
  return wire == nullptr ? "(absent)" : std::string(wire);
}

const std::string kA = "11111111-1111-1111-1111-111111111111";
const std::string kB = "22222222-2222-2222-2222-222222222222";
const std::string kC = "33333333-3333-3333-3333-333333333333";
const std::string kD = "44444444-4444-4444-4444-444444444444";
const std::string kE = "55555555-5555-5555-5555-555555555555";

/// A fresh report says nothing, and says it as absence rather than as zero -
/// which is the contract the wire fields rest on. wireName() answering nullptr
/// is how CheckIn.cpp knows to omit the member entirely.
void nothingReported() {
  Report report;
  check(report.count() == 0, "a fresh report counts nothing");
  check(report.ids().empty(), "a fresh report names nothing");
  check(!report.hasAny(), "a fresh report has nothing");
  check(report.reason() == Reason::Unclassified, "a fresh report has no reason");
  check(DecodeFailure::wireName(report.reason()) == nullptr,
        "an unclassified reason has no wire name");
  check(report.bytes() == 0, "a fresh report has no byte size");
  check(report.largestBlock() == 0, "a fresh report has no heap figure");
  check(report.giveUps() == 0, "a fresh report has given up on nothing");
}

/// One failure carries all four values, and the id list is the id.
void oneFailure() {
  Report report;
  report.record(kA, Reason::BytesRejected, 9216, 40948);

  check(report.count() == 1, "one picture is one");
  check(report.ids() == kA, "the id list is that id");
  check(report.hasAny(), "one failure is something to report");
  check(report.reason() == Reason::BytesRejected, "the reason is carried");
  check(name(report.reason()) == "bytes-rejected",
        "bytes-rejected is its wire name");
  check(report.bytes() == 9216, "the byte size is carried");
  check(report.largestBlock() == 40948, "the heap figure is carried");
  check(report.giveUps() == 1, "one give-up");
}

/// THE DEFECT, first half. The same picture refailing must not count twice: an
/// aircraft logo overlay redraws on every appearance of that card.
void theSamePictureTwice() {
  Report report;
  report.record(kA, Reason::BytesRejected, 9216, 40948);
  report.record(kA, Reason::BytesRejected, 9216, 40100);

  check(report.count() == 1, "the same picture twice is one picture");
  check(report.ids() == kA, "the id is named once");
  check(report.giveUps() == 2, "but two draws were given up on");
}

/// Four distinct pictures fill the report, comma-joined in the order they failed.
void fourDistinctPictures() {
  Report report;
  report.record(kA, Reason::BytesRejected, 1, 2);
  report.record(kB, Reason::BytesRejected, 3, 4);
  report.record(kC, Reason::BytesRejected, 5, 6);
  report.record(kD, Reason::BytesRejected, 7, 8);

  check(report.count() == 4, "four pictures is four");
  check(report.ids() == kA + "," + kB + "," + kC + "," + kD, "all four, in order, comma-joined");
  check(report.giveUps() == 4, "four give-ups");
}

/// THE DEFECT, second half, and the reason it was never a count of pictures.
///
/// The dedup used to ask whether the id was a substring of the joined id list,
/// and ids stopped being appended at the cap. So the fifth distinct id was never
/// in the list, therefore never matched, and incremented the counter on every
/// single failed draw of it. A display rotating five bad pictures reported 5, then
/// 6, then 7, with four ids beside the number.
void theFifthPictureDoesNotInflateTheCount() {
  Report report;
  report.record(kA, Reason::BytesRejected, 1, 2);
  report.record(kB, Reason::BytesRejected, 3, 4);
  report.record(kC, Reason::BytesRejected, 5, 6);
  report.record(kD, Reason::BytesRejected, 7, 8);

  const String fourIds = report.ids();
  report.record(kE, Reason::BytesRejected, 9, 10);

  check(report.count() == 4, "a fifth picture the report has no room for does not raise the count");
  check(report.ids() == fourIds, "and does not reach the id list");
  check(report.giveUps() == 5, "the give-up tally does see it");
}

/// THE WRAP. The old increment was a bare uint8_t++ with no ceiling, so 255 failed
/// draws of an unreportable picture rolled the count to 0 and the report read as
/// "nothing failed". gAllocationRefusals beside it has always had the guard.
void nothingWraps() {
  Report report;
  report.record(kA, Reason::BytesRejected, 1, 2);
  report.record(kB, Reason::BytesRejected, 3, 4);
  report.record(kC, Reason::BytesRejected, 5, 6);
  report.record(kD, Reason::BytesRejected, 7, 8);

  for (int i = 0; i < 600; ++i) {
    report.record(kE, Reason::BytesRejected, 9, 10);
  }

  check(report.count() == 4, "600 draws of a fifth picture leave the count at four");
  check(report.giveUps() == 255, "the give-up tally saturates rather than wrapping to zero");
  check(report.hasAny(), "and the report still says something failed");
}

/// The three values describe the FIRST failure of the cycle, which is the one the
/// first id names. A later failure's heap figure is a measurement of a heap the
/// earlier failures already changed.
void theThreeValuesAreTheFirstFailures() {
  Report report;
  report.record(kA, Reason::DecoderAbsent, 44408, 3444);
  report.record(kB, Reason::AllocationRefused, 153600, 2048);

  check(report.reason() == Reason::DecoderAbsent, "the first failure's reason stands");
  check(report.bytes() == 44408, "the first failure's byte size stands");
  check(report.largestBlock() == 3444, "the first failure's heap figure stands");
  check(report.count() == 2, "both pictures are still counted");
}

/// A repeat of the first picture must not restate the three values either, or a
/// refail minutes later would overwrite the measurement that mattered.
void arepeatDoesNotRestateTheValues() {
  Report report;
  report.record(kA, Reason::DecoderAbsent, 44408, 3444);
  report.record(kA, Reason::BytesRejected, 1, 1);

  check(report.reason() == Reason::DecoderAbsent, "the reason is the first one");
  check(report.bytes() == 44408, "the byte size is the first one");
  check(report.largestBlock() == 3444, "the heap figure is the first one");
}

/// A value the device could not measure is recorded as absent rather than as a
/// zero that reads like a measurement. A display with no SD card has no file to
/// size.
void whatCouldNotBeMeasuredStaysAbsent() {
  Report report;
  report.record(kA, Reason::BytesRejected, 0, 0);

  check(report.count() == 1, "the failure is still reported");
  check(report.bytes() == 0, "an unmeasured size stays zero here");
  check(report.largestBlock() == 0, "an unmeasured heap figure stays zero here");
  check(!report.reportsBytes(), "and zero bytes is not offered to the wire");
  check(!report.reportsLargestBlock(), "nor is a zero heap figure");
}

/// clear() is called once a check-in carrying the report has actually succeeded,
/// so the next cycle starts from nothing - including the three values.
void clearingResetsEverything() {
  Report report;
  report.record(kA, Reason::DecoderAbsent, 44408, 3444);
  report.record(kB, Reason::BytesRejected, 1, 2);
  report.clear();

  check(report.count() == 0, "cleared counts nothing");
  check(report.ids().empty(), "cleared names nothing");
  check(report.reason() == Reason::Unclassified, "cleared has no reason");
  check(report.bytes() == 0, "cleared has no byte size");
  check(report.largestBlock() == 0, "cleared has no heap figure");
  check(report.giveUps() == 0, "cleared has given up on nothing");

  report.record(kE, Reason::AllocationRefused, 153600, 2048);
  check(report.count() == 1, "and takes a new failure afterwards");
  check(report.ids() == kE, "naming the new picture only");
}

/// The three names are the wire contract. A typo here is a reason code the server
/// files as unrecognized, so they are pinned rather than assumed.
void theWireNames() {
  check(name(Reason::DecoderAbsent) == "decoder-absent",
        "decoder-absent");
  check(name(Reason::AllocationRefused) == "allocation-refused",
        "allocation-refused");
  check(name(Reason::BytesRejected) == "bytes-rejected",
        "bytes-rejected");
  check(DecodeFailure::wireName(Reason::Unclassified) == nullptr, "unclassified has no name");
}

/// An unclassified failure is still worth reporting - the picture failed - and
/// simply carries no reason. Nothing substitutes a default.
void anUnclassifiedFailureStillReports() {
  Report report;
  report.record(kA, Reason::Unclassified, 9216, 40948);

  check(report.count() == 1, "the failure is reported");
  check(report.ids() == kA, "and names the picture");
  check(DecodeFailure::wireName(report.reason()) == nullptr, "with no reason on the wire");
  check(report.reportsBytes(), "the byte size it did measure still goes");
  check(report.reportsLargestBlock(), "and so does the heap figure");
}

}  // namespace

int main() {
  nothingReported();
  oneFailure();
  theSamePictureTwice();
  fourDistinctPictures();
  theFifthPictureDoesNotInflateTheCount();
  nothingWraps();
  theThreeValuesAreTheFirstFailures();
  arepeatDoesNotRestateTheValues();
  whatCouldNotBeMeasuredStaysAbsent();
  clearingResetsEverything();
  theWireNames();
  anUnclassifiedFailureStillReports();

  if (gFailures > 0) {
    std::cout << "FAIL: " << gFailures << " decode failure report check(s)\n";
    return 1;
  }
  std::cout << "PASS: decode failure report, 12 cases\n";
  return 0;
}
