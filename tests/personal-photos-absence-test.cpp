// App/PersonalPhotosPresence.h: whether a check-in carried an instruction about
// the household's photographs, and which one.
//
// The case this exists for is OUTAGE_CONTENT_LOSS_REVIEW.md section 4 in the
// server tree, 2026-11-07. CheckInGatewayService.ResolvePersonalPhotosAsync
// catches whatever its two database round trips throw, logs "the household's
// photographs could not be resolved, so this check-in carries none", and returns
// null - which PersonalPhotoAssetIds leaves out of the JSON altogether, because
// it carries WhenWritingNull. Until this header existed, parsePersonalPhotos()
// read that absence and an empty array as the same instruction and cleared the
// card for both, so one failed query on an otherwise healthy check-in took a
// family's photographs off the glass.
//
// The server states the contract the other way round in its own remarks: null is
// the server having nothing to say, "An empty list is the household's own answer
// and keeps its key." Sports has read it that way since it was written - "A
// MISSING `sports` KEY CLEARS NOTHING" in CheckIn.cpp - and this is the same rule
// for the same reason.
//
// The header is compiled as it ships and names only <stdint.h>, so there are no
// stubs. Every case is asserted twice, as a static_assert over the constexpr
// function and again at runtime, so a rule that collapses the two answers again
// stops the compile on any toolchain and also prints a Failed! line under a
// runner.
//
// WHAT THIS CANNOT REACH, because nothing under tests/ compiles CheckIn.cpp or
// includes ArduinoJson: that parsePersonalPhotos() asks this header at all, and
// that `is<JsonArrayConst>()` is what it asks with. Those are call-site
// properties and TEST_PLAN.md section 1h says what reads them.

#include <cassert>
#include <cstdio>

#include "PersonalPhotosPresence.h"

namespace {

using PersonalPhotosPresence::Decision;
using PersonalPhotosPresence::decide;

constexpr Decision absent(uint8_t ids) { return decide(false, ids); }
constexpr Decision anArray(uint8_t ids) { return decide(true, ids); }

const char* name(Decision d) {
  switch (d) {
    case Decision::LeaveHeldSetAlone:
      return "LeaveHeldSetAlone";
    case Decision::ClearHeldSet:
      return "ClearHeldSet";
    case Decision::ReplaceHeldSet:
      return "ReplaceHeldSet";
  }
  return "unknown";
}

// --- the rule ------------------------------------------------------------
//
// The two lines that are the whole defect. A member that is not an array is the
// server saying nothing, and nothing is what the card does about it.

static_assert(absent(0) == Decision::LeaveHeldSetAlone,
              "No key, so the server said nothing about photographs. This is the failed "
              "query, and it must cost a household nothing at all.");
static_assert(anArray(0) == Decision::ClearHeldSet,
              "An empty array is the household's own answer - they have chosen nothing, or "
              "nothing they chose is drawable - and it clears the card.");

// An array with pictures in it replaces the held set, which is what every
// ordinary check-in does and what must not change.
static_assert(anArray(1) == Decision::ReplaceHeldSet, "One photograph is a set of one.");
static_assert(anArray(10) == Decision::ReplaceHeldSet,
              "Ten is PersonalPhotos::kMaxPhotos, the longest set the parser hands over.");

// THE COUNT IS NOT CONSULTED WHEN THE MEMBER IS NOT AN ARRAY. A count can only
// arrive from entries the parser walked, so there is no way to reach this from
// production - and that is the point of asserting it: the answer must come off
// the member's presence alone, so a later edit that reads the count first fails
// here rather than on a device.
static_assert(absent(1) == Decision::LeaveHeldSetAlone,
              "Absent is absent whatever else the caller is holding.");
static_assert(absent(10) == Decision::LeaveHeldSetAlone, "Still absent.");

// An array whose every entry was skipped - null, not a string, or the empty
// string - parses to no usable id and still clears, because the server sent a
// key. This is the case the fix is most likely to be undone by: it looks exactly
// like absence from inside the parser, and it is not absence.
static_assert(anArray(0) != Decision::LeaveHeldSetAlone,
              "A key that yielded no usable id is still a key. Reading it as absence puts "
              "the defect back.");

struct Case {
  bool memberIsArray;
  uint8_t usableIdCount;
  Decision expected;
  const char* what;
};

constexpr Case kCases[] = {
    {false, 0, Decision::LeaveHeldSetAlone, "no key, nothing parsed"},
    {false, 1, Decision::LeaveHeldSetAlone, "no key, a count that cannot happen"},
    {false, 10, Decision::LeaveHeldSetAlone, "no key, a full count that cannot happen"},
    {true, 0, Decision::ClearHeldSet, "an empty array"},
    {true, 1, Decision::ReplaceHeldSet, "one photograph"},
    {true, 4, Decision::ReplaceHeldSet, "four photographs"},
    {true, 10, Decision::ReplaceHeldSet, "ten photographs, kMaxPhotos"},
};

}  // namespace

int main() {
  for (const auto& c : kCases) {
    const Decision got = decide(c.memberIsArray, c.usableIdCount);

    if (got != c.expected) {
      std::printf("Failed: %s - expected %s, got %s\n", c.what, name(c.expected), name(got));
      return 1;
    }
  }

  std::printf("PASS: personal photographs presence, %zu cases\n",
              sizeof(kCases) / sizeof(kCases[0]));
  return 0;
}
