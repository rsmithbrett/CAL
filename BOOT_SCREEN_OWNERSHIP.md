# Boot-screen ownership: which binary owns the panel, and when

**Status:** designed and implemented 2026-09-16, not yet observed on hardware.
**Applies to:** `CAL.ino`, `Display.cpp` (CAL), `App/App.ino`, `App/Assets.cpp`.
**Read this before adding any message to a boot path in either binary.** Section
4 is the convention; everything else is why it is that and not something else.

---

## 1. The problem, measured

Every restart on this hardware runs **App → CAL → App**. CAL lives in the
`factory` partition and the bootloader runs `factory` on every boot, so CAL is
not a first-boot component — it is on the critical path of every single restart
for the life of the device (`App/BootDiag.h` records the measurement that forced
that fact into the open: an intent stored in RTC memory did not survive the hop,
because the hop is two software resets with a different binary executing in
between).

Both binaries drew their own boot screens. On a restart of a healthy device the
observer saw, in order:

| # | Drawn by | What appeared |
|---|----------|---------------|
| 1 | CAL `Display::begin()` | black (`lcd.init()` then `fillScreen`) |
| 2 | CAL `showBrandSplash()` | brand mark from LittleFS `/brand.565`, at y=40 |
| 3 | CAL `showStatus("Starting", version)` | screen cleared, **same brand mark redrawn at y=20**, plus "Starting" and the version |
| 4 | App `Display::begin()` | black again |
| 5 | App `showStatus("Starting", "")` | screen cleared again, **"Starting" a second time** — the App's `showStatus` is `clear()` plus text, with no brand backdrop of its own |
| 6 | App `Assets::showBootSplash()` | brand splash a third time, now the SD-card PNG |

Four screen clears, three brand marks and two "Starting" lines to start one
device. Negligible at one boot a week. On 2026-09-11 the fleet was restarting
every **13–27 minutes**, with devices 12 and 17 self-restarting **12 and 24
times inside five hours** — at which point the above is not the boot
experience, it is the product experience.

The App half was fixed first (see the `What a boot is allowed to say on the
glass` block at the top of `App/App.ino`): a therapeutic restart —
`RestartCause::LowHeap`, `::Unreachable`, `::LowHeapResponse` — now suppresses
the WiFi/time/handshake ladder and holds one static screen. CAL was deliberately
left alone while stability testing ran, so that firmware under test changed in
one place at a time. That testing has concluded; this document is the CAL half.

## 2. The constraint that dominates everything

CAL is the recovery image. A device whose App will not boot is rescued by what
CAL displays, and these units cannot be serviced remotely.

> Silencing CAL carries a failure mode the App does not: going quiet in the
> wrong branch leaves a bricked device showing nothing and appearing dead
> rather than recoverable.

The costs are asymmetric by orders of magnitude. A redundant splash is a
cosmetic nuisance that annoys somebody. A dark panel on a device that cannot
start its App is a unit that gets unplugged, boxed and returned as dead when it
was recoverable over USB in two minutes — a truck roll, against a binary that
cannot be patched over the air.

So the design rule is not "find the branches where CAL can be quiet". It is:

> **CAL is loud by default. Silence is a narrow special case that has to be
> argued for out of positive evidence, and every input to that argument is
> written to the journal whether it is used or not.**

Every condition in section 5 is stated as evidence *for* silence. A missing,
unreadable or unexpected input therefore lands on "loud", which is the direction
that costs a splash rather than a device.

## 3. The three options, and why two lost

### Option 1 — CAL skips its own splash when chaining into a healthy App. **CHOSEN.**

CAL already computes the fact that matters: `mustContactServer()` is false when a
healthy application is installed and nothing asked for an update, which is
exactly the boot where CAL's very next action is `Updater::bootApplication()` and
nothing else. On that branch, and only that branch, CAL has a successor that is
about to paint the panel within a second. Everywhere else, CAL is the last thing
that will draw anything for a while — possibly forever.

Taken literally ("skip the splash when handing over") this is still unsafe,
because `haveBootableApplication()` is a *belief*, not an observation: it is true
of an App that is about to panic in its first instruction. Section 5 is what
narrows it into something defensible, and the narrowing is the whole design.

### Option 2 — CAL draws the splash and the App does not redraw it. **REJECTED.**

Attractive on paper: the image becomes continuous across the handoff rather than
redrawn identically. Three things kill it, and the third is fatal.

