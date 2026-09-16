# Updating CAL over the air — design

Design only. Nothing in this document is implemented. It covers steps 1–3 of the
approved suggestion *"Make CAL updatable without USB"*
(`7ab3267a-0f52-4153-9ece-89880ffb535e`): settle the bootloader-fallback
question, re-measure the two binaries, and decide where a CAL image is staged.
The `Kind=Cal` server work (step 4) and the App-side writer (step 5) are
deliberately **not** designed here beyond the interface the staging decision
forces on them.

The thing this feature is for, stated once so that every trade-off below can be
measured against it: **CAL is the image that rescues a device nobody can reach.**
A design that can leave a panel in somebody's hallway needing a USB cable is
wrong even if the window is milliseconds wide, because the whole reason CAL
exists is that the cable is not available.

---

## 1. The blocking question, and what the evidence actually says

> When `factory` holds an invalid image, does the ESP32 second-stage bootloader
> fall through and boot `ota_0`?

**The answer the shipped bootloader binary gives is yes.**

**EVIDENCE STATUS: read from the compiled bootloader. UNVERIFIED AGAINST
HARDWARE.** Section 2 is the procedure that settles it for real, and it is a
required deliverable precisely because this project has twice been wrong about
what a configuration appeared to request.

### 1.1 What was read, and why it is better than documentation

The pinned core ships no bootloader `.c` files — only headers and prebuilt
libraries. It does, however, ship the **linked bootloader ELF with full DWARF**,
which is the actual artifact flashed to `0x1000`:

```
~/AppData/Local/Arduino15/packages/esp32/tools/esp32-libs/3.3.11/bin/
    bootloader_qio_80m.elf          (527,652 bytes, 2026-08-30)
```

`bootloader_qio_80m` is the one this build selects: `boards.txt` lists
`esp32.menu.FlashMode.qio` and `esp32.menu.FlashFreq.80` first, so
`build.boot=qio` / 80MHz are the defaults, and `platform.txt`'s
`recipe.hooks.prebuild.4` turns `bin/bootloader_{build.boot}_{build.boot_freq}.elf`
into `CAL.ino.bootloader.bin`. All four variants are built from one source tree
with one `sdkconfig`; only flash timing differs.

That ELF was disassembled with the core's own
`xtensa-esp32-elf-objdump`. Line numbers resolve to
`esp-idf/components/bootloader_support/src/bootloader_utility.c`, so what follows
is the compiled control flow of the code that runs on the fleet, not a recollection
of upstream.

### 1.2 The selection path, decoded

`bootloader_utility_get_selected_boot_partition` (`bootloader_utility.c:380`,
symbol at `0x40079b50`):

| Condition, as compiled | Result |
|---|---|
| `bs->ota_info.offset == 0` (no `otadata` partition) | returns `FACTORY_INDEX` |
| `bootloader_common_read_otadata` fails | returns `INVALID_INDEX` (`-99`) |
| both `otadata` entries invalid **and** `bs->factory.offset != 0` | returns `FACTORY_INDEX` |
| an entry is active | `boot_index = (ota_seq - 1) % bs->app_count` |

Two details from the disassembly that matter later:

- `bs->app_count` is read at `l32i a8, a2, 152`, which pins the
  `bootloader_state_t` layout (`ota_info` 0, `factory` 8, `test` 16, `ota[16]`
  24…152, `app_count` 152). **On this table `app_count` is 1**, so
  `(ota_seq - 1) % 1` is always `0` — `ota_0`. There is no second slot to land on
  by arithmetic accident.
- `bootloader_common_ota_select_invalid` (`bootloader_common_loader.c:76`,
  `0x400791dc`) compiles to
  `seq == UINT32_MAX || (ota_state - 3) < 2` — i.e. an entry is invalid when its
  sequence is blank **or its state is `ESP_OTA_IMG_INVALID` (3) or
  `ESP_OTA_IMG_ABORTED` (4)**. That is the hinge of §1.5.

### 1.3 The fallthrough itself

`bootloader_utility_load_boot_image` (`bootloader_utility.c:580–629`,
`0x40079ca4`) compiles to two loops around `try_load_partition`:

```
0x40079d1c:  bgei a7, -1, 0x40079cea     ; backwards loop: index >= FACTORY_INDEX
0x40079cea:    index_to_partition(bs, index)
               if (size == 0) continue
               if (try_load_partition(&part, &image)) {
                   set_actual_ota_seq(bs, index); load_image(&image);   ; BOOTS
               }
               log_invalid_app_partition(index)
0x40079d1a:    index--
0x40079d1f:  addi a3, a3, 1              ; forward loop: index = start_index + 1
0x40079d25:  l32i a8, a2, 152            ; while (index < bs->app_count)
0x40079d28:  blt  a3, a8, 0x40079d5a     ;   same body, same load_image
0x40079d2b:  try_load_partition(&bs->test, &image)   ; bs->test.size == 0 here
0x40079d37:  "No bootable app partitions in the partition table" ; then bootloader_reset()
```

With `start_index == FACTORY_INDEX` (`-1`) and `factory` invalid:

1. the backwards loop tries `factory`, `try_load_partition` fails, and
   `log_invalid_app_partition(-1)` prints its `FACTORY_INDEX` arm;
2. `index--` makes it `-2`, `bgei a7, -1` fails, the loop exits;
3. the forward loop starts at `start_index + 1 == 0`, which
   `index_to_partition` resolves to `bs->ota[0]` — **`ota_0`** — and boots it if
   its image verifies.

`index_to_partition` (`0x40079d84`) was decoded to confirm the mapping:
`index == -1` → `bs+8` (`factory`), `index == -2` → `bs+16` (`test`), otherwise
bounds-checked `bs->ota[index]` at `bs+24+8*index`.

`try_load_partition` (`bootloader_utility.c:474–487`) is
`part->size != 0 && bootloader_load_image(part, data) == ESP_OK`. There is no
skip-validation shortcut in this build:
`CONFIG_BOOTLOADER_SKIP_VALIDATE_ALWAYS` and
`CONFIG_BOOTLOADER_SKIP_VALIDATE_ON_POWER_ON` are both *not set* in the pinned
`sdkconfig` (only `..._IN_DEEP_SLEEP=y`, which this firmware never exercises).
Validation on a cold boot is therefore real, which is the precondition for any
fallthrough at all — a bootloader that skipped validation would jump into
garbage instead of moving on.

### 1.4 What Brett will see on serial, exactly

The log strings were extracted from `.dram0.rodata` at the addresses the
disassembly loads. `CONFIG_BOOTLOADER_LOG_LEVEL=1` (ERROR only), so these three
are the ones that survive into the shipped binary, and they are the whole
observable surface of the test in §2:

```
3fff01df   "E (%lu) %s: Factory app partition%s"
3fff022e   "E (%lu) %s: OTA app partition slot %d%s"
3fff01ce   " is not bootable"
3fff04c4   "E (%lu) %s: No bootable app partitions in the partition table"
```

with `%s: ` filled by the tag `boot`. So a corrupt `factory` announces itself as
`E (…) boot: Factory app partition is not bootable`, and the terminal failure is
`E (…) boot: No bootable app partitions in the partition table`.

