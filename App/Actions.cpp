#include "Actions.h"

#include <Preferences.h>
#include <time.h>

#include "Identity.h"
#include "Log.h"

namespace Actions {
namespace {

// A namespace of its own, deliberately not the "cal" namespace Identity uses.
// Identity.h/.cpp are kept byte-identical between CAL and App because both
// binaries read and write that schema; adding keys to it from one side only
// is exactly the drift that file warns about. Nothing in CAL knows or cares
// about pending actions, so they get their own namespace instead.
constexpr const char* kNvsNamespace = "appacts";
constexpr const char* kKeyCount = "n";
constexpr const char* kKeyCounter = "c";

Definition gDefinitions[kMaxDefinitions];
uint8_t gDefinitionCount = 0;

Pending gPending[kMaxPending];
uint8_t gPendingCount = 0;

/// "a0".."a7" - the slot keys. Written as a packed single string rather than
/// three keys per entry: NVS entries are a scarce, fixed-count resource on
/// this partition (0x5000 total, shared with CAL's own identity data), and
/// one key per queued press is cheaper than three.
String slotKey(uint8_t index) { return String("a") + String(index); }

constexpr char kFieldSeparator = '\n';

/// Whether a field read back out of NVS is a Compass pid rather than the start
/// of a summary. Digits only and bounded, which is the same rule the server
/// applies on the way in - see NormaliseCompassPid.
bool looksLikeCompassPid(const String& value) {
  if (value.length() == 0 || value.length() > 30) {
    return false;
  }
  for (unsigned int i = 0; i < value.length(); ++i) {
    if (value[i] < '0' || value[i] > '9') {
      return false;
    }
  }
  return true;
}

String packEntry(const Pending& entry) {
  return entry.actionId + kFieldSeparator + entry.instanceId + kFieldSeparator +
         entry.pressedAtUtc + kFieldSeparator + entry.compassPid + kFieldSeparator +
         entry.onScreenSummary;
}

/// Reads the three-, four- and five-field layouts.
///
/// Each later field arrived while devices were already in the field with presses
/// queued in NVS across a reboot. An older entry is not corrupt, and it has to
/// keep its press rather than have it dropped by the very update meant to
/// improve things. Absent reads as empty, which is exactly what the server
/// stores for a card that had nothing to name, so the two arrive at the same
/// place.
///
/// The separator is '\n', and the summary is the last field for a reason: it is
/// the only one composed from provider text rather than from ids this firmware
/// controls. Being last means even a summary that somehow contained a newline
/// can only corrupt itself, never the instanceId that dedup depends on. Cards
/// are expected to return a single line; see Actions.h.
///
/// THE FOURTH FIELD IS AMBIGUOUS AND IS RESOLVED BY SHAPE. In the four-field
/// layout it is the summary; in the five-field layout it is the Compass pid. A
/// stored entry does not say which it is, and the count of separators cannot
/// settle it either, because a four-field summary that contained a newline
/// presents as five. So the fourth field is taken as a pid only when it looks
/// like one - digits, bounded - and otherwise the whole remainder is the
/// summary, which is what an older entry meant. A summary is an address and a
/// price; it does not read as digits alone.
bool unpackEntry(const String& packed, Pending& out) {
  const int first = packed.indexOf(kFieldSeparator);
  if (first < 0) {
    return false;
  }
  const int second = packed.indexOf(kFieldSeparator, first + 1);
  if (second < 0) {
    return false;
  }
  out.actionId = packed.substring(0, first);
  out.instanceId = packed.substring(first + 1, second);

  const int third = packed.indexOf(kFieldSeparator, second + 1);
  if (third < 0) {
    out.pressedAtUtc = packed.substring(second + 1);
    out.onScreenSummary = "";
    out.compassPid = "";
    return true;
  }

  out.pressedAtUtc = packed.substring(second + 1, third);

  const String remainder = packed.substring(third + 1);
  const int fourth = remainder.indexOf(kFieldSeparator);

  if (fourth >= 0) {
    const String candidate = remainder.substring(0, fourth);
    if (looksLikeCompassPid(candidate)) {
      out.compassPid = candidate;
      out.onScreenSummary = remainder.substring(fourth + 1);
      return true;
    }
  }

  // Four fields, or five whose fourth is not a pid: the remainder is all
  // summary, newline and all, exactly as the older layout meant it.
  out.compassPid = "";
  out.onScreenSummary = remainder;
  return true;
}

/// Same time_t -> ISO 8601 idiom as CheckIn.cpp's nowAsIso8601Utc(). Not
/// shared with it: this module has to stamp a press at the moment it happens
/// (which may be minutes before the check-in that carries it), so the two
/// call sites genuinely want their own "now".
String nowAsIso8601Utc() {
  const time_t now = time(nullptr);
  struct tm utc;
  gmtime_r(&now, &utc);
  char buffer[21];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return String(buffer);
}

/// Rewrites the whole queue to NVS. Called after any mutation - the queue is
/// at most eight short strings, so rewriting it wholesale is simpler and
/// harder to get wrong than patching individual slots, and a press happens
/// far too rarely for the extra writes to matter for flash wear.
void persistQueue() {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, /*readOnly=*/false)) {
    Log::line("[actions] could not open NVS to persist the pending queue");
    return;
  }
  for (uint8_t i = 0; i < gPendingCount; ++i) {
    prefs.putString(slotKey(i).c_str(), packEntry(gPending[i]));
  }
  // Slots past the new count are removed rather than left behind - a stale
  // slot would otherwise be re-read on the next boot if the count ever grew
  // back over it.
  for (uint8_t i = gPendingCount; i < kMaxPending; ++i) {
    prefs.remove(slotKey(i).c_str());
  }
  prefs.putUChar(kKeyCount, gPendingCount);
  prefs.end();
}

}  // namespace

