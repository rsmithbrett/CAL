# CAL / App test plan

This file is the firmware half of the project's testing rule. The rule is that a
change is validated by an automated test wherever one is possible, and that
where it is not, the manual procedure is written down instead of being left in
somebody's head.

**Almost nothing in this repository can have an automated test, and this file
exists to be honest about that rather than to hide it.** The App is an Arduino
sketch cross-compiled for an ESP32-WROOM-32E; its behaviour is a function of a
real heap, a real TLS stack, a real NVS partition, a real 2.8" panel and a real
server on the other end of a real network. There is no host-runnable test
target, no injectable clock, and no seam between the watchdogs and
`esp_restart()`. A clean `arduino-cli compile` proves that the code builds and
that a string is present in the image. It proves nothing else, and every commit
message in this repository that says "verified" means exactly that unless it
also names a device.

The server's own `TEST_PLAN.md` (in the DiscoverAroundMe repository) covers
everything on the other side of the wire - the check-in gateway, telemetry
ingestion, the firmware catalog, `/diag`. Where a firmware behaviour has a
server-side consequence it is noted here and tracked there.

## How to observe anything at all

Every procedure below depends on one of these four channels, so they are worth
stating once.

1. **The remote debug stream.** The only diagnostic channel a deployed device
   has. Toggled per device from the server and mirrored back on every check-in
   response, so it takes up to one check-in interval to come on. Everything the
   watchdogs decide is logged there with its numbers, including the branches
   that decide to do nothing - that is a standing requirement in this codebase,
   not a nicety, and several of the checks below consist of reading a line that
   says why something did *not* happen.
   **Note the stream is itself a heap consumer** - the measured ratchet in the
   `HeapRatchet` investigation was the stream - so a test that needs both the
   stream and a low-heap condition is not measuring an untouched device.
2. **A USB serial console.** Sees boot lines the stream cannot, because the
   stream is not up yet. The only way to read the `[boot] restart reason` pair
   and the `[health]` seeding line on the boot immediately after a self-restart.
   Note that opening a serial port with DTR asserted resets this board, which
   destroys the state most of these procedures are trying to observe - open the
   port first, then cause the condition.
3. **`/diag` on the server**, for telemetry, the reboot heatmap and the retained
   card status line. This is where a *misreported* fault shows up, and several
   of the changes below are about the label rather than the action.
4. **CAL's boot journal**, in the `callog` flash partition. The only channel
   that survives the boot that wrote it, and the only one that works when CAL
   itself is what failed - neither the stream nor `/diag` exists at that point,
   and channel 2 above shows only what happens while the cable is attached. CAL
   prints the previous boot automatically at power-on, `d` on the serial console
   dumps all sixteen retained boots, and if CAL will not run at all it comes off
   the chip with `esptool read_flash 0x3B0000 0x10000`. Section 6 below is its
   own procedure; it is listed here because several other sections become
   readable on a CAL-side failure that previously left nothing at all.

---

## 1. The self-restart backoff (this pass)

`selfRestartFloorMs()` in `App.ino`. Twenty minutes doubling to a four-hour
ceiling - 20, 40, 80, 160, 240, 240... - stepped by the connection watchdog and
the response-OOM watchdog, cleared only by a completed check-in. See the
README's "The unreachable watchdog rebooted on the same cadence forever".

**Automated coverage: none, and none is possible in this repository.** The
schedule is a pure function of one `uint8_t` and `millis()`, which is testable
in principle - but nothing in this tree can host a test, and extracting the
function to something that could would mean building a host test target for a
seven-line function. The judgement is that the log lines below are the cheaper
proof.

### 1a. The ladder actually doubles

The honest version of this test takes just over five hours, so do it in two
parts.

**Short form (bench, ~20 minutes), with a scratch build.** Temporarily set
`kBaseMsBetweenSelfRestarts` to 60,000 and `kMaxMsBetweenSelfRestarts` to
480,000 in a build that is never registered in the firmware catalog. Then:

1. Provision a device normally and let it complete at least one check-in.
2. Make the server unreachable *without* making WiFi drop - the watchdog
   declines outright if WiFi is not associated, so pulling the AP tests nothing.
   Stopping the Host process is the cleanest; a DNS override that resolves the
   service name to an unroutable address also works and is closer to the field
   failure.
3. Watch the stream. Expect `[checkin] failure N of 5`, then the restart line
   naming `self-restart 1 of this run` and a next-gap of 60,000 ms.
4. Across the next four restarts, the gap named on each restart line must read
   60,000 / 120,000 / 240,000 / 480,000 / 480,000. The hold-off line between
   them must name the same figure and say how many doublings produced it.
5. Confirm the hold-off line appears **once per episode**, not once per loop
   iteration. A flood here is a regression of `gHoldOffLogged` and has happened
   before.

**Long form (one real overnight run, no scratch build).** Same setup on shipping
constants. Record restart timestamps from the reboot heatmap on `/diag`. Expect
restarts at roughly T+5 min, +25 min, +65 min, +145 min, +305 min, then every
240 min. This is the run that proves the shipped constants, and it is the only
one that does; the short form proves the shape.

### 1b. A completed check-in resets it, and nothing weaker does