### 1.5 Three further findings, none of which were being looked for

These came out of the same reading and they change the design more than the
yes/no does.

**(a) `esp_ota_set_boot_partition()` validates before it commits.** Disassembled
from `lib/libapp_update.a`, `esp_ota_ops.c:600–640`: the function verifies the
target image (`esp_image_verify`) *before* touching `otadata`, rejects a
non-`APP` partition with `ESP_ERR_INVALID_ARG`, and for
`ESP_PARTITION_SUBTYPE_APP_FACTORY` (subtype 0) takes a separate branch that
erases the `otadata` partition rather than writing a sequence number.

The consequence for `App/Loader.cpp` is worth stating plainly, because it is a
safety net nobody designed: `bootFactoryAndRestart()` ignores the return value
and reboots regardless — but if `factory` is corrupt at that moment the call
**fails and `otadata` is left alone**, still naming `ota_0`. The device reboots
into the App rather than into nothing. The App's handback is self-blocking
against a broken `factory`.

`Updater::bootApplication()` does check the return value and journals
`esp_ota_set_boot_partition failed (esp_err %d)`, so the same guard is already
visible in CAL's log on the other side of the handover.

**(b) Arduino's `Update` library refuses to write the running partition.**
`libraries/Update/src/Updater.cpp:236–237`:

```cpp
_partition = esp_ota_get_next_update_partition(NULL);
if (!_partition || _partition == esp_ota_get_running_partition()) {   /* error */ }
```

On a one-OTA-slot table `esp_ota_get_next_update_partition` returns `ota_0`, so
an image running *from* `ota_0` cannot use `Update` to overwrite itself. This
matters for §5.4. It also means `Update` can never be used to write `factory` at
all — `esp_ota_get_next_update_partition` never returns a factory partition — so
the CAL writer is necessarily raw `esp_partition_erase_range` /
`esp_partition_write`, not the `Update` class CAL already uses for the App.

**(c) Rollback is already compiled in and already armed.** The pinned
`sdkconfig` has `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` and
`CONFIG_APP_ROLLBACK_ENABLE=y`. In `bootloader_utility.c:397–402` the bootloader
rewrites any `otadata` entry in `ESP_OTA_IMG_PENDING_VERIFY` (1) to
`ESP_OTA_IMG_ABORTED` (4), and at line 449 it promotes `ESP_OTA_IMG_NEW` (0) to
`PENDING_VERIFY`. Combined with §1.2's finding that `ABORTED` counts as
*invalid*: **an image that boots once and does not confirm itself is, on the next
boot, rolled back — and on this table "rolled back" means `FACTORY_INDEX`.**

The core marks images valid automatically:
`cores/esp32/esp32-hal-misc.c:314–326` calls
`esp_ota_mark_app_valid_cancel_rollback()` from `initArduino()` whenever the
running partition is in `PENDING_VERIFY`, unless the sketch overrides the weak
`verifyRollbackLater()` (default `false`). Neither CAL nor App overrides it
today — checked, no hits. §5.4 proposes that CAL start overriding it, because
that hook is a free automatic rollback and it is currently being spent on
nothing.

### 1.6 The honest version of the answer

The suggestion framed this as *if yes the feature is safe, if no it is not*.
Having read the code, that framing is too coarse in both directions.

**The feature's safety does not rest on the fallthrough.** In every staging
design in §5, `otadata` points at the partition that is *not* being written for
the whole duration of the write, so the bootloader's first choice is already the
valid image and the fallthrough is never consulted. The fallthrough matters in
exactly one place — §5.4's deliberate-rollback path, where a failed copy leaves
`factory` invalid *and* the boot pointer aimed back at it. That is a real
dependency, and it is why the bench test is still required rather than merely
nice.

**What would actually brick a unit is a different condition:** both app
partitions invalid at once. Then `try_load_partition` fails twice, `bs->test.size`
is `0`, and the bootloader prints
`No bootable app partitions in the partition table` and calls
`bootloader_reset()` — a reboot loop with no exit but a cable. Every design below
is therefore judged on one invariant:

> **At every instant, at least one app partition holds a fully written,
> verified, bootable image, and the boot pointer names it.**

Not "the window is short". The window's length is not the property that saves a
household unit; the invariant is.

---

## 2. Bench procedure — settling the fallthrough on real hardware

**Required deliverable. Ten minutes, one bench unit, one USB cable.** Do not
skip it because §1 says yes; §1 says what the binary says, and this project's own
history is the argument for the difference.

Two tests. **Test A** answers the blocking question. **Test B** answers a second
question that §5.4 depends on just as heavily and that nobody has ever
exercised: *does a CAL image execute correctly from `ota_0`?*

### 2.0 Before touching anything — take a backup

Everything here is reversible, but only if the original bytes exist somewhere.

```
esptool --chip esp32 --port COM5 read_flash 0x0 0x400000 bench-backup-full.bin
```

One 4MB file, five minutes, and it restores the unit to exactly its present
state including `nvs` (so its secret and its remembered WiFi survive). Keep it
until the unit is back in service.

Also capture the starting state of the boot pointer and the first sector of
`factory`, since the whole test is about changing them:

```
esptool --chip esp32 --port COM5 read_flash 0xE000  0x2000 bench-otadata.bin
esptool --chip esp32 --port COM5 read_flash 0x10000 0x1000 bench-factory-sector0.bin
```

`bench-factory-sector0.bin` should begin with `E9` — the ESP32 image magic byte.
Confirm that before proceeding; if it does not, this unit is not in the state the
test assumes and nothing below will mean what it is supposed to mean.

### 2.1 Test A — corrupt `factory`, observe what boots

**Preconditions.** The unit must have a working App installed in `ota_0` (let it
boot normally and reach cards once, so `ota_0` is known-good). Serial monitor
attached at **115200**, and started *before* the reset, or the bootloader's
four lines are gone before the terminal opens.

**Step 1 — aim the boot pointer at `factory`.** Erasing `otadata` is what makes
`bootloader_utility_get_selected_boot_partition` return `FACTORY_INDEX`
(§1.2). Without this the bootloader would boot `ota_0` directly and the test
would prove nothing.

```
esptool --chip esp32 --port COM5 erase_region 0xE000 0x2000
```

**Step 2 — power-cycle and confirm the baseline.** Serial should show CAL
starting (its splash, then its journal lines). This proves the boot pointer is
now on `factory` and that `factory` is *currently* bootable. **If CAL does not
come up here, stop** — the rest of the test is uninterpretable.

**Step 3 — corrupt the first sector of `factory`.** The first sector holds the
image header (magic `0xE9`, segment count, entry point) and the first segment
header, so erasing it guarantees `bootloader_load_image` rejects the image. It
is also the cheapest thing to repair in §2.3.

```
esptool --chip esp32 --port COM5 erase_region 0x10000 0x1000
```

**Step 4 — power-cycle with the serial monitor already running, and read the
first four lines.** Exactly one of these three outcomes occurs.

