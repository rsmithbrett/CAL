#pragma once

#include <Arduino.h>

/// CAL's boot journal: what happened, written where a brick cannot take it.
///
/// **The problem this exists for.** CAL is the one component that can leave a
/// device unbootable, and it was the one component that reported nothing. When
/// a unit is found showing `Update failed`, the App's remote debug stream is no
/// help - that stream belongs to the App, and the App is precisely what CAL
/// failed to reach. RAM is gone by the time anyone looks. So the record has to
/// be on flash, and it has to be readable afterwards by somebody holding a USB
/// cable. See the README's "CAL can say why now" for the full argument,
/// including the four storage options rejected before this one.
///
/// **Where it goes.** The `callog` partition (`partitions.csv`, 65,536 bytes at
/// `0x3B0000`), written as raw flash through `esp_partition_*`. No filesystem,
/// no mount, no heap, no locks - so it is usable on the first instruction of
/// `setup()`, before the display, before NVS, before WiFi, and on a device
/// whose heap is the thing that is broken.
///
/// **The shape.** Sixteen 4,096-byte sectors, one boot per sector, oldest
/// overwritten. Each sector opens with a 32-byte `CALJRNL1 boot=%010lu` header
/// whose sequence number is what orders the ring; the remaining 4,064 bytes are
/// that boot's lines, less 64 held back for the "sector full" marker, so 4,000
/// bytes of ordinary logging. A boot therefore cannot write past its own
/// sector, which is the per-boot budget and the reason one pathological boot
/// cannot evict the history.
///
/// **What it must never do.** A logger that can brick the device is worse than
/// no logger, so every entry point here returns `void`, nothing allocates,
/// nothing retries, and no caller branches on any of it:
///
/// - Serial is written first and unconditionally on every call, before any
///   flash work and whether or not the journal is enabled. Somebody with a
///   cable must never lose what they had before this module existed.
/// - A missing, undersized or unusable `callog` partition is a normal state,
///   not an error: the journal disables itself, says so once on Serial, and
///   every later call is a Serial-only no-op. That is also what a device still
///   running the older partition table does, so this firmware does not need a
///   flag day.
/// - The first `esp_partition_*` error of the boot disables the journal for the
///   rest of that boot. One Serial line, no retry, no escalation.
/// - `esp_partition_write` bounds-checks against the partition handle, so a
///   cursor bug here physically cannot reach NVS, `factory` or `ota_0`.
/// - Called only from `setup()` and `loop()` on the Arduino task. Never from an
///   ISR, never from a second task, and nothing it calls logs - so there is no
///   path back into it.
namespace Journal {

/// Finds the partition, works out which sector is next, erases it and writes
/// its header. Safe to call before anything else is initialised; requires only
/// that `Serial.begin()` has run so its own status line has somewhere to go.
void begin();

/// Prints the most recent COMPLETED boot - the one that failed - to Serial.
///
/// Called immediately after `begin()` and deliberately before
/// `Display::begin()`, so that a hang inside `lcd.init()` or a LittleFS format
/// still yields the previous boot's whole record. One sector is at most 4,096
/// bytes, which at 115200 8N1 (11,520 bytes a second) is 356 ms worst case and
/// nearer 180 ms for a typical boot - chosen against the boot ladder's
/// requirement that something appear on the panel within about two seconds.
/// Dumping all sixteen sectors here would be up to 5.7 seconds and would break
/// that outright, which is why the rest is behind a keypress.
void dumpLastBoot();

/// Prints every retained boot, oldest first, to Serial. This is what `d` on the
/// serial console does.
void dumpAll();

/// Reads one character of serial input and acts on it: `d` dumps everything,
/// `?` prints the key, anything else is ignored silently.
///
/// Call it from wherever CAL sits still - the `haltWithFailure()` screen, the
/// enrollment wait, and `loop()`. It exists for the operator who attaches a
/// cable AFTER the failure: without it the only way to read the journal is to
/// power-cycle, which both consumes a ring slot and may disturb the state being
/// examined.
void poll();

/// printf-style, matching the `[stage] what happened` convention the call sites
/// already use. No trailing newline is expected - one is always added. Output
/// longer than the fixed 160-byte scratch buffer is kept and marked
/// `...(truncated)` rather than cut off mid-word or grown on the heap.
void printf(const char* format, ...) __attribute__((format(printf, 1, 2)));

/// One already-formatted line. Takes `const char*`, not `String`, on purpose:
/// binding a literal to a `String` parameter heap-allocates, and this is the
/// module that has to work when the heap is the problem.
void line(const char* text);

/// Whether this boot is actually being written to flash, as opposed to Serial
/// only. False on a device whose partition table predates `callog`, and after
/// any flash error. Reported in the boot banner so a reader is never misled
/// about whether what they are watching will still exist afterwards.
bool persistent();

}  // namespace Journal
