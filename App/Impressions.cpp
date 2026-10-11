#include "Impressions.h"

#include <Preferences.h>
#include <time.h>

#include "Identity.h"
#include "Log.h"

namespace Impressions {
namespace {

constexpr const char* kPrefsNamespace = "impr";
constexpr const char* kKeyCounter = "counter";

/// HOW MANY IDS ARE RESERVED PER FLASH WRITE.
///
/// Actions writes the counter to NVS on every press, which is right there: a
/// press is a deliberate act and happens a few times a day. A showing happens
/// every fifteen seconds, so the same pattern would be about six thousand
/// flash writes a day on a part rated for a hundred thousand - weeks, not
/// years, before the sector is gone.
///
/// So a block is reserved with one write and handed out from RAM. A power loss
/// costs up to this many unused ids, which is the same trade Actions makes at
/// a block size of one: an unused id is nothing, and two showings sharing one
/// would have the server deduplicate them into one.
constexpr uint32_t kIdBlock = 256;

uint32_t gNextId = 0;
uint32_t gIdsLeftInBlock = 0;

Entry gEntries[kMaxEntries];
uint8_t gCount = 0;
uint16_t gDropped = 0;

/// The showing on screen. Not an Entry: it has no dwell until it ends, and a
/// half-filled Entry in the buffer is the shape a reader would mistake for a
/// finished one.
bool gShowing = false;
String gCardId;
String gContentKey;
String gSummary;
String gShownAtUtc;
uint32_t gStartedMs = 0;
Arrival gArrival = Arrival::Rotation;
bool gSought = false;

/// Presence, accumulated against the showing on screen.
bool gPresent = false;
uint32_t gPresentSinceMs = 0;
uint32_t gSeenMs = 0;

/// The extra entities the showing on screen is also displaying - the second,
/// third and fourth games of a scoreboard. Fixed buffers beside the entry
/// array and for the same reason: this file does not touch the heap.
struct FrameMember {
  char contentKey[kMaxContentKey + 1];
  char summary[kMaxSummary + 1];
};
FrameMember gFrameMembers[kMaxFrameMembers];
uint8_t gFrameMemberCount = 0;

/// Copies into a fixed destination and terminates by hand. strncpy writes no
/// terminator when the source fills the buffer, and a summary arrives from a
/// card that composed it rather than from anything bounded here - so a long one
/// is cut rather than allowed to run into the next field of the row.
void copyInto(char* destination, size_t capacity, const char* source) {
  if (destination == nullptr || capacity == 0) { return; }
  strncpy(destination, source != nullptr ? source : "", capacity - 1);
  destination[capacity - 1] = '\0';
}

String reserveInstanceId() {
  if (gIdsLeftInBlock == 0) {
    Preferences prefs;
    if (prefs.begin(kPrefsNamespace, false)) {
      const uint32_t reservedThrough = prefs.getUInt(kKeyCounter, 0) + kIdBlock;
      prefs.putUInt(kKeyCounter, reservedThrough);
      prefs.end();

      gNextId = reservedThrough - kIdBlock + 1;
      gIdsLeftInBlock = kIdBlock;
    } else {
      // NVS would not open. Carry on from wherever RAM had got to rather than
      // refusing to record anything: a duplicate id across a reboot is
      // deduplicated by the server, and losing a day of showings because a
      // preferences partition is unhappy is the worse outcome.
      Log::printf("[impressions] could not reserve ids, continuing from %lu",
                  static_cast<unsigned long>(gNextId));
      gIdsLeftInBlock = kIdBlock;
    }
  }

  gIdsLeftInBlock--;
  return Identity::macAddress() + ":" + String(gNextId++);
}

/// The same time_t to ISO 8601 idiom Actions.cpp and CheckIn.cpp each keep
/// their own copy of, for the reason they give: a showing is stamped when it
/// ends, which can be minutes before the check-in that carries it.
String nowAsIso8601Utc() {
  const time_t now = time(nullptr);
  struct tm utc;
  gmtime_r(&now, &utc);
  char buffer[21];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return String(buffer);
}

String clamp(const char* value, uint8_t max) {
  if (value == nullptr) {
    return String();
  }

  String text(value);
  if (text.length() > max) {
    text.remove(max);
  }
  return text;
}

/// Adds whatever presence has accrued since the last change, and restarts the
/// clock. Called before anything reads or resets gSeenMs.
void settlePresence(uint32_t nowMs) {
  if (gPresent && gShowing) {
    gSeenMs += nowMs - gPresentSinceMs;
  }
  gPresentSinceMs = nowMs;
}

void closeShowing(uint32_t nowMs) {
  if (!gShowing) {
    return;
  }

  settlePresence(nowMs);

  gShowing = false;

  const uint32_t dwellMs = nowMs - gStartedMs;

  // Somebody pressed to get here, so they were in front of it. The whole
  // showing counts as seen, which is what makes a unit with no motion sensor
  // report a real figure rather than a zero - a floor rather than a measure,
  // and the report says which displays had a sensor.
  uint32_t seenMs = gArrival == Arrival::Manual && gSeenMs == 0 ? dwellMs : gSeenMs;

  // Seen is carved out of shown rather than counted beside it, so it can never
  // be the larger of the two. A clock that moved mid-showing is the way that
  // would otherwise happen, and an attention rate over a hundred per cent is
  // the sort of figure somebody quotes to a client once.
  if (seenMs > dwellMs) {
    seenMs = dwellMs;
  }

  gSeenMs = 0;

  if (gCount >= kMaxEntries) {
    // Counted rather than logged once. A listings feed can walk further
    // between check-ins than this buffer holds, so the server is told how many
    // it did not get and a report says "and 40 further showings" rather than
    // leaving the gap unexplained.
    gDropped++;
    return;
  }

  Entry& entry = gEntries[gCount++];

  // Copied into the row's own buffers and terminated by hand, because strncpy
  // writes no terminator when the source fills the destination. The same shape
  // Sports.cpp's copyInto() uses on the same board for the same reason.
  copyInto(entry.instanceId, sizeof(entry.instanceId), reserveInstanceId().c_str());
  copyInto(entry.cardId, sizeof(entry.cardId), gCardId.c_str());
  copyInto(entry.contentKey, sizeof(entry.contentKey), gContentKey.c_str());
  copyInto(entry.summary, sizeof(entry.summary), gSummary.c_str());
  copyInto(entry.shownAtUtc, sizeof(entry.shownAtUtc), gShownAtUtc.c_str());
  entry.dwellMs = dwellMs;
  entry.seenMs = seenMs;
  entry.fromShuffle = gArrival == Arrival::Shuffle;
  entry.wasSought = gSought;

  // No frame unless the card asked for one, which is every card that draws a
  // single thing. An empty frameId is what the server reads as "this appearance
  // was one entity", and it is the default on the wire too.
  entry.frameId[0] = '\0';
  entry.carriesScreenTime = true;

  Log::verbose("[impressions] '%s' for %lums, %lums with somebody there%s",
               entry.cardId, static_cast<unsigned long>(dwellMs),
               static_cast<unsigned long>(seenMs), entry.wasSought ? ", sought" : "");

  if (gFrameMemberCount == 0) {
    return;
  }

  // THE FRAME. Its id is the first entry's instance id, which is already unique
  // per device and costs nothing to generate. The members repeat the card, the
  // instant and the arrival, carry their own content, and carry no time at all:
  // the seconds were counted once, on the entry above.
  copyInto(entry.frameId, sizeof(entry.frameId), entry.instanceId);

  for (uint8_t i = 0; i < gFrameMemberCount; ++i) {
    if (gCount >= kMaxEntries) {
      // The frame is short rather than the showing lost. Counted like any other
      // overflow, so a report can say the attribution is incomplete instead of
      // quietly showing three games where four were on the glass.
      gDropped++;
      continue;
    }

    Entry& member = gEntries[gCount++];
    copyInto(member.instanceId, sizeof(member.instanceId), reserveInstanceId().c_str());
    copyInto(member.cardId, sizeof(member.cardId), entry.cardId);
    copyInto(member.contentKey, sizeof(member.contentKey), gFrameMembers[i].contentKey);
    copyInto(member.summary, sizeof(member.summary), gFrameMembers[i].summary);
    copyInto(member.shownAtUtc, sizeof(member.shownAtUtc), entry.shownAtUtc);
    copyInto(member.frameId, sizeof(member.frameId), entry.frameId);
    member.dwellMs = 0;
    member.seenMs = 0;
    member.fromShuffle = entry.fromShuffle;
    member.wasSought = entry.wasSought;
    member.carriesScreenTime = false;
  }

  Log::verbose("[impressions] '%s' carried %u more in one frame, sharing its %lums",
               entry.cardId, static_cast<unsigned>(gFrameMemberCount),
               static_cast<unsigned long>(dwellMs));
}

}  // namespace

void begin() {
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, true)) {
    gNextId = prefs.getUInt(kKeyCounter, 0);
    prefs.end();
  }

  // Nothing is reserved here. The first showing takes a block, so a device
  // that never draws a card never writes to flash at all.
  gIdsLeftInBlock = 0;
}

