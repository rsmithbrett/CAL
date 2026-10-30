#include "Recovery.h"

#include <Arduino.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <PresenceInput.h>

#include "BootDiag.h"
#include "Display.h"
#include "Identity.h"
#include "Journal.h"

namespace Recovery {
namespace {
Controller controller;
bool enabled = false;
PresenceTest::State presence;
uint32_t lastSampleMs = 0;
uint32_t lastDrawMs = 0;
GpioMonitor::State gpio;
uint32_t lastGpioSampleMs = 0;

void sampleGpio(uint32_t now, bool first = false) {
  if (!first && static_cast<uint32_t>(now - lastGpioSampleMs) < 50) return;
  for (const uint8_t pin : GpioMonitor::kInputs) {
    const uint32_t value = GpioMonitor::kind(pin) == GpioMonitor::Kind::Analog
                               ? analogReadMilliVolts(pin) : (digitalRead(pin) == HIGH ? 1 : 0);
    gpio.sample(pin, now, value);
  }
  lastGpioSampleMs = now;
}

void samplePresence(uint32_t now) {
  if constexpr (PresenceInput::kSupported) {
    if (!presence.hasReading || static_cast<uint32_t>(now - lastSampleMs) >= PresenceInput::kSampleIntervalMs) {
      presence.sample(now, analogReadMilliVolts(static_cast<uint8_t>(PresenceInput::kPin)),
                      PresenceInput::kHighThresholdMillivolts);
      lastSampleMs = now;
    }
  }
}

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
  const InputResult result = controller.update(millis(), down, x, y, gestureAllowed,
                                                GpioMonitor::kPortCount);
  if (result.redraw) {
    if (controller.screen() == Screen::PresenceTest) {
      presence.reset();
      if constexpr (PresenceInput::kSupported) {
        analogSetPinAttenuation(static_cast<uint8_t>(PresenceInput::kPin), ADC_11db);
      }
      samplePresence(millis());
      Display::showPresenceTest(presence, millis(), PresenceInput::kSupported);
      lastDrawMs = millis();
    } else if (controller.screen() == Screen::GpioMonitor) {
      if (before != Screen::GpioMonitor) {
        gpio.reset();
        for (const uint8_t pin : GpioMonitor::kInputs) {
          if (GpioMonitor::kind(pin) == GpioMonitor::Kind::Analog) analogSetPinAttenuation(pin, ADC_11db);
          else pinMode(pin, INPUT); // no output, no pull-up/down, allowlisted inputs only
        }
        sampleGpio(millis(), true);
      }
      Display::showGpioMonitor(gpio, controller.gpioAll(), controller.gpioPage(), millis());
      lastDrawMs = millis();
    } else {
      Display::showRecoveryMenu(controller.screen());
    }
    if (before == Screen::Closed) Journal::line("[recovery] touch menu opened");
  }
  if (controller.screen() == Screen::GpioMonitor && !result.redraw) {
    const uint32_t now = millis();
    sampleGpio(now);
    if (static_cast<uint32_t>(now - lastDrawMs) >= 250) {
      Display::showGpioMonitor(gpio, controller.gpioAll(), controller.gpioPage(), now, false);
      lastDrawMs = now;
    }
  }
  if (controller.screen() == Screen::PresenceTest && !result.redraw) {
    const uint32_t now = millis();
    samplePresence(now);
    if (PresenceInput::kSupported && static_cast<uint32_t>(now - lastDrawMs) >= 250) {
      Display::showPresenceTest(presence, now, true, false);
      lastDrawMs = now;
    }
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
  // Persist the return-to-CAL intent before any destructive change. A power
  // loss after clearing identity must not take the healthy-App shortcut.
  Identity::setUpdateRequested(true);
  if (!Identity::updateRequested()) stop("Cannot save recovery request");

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
  if (Identity::provisioningForced() != (action == Action::WifiSetup)) {
    stop("Cannot save recovery request");
  }
  BootDiag::recordRestartIntentIfNoneRecorded(
      action == Action::ReloadApplication ? BootDiag::RestartCause::Ota
                                         : BootDiag::RestartCause::Reprovision);
  esp_restart();
}

}  // namespace Recovery
