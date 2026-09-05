#pragma once

#include <Arduino.h>

/// A scannable QR code plus an optional caption - the QR equivalent of
/// Announcement.h's admin-typed notice, and structured after it almost
/// verbatim: **no network fetch at all**, for the same reason. The QR
/// payload and the caption both arrive complete on every check-in
/// (`CardPolicyEntry.QrData`/`.Text` -> `Cards::PolicyEntry::qrData`/`.text`
/// -> `Cards::CardSpec::qrData`/`.text`, see Cards.h and
/// CardManager::applyPolicy()), so there is nothing to cache and nothing
/// that can fail to decode over the network - only at render time, which
/// Display::showQrTextCard() already guards against (see its own remarks).
/// This module therefore registers no `fetch` function, exactly like
/// Announcement.cpp.
///
/// **Two fields, two different roles - this is the one place this card
/// differs from Announcement.** Announcement has a single field (`text`)
/// that is both its content and its required-content gate. This card has
/// two: `qrData` is the actual payload encoded into the code, and is
/// required - with nothing to encode there is no QR code to draw, so the
/// card reports zero items exactly as Announcement does with no text. `text`
/// here is a caption shown alongside the code, and is optional - the same
/// role CAL's own bootloader QR screen already gives a caption (see root
/// Display.cpp's showQr(), whose `caption`/`subCaption` parameters are
/// supplementary, while the URL itself is always drawn). A QR card with
/// qrData but no text is a perfectly ordinary, silent, caption-less code;
/// one with text but no qrData is the announcement card's own "nothing
/// typed yet" case restated for a different field, and reports zero items
/// the same way.
///
/// **Changing what a household scans is a config edit, not a firmware
/// release**, restated here for a code instead of a picture or a sentence:
/// what URL or data a QR points to is something an admin changes on a
/// whim - a temporary guest-network password, a one-off event link, a house
/// rule that gained a web page - and this module deliberately contains no
/// URL, no id, and no opinion about what the code encodes. It only ever
/// draws whatever `Cards::CardSpec::qrData`/`.text` currently hold.
///
/// It registers as an **interstitial**, for the same reason Announcement's
/// card does: a single code is not a feed, and "show after every N other
/// cards" is the honest description of how one should appear.
///
/// **No content is the ordinary resting state here, and is silent**, again
/// mirroring Announcement: with no `qrData` in the policy - every device
/// until an admin types one - this card reports zero items and the
/// scheduler passes over it entirely.
///
/// **UNVERIFIED ON HARDWARE**, same standing caveat as every other card in
/// this build: checked by a clean compile and by reading, not by an actual
/// device drawing an actual scannable code yet.
namespace QrText {

/// This card's registered id, and the `id` a policy entry must use to
/// schedule it or to give it `qrData`/`text`. Exposed only so the id
/// appears exactly once in the firmware, the same convention
/// Announcement::kCardId and Graphic::kCardId keep.
extern const char* const kCardId;

}  // namespace QrText
