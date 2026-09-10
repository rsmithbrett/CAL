# Is streaming RGB565 from SD to the display safe on this board?

**Status: analysis only. Nothing in this document has been implemented, and no
code was changed to produce it.** Written 2026-09-10 against `main` at
`ed9e350`, the vendored LovyanGFX in `Documents/Arduino/libraries/LovyanGFX`,
and ESP32 Arduino core 3.3.11 as installed on the build machine.

Read this before implementing the image-format change described in
"[What this has to inform](#what-this-has-to-inform)". It answers one blocking
question, and in the course of answering it finds that the question rests on a
premise that does not hold — which changes the recommendation substantially and
opens a cheaper first move than the one that was planned.

Every claim below is labelled. **Measured** means a number somebody read off
real hardware and recorded (all such numbers here are quoted from `README.md`
or from source comments recording live runs — no device was attached while this
was written). **Documented** means it is written down in code or in a
manufacturer's document that was actually read. **Inferred** means a conclusion
drawn from those, and the reasoning is shown so it can be attacked.

---

## Contents

- [What this has to inform](#what-this-has-to-inform)
- [The answer, up front](#the-answer-up-front)
- [1. The actual wiring: the SD card and the display are not on the same SPI bus](#1-the-actual-wiring-the-sd-card-and-the-display-are-not-on-the-same-spi-bus)
- [2. What CYD-Dickey actually does, and why it matters more than expected](#2-what-cyd-dickey-actually-does-and-why-it-matters-more-than-expected)
- [3. What LovyanGFX offers for pushing raw RGB565](#3-what-lovyangfx-offers-for-pushing-raw-rgb565)
- [4. The experiment that would settle it on device 17](#4-the-experiment-that-would-settle-it-on-device-17)
- [5. Storage and transfer consequences](#5-storage-and-transfer-consequences)
- [6. Recommendation, with confidence levels](#6-recommendation-with-confidence-levels)
- [7. Superseded reasoning, kept on the record](#7-superseded-reasoning-kept-on-the-record)
- [8. What would change this answer](#8-what-would-change-this-answer)

---

## What this has to inform

The product owner has settled on an image-format architecture to replace
PNG-for-everything:

| Content | Format |
| --- | --- |
| Photos | JPEG |
| Logos needing transparency | PNG, only where necessary |
| Fixed screens, icons, backgrounds | **RGB565 raw** |
| Everything else | PNG stays supported, stops being the default |

The motivation is a measurement, not a preference. At the instant a
10,568-byte file-buffer allocation failed on device 17
(`README.md`, "Both of these numbers were wrong"):

```
ESP.getFreeHeap()                             = 49,960
ESP.getMaxAllocHeap()                         = 32,756
heap_caps_get_free_size(MALLOC_CAP_8BIT)      = 11,340
heap_caps_get_largest_free_block(8BIT)        =  6,132
heap_caps_get_minimum_free_size               =  5,156
heap_caps_check_integrity_all                 = OK
```

Total free (11,340) barely exceeds the single allocation being attempted
(10,568). No amount of defragmentation fixes that; the peak concurrent working
set has to come down. A PNG draw as this firmware currently performs it wants
LovyanGFX's retained pngle scratch **plus** a whole-file buffer. RGB565 needs
neither: a 320×240 frame is 153,600 bytes and so cannot be held at once, but
streamed in chunks it needs only the chunk — 4 rows is 2,560 bytes, 8 rows is
5,120, both under the observed 6,132 ceiling.

The blocking unknown was framed as: **streaming RGB565 chunk-by-chunk
reintroduces SD/display interleaving on a bus the two devices share, and the
existing `readFileToBuffer()` exists precisely to avoid that.** That framing is
the thing this document had to check first, and it did not survive the check.

---

## The answer, up front

**The premise is wrong. The SD card and the display are not on the same SPI
bus on this board, as this firmware configures them.** The panel is driven on
SPI2/HSPI over GPIO 14 (SCLK) / 12 (MISO) / 13 (MOSI) with CS on GPIO 15. The
SD card is driven on SPI3/VSPI over GPIO 18 / 19 / 23 with CS on GPIO 5. Two
different SPI peripherals, six different GPIOs, two independent Arduino bus
mutexes. This is established from code on this machine, not inferred from a
board name — see [§1](#1-the-actual-wiring-the-sd-card-and-the-display-are-not-on-the-same-spi-bus)
for the full chain and for what to do with the README section it contradicts.

Consequently:

- **Bus contention is not the hazard it was believed to be.** Interleaving an
  SD read with a display write does not put two transactions on one wire here.
- **`readFileToBuffer()`'s stated justification does not hold**, and worse, that
  function is now the *source* of the failing allocation this whole
  investigation is about. It takes a 10-24KB contiguous block, per draw, on a
  device whose largest free block was measured at 6,132 bytes.
- **There is a cheaper first move than changing image formats at all**: stop
  buffering whole files and let LovyanGFX stream the decode from SD, which is
  what CYD-Dickey has always done and what this firmware did before the
  contention theory. Peak *new* heap per draw goes from 10-24KB to **zero**.
- **RGB565 for fixed screens is still the right destination**, and it is the
  only option with a genuinely zero-heap draw path, because the chunk buffer
  can be a static `.bss` array that never touches the allocator.
- **JPEG is stronger than the plan credits it.** LovyanGFX's JPEG decoder's
  entire workspace is one **3,900-byte** malloc, freed at the end of the draw,
  and streamed from SD it needs no file buffer at all. 3,900 is under the 6,132
  measured ceiling. There is no retained scratch and no `releaseJpgMemory()` to
  remember.

What is **not** established, and what [§4](#4-the-experiment-that-would-settle-it-on-device-17)
exists for: that chunked RGB565 SD→display streaming actually renders correctly
on device 17. The evidence is strong and indirect (CYD-Dickey ships the
equivalent interleave from SD; CAL's own bootloader already ships the exact
chunked `pushImage` pattern, though from LittleFS rather than SD). No direct
measurement exists. The failure mode if it is wrong is visual and diagnosable —
torn or displaced rows — not destructive.

---

## 1. The actual wiring: the SD card and the display are not on the same SPI bus

### The chain, link by link

**Documented.** `App/Display.cpp:14-16` brings the panel up through
autodetection rather than a pin map, and `:42` instantiates it:

```
#define LGFX_AUTODETECT
#include <LovyanGFX.hpp>
#include <LGFX_AUTODETECT.hpp>
...
LGFX lcd;
```

**Documented.** `README.md:2506-2508` records which board profile autodetect
matches: the CYD-family `_detector_Sunton_ESP32_2432S028_t`. That profile is in
the vendored library at
`Documents/Arduino/libraries/LovyanGFX/src/lgfx/v1_autodetect/LGFX_AutoDetect_ESP32_all.hpp`
and is in the ESP32-D0WD detector list at `:3562-3563`. The README's own
corroborating details — `cfg.bus_shared = false` on the panel, and the XPT2046
touch controller on software SPI with `cfg.spi_host = -1` — are at `:3114` and
`:3127-3132` of that file, so we are certainly looking at the same profile the
README was looking at.

**Documented.** The concrete pin and host assignment,
`_detector_Sunton_2432S028_9341_t` at `:3139-3172`:

```
, 40000000, 16000000
, GPIO_NUM_13     // MOSI
, GPIO_NUM_12     // MISO
, GPIO_NUM_14     // SCLK
, GPIO_NUM_2      // DC
, GPIO_NUM_15     // CS
, (gpio_num_t)-1  // RST
, (gpio_num_t)-1  // TF CARD CS
, 0               // SPI MODE
, false           // SPI 3wire
, HSPI_HOST       // SPI HOST
```

So: **panel on HSPI (SPI2), SCLK 14 / MISO 12 / MOSI 13, CS 15, 40 MHz write,
16 MHz read.** The ST7789 sibling at `:3173-3199` has the same pins and host
and an 80 MHz write clock; which of the two matched does not change the bus
question.

**Documented.** `App/SdStorage.cpp:14` and `:42` are the whole of this
firmware's SD bring-up:

```
constexpr uint8_t kChipSelectPin = 5;
...
gReady = SD.begin(kChipSelectPin);
```

One argument. No `SPIClass`, no frequency, no pins.

**Documented.** ESP32 core 3.3.11, `libraries/SD/src/SD.h:29-31`:

```
bool begin(
  uint8_t ssPin = SS, SPIClass &spi = SPI, uint32_t frequency = 4000000, const char *mountpoint = "/sd", ...
);
```

So `SD.begin(5)` binds to the global `SPI` object at 4 MHz.

**Documented.** `libraries/SPI/src/SPI.cpp:355`:

```
SPIClass SPI(VSPI);
```

and `:96-99`, which is the pin defaulting for that instance:

```
_sck  = (_spi_num == VSPI) ? SCK  : 14;
_miso = (_spi_num == VSPI) ? MISO : 12;
_mosi = (_spi_num == VSPI) ? MOSI : 13;
_ss   = (_spi_num == VSPI) ? SS   : 15;
```

with `variants/esp32/pins_arduino.h:12-15` giving `SS=5, MOSI=23, MISO=19,
SCK=18`.

Note what that snippet says in passing, because it is a useful cross-check: the
*non*-VSPI defaults in the Arduino core are 14/12/13/15 — exactly the panel's
pins from the autodetect profile. The two halves of this board were wired to
the two SPI peripherals' conventional pinouts, and each driver picked up its
own.

**Documented.** `libraries/SD/src/sd_diskio.cpp:450` shows how the SD driver
actually talks: `card->spi->beginTransaction(SPISettings(card->frequency,
MSBFIRST, SPI_MODE0))` on that `SPIClass`, i.e. through the Arduino per-host
transaction lock on VSPI.

**Documented.** Nothing else in either sketch touches SPI. A grep for
`SD.begin` and `SPI.begin` across `App/*.cpp`, `App/*.ino`, `*.cpp` and `*.ino`
returns exactly one hit: `App/SdStorage.cpp:42`.

### The conclusion, and its strength

**Inferred, but only in the weakest sense of the word** — this is arithmetic
over documented facts, not a hypothesis:

| | SPI host | SCLK | MISO | MOSI | CS | Clock |
| --- | --- | --- | --- | --- | --- | --- |
| Panel (ILI9341) | SPI2 / HSPI | 14 | 12 | 13 | 15 | 40 MHz W, 16 MHz R |
| SD card | SPI3 / VSPI | 18 | 19 | 23 | 5 | 4 MHz |
| Touch (XPT2046, unused) | software SPI | 25 | 39 | 32 | 33 | — |

There is a further, independent corroboration inside the library itself.
LovyanGFX's detector base *has* a slot for a TF card on the panel's own host —
`pin_tfcard_cs` (`:800`, `:821`, `:827`) — and when it is set, autodetect runs
`_set_sd_spimode(spi_host, pin_tfcard_cs)` (`:693`, called at `:865-867`) to put
that co-tenant card into SPI mode over the shared host before probing the panel.
The Sunton 2432S028 profiles pass `(gpio_num_t)-1` for it (`:3152`, `:3186`).
The library's own board profile therefore asserts, in its own data, that on this
board there is no SD card on the panel's SPI host.

### What to do with the README's "likely root cause" section

`README.md:2482` opens **"The likely root cause: the SD card's SPI lines are
shared with the display's, by hardware design"**, and quotes LCDWIKI's manual
for this exact board:

> "SPI_CLK, SPI_MISO, and SPI_MOSI pins are shared with the MicroSD card
> SPI pins."

**I did not read that manual.** It is an external PDF, one part of it is
image-scanned, and this analysis had no network access; everything below is
about how that quoted sentence reconciles with the code, not about whether the
quotation is accurate.

Two readings of it are available, and they are not equally supported.

**Reading (a), the README's:** the SD shares SCLK/MISO/MOSI *with the display*.
This is incompatible with everything in the table above. If the card's clock
were GPIO 14, autodetect could not have identified the panel by reading its ID
register over 14/12/13 while a card sat unconfigured on the same lines, and
`SD.begin(5)` could not then mount a card over 18/19/23 and report its true
size — which it does, per `SdStorage.cpp:44-46`'s log line. Both subsystems
work, at their own pins, on their own hosts. The reading contradicts the
observable behaviour of the device.

**Reading (b), CYD-Dickey's:** the pins *named* `SPI_CLK`/`SPI_MISO`/`SPI_MOSI`
— i.e. the pins broken out on the board's general-purpose SPI expansion header
— are shared with the MicroSD card's. `CYD-Dickey/SdCard.h`'s header comment
says exactly this, and was written independently:

> Per the LCDWIKI E32R28T schematic this shares the ESP32's default VSPI pins
> (MOSI 23 / MISO 19 / SCK 18) with the general SPI peripheral header, CS on
> GPIO5 -- no custom SPIClass needed.

Reading (b) is consistent with every documented fact in this section, and it is
also the more literal reading of the quoted sentence: the manual names three
*board pins* and says the card is on them. It does not mention the display.

**Recommendation for the README:** that section should be marked superseded
rather than deleted — the project's own convention, and the right one here,
because the section's reasoning was careful and the sources it cites are real.
What it got wrong is a single inferential step: "the only other real hardware
SPI peripheral in this build is the display itself, [...] leaving the display as
the only plausible co-tenant." The co-tenant is a header with nothing plugged
into it. The section then correctly notes that
`cfg.bus_shared = false` tells the library to assume exclusive ownership "with
no arbitration for a co-tenant" — and reads that as a danger, when it is the
library's board profile stating a fact that is true.

One further correction is owed to that section, and it is the more actionable
one. It says:

> Properly serializing access to that one shared bus [...] would mean either
> patching the vendored LovyanGFX library to expose a hook around its own SPI
> transactions, or wrapping every call site project-wide with a shared mutex —
> real surgery [...]

That facility already exists in the library, unpatched, and it is one config
line. See [§3](#the-arbitration-lovyangfx-already-has-and-that-this-board-turns-off).

---

## 2. What CYD-Dickey actually does, and why it matters more than expected

The reference project's SD bring-up is identical in every respect that matters
(`CYD-Dickey/SdCard.cpp`):

```
static const int SD_CS_PIN = 5;
...
ready = SD.begin(SD_CS_PIN);
```

Same pin, same defaulted `SPIClass`, same VSPI. Same panel bring-up too —
`CYD-Dickey.ino:21-23` and `:44` use `LGFX_AUTODETECT` and a plain `LGFX lcd`,
exactly as `App/Display.cpp` does. **So its SD is on the same bus as ours, and
its display is on the same bus as ours, and neither is the other.** Its success
is therefore directly transferable, which is the opposite of the concern raised
about it. (Had its card been on a separate host, its clean record would indeed
have told us nothing.)

**And it streams.** `CYD-Dickey.ino:132` and `:139`, inside
`showSplashScreen()`:

```
ok = lcd.drawPngFile(SD, path.c_str(), 0, 0, 0, 0, 0, 0, 0.0f, 0.0f, middle_center);
...
ok = lcd.drawJpgFile(SD, path.c_str(), 0, 0, 0, 0, 0, 0, 0.0f, 0.0f, middle_center);
```

These are the file-streaming overloads: the decoder pulls bytes from SD through
a `DataWrapper` and pushes decoded pixels to the panel, alternating for the
whole length of the image. It never buffers a whole file. It has no
`readFileToBuffer()` equivalent and no need of one.

This is worth stating plainly because it is the strongest single piece of
evidence in this document: **the exact interleaving pattern the RGB565 plan was
feared to reintroduce is what the reference project has shipped from the
beginning, from SD, on this board family, in the project whose author's own
observation is that its graphics never had any of these problems.** That
observation is already load-bearing elsewhere in this repo — `README.md`'s
"Reversing 'never released'" section credits it with cracking the decoder-scratch
question — so it is not a new or untested source of authority.

Two details of how it does it are worth carrying forward:

- **`lcd.releasePngMemory()` unconditionally after every PNG draw**
  (`CYD-Dickey.ino:135`), including after a failure. `App/Display.cpp`'s
  `releaseDecodeMemory()` deliberately does *not* do this, for a reason
  `README.md` documents at length and which is sound given that App re-draws
  constantly and CYD-Dickey draws a splash once.
- **Extension-dispatched format choice** (`.png` / `.jpg`/`.jpeg` / else BMP,
  `CYD-Dickey.ino:121-143`). The multi-format architecture being proposed here
  is, in miniature, something the reference project already had.

**Established vs reported.** That CYD-Dickey calls the streaming overloads from
SD is documented (I read it). That it never had graphics trouble is *reported* —
by its author, in this repo's own README — not measured here.

---

## 3. What LovyanGFX offers for pushing raw RGB565

All line references are into
`Documents/Arduino/libraries/LovyanGFX/src/lgfx/v1/`.

### `pushImage` — the right call, and it is already in use in this repo

`LGFXBase.hpp:408` and its overloads take a typed source pointer;
`LGFXBase.cpp`'s `pushImage(x, y, w, h, pixelcopy_t*, bool use_dma)` is the
implementation, and it ends:

```
startWrite();
_panel->writeImage(x, y, dw, dh, param, use_dma);
endWrite();
```

**Each `pushImage` call opens and closes its own panel transaction.** That is
the important behavioural fact for this design, and it is a good one: a
per-chunk `pushImage` loop with no outer `startWrite()` releases the panel bus
between chunks.

Two things must line up for the fast path. First, byte order:
`create_pc_fast(const uint16_t*)` (`LGFXBase.hpp:1114`) routes through
`_swapBytes`, which defaults to `false` (`LGFXBase.hpp:1029`), so the source is
treated as `swap565_t` — 565 stored high-byte-first, the panel's wire order.
Second, `no_convert` is set by `misc/pixelcopy.cpp:41` as simply
`src_depth == dst_depth`; with a `swap565_t` source and a 16-bit SPI panel
(`rgb565_2Byte`) it is **true**, and `Panel_LCD::writeImage` then takes its
`no_convert` branch straight to `write_bytes(src, w*h*2, use_dma)` — no
per-pixel conversion at all.

**This is not theoretical on this hardware; CAL's own bootloader already does
it.** `CAL/Display.cpp:141-172`, `drawBrandImage()`:

```
// Streamed a row at a time. A full-screen buffer would be 150KB and this
// device has no PSRAM; one row is 480 bytes.
static uint16_t row[kBrandW];
const int x0 = (kScreenW - kBrandW) / 2;
for (int y = 0; y < kBrandH; ++y) {
  if (f.read(reinterpret_cast<uint8_t*>(row), sizeof(row)) != sizeof(row)) {
    f.close();
    return false;
  }
  lcd.pushImage(x0, y0 + y, kBrandW, 1, row);
}
```

240×120 raw RGB565 from `/brand.565` (`CAL/Display.cpp:30-34`), 120 iterations of
a 480-byte read followed by a `pushImage`, with a **static** row buffer that
never touches the allocator, and a whole-file size check that discards a
truncated asset rather than rendering noise. `README.md:2203-2207` records the
same design.

So the format, the byte order, the chunked-`pushImage` pattern and the
static-buffer discipline are all already proven on this exact panel. **The one
thing that differs in the proposal is the storage: `drawBrandImage()` reads from
LittleFS on internal flash, not from SD.** That is precisely the gap
[§4](#4-the-experiment-that-would-settle-it-on-device-17) closes, and it is the
*only* gap.

### `writePixels` / `pushPixels`

`LGFXBase.hpp:342-356`. `pushPixels` is `startWrite(); writePixels(...);
endWrite();` and writes into whatever window is already set. Useful if you want
one window for the whole frame and a stream of pixels into it; not needed here,
because `pushImage` per chunk sets its own window and the window setup is a
handful of bytes against 640-5,120 bytes of payload.

### The DMA variants, and the hazard in them

**This is the one API trap in the whole design.** `pushImageDMA` /
`writePixelsDMA` (`LGFXBase.hpp:436`, `:346-348`) pass `use_dma = true`, and in
`platforms/esp32/Bus_SPI.cpp`'s `writeBytes` that path calls
`_setup_dma_desc_links(data, length)` — pointing the DMA descriptors at **the
caller's buffer** — then `exec_spi()` and returns. The transfer is still in
flight. `LGFXBase.hpp:337-338` (`waitDMA()`, `dmaBusy()`) exist because of this.

Refilling a single chunk buffer with the next SD read while DMA is still
reading it is a straightforward tearing bug, and it will look exactly like a
bus-contention failure, which is the worst possible way for it to present in
the middle of this investigation.

The non-DMA `pushImage` never has this problem, and it is not slower in any way
that matters:

- length ≤ 64 bytes: straight into the SPI FIFO registers, blocking.
- length < 1024 with a DMA channel configured — and one is
  (`SPI_DMA_CH_AUTO`, autodetect `:875-878`) — the bus **copies into its own
  internal flip buffer** and DMAs from that copy. The caller's buffer is free
  the moment the call returns.
- length ≥ 1024 with `use_dma == false`: the register/FIFO loop with
  `wait_spi()` between pushes. Blocking; reads the caller's buffer only during
  the call.

**So `pushImage` never retains a pointer to the caller's buffer past the call,
at any chunk size.** Use it. Skip the DMA variants for the first cut — the
panel write is a small fraction of the total time anyway (arithmetic in
[§5](#latency-what-a-draw-costs)), so there is nothing meaningful to win and a
real correctness hazard to lose.

### `startWrite` / `endWrite` and what they do to bus ownership

`Panel.hpp:87-89`:

```
void startWrite(bool transaction = true) { if (1 == ++_start_count && transaction) { beginTransaction(); } }
void endWrite(void) { if (_start_count) { if (0 == --_start_count) { if (_auto_display) { display(0,0,0,0); } endTransaction(); } } }
```

Refcounted. The **first** `startWrite` begins the transaction; nested ones only
increment. So wrapping a chunk loop in an outer `startWrite()`/`endWrite()`
turns every inner `pushImage`'s pair into no-ops and holds one transaction
across the entire loop — including across every SD read inside it.

What "holding a transaction" concretely means, for `Panel_LCD`
(`panel/Panel_LCD.cpp:56-64`) and `Bus_SPI`
(`platforms/esp32/Bus_SPI.cpp:504-558`):

1. `spi::beginTransaction(_cfg.spi_host)` → on Arduino,
   `spiSimpleTransaction(_spi_handle[spi_host])` (`platforms/esp32/common.cpp:1007-1015`)
   — the Arduino **per-host** SPI mutex.
2. The host's clock divider, mode and pin registers are reprogrammed for the
   panel.
3. The panel's CS is asserted and stays asserted.

`endTransaction` (`Bus_SPI.cpp:560-573`) reverses it, and does one thing worth
noticing:

```
#if defined (ARDUINO) // Arduino ESP32
    *_spi_user_reg = SPI_USR_MOSI | SPI_USR_MISO | SPI_DOUTDIN; // for other SPI device (e.g. SD card)
#endif
```

The library does anticipate co-tenancy on a host and restores register state
for it. But the mutex and the registers are **per-host**, so a panel
transaction held open on HSPI can neither block nor corrupt an SD transaction
on VSPI. Given [§1](#1-the-actual-wiring-the-sd-card-and-the-display-are-not-on-the-same-spi-bus),
holding the bus across an SD read is not the hazard it would be on a
genuinely shared bus.

It is still not free, and the recommendation is still not to do it: an
asserted CS and a half-written GRAM window held for the several hundred
milliseconds an SD read of a full frame takes is a state nothing else in the
system expects, and it buys only the per-chunk window setup. **Do not wrap the
loop.** Let each `pushImage` open and close, exactly as
`CAL/Display.cpp:164-170` already does. Variant B in
[§4](#the-five-variants-and-what-each-one-isolates) tests the held-transaction
case anyway, because it is the shape `drawPngFile` uses today and we want to
know whether it is harmless.

### The arbitration LovyanGFX already has, and that this board turns off

This is the finding that most changes what to do if the experiment goes badly.

`LGFXBase.cpp:3586-3593`:

```
void LGFXBase::prepareTmpTransaction(DataWrapper* data)
{
  if (data->need_transaction && isBusShared())
  {
    data->parent = this;
    data->fp_pre_read  = tmpEndTransaction;
    data->fp_post_read = tmpBeginTransaction;
  }
}
```

with `LGFXBase.hpp:1384-1394`:

```
static void tmpBeginTransaction(LGFXBase* lgfx) { if (lgfx->getStartCount()) { lgfx->beginTransaction(); } }
static void tmpEndTransaction(LGFXBase* lgfx)   { if (lgfx->getStartCount()) { lgfx->endTransaction(); } }
```

In other words: **LovyanGFX will drop the panel transaction before every single
file read and re-take it afterwards, automatically, for the whole length of a
decode** — releasing the bus mutex, deasserting CS, restoring the registers —
if two conditions hold. `prepareTmpTransaction` is called by `draw_png`
(`LGFXBase.cpp:3420`), `draw_jpg` (`:3039`), `draw_bmp`, `draw_qoi` and font
loading.

The two conditions:

- `need_transaction` on the data source. For an SD-backed wrapper this is set
  at `platforms/esp32/common.hpp:255`: `need_transaction = (fs ==
  &LGFX_FILESYSTEM_SD);` — **SD specifically**, not LittleFS or SPIFFS. The
  library knows which filesystem sits on a potentially-shared SPI bus.
- `isBusShared()` — `panel/Panel_Device.hpp:144`, returning `_cfg.bus_shared`,
  which the Sunton profile sets to **`false`** (autodetect `:3114`).

So on this board the arbitration is switched off, and
`draw_png`/`draw_jpg` reach their
`this->startWrite(!data->hasParent())` (`LGFXBase.cpp:3470`, `:3098`) with
`hasParent()` false, meaning `startWrite(true)` — the panel transaction is
begun and **held for the entire streamed decode**, SD reads and all. That is
what CYD-Dickey has been doing successfully all along, and it is the
strongest-form version of the interleave.

**The consequence for this project is large.** If the shared-bus hypothesis had
been correct, the fix was never "read whole files into RAM" and it was never
"patch the vendored library". It was:

```
auto cfg = lcd.getPanel()->config();
cfg.bus_shared = true;
lcd.getPanel()->config(cfg);
```

— one config line, at init, using a facility the library ships for exactly this
case. That line remains the correct fallback if
[§4](#4-the-experiment-that-would-settle-it-on-device-17)'s variant B fails.
(It has no side effect on this firmware's touch handling, because CAL and App
do not use touch; `panel/Panel_Device.cpp:528` shows `bus_shared` also
bracketing touch reads with transactions, which is moot here.)

### The decoder scratch numbers, since they decide the format question

**Documented, exactly, from the library source:**

| Path | Workspace | Lifetime | File buffer needed |
| --- | --- | --- | --- |
| PNG (`draw_png`) | `pngle_t`, containing `uint8_t lz_buf[TINFL_LZ_DICT_SIZE]` = **32,768 bytes** plus tinfl state and struct overhead — the "~44KB" this repo quotes (`utility/lgfx_pngle.c:113`, `:136`) | Allocated once by `lgfx_pngle_new()` and **deliberately retained** until `releasePngMemory()` (`LGFXBase.cpp:3420-3428` and its comment) | none if streamed from SD; 10-54KB if handed a RAM buffer |
| JPEG (`draw_jpg`) | one `malloc(3900)` (`LGFXBase.cpp:3050-3051`), with a 512-byte stream input buffer inside it (`utility/lgfx_tjpgd.h:16` `JD_SZBUF`) | `free(pool)` on every exit path (`:3063`, `:3079`, `:3106`) — **nothing retained** | none if streamed from SD |
| RGB565 raw | none — no decoder exists | — | one chunk; can be static `.bss` |

**3,900 bytes is under the measured 6,132-byte largest free block.** That single
number is why [§6](#6-recommendation-with-confidence-levels) rates JPEG higher
than the plan does.

One more capability worth recording, because it solves a problem this firmware
currently solves badly: `drawJpg(Stream*, ...)` exists
(`lgfx_filesystem_support.hpp:184-190`), so a JPEG can be decoded straight off
an `HTTPClient` stream with a 3,900-byte pool and **no buffer of any kind**.
Today the no-SD-card fallback is `Assets::fetchToRam()` → `drawRam()`
(`App/Assets.h:130-176`), which holds the entire asset in RAM — on a device with
11,340 bytes of 8BIT free. A streamed JPEG is a strictly better card-less path.
Note the interesting asymmetry at `lgfx_filesystem_support.hpp:149` vs `:190`:
the `File*` variant hardcodes `need_transaction = true` while the `Stream*`
variant uses `isBusShared()`.

---

## 4. The experiment that would settle it on device 17

### What is actually unknown

Only one thing: **does a chunked read-from-SD / push-to-panel loop render
correctly and repeatably on this hardware?** Everything else in the design is
either documented library behaviour or already shipping in
`CAL/Display.cpp:141-172`.

It is worth being precise about why this still needs measuring even though
[§1](#1-the-actual-wiring-the-sd-card-and-the-display-are-not-on-the-same-spi-bus)
removed the bus-sharing premise. Three reasons:

1. This repo's decode-failure history is explicitly **intermittent**
   (`README.md`, "the same picture has been observed to display successfully,
   then fail to decode on a *later* draw of the exact same already-cached
   file"). Whatever that is — the README's leading hypothesis is an intermittent
   SD read glitch — a new draw path that reads from SD many times per frame
   multiplies its exposure by 30-240×. That is a real risk that has nothing to
   do with bus sharing.
2. A single successful draw would prove almost nothing about an intermittent
   fault. The experiment has to repeat.
3. The point of this document is to not ship a fix built on a plausible theory.
   The last one cost a night.

### Where it goes

**`SelfTest/`.** It already has everything: the per-device manifest override
delivery path (`README.md:2036-2045`), the same `LGFX_AUTODETECT` panel
bring-up as App, the remote debug stream with an explicit
`Log::setStreamingEnabled(true)` / `flushNow()` (`SelfTest/Log.h:49`, `:64`) so
results are visible without waiting for a check-in to turn streaming on, and a
JSON report (`SelfTest/Report.cpp`) that logs unconditionally and POSTs
best-effort. It already reads a **150,000-byte** file from SD and verifies it
byte-for-byte (`SelfTest/SdTest.cpp:214`: `const uint32_t sizes[] = {2048,
34816, 80000, 150000};`), so a full-frame-sized SD read is a known-working
operation there. What has never been tested is an SD read *interleaved with*
display writes.

Add `SelfTest/RawImageTest.cpp` / `.h` and one field in `Report.cpp`'s JSON. No
change to `App/`, `CAL/`, or the vendored library — except variant D below,
which sets one config value in SelfTest's own `Display.cpp` init, leaving App
untouched.

Do **not** put this in `App/`: boot-time logs never reach the remote stream
(`README.md`, "Where the boot splash has to happen"), and App has no
remote-command channel to trigger a one-off test — `App/Actions.h` is card
buttons, not a test harness.

### The test asset

One file on the card: `/selftest/raw320x240.565`, exactly **153,600 bytes**,
raw RGB565, **high byte first** — the same byte order `/brand.565` already uses,
which is what `pushImage(const uint16_t*)` consumes with the default
`setSwapBytes(false)` (see [§3](#pushimage--the-right-call-and-it-is-already-in-use-in-this-repo)).
Generate it host-side with a short script and copy it to the card over USB, or
push it through the existing asset pipeline.

**The content is part of the experiment design, not a detail.** Use a
self-verifying pattern:

- Row *r* is a solid colour derived from *r*: `R = r`, `G = 255 - r`,
  `B = (r * 7) & 0xFF`, quantised to 565.
- Overlay a 16-pixel-wide vertical white bar at `x = (r * 4) % 304`, so the bars
  form a continuous diagonal down the screen.

Why not a gradient or noise: a gradient hides tearing, and noise hides
everything. With this pattern a chunk written at the wrong `y` shows as a
repeated or missing colour band, and a torn or duplicated chunk breaks the
diagonal visibly. It is also cheap to recompute row-by-row on the device for the
readback comparison below, without storing an expected copy.

### The loop under test

```
static uint16_t chunk[320 * 24];   // .bss, 15,360 bytes, zero heap
```

For each chunk height `R` in **{1, 2, 4, 8, 16, 24}** rows (640 / 1,280 / 2,560
/ 5,120 / 10,240 / 15,360 bytes):

1. `lcd.fillScreen(TFT_BLACK); delay(50);`
2. Open `/selftest/raw320x240.565` once.
3. `for (int y = 0; y < 240; y += R) { f.read((uint8_t*)chunk, 320*R*2);
   lcd.pushImage(0, y, 320, R, chunk); }`
4. Close.

Note that R=16 and R=24 exceed the measured 6,132-byte largest free block. They
are included deliberately: with a static buffer that ceiling is irrelevant, and
knowing whether larger chunks are meaningfully faster is what decides the
shipping value of R. If they are not faster, the shipping buffer can be small.

### What to log, per pass

Everything here is a number someone can act on weeks later:

- `R`, chunk bytes, iteration count (`240 / R`).
- Total elapsed ms for the pass.
- Accumulated `micros()` inside `f.read` vs inside `pushImage`, separately. **This
  split is the single most useful number the run produces** — nobody currently
  knows the SD read throughput on this device, and §5's latency arithmetic is an
  estimate for want of it.
- Short-read count and any `f.read` returning less than requested.
- `heap_caps_get_free_size(MALLOC_CAP_8BIT)` and
  `heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)` immediately before and
  immediately after the pass. Reuse `App/Display.cpp:1964-1981`'s
  `logHeapSnapshot()` shape so the lines are comparable with every existing heap
  line in the fleet's logs.
- Readback mismatch counts (below).

Run each `(R, variant)` combination **20 times back to back**, and then run the
whole set once more **immediately after a check-in**, so the run also covers the
post-TLS heap and register state where the current failures cluster
(`README.md`: "the failures cluster in the seconds after a check-in while the
draws between check-ins succeed").

### The objective pass criterion: read the panel back

Visual inspection alone puts the result at the mercy of whoever is holding the
device. `lcd.readRect(x, y, w, h, uint16_t*)` exists (`LGFXBase.hpp:661`) and
this panel is configured with a 16 MHz read clock (autodetect `:3149`), so after
each pass read the frame back one row at a time into a 640-byte buffer and
compare against the recomputed expected row, counting mismatched pixels and
fully-mismatched rows.

**State the caveat in the report rather than discovering it later.** ILI9341
SPI readback commonly returns colour truncated to 6-6-6 and is not always
reliable. A small, uniformly distributed low-bit mismatch is expected and is
**not** a failure. The signals that matter are structural:

- whole rows wrong, or rows appearing at the wrong `y`;
- a mismatch count that **rises with `R`**, or with iteration index;
- mismatches clustered at chunk boundaries.

If readback proves unusable — which variant E will reveal — fall back to a
photograph and say so in the report. An honest "inconclusive" is the third
legitimate outcome here.

### The five variants, and what each one isolates

| | Variant | What it does | What it isolates |
| --- | --- | --- | --- |
| **A** | Baseline | `pushImage` per chunk, no outer transaction | The shipping candidate. This is `CAL/Display.cpp:141-172`'s pattern, moved from LittleFS to SD. |
| **B** | Held transaction | `lcd.startWrite()` before the loop, `lcd.endWrite()` after | The panel transaction, HSPI mutex and CS stay asserted across every SD read. This is exactly what `drawPngFile(SD,...)` does today on this board (`LGFXBase.cpp:3470`). **If A passes and B fails, bus co-tenancy is real after all** and §1 is wrong — go straight to D. |
| **C** | DMA | `pushImageDMA` with (i) one buffer, refilled immediately — deliberately incorrect; (ii) two alternating buffers plus `lcd.waitDMA()` | Puts a number on the DMA-source-lifetime hazard from §3. Expect (i) to tear. Run it anyway, so the next person to "optimise" this path finds the measurement instead of rediscovering the bug. |
| **D** | Arbitrated | `cfg.bus_shared = true` on the panel config at init, then re-run B | If B failed, this is the fix, and it is one line. Enables `prepareTmpTransaction`'s automatic drop-and-retake around every SD read (§3). |
| **E** | Control | The same pass reading from LittleFS instead of SD | Distinguishes "reading a file while drawing is a problem" from "reading **SD** while drawing is a problem", and independently validates whether `readRect` is trustworthy at all. |

**Sizing note for E:** the `spiffs` partition is `0x30000` = 196,608 bytes
(`App/partitions.csv`), and a 153,600-byte file will not comfortably fit
alongside anything else. Use a 320×60 asset (38,400 bytes) or reuse CAL's
existing 240×120 `/brand.565` (57,600 bytes) and scale the comparison
accordingly.

### Success, failure, inconclusive — stated so nobody has to interpret

**PASS** for a given `(R, variant)`:

- 20/20 passes complete;
- zero short reads;
- readback mismatch confined to low-order colour bits — under 1% of pixels, **no
  whole row wrong, no row displaced, no clustering at chunk boundaries**;
- no watchdog reset and no crash;
- the next SD operation after the pass succeeds (open the file again and read
  one chunk);
- `largest free 8BIT block` unchanged within ±512 bytes before vs after.

**FAIL:**

- any whole row wrong or displaced; or
- any short read; or
- any pass that does not complete; or
- a mismatch count that increases with `R` or with iteration index; or
- a subsequent SD operation failing where it succeeded before the pass; or
- a watchdog reset.

**INCONCLUSIVE** — and report it as such rather than rounding it to pass:

- readback unusable; or
- variant E shows mismatches at the same rate as A, which indicts `readRect`
  rather than the bus.

### The shipping decision rule

Pick the **smallest `R` whose total elapsed time is within ~10% of the best
observed**. Once the SD read dominates — which the read/push time split will
show — larger chunks buy very little, and a smaller `R` keeps the static buffer
smaller and releases the panel bus more often. My prediction is `R = 8`
(5,120 bytes), for what that is worth; the measurement decides.

### Effort

One new `.cpp`/`.h` in `SelfTest/` (~250 lines), one field in `Report.cpp`'s
JSON, one generated asset on the card, one server-side manifest override to
point device 17 at the SelfTest build. Variant D touches one line in
`SelfTest/Display.cpp`. Nothing in `App/`, `CAL/`, or the vendored library
changes.

---

## 5. Storage and transfer consequences

### Storage: a non-issue by four orders of magnitude

**Measured** (telemetry, reported to this analysis; `App/Telemetry.cpp:121-122`,
`:155-156` are the fields, `Sd::totalBytes()`/`Sd::usedBytes()` the sources):
roughly **7.4 GB** total, about **0.5 MB** used.

**Arithmetic:**

| | Bytes |
| --- | --- |
| Full-screen 320×240 RGB565 | 320 × 240 × 2 = **153,600** |
| One row | 320 × 2 = 640 |
| Current re-encoded PNG, typical | ~12,000 |
| Server's hard ceiling today (`AssetSizeTarget.MaxBytes`) | 24,576 (`README.md:3149`) |
| Ratio, RGB565 vs typical PNG | **12.8×** |
| Ratio, RGB565 vs the 24KB ceiling | 6.25× |

- 7.4 GB ÷ 153,600 = **~48,000** full-screen assets before the card is full.
- A generous catalogue of 200 full-screen assets = 30.7 MB = **0.41%** of the
  card.
- Current usage would go from ~0.5 MB to, say, ~5 MB for a realistic catalogue.
  Still under 0.1%.

**Verdict: not a constraint.** Not close to one. `App/Assets.h`'s own framing of
SD storage as "unlimited but accountable" is exactly right and the accounting
comes out fine. FAT cluster slack on a card this size (typically 32 KB clusters)
adds at most one cluster per file — noise.

**Two storage caveats that are real:**

1. **Do not extend full-screen RGB565 to CAL's LittleFS brand asset.** The
   `spiffs` partition is 196,608 bytes (`App/partitions.csv`); a 320×240 raw
   asset would take 78% of it. CAL's 240×120 format (57,600 bytes) is already
   the right size for where it lives, and `CAL/Display.cpp:30-34` should stay as
   it is.
2. **RGB565 is an SD-only format.** The no-card fallback path
   (`Assets::fetchToRam()` → `drawRam()`, `App/Assets.h:130-176`) holds the whole
   asset in RAM, and 153,600 bytes is not available on a device with 11,340 free.
   A card-less device therefore loses any asset that is RGB565-only. Keep a
   JPEG or PNG variant available for that path — or better, adopt streamed
   `drawJpg(Stream*, ...)` for it (see
   [§3](#the-decoder-scratch-numbers-since-they-decide-the-format-question)),
   which needs 3,900 bytes and no buffer at all and is a strict improvement on
   what that path does today.

### Transfer: one real problem, and it is a two-line fix

Assets are fetched over HTTPS once and cached on SD
(`App/Assets.cpp`'s `fetchToCard()`), so the recurring cost is zero and the
one-time cost is 12.8× more bytes. On its own that is fine.

**But there is a specific latent bug that 153,600 bytes will expose and 12,000
bytes never could.** `App/Assets.cpp:269`:

```
const uint32_t deadline = millis() + Config::kHttpTimeoutMs;

while (http.connected() && (expectedSize < 0 || written < expectedSize)) {
  const size_t available = stream->available();
  if (available == 0) {
    if (millis() > deadline) {
      break;
    }
    delay(1);
    continue;
  }
  ...
}
```

`deadline` is computed **once, before the loop, and never refreshed on
progress**. It is therefore not a stall timeout, it is a hard wall-clock budget
for the entire body transfer, enforced at the first moment the stream has no
bytes ready. With `Config::kHttpTimeoutMs = 20000` (`App/Config.h:20`):

| Asset size | Sustained throughput needed to finish inside 20 s |
| --- | --- |
| 12,000 bytes | ~0.6 KB/s |
| 24,576 bytes (server ceiling) | ~1.2 KB/s |
| **153,600 bytes** | **~7.5 KB/s** |

0.6 KB/s is effectively unreachable-as-a-failure. 7.5 KB/s is not: a device on
marginal WiFi can sit below it, and the failure is nasty — the loop `break`s,
the file is short, the SHA-256 check fails, the temp file is removed, and the
device retries forever without ever caching the asset. Silent, repeating, and
indistinguishable in telemetry from a server problem.

**Recommendation:** before any full-screen RGB565 asset ships, change that
deadline to refresh on progress (reset it whenever `read > 0`), so it becomes
the stall timeout it reads as. That is the correct behaviour at any size and it
is a two-line change. Optionally raise `kHttpTimeoutMs` as well, but the
refresh is the real fix. **This is the only genuine transfer constraint I found,
and it is worth fixing on its own merits regardless of what happens to the image
formats.**

Secondary, and fine: `fetchToCard()` also does a full SHA-256 **readback verify**
after writing (`App/Assets.cpp:341-365`), so a fetch costs a 153,600-byte SD
write plus a 153,600-byte SD read. At the SD bus's 4 MHz that is roughly 0.3 s
each way plus FAT overhead, once per asset, at fetch time rather than draw time.
Not worth changing.

### Latency: what a draw costs

**Inferred, and flagged as such because the SD number is an estimate** — the
experiment in §4 replaces it with a measurement, which is the main reason to
log the read/push split.

- **Display write:** 153,600 bytes = 1,228,800 bits at the configured 40 MHz
  write clock (autodetect `:3145`) = **~31 ms**, plus per-chunk window setup.
- **SD read:** 153,600 bytes at the SD library's default 4 MHz
  (`SD.h:29-31`) = ~307 ms of pure bus time, so realistically **~0.35-0.6 s**
  with per-512-byte-block command overhead and FAT traversal.
- **Total, full-screen: roughly 0.4-0.65 s, overwhelmingly SD-bound.**

Two consequences. First, the display write is about 5% of the draw, which is
why the DMA variants are not worth their hazard. Second, a full-screen RGB565
draw is likely *slower* than the PNG decode it replaces at 12KB — a smaller read
but real inflate and per-pixel alpha compositing work over 76,800 pixels. That
trade is memory for latency, and on a device that currently cannot complete the
allocation at all it is obviously the right trade; it should just be stated
rather than discovered. If it turns out to matter, raising the SD clock above
4 MHz is the lever, and it is independent of everything else here.

For icons and small fixed elements — the other half of the RGB565 proposal —
none of this applies: a 48×48 icon is 4,608 bytes and reads in single-digit
milliseconds.

---

## 6. Recommendation, with confidence levels

### The blocking question, answered

**"Does streaming RGB565 from SD to the display work on this board, given that
the SD card and the display share the SPI bus?"** — The question dissolves:
**they do not share the bus.** Panel on SPI2/HSPI (14/12/13, CS 15), SD on
SPI3/VSPI (18/19/23, CS 5).

**Confidence: high.** This is established from code on this machine — the
autodetect profile's own pin and host constants, the Arduino core's VSPI
defaults, this firmware's single defaulted `SD.begin(5)` — and independently
corroborated twice: by the library's own board profile declaring no TF-card CS
on the panel host, and by CYD-Dickey's `SdCard.h` comment, written by someone
else at another time from the same schematic. The residual risk is not bus
contention.

### What to do, in order

**1. Immediately, and independent of the format change: stop buffering whole
files.** Revert `App/Display.cpp`'s `drawPngFromSd()` /
`drawPngFromSdInRect()` to LovyanGFX's streaming `drawPngFile(SD, ...)`
overload, and delete `readFileToBuffer()`, `ensureFileBufferCapacity()`,
`gFileBuffer` and the provisional `Http::releaseTlsSession()` experiment block
that `App/Display.cpp:2122-2128` already marks for deletion.

This is the highest-value change available and it needs no format work at all:

| | Peak *new* heap per draw |
| --- | --- |
| Today (whole file in RAM + retained pngle) | 10,568-24,576 bytes contiguous, against a measured 6,132-byte largest block |
| `drawPngFile(SD, ...)` (retained pngle only) | **0** |

The failing allocation is not a symptom of the graphics stack; it is
`readFileToBuffer()` itself, added to solve a bus-contention problem that
[§1](#1-the-actual-wiring-the-sd-card-and-the-display-are-not-on-the-same-spi-bus)
finds does not exist. Removing it removes the failure directly. It also happens
to be what CYD-Dickey has always done.

*Confidence: high on the memory arithmetic (documented library behaviour plus
this repo's own measured numbers). Medium-high on the interleave being
harmless — CYD-Dickey ships it from SD on this board family and reports no
graphics trouble, but that is reported, not measured here.* §4's variant B tests
precisely this, so it can be validated before or alongside shipping. **If it
fails, the fallback is `cfg.bus_shared = true` — one line — not a return to
whole-file buffering.**

**2. Adopt JPEG for photos. This is the strongest single element of the
proposed architecture and the plan understates it.** `draw_jpg`'s entire
workspace is one 3,900-byte `malloc`, freed on every exit path
(`LGFXBase.cpp:3050-3051`, `:3063`, `:3079`, `:3106`), with a 512-byte stream
buffer inside it. Streamed from SD it needs no file buffer. **3,900 bytes fits
under the 6,132-byte measured ceiling with room to spare**, there is no retained
scratch, and there is no `releaseJpgMemory()` to forget. It requires no
bus-sharing change and no new file format on the server.

*Confidence: high. These are exact constants read out of the decoder source, and
CYD-Dickey already calls `drawJpgFile(SD, ...)` (`CYD-Dickey.ino:139`).*

**3. Adopt RGB565 for fixed screens, icons and backgrounds — after §4's
experiment.** It is the only option with a **zero-heap** draw path, because the
chunk buffer can be a static `.bss` array, and it needs no decoder at all. The
format, the byte order and the chunked-`pushImage` pattern are already proven on
this panel by `CAL/Display.cpp:141-172`. Use `pushImage` per chunk, no outer
`startWrite`, no DMA variants, and a static buffer.

*Confidence: medium-high, and deliberately not higher. The one untested link is
SD rather than LittleFS as the source, and this repo has a documented history of
intermittent SD read trouble that a 30-240-reads-per-frame draw path would
multiply its exposure to. That is what §4 is for, and it is cheap.*

**4. Retire the retained pngle scratch once PNG is no longer the default.** This
is the largest single heap win on the table and it falls out of the format
change rather than needing its own work. That block is 32,768 bytes of
`lz_buf` plus decompressor state — "~44KB" — held for the entire uptime
(`utility/lgfx_pngle.c:113`; `App/Display.cpp:2271-2299` explains why it is
currently kept). Once fixed screens are RGB565 and photos are JPEG, the only
remaining PNG draws are transparency-requiring logos, and if those are rare
enough, `releasePngMemory()` can be called after each one — or PNG can be
dropped from the App entirely. The board's largest free block should move by
roughly the size of that block. **Do not do this while PNG is still drawing
routinely:** `README.md` records that releasing it broke every graphic card,
because a post-WiFi heap has no 44KB hole to re-acquire it from. The ordering
matters.

**5. Keep PNG only where transparency is genuinely required, drawn from SD, not
from RAM.** And accept that each such draw needs the 44KB — which is affordable
only as long as the block is taken early, while ~110,580 bytes are still
contiguous, exactly as the boot splash does today.

**6. Fix the fetch deadline before any 153,600-byte asset ships**
(`App/Assets.cpp:269` — refresh on progress). See
[§5](#transfer-one-real-problem-and-it-is-a-two-line-fix). Worth doing on its
own merits.

### The honest bottom line

The document was asked to consider concluding "cannot be determined without the
experiment". That is the right answer to a *narrower* question than the one
asked, and it should be stated that way:

- **"Are the SD card and the display on the same SPI bus?"** — Determined. No.
- **"Is the RGB565 architecture sound on this hardware?"** — Yes, with high
  confidence on the memory arithmetic and the API mechanics.
- **"Will chunked RGB565 SD→display streaming render correctly and repeatably on
  device 17?"** — **Not determined.** Strong indirect evidence, no direct
  measurement. §4 settles it in one SelfTest build, and the failure mode is
  visual and diagnosable rather than destructive.
- **"Is there something safer to do first?"** — Yes, and it is better than the
  planned first move: delete `readFileToBuffer()`. It removes the failing
  allocation entirely, needs no format change, no server change and no new
  asset pipeline, and it is what the reference project has always done.

---

## 7. Superseded reasoning, kept on the record

Per this project's convention, the reasoning being replaced is recorded rather
than deleted, because each step was defensible on what was known at the time and
deleting it would let the same wrong turn be taken again.

**"The SD card's SPI lines are shared with the display's"** (`README.md:2482`).
Careful work, real sources, one wrong inferential step: having established that
the board's SD shares SCLK/MISO/MOSI with *something*, and having correctly
ruled out the touch controller (software SPI, `cfg.spi_host = -1`), it concluded
the display was "the only plausible co-tenant". The co-tenant is the board's
general-purpose SPI expansion header, with nothing plugged into it — which is
both the more literal reading of the manufacturer's own quoted sentence and what
CYD-Dickey's independently-written `SdCard.h` comment says. Mark superseded,
keep the section, keep the links.

**"Reading the whole file before decoding it, instead of streaming and decoding
at once"** (`README.md`, and `App/Display.cpp:2154-2163`). Follows directly from
the above and is therefore superseded with it. Two further notes:

- Its own memory justification was written when the facts looked different:
  *"the largest asset in the catalog today is under 34KB, against roughly 250KB
  of free heap in ordinary operation"*. The corrected measurement is 11,340
  bytes of 8BIT free and a 6,132-byte largest block, and the README already
  records that the 250KB figure was `ESP.getFreeHeap()` taken early in a boot
  and inflated ~4×. Against the real numbers the whole-file read was never
  affordable.
- The change may nonetheless have appeared to help at the time, and the reason
  is worth keeping: it moved the draw from a path that allocates nothing to a
  path that allocates once, at a moment in the boot when memory was still
  plentiful. That is not evidence for the contention theory.

**"The fix would mean patching the vendored LovyanGFX library to expose a hook
around its own SPI transactions, or wrapping every call site project-wide with a
shared mutex — real surgery"** (`README.md`, same section). Superseded by
`LGFXBase::prepareTmpTransaction` (`LGFXBase.cpp:3586-3593`): the hook exists,
unpatched, is applied automatically to SD-backed reads during every
`draw_png`/`draw_jpg`/`draw_bmp`/font load, and is gated on one config value the
board profile happens to set to `false`. The surgery is `cfg.bus_shared = true`.

**"DMA starvation", "memory stranded in word-addressable-only regions", "heap
corruption"** — all three already closed by the capability-class measurement
recorded in `README.md` ("Both of these numbers were wrong"), and nothing in
this analysis reopens any of them. Recorded here only so a future reader does
not resurrect them on seeing the phrase "SPI DMA" in
[§3](#the-dma-variants-and-the-hazard-in-them). The DMA hazard discussed there
is a source-buffer-lifetime bug in a specific API, not a memory-capability
problem.

---

## 8. What would change this answer

Stated explicitly so a future reader knows which facts are load-bearing and
therefore worth re-checking.

1. **A different autodetect profile matching.** Everything in §1 rests on the
   panel being on `HSPI_HOST` at 14/12/13. That comes from
   `_detector_Sunton_2432S028_9341_t`/`_7789_t`, which `README.md:2506-2508`
   states is the profile that matches. If a board revision, a library upgrade,
   or a different unit matched some other profile, re-derive the table. **Cheap
   check:** LovyanGFX logs its choice at init — `ESP_LOGI(LIBRARY_NAME,
   "[Autodetect] Sunton_2432S028 (ILI9341)")` (autodetect `:3160`). Nothing in
   this firmware currently records it. **Logging the detected board name and the
   panel's configured `spi_host`/`pin_sclk`/`pin_mosi`/`pin_miso` once at boot
   would turn the single most load-bearing inference in this document into a
   measurement, and it is a three-line change.** Do that.
2. **Replacing `LGFX_AUTODETECT` with an explicit pin map.** `README.md:61-66`
   already warns about this. It would also invalidate every autodetect-derived
   fact here.
3. **Any SD bring-up that passes a custom `SPIClass`.** If someone ever moves
   the SD onto HSPI to share the panel's bus deliberately — for pins, or for
   speed — then bus sharing becomes real, `cfg.bus_shared = true` becomes
   mandatory, and §4's variant B becomes the shipping configuration test rather
   than a curiosity.
4. **Raising the server's `AssetSizeTarget.MaxBytes`.** `README.md:3160-3166`
   already flags `kMinMaxAllocHeapBytes` and that ceiling as a pair that must
   move together. A format change that makes some assets 153,600 bytes needs
   that pairing rethought: the watchdog threshold is derived from "the largest
   contiguous block a draw must be able to allocate", and for an RGB565 draw
   that number becomes the chunk size, not the file size — which is the whole
   point, and means the constant should get *smaller*, not larger.
5. **The measured heap figures.** 11,340 free / 6,132 largest block is one
   instant on one device. If a later measurement shows a materially different
   steady state, the chunk-size arithmetic changes, though the ordering of the
   recommendations does not.
