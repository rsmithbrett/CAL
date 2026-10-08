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
  entry.instanceId = reserveInstanceId();
  entry.cardId = gCardId;
  entry.contentKey = gContentKey;
  entry.summary = gSummary;
  entry.shownAtUtc = gShownAtUtc;
  entry.dwellMs = dwellMs;
  entry.seenMs = seenMs;
  entry.fromShuffle = gArrival == Arrival::Shuffle;
  entry.wasSought = gSought;

  Log::verbose("[impressions] '%s' for %lums, %lums with somebody there%s",
               entry.cardId.c_str(), static_cast<unsigned long>(dwellMs),
               static_cast<unsigned long>(seenMs), entry.wasSought ? ", sought" : "");
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