| What serial shows | What it means |
|---|---|
| `E (…) boot: Factory app partition is not bootable` **followed by the App starting** | **YES — the fallthrough is real.** §1 is confirmed on hardware and §5.4's rollback path has its backstop. |
| `Factory app partition is not bootable` then `OTA app partition slot 0 is not bootable` then `No bootable app partitions in the partition table`, looping | The fallthrough did **not** happen, *or* `ota_0` was not actually good. Re-check the precondition before believing the first reading. |
| No `Factory app partition` line at all, and the App starts immediately | `otadata` was not blank — step 1 did not take, or something rewrote it. Repeat from step 1; the result so far is void. |

**Step 5 — check whether the fallthrough is sticky.** If outcome 1 occurred,
read `otadata` back:

```
esptool --chip esp32 --port COM5 read_flash 0xE000 0x2000 after-fallthrough.bin
```

`set_actual_ota_seq` (`bootloader_utility.c:492–515`) writes `otadata` with
`seq = index + 1` and `ota_state = ESP_OTA_IMG_VALID` (2) **when the previous
contents were blank** — which they are here. So the prediction is that
`after-fallthrough.bin` is no longer `FF`-filled, and that a *second* power-cycle
goes straight to the App with **no** `Factory app partition` line. Confirming
that matters: it means a fallthrough silently re-points the boot pointer, so a
unit that has fallen through once will not complain again, and damage to
`factory` can sit undetected until something asks for it.

### 2.2 Test B — does CAL run from `ota_0`?

§5.4 stakes the whole remote path on an ESP32 app image being
partition-agnostic. The inference is strong — that property is exactly what makes
conventional dual-slot OTA work, and in this repository `CAL.ino.bin` and
`App.ino.bin` are built by one toolchain with one linker script yet execute
correctly at `0x10000` and `0x1B0000` respectively. **It has still never been
observed for CAL specifically, and it is cheap to observe.**

Write a CAL image into `ota_0` — note the offset is `0x1B0000` on the current
table and `0x170000` on the old one, so read it off §3.1's decode for the unit in
hand rather than typing it from memory:

```
esptool --chip esp32 --port COM5 write_flash 0x1B0000 CAL.ino.bin
```

Then point the boot pointer at `ota_0`. Crafting an `otadata` entry by hand means
getting its CRC right, so the pragmatic bench route is to **combine this with
Test A**: leave `factory` corrupt from §2.1 step 3, and let the fallthrough
deliver control to `ota_0` — which now holds **CAL**, not the App. One power
cycle answers both questions at once.

**What to look for:** CAL's own splash and its journal lines, from a unit whose
`factory` is broken. If CAL renders and reaches its ladder, the image is
offset-independent and §5.4 is viable. If it panics, reboots, or shows a
scrambled panel, §5.4 is dead and §5.3 (SD) becomes the only remote path — so
this test is a go/no-go, not a curiosity.

While CAL is running from `ota_0`, one behaviour is worth watching for
deliberately: CAL will find no bootable App and try to install one. §1.5(b) says
`Update.begin()` must refuse, because the target and the running partition are
the same. Expect a journal line from `Updater::installApplication` reporting
`Update.begin(...) failed` — and **note that CAL currently presents that as
`Update failed` on the panel with no idea why**, which is precisely the interlock
§5.4 has to add.

### 2.3 Restoring the unit

Repair is a single write — the image is intact apart from the sector erased in
step 3, and `CAL.ino.bin` re-lays the whole thing anyway:

```
esptool --chip esp32 --port COM5 write_flash 0x10000 CAL.ino.bin
esptool --chip esp32 --port COM5 erase_region 0xE000 0x2000
```

If Test B was run, also put the App back where it belongs, because `ota_0` now
holds a copy of CAL:

```
esptool --chip esp32 --port COM5 erase_region 0x1B0000 0x200000
```

and let CAL re-download the App on its own — `Updater::installApplication`
resolves `ota_0`'s real offset itself, which is the only correct way to install
the App and the reason the README forbids `write_flash`-ing `App.ino.merged.bin`
by hand.

`nvs` is untouched throughout, so **no `/diag/deviceregistry` re-registration is
needed** and the unit keeps its secret and its WiFi. That is the one respect in
which this procedure is gentler than `RETABLE_RUNBOOK.md`'s. If anything goes
sideways, `bench-backup-full.bin` from §2.0 restores the whole chip:

```
esptool --chip esp32 --port COM5 write_flash 0x0 bench-backup-full.bin
```

---

## 3. Sizes: NOT re-compiled by this pass

`partitions.csv` says *"Re-measure both figures before trusting this table
again"*, and this pass did not re-compile. The build was started and then stopped
on instruction: this machine has 2 cores and ~3.9GB of RAM, a second agent needs
the toolchain to verify its own change, and this project has a recorded history
of agents dying at exit 127 — a memory abort that reads as a code failure — when
free memory approaches 130MB. Both compiles are deferred rather than abandoned.

### 3.1 What could be measured without a compiler, and was

**The table this branch's `partitions.csv` actually produces.** The core ships
`tools/gen_esp32part.exe`, so the CSV can be turned into its binary table and the
binary decoded — the same byte-for-byte check `ci/build-firmware.sh` describes as
a one-time manual diagnosis, done here from the CSV rather than from a compiled
image. Generated from this branch's root `partitions.csv` and decoded entry by
entry (magic `0xAA50`, 32-byte records):

```
NAME       TYPE  SUBTYPE  OFFSET     SIZE         END
nvs        data  0x02     0x009000        20480   0x00e000
otadata    data  0x00     0x00e000         8192   0x010000
factory    app   0x00     0x010000      1703936   0x1b0000
ota_0      app   0x10     0x1b0000      2097152   0x3b0000
callog     data  0x99     0x3b0000        65536   0x3c0000
spiffs     data  0x82     0x3c0000       196608   0x3f0000
coredump   data  0x03     0x3f0000        65536   0x400000
md5 checksum entry present
highest end: 0x400000 = 4194304 bytes
```

**Measured, this pass.** The table in §4 is what the CSV really encodes, it fills
4MB exactly, and the MD5 entry `partitions.csv` warns about is present. Two
details confirm §1's reading: `factory` carries subtype `0x00`, which is the
value `esp_ota_set_boot_partition` branches on to erase `otadata` (§1.5(a)), and
`ota_0` carries `0x10`, which is the single entry `bs->ota[]` that makes
`app_count == 1` (§1.2).

**Artifacts already on disk, with honest provenance.** The main working tree
holds exported binaries from earlier builds. These were **not produced by this
pass** and their tree state at build time cannot be proven, so they are evidence,
not measurements:

| Artifact | `stat` size | mtime | What can be established |
|---|---:|---|---|
| `CAL/build/.../CAL.ino.bin` | 1,337,360 | 09-15 00:26 | matches the figure `partitions.csv` and `README.md` both record for the with-journal build; no commit since has touched a CAL root source (`51f3883` at 00:32 was the last, and `370a207`/`785d442`/`6166fb7`/`fa8d06a` touched only `partitions.csv`, the runbook, `App/` and `ci/`) |
| `CAL/App/build/.../App.ino.bin` | 1,489,744 | 09-16 07:03 | six minutes before `fa8d06a` was committed at 07:09 |

