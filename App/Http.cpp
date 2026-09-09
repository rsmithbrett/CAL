#include "Http.h"

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

}  // namespace Http
