#include "Http.h"

#include <WiFi.h>
// For the per-capability heap figures releaseTlsSession() reports - ESP's own
// wrappers overstate free memory by roughly 4x on this board (49,960 against
// heap_caps' 11,340 at the same instant), so they are not usable for measuring
// whether a release actually bought anything.
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

void releaseTlsSession() {
  // Hands mbedTLS's per-session memory back between requests.
  //
  // **This is the largest thing alive during a graphics draw, and it has
  // nothing to do with graphics.** mbedTLS defaults to a 16KB inbound plus a
  // 16KB outbound content buffer, on top of session and certificate state, and
  // gClient above is deliberately a process-lifetime global so its TLS session
  // can outlive a single request. That was a sound trade while nobody had
  // measured what else needed the room - session resumption saves a full
  // handshake, and a handshake on this chip is expensive.
  //
  // Then it was measured. On device 17, at the instant a 10,568-byte
  // file-buffer allocation failed:
  //
  //   largest free block, every capability class =  6,132
  //   total free,         every capability class = 11,340
  //
  // Roughly 32KB of TLS buffers were resident while the draw had 11,340 bytes
  // to work with - and note that even a perfectly compacted heap would only
  // barely have satisfied that single allocation. That is what makes reducing
  // PEAK CONCURRENT use the actual fix, and chasing fragmentation a
  // distraction. This is the biggest single overlap available to remove.
  //
  // The cost is real and should not be glossed: the next request pays a full
  // handshake instead of resuming one. Check-ins are 60 seconds apart, so that
  // is comfortably affordable; an asset-fetch burst pays it per asset, which is
  // slower. A device that cannot draw its cards at all is the worse outcome. If
  // the handshake cost turns out to hurt, the refinement is to release around
  // draws specifically rather than after every request - not to go back to
  // holding it forever.
  //
  // Logs the delta rather than asserting a saving, so the next person reads
  // what it bought on real hardware instead of taking this comment's word.
  const size_t freeBefore = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  const size_t largestBefore = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

  gHttp.end();
  gClient.stop();

  const size_t freeAfter = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  const size_t largestAfter = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

  Log::printf(
      "[http] released TLS session: 8BIT free %u -> %u (%+d), largest block %u -> %u (%+d)",
      static_cast<unsigned>(freeBefore), static_cast<unsigned>(freeAfter),
      static_cast<int>(freeAfter) - static_cast<int>(freeBefore),
      static_cast<unsigned>(largestBefore), static_cast<unsigned>(largestAfter),
      static_cast<int>(largestAfter) - static_cast<int>(largestBefore));
}

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
  Log::printf("[http] %s failed - wifi=%d rssi=%d dBm ip=%s freeHeap=%lu maxAlloc=%lu", tag,
              static_cast<int>(wifi), static_cast<int>(WiFi.RSSI()),
              WiFi.localIP().toString().c_str(),
              static_cast<unsigned long>(ESP.getFreeHeap()),
              static_cast<unsigned long>(ESP.getMaxAllocHeap()));

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
}

}  // namespace Http