The CAL artifact's own `CAL.ino.partitions.bin` decodes to `factory` 1,441,792 at
`0x170000` and `ota_0` 2,359,296 — the **pre-rebalance** table — which
independently dates that build to before `370a207` and corroborates the
timeline above. It does not affect the image size: an app image does not depend
on the table it is flashed beside. It also confirms `build-firmware.sh`'s claim
that a sketch-root `partitions.csv` overrides the FQBN's `min_spiffs` scheme —
the baked table is this project's asymmetric one, not the stock scheme named on
the command line.

### 3.2 Every figure in circulation

| Figure | Value | Provenance | Status |
|---|---:|---|---|
| `CAL.ino.bin` | 1,337,360 | on-disk artifact; `partitions.csv`; `README.md` | three sources agree, **not re-compiled** |
| `App.ino.bin` | 1,489,744 | on-disk artifact, `stat`-ed this pass | **not re-compiled** |
| `App.ino.bin` | 1,489,587 | reported at `fa8d06a` | 157 bytes off the artifact |
| `App.ino.bin` | 1,484,976 | `partitions.csv` | superseded |
| `App.ino.bin` | 1,474,960 | `README.md` | superseded |
| CAL | 1,318,891 | the suggestion | **superseded — do not use** |
| App | 1,445,152 | the suggestion | **superseded — do not use** |

Four live figures for one binary. §4's arithmetic uses **1,489,744**, because it
is the only one this pass observed directly, and the design is written so that
**no conclusion depends on which is right**: §5.2's rejection holds by more than
400KB and §5.4 needs only that a CAL image fits in `ota_0`, which it does by
three-quarters of a megabyte. The entire spread between the four App figures is
14,784 bytes.

When the compiler is free, the numbers to capture are `stat -c%s` on
`build/esp32.esp32.esp32/CAL.ino.bin` and
`App/build/esp32.esp32.esp32/App.ino.bin`, which is exactly what
`ci/build-firmware.sh`'s own `check_size` reads.

### 3.3 What `build-firmware.sh` already gets right, and should keep

Two things in that script are load-bearing for this feature and must not be
lost when it grows a CAL-image step.

`arduino-cli`'s `Sketch uses X of Y bytes` line reports against the FQBN's
static `min_spiffs` memory map (1,966,080 bytes), **not** against this project's
asymmetric table — so a build that has genuinely outgrown `factory` still reports
"fits". The script's `partition_size()` parses the 5th comma-separated field of
`partitions.csv` and `check_size()` compares `stat -c%s` of the real `.bin`
against it. That is the only ceiling check that means anything, and OTA-CAL adds
a new reason to trust it: the *server* will be handing devices a CAL image, and
`Updater`'s existing `manifest.sizeBytes > target->size` guard is a
belt-and-braces check against a mismatched table, not a substitute for refusing
to publish an oversized image in the first place.

The `diff -q partitions.csv App/partitions.csv` gate stays essential for the
same reason it was added: `App.ino.merged.bin` bakes its table in at `0x8000`. As
of this branch the two root tables agree, and `SelfTest/partitions.csv` is **two
revisions stale** — `factory` at `0x160000`, `ota_0` at `0x250000`, no `callog`.
The gate does not cover it. That is out of scope here but it is the same class of
defect the gate exists to catch, and `SelfTest` publishes artifacts too.

---

## 4. Where a CAL image could be staged — the arithmetic

The table as it stands on this branch, from `partitions.csv`:

| Name | Offset | Size (hex) | Size | Holds |
|---|---|---|---:|---|
| `nvs` | `0x9000` | `0x5000` | 20,480 | secret, WiFi, flags |
| `otadata` | `0xE000` | `0x2000` | 8,192 | which app boots |
| `factory` | `0x10000` | `0x1A0000` | 1,703,936 | **CAL** |
| `ota_0` | `0x1B0000` | `0x200000` | 2,097,152 | the App |
| `callog` | `0x3B0000` | `0x10000` | 65,536 | CAL's boot journal |
| `spiffs` | `0x3C0000` | `0x30000` | 196,608 | brand splash (LittleFS) |
| `coredump` | `0x3F0000` | `0x10000` | 65,536 | panic backtraces |

Fills 4,194,304 exactly. Non-app bytes, including the 36,864 before `nvs` that
hold the bootloader and the table: 393,216. **Everything available to app
partitions, forever, on this hardware: 3,801,088 bytes.**

### 4.1 Free space in `ota_0` today

```
ota_0                          2,097,152
App.ino.bin (§3.1, on disk)    1,489,744
                               ---------
slack                            607,408
```

The suggestion said ~980KB, which was measured against an `ota_0` of 2,424,832 —
a size that stopped being true when `callog` was carved out of it and again at
the 2026-09-15 rebalance. The real figure is **607,408 bytes**, about 40% less.
Picking any of §3.2's other App figures moves this by at most 14,784 bytes.

### 4.2 What a staging partition would have to be

A staged CAL image must be stored whole before it is trusted. As a `data`
partition it needs 4KB alignment, not the 64KB an app partition needs:

```
ceil(1,337,360 / 4,096) = 327 sectors  =  1,339,392 bytes   (0x147000)
```

`607,408 < 1,339,392`. **Short by 731,984 bytes** — the slack is not half of
what is needed.

### 4.3 And there is no rearrangement that fixes it

Ask instead for the best case: zero growth headroom anywhere, every app
partition cut to exactly what it holds.

```
factory  ceil(1,337,360 / 65,536) = 21 × 65,536 = 1,376,256
ota_0    ceil(1,489,744 / 65,536) = 23 × 65,536 = 1,507,328
stage                                             1,339,392
                                                 ----------
                                                  4,222,976
available to app partitions                       3,801,088
                                                 ----------
short by                                            421,888
```

Delete the boot journal, the brand cache and the coredump area as well — all
327,680 bytes of them — and the app budget rises to 4,128,768: **still short by
94,208 bytes.**

> **There is no 4MB layout that holds CAL, the App and a CAL-sized staging area
> at once** — not with zero headroom on either app, and not after deleting the
> boot journal, the brand splash and the panic backtraces.

That is the end of the "carve a staging partition" idea, and it is the end of it
independently of which App figure from §3 is correct: the shortfall exceeds the
entire spread between them by two orders of magnitude.

---

## 5. The four candidates

### 5.1 Rejected — in-place write of `factory` from the App

The App downloads ~1.34MB over TLS and writes it straight into `factory` as it
arrives, the way `Updater::installApplication` writes `ota_0` today.

It satisfies §1.6's invariant: the App is running from `ota_0`, so `otadata`
names `ota_0` throughout, and the surviving image is both valid and selected.
A power cut leaves the device booting the App, exactly as the suggestion hoped —
and, per §1.6, without needing the fallthrough at all.

**Why it is still the wrong choice.** The invariant asks that a valid image
survive; recovery asks something more, namely that *the survivor can repair the
damage*. Here the survivor is the App, and for it to retry it needs: a network,
a TLS session, the server, its device secret accepted, and — the part this
project has already been burned by — enough **contiguous** heap.

