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

/// Claims the two contiguous blocks a TLS handshake needs, at the one moment
/// they are certainly available, and holds them until a handshake asks.
///
/// **Call this first in setup(), before the splash and before WiFi.** Not from
/// begin(): by the time begin() runs the network stack has taken its share and
/// the largest block is around 32,756, which is already less than the 33,434
/// this reserves. The window is the top of setup() and nowhere else.
///
/// **Why a ballast rather than handing mbedTLS a buffer.** mbedtls_ssl_setup()
/// allocates its own record buffers and takes no pointer from us, so the only
/// way to guarantee it finds two 16,717-byte holes is to own them until it
/// needs them and then get out of the way. The blocks are taken with
/// `HeapPools::kTlsCaps` so they come out of the same pool mbedTLS will ask
/// from; a bare 8-bit request can be served from PSRAM on a board that has it,
/// and would then hold 33,434 bytes somewhere no handshake is ever going to
/// look. beginRequest() frees them one statement before it opens a new session,
/// and reclaimIdleBallast() takes them back once no session is live, so between
/// sessions nothing else can move into the space and fragment it.
///
/// This is what makes a check-in independent of what drawing has done to the
/// heap. The three causes that starved handshakes - the splash's PNG decoder
/// scratch on a device with an SD card, the per-graphic RAM buffers on a device
/// without one, and ordinary fragmentation - stop mattering, because none of
/// them can take a hole that is already held.
///
/// False means even this early allocation failed, so the device could never
/// have held a session, and the caller should say so rather than discovering it
/// one handshake at a time.
bool reserveTlsBallast();

/// Whether both ballast blocks are held right now.
bool ballastHeld();

/// Takes the ballast back once no session is live, so the space is owned again
/// before the next handshake needs it. Call it from loop(); it does nothing
/// while a session is open or while the blocks are already held.
///
/// Reclaiming here rather than beside each `http.end()` keeps the lifecycle in
/// one place. Call sites close sessions in several modules and a missed one
/// would leave the device running unprotected until its next reboot, which is
/// exactly the silent half-state this reservation is meant to end.
void reclaimIdleBallast();

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
// It called stop() on gClient to hand back mbedTLS's two 16,717-byte session
// buffers - 33,434 bytes in total, which is where the "~32KB" this file used to
// say everywhere came from, and the figure is right as a TOTAL and wrong the
// moment anyone reads it as a contiguity requirement -
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
///
/// **16,717 twice holds on the Panellan too, and that is a build fact rather
/// than an assumption.** Its generated sdkconfig carries
/// `CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=16384` with
/// `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN` unset, which is the stock sizing: IN
/// and OUT are both 16,717 and a session holds 33,434. The core overlay on this
/// workstation sets `ASYMMETRIC_CONTENT_LEN` with IN 16384 and OUT 4096, giving
/// 16,717 in and 4,429 out for 21,146 - so an overlay-built ESP32-2432S028
/// binary and every other binary of the same commit disagree about the total
/// while agreeing about this constant, which is the largest single demand and
/// the only one either figure is compared against.
///
/// **And the block has to be in internal DRAM.** `mbedtls_ssl_setup()` calls
/// `mbedtls_calloc`, which under `CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC` is
/// `esp_mbedtls_mem_calloc()` allocating `MALLOC_CAP_INTERNAL |
/// MALLOC_CAP_8BIT`. PSRAM does not count toward this however much a board
/// carries - see HeapPools.h.
constexpr size_t kTlsRecordBufferBytes = 16717;

