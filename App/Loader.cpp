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
  // Deliberately does NOT call Identity::setProvisioningForced(true). That
  // flag exists for one reason - CAL.ino's BOOT-button gesture ("we moved it
  // to a new house"), where a human is deliberately asking to redo WiFi setup
  // and skipping straight to the portal is the whole point (see CAL.ino's own
  // remarks on why it must not try the stored network first there). A
  // rejected device secret has nothing to do with which network this unit is
  // on - the network is almost certainly still right and still in range - so
  // forcing the portal here just stranded a device on a screen asking a
  // person to redo WiFi it already knew, for a problem that was never about
  // WiFi. setUpdateRequested alone is enough: it is what makes CAL attempt
  // its own join at all rather than skipping straight back to
  // bootApplication() because a working application is already installed
  // (see CAL.ino's mustContactServer()), and CAL.ino's ordinary
  // joinStoredNetwork()-first path only falls back to the portal if that
  // actually fails - exactly the behaviour a secret-only problem should get.
  //
  // Identity::clearSecret() is the other half, and without it this whole
  // function was a no-op loop found live: CAL.ino only ever calls
  // Enrollment::requestKey() when `!Identity::hasSecret()`, so a unit that
  // still holds the very secret the server just rejected sails past that
  // check, re-fetches a manifest that says its version is already current,
  // and hands straight back to the same App - which fails its next check-in
  // the exact same way and comes right back here. Forgetting the secret
  // before rebooting is what actually breaks the loop: the next CAL boot
  // takes the `awaitKeyAssignment()` path and calls RegisterViaMacAddress,
  // which - for a device an admin has genuinely reset for re-registration -
  // succeeds and hands back a fresh secret this time.
  Identity::clearSecret();
  Identity::setUpdateRequested(true);
  Log::line("[loader] returning to CAL for reprovisioning");
  bootFactoryAndRestart();
}

}  // namespace Loader