- **The two images are not the same image.** CAL draws raw RGB565 from LittleFS
  `/brand.565`, 240×120, at a fixed offset, with no decoder. The App draws a PNG
  from the SD card through LovyanGFX's decoder, at whatever size the server
  prepared. They are different files, in different storage, at different sizes,
  produced by different pipelines. "Continuous" would be a lie told by two
  binaries that cannot see each other's output.
- **Neither binary survives to verify the handoff.** `esp_restart()` takes the
  RAM. The App cannot ask what CAL drew, and CAL cannot know whether the App
  agreed to leave it alone — the only channel between them is CAL's NVS
  namespace, and adding a "what is on the glass" protocol to it would make the
  unpatched binary the one holding the contract.
- **The App's splash draw is load-bearing for heap, not for looks.** This is
  the fatal one. `Assets::showBootSplash()` is what allocates LovyanGFX's
  ~44KB PNG decoder scratch, and it runs before `Http::begin()` specifically
  because `maxAllocHeap` is 110,580 bytes there and 32,756 bytes once the
  network stack has allocated. `Display.cpp` then deliberately never calls
  `releasePngMemory()`, so that one early scratch buffer serves every graphic
  card for the whole uptime. Suppressing the App's splash to avoid a redraw
  would move the first decode to after the network is up, where it fails — and
  every graphic card then fails for the rest of the run and drops itself from
  the rotation. That is the exact bug documented in the README's *Where the boot
  splash has to happen*, reintroduced on purpose to save a cosmetic redraw.

Option 2 also has the wrong risk shape for this project: it puts the
can't-be-updated binary in charge of what the updatable one must not do.

### Option 3 — a documented boot-screen ownership convention. **ADOPTED ALONGSIDE, NOT INSTEAD.**

Not an alternative to option 1 — it is what stops option 1 decaying. The
duplication in section 1 was not a bug anybody wrote; it accreted because two
binaries each had a defensible local reason to draw and no shared statement of
who owned the panel at which instant. Without section 4 written down somewhere a
firmware author actually opens, the next boot message lands wherever it seemed
natural and the fleet gets a fourth brand mark.

## 4. The convention

**At every instant of a boot, exactly one binary owns the panel, and ownership
transfers only at `esp_restart()`.**

1. **CAL owns the panel from power-on until it hands over.** Anything CAL draws
   must be something CAL itself is doing or waiting on. CAL must have something
   on the glass within about two seconds of power being applied — a dark screen
   is indistinguishable from a dead device and gets unplugged mid-setup. This
   rule is absolute on every path except the one in rule 3.

2. **The App owns the panel from its first instruction until power is removed.**
   Once CAL has called `esp_ota_set_boot_partition()` and restarted, CAL's
   screens are gone and nothing in CAL can put them back. The App's
   `Display::showStatus("Starting", "")` at the top of `setup()` is drawn on
   **every** boot, quiet ones included, and must stay that way: it is what
   covers the gap while SD mounts and the splash decodes, and it names no
   network, no clock and no server.

3. **The one exception: on an immediate handover, CAL yields the panel to the
   App and paints nothing at all.** This is the *only* circumstance in which CAL
   is allowed to be silent, and it requires all four conditions in section 5.
   The reasoning is that CAL has a successor which is about to draw, is known
   from evidence to be capable of drawing, and will draw within about a second —
   so CAL painting first produces a redundant frame rather than reassurance.

4. **A failure screen is never suppressed, on any path, in either binary.**
   `haltWithFailure()`, `Display::showFailure()` and the WiFi-recovery prompts
   always draw. A household that has to hold BOOT to fix its network has to be
   told so, and a terminal condition must name itself on the glass — the journal
   is for whoever arrives later with a cable, not for the person standing there
   now.

5. **A gesture in progress always breaks silence.** A finger on the BOOT button
   means a human is present and acting, and the one user-accessible control on
   this appliance must never look dead. If CAL has drawn a gesture prompt this
   boot, it is loud for the remainder of the boot regardless of everything else.

6. **Adding a boot message is a question about ownership before it is a question
   about wording.** Ask which binary owns the panel at the instant the message
   would appear, and put the message in that binary. If the answer is "both, for
   a moment" — the handoff — then it belongs in the App, because the App's frame
   is the one that survives into the rest of the uptime.

7. **A skipped screen is logged, always.** `Journal::printf` in CAL,
   `Log::printf` in the App, naming the screen that was withheld and the
   decision that withheld it. "This screen was deliberately not drawn" must be
   distinguishable in the stream from "this stage never ran" — the remote stream
   and the flash journal are the only diagnostic channels a deployed device has,
   and a silent skip cost this project a night on 2026-09-15.