1. Drive a device to at least self-restart 3 (floor 80 minutes) using 1a.
2. Bring the server back. On the next check-in the stream must print
   `[checkin] a complete round trip after 3 self-restart(s) in this run - the
   backoff is reset to its 1200000 ms base`.
3. Power-cycle the device and confirm the boot no longer prints the
   `[health] last restart was ...` hold-off seeding line with a raised floor -
   the NVS key `rstbackoff` must be back to 0.
4. **The negative half, which is the point.** Repeat from step 1, but instead of
   bringing the server back, restore only DNS and the TCP listener while keeping
   the API refusing (a listener that accepts and immediately closes, or a TLS
   port answering with a rejected certificate). `Http::diagnoseFailure()` will
   report the name resolving and the connect succeeding. The backoff must
   **not** reset: the next hold-off line must still name the raised floor. This
   is the scenario a well-meaning "reset on any sign of life" would break, and
   it is the exact shape of the poisoned-TLS-client fault.

### 1c. A maintenance window does not touch the schedule *via this watchdog*

Note the scoping in that heading. "A window does not step the backoff" is the
tempting summary and it is **false**: the response-OOM watchdog is deliberately
not suppressed by a window (see 4b step 6) and it steps the same counter, so a
device that runs out of heap during a deploy restarts and steps the ladder inside
the window. What is being tested here is the connection watchdog, which cannot
restart inside a window and therefore cannot step it. Test 1c-ii covers the
other half.

1. Drive a device to self-restart 2 (floor 40 minutes).
2. With the server still up, declare a maintenance window
   (`maintenance_until_utc`) far enough out to cover the test, then take the
   server down.
3. The stream must print the maintenance suppression line, and that line must
   name the current floor and the step count. It must say "nothing **here**
   steps the backoff" rather than claiming the window freezes the schedule
   outright. No restart may occur for the whole window, however long it runs.
4. Let the window expire with the server still down. The next restart must use
   the floor from step 1 - the window must not have advanced the ladder (this
   watchdog did not restart, so nothing here stepped) and must not have reset it.
5. Check the clock-skew guard while here: set the device's clock behind the
   server's and confirm the device still enforces its own expiry rather than
   trusting the server's.
6. **The provably-cannot-reconnect path is suppressed too, and that is the
   deliberate part.** With a window declared, drive the device below
   `Http::kTlsRecordBufferBytes` contiguous (recipe in 4a) so
   `Http::canOpenNewSession()` is false while check-ins are failing. This is a
   proven *local* fault that the window does not explain, and the device must
   still not restart: a restart cannot reach a server that is down, so it would
   spend a backoff step on an attempt doomed before it began. Confirm the restart
   happens promptly once the window closes, with the step count unchanged by the
   window.

### 1c-ii. The response-OOM watchdog *does* step the ladder inside a window

The counterpart to 1c, and the one that proves the scoping above is real rather
than a caveat nobody checked.

1. Declare a maintenance window.
2. Reproduce the low-heap-response condition inside it (4a).
3. The device must restart (per 4b step 6) **and** the restart line must show the
   step count advancing. On the next boot the raised floor must be in force.
4. Confirm the connection watchdog's suppression line, if it also appears, does
   not claim the window left the schedule untouched.

### 1d. The response-OOM watchdog shares the schedule

1. Force the low-heap-response condition (see 4 below).
2. Confirm the restart line from `checkResponseOomWatchdog()` names
   `self-restart N of this run` with N continuing the *same* run as any
   connection-watchdog restarts, not restarting from 1.
3. Alternate the two faults and confirm the floor keeps climbing across both.

### 1d-ii. A second OOM hold-off episode still explains itself

A regression test for a latch defect found on 2026-09-14 while checking this
change: `gHoldOffLogged` is one flag shared by both watchdogs' hold-off lines,
and `performCheckIn()` cleared it from the connection-recovery branch only. Since
`92cc034` deliberately keeps a `NoMemory` parse failure *out* of
`gConsecutiveCheckInFailures`, an episode that came purely through the OOM path
left that branch untaken and the latch stuck true for the rest of the boot -
after which every later OOM hold-off held off in complete silence. The backoff
makes that worse rather than better, because hold-off episodes are now both
longer and more numerous.

1. Drive a device through one OOM hold-off so the hold-off line appears once.
2. Let a check-in succeed. The stream must print
   `[checkin] response parsed after N out-of-heap failure(s)`.
3. Drive it into a **second** OOM hold-off. The hold-off line must appear again.
   Before the fix it never did, and the device declined to reboot in silence.
4. Confirm it still appears only once per episode, not once per loop iteration -
   the fix must not undo the latch, only reopen it.

### 1e. A power cycle gets one free attempt

1. Drive a device to a raised floor and leave it holding off.
2. Pull power and restore it.
3. On the serial console, the boot must print the "recorded in this backoff run,
   but the restart that led to this boot was neither watchdog's" line, and must
   **not** seed the hold-off clock.
4. The next watchdog trigger must fire at the ordinary five-failure threshold
   with no wait, and the restart after it must name a step count that continued
   the run rather than restarting it.

### Known gap

