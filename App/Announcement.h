#pragma once

#include <Arduino.h>

/// A short admin-typed notice as a card in its own right - the text
/// equivalent of Graphic.h's picture: that module shows an admin-*chosen
/// image*, this one shows admin-*typed text*, and the two are otherwise the
/// same idea. Sits beside Weather.h, Aircraft.h, SunMoon.h and Graphic.h as
/// its own module for the same reason they are separate from one another.
///
/// **This card has no network fetch at all, and that is the one real
/// difference from Graphic.h.** A picture is an id that must be resolved
/// through the Assets cache (SD hit, or an HTTP fetch and a decode); words are
/// not - the text arrives already complete, inside the policy itself, on
/// every check-in (`CardPolicyEntry.Text` -> `Cards::PolicyEntry::text` ->
/// `Cards::CardSpec::text`, see Cards.h and CardManager::applyPolicy()). There
/// is nothing to cache and nothing that can fail to decode, so this module
/// registers no `fetch` function at all - see cardFetch's absence in
/// Announcement.cpp, and CardManager.cpp's `if (card.fetch == nullptr)`
/// guards, which already tolerate a card with none.
///
/// **Changing what a household reads is a config edit, not a firmware
/// release**, restated here for words instead of a picture: an admin's
/// reminder or house rule is something they change on a whim - it stops being
/// true, or it was never meant to be permanent - and this module deliberately
/// contains no text, no id, and no opinion about what the notice says. It
/// only ever draws whatever `Cards::CardSpec::text` currently holds.
///
/// It registers as an **interstitial**, for the same reason Graphic.h's card
/// does: a single notice is not a feed, and "show after every N other cards"
/// is the honest description of how one should appear. A list card would take
/// a fixed slot and be seen proportionally less often as other list cards
/// grow - the mistake CardManager.h records having been corrected on a
/// running device.
///
/// **No content is the ordinary resting state here, and is silent**, again
/// mirroring Graphic.h: with no `text` in the policy - every device until an
/// admin types one - this card reports zero items and the scheduler passes
/// over it entirely. There is nothing informative to say about a notice that
/// was never written, and a card reading "no announcement configured" in a
/// household's living room would be a worse outcome than one that simply
/// never appears.
///
/// **UNVERIFIED ON HARDWARE**, same standing caveat as every other card in
/// this build: checked by a clean compile and by reading, not by an actual
/// device drawing actual text yet.
namespace Announcement {

/// This card's registered id, and the `id` a policy entry must use to
/// schedule it or to give it `text`. Exposed only so the id appears exactly
/// once in the firmware, the same convention Graphic::kCardId keeps.
extern const char* const kCardId;

}  // namespace Announcement
