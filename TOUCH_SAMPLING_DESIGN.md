# Touch sampling

## The problem, measured

`loop()` samples touch and also does everything else: card draws, check-in,
telemetry, provider fetches, the heap diagnostics. Touch detection is
rising-edge, so a tap that starts and ends while `loop()` is inside something
slow is not delayed, it's gone.

How often that happens, from the fleet's own logs over one hour on 2026-10-09
(the firmware logs any iteration over 250ms; only 2 log lines were dropped in
that hour, so this is close to a full census rather than a sample):

| Display | Stalls | Median | Worst | Share of wall clock |
|---|---|---|---|---|
| Brett Test | 254 | 350 ms | 5.7 s | 3.2% |
| New Jim test | 220 | 496 ms | 13.9 s | 4.7% |
| Test2 | 178 | 470 ms | 5.2 s | 3.8% |
| Workstation | 171 | 452 ms | 5.9 s | 2.8% |

A blind window opens every 14 to 21 seconds. Grouped by the log line
immediately before the stall, 121 of the 816 came after check-in or the
telemetry ratchet and averaged 1.5 to 2.1 seconds; 352 came after a card draw
at around 450ms; 132 came after the heap diagnostics, which means the
instrumentation contributes measurably to the thing it was added to measure.

Sampling every 5ms across the 50ms pacing window, which is what the firmware
does today, fixed the gaps *between* operations. It can't do anything about
the gaps *inside* one.

## Why the touch controller, and not the network, moves

The obvious reading is that the network is the problem, so the network should
move to its own task. The hardware says otherwise.

This panel comes up through `LGFX_AUTODETECT`, and LovyanGFX's board table for
it (`lgfx/v1_autodetect/LGFX_AutoDetect_ESP32_all.hpp`, the
`_detector_Sunton_ESP32_2432S028_t` setup) gives the XPT2046 four pins of its
own and a software SPI bus:

```
cfg.spi_host = -1;          // software SPI
cfg.pin_sclk = GPIO_NUM_25;
cfg.pin_mosi = GPIO_NUM_32;
cfg.pin_miso = GPIO_NUM_39;
cfg.pin_cs   = GPIO_NUM_33;
cfg.bus_shared = false;
cfg.pin_int    = -1;        // no PENIRQ; the driver polls
```

`bus_shared = false` is the load-bearing line. `Panel_Device::getTouchRaw()`
only wraps the read in `endTransaction()`/`beginTransaction()` when
`bus_shared` is true, so on this board a touch read never touches the display's
bus. It bit-bangs four dedicated pins and reads panel geometry that nothing
mutates after setup.

So a task that does nothing but sample touch contends with no other hardware.
A task that does the network shares the heap, the TLS client, CardManager's
content state, the display (a finished fetch repaints), HeapRatchet and the
Log ring: six pieces of state that currently work because there is exactly one
thread. Moving touch gets the same outcome and puts one queue between the two
threads instead of six mutexes.

## The split

**Touch task.** Pinned, 4KB stack, priority just above the Arduino loop task.
It samples `Display::readTouchRaw()` every 10ms forever, does the rising-edge
detection, and posts `{x, y, millis()}` to a queue. That's all. It does not
classify the tap, does not call `Motion`, does not draw, and does not log,
because the Log ring is drained by `loop()` and isn't safe to write from two
threads.

**`loop()`.** `CardManager::pollTouch()` drains the queue instead of reading
the hardware. Zone classification stays here, which is the point of putting
the raw coordinate on the queue rather than a classified tap: `Touch::gZones`
is written by the draw path on this thread, so reading it on this thread needs
no protection at all.

Shared state is one FreeRTOS queue of 4 entries. Nothing else.

## Taps that arrive late

A queued tap can be drained seconds after the finger left the glass, and
delivering it then is its own kind of wrong: the card advances with nobody
touching it.

Two rules, both in the drain:

- **A tap older than 1500ms is discarded and counted.** Below that, delivery
  feels like a response; above it, it feels like a ghost. 95% of today's
  losses are stalls under a second, so almost all of them become presses that
  land.
- **Several queued taps collapse to the first, and the count is logged.** More
  than one tap in the queue means the person tapped again because the first
  looked lost. Delivering both would advance two cards. Two deliberate taps
  with the loop running normally land on separate iterations 50ms apart and
  never meet in the queue, so this only ever collapses the repeat-because-it-
  seemed-dead case.