The RAM mirror of the step count and the NVS write are not atomic. Power lost in
the few instructions between them costs one shorter floor. Not tested, not worth
testing, recorded so nobody reports it as a defect.

---

## 2. The mbedTLS figure is 16,717 contiguous, twice - not "~32KB" (`3cfbb08`)

A documentation correction with no runtime component, which makes it the one
item here where "read the code" is a complete test. What matters is that the
wrong figure routes a reader to the wrong work.

1. `grep -rn "32KB" .` across the repository. Every remaining hit must be either
   (a) inside an explicitly-labelled superseded block that says so, or (b) a
   statement about the 33,434-byte **total** that cannot be misread as a
   contiguity requirement. There must be no unqualified "needs roughly 32KB
   contiguous" left anywhere, in Markdown, in a workflow file or in a comment.
2. `Http::kTlsRecordBufferBytes` must be the single definition of the number.
   **This is currently aspirational and the tree does not meet it** - checked
   2026-09-14, `grep -rn 16717 App/` finds three other sites:
   - `App/SdStorage.cpp:144`, a live `largestAfterMount >= 16717` **comparison**.
     This is the one that matters: it is a second definition of the figure that
     decides whether the mount log says the device can open a TLS session, and
     nothing makes it follow `kTlsRecordBufferBytes` if that constant ever
     changes. `SdStorage.cpp` does not include `Http.h`, which is why it was
     written as a literal; fixing it means either that include or lifting the
     figure somewhere both can see. Left alone here rather than folded into an
     unrelated change.
   - `App/SdStorage.cpp:140` and `App/BootDiag.cpp:184` and
     `App/HeapRatchet.cpp:146`, all inside log-string prose. Harmless to
     behaviour, still a figure that can drift out of step with the constant.

   Re-check this step after any change to `kTlsRecordBufferBytes`, and treat the
   `SdStorage.cpp:144` comparison as the acceptance criterion for closing it.
3. **The arithmetic worth re-deriving at the bench once**, because it is what
   the correction is for: a device sitting at largest8 = 21,000 satisfies the
   *first* record buffer and still cannot complete a handshake, because the
   second needs another 16,717 contiguous and the first has just been carved out
   of the only block that size. Confirm against a card-less device (which sits
   at largest8 21,000-26,000 while drawing nothing) that handshakes fail with
   `MBEDTLS_ERR_SSL_ALLOC_FAILED` at that figure. On the old framing those
   devices "obviously" lacked memory; on the correct one it takes the two-buffer
   argument to explain them, and if that argument is wrong the whole watchdog
   design is built on sand.
4. The workflow header's second correction - that
   `MBEDTLS_SSL_MAX_FRAGMENT_LENGTH` **is** compiled into the shipped core but
   is inert because `MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH` is not set - is
   checkable without hardware: grep the installed core's `mbedtls/esp_config.h`
   for the first symbol and the shipped `sdkconfig` for the second. Do this
   again after any core version bump, because the conclusion is version-specific
   and the file says so.

---

## 3. Listings tells "none for sale" apart from "could not ask" (`902fc69`)

`Status::RefreshFailed` on `Listings::Result`, plus the removal of a raw
upstream error from a drawn string.

**Automated coverage:** the server side of this is covered by
`Providers.Tests` / `MyListingsServiceTests` and `DeviceFacingPayloadTests` in
the DiscoverAroundMe repository, which are what put `status` on the wire and
keep `lastRefreshError` off it. Nothing tests the card. **Nothing can:** the
behaviour under test is a branch taken inside `fetchMine()` on a parsed
response, on hardware, with no test harness on either side of it - which is why
every step below is a human at a bench, and why 3c exists at all.

> **The signal this card reads changed.** It used to be the *presence* of
> `lastRefreshError`; it is now the server's `status` field, with the old
> inference kept as a fallback. Section 3c is the procedure for that, and it
> supersedes 3a step 5 - read 3c before running 3a against a current server, or
> you will report a regression that is the fix working.

### 3a. The three empty states draw three different things

Requires a device with the listings card in its policy, and server-side control
of the RentCast key.

1. **Genuinely empty.** A valid key and a search area with no matches. Expect
   "No listings nearby right now" - the only case licensed to say that.
2. **Refresh failed.** Set an invalid key so the server's refresh 401s while
   still answering the check-in normally. Expect "Could not check for listings"
   over "The listings near <area> could not be refreshed just now". Expect it
   **resting, not amber**.
3. **Not configured.** Remove the key entirely. Expect a fixed sentence.
4. In cases 2 and 3, **photograph the screen** and confirm the words
   "rentcast.io", "API key", "MyListings" and any HTTP status code appear
   nowhere on it. This is the actual regression being guarded: a household read
   a vendor's sign-up instructions off a kitchen wall.
5. ~~Confirm the same error text **is** present in the debug stream and on
   `/diag`'s status line for that card, truncated to 120 characters.~~
   **Superseded - see 3c step 4.** This was right when the device could still
   read the sentence. A current server sends no operator diagnostic to a device
   at all, so the text is *absent* from the stream by design and the card's
   `/diag` line reads "upstream refresh failed (reason is on the server, not the
   device)". The reason has not been lost - it moved to the server's own
   operator routes. Against a pre-strip server the original wording still
   applies, which is the only reason it is struck through rather than deleted.
