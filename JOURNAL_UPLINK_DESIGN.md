# Getting CAL's journal off the device

## The gap, as it actually is

CAL records every boot to the `callog` partition: sixteen 4,096-byte sectors at `0x3B0000`,
one boot per sector, oldest overwritten. Raw `esp_partition_*` writes, no filesystem, no
heap, usable on the first instruction of `setup()`. `Journal.h` sets all of this out.

So the record exists. What does not exist is any way to read it without a cable. Every read
entry point prints to Serial:

- `dumpLastBoot()` - the previous boot, printed at startup
- `dumpAll()` - every retained boot, behind the `d` keypress
- `poll()` - reads one serial character and acts on it

None return bytes. Nothing in the App calls any of them, and nothing could: CAL and App are
separate sketches with no shared translation unit. The App's only contact with the partition
is `Telemetry.cpp` asking `esp_partition_find_first` whether it exists at all, so it can
report `callog=1` - the one-bit answer to "has this device been re-tabled".

**What that cost on 2026-10-11.** Device 17 was told to install a build it could not fetch
and rebooted nine times. Each cycle wrote a sector naming what failed, including
`[heap] at the failed download request: free=... largest8BitBlock=...` - which is the exact
figure `Updater.cpp` says the fragmentation hypothesis "stands or falls on". Nine sectors of
answer, on the device, unreachable. The diagnosis stalled on "we cannot tell a slow download
from a stuck one" while the device had already written down which it was.

## What to build

**An App-side reader, not a CAL-side sender.** CAL has no network of its own, no check-in,
and no business growing one: it exists to be small and to work when everything else does
not. The App has the network, the debug stream and a check-in to ride on. The journal format
is stable and documented, so the App can parse it directly.

**Chunked, never a whole sector in RAM.** A 4,096-byte buffer is a large contiguous
allocation on a board whose largest free block has been measured at 3,444 bytes. Read
256 bytes at a time into a fixed `.bss` buffer, split on newlines, emit each line. Zero heap
and no allocation that can fail.

**Emitted through `Log::line()`, not a new endpoint.** Those lines then ride the existing
debug stream, land in `device_debug_log_entries`, and are queryable beside everything else.
No server change at all - which also means this ships without a deploy.

**Once per boot, and only what is new.** Each sector header carries
`CALJRNL1 boot=%010lu`, so the App can record the highest sequence it has already sent in
NVS and emit only sectors above it. A device that reboots nine times then reaches a
check-in sends nine sectors once, not the same sector nine times.

## What it must not do

- **Never block the boot.** Reading sixteen sectors is 64KB of flash reads. It happens after
  the first successful check-in, on the ordinary `loop()` path, a sector at a time across
  several iterations - not in `setup()`, and never where a card draw is waiting.
- **Never write.** The App opens that partition read-only. CAL owns it, the App is a reader,
  and a cursor bug on the App side must not be able to corrupt the record it is reading.
- **Never retry.** A read error disables the uplink for that boot and says so once, which is
  the rule `Journal` already holds itself to.
- **Say when it is truncated.** The ring holds sixteen boots. A device that looped more than
  that has lost the earliest, and the reader should say which sequence numbers it found
  rather than implying it found everything.

## What this is not

It is not a replacement for the serial path. Somebody with a cable must keep getting
`dumpAll()`, because the case this is written for - a device that cannot reach the
server - is exactly the case where the uplink cannot run either. This closes the gap for a
device that eventually recovers, which is most of them. A device that never recovers still
needs the cable, and `Journal::poll()` exists for the operator who attaches one after the
failure.

## Cost

A 256-byte `.bss` read buffer, a four-byte NVS key for the last sequence sent, and a parser.
Call it 150 lines in the App and nothing at all in CAL, which is the part that matters: the
module that has to work when the heap is broken does not change.

## Still open

Whether to send on every check-in that enables streaming, or only once per boot. Sending
repeatedly would let an admin turn the stream on days later and still get the boot that
failed, at the cost of re-sending sectors already in the database. The database can
deduplicate on sequence number, so this is a bandwidth question rather than a correctness
one, and it should be settled by somebody who knows what the fleet's data budget looks like.