`App/SdStorage.cpp` records the measurement: mounting the SD card costs ~67KB of
contiguous 8-bit heap, held for the life of the process, and on device 17 it took
the largest free block from 77,812 to 10,228 bytes. `Http.h`'s
`kTlsRecordBufferBytes` is 16,717, needed twice. A card-equipped device was below
the floor for a *new* TLS session from the instant it booted, and devices 12 and
17 sat unreachable for hours in exactly that state on 2026-09-11. `kMaxOpenFiles`
was cut from 5 to 1 to claw the cost back and **the recovered figure has not been
measured on hardware** — `SdStorage.cpp` says so itself.

So this option asks the fleet's recovery mechanism to depend, at its most
fragile moment, on the one resource whose exhaustion caused the incident that
motivated half of CAL's current instrumentation. And it does so while `factory`
is invalid, i.e. while the fallback that normally rescues a wedged App is the
thing being rewritten. If the App crash-loops in that window, nothing else runs:
the boot-attempt ceiling that treats a bad App as bad lives in
`Updater::haveBootableApplication()`, in CAL, which is gone.

Rejected. Not because the window is long, but because the survivor is the weakest
component in the system at the moment it is asked to be the strongest.

*(One thing this option does have going for it, kept for the record: it needs no
partition change and no new server concepts beyond `Kind=Cal`. If §5.4's Test B
fails on hardware, this becomes the fallback candidate, with the SD path of §5.3
as its bench harness.)*

### 5.2 Rejected — carve a staging partition out of `ota_0`

Arithmetically impossible: §4.2 and §4.3. Short by 731,984 bytes against today's
slack; short by 421,888 against the entire flash with zero headroom; short by
94,208 even after deleting `callog`, `spiffs` and `coredump`.

Worth naming what it would have cost even if it had fitted, because it is the
reason not to go looking for 94,208 bytes: a new partition means a new table,
a table change means USB, and USB means a visit to every unit — the thing
`RETABLE_RUNBOOK.md` exists to organise and the thing this feature exists to stop
needing. Spending a mandatory truck roll to buy a *safer* way of avoiding truck
rolls is a poor trade even when the arithmetic allows it.

### 5.3 Accepted, as a second path — stage on the SD card

Brett asked for this specifically ("also want to test cal off a SD"), and it is a
better idea than it first looks, for a reason that has nothing to do with saving
flash.

**Can the App write a CAL image to SD?** Yes, and not speculatively —
`App/Assets.cpp` already implements this exact pipeline for brand assets: TLS,
authenticated GET, a streamed `mbedtls_sha256` running alongside the write, a
512-byte buffer, a `.part` temporary name renamed only on success, and an idle
timeout that distinguishes *slow* from *stalled*. 1.34MB is nothing on a
multi-gigabyte card.

**Can it verify SHA-256?** Yes — and `Assets.cpp` goes further in a way that is
directly relevant. After hashing the bytes as they arrive from the network, it
**re-opens the file and re-hashes it from the card**, because a live bug proved
that `SD.write()` can return success having stored different bytes:

> *"storage corrupted what was written (network side verified fine, re-read from
> SD did not) - this card may be failing"*

Any CAL image staged on a card must therefore be re-hashed **from the card,
immediately before the copy into `factory`** — not merely at download time. A
card that silently altered an asset produced a picture that would not decode; the
same card altering CAL produces a device that will not boot.

**Can it copy SD → `factory` without holding the image in RAM?** Yes: read 4KB
from the file, `esp_partition_write` 4KB, repeat. Constant memory, no large
allocation. This must **not** reuse `Display.cpp`'s `readFileToBuffer()`, which
pulls whole files into a contiguous 10–24KB allocation — that allocation is the
one that was failing in the field.

`esp_partition_write` permits it. Disassembled from `lib/libesp_partition.a`
(`partition_target.c:65–89`), its only per-partition refusal is the `readonly`
flag at struct offset 42 — set from a CSV `R` flag, and this table sets no flags
— plus bounds checks. There is no app/data type restriction and no
running-partition check. The address-range protection from
`CONFIG_SPI_FLASH_DANGEROUS_WRITE_ABORTS=y` covers the bootloader and the
partition table (which is why `partitions.csv` is right that writing `0x8000`
panics); `factory` at `0x10000` is outside it. And the capability is already
demonstrated daily in a different direction: CAL writes ~1.49MB into `ota_0`
through this same stack on every App update. **The `factory`-specific case is
still unverified on hardware** — it is what §2 Test B's aftermath and the first
bench run of the writer will settle.

**Power loss, phase by phase.** Download interrupted → a `.part` file, discarded
on the next attempt, `factory` never touched. Copy interrupted → `factory`
invalid, `otadata` still names `ota_0`, the App boots and **retries the copy from
the card with no network at all**. That last property is genuinely good: unlike
§5.1, the survivor needs nothing but power and a card.

**Does it remove the need for a partition change?** Yes, completely.

**Why it is not the fleet path.** Three reasons, in order of weight:

1. **Most of the fleet may have no card.** `SdStorage.h` is explicit that a
   device with an empty slot is *"a completely ordinary state, not a fault"*.
   A recovery mechanism that only works on some units is not a fleet mechanism.
2. **It puts the download back inside the heap window of §5.1.** Getting the
   image onto the card is still a 1.34MB TLS transfer by the App with the card
   mounted — the 2026-09-11 configuration exactly. SD staging shortens the
   *dangerous* window but not the *fragile* one.
3. **The card is unproven hardware.** `SdStorage.h`: *"UNVERIFIED ON HARDWARE.
   Nobody has put a card in that slot."* And `Assets.cpp` has already caught a
   card corrupting data. Putting the recovery image's integrity behind a
   component with that record needs the §5.3 re-hash to be treated as mandatory,
   not defensive.

**What it is the best available tool for, and should ship as:**

- **The bench harness for the writer.** The SD→`factory` copy is the same code
  as §5.4's `ota_0`→`factory` copy with a different source. Testing it off a card
  needs no server, no `Kind=Cal`, no manifest and no network — put `CAL.ino.bin`
  and its SHA-256 on a card, boot, watch. That is the cheapest possible way to
  earn confidence in the one operation that can brick a unit, and it is available
  before any server work exists.
- **A field recovery path with no cable.** A unit whose network is gone, or whose
  server credentials are rejected, but whose card slot a person can reach, can
  take a new CAL from a card. That is strictly more than is possible today and it
  costs one source implementation.

So: **build it, ship it, use it first — and do not make the fleet depend on it.**

### 5.4 Recommended for the fleet path — the `ota_0` trampoline

The insight §4 keeps pointing at: `ota_0` is already large enough to hold a CAL
image — 2,097,152 against 1,337,360, with 759,792 bytes spare. What it cannot do
is hold the App **and** a CAL image at the same time. It does not have to. It can
hold them one after the other.

And §1.5(b)'s constraint turns out to be the enabling fact rather than an
obstacle: an app image on ESP32 is partition-agnostic (which is why conventional
dual-slot OTA works with one binary, and why this repo's own `CAL.ino.bin` and
`App.ino.bin` execute from `0x10000` and `0x1B0000` while being built by one
toolchain with one linker script). **So CAL can run from `ota_0`, and CAL running
from `ota_0` can write `factory`.** Nothing has to overwrite the partition it is
executing from at any point.