8. **In CAL, verbose means several short lines, never one long one.**
   `Journal` caps every line at **160 bytes** — `kMaxLine` in `Journal.cpp`,
   enforced in both `printf()` (which appends `...(truncated)`) and
   `writeRecord()`. A line written as a paragraph therefore loses its *end*,
   which is where the conclusion usually sits: the first draft of the ownership
   decision's own log line was 230 characters and would have shipped with the
   verdict cut off and the inputs intact — a diagnostic that reports its
   evidence and swallows its finding. So: put the identifiers and the verdict on
   one line inside the cap, split anything longer across lines that each stand
   alone, and keep the prose in this document, where there is room for it. The
   App's `Log` is more generous but not unbounded — a 256-byte scratch with the
   same `...(truncated)` marker — so the rule is the same there with a different
   number. Checked rather than assumed, because "surely it fits" is how the
   first draft of this line came to be 230 characters.

## 5. The four conditions for CAL's silence

All four must hold. Any one of them false, and CAL draws exactly what it drew
before this change. Implemented as `appOwnsThePanelThisBoot()` in `CAL.ino`,
which logs all four inputs and the outcome on one journal line every boot,
including the boots where it returns false.

### 5.1 `esp_reset_reason() == ESP_RST_SW`

The restart was asked for by software. This excludes, deliberately:

- **`ESP_RST_POWERON`.** Somebody just plugged it in and is standing there
  waiting to find out whether it works. This is precisely the case CAL's
  two-second rule exists for, and a silent panel reads as a dead appliance.
  Loud.
- **`ESP_RST_BROWNOUT`.** The supply sagged. Something is wrong with the power,
  a human may well be at the device, and the firmware has no basis for claiming
  it is fine. Loud.
- **`ESP_RST_PANIC`, `ESP_RST_INT_WDT`, `ESP_RST_TASK_WDT`, `ESP_RST_WDT`.** A
  crash is not therapy. This firmware did not choose it, does not know what
  state the device is in, and must not present a boot it cannot vouch for as an
  uneventful one. Loud. (This is the same conclusion the App reached
  independently for unexpected resets; the two now agree.)
- **`ESP_RST_DEEPSLEEP`, `ESP_RST_SDIO`, `ESP_RST_UNKNOWN`** and anything a
  future ESP-IDF adds. Not enumerated, not special-cased: the test is equality
  against `ESP_RST_SW`, so everything unrecognised is loud by construction.

### 5.2 `mustContactServer()` is false

CAL's very next statement is `Updater::bootApplication()`. Nothing else in CAL
is going to run: no WiFi join, no SNTP wait, no enrollment poll, no download, no
QR. This is what makes "there is a successor about to draw" true rather than
hopeful. Note what this condition excludes for free, without a second rule:

- **An OTA restart** sets `updreq`, so `mustContactServer()` is true and CAL is
  loud through its whole download-and-install ladder. Correct, and it matches
  the App's own reasoning: the OTA restart is the one restart where a long wait
  is expected and legitimate, and if a new build fails to come up, CAL's boot
  screens are the only visible evidence of how far it got.
- **A return to CAL for reprovisioning** also sets `updreq`
  (`Loader::returnToLoaderForReprovisioning()`), so it is loud too. A household
  is standing at the device.
- **A device with no App installed, or one whose App has burned its boot
  attempts,** fails `haveBootableApplication()`, so `mustContactServer()` is
  true. Loud — and this is the bricked-device case the constraint in section 2
  is about.

### 5.3 `Identity::bootAttempts() == 0`

