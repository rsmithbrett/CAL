#include "Tls.h"

// The root certificate bundle the ESP32 core embeds in the image. These symbols
// are produced by the build, not declared in any header, which is why they are
// spelled out here.
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t rootca_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

namespace Tls {

bool configure(NetworkClientSecure& client) {
  const size_t size =
      static_cast<size_t>(rootca_crt_bundle_end - rootca_crt_bundle_start);

  // NetworkClientSecure::setCACertBundle treats a null pointer or a zero size
  // as "detach the bundle", which disables certificate validation outright
  // rather than failing. That is the single most dangerous call in this
  // codebase to get wrong, so the arguments are checked here and a failure is
  // reported to the caller instead of quietly producing an insecure client.
  if (size == 0) {
    return false;
  }

  client.setCACertBundle(rootca_crt_bundle_start, size);
  return true;
}

}  // namespace Tls