**This is the linchpin and it is unverified.** §2 Test B is a go/no-go on it.

#### The sequence

Phases are named by what they are *about to do*, and the phase marker lives in
`nvs` alongside a `{version, sha256, sizeBytes}` record of the staged image.

**Phase 0 — the App notices (App, from `ota_0`).** Only the App runs on a healthy
device; CAL is not consulted on an ordinary boot, because
`CAL.ino`'s `mustContactServer()` hands straight over when a bootable App is
installed. So the App must be the one that learns a newer CAL is current, set
`calUpdateRequested`, and hand back — reusing `Loader::requestUpdate()`'s exact
shape, which already sets a flag and reboots into `factory`.

**Phase 1 — download the candidate into `ota_0` (CAL, from `factory`).** Set the
phase marker *before* the download. Fetch the CAL image with the machinery
`Updater::installApplication` already has — streamed SHA-256, size checked
against the real partition, verified before commit, `Update.end(true)`. This is
CAL's daily job and it runs with a clean heap: **CAL does not mount the SD card
at all** (no `SD.h`, no `SdStorage` — checked, zero hits in CAL), so it never
pays the ~67KB contiguous cost that §5.1 turns on. Then `esp_ota_set_boot_partition(ota_0)`
and restart.

*A note on bookkeeping:* `Identity::setInstalledAppVersion()` must **not** be
given the CAL version here. The App-version field is what
`haveBootableApplication()` reads, and lying to it would make CAL hand over to a
CAL image believing it was the App. The staged-CAL record is a separate key, and
the App version must be cleared so phase 3 knows to re-download.

**Phase 2 — copy `ota_0` → `factory` (candidate CAL, from `ota_0`).** The
candidate is now executing. It detects
`esp_ota_get_running_partition()->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0`,
which is the interlock: **in this state CAL does exactly one thing and nothing
else** — no App install, no provisioning portal, no handover. Then:

1. re-hash its own running image out of `ota_0` via `esp_partition_read` and
   compare against the staged record, so the bytes about to be copied are proven
   to be the bytes that were downloaded;
2. `esp_partition_erase_range(factory, 0, image_size rounded up)`;
3. copy in 4KB chunks, `esp_partition_read` → `esp_partition_write`, hashing as
   it goes;
4. read `factory` back and hash it — verifying the destination, not the source,
   because §5.3's card bug is the same class of failure a marginal flash sector
   would produce;
5. `esp_ota_set_boot_partition(factory)`, which per §1.5(a) **independently
   validates the image before erasing `otadata`** and refuses if the copy is bad.
   That is a second, free verification by the library that decides what boots;
6. advance the phase marker and restart.

**Phase 3 — reinstall the App (new CAL, from `factory`).** `ota_0` holds a CAL
image, not the App, and the App version was cleared in phase 1. So
`haveBootableApplication()` correctly says no, and CAL's existing
"no application installed" path downloads the App into `ota_0`. Clear the phase
marker. Done.

#### Why this is the safest of the four

**The invariant of §1.6 holds by construction, not by luck.** Exactly one app
partition is ever under the write head, and the other always holds a fully
written, verified image that the boot pointer already names:

| Phase | `factory` | `ota_0` | `otadata` names | If power is cut |
|---|---|---|---|---|
| 1 | valid (old CAL) | being written | `factory` | boots old CAL, restarts the download |
| 2 | being written | valid (candidate CAL) | `ota_0` | **boots the candidate, retries the copy** |
| 3 | valid (new CAL) | being written | `factory` | boots new CAL, restarts the App download |

**The phase-2 survivor needs nothing but power.** No network, no TLS, no server,
no card, no heap headroom — a local flash-to-flash copy of an image already
verified twice. That is the property §5.1 cannot offer and the reason to prefer
this over it. The comparison is not "shorter window" but "the thing that survives
can finish the job on its own".

**Phase 2 is a free smoke test.** A candidate CAL that cannot boot never reaches
step 1 of the copy, so `factory` is never touched by a CAL that does not run. A
candidate that panics on startup is caught *before* it becomes the recovery
image — which is more than the USB path offers today.

**And §1.5(c) makes that smoke test automatic.** Rollback is already compiled in:
`esp_ota_set_boot_partition(ota_0)` leaves the entry `ESP_OTA_IMG_NEW`, the
bootloader promotes it to `PENDING_VERIFY`, and a second boot without
confirmation rewrites it to `ABORTED` — which §1.2 shows counts as *invalid*, so
both entries are invalid and selection returns `FACTORY_INDEX`. **On this table,
rollback means "boot the old CAL in `factory`".** Exactly the right target,
already built, currently spent on nothing.

To use it, **CAL should override the weak `verifyRollbackLater()` to return
`true`** and mark itself valid explicitly, rather than letting
`initArduino()` confirm the image before phase 2 has proved anything. Then:

- candidate crashes before reaching phase 2 → next boot rolls back to old CAL,
  `factory` untouched, device fine, and the failure is reportable;
- candidate reaches phase 2 and the copy verifies → commit and advance;
- candidate reaches phase 2 and the copy keeps failing → retry in-process to a
  bounded count, then **deliberately reboot to trigger the rollback**. If
  `factory` was already erased, this is the one place the design genuinely leans
  on §1's fallthrough: `factory` is invalid *and* selected, and the bootloader's
  forward loop must deliver control back to the candidate in `ota_0` so it can
  keep trying. **This is why §2 Test A is required** rather than optional.

`verifyRollbackLater()` is weak per sketch, so overriding it in CAL leaves the
App's behaviour alone. Note that a `factory` image is never in `PENDING_VERIFY`
— the `FACTORY_INDEX` path has no `otadata` at all, and `set_actual_ota_seq`
writes `VALID` — so the hook is a no-op on an ordinary CAL boot.

**It needs no partition-table change.** Which means OTA-CAL does **not** have to
ride the single planned USB pass for layout reasons, and the arithmetic is
table-agnostic: the writer resolves `factory` and `ota_0` by subtype at runtime
and checks sizes, the way `installApplication` already checks
`manifest.sizeBytes > target->size`. On the *old* table it still works —
`ota_0` was 2,424,832 and `factory` 1,441,792, both larger than a 1,337,360-byte
CAL, though `factory`'s margin there is only 104,432 bytes and shrinking.

#### What it costs

- **The App is destroyed and re-downloaded.** One extra ~1.49MB download per CAL
  update, and a household watching a panel run CAL's screens through two
  downloads and three reboots. CAL updates are rare; this is the right thing to
  spend. It does need a screen that says what is happening, because "showing CAL
  for several minutes" is otherwise indistinguishable from a fault.
- **Three reboots and a persistent state machine**, which is new complexity in
  the component whose whole design principle is to do as little as possible. The
  phase marker must be idempotent at every step — notably, a cut between phase
  2's step 5 and step 6 leaves a *verified new* `factory` selected while the
  marker still says `CopyPending`; CAL must recognise "I am running from
  `factory` and my own image matches the staged hash" as *already done* and
  advance, rather than re-copying.