6. Declare a maintenance window while the key is invalid. The card must keep
   saying "could not be refreshed" - **not** "server maintenance". Our server is
   answering fine; it is RentCast that is not, and relabelling that as our
   downtime is a new false claim replacing the old one.
7. Rows present alongside an error: confirm the card still draws the rows and
   says nothing about the error. Deliberate.

### 3b. Known gaps, unfixed, verified as still present

These are the two findings from the survey done alongside `902fc69`. **Both are
open.** Each step below is expected to FAIL today; they are written as tests so
the fix, when it comes, has an acceptance criterion, and so nobody rediscovers
them from scratch.

- **Aircraft, same leak, arguably worse.** `AircraftResult` carries
  `LastRefreshError` on the wire; `Aircraft.cpp`'s ArduinoJson filter does not
  whitelist it, so the card cannot read it even to log it; and the empty path
  draws **"No aircraft within 10 mi right now"** - a flat claim about the sky
  made by a device that may have been told nothing about the sky. Worse than the
  listings case in one respect: an empty sky is plausible far more often than an
  empty housing market, so nobody in the room will question it. *Test, once
  fixed:* break the aircraft feed server-side and confirm the card stops
  claiming the sky is empty.
- **Forecast, softer form.** `WeatherResult` carries the field, the filter does
  not, and the empty path says "No forecast is available yet" - which asserts
  nothing about the weather, so only the diagnostic half is missing. *Test, once
  fixed:* break the weather feed and confirm the reason reaches the stream.
- **Tides and HomeValue cannot have this.** Neither fetches; both arrive
  flattened on the check-in response with no error field on the wire, and
  HomeValue drops its card entirely rather than drawing a claim. No test needed,
  recorded so the survey is not repeated.

Note both open fixes need a filter entry added before the field is readable at
all, and the filter is what decides how much heap the parse takes. That is why
neither was smuggled into the listings change.

### 3c. The card reads `status`, and still works on a server that does not send it