void beginShowing(const char* cardId, const char* contentKey, const char* summary,
                  Arrival arrival) {
  const uint32_t now = millis();

  closeShowing(now);

  gCardId = clamp(cardId, 64);
  gContentKey = clamp(contentKey, kMaxContentKey);
  gSummary = clamp(summary, kMaxSummary);
  gShownAtUtc = nowAsIso8601Utc();
  gStartedMs = now;
  gArrival = arrival;

  // Cleared here rather than after closeShowing() wrote them, so a card that
  // adds no members cannot inherit the last card's. closeShowing() above has
  // already consumed whatever the previous showing had.
  gFrameMemberCount = 0;

  // A card somebody pressed forward or back to reach was looked at by whoever
  // pressed, whether or not they stay on it - the whole reason a display with
  // no motion sensor still reports a seen figure.
  //
  // Credited when the showing closes, NOT by asserting presence here. Presence
  // set this way has nothing to turn it off on a unit with no sensor, so one
  // tap would have marked every card after it as seen, for as long as the
  // device stayed up. The press proves somebody was there for THIS showing and
  // says nothing about the next one.
  gSought = false;

  gShowing = true;
  gPresentSinceMs = now;
}

void endShowing() { closeShowing(millis()); }

void addFrameMember(const char* contentKey, const char* summary) {
  // Every refusal here is silent and costs one row of attribution rather than
  // the showing - see Impressions.h. A caller that gets this wrong should not
  // be able to lose the appearance it is describing.
  if (!gShowing || gFrameMemberCount >= kMaxFrameMembers) {
    return;
  }
  if (contentKey == nullptr || contentKey[0] == 0) {
    return;
  }

  FrameMember& member = gFrameMembers[gFrameMemberCount++];
  copyInto(member.contentKey, sizeof(member.contentKey), contentKey);
  copyInto(member.summary, sizeof(member.summary), summary);
}

void setPresent(bool present) {
  if (present == gPresent) {
    return;
  }

  const uint32_t now = millis();
  settlePresence(now);
  gPresent = present;
}

void markSought() { gSought = true; }

uint8_t pendingCount() { return gCount; }

uint16_t droppedCount() { return gDropped; }

const Entry* entryAt(uint8_t index) { return index < gCount ? &gEntries[index] : nullptr; }

void clearReported(uint8_t count) {
  if (count >= gCount) {
    gCount = 0;
    gDropped = 0;
    return;
  }

  // A partial report shifts the rest down rather than dropping them. The
  // server took the first `count`, and the ones behind them have not been
  // sent at all.
  for (uint8_t i = 0; i + count < gCount; ++i) {
    gEntries[i] = gEntries[i + count];
  }
  gCount -= count;

  // The dropped count belongs to the whole buffer rather than to any entry, so
  // it only clears when the buffer does.
}

void reset() {
  gCount = 0;
  gDropped = 0;
  gShowing = false;
  gPresent = false;
  gSeenMs = 0;
  gSought = false;
}

}  // namespace Impressions