- **Flash space in `factory` for the writer**, which is the partition that cannot
  be resized in the field. `factory` has 366,576 bytes spare against the
  2026-09-15 CAL measurement, and the journal pass spent 13,608 of the previous
  margin. A raw-partition copier plus a phase state machine is small, but it is
  not free and §3's re-measure must happen before it lands.
- **No rollback to the *previous* CAL once phase 2 commits.** After the copy both
  partitions hold the candidate; there is no third slot and §4.3 proves there
  cannot be one. Rollback exists only up to the moment `factory` is committed.

#### The irreducible risk, named

**Making the recovery image remotely replaceable makes it remotely breakable.**
SHA-256 and `esp_image_verify` prove integrity, never correctness: a CAL that
hashes perfectly and fails to join WiFi will be installed faithfully into
`factory` on every unit it is offered to, and the fleet will have lost the thing
that rescues it — at scale, in one operation, with no cable.

Nothing in the mechanism can fix that; it is a release-process problem and it
must be treated as one:

- one bench unit first, then one field unit, then a fraction of the fleet — never
  fleet-wide in one step;
- the server-side per-kind "current build" (step 4) is where the staging lives,
  and it should be capable of naming a *cohort*, not just a build;
- a candidate CAL that has not been observed booting from `ota_0` **and** from
  `factory` on real hardware is not a candidate;
- the existing `Identity::kMaxBootAttempts` ladder protects against a bad App.
  There is no equivalent for a bad CAL and there cannot be, because CAL is what
  the ladder falls back to.

This risk is the reason the feature is worth building anyway: today a bad CAL
costs a truck roll to every unit, and after this it costs a truck roll to every
unit *plus* the ability to have caused it remotely. What changes is that a *good*
CAL becomes deliverable — which is the whole point, and the exposure is managed
by how releases are rolled out, not by how the bytes are written.

---

## 6. Recommendation, and the order of work

**One writer module, two sources.** The SD→`factory` and `ota_0`→`factory` copies
differ only in where the bytes come from; both are "read 4KB, verify, write 4KB,
re-hash the destination". Build the writer once behind a source interface.

1. **§2's bench tests.** Test A settles the fallthrough. Test B is go/no-go on
   §5.4. Nothing after this is worth starting until both have been observed.
2. **§3's re-measure**, and add a CAL-image ceiling check to
   `ci/build-firmware.sh` next to the two `check_size` calls.
3. **The writer, with the SD source only** (§5.3). Testable at a bench with no
   server, no `Kind=Cal` and no network — and it delivers Brett's "test cal off a
   SD" as a shipped capability rather than a scaffold.
