#include "Journal.h"

#include <esp_partition.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace Journal {
namespace {

/// Looked up by NAME rather than by subtype, so the custom subtype value in
/// partitions.csv is not load-bearing and a device with an older table simply
/// reports "not found" instead of matching something else by accident.
constexpr const char* kPartitionLabel = "callog";

/// The flash erase unit on this part. Every size below is a consequence of it.
constexpr size_t kSectorBytes = 4096;

/// Exactly 32 bytes: "CALJRNL1 boot=" (14) + ten digits (24) + spaces + '\n'.
/// Fixed width so the scan in begin() can read one header per sector without
/// parsing anything variable-length, and 4-byte aligned like every other write.
constexpr size_t kHeaderBytes = 32;
constexpr const char kMagic[] = "CALJRNL1";
constexpr size_t kMagicLen = sizeof(kMagic) - 1;  // 8
constexpr size_t kSeqDigits = 10;                 // holds UINT32_MAX
constexpr size_t kSeqOffset = kMagicLen + 1 + 5;  // past "CALJRNL1 boot="

/// Bounds the header scan so a mis-sized partition cannot turn begin() into a
/// long loop. Sixteen is the real figure for the 64KB partition; the cap only
/// matters if somebody grows it.
constexpr uint8_t kMaxSectors = 64;

/// One log line. Long enough for a URL or an SSID plus context, short enough
/// that two of these buffers are a rounding error against SRAM.
constexpr size_t kMaxLine = 160;

/// The line, its padding to a 4-byte write boundary, and its newline.
constexpr size_t kRecordBytes = kMaxLine + 8;

constexpr size_t kReadChunk = 256;

/// Written once when a boot runs out of sector, and the reason writeRecord()
/// stops at kUsableBytes rather than at kSectorBytes: the space for this marker
/// is reserved up front so it is always possible to say that the record is
/// incomplete. Without the reservation the log would simply stop mid-boot and
/// read as though CAL had stopped there too, which is the worse failure.
constexpr char kFullMarker[] =
    "[journal] sector full - the rest of this boot is on serial only";
constexpr size_t kMarkerLen = sizeof(kFullMarker) - 1;
constexpr size_t kMarkerTotal = ((kMarkerLen + 1) + 3) & ~static_cast<size_t>(3);
constexpr size_t kUsableBytes = kSectorBytes - kMarkerTotal;

const esp_partition_t* gPart = nullptr;
bool gEnabled = false;
uint8_t gSectorCount = 0;
uint8_t gSector = 0;
uint32_t gBootSeq = 0;
size_t gCursor = 0;
bool gSectorFull = false;
uint16_t gDropped = 0;

// Static rather than stack: these are touched from inside deep call chains
// (the download loop, the TLS paths) where stack is the scarcer resource, and
// static also guarantees they are in DRAM, which esp_partition_write requires
// of its source buffer.
char gScratch[kMaxLine];
char gRecord[kRecordBytes];
uint8_t gReadBuf[kReadChunk];

/// Turns the journal off for the rest of this boot and says why, exactly once.
/// Deliberately not a retry: a flash subsystem that just refused a write is not
/// going to be talked round, and a logger that loops on failure is a logger
/// that hangs the boot it was supposed to explain.
void disableAfterError(const char* what, esp_err_t err) {
  gEnabled = false;
  Serial.printf("[journal] %s failed (esp_err %d) - journal is serial-only for the rest of this boot\n",
                what, static_cast<int>(err));
}

/// Reads one sector's header and, if it is one of ours, its sequence number.
/// Anything unrecognised - erased flash, a sector we never wrote, a partition
/// that used to hold something else - is simply "not a record", never an error.
bool readHeader(uint8_t index, uint32_t* seqOut) {
  uint8_t hdr[kHeaderBytes];
  if (esp_partition_read(gPart, static_cast<size_t>(index) * kSectorBytes, hdr,
                         kHeaderBytes) != ESP_OK) {
    return false;
  }
  if (memcmp(hdr, kMagic, kMagicLen) != 0) {
    return false;
  }
  if (memcmp(hdr + kMagicLen + 1, "boot=", 5) != 0) {
    return false;
  }
  uint32_t seq = 0;
  for (size_t i = 0; i < kSeqDigits; ++i) {
    const char c = static_cast<char>(hdr[kSeqOffset + i]);
    if (c < '0' || c > '9') {
      return false;
    }
    seq = seq * 10 + static_cast<uint32_t>(c - '0');
  }
  *seqOut = seq;
  return true;
}

/// Appends a line with no capacity check. Only ever called with a length the
/// caller has already established fits, which is what lets the full-sector
/// marker be written from inside the branch that discovered there was no room.
void appendRaw(const char* text, size_t len) {
  const size_t pad = (4 - ((len + 1) % 4)) % 4;
  const size_t total = len + pad + 1;

  memcpy(gRecord, text, len);
  // Padding goes BEFORE the newline, so it is invisible when the sector is read
  // as text - trailing spaces rather than stray leading ones on the next line.
  memset(gRecord + len, ' ', pad);
  gRecord[len + pad] = '\n';

  const esp_err_t err = esp_partition_write(
      gPart, static_cast<size_t>(gSector) * kSectorBytes + gCursor, gRecord, total);
  if (err != ESP_OK) {
    disableAfterError("esp_partition_write", err);
    return;
  }
  gCursor += total;
}

void writeRecord(const char* text) {
  if (!gEnabled) {
    return;
  }

  size_t len = strlen(text);
  if (len > kMaxLine - 1) {
    len = kMaxLine - 1;
  }
  // The caller's convention is no trailing newline; strip one anyway rather
  // than writing a blank line into the sector if somebody forgets.
  while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
    --len;
  }
  if (len == 0) {
    return;
  }

