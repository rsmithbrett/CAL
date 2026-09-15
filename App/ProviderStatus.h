#pragma once

#include <Arduino.h>

/// The server's device-facing provider `status` vocabulary - `ProviderStatus`
/// in DiscoverAroundMe.SharedKernel.Content, serialized by name in PascalCase
/// (`"Ok"`, `"NotConfigured"`, `"Stale"`, `"Unavailable"`) rather than in the
/// camelCase the property names around it use, to match the other enum already
/// on the wire in a sibling payload.
///
/// WHY IT EXISTS. `lastRefreshError` is prose written for an operator, and the
/// cards were reading its mere PRESENCE as a signal - which made a sentence
/// into a protocol element, and put the two on a collision course the moment
/// the server started stripping it from device payloads (it is marked
/// `[OperatorDiagnostic]` and removed at `DeviceJsonResult`, because one of
/// those sentences reached a household's kitchen wall). The server derives this
/// value on the record from state it already has - is a credential configured,
/// is there a live error, is there anything cached - rather than authoring it
/// at each failure site, so it cannot drift out of agreement with the
/// diagnostic it replaces.
///
/// WHY THIS IS SHARED RATHER THAN COPIED PER CARD, which is the opposite of
/// what `describeFreshness()` does in three of these same files. That helper is
/// duplicated because each copy reads a different card's unrelated `Result`,
/// and sharing it would mean a new file to hold one function. This one is
/// different on both counts: it touches no card's `Result` at all - it parses
/// a wire vocabulary into a wire vocabulary - and three cards need the SAME
/// answer from it. Copies of a parser are how one card learns about a fifth
/// status value and another silently keeps treating it as `Unrecognized`, and
/// this is a closed vocabulary specifically so that cannot happen. Same
/// reasoning as `Maintenance::failureText()`: one helper, not a conditional in
/// every card.
///
/// WHAT IS DELIBERATELY NOT HERE: what to do when the value is `Absent`. That
/// is a per-card policy, not a property of the vocabulary, and the two cards
/// that read this today answer it differently on purpose - `Listings.cpp` falls
/// back to the old presence-inference because it has correct behaviour to
/// preserve, `Aircraft.cpp` deliberately does not. Each says why at its own
/// call site.
namespace ProviderStatus {

/// Four values from the server, and two that are this firmware's own. The two
/// are deliberately distinct from each other:
///
///   - `Absent` - a 200 from a server that does not send the field yet. Every
///     server this fleet talks to, until the strip ships. This is the one
///     value that could license reading the old signal.
///   - `Unrecognized` - a value a later server added that this build has never
///     heard of. NOT the same situation as `Absent`: a newer server said
///     something specific, and `lastRefreshError` will already be gone from
///     its payload, so there is nothing older to fall back to. Every caller
///     treats it as a failed refresh, because that is the reading which never
///     invents a claim about the world on the far side of the failure.
enum class Value : uint8_t {
  Absent,
  Ok,
  NotConfigured,
  Stale,
  Unavailable,
  Unrecognized,
};

/// An absent or empty field is `Absent`; anything outside the closed
/// vocabulary is `Unrecognized`. Never returns a value the caller has to
/// bounds-check.
inline Value parse(const char* text) {
  if (text == nullptr || strlen(text) == 0) {
    return Value::Absent;
  }
  if (strcmp(text, "Ok") == 0) {
    return Value::Ok;
  }
  if (strcmp(text, "NotConfigured") == 0) {
    return Value::NotConfigured;
  }
  if (strcmp(text, "Stale") == 0) {
    return Value::Stale;
  }
  if (strcmp(text, "Unavailable") == 0) {
    return Value::Unavailable;
  }
  return Value::Unrecognized;
}

/// For the debug stream only - never drawn, and never compared against. The
/// names double as the reason each value means what it does, because the reader
/// of this line is somebody trying to work out why a card said what it said.
/// Worded without naming any one card's subject, since three of them share it.
inline const char* describe(Value value) {
  switch (value) {
    case Value::Absent:
      return "no status field on this payload - a server that predates the field";
    case Value::Ok:
      return "the server's refresh succeeded; its data is current";
    case Value::NotConfigured:
      return "no provider credential on file, so nothing was ever attempted";
    case Value::Stale:
      return "refresh failed, last-known-good data still available";
    case Value::Unavailable:
      return "refresh failed with nothing cached to fall back on";
    case Value::Unrecognized:
      return "a status value this build does not know";
  }
  return "unknown";
}

}  // namespace ProviderStatus
