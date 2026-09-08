#pragma once

#include <NetworkClientSecure.h>

/// Transport security, configured in exactly one place.
///
/// This exists as its own unit because getting it wrong is silent. An
/// unvalidated connection looks identical to a validated one until somebody on
/// the household network is reading the device secret out of the headers, and
/// the device presents that secret on every request.
namespace Tls {

/// Configures a client to validate the server certificate against trusted
/// roots. Returns false if no trust source is available, and callers must treat
/// that as a hard failure rather than continuing unvalidated.
///
/// Deliberately validates against a root bundle rather than pinning the leaf.
/// Pinning is ordinarily good practice on an embedded device and is a liability
/// here: server certificates rotate on a ninety-day cycle, CAL cannot be
/// updated over the air, and a device trusting exactly one leaf certificate
/// would stop working the day that certificate is replaced - taking every unit
/// in the field with it simultaneously.
bool configure(NetworkClientSecure& client);

/// **Investigated and deliberately NOT done here: sharing one long-lived
/// NetworkClientSecure/HTTPClient pair across calls to avoid repeated TLS
/// handshakes.** Recorded here rather than only in a PR description, because
/// the next person touching a fetch() function needs the same reasoning
/// this one already worked through.
///
/// Every fetch()-shaped function in this build (CheckIn::perform(),
/// Forecast::fetch(), Aircraft's and Listings' equivalents, Telemetry::
/// report(), Assets::fetchToCard()/fetchToRamImpl(), AppUpdater::
/// newerVersionAvailable()) declares its own `NetworkClientSecure client;`
/// and `HTTPClient http;` as function-local stack variables, torn down on
/// every single call. That looks wasteful next to the fact that this exact
/// esp32 core's own HTTPClient (v3.3.11) already defaults `_reuse = true`
/// and already sends `Connection: keep-alive`, and its own end() already
/// skips closing the socket when the server's response allows reuse
/// (HTTPClient.cpp's disconnect(): `if (_reuse && _canReuse) { /* tcp keep
/// open for reuse */ }`) - the plumbing for keep-alive is already there and
/// already on by default. The reason it never fires is simpler and
/// unrelated to any of that: `NetworkClientSecure client` is a stack local,
/// so its destructor tears down the real TCP socket and the mbedtls TLS
/// session state the moment the function returns, regardless of what
/// HTTPClient's own bookkeeping wanted to keep open. Making it `static`
/// instead of a stack local is a one-line change per call site that would
/// let that already-built-in reuse logic actually take effect.
///
/// It was not made anyway, for a reason specific to this device rather than
/// to the idea in general: this whole pass of memory work exists because
/// `ESP.getMaxAllocHeap()` - not total free heap, the largest single
/// contiguous block - was measured falling to 34-43KB on real hardware (see
/// App.ino's checkHeapHealth()). A `static` client in every one of the
/// eight call sites above would hold each one's mbedtls session state
/// (in/out record buffers plus session bookkeeping - real bytes, size not
/// measured on this exact build) allocated *continuously* between fetches
/// instead of only *during* one, in exchange for skipping the handshake's
/// own alloc/free churn on whichever calls happen to land inside the same
/// server's keep-alive idle window. That trade is very plausibly a net win
/// - a handful of stable, never-freed blocks does not fragment a heap the
/// way repeated variable-sized alloc/free does, which is the entire
/// argument for gFileBuffer in Display.cpp and RamAssetBuffer in Assets.h -
/// but "plausibly a net win" is exactly the kind of claim this codebase does
/// not commit to firmware on real, unattended hardware without a device to
/// measure it against, and this is the one change on this pass's list that
/// most directly trades against the specific number
/// (kMinMaxAllocHeapBytes) this whole effort exists to keep clear of.
/// Left as a scoped, well-understood follow-up rather than eight speculative
/// call-site edits: whoever picks this up next has a live device to check
/// the actual before/after maxAllocHeap against, which is the one thing
/// missing here.
}  // namespace Tls
