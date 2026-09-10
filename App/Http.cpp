#include "Http.h"

#include <WiFi.h>

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