The signal moved. `Status::RefreshFailed` used to be reached by noticing that
`lastRefreshError` was a non-empty string; the server now strips that field from
every device-facing payload (it is operator prose, and one of those sentences
reached a household's wall), so noticing its absence would have put the original
defect straight back. The card reads the server's `status` field instead -
`ProviderStatus`, four values, serialized by name in PascalCase - and falls back
to the old inference only when `status` is not on the payload at all.

So there are **two wire shapes in service at once**, and this section is about
proving the card is right on both. It needs no new hardware state beyond 3a's;
what it needs is the debug stream on and somebody reading it.

**Prerequisites, and one that silently voids the whole section.**

- A device with the listings card in its policy, on `v2026.09.14.0003` or later.
- Server-side control of `MyListings:ApiKey` **and** of which server build is
  deployed at `kServiceHost` (`api.discoveraroundme.com`, compiled in - there is
  no runtime override, so "point it at a different server" means a firmware
  rebuild, not a setting).
- **The remote debug stream must be ON.** The two lines that name the decision
  are `Log::verbose`, which is a complete no-op when nobody is listening. With
  the stream off, every step below is unobservable and the card looks identical
  in all four states. This is the single most common way to waste a bench
  session on this card.
- Content refreshes every 10 minutes (`kContentRefreshIntervalMs`), so budget
  one wait per state change rather than expecting the screen to follow a
  server-side edit immediately. Do not read a stale card as a failed test.

#### 1. Establish which shape the server is actually sending

Watch the stream across one fetch and find the pair:

```
[listings] status='Unavailable' - refresh failed with nothing cached to fall back on
[listings] lastRefreshError absent; refresh treated as FAILED, decided by status (lastRefreshError not consulted)
```

The first line is the whole point of the exercise: **`status='(absent)'` against
a server you know sends the field means the field was dropped by
`Listings.cpp`'s ArduinoJson filter, not by the server.** An un-whitelisted key
never reaches the parsed document, so a missing filter entry is indistinguishable
at every other observation point from a server that never sent anything - it
would look exactly like a permanent, silent fallback that happens to still work.
This line is the only place that distinction is visible. Check it first; if it
reads `(absent)` when it should not, stop, because nothing below is meaningful.

#### 2. Each of the four values, with the branch it must produce

Drive these the same way 3a drives its three states - the server derives `status`
from the key and the cache rather than being told it, so there is no way to set
it directly, which is deliberate.

| Server state | `status` | Card must draw | Must NOT draw |
|---|---|---|---|
| Valid key, market genuinely empty | `Ok` | "No listings nearby right now" | anything about a failure |
| Invalid key, no cached rows | `Unavailable` | "Could not check for listings" | "No listings nearby right now" |
| Invalid key, cached rows present | `Stale` | the real listings, dated | any warning at all |
| Key removed entirely | `NotConfigured` | the fixed "not set up yet" sentence | a vendor, a URL, a settings key |

Row 2 is the regression this commit exists to prevent: on a stripped payload
with the old code the card would have said the market was empty. Row 3's "no
warning at all" is a deliberate choice, not an oversight - see 3a step 7.

The corresponding decision lines, which say what was chosen **and what was
skipped**, are:

```
[listings] empty list AND a failed refresh -> RefreshFailed; Empty NOT taken - ...
[listings] empty list and a SUCCESSFUL refresh -> Empty; RefreshFailed NOT taken - ...
[listings] serving 3 cached listing(s) behind a failed refresh - drawing them rather than a warning, and NOT taking RefreshFailed: ...
[listings] NOT CONFIGURED - resting; neither an empty market nor a failed refresh, and the listings array was not consulted
```

Confirm the line matches the screen. A card and a stream that disagree is a
worse finding than either being wrong alone.

#### 3. The fallback, which is the half that is live on the fleet today

Deploy a server build from **before** the strip (one that still sends
`lastRefreshError` and no `status`) and repeat rows 2-4. Every screen must be
identical to the table above. The stream is what differs, and must say so
explicitly:

```
[listings] status='(absent)' - no status field on this payload - a server that predates the field
[listings] lastRefreshError present; refresh treated as FAILED, decided by lastRefreshError's presence (FALLBACK: no status on this payload)
```

Two shapes, one set of screens. That is the requirement. A device in the field
sees both across a rolling deploy, sometimes minutes apart.

*Deleting this step is how you know the fallback can go:* once no reachable
server predates the strip, `decided by ... FALLBACK` can never be logged again,
and `Listings.cpp` says which names then become a pure deletion.

#### 4. The reason is gone from the device, and that is the fix

Against a current server, in rows 2, 3 and 4:

1. The operator's sentence appears **nowhere on the device** - not on screen
   (3a step 4 already photographs for this), and now not in the debug stream or
   the `/diag` status line either. The card's `/diag` line reads "upstream
   refresh failed (reason is on the server, not the device)".
2. The same sentence is still readable on the server's own operator routes -
   `/diag/providers`, `by-zip`, `for-user`, the POST refresh. **Check this.**
   Losing the diagnostic entirely would be a worse outcome than the leak, and
   "it is off the device" and "it still exists" are two separate claims.
3. Confirm the stream says the absence is expected rather than staying silent
   about it - the "no reason on the wire - status alone said so" line. A blank
   where a reason used to be must not read as a server bug to the next person.

#### 5. The two distinctions that must survive, and are easy to break

1. **`serviceUnreachable` stays false.** Declare a maintenance window while the
   key is invalid (3a step 6 already covers the wording; this is the same check
   restated against the new signal, because the signal changed and the reason
   for the flag did not). The card must keep saying "could not be refreshed" and
   must not say "server maintenance". None of the four `status` values means our
   server is unreachable - every one of them arrives on a well-formed 200 from a
   server that answered - so nothing read out of that field may ever raise the
   flag.
2. **`NotConfigured` is unchanged and must stay calm.** `isConfigured` is a
   boolean, not operator prose, and is **not** stripped - the server asserts it
   present on the device payload. So the "not set up yet" branch was not touched
   and must behave exactly as it did in 3a step 3. Confirm it still rests rather
   than going amber, and that the card does not report a failed refresh for a
   provider that was never asked.

#### 6. A status this build has never heard of

Not reachable against today's server - `ProviderStatus` has four values and the
card knows all four - so this is a read of the code rather than a bench step,
recorded because the next value added is when it matters. An unknown `status` is
treated as a failed refresh, **not** as an absent one: a newer server saying
something specific is not the same as an older server saying nothing, and its
payload will have no `lastRefreshError` to fall back to anyway. The consequence
is that a future fifth value degrades to "could not check for listings" - never
to a claim about the market. If a fifth value is ever added and that is the wrong
default for it, this is the line to change.

---

## 4. A device out of heap restarts as `LOW_HEAP_RESPONSE`, not `UNREACHABLE` (`92cc034`)

The action was always right and the stated cause was always wrong: a
`deserializeJson()` `NoMemory` fed the connection watchdog's counter, so a
device that had reached the server, authenticated and been answered in full
reported a connectivity incident.

### 4a. Producing the condition

This is the hardest state on this page to create deliberately. The reliable
recipe is to make the check-in response large and the heap small at the same
time:

1. Use a device **with no SD card**, so every graphic falls back to a RAM buffer
   held for the process lifetime.
2. Turn the debug stream **on** and leave it on - it is a known heap consumer
   and was the ratchet the `HeapRatchet` work identified.
3. Give the device a policy with several graphic cards and a long announcement
   queue, so the check-in response is at the large end.
4. Let it run. Watch largest8 in the `[health]` line decay.

### 4b. What must be observed

1. `[checkin] out of heap parsing the response, N of 3 ... NOT counted toward
   the unreachable threshold`. The count must be the OOM counter, and the
   connection counter must stay where it was.
2. `err.c_str()` must name `NoMemory` specifically, with largest8 and free heap
   beside it. A response that is genuinely malformed must still print its own
   `DeserializationError` code and must **not** touch the OOM counter - test
   this separately by having the server return truncated JSON.
3. `Graphic::releaseRamBuffers()` must run on the first occurrence only.
   Confirm from largest8 jumping once and from the pictures re-fetching.
4. At the third consecutive OOM the device restarts. On the serial console the
   next boot must read `SOFTWARE_RESET + LOW_HEAP_RESPONSE` - not
   `UNREACHABLE`, and not the bare `LOW_HEAP` the draw watchdog uses.
5. On `/diag`, telemetry must carry `LOW_HEAP_RESPONSE` and the reboot heatmap
   must sort it as a **recovery** restart, not as an unknown reason. This works
   without a server change because the token contains `LOW_HEAP` and
   `RebootHeatmap.Classify()` already keys on that - confirm it, because it is a
   dependency on a server implementation detail that nothing enforces.
6. **The maintenance-window difference, deliberately opposite to the connection
   watchdog.** Declare a window and reproduce the OOM condition inside it. This
   watchdog must still fire. A declared window explains a silent server and
   explains nothing about this device's heap.

---

## 5. `HeapRatchet` names the boot, and the identity now needs six buckets (`02d696c`)

### 5a. On the device

1. With the debug stream off (the stream is the thing being measured, so a run
   with it on measures the instrument), let a device run for at least 15 minutes
   after boot.
2. From the telemetry report, confirm the six buckets sum to
   `bootLargestFreeBlockBytes - largestFreeBlock8BitBytes` exactly. Any
   discrepancy means a phase is missing a scope, and the instrument is designed
   to be caught this way.
3. Confirm `heapPhaseBootBytes` stops changing the moment `setup()` returns -
   compare two consecutive reports.
4. Confirm the splash PNG decode (~45,056 bytes on device 17) lands in **Boot**
   and not in **Idle**. Before this change it was in Idle and made the ~2KB that
   was the real background answer invisible.
5. Confirm `heapRatchetSteps` no longer counts the check-in/service transient
   pair. A run with no genuine decay should report a step count of zero, not two
   per check-in.

### 5b. The wire change, which breaks on the server

**This is a firmware-to-server contract change and the symptom appears on the
server.** A reader still summing five buckets will come up short by whatever
`setup()` cost - most of the session total on device 17 - and the identity check
will fail while the firmware is right.

1. Confirm `heapPhaseBootBytes` is present on the telemetry POST and is sent
   first.
2. **Server side, tracked in the DiscoverAroundMe repository, not fixable from
   here:** the bucket identity check must learn the sixth field, and must
   tolerate its absence so a device still running older firmware does not read
   as broken.
3. Until that lands, treat a failing identity check on `/diag` as expected and
   check the reading end first. This step exists to save the fifteen minutes
   somebody would otherwise spend looking for a firmware bug.

---

## 6. CAL's boot journal (`Journal.h`/`.cpp`, `callog` partition)

**Automated coverage: none, and less than usual.** Everything here is a function
of real flash sectors, a real erase, a real partition table and a real serial
console. A clean compile proves the module builds and that `strings
CAL.ino.bin` contains `CALJRNL1`. It proves nothing about whether a single byte
ever reaches flash.

**Nothing in this section has run on hardware.** The owner chose to build it
ahead of a bench session. Treat every step below as the first execution of that
code path, and do them in order — 6a gates everything after it.

### 6a. The table actually has `callog`, before anything else is believed

Nothing below means anything if the partition is not there, and `Journal` is
deliberately silent-and-harmless when it is missing, so a device with a stale
table will look like a device with a broken journal.

1. Build: `ci/build-firmware.sh`. It must succeed — the generator rejects a bad
   subtype or a misaligned offset, so a build failure here is the table, not
   the code.
2. Decode the table that was actually baked in, rather than trusting the CSV.
   The core ships a bundled binary, which is the one to use — `python` is not
   necessarily on PATH on the build machine:

   ```
   "$ARDUINO15/packages/esp32/hardware/esp32/3.3.11/tools/gen_esp32part.exe" \
       build/esp32.esp32.esp32/CAL.ino.partitions.bin
   ```

   Expect exactly seven rows, `factory` at `0x10000` sized `1408K`, `ota_0` at
   `0x170000` sized `2304K`, and `callog` at `0x3B0000` sized `64K` with subtype
   printed as `153` (decimal for `0x99`). If `callog` is absent the sketch-root
   `partitions.csv` was not picked up and everything below will report "serial
   only".

   **This decode has been done once, on the build of this change, and it
   produced exactly that.** The tool prints a `ValueError: I/O operation on
   closed file` traceback *after* the table — a cosmetic artifact of the bundled
   executable flushing stdout, not a table fault. Read the rows, not the exit
   code.
3. Flash the device: `esptool --chip esp32 --port COMx write_flash 0x0
   build/esp32.esp32.esp32/CAL.ino.merged.bin`.

**Migration check, on a unit that already has a working App installed** — this
is the claim that the table change costs nothing, and it is worth proving once
rather than assuming it on the whole lab:

1. Note the device's installed App version and its WiFi behaviour first.
2. Write the table alone: `esptool --chip esp32 --port COMx write_flash 0x8000
   build/esp32.esp32.esp32/CAL.ino.partitions.bin`.
3. Power-cycle. The device must still hold its secret, still rejoin WiFi without
   the portal, and still hand over to the same App version. It will report
   `[journal] no 'callog' partition...` **no longer** — the table now has it —
   but the *old* CAL in `factory` does not know about the journal at all, so
   expect no journal lines until CAL itself is also reflashed. The point of this
   step is only that nothing was destroyed.

### 6b. A first boot on a blank journal

Erase just the journal so the starting state is known:
`esptool --chip esp32 --port COMx erase_region 0x3B0000 0x10000`.

Open the monitor, then reset:

```
arduino-cli monitor -p COMx -c baudrate=115200
```

Expect, in this order:

- `[journal] no previous boot on record - this is the first boot to keep one`
- `[boot] CAL starting: resetReason=... freeHeap=... largest8BitBlock=...`
- **No** `[boot] journal is SERIAL ONLY this boot` line. If that line appears,
  the partition was not found or the erase failed — stop and go back to 6a.
- then the ordinary ladder: `[display]`, `[identity]`, `[boot] BOOT not held...`

### 6c. The previous boot is dumped automatically, and it fits the splash budget

Power-cycle the device from 6b and watch two things at once — this needs a
person looking at the panel, not just the console.

1. The console must open with
   `[journal] ---- previous boot 1 (sector 0) - press 'd' for all 16 ----`,
   then the entire text of the boot before it, then `---- end of previous boot
   ----`.
2. **The splash must still appear within about two seconds of power.** This is
   the requirement the auto-dump was sized against and the one it could break.
   Time it with a phone camera if it looks marginal. A visibly slower splash
   than a pre-journal build is a failure of this design, not a tuning issue —
   report the measured delay and the sector's byte count together.

Repeat with a *long* previous boot (one that went through the captive portal and
a full download, so its sector is near 4,064 bytes). That is the worst case the
353 ms figure in the README claims.

### 6d. The `d` and `?` commands

With the device sitting still — on the halt screen, on the QR wait, or after
hand-over has failed — type `d` in the monitor.

- Expect `======== journal dump ========`, then every retained boot in
  **ascending `boot=` order**, each headed `---- boot N (sector M) ----`, with
  the newest marked `- this boot, still running`.
- Type `?`. Expect exactly one line: `[journal] d = dump every retained boot, ?
  = this line`.
- Press Enter on its own. Expect **nothing** — stray bytes and newlines are
  ignored deliberately, and a console that answers back to noise is a bug here.

**Note the standing hazard:** opening a serial port with DTR asserted resets
this board. That destroys the state most of this section is trying to read.
Open the port first, then cause the condition.

### 6e. The brick-proof path: reading the journal with no firmware at all

This is the property the whole design is for, so it must be proved with CAL
genuinely unable to run, not merely idle.

1. Get a device into a known state with a few boots of history.
2. Erase the application image so the device cannot boot into anything useful —
   `esptool --chip esp32 --port COMx erase_region 0x10000 0x160000` erases
   `factory` itself, which is the real "CAL will not run" condition.
3. Read the journal straight off the chip:

   ```
   esptool --chip esp32 --port COMx --baud 921600 read_flash 0x3B0000 0x10000 callog.bin
   ```

4. Open `callog.bin` in a text editor. Expect readable ASCII, sixteen
   `CALJRNL1 boot=` headers or fewer, `0xFF` padding after each boot's last
   line, and the sectors in **ring order, not time order**.
5. Recover the device per *Recovering a device from a bare or erased chip* in
   the README.

If step 4 does not produce readable text, the journal has failed at the only
job that cannot be done any other way, regardless of how well 6b–6d went.

### 6f. The ring wraps, and the oldest boot is the one that goes

Power-cycle the device eighteen times, slowly enough that each boot completes.

- Dump with `d`. Expect exactly sixteen boots, and the lowest `boot=` number
  present to be `N-15` where `N` is the newest.
- Expect the sector numbers to be out of order relative to the boot numbers —
  that is the ring having wrapped, and the sort in `dumpAll()` is what hides it.
- Expect **no** gap and no repeat in the sequence numbers. A gap means a sector
  erase failed silently; a repeat means the header scan picked the wrong newest.

### 6g. A device on the old table is not harmed

The claim is that this firmware is safe on a unit that never gets reflashed.

1. Write the **old** partition table (from `git show 94cd5ae:partitions.csv`,
   built into a `.partitions.bin`) over a device running the new CAL, or simply
   test on a lab unit that has not been reflashed since before this change.
2. Boot. Expect exactly one line:
   `[journal] no 'callog' partition in this device's table - serial only, nothing
   from this boot will survive it`, followed by
   `[boot] journal is SERIAL ONLY this boot - nothing here will survive a restart`.
3. Expect the **entire rest of the boot to behave normally** — join WiFi, fetch
   discovery, hand over. Every journal line still appears on serial.
4. Press `d`. Expect `[journal] no 'callog' partition - there is nothing to
   dump`, and nothing else.

### 6h. The install path says what it did — the reason this exists

This is the section the 2026-09-11 incident is about, and it needs a real OTA.

**A successful install.** Mark a new build current on the server, let a device
take it, and read the journal afterwards. It must contain, in order:

- `[update] install? manifestOk=1 isConfigured=1 offered='...' installed='...' -> YES`
- `[install] target ota_0 at 0x170000, 2359296 bytes; image '...' is N bytes`
- `[heap] before the download TLS session: free=... largest8BitBlock=...`
- `[install] about to call Update.begin() - THIS ERASES ota_0 ...`
- ten `[install] NN% - X of Y bytes, freeHeap=... largest8BitBlock=...` lines
- `[install] committed '...'`
- `[updater] handing over to '...' at 0x170000 - boot attempts now 1 of 3`
- `[updater] boot partition set - restarting into the application now`

**Check the heap figures are actually moving.** Three or four progress lines all
reporting an identical `largest8BitBlock` means the instrument is reading
something static, not the heap, and the whole fragmentation hypothesis this was
built to test would be untestable.

**A failed install, deliberately.** The failure the incident describes cannot be
reproduced to order, but three of its candidate causes can:

| Induce | Expect the journal to say |
|---|---|
| Pull the device's WiFi mid-download (unplug the AP) | `[install] download loop ended at X of Y bytes (connected=0)` then `[install] SHORT DOWNLOAD: X of Y bytes arrived - ota_0 is erased and nothing bootable is in it` |
| Put a wrong `sha256Hash` in the server's manifest for a good binary | `[install] SHA-256 MISMATCH after a complete N-byte download`, followed by both digests on their own lines |
| Set the manifest `sizeBytes` larger than `ota_0` | `[install] refused: image is N bytes and ota_0 holds 2359296` — and critically, **no** `about to call Update.begin()` line, because nothing should have been erased |

In the first two cases, confirm the device then shows `Update failed` **and**
that the journal explains which one happened. That is the entire point: the
screen is unchanged, the evidence is not.

### 6i. A broken journal does not break the boot

The one property that must hold no matter what.

1. Make the journal fail at runtime by pointing it at a partition it cannot
   write — the cheapest version is to build with `kPartitionLabel` changed to a
   name that does not exist, which exercises the not-found path, and separately
   to change it to `"coredump"`, which exercises a *present* partition that
   another subsystem also writes.
2. In both cases the device must complete its whole ladder and hand over
   normally. Any change in boot outcome is a defect in the containment, not in
   the journal.
3. Confirm that after a write error the console carries exactly one
   `[journal] ... failed (esp_err N) - journal is serial-only for the rest of
   this boot` line, and **not** one per subsequent log call.

Revert the label afterwards. This test edits the source; do not flash the
modified build onto anything that leaves the bench.

### 6j. A boot that overruns its sector

Hard to induce naturally; the enrollment wait is the realistic route. Leave a
device with no secret sitting in `awaitKeyAssignment()` and change the
server-side enrollment message repeatedly so the change-gated log line fires
each time, until roughly 4 KB has accumulated.

- Expect one `[journal] sector full - the rest of this boot is on serial only`
  line and then nothing more from that boot in flash.
- Expect logging **on serial** to continue unaffected.
- Expect `d` afterwards to report `N lines of this boot were dropped after its
  sector filled`.
- Expect the next boot to start cleanly in the next sector — a full sector must
  not corrupt the ring.

### 6k. Known gaps, stated rather than discovered later

- **The `?`/`d` console is not polled everywhere.** It runs in
  `haltWithFailure()`, in the enrollment wait, and in `loop()`. It does **not**
  run during a WiFi join, an SNTP wait, the captive portal, or a download — so a
  device wedged inside any of those will not answer `d`. Power-cycling and
  reading the auto-dump is the fallback, at the cost of one ring slot.
- **A crash before a log call is not recorded**, by definition. That case
  belongs to the `coredump` partition, which is enabled
  (`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`) and is a separate read with
  `espcoredump.py`.
- **A boot that wedges without crashing leaves a sector with no terminator.**
  The journal will show the last line it managed to write and then stop; there
  is nothing distinguishing that from a boot that ended tidily other than the
  absence of a hand-over or halt line. Read the *absence* of `[updater] boot
  partition set` or `[halt]` as the tell.
- **Sequence numbers are `uint32_t` and the scan assumes they never wrap.** At
  one boot per minute that is about eight thousand years, so this is recorded as
  a known assumption rather than a risk.
- **The journal records the SSID of a network it joins, and never the
  passphrase.** Anyone reading a device's flash gets its network names and its
  MAC. That is a deliberate line and worth re-checking if new log lines are
  added.

---

## What a clean compile does and does not prove

Recorded once, because several commits in this repository lean on it:

- It proves the code builds against the pinned core and libraries, and it
  reports a size. Report sizes against the **real** ceilings from
  `partitions.csv` - `ota_0` is **2,359,296** bytes and `factory`, which is
  CAL's own, is **1,441,792** - never against `arduino-cli`'s generic 1,966,080,
  which is for a partition scheme this project does not use. Note that `ota_0`
  shrank by 65,536 bytes when `callog` was added: a figure of 2,424,832 quoted
  anywhere is pre-`callog` and is now the wrong number.
- `strings` on the built image proves a literal is present. That is a real check
  and worth doing when a change is "the card must now be able to say X".
- It proves nothing about heap, TLS, NVS, timing, the panel, or any decision
  taken at runtime. Every item on this page is on this page for that reason.
