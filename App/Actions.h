#pragma once

#include <Arduino.h>

/// Card buttons, and the queue of presses waiting to ride the next check-in.
///
/// Two halves that only touch each other at the moment of a press:
///
/// 1. **Definitions** - which buttons a card should draw, handed down by the
///    server on every check-in (`cardActions`). Held in RAM only; the server
///    re-sends them every time, so persisting them would just be another
///    thing to fall out of sync.
/// 2. **The pending queue** - presses recorded but not yet acknowledged.
///    NVS-backed, because a press that a power cycle eats is a press the
///    household believes they made.
///
/// **The device reports intent, never meaning.** `actionId` is opaque here.
/// This firmware knows "im-ok" was pressed; it does not know an email goes
/// out, or to whom. That binding is a database row in the server's Commands
/// domain, so changing who gets notified never requires a firmware release
/// across the fleet.
///
/// **Passive push, no confirmation.** A press is recorded locally, rides the
/// next ordinary check-in, and this module's job ends there. There is no
/// round trip, no "sent" state, and deliberately no UI built on the
/// acknowledgement below - the user gets no confirmation by design.
///
/// **A single press fires the action.** No confirm tap, per the user's
/// explicit decision.
///
/// **UNVERIFIED ON HARDWARE.** Nothing in this file has run on a device.
/// This firmware has no automated tests and cannot have any; a clean compile
/// proves the NVS and JSON calls type-check, not that a finger on the glass
/// puts a row in NVS or that the server accepts what comes out.
namespace Actions {

/// One button, exactly as the server described it. `label` is drawn verbatim -
/// the server does not describe geometry, and this firmware does not
/// interpret the text.
struct Definition {
  String cardId;
  String actionId;
  String label;
};

/// One recorded press, in the shape CheckIn.cpp serialises into
/// `pendingActions`.
struct Pending {
  String actionId;
  /// Device-local and unique: "<device>:<monotonic counter>", the counter
  /// persisted in NVS so it keeps climbing across reboots. This exists
  /// PURELY for server-side dedup - if a check-in succeeds but its response
  /// is lost, the device re-sends the same press and the recipient must not
  /// be notified twice. It is not user-visible confirmation; do not build UI
  /// on it.
  ///
  /// The prefix is this device's MAC address rather than its server-side
  /// device id, because nothing has ever told a device its own id (see
  /// CheckInModels.cs's own remarks on why the check-in request carries no
  /// DeviceId at all). The server dedups on (device, instanceId) and already
  /// knows which device is calling, so the prefix only has to be stable and
  /// device-local, which a MAC is.
  String instanceId;
  String pressedAtUtc;
};

/// A card draws at most this many buttons - what fits on a 320px-wide panel
/// beside the corner clock without crowding the card's own content.
static constexpr uint8_t kMaxButtonsPerCard = 3;
static constexpr uint8_t kMaxDefinitions = 8;
/// The queue is small on purpose. Check-in drains it on the server's own
/// cadence (minutes), so a queue this deep only fills if check-ins have been
/// failing for a long stretch - at which point the *earliest* presses are the
/// ones that still describe what happened, so a press arriving at a full
/// queue is dropped and logged rather than evicting one already recorded.
static constexpr uint8_t kMaxPending = 8;

void begin();

/// Replaces the whole definition set with what the latest check-in returned.
/// An empty set is normal and means no card draws any buttons.
void applyDefinitions(const Definition* definitions, uint8_t count);

/// The buttons `cardId` should draw right now, in the order the server listed
/// them. Returns how many were written to `out`.
uint8_t forCard(const char* cardId, Definition* out, uint8_t maxOut);

/// Records a press: assigns the next instanceId, stamps it with the current
/// UTC time, and writes it to NVS. Returns false when the queue is full (see
/// kMaxPending) or NVS refused the write.
bool recordPress(const Definition& definition);

uint8_t pendingCount();

/// Copies the queue out for serialisation into a check-in request. Returns
/// how many entries were written.
uint8_t pendingSnapshot(Pending* out, uint8_t maxOut);

/// Drops every queued press whose instanceId the server just acknowledged in
/// `acceptedActionIds`. This is the one-shot-consume handshake in the
/// opposite direction from FirmwareUpdateForced: the device keeps carrying a
/// press until the server confirms it has it, so a lost response costs a
/// duplicate send (which dedup absorbs) rather than a lost press.
void clearAccepted(const String* acceptedIds, uint8_t count);

}  // namespace Actions
