#pragma once

#include <Arduino.h>

#include <cstdint>

/// A household's own photographs, drawn one at a time.
///
/// **The ids come from the check-in response, not from the card policy, and
/// that is the whole difference between this card and Graphic.** A graphic
/// draws whatever `CardPolicyEntry.assetId` names, which an operator sets on a
/// page. These are a family's own pictures, chosen by that family on their own
/// page, and no operator picks them - the server scopes them to the household
/// that holds this display and sends the chosen set as
/// `personalPhotoAssetIds`. See PERSONAL_PHOTOS_DESIGN.md section 5.1 for why
/// an operator never names one, and section 11.5.1 for the choosing.
///
/// **Ids, not bytes.** Each picture is fetched from
/// `/api/assets/{id}/content` through Assets.h, the same route and the same SD
/// cache every other image on this display already uses. Ten pictures in a
/// check-in response would not survive the parse: that response is the
/// project's tightest byte budget, and this module adds a short array of ids
/// to it rather than anything that scales with picture size.
///
/// **A List, not an Interstitial.** Graphic is one picture and interleaves
/// after every N cards. This is a variable-length set that cycles an item at a
/// time, which is what Kind::List means, and it is the same shape the aircraft
/// and calendar cards use. A household with four photographs gets four items
/// in the rotation; one with none reports zero and the scheduler passes over
/// the card entirely, which is the resting state for every display until a
/// family uploads something.
///
/// **Only JPEG arrives here, and the server is what guarantees it.** The
/// decoder wants 44,408 bytes in one contiguous allocation and a display past
/// setup() cannot be counted on to have them, so a stored PNG fetches, fails
/// to draw and leaves the card blank with nothing on screen saying why. The
/// server filters the list to pictures with a drawable copy before sending it
/// - see PersonalPhotoCard.DrawsOnEveryPanel - so this module does no format
/// check of its own and has no way to report one. A picture a family holds and
/// this fleet cannot draw is counted for them on their own page instead.
namespace PersonalPhotos {

/// The most photographs one display draws, matching
/// `PersonalPhotoCard.PicturesOnTheCard` on the server. The server caps the
/// list before sending it; this bound is what keeps a longer one from writing
/// past the array if the two ever disagree.
static constexpr uint8_t kMaxPhotos = 10;

/// A GUID in the canonical 8-4-4-4-12 form, which is how every asset id
/// reaches this device. Fixed rather than a String for the reason
/// Cards::CardSpec gives for its own buffers: this array is static and its
/// size has to be knowable without a heap walk.
static constexpr uint8_t kAssetIdLength = 36;

/// Replaces the held set with what this check-in carried.
///
/// Replaces rather than merges: the server sends the household's current
/// choice in full every time, so a picture missing from a later response is
/// one the family unticked or deleted, and keeping it would draw something
/// they have already removed. An empty array is a real answer and clears the
/// card.
///
/// A picture that leaves the set has its cached file deleted, so a display
/// reassigned to another household stops holding the previous one's
/// photographs - see PERSONAL_PHOTOS_DESIGN.md sections 5.1 and 15.2, and
/// invalidateDeparted() in PersonalPhotos.cpp. A set that did not change deletes
/// nothing, which is what keeps a picture the family still wants on the card.
void setFromCheckIn(const char ids[][kAssetIdLength + 1], uint8_t count);

/// How many photographs are held right now. Zero is the ordinary resting
/// state and the scheduler passes over the card.
uint16_t count();

/// Hands back the heap buffer a cardless display is holding a photograph in,
/// for the same trade Graphic::releaseRamBuffers() makes and on the same call
/// path.
///
/// A photograph is ten to twenty kilobytes, and what denies a new TLS session
/// is not the size of a block but where the block sits: a session needs 16,717
/// bytes contiguous for each of two record buffers, and one picture sitting in
/// the middle of the heap is enough to deny the second. Worth reclaiming on
/// the check-in failure path for exactly that reason. A display with an SD card
/// holds no buffer and this does nothing.
void releaseRamBuffer();

}  // namespace PersonalPhotos
