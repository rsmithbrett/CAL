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

// There was a releaseTlsSession() here. It is gone, and the reasoning is worth
// keeping because the measurement behind it was sound and the conclusion was
// not.
//
// It called stop() on gClient to hand back mbedTLS's ~32KB of session buffers,
// and it worked exactly as advertised: on device 17 it returned 41,312 bytes
// and moved the largest contiguous 8BIT block from 6,132 to 36,852, after
// which an identical 10,568-byte allocation that had just failed succeeded.
// That is what established that peak concurrent use, not fragmentation, was the
// constraint.
//
// Two things were wrong with it anyway.
//
// It attacked the wrong end. The draw path was taking the contiguous block; the
// fix was to stop it taking one, not to evict the TLS session to make room. Now
// that draws stream from SD (see Display.cpp's drawPngFromSd) there is nothing
// to make room for.
//
// And gClient is shared with the debug log stream and telemetry. A client left
// unusable after stop() takes the device's own diagnostic channel down with it -
// so the failure hides its own evidence, which is precisely what happened to two
// devices. Anything revisiting TLS lifetime should use a short-lived
// per-request client rather than reaching into this shared one.

/// Bytes mbedTLS takes for ONE of its two record buffers, and therefore the
/// smallest contiguous block in which a new TLS session can possibly be set up.
///
/// Derived, not guessed. `mbedtls_ssl_setup()` allocates IN_BUFFER_LEN and
/// OUT_BUFFER_LEN as two separate flat `mbedtls_calloc(1, len)` calls, each
/// sized from compile-time constants:
///
///   MBEDTLS_SSL_HEADER_LEN                             13
///   MBEDTLS_MAX_IV_LENGTH                              16
///   MBEDTLS_SSL_MAC_ADD           (SHA-384 branch)     48
///   MBEDTLS_SSL_PADDING_ADD       (CBC compiled in)   256
///   MBEDTLS_SSL_MAX_CID_EXPANSION (CID off)             0
///   CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN              16384
///   ------------------------------------------------------
///   13 + (16 + 48 + 256 + 0) + 16384              = 16,717
///
/// Neither figure depends on the ciphersuite, on certificates, or on what
/// max_fragment_length gets negotiated - only on
/// MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH, which is not set in the shipped build.
///
/// **Note this is 16,717 twice, not "roughly 32KB" once.** That distinction
/// matters and was got wrong for a while: the total is 33,434, but the largest
/// single contiguous demand is half that, so a device with a 20,000-byte block
/// is much closer to working than a "needs 32KB" framing suggests.
constexpr size_t kTlsRecordBufferBytes = 16717;

/// The largest single contiguous 8-bit allocation this device could satisfy
/// right now. `heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)`, never
/// `ESP.getMaxAllocHeap()` - the latter reads a constant 32,756 on this board
/// while the true figure has been measured anywhere from 1,780 to 77,812, and
/// it has caused several wrong diagnoses.
size_t largestContiguousBytes();

/// Whether a NEW TLS session could be established at this instant.
///
/// This is a PROOF OF IMPOSSIBILITY when it returns false, not an estimate.
/// Below kTlsRecordBufferBytes there is no block big enough for even the first
/// of the two buffers, so `mbedtls_ssl_setup()` cannot succeed - no amount of
/// retrying, waiting, or better luck changes that. Returning true is weaker: it
/// means the first buffer could be allocated, not that both can, so a handshake
/// may still fail for ordinary reasons. The asymmetry is deliberate and is what
/// makes the false case safe to act on.
///
/// **Why a caller wants this.** It separates the only two reasons an HTTPS
/// request fails on this device, which want opposite responses:
///
///   - can open a session, request failed  -> the network or the server is the
///     problem. A restart cannot help and would be pure churn: a lost boot, a
///     fresh fragmentation cycle, and a device off the wall for several seconds
///     to fix something that was never on this device.
///   - cannot open a session               -> the device is out of contiguous
///     heap. A restart is the ONLY recovery this firmware has, observed on
///     every occasion across two devices, and every second spent not restarting
///     is a second the device is unreachable for telemetry, card policy AND
///     firmware updates simultaneously.
///
/// Before this existed the two were indistinguishable and both were handled the
/// same way - count five consecutive failures, then restart - which was too
/// slow for the second case and wrong for the first.
///
/// Note the live session is a separate question. A device already holding an
/// established session keeps using it happily below this floor; what it cannot
/// do is get a new one once that session drops. That is exactly how devices 12
/// and 17 were found rendering their cards perfectly off SD while invisible to
/// the server - see BootDiag::RestartCause::Unreachable.
bool canOpenNewSession();

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
