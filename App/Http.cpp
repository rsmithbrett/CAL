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
  // Cap how long a TLS handshake may block before it is abandoned.
  //
  // **The argument is in SECONDS, not milliseconds.** NetworkClientSecure
  // multiplies it by 1000 internally (NetworkClientSecure.cpp:450), so passing
  // a millisecond figure here would ask for a timeout a thousand times longer
  // than intended and look like it had done nothing.
  //
  // Why this is needed at all: the core defaults handshake_timeout to 120000 ms
  // (NetworkClientSecure.cpp:41), and http.setTimeout(kHttpTimeoutMs) does NOT
  // cover it - that is HTTPClient's read timeout, a different clock entirely.
  // A device was measured blocking its whole loop for 121,019 ms on one failed
  // handshake, which is the 120-second default firing almost exactly. For those
  // two minutes the App renders nothing, samples no touches, and sends no
  // telemetry: the device looks frozen to anyone in front of it, and looks
  // silent to the server. That is precisely the "frozen on the clock" symptom
  // this fleet has been showing.
  //
  // 15 seconds because it has to sit above the worst SUCCESSFUL handshake and
  // below anything a person would call frozen. Observed good handshakes on this
  // hardware complete inside a second (see the [assets] fetch traces, which
  // include connection setup and land at 241-787 ms); the slowest full-loop
  // iteration containing a successful check-in was about 8 s. 15 s clears that
  // with room and still turns a two-minute freeze into a hiccup.
  //
  // This does NOT fix whatever makes a handshake stall - mbedTLS reports only
  // "-1 generic error" when it happens, and that is still open. It bounds the
  // damage, which is worth having regardless of the cause.
  gClient.setHandshakeTimeout(15);

  // Tls::configure() already distinguishes "no trust source" from a validated
  // client internally; nothing more specific to add here. Every call site
  // logs its own message off ready() the same way it used to log its own
  // per-request Tls::configure() failure.
  gReady = Tls::configure(gClient);
}

bool ready() { return gReady; }

size_t largestContiguousBytes() {
  return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}

bool canOpenNewSession() { return largestContiguousBytes() >= kTlsRecordBufferBytes; }

bool beginRequest(const String& url) {
  // Pre-flight, and the reason it is worth a branch on every request: when
  // canOpenNewSession() is false the handshake CANNOT succeed - there is no
  // block large enough for the first of mbedTLS's two record buffers - and
  // attempting it anyway costs up to the full 15-second handshake timeout
  // (see begin()). A device in this state makes several requests a minute
  // across check-in, telemetry, the debug stream and every card's own fetch,
  // so those timeouts are most of its loop: it stops sampling touch, stops
  // advancing the rotation, and looks frozen to anyone in front of it while
  // achieving nothing.
  //
  // Only blocks a request that would need a NEW session. An established one
  // keeps working below this floor, which is why the check is here rather
  // than in begin() - gHttp.begin() reuses gClient's live connection when it
  // has one, and refusing that would take a working device off the air to
  // prevent a handshake it was never going to attempt.
  if (!gClient.connected() && !canOpenNewSession()) {
    Log::printf(
        "[http] refusing %s - largest 8-bit block is %lu and one TLS record buffer needs %lu, so a "
        "new session is impossible rather than unlikely. Not spending a 15s handshake timeout to "
        "discover that. This device needs a restart to talk to the server again - see "
        "Http::canOpenNewSession() and BootDiag::RestartCause::Unreachable.",
        url.c_str(), static_cast<unsigned long>(largestContiguousBytes()),
        static_cast<unsigned long>(kTlsRecordBufferBytes));
    return false;
  }
  return gHttp.begin(gClient, url);
}

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
