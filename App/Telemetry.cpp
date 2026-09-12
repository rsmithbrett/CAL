#include "Telemetry.h"

#include <ArduinoJson.h>
#include <WiFi.h>
// For the per-capability heap figures reported alongside ESP.getFreeHeap()
// below. See the block above free8Bit for why the Arduino wrappers are not
// enough on their own, and Http.cpp/Display.cpp for the same include with the
// same justification.
#include <esp_heap_caps.h>

#include "Assets.h"
#include "BootDiag.h"
#include "Config.h"
#include "HeapRatchet.h"
#include "Http.h"
#include "Identity.h"
#include "Log.h"
#include "PowerProbe.h"
#include "SdStorage.h"

namespace Telemetry {
namespace {

constexpr const char* kPath = "/api/telemetry";

}  // namespace

void report(const char* lastCheckInOutcome) {
  // This report's own allocations - the JsonDocument's pool, the body String,
  // HTTPClient's headers - belong to a phase, and the honest phase for them is
  // Service rather than the CheckIn scope this function is called from inside
  // (see performCheckIn()). Opened as the very first statement so the Scope's
  // entry observation closes out everything check-in did BEFORE the report
  // starts, which is what keeps the identity in HeapRatchet.h tight: the only
  // thing left unaccounted between the buckets and the live largest-block
  // figure read below is what this function itself has done in between, and
  // that is a handful of Strings rather than a whole check-in.
  //
  // An instrument reporting on a channel that itself allocates is a real
  // hazard, and this is the answer to it: the cost is not hidden, it is
  // measured, and it lands in a bucket that says so.
  const HeapRatchet::Scope scope(HeapRatchet::Phase::Service, "telemetry");

  if (!Http::ready()) {
    Log::line("[telemetry] TLS setup failed, skipping this report");
    return;
  }

  const String url = String("https://") + Config::kServiceHost + kPath;
  if (!Http::beginRequest(url)) {
    Log::line("[telemetry] could not begin request, skipping this report");
    return;
  }
  HTTPClient& http = Http::client();
  http.setTimeout(Config::kHttpTimeoutMs);
  http.addHeader("X-Device-Secret", Identity::deviceSecret());
  http.addHeader("Content-Type", "application/json");

  // millis()/1000 wraps back toward 0 alongside millis() itself at ~49.7
  // days of continuous uptime (32-bit overflow), rather than saturating -
  // not worth guarding here. This board reboots on every firmware update
  // already (Loader::requestUpdate()), check-in's own fast path delivers one
  // within a single checkInIntervalMs of it being published, and
  // AppUpdater's independent hourly fallback means an update is never more
  // than about an hour from triggering one even if check-in itself were
  // somehow broken the whole time. A unit actually reaching 49 uninterrupted
  // days without any of that intervening would be a surprise worth its own
  // investigation, not a case this field needs to paper over.
  const uint32_t uptimeSeconds = millis() / 1000UL;
  const int rssi = WiFi.RSSI();
  const uint32_t freeHeap = ESP.getFreeHeap();

  // The two figures that are actually true, sent BESIDE freeHeap rather than
  // instead of it.
  //
  // ESP.getFreeHeap() overstates the memory an allocation can reach by roughly
  // 4x on this board. Measured on device 17 at one instant, while a
  // 10,568-byte allocation was failing:
  //
  //   ESP.getFreeHeap()                        = 49,960
  //   ESP.getMaxAllocHeap()                    = 32,756
  //   heap_caps_get_free_size(8BIT)            = 11,340
  //   heap_caps_get_largest_free_block(8BIT)   =  6,132
  //   heap_caps_get_minimum_free_size          =  5,156
  //   heap_caps_check_integrity_all            = OK
  //
  // All four capability classes (8BIT, INTERNAL|8BIT, DMA, DEFAULT) reported
  // identical figures, so there is no DMA starvation, no memory stranded in
  // word-addressable-only regions, and no corruption - the shortage was simply
  // never being measured. ESP.getMaxAllocHeap() sits pinned at exactly 32,756
  // (0x7FF4) on every device and every boot while freeHeap moves around it,
  // which is the signature of a cap or a region boundary rather than of a
  // largest-free-block measurement; it is not reading the pool malloc draws
  // from. Every judgement this fleet's heap history has ever supported was
  // therefore made from a number about four times too kind.
  //
  // 8BIT specifically because that is the pool a byte buffer comes from - see
  // Display.cpp's ensureFileBufferCapacity(), whose failures are the harm the
  // whole heap story is ultimately about.
  //
  // **Both the free size and the largest block, not just the free size.** The
  // entire reason this went unnoticed for so long is that a free-size figure
  // alone cannot distinguish "plenty of room" from "plenty of room in
  // fragments too small to use" - 11,340 free with a 6,132 largest block is
  // exactly the second case, and a single free-size column on /diag/telemetry
  // would have shown a device in that state as merely low rather than as
  // unable to draw a card at all.
  const uint32_t free8Bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  const uint32_t largestFreeBlock8Bit = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

  // Cleared by App.ino's setup() the moment WiFi first connects (see
  // Identity.h's own remarks on bootAttempts), so this reads 0 for every
  // device healthy enough to ever reach performCheckIn() at all - expected,
  // not a bug in this reporting path. A device stuck retrying before that
  // point never gets far enough to send telemetry in the first place, and
  // one that never reaches steady state keeps counting until CAL's own
  // anti-brick threshold falls back to the factory partition instead.
  const uint8_t bootCount = Identity::bootAttempts();

  // The counter that answers the question bootCount above cannot: incremented
  // once per start in setup() and never cleared, so a device rebooting when it
  // shouldn't be is visible from one report rather than from watching
  // uptimeSeconds fail to climb across several.
  const uint32_t totalBoots = Identity::totalBoots();

  // Sent so the server can say which build a device is actually on. Without it
  // that is only inferable from whether a check-in came back "ok" - which says
  // "matches the current manifest" and never which version, and says nothing at
  // all about a device that has not checked in since a build went live.
  const String firmwareVersion = Identity::installedAppVersion();

  // Storage, reported for the same reason free heap already is: so pressure
  // shows up fleet-wide on /diag/telemetry before it shows up as a device
  // that quietly stopped caching assets. The card is treated as effectively
  // unlimited - it is user-upgradeable - but "unlimited" is only a defensible
  // position while somebody can actually see how full it is. All three read
  // as zero on a device with no card in the slot, which is an ordinary state
  // rather than a fault (see SdStorage.h).
  const uint64_t sdTotalBytes = Sd::totalBytes();
  const uint64_t sdUsedBytes = Sd::usedBytes();
  const uint16_t assetCount = Assets::cachedCount();

  JsonDocument requestDoc;
  requestDoc["uptimeSeconds"] = uptimeSeconds;
  requestDoc["wifiRssiDbm"] = rssi;
  // freeHeapBytes keeps sending ESP.getFreeHeap(), unchanged, despite the block
  // above establishing that it is the wrong number. Two reasons, both
  // deliberate:
  //
  //   1. This project has a standing mandate that the server keeps working
  //      with device firmware up to six months old. A server that has not been
  //      updated yet still reads this field and still stores it in
  //      device_telemetry; if the meaning changed underneath it, that server
  //      would silently start recording a different quantity in a column
  //      labelled for the old one - which is a worse failure than an inflated
  //      number, because it is undetectable from the data. Giving a new
  //      quantity a new field name is backward compatible; redefining an
  //      existing field never is.
  //
  //   2. Keeping both side by side is what makes the discrepancy AUDITABLE
  //      rather than a story told in a commit message. Once /diag/telemetry
  //      shows 49,960 next to 11,340 for the same device at the same instant,
  //      the 4x gap is a fact anyone can see in the fleet's own history, and
  //      the historical rows recorded before this change stay interpretable
  //      because the field they were recorded in still means what it meant.
  //
  // So this field is now best read as "what the Arduino wrapper claims", and
  // free8BitBytes/largestFreeBlock8BitBytes as what is actually available.
  requestDoc["freeHeapBytes"] = freeHeap;
  requestDoc["free8BitBytes"] = free8Bit;
  requestDoc["largestFreeBlock8BitBytes"] = largestFreeBlock8Bit;
  requestDoc["bootCount"] = bootCount;
  requestDoc["lastCheckInOutcome"] = lastCheckInOutcome;
  requestDoc["sdTotalBytes"] = sdTotalBytes;
  requestDoc["sdUsedBytes"] = sdUsedBytes;
  requestDoc["assetCount"] = assetCount;
  requestDoc["totalBoots"] = totalBoots;
  // WHY this device last restarted, beside totalBoots which only says THAT it
  // did. Those two have been a count with no explanation for the life of this
  // fleet: on 2026-09-11, 38 restarts were measured across five hours and 36 of
  // them could not be attributed to anything, because the only place the reason
  // was ever written was the debug stream - which emits it in setup(), before
  // check-in authorises that stream, and which was delivering 1.3% and 7.3% of
  // the lines it produced on the two devices restarting most often. See
  // BootDiag::restartReasonToken() for the full measurement, and for why this
  // rides on every report rather than only the first after a boot.
  //
  // A short token rather than the prose BootDiag logs ("PANIC_RESET+NONE", not
  // "a crash: null dereference, stack canary, or a failed assertion"): the
  // server groups and counts these across a fleet, and the sentence explaining
  // what a panic means belongs once on a diagnostics page, not in every row of
  // a database table.
  requestDoc["restartReason"] = BootDiag::restartReasonToken();
  // The two ADC pins that could carry a battery sense line on this board, as
  // measured, with no divider ratio applied - see PowerProbe.h. Sent as raw pin
  // millivolts precisely because the right interpretation is not yet known:
  // GPIO34 is documented elsewhere as the onboard LDR, GPIO35 is the free
  // input-only ADC1 pin on P3, and published pinouts for this board disagree.
  //
  // These exist to replace batteryPercent/charging above, which have been
  // literal constants (100/true) since this firmware was written. That is a
  // placeholder shaped like a measurement, and this fleet has already been
  // misled twice by that shape - totalBoots counting restarts with no cause,
  // and ESP.getMaxAllocHeap() returning a constant that reads as a real figure.
  //
  // They stay separate from batteryPercent rather than replacing its value
  // in-place: a field whose meaning changes underneath a server that has not
  // been updated is the one failure the firmware-compatibility rule exists to
  // prevent, and giving a new quantity a new name is the cheap way to avoid it.
  // Once a multimeter has confirmed which pin is which and what the divider is,
  // batteryPercent can start carrying a real number and these can go.
  requestDoc["adc34Millivolts"] = PowerProbe::millivoltsGpio34();
  requestDoc["adc35Millivolts"] = PowerProbe::millivoltsGpio35();
  // The two figures that turn the heap ratchet from something somebody has to
  // sit and watch into arithmetic the server does on every report.
  //
  // bootLargestFreeBlockBytes is this boot's high-water mark, captured in
  // setup() before the rotation has drawn anything, and repeated unchanged on
  // every report for the life of the boot - the same redundancy restartReason
  // uses, and for the same reason: the first post-boot report is the one most
  // likely to be lost, because a fragmented device is one that fails at the TLS
  // handshake.
  //
  // Why a SECOND heap field when largestFreeBlock8BitBytes already reports the
  // live one: the live figure alone cannot separate the two conditions that need
  // different answers. A device at 16,372 bytes that booted at 90,100 has a
  // fragmentation problem in its rotation; one that booted at 18,000 has a
  // mount-and-init cost problem and nothing to do with drawing cards at all.
  // Those are indistinguishable in a single sample, and telling them apart by
  // hand meant reading this very log line by line - which is how a fleet-wide
  // ratchet went a week unquantified, and how the same plateau got called
  // wrongly twice in one evening.
  requestDoc["bootLargestFreeBlockBytes"] = BootDiag::bootLargestFreeBlock();

  // ---- Where the contiguous heap actually went, by phase of the rotation ----
  //
  // bootLargestFreeBlockBytes above and largestFreeBlock8BitBytes further up
  // between them say HOW FAR this device has fallen. They have never said WHAT
  // DID IT, and that is now the only open question on this fleet: the asset
  // path was instrumented at eight transitions and ruled out, RGB565 draws were
  // measured five times with the largest block identical either side, and
  // nobody has ever instrumented the ordinary card rotation at all.
  //
  // **These ride telemetry rather than the debug stream on purpose, and it is
  // the whole design.** The stream is itself the dominant consumer (200 String
  // slots, a 16KB ceiling, a JsonDocument and a body String per batch), device
  // 19 has never streamed and has never decayed, and every stream on the fleet
  // is off right now deliberately so that real-world reboots can be measured.
  // An answer that only existed in the stream would describe a device in a
  // state nobody is trying to fix. Telemetry is a small fixed POST that still
  // gets through when the stream does not - the same argument that put
  // bootLargestFreeBlockBytes and sdMountCostBytes here in the first place.
  //
  // **Signed, and they sum.** Each field is the NET contiguous heap that phase
  // took and did not give back since boot. Positive is a phase that ratchets;
  // negative is one the free list coalesced under; near-zero on a phase that
  // allocates megabytes over an uptime is that phase being exonerated, which no
  // gross-allocation counter could ever say. By construction they sum to
  // (bootLargestFreeBlockBytes - largestFreeBlock8BitBytes), give or take what
  // this very function has allocated between the Scope above and the reading a
  // few lines up. **The server should check that sum**, because if it does not
  // hold, this instrument is wrong - a phase is missing a Scope, or the carving
  // happens somewhere all five buckets miss - and an instrument that can be
  // caught being wrong from its own output is the only kind worth adding to an
  // investigation that has already been sent down two blind alleys by figures
  // that looked authoritative (ESP.getMaxAllocHeap()'s constant 32,756, and
  // "roughly 32KB" for a floor that is really 16,717 twice over).
  //
  // heapPhaseIdleBytes is not the leftovers column. It is the WiFi and LWIP
  // stacks, touch, the timers, the whole of setup() before the rotation starts
  // - and anything this instrumentation forgot. A device up for half an hour
  // with its decay sitting in Idle has ruled out drawing, fetching, check-in
  // and the housekeeping POSTs in a single figure, which is the most useful
  // thing this could possibly report and the one result a set of buckets that
  // only covered the suspects could never produce.
  requestDoc["heapPhaseDrawBytes"] = HeapRatchet::netBytes(HeapRatchet::Phase::Draw);
  requestDoc["heapPhaseFetchBytes"] = HeapRatchet::netBytes(HeapRatchet::Phase::Fetch);
  requestDoc["heapPhaseCheckInBytes"] = HeapRatchet::netBytes(HeapRatchet::Phase::CheckIn);
  requestDoc["heapPhaseServiceBytes"] = HeapRatchet::netBytes(HeapRatchet::Phase::Service);
  requestDoc["heapPhaseIdleBytes"] = HeapRatchet::netBytes(HeapRatchet::Phase::Idle);
  // How many discrete downward steps made up all of that. Sent so the "steps
  // are multiples of 2,048" claim - made by reading a debug stream line by line
  // on one device - becomes arithmetic the server can do on a whole fleet with
  // every stream off. Total decay divided by this is the mean step, and if that
  // lands on 2,048 across many devices the quantum is real; if it lands on 40
  // the decay is small-String churn and the large-block hypotheses are dead.
  requestDoc["heapRatchetSteps"] = HeapRatchet::stepCount();
  // The biggest single carve, and which phase took it. One 20KB step and fifty
  // 400-byte ones decay a heap at the same rate and want completely different
  // fixes; the buckets alone cannot tell those apart, and this is the cheapest
  // field that can. The card id that was on screen at the time is deliberately
  // NOT sent - it only means anything once the phase is already narrowed down,
  // and it is on the [ratchet] log line for whoever turns a stream on next.
  requestDoc["heapWorstStepBytes"] = HeapRatchet::worstStepBytes();
  // A short literal ("draw"/"fetch"/"checkin"/"service"/"idle"/"none"), never a
  // String - ArduinoJson links a const char* rather than copying it, so this
  // field costs one slot and no string storage. Same reasoning as
  // restartReason's token above: the server groups these across a fleet, and
  // prose belongs once on a diagnostics page rather than in every row.
  requestDoc["heapWorstStepPhase"] = HeapRatchet::worstStepPhaseName();
  // -1 from Sd::mountCostBytes() means "there was no card to mount", which is
  // NOT the same as a mount that cost nothing. Omitted rather than sent, so the
  // server's own "this firmware does not report it" null carries the
  // distinction instead of a negative byte count appearing on the wire.
  const int32_t mountCost = Sd::mountCostBytes();
  if (mountCost >= 0) {
    requestDoc["sdMountCostBytes"] = mountCost;
  }
  // Omitted rather than sent empty when nothing is installed yet: the server
  // treats a missing field as "this firmware does not report it", and an empty
  // string would be stored as a real answer that happens to say nothing.
  if (firmwareVersion.length() > 0) {
    requestDoc["firmwareVersion"] = firmwareVersion;
  }

  String body;
  serializeJson(requestDoc, body);

  // Found via grep alongside the other HTTPClient call sites this session
  // instrumented (CheckIn.cpp, AppUpdater.cpp, Assets.cpp, the provider
  // cards) - not one of those named up front, but it is exactly the same
  // shape of server exchange the rest of this pass covers.
  Log::verbose("[telemetry] POST %s body=%s", url.c_str(), body.c_str());

  const int status = http.POST(body);
  Log::verbose("[telemetry] response status=%d", status);
  http.end();

  // Any HTTP 200 is success, same as CheckIn's own handling of its response
  // - the body (serverUtcTimestamp/acknowledged) has nothing this firmware
  // acts on, so it is not even parsed here.
  if (status != 200) {
    Log::printf("[telemetry] failed, http status=%d", status);
    return;
  }

  // freeHeap and free8Bit are both printed, in that order, for the same reason
  // both are sent: a log line that quoted only one of them would be the thing
  // that hid this for months all over again. Whoever reads this line should see
  // the gap without having to know it exists.
  Log::printf(
      "[telemetry] ok (uptimeSeconds=%lu rssi=%d freeHeap=%lu free8BIT=%lu largest8BIT=%lu "
      "bootCount=%u totalBoots=%lu restart=%s "
      "version=%s outcome=%s sdUsedMB=%lu sdTotalMB=%lu assets=%u)",
      static_cast<unsigned long>(uptimeSeconds), rssi, static_cast<unsigned long>(freeHeap),
      static_cast<unsigned long>(free8Bit), static_cast<unsigned long>(largestFreeBlock8Bit),
      bootCount, static_cast<unsigned long>(totalBoots), BootDiag::restartReasonToken(),
      firmwareVersion.length() > 0 ? firmwareVersion.c_str() : "(none)", lastCheckInOutcome,
      static_cast<unsigned long>(sdUsedBytes / (1024ULL * 1024ULL)),
      static_cast<unsigned long>(sdTotalBytes / (1024ULL * 1024ULL)), assetCount);

  // A SECOND line rather than more fields on the one above, because Log::printf
  // formats into a fixed 256-byte scratch and truncates past it - the line
  // above is already most of the way there, and a summary that silently lost
  // its last two buckets to "...(truncated)" would be worse than no summary.
  //
  // Not the deliverable - the eight fields on the POST above are - but a device
  // on a bench cable or a stream turned on for ten minutes should not have to
  // wait for a server round trip to see where its heap is going. The same five
  // figures, in the same order, so a serial capture and a telemetry row can be
  // read against each other without a translation step.
  Log::printf("[ratchet] since boot: draw=%d fetch=%d checkin=%d service=%d idle=%d over %u steps, "
              "worst -%u in %s (%s)",
              static_cast<int>(HeapRatchet::netBytes(HeapRatchet::Phase::Draw)),
              static_cast<int>(HeapRatchet::netBytes(HeapRatchet::Phase::Fetch)),
              static_cast<int>(HeapRatchet::netBytes(HeapRatchet::Phase::CheckIn)),
              static_cast<int>(HeapRatchet::netBytes(HeapRatchet::Phase::Service)),
              static_cast<int>(HeapRatchet::netBytes(HeapRatchet::Phase::Idle)),
              static_cast<unsigned>(HeapRatchet::stepCount()),
              static_cast<unsigned>(HeapRatchet::worstStepBytes()),
              HeapRatchet::worstStepPhaseName(), HeapRatchet::worstStepSubject());
}

}  // namespace Telemetry
