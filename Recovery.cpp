#include "Recovery.h"

#include <Arduino.h>
#include <esp_ota_ops.h>
#include <esp_system.h>

#include "BootDiag.h"
#include "Display.h"
#include "Identity.h"
#include "Journal.h"

namespace Recovery {
namespace {
Controller controller;
bool enabled = false;

[[noreturn]] void stop(const char* message) {
  Journal::line(message);
  Display::showFailure(message, "Restart the device and try again.");
  while (true) {
    Journal::poll();
    delay(10);
  }
}
}  // namespace

void begin() {
  int32_t x = 0, y = 0;
  controller.reset(Display::readTouch(x, y));
  enabled = true;
}

void end() {
  enabled = false;
  controller.reset();
}

bool visible() { return enabled && controller.visible(); }

InputResult poll(bool gestureAllowed) {
  if (!enabled) return InputResult{};
  int32_t x = 0, y = 0;
  const bool down = Display::readTouch(x, y);
  const Screen before = controller.screen();
  const InputResult result = controller.update(millis(), down, x, y, gestureAllowed);
  if (result.redraw) {
    Display::showRecoveryMenu(controller.screen());
    if (before == Screen::Closed) Journal::line("[recovery] touch menu opened");
  }
  if (result.restore) Journal::line("[recovery] menu closed - restoring setup screen");
  return result;
}

[[noreturn]] void execute(Action action) {
  const esp_partition_t* factory = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
  if (!factory || esp_ota_set_boot_partition(factory) != ESP_OK) {
    stop("Cannot restart CAL");
  }

  switch (action) {
    case Action::WifiSetup:
      // Reopen setup, retaining both identity and the other remembered sites.
      Identity::setProvisioningForced(true);
      Journal::line("[recovery] WiFi setup requested - identity and networks kept");
      break;
    case Action::ResetIdentity:
      Identity::clearSecret();
      Identity::setProvisioningForced(false);
      if (Identity::hasSecret()) stop("Cannot clear device identity");
      Journal::line("[recovery] identity reset confirmed - networks kept");
      break;
    case Action::ReloadApplication:
      Identity::setAppReloadRequested(true);
      Identity::setProvisioningForced(false);
      if (!Identity::appReloadRequested()) stop("Cannot save reload request");
      Journal::line("[recovery] application reload confirmed - identity and networks kept");
      break;
    default:
      stop("Invalid recovery action");
  }
  Identity::setUpdateRequested(true);
  if (!Identity::updateRequested() ||
      Identity::provisioningForced() != (action == Action::WifiSetup)) {
    stop("Cannot save recovery request");
  }
  BootDiag::recordRestartIntentIfNoneRecorded(
      action == Action::ReloadApplication ? BootDiag::RestartCause::Ota
                                         : BootDiag::RestartCause::Reprovision);
  esp_restart();
}

}  // namespace Recovery