This is the condition that makes option 1 safe rather than merely plausible, and
it is worth being precise about what it measures. `Updater::bootApplication()`
calls `Identity::recordBootAttempt()` *immediately before* `esp_restart()`, and
the App calls `Identity::clearBootAttempts()` only once it has reached steady
state (after `ensureWifiConnected()` returns in `App.ino`'s `setup()`). So at
CAL's decision point:

- **`0`** means: the last time CAL handed over, the App came up, got onto a
  network, and said so. That is an *observation* that this App boots on this
  device, recorded by the App itself, surviving the restart in NVS.
- **non-zero** means: the last handover did not produce an App that reported
  itself healthy. CAL does not know why and does not need to — it knows the
  thing it is about to trust has already disappointed it once.

**This bounds the dark window at exactly one boot cycle, which is the core
safety argument.** Suppose every condition holds, CAL goes quiet, and the App
then panics before its first `Display` call — the worst case for this design.
The counter is *already* at 1, because CAL incremented it on the way out. The
device restarts, CAL comes up, 5.3 is false, and CAL draws its splash and runs
its ladder exactly as it does today. One dark cycle of roughly a second,
followed by a permanently loud CAL, and then at three attempts
`haveBootableApplication()` turns false and CAL stops handing over at all and
re-downloads. A device cannot be silently dark: it can only be briefly dark once.

### 5.4 No gesture prompt has been drawn this boot

`bootHoldRequested()` draws "Keep holding BOOT…" the instant the button reads
LOW, and leaves it on screen if the household releases before the first tier
completes. Nothing used to clear that, because the splash and the WiFi ladder
landed on top of it milliseconds later. A quiet handover would remove the thing
that cleaned it up by accident, and hand over with a screen telling somebody to
keep holding a button they already let go of.

So CAL tracks whether it has painted for a gesture, and if it has, it is loud
for the rest of the boot. This is the same defect the App fixed in
`restoreHeldBootScreen()`, caught here before it shipped rather than after.

### Rejected: reading the App's `BootDiag::RestartCause` from CAL

The tempting fifth input. `RestartCause` names the therapeutic restarts exactly
— `LowHeap`, `Unreachable`, `LowHeapResponse` — where `ESP_RST_SW` is a coarser
proxy for the same thing, and it lives in NVS precisely because it has to
survive the App → CAL → App hop.

Rejected on coupling. Those enum values are the App's schema, versioned on the
App's clock, and `BootDiag.h` states the contract as "written to NVS by one boot
and read by the next". Making CAL a third reader puts a wire format between the
binary that is replaced weekly and the binary that cannot be replaced without a
USB cable, pointing the wrong way: an App change to those numbers would
misinform a CAL that can only be fixed by a truck roll. `esp_reset_reason()`,
`mustContactServer()` and `bootAttempts()` are all facts CAL already owns, and
the coarseness costs nothing — every restart the finer signal would have
identified as therapeutic is already covered by §5.1 plus §5.2, and every
restart it would have excluded is already excluded by §5.2.

## 6. The safety net: CAL can no longer fail to draw because it skipped startup

Before this change, `Display::begin()` was called unconditionally at one place
in `setup()` and every other `Display::` entry point assumed it had run. A CAL
that skips `begin()` on the quiet path and then needs to draw a failure screen —
because `bootApplication()` returned instead of restarting — would have been
drawing to an uninitialised panel. That is the section 2 failure mode arriving
through the back door.

`Display::begin()` is therefore now idempotent, and **every public `Display::`
entry point brings the panel up itself if it is not up yet.** `begin()` remains
where it was and still means "light the panel now"; what changed is that it is
no longer the only thing that can. The property this buys is worth stating
plainly:

> There is no reachable state in which CAL wants to draw and cannot.

The quiet path is now the *absence* of a draw call, not a disabled display, and
the panel is brought up lazily by whichever screen needs it first. It costs one
`bool` and one branch per draw call.

## 7. What a boot looks like now

**Cold power-on, healthy device (unchanged).** Black; brand splash within about
two seconds; "Starting <version>"; handover; App black; App "Starting"; App
splash held its three-second minimum; first card. §5.1 fails, so CAL is loud —
deliberately, because a person is standing there.

**Therapeutic restart (`LowHeap` / `Unreachable` / `LowHeapResponse`) — the case
this change exists for.** The App draws "Refreshing — Reclaiming memory, back in
a moment" (or "Reconnecting"), waits 1.5 s so it is actually seen, and restarts.
CAL comes up and **paints nothing**: no `lcd.init()`, no `LittleFS.begin()`, no
splash, no "Starting". It writes its journal, reads identity, logs the ownership
decision and hands over. The App then draws "Starting", then the splash, which
`holdSplash()` keeps up for at least three seconds, until the first card
replaces it. **One brand mark, one "Starting", one binary.** CAL's whole
contribution to the glass is nothing, and its whole contribution to the record
is a journal line saying so.

**OTA restart.** §5.2 fails (`updreq` is set). Loud, unchanged: splash, WiFi,
time, discovery, manifest, the download progress bar, "Do not unplug".

**Reprovision / BOOT held.** §5.2 fails, or §5.4 does. Loud, unchanged.

**App panics before drawing, on a boot CAL was quiet for.** Roughly a second of
black, then a restart. From that restart onward §5.3 is false, so CAL is loud
for every subsequent attempt, and at three attempts it stops trusting the App
and re-downloads. See §5.3.

**Handover itself fails (`esp_ota_set_boot_partition()` refuses).**
`bootApplication()` returns instead of restarting. CAL logs that it is breaking
its silence and draws "Cannot start application / Restart the device", which
brings the panel up on its own (§6). Unchanged in effect from before, and now
explicitly reasoned rather than incidental.

**No App installed at all.** §5.2 fails. Loud: the enrollment QR, the
hardware address, the not-yet-activated screen — all exactly as before.

## 8. What this does *not* do

- **It does not remove any App screen.** The App's "Starting" is drawn on every
  boot, the splash minimum still applies, and the therapeutic suppression the
  App already had is untouched. This change is CAL-side only apart from the
  cross-references in section 10.
- **It does not make CAL quiet on a cold boot.** That was tempting, since
  `bootAttempts()` is 0 there too, and it is wrong: the two-second rule is about
  the person holding the plug.
- **It does not introduce a protocol between the binaries.** Nothing new is
  written to NVS; the decision reads three values that already existed for other
  reasons.
- **It does not address CAL's black frame on the loud paths.**
  `Display::begin()` still clears to black before the splash lands. That frame
  is short, it is on a path where a person is expected to be watching anyway,
  and removing it would mean reasoning about what an ILI9341's GRAM holds after
  a chip reset and a panel software reset — not a thing to guess at in the
  unpatchable binary.
- **It does not change the App's own splash-vs-ladder rule**, including the
  `kMaxSilentSelfRestarts` budget that makes the App start talking again after
  three self-restarts in a row. Note the interaction, because it is deliberate:
  past that budget the **App** says "This device keeps restarting itself" on the
  one screen it holds, while **CAL** stays quiet on the same boot (§5 knows
  nothing about that budget). That is the right split under section 4 — the
  message belongs to the binary that owns the panel when it is read, and the
  App's frame is the one that survives.

## 9. What is verified, and what needs hardware

Verified by compilation only: both sketches build clean against
`esp32:esp32@3.3.11` at the pinned library versions, and both binaries fit their
real partition ceilings from `partitions.csv`.

**Not verified, and each needs a device on a bench:**

1. That a therapeutic restart actually shows one continuous screen — the thing
   this change is for. Needs a device driven into `LowHeap` or `Unreachable`
   and watched across the hop.
2. That the panel behaves when CAL never calls `lcd.init()` and the App calls it
   a second later. Expected to be identical to today, since the App's
   `Display::begin()` is unconditional and already re-inits a panel CAL has
   initialised; but "expected" is not "observed", and this is the one step where
   a surprise would be visible as a garbled or dark first frame.
3. That the lazy-init fallback works: force `esp_ota_set_boot_partition()` to
   fail on a quiet boot and confirm "Cannot start application" reaches the glass
   from a cold `Display`.
4. That the dark window on a quiet boot is as short as the reasoning claims.
   Worth timing, because the argument in §5.3 trades a bounded dark window for
   the whole benefit, and the bound should be measured rather than asserted.
5. That §5.4 holds: press and release BOOT briefly during a therapeutic restart
   and confirm the splash covers the abandoned prompt rather than handing over
   with it on screen.

`TEST_PLAN.md` carries these as numbered steps.

## 10. Where else this is written down

- `README.md`, *The boot ladder* — step 1 now names this document and states the
  handover exception, because the ladder is where a firmware author looks first.
- `README.md`, *Where the boot splash has to happen, and why it now stays on
  screen* — the same section also had a standing documentation defect, fixed
  alongside: it claimed the logo "stays up through the WiFi join", which had
  never been true in any shipped binary. The hold was two `if (!splashOnScreen)`
  guards around `App.ino`'s own status calls, and `WifiJoin::joinStoredNetwork()`
  drew unconditionally from a different translation unit, painting over the
  splash a few hundred milliseconds later on every boot for the whole life of
  the feature. It is true *now*, since the suppression reaches WifiJoin through
  `WifiJoin::setProgressVisible()` — but the section was describing an intention
  as though it were the binary, which is the kind of documentation that costs
  somebody an evening.
- `App/App.ino`, the *What a boot is allowed to say on the glass* block — now
  cross-references this document, so the App-side reader learns that CAL has a
  rule too.
- `CAL.ino`, `appOwnsThePanelThisBoot()` — the conditions in code, with the
  journal line that reports them.
