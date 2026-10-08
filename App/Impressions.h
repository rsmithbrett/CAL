#pragma once

#include <Arduino.h>

/// What this display drew, buffered until the next check-in.
///
/// The server keeps every showing for ninety days and rolls them into
/// quarter-hour rows kept for thirteen months, so a report can answer which
/// listings got looked at and when. See CARD_IMPRESSIONS_DESIGN.md in the
/// server repository, and the superseded section of
/// BOOT_BUTTON_AND_IMPRESSIONS_DESIGN.md here, which had the device keeping
/// totals instead.
///
/// Three facts only this device can answer, which is why they are reported
/// rather than worked out on the server:
///
/// - **Which item was on the glass.** The provider cache behind a card can be
///   replaced between the draw and its delivery, so reconstructing it there
///   would be a guess that looks exactly like a fact.
/// - **Whether a person was there.** The motion sensor, or somebody working
///   the controls. Either is proof, and a card crossed during a rewind was
///   looked at whether or not they stopped on it.
/// - **Whether it came up in the interleave.** The server could derive it from
///   the policy and a draw sequence, and would be wrong whenever the policy
///   changed in between, in a way nothing downstream could detect.
///
/// One entry per showing, not a total per card. A total loses the time within
/// the check-in interval, so a display reporting every half hour could never
/// produce the quarter-hour shape of a day.
namespace Impressions {

/// How many showings are held between check-ins.
///
/// A fifteen-second dwell fills this in about twelve minutes, which is shorter
/// than some check-in intervals, so overflow is an ordinary condition rather
/// than a fault - which is why it is counted and reported rather than logged.
constexpr uint8_t kMaxEntries = 48;

/// The longest summary carried, matching what the server's column takes.
constexpr uint8_t kMaxSummary = 200;

/// The longest content key carried.
constexpr uint8_t kMaxContentKey = 64;

/// How a showing came up.
enum class Arrival : uint8_t {
  /// The dwell timer advanced to it.
  Rotation,
  /// A singleton interleaved into the rotation.
  Shuffle,
  /// Somebody pressed forward or back.
  Manual,
};

/// One finished showing, in the shape the check-in sends.
struct Entry {
  /// Device-local and unique, "<mac>:<counter>". The server deduplicates on
  /// it, which is what makes a retried check-in safe.
  String instanceId;

  /// The policy entry's own id, as this firmware knows it.
  String cardId;

  /// Which listing or item was on the glass. Empty for a card that is its own
  /// content, such as a clock.
  String contentKey;

  /// What the card was showing, one line, as the device composed it then.
  String summary;

  /// When it came up, ISO 8601 UTC by this device's clock. A display that has
  /// not reached NTP is wrong here, which the server knows: it stamps its own
  /// arrival time and every report reads that one.
  String shownAtUtc;

  /// How long it stayed up.
  uint32_t dwellMs;

  /// How much of that a person was confirmed there. Never more than the dwell.
  uint32_t seenMs;

  /// Whether it came up through the interleave.
  bool fromShuffle;

  /// Whether somebody navigated to it and stayed.
  bool wasSought;
};

/// Reads the persisted instance counter. Call once at startup, before any
/// showing begins.
void begin();

/// Starts timing a showing, closing the one before it.
///
/// Called from the one place a card goes up. `contentKey` and `summary` may be
/// null or empty for a card that is its own content, and both are copied,
/// because the strings a card hands back are rebuilt on its next draw.
void beginShowing(const char* cardId, const char* contentKey, const char* summary,
                  Arrival arrival);

/// Closes the showing on screen without starting another. For going blank: a
/// policy that removed every card, or a shutdown.
void endShowing();

/// Tells the buffer whether a person is there right now.
///
/// Both sources call it: the motion sensor on a state change, and the touch
/// handler on any deliberate press. Presence accrues against whatever is on
/// screen, so a run of back presses credits every card it crossed.
///
/// Idempotent, so the motion poll can call it every loop without reasoning
/// about edges.
void setPresent(bool present);

/// Records that somebody navigated to the card on screen and stayed on it.
///
/// Narrower than presence, deliberately: a card flicked past on the way
/// somewhere else was seen and not sought. Called when the auto-advance hold
/// expires with the card still up, which is this firmware's own definition of
/// having stayed.
void markSought();

/// How many finished showings are waiting to be reported.
uint8_t pendingCount();

/// How many showings were dropped because the buffer was full.
///
/// Reported alongside the entries so a report can say "and 40 further
/// showings" rather than dropping them with nothing to say so.
uint16_t droppedCount();

/// Copies up to `max` finished showings into `out`, oldest first. The showing
/// currently on screen is not among them: it has no dwell yet.
uint8_t snapshot(Entry* out, uint8_t max);

/// Forgets the showings the server acknowledged, and the dropped count with
/// them.
///
/// Not called after a failed check-in: the entries ride the next one instead,
/// and the server deduplicates on the instance id, so a retry cannot
/// double-count.
void clearReported(uint8_t count);

/// Drops everything buffered and the showing in progress.
void reset();

}  // namespace Impressions
