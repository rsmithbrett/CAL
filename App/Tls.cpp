#include "Tls.h"

#include "Log.h"
#include "TlsRoots.h"

namespace Tls {

bool configure(NetworkClientSecure& client) {
  // setCACert, not setCACertBundle. The core's ~150-certificate Mozilla
  // snapshot was what this used, and validating the service's four-level
  // ECDSA P-384 chain against it ran out of heap part way through the bignum
  // arithmetic - MBEDTLS_ERR_MPI_ALLOC_FAILED, which the bundle layer reports
  // as "Certificate matched but signature verification failed". TlsRoots.h
  // carries the full measurement, the two devices it was taken from, and the
  // trade this pin accepts. Read it before changing this call.
  //
  // The argument is a compile-time string literal in .rodata with static
  // storage duration, so it deliberately outlives this function: the client
  // keeps the pointer rather than copying the PEM. That is the same lifetime
  // contract the old linker-symbol bundle relied on.
  client.setCACert(TlsRoots::kIsrgRoots);

  // No failure path any more, and that is a change worth noticing rather than
  // glossing. The bundle version could genuinely fail: it computed a size from
  // two linker symbols, and setCACertBundle treats a null pointer or a zero
  // size as "detach the bundle" - disabling certificate validation outright
  // instead of erroring, which made it the most dangerous call in this
  // codebase to get wrong and worth checking. A string literal can be neither
  // absent nor zero-length, so there is nothing left to check, and a
  // conditional here would be unreachable code implying a risk that no longer
  // exists.
  //
  // The bool return and Http.cpp's ready()/gReady stay regardless, because a
  // future trust source that CAN fail - a certificate loaded from SD or NVS,
  // say - should not require rebuilding this contract and every call site's
  // error handling along with it.
  Log::verbose("[tls] trust anchors: ISRG Root X2 (ECDSA) + X1 (RSA), pinned - not the core bundle");
  return true;
}

}  // namespace Tls
