#pragma once

#include <Arduino.h>

/// A picture as a card in its own right - the third content type this build
/// renders, sitting beside Weather.h and Aircraft.h as its own module for the
/// same reason they are separate from one another.
///
/// **This module provides five independently-configured instances**, ids
/// `"graphic"` through `"graphic5"`, each its own entry in the card
/// registry with its own retained state (its own cached asset, its own
/// ready flag, its own change-logging). A household that wants to rotate
/// through a seasonal notice, a house rule and a QR code no longer has to
/// pick one; it configures up to five policy entries, one per id, and each
/// behaves exactly as if it were the sole graphic card - independently
/// fetched, independently drawn, independently silent when its own
/// `assetId` is unset. Nothing here shares state across the five: instance
/// 2's asset going stale has no effect on instance 1 or any other.
///
/// Five, not three, as of the multi-instance generalisation that gave every
/// content-configurable card the same headroom (see Cards.h's own remarks
/// on kMaxCards, and the judgment call recorded in Announcement.h/QrText.h/
/// Forecast.h about which card *types* got this treatment at all). Graphic
/// was the original precedent this template idiom was extracted from; this
/// change only widens its own instance count to match its siblings, nothing
/// about the mechanism itself changed.
///
/// **None of the three has content of its own.** Weather and aircraft each
/// own a server route, a response shape and a status vocabulary; these own
/// none of that. What each one draws is whatever asset its own policy entry
/// names - `CardPolicyEntry.assetId`, carried onto that instance's own
/// descriptor as `Cards::CardSpec::assetId` (see Cards.h) - resolved through
/// the existing Assets.h cache, which already knows how to fetch an id from
/// the server, store it on SD and draw it. Nothing about caching is
/// re-implemented here, and nothing about it is duplicated three times
/// either: all three instances share one parameterized implementation
/// (`Graphic.cpp`'s `Instance<N>` template), instantiated once per id, not
/// three independent copies of the same logic.
///
/// That indirection is the whole point of the card. **Changing which picture
/// a household sees is a config edit, not a firmware release**, so this module
/// deliberately contains no image, no id beyond the three below, and no
/// opinion about what any picture is for - a seasonal graphic, a house rule,
/// a logo, a QR code, a notice. CYD-Dickey's nearest equivalents are its
/// splash and QR cards, which are exactly this card with the image hardcoded
/// in the firmware (`splashImage = "/LRBH.PNG"`) and therefore need a reflash
/// to change - and CYD-Dickey only ever had room for one at a time.
///
/// Each instance registers as an **interstitial**: it is one picture, not a
/// feed, and an interstitial's "show after every N other cards" is the
/// honest description of how a picture should appear. A list card would take
/// a fixed slot in the rotation and so appear proportionally less often as
/// the aircraft list grows - the mistake CardManager.h records CYD-Dickey
/// having made and corrected on a running device. See the registration block
/// at the bottom of Graphic.cpp.
///
/// **No content is the ordinary resting state here, and is silent, for each
/// instance independently.** With no `assetId` in an instance's policy entry
/// - which is every instance on every device until somebody sets one - that
/// instance reports zero items and the scheduler passes over it entirely.
/// Same for an asset that will not fetch or will not decode. That is
/// deliberately unlike weather and aircraft, whose "not activated" and
/// "nothing overhead" messages are real content worth a screen: there is
/// nothing informative to say about a picture that isn't there, and a card
/// reading "no image configured" in a household's living room would be a
/// worse outcome than one that simply never appears. A household that only
/// wants one picture configures only `"graphic"` and never touches the other
/// two ids; `"graphic2"` and `"graphic3"` then sit silent, exactly as
/// `"graphic"` alone used to for a device with no policy at all.
///
/// **UNVERIFIED ON HARDWARE, and further from verified than most of this
/// tree.** This firmware has zero automated tests and no way to get any, so
/// what is below is checked by a clean compile and by reading. Every layer
/// this card sits on is itself unexercised: no asset has ever been fetched by
/// a real device (`showBootSplash()` never had one to fetch), no PNG has been
/// decoded, no SD write has happened, and the server's `/api/assets/{id}`
/// route and the `assetId` policy field are both being built in parallel with
/// this. A compile proves this links, not that a picture appears.
namespace Graphic {

/// The first instance's registered id, and the `id` a policy entry must use
/// to schedule it or to give it an `assetId`. Exposed only so that the id
/// appears exactly once in the firmware.
extern const char* const kCardId;

/// The second instance's registered id - independent of `kCardId` in every
/// respect: its own cached asset, its own ready flag, its own place in the
/// rotation.
extern const char* const kCardId2;

/// The third instance's registered id, on the same terms as `kCardId2`.
extern const char* const kCardId3;

/// The fourth instance's registered id, on the same terms as `kCardId2`.
extern const char* const kCardId4;

/// The fifth instance's registered id, on the same terms as `kCardId2`.
extern const char* const kCardId5;

/// Frees every instance's direct-to-RAM asset buffer and forgets the pictures
/// they held, so the next refresh re-fetches them.
///
/// **Called when a check-in has failed, to buy back contiguous heap for the
/// TLS handshake.** On a device with no SD card every graphic falls back to
/// holding its bytes in RAM for the process lifetime (see Assets.h's
/// fetchToRam and RamAssetBuffer), and two such buffers are enough that no
/// 32KB contiguous hole remains anywhere for a new TLS session. Measured: the
/// two card-less devices in this fleet sit at largest8 21,000-26,000 while
/// drawing nothing at all, and their handshakes fail with SSL_ALLOC_FAILED
/// (-32512). A device in that state cannot check in, cannot be sent a card
/// policy and cannot be given a firmware update, so trading the pictures for
/// the connection is the right way round - and the pictures come back on the
/// next successful fetch.
///
/// Costs nothing on a device with a working SD card: those instances hold no
/// RAM buffer, so this is five null checks and no log output. Safe to call at
/// any time; it is NOT safe to call it and then assume a card still has its
/// picture.
void releaseRamBuffers();

}  // namespace Graphic
