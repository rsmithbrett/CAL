#pragma once

#include <HTTPClient.h>
#include <NetworkClientSecure.h>

/// One shared, persistent HTTPS connection to Config::kServiceHost, reused by
/// every call site that used to open (and immediately tear down) its own.
///
/// Why this exists: this ESP32 Arduino core's HTTPClient already defaults to
/// `Connection: keep-alive` and skips closing the socket in end() when the
/// server allows it (see HTTPClient::disconnect()'s own `_reuse && _canReuse`
/// check) - that plumbing was never the problem. What defeated it was every
/// call site constructing its own `NetworkClientSecure client;` as a stack
/// local: its destructor tears down the real TLS session the moment the
/// function returns, so the *next* call - even to the same host, seconds
/// later - always pays a full TLS handshake from scratch. Every call site
/// here targets the same host (Config::kServiceHost) and this firmware runs
/// strictly out of loop() with no FreeRTOS tasks (see App.ino), so a single
/// shared connection gets the keep-alive benefit while holding open exactly
/// one TLS session's worth of memory, not eight (or nine, counting Log.cpp's
/// own debug-stream POSTs - see below).
///
/// Field data motivating this: on a live 14-card device, ESP.getMaxAllocHeap()
/// was already down to ~42996 bytes within ~2 minutes of every fresh boot and
/// ~38900 bytes by the 3-minute mark, repeatably across three boot cycles -
/// tripping the heap-health watchdog (App.ino, restarts below 60000 bytes) on
/// an almost exact ~181-182 second cycle, forever. All of this firmware's
/// HTTP traffic (check-in, telemetry, and every "due" card's own fetch) lands
/// in that same crowded first couple of minutes after boot, so the constant
/// full-handshake churn was concentrated in exactly the window this device
/// can least afford it. This does not by itself prove the handshake churn
/// (as opposed to, say, JSON parsing or PNG decode scratch space) was the
/// dominant contributor to that fragmentation - nobody has instrumented
/// maxAllocHeap before/after this specific change on real hardware yet - but
/// paying for one held-open TLS session instead of nine repeated handshakes
/// during that window is very unlikely to make fragmentation worse, and the
/// live symptom (both graphic cards failing to decode 100% of the time
/// across all three observed boots) is consistent with something eating
/// contiguous heap hardest in exactly that early window.
///
/// What "one shared connection" does NOT mean: this is not a connection pool,
/// there is no per-host lookup, and calling beginRequest() with a URL for a
/// different host than the one already connected works correctly (see
/// HTTPClient::beginInternal()'s own `_host != the_host` check, which
/// disconnects and reconnects) but defeats the whole point of this module -
/// every current call site happens to target Config::kServiceHost, and this
/// was not built to be more general than that.
namespace Http {

/// Configures the one shared NetworkClientSecure's trust bundle exactly once,
/// via Tls::configure() - must be called once from setup(), before anything
/// calls beginRequest(). Doing this once here (instead of once per request,
/// as every call site used to) is safe because Tls::configure() only attaches
/// this device's baked-in cert bundle to the client - it does not touch the
/// live connection, and it needs re-doing only if the client itself were
/// replaced, which it never is.
void begin();

/// Whether begin() actually managed to configure TLS. False for the same
/// reason Tls::configure() itself would have returned false before this
/// change (no cert bundle available) - a hard, deterministic failure for
/// this device's build, not something expected to recover between calls.
/// Every caller checks this the same way it used to check Tls::configure()'s
/// own per-request return value.
bool ready();

/// Starts a request against `url` on the shared connection - the
/// `http.begin(client, url)` half of what every call site used to do with
/// its own local client. Returns false exactly when HTTPClient::begin()
/// itself would (malformed URL); callers keep their own existing handling
/// for that. Does not touch headers or the timeout - every call site already
/// sets its own (X-Device-Secret, Content-Type where it applies,
/// Config::kHttpTimeoutMs) on client() right after this returns true, and
/// this module has no opinion on what those should be.
bool beginRequest(const String& url);

/// The one shared HTTPClient. Callers set their own headers and timeout on
/// it, then call GET()/POST()/getStream()/getStreamPtr()/end() exactly as
/// they did on their own local `http` before this change - see
/// HTTPClient::disconnect() for why calling end() here does not necessarily
/// close the underlying socket (that is the entire point).
HTTPClient& client();

/// Hands mbedTLS's per-session memory back, ending the session this module
/// otherwise keeps alive on purpose.
///
/// **Call this after a request when what happens next needs contiguous RAM
/// more than it needs a warm TLS session.** Measured on device 17: roughly
/// 32KB of mbedTLS buffers were resident while a graphics draw had 11,340
/// bytes of 8BIT heap to work with and its largest free block was 6,132 - so
/// the draw could not get the 10,568 bytes it needed even though nothing was
/// wrong with the heap. Peak concurrent use, not fragmentation, is what
/// actually fails on this board.
///
/// The trade is a full handshake on the next request instead of a resumed
/// session. At the 60-second check-in cadence that is affordable; across an
/// asset-fetch burst it is paid per asset. Not calling it at all was the
/// previous behaviour and cost the device its graphics entirely.
///
/// Logs the before/after 8BIT free size and largest block, so the saving is
/// reported from hardware rather than assumed. Safe to call when no session is
/// open - stop() on an idle client does nothing.
void releaseTlsSession();

/// Says which layer an HTTPS failure actually happened at, for a call site
/// that just got a negative status back. HTTPClient reports a DNS failure, a
/// refused TCP connect and a rejected TLS handshake all as -1, which is not
/// enough to act on; this walks name resolution, then a plain TCP connect to
/// 443, and logs which one broke.
///
/// Only worth calling on the failure path - it costs a DNS lookup and a TCP
/// connect. `tag` is the caller's own name ("checkin", "manifest", ...) so a
/// device with several failing subsystems stays readable in one stream.
void diagnoseFailure(const char* tag);

}  // namespace Http
