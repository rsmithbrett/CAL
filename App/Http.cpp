#include "Http.h"

#include <WiFi.h>
#include <esp_heap_caps.h>

#include "Config.h"
#include "Log.h"
#include "Tls.h"

namespace Http {
namespace {

/// Never destructed - both live for the entire process, the same lifetime
/// every other always-on module-level global in this codebase already has
/// (Identity's cached fields, Log's ring buffer, ...). The whole point of
/// this module is that this NetworkClientSecure's TLS session is allowed to
/// outlive any single request; a local that went out of scope would be
/// exactly the pattern this replaces.
NetworkClientSecure gClient;
HTTPClient gHttp;

bool gReady = false;

}  // namespace

void begin() {
  // Tls::configure() already distinguishes "no bundle" from a validated
  // client internally; nothing more specific to add here. Every call site
  // logs its own message off ready() the same way it used to log its own
  // per-request Tls::configure() failure.
  gReady = Tls::configure(gClient);
}

bool ready() { return gReady; }

bool beginRequest(const String& url) { return gHttp.begin(gClient, url); }

HTTPClient& client() { return gHttp; }


void diagnoseFailure(const char* tag) {
  // HTTPClient collapses every pre-response failure into a single negative
  // status - DNS, TCP connect, and the TLS handshake all surface as -1 - which
  // is not enough to act on. This walks the same three layers in order and
  // says which one actually broke, so a device that "cannot reach the server"
  // stops being one symptom and becomes a specific one.
  //
  // Written for a real, unexplained field failure: one device returning -1 on
  // every HTTPS request (check-in AND firmware manifest alike) while SNTP
  // succeeded on the same WiFi, and a second device on the same account,
  // firmware and server checked in perfectly throughout. Nothing in the log
  // could distinguish "the name did not resolve" from "the socket never
  // opened" from "the handshake was rejected", so the cause stayed a guess.
  //
  // Deliberately verbose and deliberately only called on the failure path -
  // it costs a DNS lookup and one TCP connect, which is free compared to
  // continuing to not know.
  const wl_status_t wifi = WiFi.status();

  // heap_caps_*, NOT ESP.getFreeHeap()/ESP.getMaxAllocHeap(). This line used
  // the wrappers and actively misled: a real capture of a failed check-in
  // reported maxAlloc=32756 while the [heapdiag] line beside it measured the
  // true largest 8-bit block at 22,516. A reader seeing 32,756 concludes there
  // is ample room for a TLS handshake and rules memory out - which is exactly
  // the wrong conclusion to hand someone at the start of an investigation.
  // Both figures are printed because they answer different questions: free8 is
  // "how much is there" and largest8 is "how much can actually be handed to one
  // allocation", and mbedTLS needs the second.
  Log::printf("[http] %s failed - wifi=%d rssi=%d dBm ip=%s free8=%lu largest8=%lu", tag,
              static_cast<int>(wifi), static_cast<int>(WiFi.RSSI()),
              WiFi.localIP().toString().c_str(),
              static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
              static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));

  if (wifi != WL_CONNECTED) {
    Log::printf("[http] %s: not associated with an access point - nothing above this can work",
                tag);
    return;
  }

  // Layer 1: does the name resolve at all? A failure here is DNS, not TLS,
  // and no amount of certificate work would fix it.
  IPAddress resolved;
  if (!WiFi.hostByName(Config::kServiceHost, resolved)) {
    Log::printf("[http] %s: DNS lookup for %s FAILED - the network is up but the name did not "
                "resolve",
                tag, Config::kServiceHost);
    return;
  }
  Log::printf("[http] %s: DNS ok, %s -> %s", tag, Config::kServiceHost,
              resolved.toString().c_str());

  // Layer 2: a plain TCP connect to 443, with no TLS at all. If this succeeds
  // and the real request still failed, the problem is the handshake above it
  // (certificate, cipher, or a truncated/lost handshake packet) rather than
  // reachability.
  NetworkClient plain;
  plain.setTimeout(5);
  if (!plain.connect(resolved, 443)) {
    Log::printf("[http] %s: plain TCP connect to %s:443 FAILED - unreachable at the socket "
                "level, so this is routing/firewall, not TLS",
                tag, resolved.toString().c_str());
    return;
  }
  plain.stop();

  Log::printf("[http] %s: plain TCP to %s:443 SUCCEEDED - so DNS and routing are fine and the "
              "failure is in the TLS handshake itself",
              tag, resolved.toString().c_str());

  // Layer 3: stop inferring and ask mbedTLS. Everything above narrows the
  // failure down to "the handshake", which is still a category rather than a
  // cause - a certificate that will not validate, a clock too far off for the
  // validity window, an out-of-memory on the ~32KB of session buffers, and a
  // peer that closed the connection are all "the handshake" and want four
  // different fixes.
  //
  // NetworkClientSecure::lastError() is the mbedTLS error from the actual
  // failed attempt on gClient, already rendered to text by the library. It was
  // available all along and never read, which is why a real handshake failure
  // could be narrowed to this line and no further.
  //
  // Read AFTER the probes above rather than first: they only touch `plain` and
  // WiFi, never gClient, so its error state is still the one from the request
  // that actually failed.
  char tlsError[128] = {0};
  const int tlsCode = gClient.lastError(tlsError, sizeof(tlsError));
  if (tlsCode != 0) {
    Log::printf("[http] %s: mbedTLS error %d: %s", tag, tlsCode,
                tlsError[0] != '\0' ? tlsError : "(no description available)");
  } else {
    // Worth stating rather than staying silent. A handshake that failed while
    // mbedTLS holds no error usually means the failure was above it - the
    // session was reused and the peer dropped it, or HTTPClient gave up on its
    // own timeout before TLS ever reported anything.
    Log::printf("[http] %s: mbedTLS reports NO error, so the handshake itself did not fail - "
                "suspect a dropped reused session or an HTTPClient timeout above it",
                tag);
  }
}

}  // namespace Http