  const size_t total = len + ((4 - ((len + 1) % 4)) % 4) + 1;
  if (gCursor + total > kUsableBytes) {
    if (!gSectorFull) {
      gSectorFull = true;
      appendRaw(kFullMarker, kMarkerLen);
    }
    ++gDropped;
    return;
  }

  appendRaw(text, len);
}

/// Streams one sector to Serial, stopping at the first unwritten byte. Records
/// are ASCII, so 0xFF - what erased flash reads as - is unambiguous as the end
/// of what was written.
void dumpSector(uint8_t index) {
  const size_t base = static_cast<size_t>(index) * kSectorBytes;
  size_t off = 0;
  while (off < kSectorBytes) {
    size_t n = kSectorBytes - off;
    if (n > kReadChunk) {
      n = kReadChunk;
    }
    if (esp_partition_read(gPart, base + off, gReadBuf, n) != ESP_OK) {
      Serial.println("[journal] read failed partway through this sector");
      return;
    }
    // Written a chunk at a time rather than a byte at a time: a full sector is
    // 4,096 Serial.write() calls otherwise, and this runs inside the two-second
    // splash budget.
    size_t usable = n;
    for (size_t i = 0; i < n; ++i) {
      if (gReadBuf[i] == 0xFF) {
        usable = i;
        break;
      }
    }
    if (usable > 0) {
      Serial.write(gReadBuf, usable);
    }
    if (usable < n) {
      return;
    }
    off += n;
  }
}

}  // namespace

void begin() {
  gEnabled = false;
  gPart = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                   ESP_PARTITION_SUBTYPE_ANY, kPartitionLabel);
  if (gPart == nullptr) {
    // Not a fault. A device flashed before the callog partition existed lands
    // here and boots exactly as it always did, with serial output only.
    Serial.println("[journal] no 'callog' partition in this device's table - serial only, "
                   "nothing from this boot will survive it");
    return;
  }

  if (gPart->size < 2 * kSectorBytes || (gPart->size % kSectorBytes) != 0) {
    Serial.printf("[journal] 'callog' is %u bytes, which is not two or more whole %u-byte "
                  "sectors - serial only\n",
                  static_cast<unsigned>(gPart->size), static_cast<unsigned>(kSectorBytes));
    gPart = nullptr;
    return;
  }

  const size_t sectors = gPart->size / kSectorBytes;
  gSectorCount = sectors > kMaxSectors ? kMaxSectors : static_cast<uint8_t>(sectors);

  // The highest sequence number anywhere in the ring is the last boot recorded,
  // wherever it physically sits. Sixteen 32-byte reads.
  int16_t newest = -1;
  uint32_t newestSeq = 0;
  for (uint8_t i = 0; i < gSectorCount; ++i) {
    uint32_t seq = 0;
    if (!readHeader(i, &seq)) {
      continue;
    }
    if (newest < 0 || seq > newestSeq) {
      newest = static_cast<int16_t>(i);
      newestSeq = seq;
    }
  }

  if (newest < 0) {
    // A blank or foreign partition. Start at the beginning; sequence numbers
    // begin at 1 so that 0 is never a valid record and cannot be confused with
    // erased flash that happened to parse.
    gSector = 0;
    gBootSeq = 1;
  } else {
    gSector = static_cast<uint8_t>((newest + 1) % gSectorCount);
    gBootSeq = newestSeq + 1;
  }

  const size_t base = static_cast<size_t>(gSector) * kSectorBytes;
  esp_err_t err = esp_partition_erase_range(gPart, base, kSectorBytes);
  if (err != ESP_OK) {
    disableAfterError("esp_partition_erase_range", err);
    return;
  }

  const int written = snprintf(gRecord, kRecordBytes, "%s boot=%010lu", kMagic,
                               static_cast<unsigned long>(gBootSeq));
  if (written < 0 || static_cast<size_t>(written) >= kHeaderBytes) {
    // Cannot happen with the fixed widths above, but the header is the one
    // thing the scan depends on, so a malformed one disables rather than ships.
    Serial.println("[journal] header did not format to its fixed width - serial only");
    return;
  }
  memset(gRecord + written, ' ', kHeaderBytes - written - 1);
  gRecord[kHeaderBytes - 1] = '\n';

  err = esp_partition_write(gPart, base, gRecord, kHeaderBytes);
  if (err != ESP_OK) {
    disableAfterError("esp_partition_write of the sector header", err);
    return;
  }

  gCursor = kHeaderBytes;
  gSectorFull = false;
  gDropped = 0;
  gEnabled = true;
}