void begin() {
  gPendingCount = 0;

  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, /*readOnly=*/true)) {
    // Ordinary first-boot state: the namespace does not exist until the first
    // press is ever recorded. Not a fault.
    Log::line("[actions] no pending-action store yet (first boot)");
    return;
  }

  const uint8_t stored = prefs.getUChar(kKeyCount, 0);
  for (uint8_t i = 0; i < stored && i < kMaxPending; ++i) {
    const String packed = prefs.getString(slotKey(i).c_str(), "");
    if (packed.length() == 0) {
      continue;
    }
    Pending entry;
    if (unpackEntry(packed, entry)) {
      gPending[gPendingCount++] = entry;
    }
  }
  prefs.end();

  if (gPendingCount > 0) {
    Log::printf("[actions] %u press(es) survived a reboot and are still pending", gPendingCount);
  }
}

void applyDefinitions(const Definition* definitions, uint8_t count) {
  gDefinitionCount = 0;
  for (uint8_t i = 0; i < count && i < kMaxDefinitions; ++i) {
    gDefinitions[gDefinitionCount++] = definitions[i];
  }
  // Slots past the new count keep stale Strings otherwise, and forCard()
  // below only reads up to gDefinitionCount - clearing them is purely to
  // release the heap those Strings hold.
  for (uint8_t i = gDefinitionCount; i < kMaxDefinitions; ++i) {
    gDefinitions[i] = Definition();
  }
}

uint8_t forCard(const char* cardId, Definition* out, uint8_t maxOut) {
  if (cardId == nullptr || out == nullptr) {
    return 0;
  }
  uint8_t written = 0;
  for (uint8_t i = 0; i < gDefinitionCount && written < maxOut; ++i) {
    if (gDefinitions[i].cardId == cardId) {
      out[written++] = gDefinitions[i];
    }
  }
  return written;
}

bool recordPress(const Definition& definition, const String& onScreenSummary,
                 const String& compassPid) {
  if (gPendingCount >= kMaxPending) {
    Log::printf("[actions] queue full (%u) - dropping press of '%s'", gPendingCount,
                definition.actionId.c_str());
    return false;
  }

  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, /*readOnly=*/false)) {
    Log::line("[actions] could not open NVS to record a press");
    return false;
  }
  // Monotonic and persisted: incremented and written before the press itself
  // is stored, so a power loss mid-write costs an unused id rather than two
  // presses sharing one (which would make the server dedup them into one).
  const uint32_t counter = prefs.getUInt(kKeyCounter, 0) + 1;
  prefs.putUInt(kKeyCounter, counter);
  prefs.end();

  Pending entry;
  entry.actionId = definition.actionId;
  entry.instanceId = Identity::macAddress() + ":" + String(counter);
  entry.pressedAtUtc = nowAsIso8601Utc();

  // Capped here rather than at the call site so every path into the queue gets
  // the same limit, and so a card returning something long can never push an NVS
  // write past what the slot holds.
  // Stored only when it is what the server will accept: digits, bounded. A card
  // handing over anything else would otherwise be written to NVS, read back as
  // the start of a summary, and corrupt the entry it was meant to enrich - see
  // unpackEntry().
  entry.compassPid = looksLikeCompassPid(compassPid) ? compassPid : String();

  entry.onScreenSummary = onScreenSummary;
  if (entry.onScreenSummary.length() > kMaxOnScreenSummaryLength) {
    entry.onScreenSummary = entry.onScreenSummary.substring(0, kMaxOnScreenSummaryLength);
    Log::printf("[actions] on-screen summary truncated to %u chars",
                static_cast<unsigned>(kMaxOnScreenSummaryLength));
  }

  gPending[gPendingCount++] = entry;
  persistQueue();

  Log::printf("[actions] recorded press actionId=%s instanceId=%s at=%s onScreen=%s",
              entry.actionId.c_str(), entry.instanceId.c_str(), entry.pressedAtUtc.c_str(),
              entry.onScreenSummary.length() > 0
                  ? entry.onScreenSummary.c_str()
                  : "(nothing - this card does not describe its items)");
  return true;
}

uint8_t pendingCount() { return gPendingCount; }

uint8_t pendingSnapshot(Pending* out, uint8_t maxOut) {
  if (out == nullptr) {
    return 0;
  }
  uint8_t written = 0;
  for (uint8_t i = 0; i < gPendingCount && written < maxOut; ++i) {
    out[written++] = gPending[i];
  }
  return written;
}

void clearAccepted(const String* acceptedIds, uint8_t count) {
  if (acceptedIds == nullptr || count == 0 || gPendingCount == 0) {
    return;
  }

  uint8_t kept = 0;
  uint8_t dropped = 0;
  for (uint8_t i = 0; i < gPendingCount; ++i) {
    bool accepted = false;
    for (uint8_t j = 0; j < count; ++j) {
      if (gPending[i].instanceId == acceptedIds[j]) {
        accepted = true;
        break;
      }
    }
    if (accepted) {
      dropped++;
      continue;
    }
    gPending[kept++] = gPending[i];
  }
  for (uint8_t i = kept; i < gPendingCount; ++i) {
    gPending[i] = Pending();
  }
  gPendingCount = kept;

  if (dropped > 0) {
    persistQueue();
    Log::printf("[actions] server accepted %u press(es); %u still pending", dropped, gPendingCount);
  }
}

}  // namespace Actions
