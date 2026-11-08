#pragma once

// Whether a check-in carried an instruction about the household's photographs,
// and which one.
//
// Pulled out of parsePersonalPhotos() in CheckIn.cpp so the decision can be
// walked on a host with no Arduino and no ArduinoJson, for the reason
// App/ReportedCounter.h and App/UnreachableHoldOff.h give: a header a test can
// include as it ships needs no stubs, and nothing under tests/ compiles
// CheckIn.cpp.
//
// The parser still owns everything around this - reading the member, skipping
// entries it cannot use, bounding the copies at kMaxPhotos, and calling
// PersonalPhotos::setFromCheckIn. This answers one question: given what the
// member turned out to be, does the held set get replaced, cleared, or left
// where it is?

#include <stdint.h>

namespace PersonalPhotosPresence {

/// What the parser should do with the set the card is already holding.
enum class Decision {
  /// Nothing. The server said nothing about photographs, so whatever the card
  /// drew last check-in is still the best account of the household's choice.
  LeaveHeldSetAlone,

  /// Hand the card an empty set, which takes the photographs off the rotation
  /// and deletes their cached files.
  ClearHeldSet,

  /// Hand the card the ids that were parsed.
  ReplaceHeldSet,
};

/// \param memberIsArray `personalPhotoAssetIds` arrived as a JSON array.
///        False for a member that is not there, for one written as null, and
///        for one holding something that is not an array.
/// \param usableIdCount How many ids the parser read out of that array - its
///        entries less the ones that were null, not a string, or empty, and
///        capped at PersonalPhotos::kMaxPhotos.
constexpr Decision decide(bool memberIsArray, uint8_t usableIdCount) {
  // AN ABSENT MEMBER CLEARS NOTHING, which is the same rule CheckIn.cpp states
  // at the sports branch - "A MISSING `sports` KEY CLEARS NOTHING" - and states
  // there because conflating the two blanks a card every time the server has
  // nothing to say.
  //
  // What the server means by absence is written down on its side:
  // CheckInResponse.PersonalPhotoAssetIds carries
  // JsonIgnoreCondition.WhenWritingNull so a null is left out of the JSON
  // rather than written, and its remarks say null is the server having nothing
  // to say while "empty is the household's own answer and keeps its key".
  // CheckInGatewayService.ResolvePersonalPhotosAsync returns that null in three
  // cases and none of them is a household choosing nothing: the photograph
  // source is not wired into the deployment, the device's resolved card policy
  // does not name the card, or the two database round trips threw and were
  // caught so the check-in would survive them.
  //
  // That third case is the defect this header was written for. One failed query
  // on an otherwise healthy check-in used to take a family's photographs off the
  // glass - OUTAGE_CONTENT_LOSS_REVIEW.md section 4 in the server tree.
  //
  // A member that is not an array at all is read the same way, the tolerance
  // parseCardPolicy applies to a member it cannot read. The server has never
  // sent one and a device cannot act on what it cannot read, so the choice is
  // between leaving the card alone and clearing it on a shape nobody intended.
  if (!memberIsArray) {
    return Decision::LeaveHeldSetAlone;
  }

  // AN EMPTY ARRAY IS AN ANSWER AND IT CLEARS. The household has chosen nothing,
  // or every picture they chose is a format this fleet cannot draw, and both
  // mean there is nothing to put on the glass. Keeping a set here would draw
  // pictures a family has already unticked or deleted.
  //
  // An array whose every entry was skipped reaches this the same way, and that
  // is deliberate: it looks like absence from inside the parser and it is not
  // absence. The server sent a key.
  if (usableIdCount == 0) {
    return Decision::ClearHeldSet;
  }

  return Decision::ReplaceHeldSet;
}

}  // namespace PersonalPhotosPresence
