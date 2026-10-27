#line 1 "C:\\Users\\Administrator\\Documents\\CAL\\App\\FeaturedListings.h"
#pragma once

#include <Arduino.h>

/// Properties an agent has sent to this household, by name.
///
/// **Why this is not the listings card.** Listings.h shows a postcode's market:
/// public, the same for every display in the area, and resolved from a target
/// zip. These belong to one named person - they arrived in an email an agent
/// forwarded for them - and a display shows them only because its owner is
/// that person. Two different things with two different gates, so two cards.
///
/// The practical consequence is on the server: a personal match must never be
/// reachable from the anonymous by-zip route, and a Brand switching off
/// regional listings must not also switch off somebody's own house hunt. On
/// the device the consequence is smaller but real - a distinct card id on
/// every impression, so time spent on somebody's own property search is never
/// averaged into generic rotation.
///
/// **It is domain-agnostic on purpose.** The server sends a title, a headline,
/// an optional subhead, a price and up to three already-labelled facts. This
/// file does not know what a bedroom is, or a beam, or engine hours - it draws
/// whatever labels arrive. A boat broker's listings come down the same pipe
/// and need no firmware change, which is the whole reason the server sends
/// labels rather than field names.
namespace FeaturedListings {

enum class Status {
  Ok,
  Empty,             // configured for this household, nothing current. Their matches have
                     // all expired or been withdrawn, which is a real answer rather than
                     // an error - and distinct from NotConfigured below, because a card
                     // that vanishes reads as broken where one saying "nothing right now"
                     // reads as true.
  NotConfigured,     // nobody has ever matched a listing to this display's owner.
  NotActivated,
  ProviderDisabled,
  AuthError,
  NetworkError,
};

/// The most either half of a fact can be. A fact is drawn into a half-column
/// at 9pt, which fits around fifteen characters before the layout ellipsizes
/// it, so anything longer was never going to reach the glass. The server caps
/// the same figure (FeaturedListingsService.MaxFactTextLength), which is what makes
/// this a contract rather than a device-side truncation nobody sees.
constexpr uint8_t kMaxFactText = 24;

/// One fact, already labelled by the server. The firmware prints both halves
/// and interprets neither.
///
/// **Fixed buffers, not Strings, and this is the one place in the card where
/// that is worth the awkwardness.** Three facts on each of five cards is
/// thirty of them, and a String is a separate heap allocation that lives as
/// long as the result does - which is until the next fetch, straddling the
/// next TLS handshake. On this device the binding constraint is not free heap
/// but the largest contiguous block, which mbedTLS needs 16,717 bytes of,
/// twice; thirty small live allocations scattered through it is exactly the
/// placement cost CAL commit 9df1fc9 documents. The rest of the card keeps
/// Strings because they are four per card and genuinely variable-length; these
/// are thirty, short, and fixed in shape.
struct Fact {
  char label[kMaxFactText + 1] = {0};
  char value[kMaxFactText + 1] = {0};
};

constexpr uint8_t kMaxFacts = 3;

/// The longest image cache key, matching Assets::kMaxIdLength - the server builds a content
/// key plus eight characters of a hash, which is 41.
constexpr uint8_t kMaxImageKey = 48;

/// The most a 240px panel fits beside a headline and a price, and the same cap
/// the server applies. Asking for more and discarding the rest would spend the
/// heap of a device short of it.
constexpr uint8_t kMaxCards = 5;

struct Card {
  /// What the device echoes back on every impression of this card.
  ///
  /// **Server-minted, and the join for everything measured about this
  /// listing.** Unlike the listings card, which derives a key from the row it
  /// drew, this one is told: the server owns the association being measured,
  /// so it mints the key with it. Opaque here - nothing in this file parses or
  /// shortens it, because the far end matches it exactly.
  String contentKey;

  /// The keyword-derived line - "New match", "Price drop". Empty when no rule
  /// matched, and the card then draws without one rather than inventing a
  /// label from a vocabulary that belongs to whoever sent the mail.
  String title;

  String headline;
  String subhead;

  /// Negative means the source did not state one. Never zero: a card showing
  /// "$0" is worse than one showing no price at all.
  int32_t price = -1;

  /// What `price` replaced, drawn as a second smaller line under it.
  ///
  /// **Negative means the email stated none**, which is the ordinary case and
  /// the shape `price` has always used for "not stated". A new match, a status
  /// change and an alert about a house that has held one price all carry one
  /// figure.
  ///
  /// **Nothing here works out that a price moved.** The sender says so and
  /// names both figures, and the server stores the one it struck through. So
  /// this is drawn when it arrives and not drawn when it does not, with no
  /// comparison against `price` and no reading of how old the listing is. A
  /// property that dropped once and then sat gets this cleared by its next
  /// alert, server-side, rather than by a rule here.
  ///
  /// Older firmware has no member for this and the server sends it last and
  /// optional, so a binary predating it ignores the member and draws the card
  /// it drew before - the compatibility gate that closed on 2026-09-22 permits
  /// nothing else.
  int32_t previousPrice = -1;

  Fact facts[kMaxFacts];
  uint8_t factCount = 0;

  /// What this card's picture is cached under, empty when it has none.
  ///
  /// **The server builds this so it CHANGES when the picture changes.** A key that stayed
  /// the same would leave a replaced photograph sitting behind the file already on the
  /// card, so a correction an operator can see on their own screen would never reach the
  /// glass. Fixed buffer for the same reason the facts are: five of these live across the
  /// next TLS handshake, and the binding constraint on this board is contiguous heap.
  char imageKey[kMaxImageKey + 1] = {0};
};

struct Result {
  Status status = Status::NetworkError;
  String message;
  Card cards[kMaxCards];
  uint8_t count = 0;

  /// True when the fetch could not be begun or did not complete - see
  /// Forecast::Result::serviceUnreachable for why this is distinguished from
  /// every other failure: it is the one case where the last good answer is
  /// still the best available account of the world, so the card keeps showing
  /// what it had instead of painting an error over it.
  bool serviceUnreachable = false;
};

/// Fetches GET /api/featuredlistings/mine. Registered with the scheduler from the .cpp
/// like every other card module.
Result fetch();

}  // namespace FeaturedListings
