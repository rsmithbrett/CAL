#include "Log.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "Config.h"
#include "Identity.h"
#include "Tls.h"

namespace Log {
namespace {

constexpr const char* kPath = "/api/debuglog";

// The pending-buffer ceiling: this is what actually bounds memory if the
// network is down for a while, independent of (and larger than) the
// per-POST batch cap below. Once either limit here is hit, the oldest
// buffered line is dropped to make room for the newest, rather than growing
// forever or risking a failed allocation on a board this RAM-constrained.
// 200 lines / 16KB is generous headroom for several minutes of this
// firmware's actual log volume (roughly one line every few seconds in
// normal operation, brief bursts during WiFi join/check-in/update) while
// still being a small, fixed slice of the ESP32's ~320KB SRAM - and it is
// only ever paid while an admin has actually turned streaming on.
constexpr size_t kMaxBufferedLines = 200;
constexpr size_t kMaxBufferedBytes = 16384;

// The per-POST batch ceiling: deliberately smaller than the buffer above, so
// a single flush sends a reasonably sized request instead of dumping the
// entire pending buffer into one POST body (which would spike that
// request's latency and hold up the loop() driving it for longer than
// necessary). Whatever doesn't fit in one batch simply waits for the next
// poll() - the buffer cap above, not this one, is what bounds total memory.
constexpr size_t kMaxBatchLines = 40;
constexpr size_t kMaxBatchBytes = 4096;

// How often poll() actually sends, measured against millis() the same way
// every other timer in this codebase already works (checkInIntervalMs,
// lastContentFetchMs, ...). App's loop() already runs on roughly a 1-second
// cadence of its own (see App.ino's closing delay(1000)) with no hardware
// timer or second task driving anything faster - so 1000ms is the finest
// granularity poll() actually gets called at regardless of what this
// constant says; picking something shorter would only be aspirational. The
// line/byte caps above are what actually catch a sudden burst faster than
// this timer would, between one loop() iteration and the next.
constexpr uint32_t kBatchIntervalMs = 1000;

bool streaming = false;
String buffer[kMaxBufferedLines];
size_t head = 0;
size_t count = 0;
size_t bufferedBytes = 0;
uint32_t droppedSinceFlush = 0;
uint32_t lastFlushMs = 0;

void clearBuffer() {
  for (size_t i = 0; i < kMaxBufferedLines; ++i) {
    buffer[i] = String();
  }
  head = 0;
  count = 0;
  bufferedBytes = 0;
  droppedSinceFlush = 0;
}

void pushToBuffer(const String& text) {
  const size_t textCost = text.length() + 1;  // +1 for the newline joining it to the next line

  while (count > 0 && (count >= kMaxBufferedLines || bufferedBytes + textCost > kMaxBufferedBytes)) {
    bufferedBytes -= buffer[head].length() + 1;
    buffer[head] = String();
    head = (head + 1) % kMaxBufferedLines;
    --count;
    ++droppedSinceFlush;
  }

  if (count >= kMaxBufferedLines) {
    // A single line bigger than the entire buffer cap - drop it too rather
    // than looping forever trying to make room in a buffer that can never
    // fit it.
    ++droppedSinceFlush;
    return;
  }

  const size_t tail = (head + count) % kMaxBufferedLines;
  buffer[tail] = text;
  bufferedBytes += textCost;
  ++count;
}

/// Sends up to kMaxBatchLines/kMaxBatchBytes worth of the oldest buffered
/// lines. Only what the server actually accepted (HTTP 200) is removed from
/// the buffer - a failed POST leaves it untouched so nothing is lost beyond
/// what the buffer-cap eviction above already dropped for capacity reasons,
/// and the same lines are simply retried on the next poll().
void sendOneBatch() {
  if (count == 0 && droppedSinceFlush == 0) {
    return;
  }

  JsonDocument doc;
  JsonArray lines = doc["lines"].to<JsonArray>();

  if (droppedSinceFlush > 0) {
    lines.add("[" + String(droppedSinceFlush) + " lines dropped]");
  }

  size_t taken = 0;
  size_t bytesTaken = 0;
  while (taken < count && taken < kMaxBatchLines) {
    const String& candidate = buffer[(head + taken) % kMaxBufferedLines];
    const size_t cost = candidate.length() + 1;
    if (taken > 0 && bytesTaken + cost > kMaxBatchBytes) {
      break;
    }
    lines.add(candidate);
    bytesTaken += cost;
    ++taken;
  }

  NetworkClientSecure client;
  if (!Tls::configure(client)) {
    return;  // try again next poll(); nothing consumed from the buffer
  }

  HTTPClient http;
  const String url = String("https://") + Config::kServiceHost + kPath;
  if (!http.begin(client, url)) {
    return;
  }
  http.setTimeout(Config::kHttpTimeoutMs);
  http.addHeader("X-Device-Secret", Identity::deviceSecret());
  http.addHeader("Content-Type", "application/json");

  String body;
  serializeJson(doc, body);
  const int status = http.POST(body);
  http.end();

  if (status != 200) {
    // Left in the buffer to retry on the next poll(); the eviction cap above
    // is still what keeps this from growing unbounded if the outage lasts.
    return;
  }

  for (size_t i = 0; i < taken; ++i) {
    bufferedBytes -= buffer[head].length() + 1;
    buffer[head] = String();
    head = (head + 1) % kMaxBufferedLines;
    --count;
  }
  droppedSinceFlush = 0;
}

}  // namespace

void printf(const char* format, ...) {
  char scratch[256];
  va_list args;
  va_start(args, format);
  const int written = vsnprintf(scratch, sizeof(scratch), format, args);
  va_end(args);

  if (written < 0) {
    return;
  }
  if (static_cast<size_t>(written) >= sizeof(scratch)) {
    // Ran past this fixed scratch buffer. Rather than growing it (this runs
    // on every log call, including ones made from deep, stack-tight retry
    // loops), keep what fit and say so, so a truncated line reads as
    // truncated instead of silently cut off mid-word.
    static const char kMarker[] = "...(truncated)";
    constexpr size_t kMarkerLen = sizeof(kMarker) - 1;
    memcpy(scratch + sizeof(scratch) - 1 - kMarkerLen, kMarker, kMarkerLen);
    scratch[sizeof(scratch) - 1] = '\0';
  }

  line(String(scratch));
}

void line(const String& text) {
  Serial.println(text);

  if (streaming) {
    pushToBuffer(text);
  }
}

void setStreamingEnabled(bool enabled) {
  if (enabled == streaming) {
    return;
  }

  if (!enabled) {
    sendOneBatch();
    clearBuffer();
  } else {
    lastFlushMs = millis();
  }

  streaming = enabled;
}

bool streamingEnabled() { return streaming; }

void poll() {
  if (!streaming) {
    return;
  }

  const uint32_t now = millis();
  const bool timerElapsed = (now - lastFlushMs) >= kBatchIntervalMs;
  const bool bufferPressured = count >= kMaxBatchLines || bufferedBytes >= kMaxBatchBytes;
  if (!timerElapsed && !bufferPressured) {
    return;
  }

  lastFlushMs = now;
  sendOneBatch();
}

void flushNow() {
  if (!streaming) {
    return;
  }
  sendOneBatch();
  lastFlushMs = millis();
}

}  // namespace Log