4. **`Kind=Cal` server-side plus a per-kind current build** (the suggestion's step
   4), able to name a cohort rather than only a build (§5.4's risk section).
5. **The trampoline: phase markers, the `ota_0` interlock, the
   `verifyRollbackLater()` override, and the `ota_0` source for the writer**
   (§5.4).
6. **One USB pass** — carrying CAL logging, the `Tls.cpp` certificate-bundle fix,
   and the first CAL that contains this feature. Not for a table change: there
   isn't one. But the first OTA-capable CAL cannot arrive over the air, because
   the CAL in the field has no phase-1 code. **This feature bootstraps over USB
   exactly once**, and the planned pass is where that happens.

Items 4 and 5 are out of scope for this pass and are not designed beyond the
interfaces above.

---


### Re-measured 2026-09-16, and the arithmetic holds

Section 3's figures were labelled not-re-measured. They now are, compiled from
this branch with the pinned toolchain:

| Binary | Measured | Ceiling | Used | Headroom |
|---|---|---|---|---|
| `CAL.ino.bin` | **1,337,360** | 1,703,936 (`factory`) | 78.5% | 366,576 |
| `App.ino.bin` | **1,492,224** | 2,097,152 (`ota_0`) | 71.2% | 604,928 |

Both conclusions survive:

- **CAL fits `ota_0` with 759,792 bytes spare**, which is the single arithmetic
  requirement of the timeshare design in §5.4.
- **A staging partition is short by 732,432 bytes** against `ota_0`'s real slack
  of 604,928 — close to the 731,984 estimated before measuring, the difference
  being the App growing by today's four builds.

`ci/build-firmware.sh` now carries a third `check_size`: CAL against `ota_0`, not
only against `factory`. That invariant had no guard, and it is the one the whole
feature rests on. Note which way round the risk runs — `factory` (1,703,936) is
the SMALLER partition, so a CAL that comfortably fits its own home would still
break staging if the table were ever rebalanced the other way. The build now says
so instead of a device finding out mid-update.

## 7. Open questions that need hardware

Listed as open rather than resolved, because each one can only be answered by a
board.

1. **Does a corrupt `factory` fall through to `ota_0`?** §1 says yes from the
   compiled bootloader. §2 Test A settles it. The design leans on it in exactly
   one place (§5.4's deliberate rollback after a failed copy).
2. **Does `CAL.ino.bin` execute correctly from `ota_0`?** §2 Test B. **Go/no-go
   for §5.4.** Strong inference, zero observations.
3. **Is a fallthrough sticky?** `set_actual_ota_seq` should rewrite blank
   `otadata` to name `ota_0` with state `VALID` (§2.1 step 5). If so, damage to
   `factory` becomes silent after the first fallthrough — which argues for a
   periodic `esp_image_verify` of `factory` reported on telemetry, so the fleet
   can see a broken recovery image before it needs one. Not designed here.
4. **Can firmware write `factory` at all on this silicon?** `esp_partition_write`
   permits it (§5.3) and CAL writes `ota_0` through the same stack daily, but the
   `factory`-specific case has never been executed. First run of the writer,
   at a bench, on a unit with a §2.0 backup.
5. **What does an SD mount actually cost now?** `kMaxOpenFiles` went 5 → 1 to
   recover part of ~67KB contiguous and the recovered figure has never been
   read off a device. It decides whether §5.3's download phase is safe on a
   card-equipped unit, and it is already instrumented — `SdStorage::begin()`
   prints it on every boot.
6. **How long does the phase-2 copy take?** Erase plus write of ~1.34MB, all
   local. It sets how wide the one window with an invalid `factory` is, and it
   should be measured rather than estimated — this file's own oldest lesson.
7. **Does `esp_image_verify` on an Arduino-built image check the appended
   SHA-256, or only the segment checksum?** It changes how much §5.4 step 5's
   free verification is worth. Step 4's explicit read-back hash does not depend
   on the answer, which is why it stays even though step 5 looks redundant.

---

## 8. What this design does not do

- **No partition-table change.** §4.3 proves a staging partition cannot exist,
  and §5.4 does not need one. `RETABLE_RUNBOOK.md`'s pass stands on its own
  merits (`factory` headroom) and is not a dependency of this feature.
- **No writing of the partition table from firmware, ever.** `partitions.csv`
  settled that — `CONFIG_SPI_FLASH_DANGEROUS_WRITE_ABORTS=y` panics rather than
  failing, and `CONFIG_PARTITION_TABLE_MD5=y` means a half-written table is
  rejected outright with no redundant copy. Both confirmed present in the pinned
  `sdkconfig` while reading §1.
- **No second OTA slot, no symmetric layout.** §4.3.
- **No automatic rollback to the previous CAL after phase 2 commits.** There is
  nowhere to keep the old image. Rollback covers the candidate up to the commit
  and no further (§5.4).
- **No server-side design.** `Kind=Cal`, per-kind current builds and cohort
  staging are named as requirements and left to step 4.
- **No App-side writer implementation.** Step 5. The interface it must satisfy —
  one writer, two sources, destination re-hash mandatory — is fixed here; the
  code is not written.
- **No claim that any of this has been tested.** Nothing in this branch compiles
  differently from `main`; no firmware was built, flashed or run. §1 is
  disassembly, §4 is arithmetic, §3 is not measured, and §7 lists what remains
  unknown.

## 8. Corrections from the bench, 2026-09-16

Three things in this document were wrong, and one of them cost a destroyed App
partition. Recorded here rather than silently edited, because the reason each was
wrong is more useful than the corrected number.

### The fleet is not on the table this document assumes

Device 17's flash was read at `0x8000`. It reports:

    nvs       0x009000     20,480
    otadata   0x00E000      8,192
    factory   0x010000  1,441,792
    ota_0     0x170000  2,424,832
    spiffs    0x3C0000    196,608
    coredump  0x3F0000     65,536

No `callog`. That is the OLD layout. `partitions.csv` describes the INTENDED one
and the re-table was never applied - consistent with its own constraint, since a
table change needs USB and the CAL reflash went out through the browser flasher.

**Every figure in §3 describes a layout no device is running.** The conclusions
survive, and in fact improve, because the old `ota_0` is *larger*:

| | Old table (real) | New table (intended) |
|---|---|---|
| CAL into `ota_0` | fits, 1,087,433 spare | fits, 759,753 spare |
| Staging partition shortfall | 404,752 | 732,432 |

Timeshare works on both. A staging partition is impossible on both.

**`factory` headroom was quoted as 366,576 and is really 104,393** - 92.8% used.
That thin margin is the whole reason the re-table was planned, and it is the
live constraint on every CAL change until the USB pass happens.
`ci/build-firmware.sh` now checks the field ceiling explicitly and says to delete
that check only once the fleet is re-tabled.

### §2's Test B instructed the wrong address

The procedure said `write_flash 0x1B0000`, taken from `partitions.csv`. On the
device in hand that address is 0x40000 *inside* `ota_0`, so the write dropped
1.3MB of CAL into the middle of the App. The next boot reported `invalid segment
length 0x5d746f6f` - which is ASCII, `"oot]"`, App bytes read as an image header.

The procedure's own text warned against exactly this: *"read it off §3.1's decode
for the unit in hand rather than typing it from memory."* It was not followed.
**§2 now begins by reading the table off the device**, and no address in it is
quoted from this repository.

### What the recovery attempt found instead

The corrupted App turned out to be more informative than the test would have
been. Three separate refusals to self-heal, none of them caused by the day's
changes:

1. **CAL would not reinstall, because the version matched.** `ota_0` erased,
   `nvs` still recording the current version, and the install decision was
   version inequality alone. CAL printed `bootableApp=0` and `install? -> no`
   two lines apart and acted on the second. **Fixed** - the decision now consults
   `haveBootableApplication()`, because `installedAppVersion()` is what nvs
   RECORDS and the two part company precisely when recovery is needed.
2. **A failed handover halts.** `Cannot start application`, and CAL stops.
3. **A 30-second SNTP timeout halts.** `Cannot reach the internet`. Transient -
   it succeeded two boots later on the same network - but a slow NTP response at
   boot leaves the device on an error screen until a human intervenes.

**This is the finding that matters for §5.4.** Its safety argument is that the
survivor of a failed update can always retry. Defect 1 meant the survivor did not
believe anything was wrong; defects 2 and 3 mean the survivor stops rather than
retries. A remote CAL update cannot be built on a recovery path with three
hand-rescue states in it, and settling Test B is worth less than closing those.

### What did work, and it is the half §5.4 needs most

The full update path ran clean on the same unit, twice: App requests, reboots
into CAL, CAL downloads 1,492,144 bytes over TLS, commits, hands over. The
contiguous-heap figure did not move - `largest8BitBlock=110580` at 0%, at 50%,
at 100%, and after. Phase 1 of the timeshare design is getting an image into
`ota_0` and handing control to it, and that is now demonstrated end to end with
margin. What remains untested is CAL executing *from* `ota_0` and writing
`factory`.

## 9. The bench tests were run. Both pass, and one answered itself.

Device 17, 2026-09-16. §2's procedure was followed with corrected addresses read
off the device. Everything below was observed, not inferred.

### Test B passes, and the proof is better than the procedure asked for

CAL was written to `ota_0` at `0x170000` and the device power-cycled. It ran: the
panel came up, WiFi joined, SNTP settled, two TLS sessions opened, the manifest
was fetched. All from a partition CAL was never linked for, with
`largest8BitBlock` flat at 110,580 the whole way.

**The identifying evidence is the failure, not the success:**

    [install] Update.begin(1492144) failed: Partition Could Not be Found (error 10)

That error can only occur from `ota_0`. Arduino's `Update` asks
`esp_ota_get_next_update_partition()` for a slot that is not the running one;
from `factory` the answer is `ota_0`, and from `ota_0` on a single-slot table
there is no answer at all. So the error names the partition CAL was executing
from. No separate confirmation was needed.

**§1.5(b) predicted the refusal and got the mechanism slightly wrong.** It said
`Update.begin()` would refuse because the target and the running partition are
the same. It actually fails earlier, at the lookup, before any comparison. Same
outcome; the interlock §5.4 needs is therefore *"do not call `Update.begin()`
from `ota_0` - copy to `factory` instead"*, which is a different statement from
*"handle Update.begin() refusing"*.

### Test A passes, observed in both directions

    factory invalid  -> "OTA app partition slot 0 is not bootable" tried first, then factory
    ota_0   invalid  -> "image at 0x170000 has invalid magic byte", CAL ran from factory

The bootloader walks the table rather than stopping at a bad entry, in both
directions, on this silicon and this core version. §1's reading of the compiled
bootloader is confirmed on hardware.

### The interlock is now in, and it is a refusal rather than the feature

`Updater::installApplication` compares `esp_ota_get_running_partition()` against
its target and refuses when they match, naming the situation and pointing at
`factory` as the way back. Checked BEFORE the download: reaching
`Update.begin()` spends a 1.5MB TLS transfer to learn something knowable at the
start.

This is not phase 2. Phase 2 copies `ota_0` to `factory` and is still to be
built. Until it exists, refusing clearly beats failing obscurely - a device in
this state is one power cycle from normality, because `factory` still holds a
working CAL.

### What phase 1 looked like, four times over

The ordinary update path ran clean on the same unit four times: App requests,
reboots into CAL, CAL downloads 1,492,144 bytes over TLS, commits, hands over.
`largest8BitBlock=110580` at 0%, at 50%, at 100%, and after. **Getting an image
into `ota_0` and handing control to it - phase 1 of §5.4 - is demonstrated with
margin.** What remains is the copy back into `factory`, which is the half no
hardware has exercised.