/// The largest single contiguous allocation a TLS session could be handed right
/// now. Never `ESP.getMaxAllocHeap()` - that reads a constant 32,756 on the
/// ESP32-2432S028 while the true figure has been measured anywhere from 1,780 to
/// 77,812, and it has caused several wrong diagnoses.
///
/// **Measured over internal 8-bit heap, not the bare 8-bit capability**
/// (`HeapPools::kTlsCaps`). Both generated sdkconfigs in this repository set
/// `CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC`, so mbedTLS allocates with
/// `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` and cannot reach PSRAM however much
/// of it a board carries. The two masks walk the same regions and return the
/// same figure on the ESP32-2432S028, which has no PSRAM registered in its heap,
/// so every number this fleet has recorded still reads as it always did. On the
/// Panellan, whose 2MB of PSRAM *is* registered, the bare capability reports the
/// largest hole in either pool and runs in the megabytes - which would describe
/// a device that cannot stand up a handshake as having ample room. See
/// HeapPools.h for the derivation.
///
/// The bare-capability figure is `HeapPools::largestAnyBlockBytes()`, which is
/// what `largestFreeBlock8BitBytes` carries on the wire. This module does not
/// re-export it: nothing here compares it against kTlsRecordBufferBytes, and a
/// second name for it beside this one is how the wrong one gets reached for.
size_t largestContiguousBytes();

/// Whether a NEW TLS session could be established at this instant.
///
/// This is a PROOF OF IMPOSSIBILITY when it returns false, not an estimate.
/// Below kTlsRecordBufferBytes **of internal 8-bit heap** there is no block big
/// enough for even the first of the two buffers, so `mbedtls_ssl_setup()` cannot
/// succeed - no amount of retrying, waiting, or better luck changes that. The
/// pool is load-bearing in that sentence: measured against the bare 8-bit
/// capability on a board with PSRAM this would be a proof of nothing, because
/// 2MB of PSRAM always holds a 16,717-byte hole and mbedTLS can never allocate
/// in it. Returning true is weaker: it
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

/// What one reconnect attempt actually did, at each step.
///
/// All four heap figures are internal 8-bit heap (`HeapPools::kTlsCaps`),
/// because that is the pool a session's record buffers and its chain
/// verification both allocate from. A bare-capability figure here would include
/// PSRAM on the Panellan and make a probe that refused on the floor look like a
/// probe that refused with memory to spare.
struct ReconnectProbe {
  size_t freeBeforeClose;
  size_t largestBeforeClose;
  /// Measured AFTER the session is torn down, which is the figure that decides
  /// a reconnect and is NOT the one a live sample shows: an open session is
  /// holding its record buffers, so closing it hands memory back and the block
  /// here is normally larger than anything measured while connected. Reading a
  /// live figure and reasoning about reconnection from it compares the wrong
  /// number.
  size_t freeAfterClose;
  size_t largestAfterClose;
  /// True when canOpenNewSession() refused before a handshake was attempted.
  bool gateRefused;
  /// True when mbedTLS reported it could not allocate, as against any other
  /// handshake failure. The distinction is the point of the probe: out of
  /// memory and rejected-by-the-peer want opposite investigations.
  bool allocationFailed;
  bool connected;
  int tlsError;
  uint32_t elapsedMs;
};

/// Closes the shared TLS session and immediately tries to build a new one,
/// reporting what happened at each step.
///
/// WHY THIS CANNOT BE INFERRED FROM ORDINARY TRAFFIC. Check-in and telemetry
/// POST back to back on one client, and HTTPClient reuses a live connection, so
/// a device can run for hours without ever opening a second session. Uptime on
/// a kept-alive connection therefore says nothing about whether this device
/// could reconnect if it lost that one - which is the thing that actually
/// decides whether it goes dark. Those are two different properties and only
/// one of them was ever being measured.
///
/// Three outcomes, kept apart because they want different fixes: the gate
/// refused on the contiguous-block floor, mbedTLS could not allocate, or the
/// handshake completed. Certificate validation stays on - Tls::configure()'s
/// real root, never setInsecure() - so a success here means a session that
/// would carry a real request, not one that merely opened a socket.
///
/// COSTS A HANDSHAKE, and leaves the new session open for the next request to
/// use, so nothing is wasted when it succeeds. Call it right after a SUCCESSFUL
/// check-in: the log stream is demonstrably working at that moment, which is
/// what gets the result off the device. A probe run while the device is already
/// in trouble reports into a channel that cannot send.
ReconnectProbe probeReconnect();

}  // namespace Http