Queue overflow drops the newest and is counted the same way. A full queue
means four taps inside one stall, which is the same situation.

## What this does not fix

Card advance still waits for `loop()` to come back round. A tap during a
5-second fetch is *recorded* in 10ms and *acted on* when the fetch finishes.
The screen won't change until then. If that still reads as slow once this is
in, the next change is the network one, and by then there'll be evidence about
which of its six shared pieces actually matters.

## This was built, shipped, and taken back out

Everything above is the design as written. It ran as v2026.10.09.0012 through
.0014 on 2026-10-09 and was rolled back to v2026.10.09.0011-touchfix the same
afternoon. **Do not put it back without reading this section.**

What happened: it works on one display out of four. Workstation ran it for
twenty minutes with a largest contiguous block of 53,236 bytes and no restarts.
Brett Test, New Jim test and Test2 all fell into a restart loop within about
fifteen minutes of taking it.

The loop, from their telemetry:

- Each boots with `BootLargestFreeBlockBytes` of 110,580, so there is nothing
  wrong at startup.
- At 13 to 42 seconds of uptime the largest contiguous block is 3,000 to 6,600
  bytes, well under the 16,717 that `Http::kTlsRecordBufferBytes` needs.
- `canOpenNewSession()` therefore refuses, check-in cannot open a session, five
  consecutive failures trip the unreachable watchdog, and the device restarts
  back into the same window. `RestartReason` reads `SOFTWARE_RESET+UNREACHABLE`
  on all three.

Why: measured on all four, before and after the rollback.

| Display | Largest block without the task | With it | Lost |
|---|---|---|---|
| Brett Test | 36,852 | 5,876 | ~31,000 |
| New Jim test | 36,852 | 3,060 | ~33,800 |
| Test2 | 42,996 | 3,316 | ~39,700 |
| Workstation | 86,004 | 51,188 | ~34,800 |

The cost is the same on every display, 30 to 35KB, and it is paid out of the
**largest contiguous block** rather than out of total free heap, which barely
moves. Workstation starts with 86KB and can afford it. The other three start at
37 to 43KB against a 16,717 gate and land underneath.

A 4KB task stack does not consume 30KB. It splits a block. `xTaskCreate`
allocates the stack and the TCB from the heap, and a 4KB allocation landing
inside the one large free region leaves two smaller regions where there was
one. The loss is the half that ends up on the wrong side of the split, which is
why it is roughly the same size everywhere and unrelated to the 4KB asked for.

So the fix, when this is tried again, is to stop the allocation touching that
region at all: `xTaskCreateStaticPinnedToCore` with the stack as a BSS array.
That costs 4KB of RAM permanently and nothing in fragmentation, which is the
trade the fleet's heap budget already makes everywhere else - the log ring is
fixed slots for the same reason.

Still unexplained, and worth knowing independently of this: Workstation holds
roughly twice the free heap of the other three on the identical build. That is
a standing difference, not something the task caused, and anything else that
needs a large contiguous block will meet it too.

A procedural note worth as much as the technical one: this was piloted on
Workstation, confirmed by hand, and then promoted to the fleet. Workstation is
the one display it works on. A pilot of one proved nothing about the other
three, and the pilot was treated as if it had.

## How this is verified

- `[touch] sampled N taps, delivered M, discarded K stale, collapsed J` once
  per check-in, so the fleet reports whether the queue is doing anything.
- The existing `[loop] iteration took N ms` line stays. Its count should not
  change; what changes is that taps inside those windows are no longer lost.
- Piloted on one display for a full day before any other device gets it. The
  last two touch changes shipped to all four at once and both were regressions.

## History worth not repeating

Two touch faults in one day, both self-inflicted, both from assuming a pin was
free:

1. A PENIRQ interrupt was added on a pin the touch config sets to `-1`. It
   could not have helped, and `pinMode` on the pin chosen for it did harm.
2. `Sound::chirpBootIdentification()` chirped GPIO25 as a speaker candidate and
   then set it to `INPUT`. GPIO25 is the touch clock. The software SPI driver
   moves it by writing the output register, which does nothing once the output
   enable is cleared, so touch was dead from the end of `setup()` on every boot
   of v2026.10.09.0003 through .0006. The reasoning that picked GPIO25 was
   "nothing else in this firmware claims it", which was true of this repository
   and false of the board.

Both have the same cause: the pins this board's display and touch use are in
LovyanGFX's board table, not in this repository. Any future claim that a pin is
free has to be checked there.
