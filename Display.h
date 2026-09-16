#pragma once

#include <Arduino.h>

/// Everything CAL draws.
///
/// CAL's screens are deliberately plain and few. It is the one component that
/// cannot be updated without physically recovering the unit, so it renders
/// status and hands off - it does not attempt to render product content.
namespace Display {

/// Brings the panel up. Idempotent, and not actually required before the draw
/// functions below - each one brings the panel up itself if this has not been
/// called. That is deliberate rather than defensive tidiness: CAL has one path
/// (the immediate handover on a therapeutic restart) where it draws nothing at
/// all and never calls this, and it must still be able to put a failure screen
/// on the glass if the handover then fails. See BOOT_SCREEN_OWNERSHIP.md.
///
/// Still call it explicitly at the top of any boot that is going to draw: it is
/// what satisfies the two-second rule, and "light the panel now" is worth
/// saying out loud where the ladder can be read top to bottom.
void begin();

/// The neutral first-boot splash. CAL is flashed before the device is assigned
/// to a brand, so there is no branding to show yet; this is the generic mark.
void showNeutralSplash();

/// The cached brand splash, if one was previously downloaded and stored. Falls
/// back to the neutral splash when absent. Returns false if nothing was cached.
bool showBrandSplash();

/// A single line of status with an optional detail line beneath it. Used for
/// every step of the boot ladder, so a device that stalls says where.
void showStatus(const String& headline, const String& detail = "");

/// A failure the household can act on. Never an error code on its own - the
/// screen must say what to do about it.
void showFailure(const String& headline, const String& whatToDo);

/// Renders a QR alongside a caption and the URL in text. The URL is always
/// shown as characters too: the camera fails often enough, and a code nobody
/// can type is a support call.
void showQr(const String& url, const String& caption, const String& subCaption = "");

/// Progress during a firmware download. Percent is 0-100.
void showUpdateProgress(uint8_t percent, const String& version);

}  // namespace Display
