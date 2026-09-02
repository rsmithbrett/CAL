#include "Loader.h"

#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "Identity.h"

namespace Loader {
namespace {

[[noreturn]] void bootFactoryAndRestart() {
  const esp_partition_t* factory = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
  // Nothing sensible to do if the factory partition cannot be found - that
  // would mean the partition table itself is wrong, not a recoverable runtime
  // condition. Restarting at least gives CAL's own boot-partition selection
  // (whatever it currently is) another chance rather than hanging here.
  if (factory != nullptr) {
    esp_ota_set_boot_partition(factory);
  }
  esp_restart();
}

}  // namespace

void requestUpdate() {
  Identity::setUpdateRequested(true);
  bootFactoryAndRestart();
}

void returnToLoaderForReprovisioning() {
  // Clearing networks is what makes CAL actually run Provisioning::run():
  // CAL only raises its setup access point when joinStoredNetwork() first
  // fails, and setUpdateRequested is what makes CAL attempt that join at all
  // rather than skipping straight back to bootApplication() because a
  // working application is already installed (see CAL.ino's
  // mustContactServer()).
  Identity::clearNetworks();
  Identity::setUpdateRequested(true);
  bootFactoryAndRestart();
}

}  // namespace Loader
