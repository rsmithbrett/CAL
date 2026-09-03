#include "Loader.h"

#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "Identity.h"
#include "Log.h"

namespace Loader {
namespace {

// Every path back to CAL passes through here, which makes this the one place
// that needs to flush the remote debug stream (see Log::flushNow()) before
// rebooting - not each caller individually. esp_restart() below discards
// everything in RAM; without this, whatever explained the reboot (a
// requested update, a rejected secret) would never make it to a server
// someone is watching the stream on.
[[noreturn]] void bootFactoryAndRestart() {
  const esp_partition_t* factory = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
  // Nothing sensible to do if the factory partition cannot be found - that
  // would mean the partition table itself is wrong, not a recoverable runtime
  // condition. Restarting at least gives CAL's own boot-partition selection
  // (whatever it currently is) another chance rather than hanging here.
  if (factory != nullptr) {
    esp_ota_set_boot_partition(factory);
  } else {
    Log::line("[loader] factory partition not found - restarting anyway");
  }
  Log::flushNow();
  esp_restart();
}

}  // namespace

void requestUpdate() {
  Identity::setUpdateRequested(true);
  Log::line("[loader] update requested - rebooting into CAL");
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
  Log::line("[loader] returning to CAL for reprovisioning");
  bootFactoryAndRestart();
}

}  // namespace Loader