void dumpLastBoot() {
  if (gPart == nullptr) {
    return;
  }

  int16_t pick = -1;
  uint32_t pickSeq = 0;
  for (uint8_t i = 0; i < gSectorCount; ++i) {
    // Skip the sector this boot has just claimed - it holds nothing but its own
    // header, and the point of this dump is the boot BEFORE this one.
    if (gEnabled && i == gSector) {
      continue;
    }
    uint32_t seq = 0;
    if (!readHeader(i, &seq)) {
      continue;
    }
    if (pick < 0 || seq > pickSeq) {
      pick = static_cast<int16_t>(i);
      pickSeq = seq;
    }
  }

  if (pick < 0) {
    Serial.println("[journal] no previous boot on record - this is the first boot to keep one");
    return;
  }

  Serial.printf("[journal] ---- previous boot %lu (sector %u) - press 'd' for all %u ----\n",
                static_cast<unsigned long>(pickSeq), static_cast<unsigned>(pick),
                static_cast<unsigned>(gSectorCount));
  dumpSector(static_cast<uint8_t>(pick));
  Serial.println("[journal] ---- end of previous boot ----");
}

void dumpAll() {
  if (gPart == nullptr) {
    Serial.println("[journal] no 'callog' partition - there is nothing to dump");
    return;
  }

  Serial.println("[journal] ======== journal dump ========");

  // Selection by ascending sequence rather than by sector order: the ring wraps,
  // so physical order is not time order. At sixteen sectors the quadratic scan
  // costs nothing and needs no array to sort.
  uint32_t last = 0;
  uint8_t printed = 0;
  while (true) {
    int16_t pick = -1;
    uint32_t pickSeq = 0;
    for (uint8_t i = 0; i < gSectorCount; ++i) {
      uint32_t seq = 0;
      if (!readHeader(i, &seq)) {
        continue;
      }
      if (seq <= last) {
        continue;
      }
      if (pick < 0 || seq < pickSeq) {
        pick = static_cast<int16_t>(i);
        pickSeq = seq;
      }
    }
    if (pick < 0) {
      break;
    }

    const bool current = gEnabled && static_cast<uint8_t>(pick) == gSector;
    Serial.printf("[journal] ---- boot %lu (sector %u)%s ----\n",
                  static_cast<unsigned long>(pickSeq), static_cast<unsigned>(pick),
                  current ? " - this boot, still running" : "");
    dumpSector(static_cast<uint8_t>(pick));
    last = pickSeq;
    ++printed;
  }

  if (printed == 0) {
    Serial.println("[journal] the partition holds no boot records yet");
  }
  if (gDropped > 0) {
    Serial.printf("[journal] %u lines of this boot were dropped after its sector filled\n",
                  static_cast<unsigned>(gDropped));
  }
  Serial.println("[journal] ======== end of dump ========");
}

void poll() {
  while (Serial.available() > 0) {
    const int c = Serial.read();
    if (c == 'd' || c == 'D') {
      dumpAll();
    } else if (c == '?' || c == 'h' || c == 'H') {
      Serial.println("[journal] d = dump every retained boot, ? = this line");
    }
    // Everything else - newlines, stray bytes from a monitor connecting - is
    // ignored without comment. A console that answers back to noise is worse
    // than one that does not.
  }
}

void printf(const char* format, ...) {
  va_list args;
  va_start(args, format);
  const int n = vsnprintf(gScratch, sizeof(gScratch), format, args);
  va_end(args);

  if (n < 0) {
    line("[journal] a log line failed to format");
    return;
  }
  if (static_cast<size_t>(n) >= sizeof(gScratch)) {
    // Kept and marked, not discarded: a truncated line still names the stage it
    // came from, which is the half that matters.
    memcpy(gScratch + sizeof(gScratch) - 15, "...(truncated)", 14);
    gScratch[sizeof(gScratch) - 1] = '\0';
  }
  line(gScratch);
}

void line(const char* text) {
  if (text == nullptr) {
    return;
  }
  // Serial FIRST, unconditionally, before any flash work and regardless of
  // whether the journal is enabled. Nothing about having a flash journal may
  // cost somebody with a cable what they could already see.
  Serial.println(text);
  writeRecord(text);
}

bool persistent() { return gEnabled; }

}  // namespace Journal
