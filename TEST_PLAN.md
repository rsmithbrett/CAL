| binary | `origin/main`, run 41322668927 | this branch, runs 41322667127, 41325454056 and 41328848039 | delta |# CAL / App test plan

This file is the firmware half of the project's testing rule. The rule is that a
change is validated by an automated test wherever one is possible, and that
where it is not, the manual procedure is written down instead of being left in
somebody's head.

**Almost nothing in this repository can have an automated test, and this file
exists to be honest about that rather than to hide it.** The App is an Arduino
sketch cross-compiled for an ESP32-WROOM-32E; its behavior is a function of a
real heap, a real TLS stack, a real NVS partition, a real 2.8" panel and a real
server on the other end of a real network. There is no host-runnable test
target, no injectable clock, and no seam between the watchdogs and
`esp_restart()`. A clean `arduino-cli compile` proves that the code builds and
that a string is present in the image. It proves nothing else, and every commit
message in this repository that says "verified" means exactly that unless it
also names a device.

The server's own `TEST_PLAN.md` (in the DiscoverAroundMe repository) covers
everything on the other side of the wire - the check-in gateway, telemetry
ingestion, the firmware catalog, `/diag`. Where a firmware behavior has a
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
   **The live comparison now meets this; two log strings still do not.**
   - `App/SdStorage.cpp`'s `largestAfterMount >= 16717` was the site that
     mattered - a second definition of the figure deciding whether the mount log
     says the device can open a TLS session, with nothing making it follow the
     constant. It now reads `Http::kTlsRecordBufferBytes`, for the comparison and
     for the figure printed beside it; `SdStorage.cpp` includes `Http.h` to do
     it. Closed.
   - `App/BootDiag.cpp` and `App/HeapRatchet.cpp` still carry 16717 inside
     log-string prose. Harmless to behavior, still a figure that can drift out
     of step with the constant. Open.

   `grep -rn 16717 App/` is the check. Re-run it after any change to
   `kTlsRecordBufferBytes`.
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

## 3. A card tells "nothing to report" apart from "could not ask" (`902fc69`)

`Status::RefreshFailed` on `Listings::Result` and now on `Aircraft::Result`,
plus the removal of a raw upstream error from a drawn string and then from the
device entirely. Listings is the card this started on and 3a-3c are its
procedure; 3d is the same split on Aircraft, where the false claim is the harder
one to catch.

**Automated coverage:** the server side of this is covered by
`Providers.Tests` / `MyListingsServiceTests` and `DeviceFacingPayloadTests` in
the DiscoverAroundMe repository, which are what put `status` on the wire and
keep `lastRefreshError` off it. Nothing tests the card. **Nothing can:** the
behavior under test is a branch taken inside `fetchMine()` on a parsed
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
   operator routes.

   Against a **pre-strip** server the two surfaces now differ, so check them
   separately: the sentence is still in the **debug stream** (bounded to 120
   characters, printed straight from the response and retained nowhere), and it
   is **never** on the `/diag` status line, on either wire shape. The device
   stopped keeping a copy of it at all - `Result::refreshError` is deleted - so
   that line has only its no-words form now. This is the only part of the
   original step that still applies anywhere, which is why it is struck through
   rather than deleted.
6. Declare a maintenance window while the key is invalid. The card must keep
   saying "could not be refreshed" - **not** "server maintenance". Our server is
   answering fine; it is RentCast that is not, and relabelling that as our
   downtime is a new false claim replacing the old one.
7. Rows present alongside an error: confirm the card still draws the rows and
   says nothing about the error. Deliberate.

### 3b. Known gaps, unfixed, verified as still present

These are the findings from the survey done alongside `902fc69`. **One is now
closed, one is still open.** They are written as tests so a fix has an
acceptance criterion and nobody rediscovers the gap from scratch.

- **Aircraft, same leak, arguably worse - NOW FIXED, see 3d.** The empty path
  drew **"No aircraft within 10 mi right now"** - a flat claim about the sky
  made by a device that may have been told nothing about the sky. Worse than the
  listings case in one respect: an empty sky is plausible far more often than an
  empty housing market, so nobody in the room will question it. The card reads
  `status` now and splits that path; 3d is the procedure. **Note the fix is
  conditional on the server:** against a pre-strip server that sends no `status`
  this card still behaves exactly as described above, deliberately and for the
  reasons in `Aircraft.cpp`'s own remarks. 3d step 3 is that case.
- **Forecast, softer form - STILL OPEN, and blocked server-side.**
  `WeatherResult` carries `status`, but the weather *device* routes hand-project
  their payload and do not include it, so there is nothing to whitelist yet. The
  empty path says "No forecast is available yet", which asserts nothing about
  the weather, so only the diagnostic half is missing. This step is expected to
  FAIL today. *Test, once the server sends the field:* break the weather feed
  and confirm the card stops saying "not available yet" over a failed refresh,
  and that `status` names the value in the stream.
- **Tides and HomeValue cannot have this.** Neither fetches; both arrive
  flattened on the check-in response with no error field on the wire, and
  HomeValue drops its card entirely rather than drawing a claim. No test needed,
  recorded so the survey is not repeated.

The remaining fix needs a filter entry added before the field is readable at
all, and the filter is what decides how much heap the parse takes. That is why
neither was smuggled into the listings change, and it is the first thing to
check if the Forecast fix ever appears to do nothing.

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

### 3d. Aircraft tells "clear sky" apart from "could not look"

The same split as 3a/3c, on the card where the false claim is hardest to catch:
an empty sky is plausible most of the time, so "No aircraft within 10 mi right
now" drawn over a failed refresh is unlikely to be questioned by anyone in the
room. Drive these the way 3c drives the listings card - the server derives
`status` from the provider record, so breaking the upstream feed is what
produces each value.

#### 1. The filter, first, because it is the whole read

Before anything else, confirm the stream names a parsed value:

```
[aircraft] status='Ok' - the server's refresh succeeded; its data is current
```

`status='(absent)'` against a server known to send the field means the
ArduinoJson filter is dropping it, and **every check below will pass for the
wrong reason** - the card falls back to "the refresh was fine" and looks
correct. This is the same trap as 3c step 1 and is the first thing to rule out.

#### 2. Each value, and the branch it must produce

| `status` | sightings | expected screen |
|---|---|---|
| `Ok` | some | the normal card, unchanged |
| `Ok` | none | "Nothing overhead right now" - the only state allowed to say so |
| `Unavailable` | none | "Couldn't check overhead just now", then "The flight data service is not answering right now." - **no** claim about the sky |
| `Stale` | some | the normal card, sightings drawn, **no** warning; stream only |
| `NotConfigured` | any | "Aircraft overhead is not showing yet", muted, array not consulted |

Confirm the `Unavailable` screen contains no number of miles and no word
implying emptiness. The failure to look for is the card reverting to the
`Empty` wording, which is the entire defect.

Confirm the headline and the detail line do not repeat each other: the headline
names the outcome, the detail names the cause.

`Stale` must leave the sightings on screen. The stream says so and the panel
does not:

```
[aircraft] serving 3 cached sighting(s) behind a failed refresh - drawing them rather than a warning, and NOT taking RefreshFailed
```

#### 3. A pre-strip server, where this card is deliberately NOT fixed

Point the device at a server that sends no `status`. The card behaves exactly
as it did before this change: an empty list draws "No aircraft within 10 mi
right now" whatever the upstream feed did. **This is the expected result, not a
regression** - `Aircraft.cpp` says why it declined the `lastRefreshError`
fallback that `Listings.cpp` keeps. The stream must say which case it is in:

```
[aircraft] status='(absent)' - no status field on this payload - a server that predates the field
```

*This step is what stops being true when the server ships.* Once no reachable
server predates the strip, re-run step 2 and this one should become
unreachable.

#### 4. Muted, not amber, and still not `serviceUnreachable`

`RefreshFailed` must read as a resting state - the same muted treatment
`NotConfigured` and the two refusals get - not the amber "something is wrong
with this device" styling. Nothing is wrong with the device.

Then declare a maintenance window while the upstream feed is broken. The card
must keep saying "couldn't check overhead" and **not** "server maintenance":
our server is answering fine, and relabelling a provider failure as our own
downtime replaces one false claim with another. Same distinction as 3a step 6
and 3c step 5, on a second card.

#### 5. `NotConfigured` draws a literal, never the server's words

Photograph the `NotConfigured` screen and confirm it reads "Aircraft tracking is
not set up for this home yet." and contains no vendor name, no URL, and no
configuration key. This card has no `isConfigured` field of its own, so `status`
is the only thing that can reach this branch - and the branch exists so the card
never reports an empty sky for an account that has no provider on file.

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

1. `[checkin] could not take the response in for want of heap, N of 3 ... NOT
   counted toward the unreachable threshold`. The count must be the OOM counter,
   and the connection counter must stay where it was. The wording changed on
   2026-09-25 from "out of heap parsing the response", because this branch now
   also catches a failure that never reached the parse.
2. `err.c_str()` must name `NoMemory` specifically, with largest8 and free heap
   beside it.

**2a. `IncompleteInput` below the TLS floor. Added 2026-09-25, and it changes
step 2.** That step used to end: "a response that is genuinely malformed must
still print its own `DeserializationError` code and must **not** touch the OOM
counter - test this separately by having the server return truncated JSON." Half
of that is now wrong, and the half that changed is the reason this section
exists.

A truncated stream sets the OOM counter **when the largest contiguous 8-bit
block at the moment of failure was below two TLS record buffers**, 2 x 16,717 =
33,434. That is the state in which the second buffer had nowhere to go, so the
stream was short for this device's reasons rather than the server's. Confirmed
on device 17 across 74 minutes of stream on 2026-09-15: every failure followed
an rgb565 graphic card draw, at free8BIT around 30,650 with largest 25,588,
while the server had recorded every one of those POSTs and every other device
answered ok in the same minutes.

Two observations, made separately, because each is the other's control:

- **Short stream, low heap.** Reproduce as in 4a, with graphic cards in the
  rotation so the fragmentation actually happens. The line must say the stream
  was short AND that this device could not have held a fresh TLS session, must
  print largest8, and must say it is not counted toward the unreachable
  threshold.
- **Short stream, healthy heap.** Truncate the response deliberately from the
  server, on a device with a large contiguous block. The line must now say the
  server's answer is what was wrong, must **still print largest8** beside the
  error code, and must climb the CONNECTION counter rather than the OOM one.

The second is the one to be careful about. It is what proves the change followed
the heap rather than relabelling every truncation as a device fault, and it is
also where the new rule is capable of being wrong: a genuinely truncated stream
arriving on a reused session while heap happens to be low is now attributed to
heap. That trade is taken deliberately, and `App/CheckIn.h` argues it.

**The inference that looks like a disproof.** Successful check-ins are logged at
`largest8BIT=18420`, BELOW figures at which others fail. That appears to rule the
heap out and does not, because a reused session allocates neither buffer. Do not
conclude from a low-heap success that the classification is wrong. That
inference was drawn once already and was the reason this took ten days to
diagnose.
3. `Graphic::releaseRamBuffers()` must run on the first occurrence only.
   Confirm from largest8 jumping once and from the pictures re-fetching.
4. At the third consecutive OOM the device restarts. On the serial console the
   next boot must read `SOFTWARE_RESET + LOW_HEAP_RESPONSE` - not
   `UNREACHABLE`, and not the `CANNOT_DRAW` the draw watchdog uses (that one
   recorded `LOW_HEAP` until 2026-10-25; see section 12).
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

## 4c. The debug-stream buffer yields to the heap

**This is the test for the loop the stream itself used to cause.** The pending
buffer was capped only by lines and bytes; on device 23 that cost 16,040 bytes
of heap in 160 seconds, and because draining the buffer needs a TLS session
that the buffer had just made impossible, the device could neither send nor
recover and restarted on a loop. `App/Log.cpp` now also caps the buffer at one
batch whenever the largest free 8-bit block is below `kMinLargestBlockBytes`
(`2 x Http::kTlsRecordBufferBytes` = 33,434).

**Nothing here can be checked from a compile.** All of it needs hardware, and
the point of the change is a runtime figure the build output cannot show.

### 4c.1 On a device that is NOT heap-starved (the control)

1. A device with plenty of contiguous heap, debug stream **on**. Confirm from
   the `[health]` line that largest8 is comfortably above 33,434.
2. Behavior must be exactly as before: no heap marker in the stream, and a
   backlog that survives a brief network outage to arrive in full afterwards.
   This is what proves the cap followed the heap rather than simply making the
   buffer smaller for everybody.

### 4c.2 On a heap-starved device (the case)

Produce the condition as in 4a (no SD card, several graphic cards, long
announcement queue) with the stream on, and let largest8 decay past 33,434.

1. The flush must carry the marker naming the count, the measured largest block
   and the requirement - `[N of those dropped for heap: largest free block was
   X and a fresh TLS session needs 16717 twice, ...]`. A silent cap is a
   failure of this test even if the heap behaves.
2. **The marker must not multiply.** Watch several consecutive flushes. One
   marker per flush, with the count resetting after each `HTTP 200`. A marker
   that appears twice in one batch, or a count that climbs while nothing else
   is being logged, means it is being logged rather than composed into the
   batch and is feeding itself.
3. **The stream must keep working, not go silent.** New lines must continue to
   arrive at the ordinary rate; only the depth of backlog is given up. A device
   that stops streaming entirely under heap pressure has failed this, and has
   removed the only diagnostic channel it has at the moment it is needed.
4. largest8 must **stop** its decay rather than merely decaying more slowly.
   The 16 KB is the specific thing being given back.
5. The device must no longer enter the restart loop: no `UNREACHABLE` restart
   attributable to the stream, over a run long enough that the old firmware
   would have rebooted several times (devices 17 and 23 managed six in 62
   minutes).
6. **Recovery is automatic and must be observed.** Once the buffer has drained
   and largest8 climbs back above 33,434, the full 200-line / 16 KB ceiling
   must come back - confirm by causing a brief outage afterwards and seeing a
   deep backlog arrive again.

### 4c.3 Serial, which must not have changed at all

With a USB cable attached to a device in the starved state of 4c.2, the serial
console must show **every** line, including the ones the remote stream dropped.
`Log::line()` writes Serial before the buffer is consulted, so this should be
true by construction - check it anyway, because `Log.h` makes it a hard promise
and this is the change most capable of breaking it quietly.

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

1. Note the device's installed App version and its WiFi behavior first.
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

## 7. CAL records why it restarted the device (`BootDiag.h`/`.cpp` at the repo root)

The thing being tested is a sentence, not a behavior: after this change an App
boot that followed a CAL handover must say **which** handover it was, and the
server must sort it as deliberate rather than unexpected. See
CAL_OTA_DESIGN.md §13 for the design and for why CAL's `BootDiag.h` is a
deliberate partial copy of the App's.

**Nothing below has been run.** The change compiles and that is all that is
established.

### 7a. The two enum copies agree — check this before anything else

The numbers in `BootDiag.h` and `App/BootDiag.h` are the wire format between two
binaries on the same device. They are not compared by any compiler, because the
two sketches are never compiled together.

```
grep -A30 'enum class RestartCause' BootDiag.h     | grep -oE '[A-Za-z]+ = [0-9]+'
grep -A80 'enum class RestartCause' App/BootDiag.h | grep -oE '[A-Za-z]+ = [0-9]+'
```

Both must print, in order and identically — twelve values as of 2026-11-03:

```
None = 0, LowHeap = 1, Ota = 2, Reprovision = 3, SelfTest = 4,
Unreachable = 5, LowHeapResponse = 6, CalHandover = 7,
CalInstalledApp = 8, CalSelfInstall = 9, OrientationChanged = 10,
CannotDraw = 11
```

A difference here is not a style issue. CAL writes the number and the App reads
it, so a drifted value makes the device confidently report the wrong cause for
exactly the restart that installed the drift.

Check the spellings agree too — CAL's `describe()` and the App's
`describeCause()` must produce the same token for the same value, or a journal
read over USB and a telemetry row on the server will name one restart two ways.

### 7b. The ordinary handover — `CAL_HANDOVER`

The most common boot in the fleet, and the one that used to be filed as
unexplained.

1. A device with a healthy App already installed. Power-cycle it.
2. CAL runs its ladder and hands over.

**In the journal (USB), before the restart:**

```
[boot] restart cause recorded as CAL_HANDOVER - the App's next boot reports
       that rather than an unexplained software reset
[updater] boot partition set - restarting into the application now
```

**In the App's log on the boot that follows:**

```
[boot] restart reason: SOFTWARE_RESET + CAL_HANDOVER
```

and **no** `a deliberate restart recorded no cause` line. That line disappearing
is the pass condition for this entire section.

**On the server:** the telemetry row carries `SOFTWARE_RESET+CAL_HANDOVER` and
`RebootHeatmap.Classify()` sorts it **Deliberate**. Confirm against the heatmap,
not against the log — §13.1's whole point is that the log was never the
delivery mechanism.

### 7c. CAL installs on its own initiative — `CAL_INSTALLED_APP`

This is device 17's 2026-09-17 restart, the one with no cause at all.

1. Erase `ota_0`'s header so `haveBootableApplication()` says no, leaving a
   device CAL must fetch an App for without anybody asking.
2. Let it boot with a network.

Expect `[boot] restart cause recorded as CAL_INSTALLED_APP`, then
`SOFTWARE_RESET + CAL_INSTALLED_APP` on the App side.

**The distinction from 7b is the whole value here.** A device reporting
`CAL_HANDOVER` came back running the same binary; one reporting
`CAL_INSTALLED_APP` came back running something different. If both cases report
the same token, this test has failed even though nothing looks wrong.

### 7d. An App-requested update still reports `OTA`, not CAL's cause

The regression this change could most easily cause, and the reason
`recordRestartIntentIfNoneRecorded()` exists.

1. Mark a new App build current on the server.
2. Let a running App take it at check-in. The App records `OTA` and reboots
   into CAL; CAL installs and hands over.

**Expect the journal to show CAL standing down:**

```
[boot] restart cause OTA already recorded - kept, not replaced by
       CAL_INSTALLED_APP
```

and the App to report `SOFTWARE_RESET + OTA`.

If it reports `CAL_INSTALLED_APP`, first-writer-wins is broken and every OTA in
the fleet has lost its more specific cause — a diagnostic made worse by the
change meant to improve it.

Repeat for `Reprovision` and `SelfTest`, which reach CAL the same way.

### 7e. The trampoline — `CAL_SELF_INSTALL` explains all three restarts

Needs §11's bench procedure and a CAL update.

The chain is CAL → staged candidate → new CAL → re-downloaded App: three
restarts before an App runs again. The cause is recorded at the **first** of
them.

Expect exactly one App boot at the end of the burst reporting
`SOFTWARE_RESET + CAL_SELF_INSTALL`, and the journal to show the later hops
deferring:

```
[boot] restart cause CAL_SELF_INSTALL already recorded - kept, not replaced by
       CAL_SELF_INSTALL
```

**What would be wrong:** three separate causes, or a final boot reporting
`CAL_INSTALLED_APP` because the last hop overwrote the first. Somebody reading a
reboot heatmap must get one sentence for the burst, not three.

#### 7e(ii). The same burst, but the App asked for it

The variant that exercises Phase 0's call site rather than CAL's. Set up a newer
CAL on the server so the App learns about it at check-in (`calUpdateAvailable`)
rather than letting CAL discover it on its own initiative.

Expect the **same** final token, `SOFTWARE_RESET + CAL_SELF_INSTALL`, reached by
a different first writer. The App writes the cause from `performCheckIn()` before
`Loader::requestUpdate()`, so the journal on the later hops should show CAL
deferring to a record it did not write:

```
[checkin] server says a newer CAL is current (running '...', attempt 1 of 3) -
          rebooting into CAL so it can fetch and trampoline its own replacement
[boot] restart cause CAL_SELF_INSTALL already recorded - kept, not replaced by
       CAL_SELF_INSTALL
```

**What would be wrong:** a final boot reporting `OTA`. That was the behavior
before the release merge and it is the specific defect this case guards - `OTA`
makes a three-restart, minutes-dark CAL replacement indistinguishable on
telemetry from a routine single-restart App update. See CAL_OTA_DESIGN.md §13.7.

Worth confirming in the same run: this path is *not* therapeutic, so the App
should narrate its boot normally rather than going quiet
(`restartWasTherapeutic()` returns false for `CalSelfInstall`, exactly as it did
for `Ota`) - the change must move the telemetry label without changing anything
the household sees.

### 7f. Power cut between recording and restarting

The failure mode the placement in §13.5 is chosen against.

1. Pull power in the window after `[boot] restart cause recorded as ...` and
   before the device comes back.
2. Restore power.

The App must report `POWERON_RESET + NONE`, **not** `POWERON_RESET +
CAL_HANDOVER`. The App's read side suppresses a stored intent on a power-on
boot, because an intent a power cut interrupted is not the reason the device
came back up. The stored value is still cleared.

### 7g. NVS refuses to open

The case that must be loud rather than silent.

1. Make `Preferences.begin("bootdiag")` fail — fill `nvs`, or point CAL at a
   table with no `nvs` partition.
2. Boot and let CAL hand over.

Expect `[boot] NVS would not open to record restart cause CAL_HANDOVER - the
App's next boot will wrongly report NONE`, **and the handover to happen
anyway.** A device that cannot record why it is restarting must still restart.

Without that line the App reports `+ NONE` and its own message sends the reader
after a missing call site that is not missing.

### 7h. Known gaps, stated rather than discovered later

- **An older App with a newer CAL reports nothing useful.** A CAL writing `9`
  to an App whose enum stops at `6` hits the App's `takeRecordedCause()`
  fallthrough and is reported as `None` — which reads exactly like the defect
  this change fixes. Acceptable only because the two binaries ship together and
  the project is not in production; it stops being acceptable the moment a
  device can run mismatched halves.
- **7a is covered by a text gate, not by a compiler.** `ci/build-firmware.sh`
  extracts both enum bodies and compares them, and section 30 records what that
  reads and the floors under it. The two enums carry the same fully qualified
  name, so no translation unit sees both and there is nowhere to put a
  `static_assert`. The hand check above is still the one that reads the spellings
  `describe()` and `describeCause()` emit, which the gate does not touch.
- **CAL's own restarts are invisible unless an App eventually runs.** A device
  that never reaches a working App reports no cause to the server at all; the
  `callog` journal is the only record and it needs a cable.
- **`CAL_HANDOVER` is recorded on the failed-install fallback path too.** When
  an install fails and CAL falls back to the previously installed App, the cause
  reads `CAL_HANDOVER` — true, because the device did come back running what it
  had before, but it does not say that an install was attempted and failed. The
  journal says so; the telemetry token does not.
---

## 7. One loading screen across the CAL → App handoff

Design and reasoning: `BOOT_SCREEN_OWNERSHIP.md`. The change is CAL-side: on an
immediate handover to an app that has already proved it boots, CAL paints
nothing at all and the App's "Starting" screen becomes the restart's only
loading screen. **Every item here needs hardware.** A clean compile proves none
of it, and the thing it would be most embarrassing to get wrong — a panel that
stays dark because CAL went quiet on the wrong branch — is by definition
invisible to a compiler.

Watch the serial console throughout for the one line that reports the decision:

```
[display] loading screen: swReset=? (rst=?) handover=? bootAtt=? panelFree=? -> ...
```

All four inputs are printed on every boot, including the boots where CAL stays
loud. If that line is missing altogether, CAL never reached the decision, which
is a different finding from CAL deciding to draw. It is worded tightly on
purpose: `Journal::printf()` truncates at 160 bytes and marks it, so a line
discursive enough to explain itself would have its verdict cut off — the prose
lives in `BOOT_SCREEN_OWNERSHIP.md` instead. If you see `...(truncated)` on this
line, a later edit has grown it past the cap.

### 7a. A therapeutic restart shows one screen, not two — the point of the change

- Drive a device into `RestartCause::LowHeap` or `::Unreachable` (see 1a/1b for
  how each is produced) and watch the panel across the whole hop, rather than
  glancing at the end state.
- Expect: the App's "Refreshing — Reclaiming memory, back in a moment" (or
  "Reconnecting"), a brief blank, the App's "Starting", the splash held for its
  three-second minimum, then the first card.
- Expect **no brand mark from CAL and no second "Starting &lt;version&gt;"**. One
  brand mark and one "Starting" for the whole restart is a pass; two of either
  means the bug is still present.
- Expect the decision line to read `-> THE APP`, with `swReset=1 handover=1
  bootAtt=0 panelFree=1`, and to be preceded by the two `[display] panel
  deliberately untouched` / `[display] the App's own "Starting"` lines.
- Time the blank between the App's last frame and the App's "Starting". §5.3 of
  the design trades a bounded dark window for the entire benefit, so the bound
  should be measured rather than asserted. Under about a second is expected.

### 7b. A cold power-on is still loud — deliberately

- Pull the plug and reapply power, on a device with a healthy app installed.
- Expect CAL's brand splash within about two seconds, then "Starting
  &lt;version&gt;", then the App's own screens — i.e. **unchanged from before
  this change.** `bootAttempts` is 0 here too, so this case is separated from
  7a only by the reset reason, and it is the case where somebody is standing
  there holding the plug.
- Expect the decision line to read `-> CAL` with `swReset=0`.
- This is also the check that the two-second rule survived moving
  `Identity::begin()` ahead of the display. If the splash is visibly later than
  it used to be, something between the journal and `Display::begin()` is doing
  more work than the reordering assumed.

### 7c. The panel comes up correctly when CAL never initialised it

The step where a surprise would be most visible, and the one nothing in the
source can settle: on a quiet boot CAL never calls `lcd.init()`, and the App
calls it about a second later, alone.

- Perform 7a and look hard at the App's first frame for garbling, a wrong
  rotation, wrong brightness, or a panel that stays dark.
- Expected to be identical to today, since the App's `Display::begin()` is
  unconditional and has always re-initialised a panel CAL had already
  initialised. But "expected" is not "observed" — run this item first.

### 7d. A failed handover breaks the silence — the recovery path

The item the whole design is defensive about. It verifies that a CAL which chose
to stay quiet can still get a screen onto the glass.

- Force `Updater::bootApplication()` to return instead of restarting. The
  practical way is to make `esp_ota_set_boot_partition()` fail, or to return
  early from `bootApplication()` just after `recordBootAttempt()`, on a device
  that otherwise satisfies all four quiet conditions.
- Expect `[display] handover FAILED - breaking this boot's silence`, immediately
  followed by **"Cannot start application / Restart the device. If this
  persists, contact support." visible on the panel.**
- A dark panel here is a **failing result and a blocker**, not a cosmetic
  defect: it is precisely the bricked-looking-but-recoverable device the design
  exists to prevent, and it would mean the lazy panel start-up in `Display.cpp`
  does not work.

### 7e. An app that will not boot makes CAL loud again within one cycle

- Install a build that panics early in `setup()`, before its first `Display`
  call, on a device whose `bootAttempts` is 0.
- Expect the first boot to be quiet (roughly a second of black), and **every
  subsequent boot to be loud** — splash, "Starting &lt;version&gt;", the full
  ladder — because `bootAttempts` is no longer 0.
- Expect the third attempt to stop handing over at all:
  `[updater] no bootable app: ... has used 3 of 3 boot attempts`, then CAL's
  download ladder.
- The claim under test is "a device cannot be silently dark; it can only be
  briefly dark once". Count the dark boots. More than one is a failure.

### 7f. BOOT pressed and released during a therapeutic restart

- During CAL's window on a therapeutic restart, press BOOT and release it before
  three seconds.
- Expect "Keep holding BOOT to set up WiFi" to appear and then be **covered by
  CAL's brand splash** — CAL goes loud for the rest of that boot.
- Expect the decision line to read `-> CAL` with `panelFree=0`.
- Handing over with the abandoned prompt still on screen is the failure mode
  here. It is the same defect the App had to fix separately in
  `restoreHeldBootScreen()`.

### 7g. The loud paths are untouched

Regression cover for the branches that must not have changed. Each should look
exactly as it did before:

- **OTA install** (`updreq` set): splash, WiFi, "Checking the time",
  "Contacting service", the download progress bar, "Do not unplug".
- **Reprovision** (`returnToLoaderForReprovisioning()`): splash, then the ladder.
- **No app installed**: splash, then the enrollment QR with the hardware
  address, or the not-yet-activated screen.
- **BOOT held through both tiers**: both gesture prompts, the identity erase,
  then the ladder.

### 7h. Known gaps, stated rather than discovered later

- **CAL still clears to black before its splash on every loud path.**
  `Display::begin()` does `lcd.init()` then `fillScreen`. Removing that frame
  would mean reasoning about what an ILI9341's GRAM holds after a chip reset
  plus a panel software reset, which is not something to guess at in the binary
  that cannot be patched over the air. Not fixed, deliberately.
- **CAL knows nothing about the App's `kMaxSilentSelfRestarts` budget.** Past
  three self-restarts the App starts saying "This device keeps restarting
  itself" on the one screen it holds, while CAL stays quiet on that same boot.
  The split is intended — the message belongs to the binary that owns the panel
  when it is read — but it means that case is now narrated by the App alone.
  Worth confirming on hardware that the message is legible for long enough,
  since CAL no longer contributes a frame ahead of it.
- **`bootAttempts` is a one-bit signal for this purpose.** CAL treats 1 and 2
  identically (loud). Deliberate, but it means a device alternating healthy and
  unhealthy boots alternates loud and quiet ones. Not observed; recorded so it
  is not mistaken for a new fault.
- **`RestartCause::SelfTest` is declared in `App/BootDiag.h` and recorded by
  nothing** in `App.ino`. Unrelated to this change and harmless to it — a
  self-test build is requested through `updreq`, so that path is loud either way
  — but it is the same declared-with-no-caller shape as the `Motion::` gate in
  `ci/build-firmware.sh`, and worth either wiring or removing.

---

## 8. Holding BOOT during startup to set up WiFi

CAL commit `2ccaa9c`. The detector polls GPIO 0 from `attemptJoin()`'s wait
loop, which is where the join ladder spends nearly all of its time, so the
gesture is live for the whole window a household is standing there watching it
fail.

**None of this has run on hardware.** It compiles, and the four rows below are
what would say it works. Row 3 is the one that matters most: it's the row where
a wrong answer erases a customer's device.

### 8a. The gesture works mid-ladder

Store a wrong passphrase and power on. Let the first attempt time out, then hold
BOOT during the second.

- Within 3 seconds the panel says to keep holding to set up WiFi.
- The ladder stops rather than finishing its remaining attempts.
- The portal comes up and shows the setup code.
- The journal distinguishes "household asked" from "networks failed".

### 8b. A stray tap does not derail a good join

Store correct credentials and power on. Tap BOOT once, briefly, during the join.

- The join continues and completes.
- No prompt is drawn, or one is drawn and cleared on release with the panel
  restored to what it was showing.
- The timer resets on release, so two taps a second apart do not add up to a
  hold.

### 8c. Eleven seconds during the ladder does NOT erase identity

Hold BOOT for 11 seconds while the join ladder is running.

- The WiFi portal opens.
- **Device identity survives.** It still knows its id and its secret, and after
  the portal takes new credentials it checks in as the same device.
- The 10-second erase tier is reachable only from power-on, so a reader holding
  too long to fix WiFi cannot cross into it.

This is the scenario the feature is riskiest for. A household fixing their WiFi
holds the button for as long as it takes to read the screen, and an erase here
means the device has to be registered again, by somebody who did nothing wrong.

### 8d. Eleven seconds from power-on still erases, as before

Hold BOOT from the moment power is applied, through 11 seconds.

- Identity is erased, exactly as it was before this change.
- The power-on gesture and both of its tiers are untouched.

### 8e. Known gaps

- The handover boundary is unobserved. CAL tears its detector down when the App
  takes over and the App's own `forceUpdateCheckRequested()` takes the pin.
  Nothing has watched a press land in the gap between them.
- The prompt's restore path is only as good as `gPanelUsedForGesture`. A release
  during a panel redraw has not been tried.

## 9. Clearing a notice with no button by tapping the middle of the glass

Edits to `App/CardManager.cpp`, `App/Cards.h` and `App/CheckIn.cpp`, uncommitted when
this was written, so there is no commit hash to name yet. Before this, the only way a
household could clear a banner was to press its Banner Button, which only an action
announcement has. One without a button stayed on the strip until `EffectiveToUtc`
passed or somebody cleared it from the server. Now a tap in the middle of the glass
clears it while the banner is up, and the id rides the next check-in in a
`dismissedAnnouncementIds` array that `CheckInGatewayService` already reads.

**None of this has run on hardware, and no compile figures are recorded here because
the build had not been run when this was written.** The firmware has no automated
tests. A clean compile would prove that the JSON writes and the id copies type-check,
and nothing past that: not that a finger in that band clears a notice, not that the
array reaches the server, not that the band is where a household actually touches. Rows
9a and 9d are the two that matter most. 9a is the gesture itself; 9d is the row where a
wrong answer clears an action announcement without doing whatever its press was bound to
do, which is the one outcome here that costs a household something.

**What this needs on the bench.** A powered device that is checking in, the remote debug
stream on for it, two announcements effective at once - one plain, one with a Banner
Button - both targeting a card whose policy has `allowBanner` set, and the server's
Announcements admin or `/diag` to read the dismissal rows back. Note that the test
devices have generally been unpowered when banner work was written, which is why nothing
in section 9 or in the banner sections of `README.md` has an observed result.

### 9a. A tap in the middle clears a notice with no button

Let a card come up carrying the plain announcement, then tap the inner column of the
panel, below the strip and above the button row.

- `[banner] announcement <id> cleared by tap` appears in the stream, naming the id.
- The big center-screen confirmation is drawn, the same one a button press draws.
- The rotation advances to the next card rather than redrawing the one the banner was
  overlaying, and the next card gets its full dwell rather than being cut short.
- The banner does not come back on a later draw of that card within the same session.
- The next check-in logs `[checkin] carrying 1 cleared announcement(s)` and its request
  body has `dismissedAnnouncementIds` with that id in it.
- The server writes a `CardAnnouncementDismissal` row for the device's owner key, and
  stops listing the announcement on subsequent responses.

### 9b. One notice on several cards is cleared everywhere

Target the plain announcement at two cards that both allow banners. Tap it away on the
first one.

- The second card draws its own ordinary content when its turn comes, with no banner.
- One dismissal row on the server, not two.

### 9c. The gesture lands where the chrome is empty, and the edges still win

- A tap between the bottom of the strip (`kBannerStripHeight`, 100px) and `kButtonRowY`
  (160) clears the notice.
- A tap within roughly 106px of either edge navigates instead, forward or back, and
  leaves the announcement in force. The edge strips run the panel's full height, so
  this is the overlap worth checking rather than assuming.
- A tap on the corner clock or anywhere else that is not a button and not an edge
  behaves the same as the middle, since `Touch::poll()` reports all of it as
  `Hit::None`. Decide on hardware whether that reads as generous or as surprising.

### 9d. An action banner is NOT cleared by a tap

Let the card carrying the action announcement come up. Tap the middle of the glass.

- Nothing happens. No confirmation is drawn, the rotation does not advance, and no
  `cleared by tap` line appears.
- The banner is still up on the next draw.
- The next check-in carries no `dismissedAnnouncementIds` for it.
- Pressing the button still works exactly as it did, riding `pendingActions`, and the
  server records one dismissal from that press rather than two from two routes.

### 9e. An ordinary card is untouched

Tap the middle of a card that is not showing a banner at all.

- Nothing happens: no advance, no confirmation, nothing in the stream. This is the
  behavior `Hit::None` had before the change and it has to survive it.

### 9f. A check-in with nothing cleared is unchanged

Capture a request body from a device that has cleared nothing this session.

- There is no `dismissedAnnouncementIds` key present, not an empty array.
- The body is otherwise identical to what the previous firmware sent. This is the
  backward-compatibility row: the field appearing only when it has something to say is
  what keeps an older server and an older device reading these requests the same way.

### 9g. The id repeats until the server has heard, and a reboot loses it

- After a tap, the id appears on every check-in, not just the first, for as long as the
  server keeps listing the announcement.
- Once the server stops listing it, `setAnnouncements()` stops holding it and the id
  stops appearing. Confirm it does not keep being sent forever.
- Power-cycle the device after a tap but before its next successful check-in. The
  server lists the announcement again, the banner comes back, and a second tap clears
  it. That is the stated cost of keeping the flag in RAM, and this row is what confirms
  the cost is only that.

### 9h. Known gaps

- Nothing distinguishes the tap confirmation from the press confirmation, so a
  household that taps an action banner and sees nothing has no feedback explaining why.
  Whether that reads as a dead screen is an on-glass question.
- `clearedAnnouncementIds()` fills a `kMaxAnnouncements` by 37-byte local inside
  `perform()`, roughly 300 bytes on the loop-task stack that *A stack measurement
  rather than a style preference* in `README.md` is careful about. It is small, and it
  is unmeasured.
- With two or more announcements cycling on one card, a tap clears whichever
  `gBannerOnScreen` names at that instant, which the cursor may have swapped under
  somebody mid-sentence. That sharp edge predates this change and this gesture inherits
  it.
- A server that receives the id and keeps listing the announcement anyway would have
  the device carrying the id for as long as the announcement is effective. Nothing on
  the device bounds that, because the announcement list is the bound.
## 10. Naming a military aircraft on the overhead card

`App/Aircraft.h`, `App/Aircraft.cpp`. The server classifies the aircraft and resolves the
service and the airframe; this side reads four more fields, prefers a military sighting
over the nearest one, and puts the service where the airline's name goes.

**None of this has run on hardware.** The build figures below are from a clean compile on
2026-10-07: App 1,523,584 of ota_0's 2,097,152 bytes, up 1,072 from the previous build.
A clean compile proves the filter entries and the String copies type-check, not that a
Navy helicopter overhead produces the card below.

### What changed

The response filter whitelists `isMilitary`, `militaryBranch`, `aircraftType` and
`registration`. A field missing from that filter is a field the parse cannot see however
faithfully the server sends it, which is why the list is the first thing to check when one
reads back empty.

The featured sighting is the first aircraft in the list with `isMilitary` set, falling back
to element 0. The list arrives in distance order and the server keeps a place for the
nearest military aircraft even when its own cap would have dropped it - so drawing element
0 regardless would spend that place on a sighting nothing ever shows.

The operator line carries `militaryBranch`, plus `aircraftType` after a hyphen when the
provider named one: "U.S. Navy - MH-60 Seahawk". Composed at the call site, so
`showAircraftCard()` needs no new parameter and no geometry moves.

### 10a. A military aircraft nearby is the one drawn

Needs a military aircraft within the configured radius and at least one civil aircraft
nearer. Watch for `[aircraft] response ... callsign=` naming the military one rather than
the nearest, and the card drawing its service where an airline name usually goes.

### 10b. An ordinary sky is unchanged

Every aircraft civil. The card draws the nearest, with its airline, exactly as before. This
is the case that runs every other minute of the day and the one a regression would hide in.

### 10c. A military aircraft with a callsign that says nothing

"TSTR" and the like. The card should read "Military" rather than a guessed service, and
should still draw the airframe when the provider reported a type.

### 10d. An older server

Point the device at a build predating these fields. Every one reads absent, `isMilitary`
reads false, and the card draws what it drew before - the 6-month compatibility case.

### Known gaps

- **The service and the airframe are the server's answer, not this device's.** A wrong
  curated row reaches the glass unchanged; there is nothing here that could catch one.
- **The operator line is longer than it was.** "U.S. Navy - MH-60 Seahawk" is 25 characters
  against an airline name's usual 8 to 12. `layoutText()` bounds it, so the failure mode is
  truncation rather than overrun, but nothing has measured where it truncates.
- **No rotation.** The suggestion offered "rotate through the returned aircraft or show a
  military sighting when one is present" and this takes the second. The card still features
  exactly one aircraft.


## 11. iPhone Wi-Fi provisioning: fast captive portal

The CAL-side setup used to scan neighboring 2.4 GHz networks **inside**
`GET /` while the ESP32 was also serving its setup access point. On a
single-radio ESP32 this can interrupt the iPhone's AP connection precisely
when captive detection asks for the web page. The revised flow scans first,
then starts the AP, and serves an HTML snapshot with no additional radio scan.

**No iPhone hardware test has been run yet.** Both the QR join and the captive
portal must be tested on a real, USB-recoverable bench device before merging
or flashing any customer's factory CAL.

### 11a. The HTTP page never triggers a radio scan

1. Open a serial monitor **before** rebooting so connecting it doesn't reset
   the device unexpectedly: `arduino-cli monitor -p COMx -c baudrate=115200`.
2. Enter CAL Wi-Fi provisioning with an empty remembered network list, or
   hold BOOT to force it.
3. Expect **`[portal] pre-AP scan done in N ms; HTML=M bytes`** to appear
   **before** **`[portal] access point 'Discover-Setup-...' raised`**.
4. With the QR screen displayed, request `http://192.168.4.1/` repeatedly.
   Each request must log **`[portal] root served in N ms`**. The serial log
   must **not** show a second pre-AP scan until provisioning restarts.
5. Record the first-root response time on old CAL and this branch, with the
   same iPhone/router setup. Confirm the portal remains reachable during
   repeated requests and the network picker uses its cached options.

### 11b. iOS opens the portal or the direct URL works

1. On an iPhone, scan the QR once. Expect the network-join prompt for the
   matching `Discover-Setup-XXXX` SSID, unchanged `WIFI:S:...;T:nopass;;`.
2. If iOS opens its captive-network sheet automatically, the form must appear
   without a long spinner. Look for `[portal] probe /hotspot-detect.html`
   (or another `[portal] probe ...`) followed by the root request.
3. If the captive sheet **does not** appear, join the temporary Wi-Fi in
   Settings if necessary and open the **address displayed below the QR**:
   `http://192.168.4.1` (or the actual `WiFi.softAPIP()`).
4. The probe response must be HTTP 302 with an **absolute local**
   `Location: http://192.168.4.1/`, plus no-store/no-cache headers.
   The root response must carry no-store/no-cache headers too.
5. Repeat on at least two supported iOS versions; test one Android phone
   using its `/generate_204` probe as a regression check.

### 11c. Scan errors, hidden SSIDs and correct credential selection

1. With at least two visible home 2.4GHz networks, verify the dropdown
   matches those seen by the ESP32, with signal strength shown.
2. On a hidden network or when scanning returns no networks, enter an SSID
   manually; it must take precedence over the dropdown.
3. Put `<`, `&`, and `"` in a test SSID. It must render as text, and
   selecting it must still save the original unescaped SSID.
4. Submit an empty SSID or one over 32 bytes: rejected with 400.
5. Submit correct credentials: CAL closes its temporary AP and rejoins
   the selected home network. Test failed credentials separately; existing
   re-provisioning recovery must remain available.
6. Test two nearby devices simultaneously: the SSID suffix and QR must
   distinguish them.

### 11d. Scope and factory-flash protection

This patch deliberately **does not change** the setup AP from open to WPA2,
does not add HTTPS, and does not change the `/save` post-submit window. Those
are security and flow changes requiring a separate decision and device test.

On this repository, merging into `main` triggers the build-and-publish
workflow for **both CAL and App**. CAL lives in the factory partition and
requires a deliberate USB flashing/recovery process. A successful source
review is not evidence that iOS captive detection works on hardware.

---

## 12. The cannot-draw watchdog names itself, and stops after three tries

Two changes, both bench-observable, and the second one is the one worth being
careful about because its correct behavior is a device **not** doing something.

### 12a. The cause reads `CANNOT_DRAW`

Reproduce the trigger the cheap way: give a device one picture card whose asset
cannot decode on it, and no second picture card. A deliberately corrupted
cached file does the job on any device.

Device 39's shape used to be the worked example - its only image-bearing card
is `aircraft`, and the airline logo was a PNG needing 45,056 contiguous bytes
against a largest block that has never exceeded 36,852. **That example no
longer reproduces**, twice over: the server re-encoded every airline logo to
RGB565 on 2026-10-26, which needs a 5,120-byte band buffer and no decoder, and
a refusal on a cached file under 16,384 bytes now drops it once per boot so the
next fetch collects the new encoding (see `reFetchIfCheap()` in `Assets.cpp`).
A device still holding the old PNG reaches `CANNOT_DRAW` on the first pass and
not on the second.

1. Let it run past `kHeapCheckGraceMs` (3 minutes). The `[health]` line must
   report `consecutive failed draws=N/6` and must print the largest 8BIT block
   beside it as context.
2. Past 6 the device restarts. The next boot must read
   `SOFTWARE_RESET + CANNOT_DRAW`. **Not `LOW_HEAP`** - that is the whole
   change, and a `LOW_HEAP` here means the build under test predates it.
3. On `/diag`, telemetry must carry `CANNOT_DRAW`. Until the server adds the
   token to `RebootHeatmap.Classify()`'s recovery branch the heatmap will sort
   it as **Deliberate** rather than Recovery, which is the known gap - confirm
   which of the two it shows and record it, because that is the evidence for
   whether the server change has landed.
4. A device restarting for a genuine heap shortage must still be able to read
   `LOW_HEAP`, and a stored row from older firmware must still read `LOW_HEAP`
   unchanged. The value was kept, not repurposed.

### 12b. The restarts stop at three

Same setup, left running for at least two hours.

1. Count the restarts. There must be **three** and then no more. The streak is
   held in NVS under the `bootdiag` namespace, so it survives the reboot it is
   counting.
2. At the fourth time the count passes 6, the `[health]` line must say the
   device has already restarted three times for this and is staying up. That
   line appears **once per boot**, not once a minute.
3. The device must keep checking in and keep reporting telemetry while capped.
   A capped device that goes quiet is a different fault.
4. Pull the power and let it come back. The budget must still be spent - a power
   cycle does not refill it, because the ceiling did not change.
5. Now fix the decode (uncorrupt the file, or give the device a second picture
   card that draws). Once a draw succeeds and the device has been up past
   `kCannotDrawBudgetResetMs` (30 minutes) with a zero failure count, the streak
   must clear. Prove it by breaking the decode again: the watchdog must restart
   the device once more rather than staying capped forever.

Step 5 is the one that can be wrong in the quiet direction. A streak that never
clears turns this watchdog off permanently on any device that has ever hit it,
and nothing on screen would say so.

## 13. What a price drop dropped from, on the featured-listing card

`App/FeaturedListings.h`, `App/FeaturedListings.cpp`, `App/Display.cpp`. The server reads
the figure Compass struck through and sends it as `previousPrice`; this side draws it as a
second 9pt muted line under the 18pt price, reading "was $465,000".

**None of this has run on hardware.** The build figures are from a clean compile on
2026-10-25: App 1,568,599 of ota_0's 1,966,080 bytes (79%), 114,156 bytes of global data.
A clean compile proves the member parses and the layout call type-checks, not that the two
figures look right on glass.

What it has run on is the simulator mirror. `FeaturedListingPanel` in the server repository
is a statement-for-statement port of `showFeaturedListingCard()`, and twelve images of this
card at 240x320 and 320x240, with and without a picture and with and without an action
bound, are in `docs/ia/screens/` there. That is closer to evidence than a compile and it is
still not a device.

### What changed

`Card::previousPrice` is an `int32_t` defaulting to -1, read the same way `price` is: a
null or an absent member lands on -1 and nothing is drawn. The sender decides whether a
price moved and names both figures, so there is no comparison against `price` here and no
reading of how old the listing is.

The line costs 18px plus a 4px gap, taken from the same budget every block below the
headline measures against, and the subhead yields to it as it already yields to the price.
It advances `y` by 20 rather than the 22 the blocks above use: 22 cost a flat card with no
picture two fact rows and left a row of white above the freshness line.

### 13a. A reduction draws both figures

Needs a listing whose last email was a price drop. The card reads the address, then the
asking price large, then "was" and the older figure smaller and grey beneath it. Both
figures grouped with commas - an ungrouped one is the `formatPrice` defect that reached a
real card once already.

### 13b. A listing with no reduction is unchanged

Every other card, which is almost all of them. No second line, and the title, address,
price and fact rows land exactly where they did. This is the case that runs most of the
time and the one a regression would hide in.

### 13c. A flat display with a button on the card

320x240 with an action bound drops the content floor to y=154, where the price already
ends at y=158 with a picture. The "was" line is left out and the price is kept, and the
device says so on its stream: `[display] featuredlistings: no room for the previous price`.
A display standing on its end has the room and draws it.

### 13d. An older server

Point the device at a build predating the field. `previousPrice` reads absent, lands on -1,
and the card draws what it drew before - the 6-month compatibility case, from this side.
The other side of it is asserted in the server repository by
`FeaturedListingWireCompatibilityTests`.

### Known gaps

- **Nothing here has been on a panel.** Every figure above is from the mirror or the
  compiler.
- **Whether the "was" line reads as part of the fact rows below it.** It is 9pt grey and so
  are they, and on a flat card with no picture it sits directly above "Bedrooms". The
  pictures say it reads as a price because it is left-aligned prose against the facts'
  label-left value-right columns, which is a judgement rather than a measurement.
- **A listing whose price a person corrected.** The server clears the previous price when
  somebody sets the price by hand, so this device should never see the pair. It is not
  guarded here.

## 14. The App's first boot on the Panellan

Nothing has ever drawn a card on the Panellan. The App compiles for it and fits
its partition, and the paragraphs below are what the compile cannot answer. They
are in the order to check them, because the early ones make the later ones
unreadable if they fail.

**Four of these are now compile-time rather than manual.** `App/Sound.h` and
`App/CardManager.cpp` carry a `static_assert` against
`MSPI_IOMUX_PIN_NUM_CS1`..`MSPI_IOMUX_PIN_NUM_MOSI` from the SDK's own
`soc/spi_pins.h`, so a peripheral pin in the S3's flash-and-PSRAM range fails
the build instead of the board. `App/PowerProbe.cpp` and `App/Motion.cpp` carry
one each against the SDK's `soc/adc_channel.h`, so a pin this target's ADC1 does
not reach fails the build instead of returning a figure nobody can interpret.
Put any of the four pins back to its fleet-board value and the S3 compile stops
with the pin named - 14.9 quotes what it says. They cover the four pins this
repository had. They cover nothing about a pin somebody adds in the 33-37 range,
which an octal part bonds and this quad part does not.

### 14.1 Does anything light up at all

`Display::begin()` asserts GPIO47 directly before `panelDevice.init()`, so a
black panel after that write is the bus and not the backlight. LovyanGFX's own
default brightness is 127 and `Display::begin()` raises it to 255 one call
later, so a panel stuck at half brightness means the second call did not run.

### 14.2 Which way up, and is anything cut off

`setRotation(1)` plus the config's `offset_rotation = 2` gives internal rotation
3, and the viewport reads back 320x240 - the same figures the fleet board gives,
arrived at differently. So a card laid out in absolute coordinates has the room
it expects. What the compile cannot say is which way up. LovyanGFX's table
carries two variants of the fleet board, and they do not agree: the ILI9341
variant sets the panel's `offset_rotation` to 2 and lands on internal rotation
3 like the Panellan, while the ST7789 variant leaves it at 0, lands on 1, and
puts the 2 on its touch config instead. Each is right for its own glass.
`offset_rotation` in `BoardPanellan.h` is the knob if the picture is upside
down, and the value there is the vendor's. A fleet unit with the ILI9341 panel
is the one worth standing the Panellan beside.

### 14.3 Red and blue, and black and white

The Panellan config sets `rgb_order = true` where the fleet board's panel
defaults to BGR, so the two boards write different MADCTL color-order bits. It
also sets `invert = true` and carries `0x21` in the vendor's init list, where
the fleet board inverts neither way. Both values are the vendor's. The check is
one look at a card with known colors: a warning amber that reads blue is the
color order, and a white day card that comes up black is the inversion.

### 14.4 A logo with a soft edge

This board is write-only - RD is tied off and `readable = false` - and
LovyanGFX's PNG decoder has a separate path for that case. It thresholds alpha
at 128, draws every pixel above it opaque, skips the rest, and does it one
`writeFillRectPreclipped(x, y, 1, 1)` at a time. So a logo with an anti-aliased
edge draws with a hard edge and draws slowly, where the same file on the fleet
board blends against a read-back line. Look at an airline logo and a
featured-listing photo, and time the draw.

### 14.5 The asset path, which has no SD card under it

The vendor's pin header gives every SD pin as -1. The RGB565 rung needs a card
and refuses without one, so every picture on this board comes down the
direct-to-RAM fallback and is decoded by PNG or JPEG. `Graphic.cpp` says in a
log line that the RAM fallback has no in-rect draw, so a picture card with a
button bound to it draws full-screen with the button row over its bottom edge.
Expect that line and check the picture against it.

### 14.6 Touch

The FT5x06 reports over I2C port 1 on GPIO8/9, and reports nothing if
`Touch_FT5x06::_check_init()` reads a zero vendor id. The arithmetic downstream
is sound - the affine map takes raw 0..240 by 0..320 into the panel's own 0..319
by 0..239, and `Panel_Device::convertRawXY` folds the panel's `offset_rotation`
in - but whether a finger on the left edge reads as the left edge is a fact
about the glass. Tap the two edge strips and the middle; left is previous on
every panel.

### 14.7 What the heap figures mean here, which is not what they mean on the fleet

`MALLOC_CAP_8BIT` spans PSRAM on this target, so every log line and telemetry
field reporting an 8BIT largest free block reports a figure out of the PSRAM
pool rather than out of internal RAM, and it will dwarf anything the fleet has
ever sent. `logHeapSnapshot()` prints the
`MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` pair beside it, as
`INT|8BIT free= largest=`, and that is the one to read. The 45,056-byte PNG
decoder scratch is a plain `malloc`, which with
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096` prefers PSRAM, so the fleet's
draw-or-not-draw threshold does not apply here.

### 14.8 TLS, because this binary's mbedTLS is not the fleet's

The S3 half of the installed core is stock: `MBEDTLS_SSL_MAX_CONTENT_LEN 16384`,
no `ASYMMETRIC_CONTENT_LEN` and no `MBEDTLS_DYNAMIC_BUFFER`. A session holds
16,717 twice, 33,434 bytes, for its whole life. Read it off
`App/build/esp32.esp32.esp32s3/sdkconfig`, not off the esp32 one beside it.

### 14.9 The two ADC pins, and what the core does with a pin it has no channel for

**Fixed.** `App/PowerProbe.cpp` read GPIO34 and GPIO35 with
`analogReadMilliVolts`, and `App/Motion.cpp` read GPIO35 the same way. Neither
pad has an ADC channel on the S3: `soc/adc_channel.h` for that target gives ADC1
as GPIO1-10 and ADC2 as GPIO11-20, read on 2026-10-28 from
`tools/esp32s3-libs/3.3.11/include/soc/esp32s3/include/soc/adc_channel.h`. Both
modules now take their pin from `App/AdcPins.h`, which derives it from that
table, and both return before calling the core when the table has no entry. The
figures telemetry carries - `adc34Millivolts` and `adc35Millivolts` - are 0 on
that board, as they were, and `Motion::capabilityToReport()` returns an empty
string, as it did. The fleet board's generated code for both files is unchanged;
see "what was compiled" below.

**The previous version of this section said both pins come back as 0 mV. That
was not established, and it is not what the core does.** In core 3.3.11,
`__analogReadMilliVolts()` in `cores/esp32/esp32-hal-adc.c` calls
`adc_oneshot_io_to_channel()` without assigning the result to the `err` it then
tests, so the `log_e("Pin %u is not ADC pin!")` branch beside it cannot be
reached. `adc_unit` and `channel` are left holding whatever the frame held, and
`adc_unit` then indexes the file-scope `adc_handle` array, which has
`SOC_ADC_PERIPH_NUM` (2) entries. The shipped S3 image agrees: disassembling
`App/build/esp32.esp32.esp32s3/App.ino.elf` at `__analogReadMilliVolts` shows
the `call8` to `adc_oneshot_io_to_channel` followed by no test of its result,
and four instructions later `l32i a8, a5, 0` loads the unit from the
uninitialized frame slot and `subx8`/`slli` scale it by the array's 28-byte
stride. `__analogRead()` in the same file does assign `err` and does refuse the
pin, so the two entry points differ on the same argument.

That is why "0 mV reads as below the 1500 mV threshold, which is no motion" was
the wrong thing to rely on. The reading was never taken; the answer was a
property of the frame. `Motion` now returns at a pin gate, so what it reports is
that nothing was measured rather than that nothing moved - the same empty
capability string either way, reached for a reason that does not move when a
core version or a threshold does.

**Still carrying the fleet board's chip select: `App/SdStorage.cpp`.** It
compiles GPIO5 into its mount path. The gate that keeps that pin from ever being
driven on the S3 is `kBoardHasCardSlot` in commit `e6afc9a` on branch
`fix/s3-heap-pool-and-sd-gate`, which is a sibling of this branch off `add4c66`
and not an ancestor of it. No second guard was added here, because two gates on
one mount with nothing comparing them is worse than one. Whichever of the two
branches lands second has to keep that gate.

The pin is not claimed before that gate, which is the part worth checking rather
than assuming. `kChipSelectPin` is a `constexpr`; the global `SD` is
`SDFS SD = SDFS(FSImplPtr(new VFSImpl()))` and its constructor sets `_pdrv` and
nothing else; the global `SPI` is `SPIClass SPI(FSPI)` and its constructor sets
every pin field to -1. `SD.begin()` is the only call in `App/` that reaches
either object's hardware, and the gate returns before it. On the S3 that call
would claim GPIO5 plus the variant's default SPI pins - `SS 10`, `MOSI 11`,
`SCK 12`, `MISO 13` in `variants/esp32s3/pins_arduino.h` - which are four of the
six pins the EXT header brings out.

Two things nobody has read a datasheet for: that GPIO5 is this board's RS485 RTS
line, which is secondhand and is not in `BoardPanellan.h` or in the vendor
specification the server's `OPEN_ITEMS.md` transcribes, and what GPIO34 and
GPIO35 are wired to on this board at all. The module is a quad-PSRAM part
(`CONFIG_SPIRAM_MODE_QUAD=y`), so 33-37 are not bonded to the flash and PSRAM
bus and the two pads are ordinary digital GPIOs there. Neither question changes
any of the above, because nothing drives any of those pins now.

#### What was compiled, and what the guards said when they were made to fail

Not a full image. A compilation database per FQBN into its own `--build-path`,
then the touched translation units compiled on both targets from the working
tree: `Motion.cpp`, `PowerProbe.cpp`, and the two files that include their
headers, `Telemetry.cpp` and `App.ino.cpp`. All eight compiles exit 0. That
proves these files on both targets and says nothing about the whole image or its
size.

The fleet board's code is unchanged rather than only believed unchanged. Both
files were compiled for `esp32:esp32:esp32:PartitionScheme=min_spiffs` at
`17e990a` and again with this change, and compared: every non-debug section is
identical in size, `objdump -d` is identical line for line, and
`.rodata.str1.1` is identical. Only `.debug_*` moved, which is the comments.

Each guard was then made to fail. Restores were verified by SHA-256 against a
copy taken before the first perturbation, because a restored file keeps its old
timestamp and a build that skips the rebuild reports the perturbed object's
result.

GPIO34 written back into `PowerProbe.cpp` on the S3:

```
App/PowerProbe.cpp:24:21: error: static assertion failed: this target's ADC1 does not reach GPIO34 or GPIO35 - soc/adc_channel.h gives ADC1 as GPIO1-10 and ADC2 as GPIO11-20 - so this module holds no pin for either
   24 | static_assert(kPinA == AdcPins::kNoPin && kPinB == AdcPins::kNoPin,
      |               ~~~~~~^~~~~~~~~~~~~~~~~~
App/PowerProbe.cpp:24:21: note: the comparison reduces to '(34 == -1)'
```

GPIO35 written back into `Motion.cpp` on the S3:

```
App/Motion.cpp:26:20: error: static assertion failed: this target's ADC1 does not reach GPIO35 - soc/adc_channel.h gives ADC1 as GPIO1-10 and ADC2 as GPIO11-20 - so this module holds no sensor pin
   26 | static_assert(kPin == AdcPins::kNoPin,
      |               ~~~~~^~~~~~~~~~~~~~~~~~
App/Motion.cpp:26:20: note: the comparison reduces to '(35 == -1)'
```

The fleet board's half of each pair bites too. The probe pin moved to GPIO27,
which is ADC2 on that part and so not a pin this firmware can sample while WiFi
is up:

```
App/PowerProbe.cpp:28:21: error: static assertion failed: the probe pins are not GPIO34 and GPIO35 on this target, which is the pair PowerProbe.h's reasoning and the adc34Millivolts and adc35Millivolts telemetry fields are written against
   28 | static_assert(kPinA == 34 && kPinB == 35,
      |               ~~~~~~^~~~~
App/PowerProbe.cpp:28:21: note: the comparison reduces to '(-1 == 34)'
```

And the sensor pin moved to GPIO34, which is a valid ADC1 pin there and would
have compiled under a weaker guard:

```
App/Motion.cpp:30:20: error: static assertion failed: the sensor pin is not GPIO35 on this target, which is the pad the measured thresholds in this file and PowerProbe's own GPIO35 figures both describe
   30 | static_assert(kPin == 35,
      |               ~~~~~^~~~~
App/Motion.cpp:30:20: note: the comparison reduces to '(34 == 35)'
```

The S3 objects carry no ADC read at all afterwards. `nm -u` on `Motion.cpp.o`
lists no `analog*` symbol, and `PowerProbe.cpp.o` lists only
`analogSetAttenuation`, which is kept: with `pin == -1` it configures no channel
and sets the two file-scope defaults, so it is safe on a board with no ADC pin
and keeps one statement of this firmware's attenuation on both boards.

## What a clean compile does and does not prove

Recorded once, because several commits in this repository lean on it:

- It proves the code builds against the pinned core and libraries, and it
  reports a size. Report sizes against the **real** ceilings from
  `partitions.csv` — `factory`, which is CAL's own, is **1,703,936** bytes
  (`0x1A0000`) and `ota_0` is **2,097,152** (`0x200000`) — never against
  `arduino-cli`'s generic 1,966,080, which is for a partition scheme this
  project does not use. **Do not quote these figures from memory; read
  `partitions.csv`.** They have now moved twice, and every stale version of them
  is still quoted somewhere:
  - pre-`callog`: `ota_0` 2,424,832.
  - after `callog` was carved out: `ota_0` 2,359,296, `factory` 1,441,792.
    This document itself carried that pair until 2026-09-16.
  - current, after *Give the partition that cannot be resized in the field some
    room*: `factory` grew to 1,703,936 and `ota_0` shrank to 2,097,152. That
    trade was made deliberately — `factory` cannot be re-sized without a USB
    cable, and `ota_0` can be refilled over the air — so `factory` is the number
    with headroom and `ota_0` is the one to watch.

  `ci/check-partition-fit.sh` reads the table rather than holding constants, for
  exactly this reason. A number typed into prose is a number that will be wrong
  the next time somebody re-tables a device.

  **There is more than one table now, so the check reads the one that build
  used.** The 8MB Panellan has its own layout in
  `partitions-panellan-8mb.csv`, and the board wrappers select a board by
  copying its table over `partitions.csv` for the length of one compile. The
  check therefore reads `partitions.csv` out of the build output directory,
  where `arduino-cli` puts the table it compiled against, beside the image it
  produced. Each board's figures come from that board's own table, and reading
  the sketch root instead would measure one board's image against the other
  board's ceilings. Read the figures for a board off the table in that board's
  build directory, the same place the check does. Four ceilings are checked:
  CAL against `factory` and, for OTA staging, against `ota_0`; the App against
  `ota_0`; and on the 4MB board only, CAL against the pre-re-table field
  ceiling that the deployed units are actually running.

  `ci/build-firmware.sh` runs the check for **both** boards as of 2026-10-28,
  and `compile-cal-s3.cmd` and `compile-app-s3.cmd` run the Panellan's half of
  it locally. It used to run the 4MB board's three only, which left the two S3
  images measured on one workstation and nowhere else. The check matters more
  for that board than for the other one: the S3 FQBN passes
  `PartitionScheme=custom`, which raises `arduino-cli`'s own refusal to
  16,777,216 bytes, so nothing but this check stands between an oversized S3
  image and a published release.

  **Measured 2026-10-28**, read off the check's own output against the table in
  each image's own build directory:

  | Image | Partition | Size | Ceiling | |
  | --- | --- | --- | --- | --- |
  | `CAL.ino.bin`, Panellan | `factory` | 1,286,784 | 2,097,152 | 61% |
  | `CAL.ino.bin`, Panellan | `ota_0` | 1,286,784 | 2,621,440 | 49% |
  | `App.ino.bin`, Panellan | `ota_0` | 1,501,264 | 2,621,440 | 57% |

  Both images came out **smaller** than their ESP32 counterparts, which the 8MB
  table's own header predicted the other way round. Nobody should shrink
  `factory` off this: it is the partition that needs a cable to resize and it
  holds the recovery image. These figures are from a local build of an earlier
  commit than the CI change, and the first CI build will produce its own - a
  local binary and a CI binary of one commit are not the same thing, and
  `versions.txt` and the sdkconfig are how to tell them apart.

  **The CI pass swaps the table in and restores it on any exit.** Both copies
  move, the root one and `App/partitions.csv`, and the restore comes out of git
  rather than out of a backup so a corrupt backup cannot pass it. A restore
  that does not take fails the script even when the compiles succeeded: an 8MB
  table left under the 4MB name produces an `App.ino.merged.bin` that re-tables
  whatever it is flashed to. The script also refuses to start when either table
  already differs from `HEAD`, so an interrupted run cannot make the mix-up
  permanent by backing up the wrong table on the next one.
- `strings` on the built image proves a literal is present. That is a real check
  and worth doing when a change is "the card must now be able to say X".
- It proves nothing about heap, TLS, NVS, timing, the panel, or any decision
  taken at runtime. Every item on this page is on this page for that reason.


## Bench-only WPA2 captive portal A/B (2026-10-24)

This is an **experiment**, not an instruction to publish to the fleet. See
[`WPA2_PORTAL_BENCH.md`](WPA2_PORTAL_BENCH.md) for design and recoverability rules.

- Build the draft branch through the PR's `Build CAL bench image (no release)`
  workflow and download **`CAL.ino.merged.bin`**, not the bare `CAL.ino.bin`.
- On a USB-recoverable original ESP32 unit, flash through the site's opt-in
  `/cal?bench=1` loader after its companion server PR has been reviewed and
  deployed. The ordinary `/cal` page must still serve the current catalog CAL.
- Verify first run generates a 24-hex WPA2 AP password in NVS. The same SSID
  gets the same password after a power cycle. A storage failure must **fail
  closed** and never create an open fallback AP.
- iPhone Camera scans the *single* `WIFI:T:WPA;S:...;P:...;;` QR. Join should
  not request manual typing of the setup AP password. Verify Wi-Fi association,
  its timestamp, Apple `/hotspot-detect.html` requests, GET / responses, **visible
  captive UI**, form save and successful home Wi-Fi join.
- Cross-check Settings → Wi-Fi connection on the same handset. Collect at least
  10 Camera-started trials per open and WPA2 variant; compare median/p90 visible
  UI delay and completion, not merely HTTP-handler latency.
- Inspect a wireless capture to verify the household password is not exposed
  in plaintext over the air by the open temporary AP. Local provisioning remains
  HTTP **over a WPA2-protected Wi-Fi link**, not HTTPS.
- Test stale remembered-network handling, setup timeout, BOOT-hold recovery,
  Android provisioning and old App handover. Record firmware artifact SHA,
  original ESP32 model and iOS version for each result.

**Do not conclude that WPA2 fixes iOS's Camera-to-CNA transition from successful
compilation or from the presence of an HTTP request.** Require real physical
phone testing and keep the two-stage QR/URL fallback available as a separate
experiment.

## 14. The heap figure that gates TLS says which pool it measured

`App/HeapPools.{h,cpp}` is new. It defines `kTlsCaps`
(`MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`) as the mask every TLS-relevant figure
is taken with, and `anyPoolLabel()` as the string every line reporting a
bare-capability figure prints beside it. `Http`, `HeapRatchet`, `BootDiag`,
`SdStorage` and `Telemetry` were changed to use it.

**No test was added, and no test failed first.** There is no test harness in this
repository - the App and CAL are Arduino sketches with no unit-test target, and
the suites this project runs live in the server tree. What exists instead is a
compile-time guard and a bench procedure, and neither is a substitute for the
other.

### What is actually checked by a build

`HeapPools.h` carries:

```
#if !defined(CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC)
#error "HeapPools::kTlsCaps assumes CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC. ..."
#endif
```

That is the premise the internal mask rests on: mbedTLS allocates through
`esp_mbedtls_mem_calloc`, which under this option takes
`MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`. If a core tree ever sets
`DEFAULT_MEM_ALLOC` or `EXTERNAL_MEM_ALLOC` instead, mbedTLS starts drawing from
PSRAM, the internal mask stops describing the pool a handshake is graded
against, and every figure in the list above would be measuring the wrong thing
while still compiling.

**The guard was proven to bite before the change was accepted**, by inverting its
sense to `#if defined(...)` and confirming the build stops with that message
rather than succeeding. Restore it with `git checkout -- App/HeapPools.h` and
re-run; verify the restore against `git diff` rather than against a copy, and
remember that a restored file can keep its old mtime and let the build skip the
rebuild.

### What a build does not prove, and what the bench has to settle

1. **That the two masks really are the same number on the fleet board.** Three
   places in this repository already assert it - `HeapTrace.h`, the `[heapdiag]`
   lines, and the device 17 capture recorded in `Telemetry.cpp` - and the chip
   argument is sound (`esp_psram_init()` finds nothing on an ESP32-D0WD-V3, so
   no SPIRAM region is registered). It has not been re-measured since the
   change. `logHeapSnapshot()` prints `8BIT` and `INT|8BIT` side by side on
   every call: on any fleet device the two pairs must be identical, every time.
   If they ever are not, every figure this fleet has recorded needs re-reading.
2. **That they diverge on the Panellan, and by how much.** Expected: the bare
   pair in the megabytes, the internal pair in the tens or low hundreds of
   kilobytes, and `heapPoolsMeasured` reading `internal+psram`. Read it off the
   `[telemetry] heap pools=` line and the `[heapdiag]` line on a first boot.
   Nobody has booted this board.
3. **That the ballast still lands.** `reserveTlsBallast()` now asks for
   `kTlsCaps`, so on the Panellan it is competing for internal DRAM rather than
   taking the easy PSRAM hole. `[http] TLS ballast reserved: 2 x 16717 bytes of
   internal 8-bit heap held from boot` is the line that says it worked; the
   `could not reserve` line is the one that says the S3's internal pool is
   tighter at the top of `setup()` than this assumed. Either outcome is
   information and the second is not a regression on the fleet board, where the
   request is byte-identical to what it always was.
4. **That `HeapRatchet`'s identity still balances.** It is now anchored to
   `BootDiag::bootLargestTlsBlock()` and observes `kTlsCaps`, so the check is
   `bootLargestFreeBlock8BitInternalBytes - largestFreeBlock8BitInternalBytes`
   against the sum of the six buckets - not the bare pair the check used to
   name. On a fleet device both pairs give the same answer, which is the cheap
   way to confirm the change did not move anything.

### The draw path was deliberately left alone

`App/Display.cpp` carries fourteen bare-capability heap figures in its log lines
and `App/Assets.cpp` one more. None was changed, and none is a TLS figure - they
are about the pool a file buffer or a decoder scratch comes from, and on a board
with PSRAM that pool legitimately may be PSRAM:
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096` sends every `malloc()` above 4,096
bytes to PSRAM first, and a 10-24KB file buffer or the PNG decoder's 44KB
scratch is well above it.

The right mask for those is an open question rather than an oversight, and it is
one the board has to answer: whether LovyanGFX's decoder scratch and
`ensureFileBufferCapacity()` do land in PSRAM on the Panellan, and what that does
to the 32,768-byte PNG floor. `logHeapSnapshot()` prints `8BIT` and `INT|8BIT`
side by side and is the line to read first on a first boot - the gap between the
two pairs is the measurement. Until then those figures are unlabelled on the S3
and a reader has to know which board sent them.

### The reading end is not done

`FleetHealth.IsBelowTlsFloor()`, `IsHeapFragmented()`,
`LargestBlockLostSinceBoot()` and the `/api/telemetry` endpoint's own floor
warning all grade `largestFreeBlock8BitBytes`, which is the bare field. The
three new internal fields are additive and optional and nothing reads them yet,
so a Panellan below the TLS floor still reads as healthy on `/diag/telemetry`.
That is a server-tree change and it is a prerequisite for this being visible
anywhere but a debug stream.

## 15. The Panellan has no card slot, so no mount is attempted

`Sd::begin()` returns false at a board gate - `kBoardHasCardSlot`, a `constexpr
bool` gated on `CONFIG_IDF_TARGET_ESP32S3` and read by a runtime `if` - before
touching `SD.begin()`.

**No test was added and none failed first**, for the reason section 14 gives.

### What a build proves

That both branches compile on both boards. The gate is a runtime `if` on a
constant rather than an `#if` around the mount specifically so that the
card-mounting path is still compiled on the S3 and cannot rot there unnoticed.
Confirm by building both FQBNs and reading each compile's own error count, not
its exit code.

That `sdkconfig.h` precedes the gate. `App/SdStorage.cpp` includes it first; a
gate evaluated before that include takes the non-S3 branch, compiles clean, and
would attempt the mount on the board that has no slot. The same trap
`Display.cpp` and `CheckIn.cpp` each name at their own tops.

### What the bench has to settle

1. **That boot is quicker and quieter.** Expected on the Panellan: one
   `[sd] this board has no card slot wired` line, no `[sd] no card found after 3
   attempt(s)` line, and no `[sd] bus pins in force` line. Time `boot.sd` in the
   boot-heap notes either side of the change - the attempt was costing up to
   three rounds of SPI setup and CMD0 timeouts, which is arithmetic off the
   core's `sd_diskio.cpp` (500ms per `sdWait`, two per attempt, 200ms between
   attempts) and not a stopwatch reading.
2. **That nothing downstream noticed.** `isReady()` false, `totalBytes()` and
   `usedBytes()` zero, `mountCostBytes()` -1, assets held in RAM per draw, the
   splash skipping itself at its own gate, and a telemetry row the server
   already knows how to read because it is the shape a card-less fleet device
   sends. Confirm on `/diag/telemetry` that the Panellan's row is
   indistinguishable in shape from a card-less ESP32-2432S028's.
3. **That the fleet board is untouched.** A card-equipped device must still log
   its mount, its retry count and its mount cost exactly as before; a card-less
   one must still log `no card found after 3 attempt(s)`. The gate changes
   neither, and the only way to be sure is to read both lines off a real device.

---

## 16. The touch recovery menu, and the boot hold that reaches it

`RecoveryState.h`, `Recovery.cpp`, `Display.cpp` and `CAL.ino`. Design in
[CAL_TOUCH_RECOVERY_MENU.md](CAL_TOUCH_RECOVERY_MENU.md) and
[CAL_GPIO_DIAGNOSTICS.md](CAL_GPIO_DIAGNOSTICS.md).

**This was the one part of this repository with a host-runnable test, and there
are six of them now**: `ci/test-recovery-menu.sh`, `ci/test-loader-order.sh`,
`ci/test-config-request.sh`, `ci/test-card-columns.sh`,
`ci/test-brand-splash.sh` and `ci/test-ram-picture-budget.sh`. It still gets a
section that is mostly about a test rather than mostly about a bench.
`RecoveryState.h` has no Arduino or display dependency, so the input state
machine compiles and runs on a workstation with a fake clock, a fake finger and
stubbed NVS. The App's half of the same gesture is 16h.

### 16a. What runs, and where

Six scripts, each building and running one host target:
`ci/test-recovery-menu.sh` (CAL's `Recovery.cpp`, once per board),
`ci/test-loader-order.sh` (`App/Loader.cpp`), `ci/test-config-request.sh`
(`App/ConfigRequest.cpp`, 16h), `ci/test-card-columns.sh`
(`App/CardColumns.h`, 18d), `ci/test-brand-splash.sh` and
`ci/test-ram-picture-budget.sh` (`App/RamPictureBudget.h`, 16j). All but the
first run once rather than once per board, because nothing in them is behind a
target macro. All six are in the release gate and the pull-request bench, and
all six are in `.github/workflows/mut-host-test.yml`, which runs them on a push
to a `mut/**` or `test/**` branch so a mutation can be measured without a token
to dispatch the bench with.

Beside them are the call-site gates, which are text checks rather than host
targets: `ci/check-text-state.sh`, `ci/check-board-columns-call.sh`,
`ci/check-score-guard.sh` and `ci/check-ram-budget-call.sh` (16j). All four are
in the release gate and the bench. The last is its own step in both; the other
three are called from `ci/build-firmware.sh`, which both lanes run, so they
arrive by the wrapper rather than by a step and a reader looking only at the two
workflow files does not see them. This paragraph said until 2026-11-03 that only
`check-ram-budget-call.sh` reached either lane and that a release could carry a
call site none of the others had read; `build-firmware.sh` lines 330, 338 and 348
are what make that wrong. What the wrapper route does mean is ordering: it runs
after the host-test steps, so a host test that fails stops the release before any
of the three is read. Section 25 carries the whole matrix.

**There is a host compiler on this workstation now** - `g++` 16.2.0 at
`C:\Tools\mingw64\bin`, so each of these builds and runs here in seconds. This
paragraph said none of them could until 2026-11-02. CI is still the gate: it
builds on Linux `g++`, where size and ABI differ, and 16's own notes on the
controller's layout there still apply.

`tests/recovery-menu-test.cpp` links against production `Recovery.cpp` with
stubs for touch, drawing, NVS, the partition table and `esp_restart()`. Its
`main()` runs the state machine checks, the presence and GPIO sampling models,
and then `integrationTests()`, which drives the production module. It asserts
the six-tap sequence and the wrong-order, outside-tap, five-tap, timeout and
disabled-entry cases; the last pixel inside each corner zone and the first
pixel outside it on all three edges that matter; the tap count and the bounce
window read from `ConfigGesture.h` rather than from CAL's own copy of the
figures; held fingers, drags and sub-bounce contact; both confirmations, Cancel,
Back, menu expiry, and both timers across a `millis()` rollover their
subtraction actually spans; the live screens outlasting the 60-second menu
timeout; GPIO paging, mode switching and counters surviving a page change; each
action's persisted intent, secret removal or preservation, recorded restart
cause, and the failure paths that must stop without restarting; the menu opening
with no gesture at the App's request, its idle clock running from that open, and
a half-finished sequence not carrying across it; and
`Updater::Manifest::needsInstall()` including a same-version forced reload. It
prints `PASS:` and the controller's host-ABI size.

**Two of its cases used to be unfailable, and the wording above is what they
were replaced with.** The rollover pair started the clock at `UINT32_MAX - 500`
and then spent six taps at 200 ms each carrying it 1,200 ms past the boundary,
so `lastActivity_` landed at 699 and the subtraction was an ordinary one. The
second of the pair had a near-miss on top of that: the expiring tap landed in
the corner the sequence was not expecting, which resets the counter by itself,
so the assertion held whether or not the window had expired. Both are rewritten.
`Controller::open()` takes its time as an argument, which is what puts
`lastActivity_` on the far side of a wrap from `now`, and the gesture case now
puts the expiring tap in the corner the sequence is expecting. The boundary loop
over `kMenuRows` is retired: it asserted `row.contains(row.x, row.y)` and
`!row.contains(row.x + row.w, row.y)`, which hold for any rectangle with
positive width and height under any half-open `contains()`, so it could not
fail. 16c's `static_assert`s and the corner-edge literals above are what cover
that geometry now.

**What the new cases were measured against.** `mut/m20-ktaps-3-no-guards` sets
`ConfigGesture::kTaps` to 3 on the shipped tree and passes the host test on both
boards, which is the drift itself: the two binaries wanted different gestures
and nothing said so. The same mutation with the cases present fails on the tap
count. `mut/m22-literals-restored` puts CAL back on its own 6 and 100 and then
changes both shared figures, and fails on the tap count again, so the case
measures the binding rather than the constant. `mut/m23-zone-63` narrows both
corner rectangles and `kZoneWidth` together, which satisfies every
`static_assert` in 16c, and fails on the first corner-edge case.
`mut/m24-signed-timers` writes both timer comparisons as signed 64-bit
differences, which is correct on either side of a wrap and wrong across one, and
fails on the rewritten idle case. The pair of cases it replaces passes it.

The three `Controller::open` assertions were written after the code they cover,
not before it. The last of them is the one that bites: drop the `step_` reset
from `Controller::open` and three corner taps taken before the open still
count, so three more after the menu closes reopen it with nobody asking.
`mut/m12-open-keeps-step` carries exactly that mutation and fails on
`recovery-menu-test.cpp` line 192, while the same commit without it passes. The
two runs are the evidence, not this paragraph.

**The first version of that assertion caught nothing, and only a mutation run
said so.** Its second run of taps started from the left corner again, which is
the wrong corner after three and resets the counter on its own, so the
assertion held whether or not `open()` cleared `step_`. It reads as a test of
the reset and was a test of nothing. Continuing the alternation is what gives
it teeth. A green mutation run is the only reason this was found before it
shipped.

### 16b. The release is gated on this test

`build-and-release.yml` runs `ci/test-recovery-menu.sh`,
`ci/test-loader-order.sh`, `ci/test-config-request.sh` and
`ci/test-card-columns.sh` as its first four steps, so a failing host test stops
the release rather than being reported after it. Before
that, `bench-cal.yml` was the only runner and it fires on pull requests alone.
Work that reached main by merge never re-ran it, and on 2026-10-30 the test
stopped linking for a week without anybody finding out: `showRecoveryMenu` grew
a `reloadPending` argument and `Journal::printf` arrived with the boot hold,
while the stubs kept the old shapes. Three releases published over it,
`v2026.10.30.0003` among them, which is the loader currently marked current on
both boards.

Repairing the link then exposed a second thing the link error had been hiding:
the confirmation taps still addressed the pre-swap button positions, so the
test asserted Cancel where Confirm now sits. The swap was made to stop
bottom-right meaning "Reset" on one screen and "leave" on another, and the test
that should have guarded it was asserting the trap.

`ci/test-recovery-menu.sh` builds and runs it twice, once plain and once with
`-DCONFIG_IDF_TARGET_ESP32S3=1`, under `g++ -std=c++17 -Wall -Wextra -Werror`,
so the Panellan's allowlist and its absent PIR mapping are covered by the same
assertions. `ci/test-loader-order.sh` builds `tests/loader-order-test.cpp` the
same way, once, since nothing it covers is board-specific.
`.github/workflows/bench-cal.yml` runs both as **Test recovery gesture and
actions on host** and **Test the loader's NVS write order on host**, ahead of
the compiles, on any PR touching `Recovery*`, `CAL.ino`, `App/Loader.cpp`,
`Display.*`, `GpioMonitor.h`, `tests/**` or `ci/**`.

**No assertion in that test has been executed on this workstation.** There is no
host C++ compiler here; the cross toolchain was used for `-fsyntax-only` and
nothing else, which proves the file parses and does not run a single `assert`.
CI is where it runs, and a CI pass is the only evidence that any of 16a holds.
Read the step's own output, not the workflow's green tick.

### 16c. What every build proves, on both boards

`RecoveryState.h` carries a block of `static_assert`s over the shared
rectangles, so the geometry is checked by the compiler in every `arduino-cli`
build of either board and needs no host compiler at all:

- Each corner gesture zone against all three menu rows, the Diagnostics footer,
  Back, and the other zone.
- The three rows against each other, and Diagnostics against Back.
- Confirm against Cancel, and Confirm against Back.
- Cancel **not** disjoint from Back, which is the deliberate one: bottom-right
  has to keep meaning leave.
- The mode button against each GPIO footer button, and each of those against
  its neighbor.
- The outermost edges inside 320 × 240: the left zone's left edge, the right
  zone's right edge, the last menu row's bottom and Cancel's bottom.
- The timer orderings: diagnostics outlasting the menu, a boot hold outlasting
  the gesture window, and the cap outlasting a hold which outlasts the listen
  window.

These are what the geometry rests on, together with the corner-edge literals in
16a. The host test's old boundary check compared each rectangle against its own
x/y/w/h, which is true of any rectangle and any half-open `contains()`, so it
passed every boundary mutation, including the y<60 zones that overlapped Set up
Wi-Fi by 48 × 16 in each corner. It has been retired rather than left in place
reading as coverage.

The `static_assert`s prove no zone overlaps a button. They say nothing about
where an edge sits while it overlaps nothing, which is why 16a taps 63, 256 and
43 and then 64, 255 and 44: a zone shrunk by one pixel passes every assertion
here. Those four cases carry their figures as literals on purpose. Both
rectangles are asserted equal to `ConfigGesture`'s corner size, so a case that
read the edges from the constant would move with any mutation of it and catch
nothing.

**Nobody has inverted one of these to confirm it bites.** The procedure is the
one section 14 uses for `HeapPools.h`: flip an assertion's sense, confirm the
build stops with that message, restore with `git checkout --`, verify against
`git diff`, and remember a restored file can keep its old mtime and let the
build skip the rebuild.

### 16d. The order of the App's two NVS writes

`tests/loader-order-test.cpp` links against production `App/Loader.cpp` with
stubs for `Identity`, `Log`, the partition table and `esp_restart()`, and is
built and run by `ci/test-loader-order.sh` alongside the recovery test. It
records every `Identity` write in sequence and asserts, for
`Loader::requestConfigMenu()`, that `updreq` is committed before `cfgmenu` and
that the boot partition is selected after both.

That ordering is the whole content of the test, and it's worth saying why a
sequence is worth a test at all. The two writes are separate NVS commits with
`esp_restart()` after them. Lose power between them and the boot partition was
never changed, so the next boot runs the App again holding whichever flag landed
first. `updreq` alone is absorbed: `Updater::installApplication()` clears it
after an install, `CAL.ino` clears it when the server confirms the installed
version is current. `cfgmenu` alone is absorbed by nothing. The App never reads
it, CAL is its only writer of false, and it waits through every App boot until
some later boot runs CAL, which opens Configuration at an unattended device and
holds the boot for up to 60 seconds with Reset identity two taps away.

The test also asserts the other two paths, because they share
`bootFactoryAndRestart()`: `requestUpdate()` sets `updreq` and nothing else, and
`returnToLoaderForReprovisioning()` clears the secret and sets `updreq`.

`mut/m21-cfgmenu-before-updreq` carries the reversed order and nothing else, and
fails this test; the same commit without it passes. The two runs are the
evidence, not this paragraph.

**What this does not cover.** Nothing here runs `CAL.ino`, so the gate that
keeps the `cfgmenu` block off a boot where CAL drew nothing has no host
coverage. It is item 8 of 16e.

### 16e. The boot hold, which has no automated coverage at all

`Recovery::holdBootForTouch()` cannot be reached by the host test as it stands:
it drives `millis()`, `delay()` and `Display::readTouch()` in a loop, and the
test's `delay()` stub throws to stand in for the terminal error path. Every
check below is a person at a device.

1. **The listen window.** On a device with a healthy App installed, which draws
   no QR, power on and touch nothing. The splash must appear within about two
   seconds, and the handover must follow about 1.2 seconds later than it did
   before this change. That 1,200 ms is paid on every boot CAL draws.
2. **One tap, then nothing.** Tap a corner during the splash and stop. The boot
   pauses, and resumes about ten seconds after that tap. Expect
   `[recovery] touch during boot - holding the sequence` and
   `[recovery] touches stopped - resuming the boot`, and expect the splash to
   still be on the glass, because nothing drew over it.
3. **Renewal.** Tap every few seconds for a minute. Each tap must extend the
   hold, and the boot must not resume while tapping continues.
4. **Six taps from the splash.** Open Configuration with the corner sequence on
   that same healthy device. Expect
   `[recovery] configuration opened from the boot hold`, then exercise Back and,
   on a second run, the 60-second expiry. Both must redraw the splash and let
   the handover complete, with
   `[recovery] configuration closed - the boot continues` in the journal. This
   is the whole point of the feature: before it, a device with a current App and
   a claimed secret could not reach this menu by touch.
5. **A seventh tap.** With the menu open from a boot hold, tap the same corner
   once more. Nothing may happen. A device that restarts into its portal here is
   the y<60 defect back.
6. **The stuck panel, which is the case that bounds the whole design.** Hold a
   finger down from power-on and leave it; then repeat with the touch lines
   shorted, and once with a wet panel. The device must reach the end of its boot
   at 120 seconds, with the cap's line naming the figure and saying to suspect
   the touch controller. A unit that never boots has failed this, and failed it
   on the one image a cable or an OTA candidate has to be able to rescue.
7. **The quiet path is still quiet.** Software-restart a device whose App owns
   the loading screen (see
   [BOOT_SCREEN_OWNERSHIP.md](BOOT_SCREEN_OWNERSHIP.md)). The hold must be
   skipped entirely: no `[recovery]` line, no `lcd.init()`, no splash, and no
   1,200 ms added to that boot. Touch cannot be read on that path and must not
   be attempted.
8. **A configuration request on that same quiet path.** Set `cfgmenu` with
   `updreq` clear, then software-restart a device whose App owns the loading
   screen. Reaching this state by hand needs a scratch build or an NVS write;
   16d explains the power cut that used to reach it on its own. The menu must
   not open. Expect
   `[recovery] configuration was requested on a boot where the panel stays
   untouched - cleared without opening it`, and expect the App's own loading
   screen as usual. Then restart again and confirm the request is gone rather
   than waiting for the next loud boot. What this guards against is a finger
   already resting on the glass: `begin()` reads an unstarted panel and consumes
   nothing, `showRecoveryMenu()` brings the panel up under an open menu, and the
   first poll reads that finger as a rising edge on `kMenuRows[0]`, which is Set
   up Wi-Fi.
9. **Six taps on the running App, with the panel coming up for CAL.** The
   ordinary path. Expect `[recovery] configuration opened at the App's request`,
   the splash drawn before it, and the rows live with no gesture. This is the
   case item 8's gate must not break.

### 16f. The bench checks the assertions can't make

1. **Photograph both confirmation screens** on both boards. Read the whole
   sentence off the glass. The identity text runs to 125 characters at text size
   2 in a 304-pixel column, and `wrappedCenteredText` is given six lines, so
   this is where an ellipsis would show up. Confirm Reset/Reload is the left
   button and Cancel the right, and that tapping where Back sat on the menu
   cancels rather than resetting.
2. **The 10-minute diagnostics limit.** Open the presence screen or the GPIO
   monitor from the captive portal and leave it untouched. It must close itself
   and restore the QR, and the portal must then apply a form a phone submitted
   while it was up. Nothing asserts the figure: the host test proves only that a
   live screen outlasts the 60-second menu timeout.
3. **The Changes feed's page count.** Read the status line and confirm it says
   `changes page 1/2` rather than a literal `2`. Raise
   `GpioMonitor::kEventCount` in a scratch build that is never registered, and
   confirm the line and the paging both follow it.

### 16g. Known gaps, recorded rather than discovered later

- No host assertion covers `holdBootForTouch()`, the 10-minute limit's actual
  expiry, or the rendering: `showRecoveryMenu()` is a stub in the test and only
  its handoff is observed.
- The host test runs on CI's Linux `g++`. The controller's size and ABI there
  are not the device's, which is why its printed figure says "host ABI".
- A tap landing between two rectangles does nothing, by construction. There is
  no assertion that the gaps are small enough for a finger; that is item 1 of
  the physical acceptance checks in the design document.

### 16h. The App's half of the gesture, which had no coverage at all (2026-11-01)

`App/ConfigRequest.cpp`, run by `ci/test-config-request.sh` against
`tests/config-request-test.cpp`.

**It had none of any kind until this, and it is the newest code in the shipped
release.** `ci/test-recovery-menu.sh` compiles CAL's `Recovery.cpp` and nothing
else, so `mut/m7-app-any-corner` - which made the App accept six taps in one
corner instead of alternating - took the whole release gate green: the host
test, both boards' `arduino-cli` compiles, all of it. Six taps anywhere in one
corner would reboot a running display into the loader under somebody's hand.
`bench-cal.yml` did not run either, because `App/ConfigRequest.cpp` was not in
its path list. It is now, with `App/CardManager.cpp`, `App/CardColumns.h` and
`App/Display.*` beside it.

The production file is compiled as it ships, with stubs for the three functions
it calls - `Display::screenWidth`, `Display::showStatus` and
`Loader::requestConfigMenu` - and for `Log::line`. The Loader stub throws, which
is how a case tells a sequence that reached the sixth tap from one that did not.
`App/Display.h` and `App/Log.h` compile as they ship; both are declarations over
`<Arduino.h>`, which `tests/config-request-stubs/` provides.

**The counter has no reset and the test does not add one.** Every case opens with
a tap in the middle of the panel, which is in neither corner, and setting the
counter back to zero is the one thing production does with such a tap. Nothing
else in the test touches it. A helper that tapped twice, or re-centered between
taps, would reset the counter underneath a case meant to count to six - which is
how a case on this exact code was announced as the one that bit before it had
been measured, and the mutation passed.

**Every case has a mutation behind it.** Each was pushed as its own branch and
run through `.github/workflows/mut-host-test.yml`.

| mutation | branch | run | result |
|---|---|---|---|
| Any corner counts: `expected = left \|\| right` | `mut/cr1-any-corner` | 41318519919 | fails at `:180`, six left taps open the menu |
| Five taps are enough: `gStep < 5` | `mut/cr2-count-five` | 41318535527 | fails at `:203`, `kTaps - 1` taps open it |
| A seventh is needed: `gStep < kTaps + 1` | `mut/cr9-ktaps-plus-one` | 41318588915 | fails at `:158`, six alternating taps open nothing |
| `kTaps` 6 to 5 with the App counting to a literal 6 | `mut/cr3-ktaps-five` | 41318542838 | fails in CAL's test at `recovery-menu-test.cpp:51`, before this one runs |
| A 1 ms bounce window | `mut/cr4-bounce-one` | 41318553709 | fails at `:236`, a whole sequence inside one bounce window opens the menu |
| The window never expires | `mut/cr5-no-window` | 41318568515 | fails at `:297`, a sequence spanning `kWindowMs` opens it |
| The window as an absolute comparison, `now >= gStarted + kWindowMs` | `mut/cr13-absolute-window` | 41318616195 | fails at `:416`, a sequence across the `millis()` wrap expires mid-way |
| The right corner off a fixed 320 | `mut/cr6-fixed-320` | 41318574810 | fails at `:343`, the 240px panel's corner is nowhere |
| A stray left tap discards the sequence: `gStep = 0` | `mut/cr7-stray-left-zero` | 41318579869 | fails at `:322`, five more taps no longer finish it |
| A corner reaches 60px down | `mut/cr8-zone-height-60` | 41318584533 | fails at `:401`, a tap at `y = kZoneHeight` counts |
| A corner reaches 70px in | `mut/cr10-zone-width-70` | 41318594041 | fails at `:390`, a tap at `x = kZoneWidth` counts |
| Nothing drawn before the handover | `mut/cr11-no-status` | 41318597134 | fails at `:166`, the two recorded strings are one |
| The counter cleared after the handover call | `mut/cr12-clear-after` | 41318602893 | fails at `:470`, the next tap hands over at once. See below |

`mut/cr3-ktaps-five` is worth reading rather than skipping. It fails in step 3,
not step 5: CAL's `open()` helper counts to a literal 6 and asserts the menu
appears on index 5, so lowering the shared constant stops that test before this
one is reached. The dedicated `kTaps`-driven case in CAL's test is at
`recovery-menu-test.cpp:204`, and that is the one the design document's claim
rests on. **So the count is pinned at exactly `kTaps` from both sides here -
`mut/cr2-count-five` from below, `mut/cr9-ktaps-plus-one` from above - and this
test's loops being written from the constant rather than from a 6 is visible in
the source and is not demonstrated by a mutation.** Nothing in the repository can
demonstrate it while CAL's helper loops carry literal sixes.

**`mut/cr12-clear-after` passed the first time it was run, and that is the
record worth keeping.** Run 41317326384 went green against a post-handover case
written 100,000 ms after the handover. The gesture window's own expiry clears a
stale counter on the next tap after 8,000 ms, so the case held whether or not
production cleared the counter before handing over - a case announced as the one
that bit, measured, and found unfailable. It now taps 100 ms after the sixth
tap, inside that window, and run 41318602893 fails on it. The lesson is the
general one: a case that reaches production through a reset of its own proves
nothing about the thing it names.

**Every case that says the menu did not open also says the machine would
have.** A negative assertion holds just as well when production has stopped
reading taps for a reason the case knows nothing about, which is the shape
`mut/cr12-clear-after` exposed. The seven cases asserting no handover each end
with a sequence that does open the menu, delivered after the negative assertion
so it cannot mask what that was reading.

**What this does not cover.**

- `App/CardManager.cpp` feeds the taps and nothing compiles it for a host, so
  what reaches `noteTap()` is unasserted. The paging that shares those corners
  is unasserted for the same reason.
- `Display::screenWidth()` is stubbed, so the test says the corners follow
  whatever the panel reports. That the panel reports 240 on a display standing
  on its end is `Display::begin()`'s `kScreenW = lcd.width()`, and that is
  device work.
- Nothing here has been on glass. No device was flashed or commanded.

### 16j. Whether a picture may be held in RAM beside a handshake (2026-11-02)

Two rules in `App/RamPictureBudget.h`, graded by
`ci/test-ram-picture-budget.sh` against `tests/ram-picture-budget-test.cpp`,
plus a call-site gate in `ci/check-ram-budget-call.sh`. One PASS line each.

The house-hunt card's picture was reachable only through
`Assets::ensureCachedFrom()`, which returns false on its first line when
`Sd::isReady()` is false, so a display with no SD card drew the card text-only
and never issued a request for the picture at all. Device 44's debug stream on
2026-11-02 carries `picture ... is not on the card, drawing without it` five
times per rotation with no `[assets] GET` line beside it, while four other
picture-bearing cards on the same display in the same minutes fetch straight to
RAM and draw. `Assets::fetchToRamFrom()` closes that, and these rules decide
whether holding the bytes is affordable.

**What they are guarding.** On a board whose heap is internal DRAM only, a held
picture comes out of the pool `mbedtls_ssl_setup()` allocates its record
buffers from. Spending it draws the picture once and then takes the display off
the network - no check-in, no card policy, no firmware update.

**Why there are two rules, which is the part worth reading.** The first draft
had one: predict before allocating, modelling the picture as coming out of the
largest free block. Retrofitting that onto the graphic cards and the aircraft
logo, which have shipped without any guard since 2026-09-08, would have
withdrawn pictures that work.

Proto 2 holds 4,750 + 11,187 + 4,368 bytes of those buffers while its largest
internal block reads **17,396 - and that figure does not move when a picture is
allocated.** 129 heaptrace readings on 2026-11-02, every one 17,396, through
`beforeAssetFetch`, `afterResponseHeaders` and `afterRamAlloc` alike, while
`freeInt` moved from 32,168 to 24,848 underneath it. The pictures are not carved
out of that block. The same device logged 25 check-ins and zero handshake
failures in the same window. So the prediction's model is one that device
disproves, and the prediction refuses all three of its pictures.

`leavesHandshakePossible()` is therefore a measurement taken after the
allocation, asking only the question that can be proved: is a handshake still
possible, meaning is the largest internal block still at or above one record
buffer. On Proto 2 the answer stays yes and nothing changes. It bites when an
allocation genuinely eats the block, and the buffer then goes straight back
instead of being held until a check-in fails.

**The demand is derived per build, not hardcoded.** The two trees disagree by
12,288 bytes: the ESP32 build sets `MBEDTLS_ASYMMETRIC_CONTENT_LEN` (IN 16384,
OUT 4096) with `MBEDTLS_DYNAMIC_BUFFER`, so 16,717 + 4,429 = 21,146 and
transient; the Panellan build is stock symmetric with no dynamic buffer, so
16,717 twice = 33,434 and standing. Read from both generated sdkconfigs on
2026-11-02. A hardcoded "twice over" is wrong on the ESP32 tree in the strict
direction, which is the direction that silently withdraws a working picture.
`Assets.cpp` derives it with `#if` on those options and static_asserts the
result is one of those two figures.

**A correction this made to 16j's own first draft**: "neither
`Esp32_2432S028` can hold a picture at any size" is wrong. That was read off
the stock 33,434 figure. On the asymmetric build Workstation's 32,756 leaves
11,610 above the demand, so the smallest picture the server has produced (9,923)
fits there and the mean and largest do not. The test asserts the boundary at
11,610 rather than the flat claim.

The figures the cases are written against, read from `device_telemetry`,
`imported_listing_images` and the devices' own heaptrace lines on 2026-11-02,
row counts on a date rather than standing facts: Proto 2 at 17,396 and
Workstation at 32,756 bytes of largest internal block, both `Esp32_2432S028`
with no PSRAM; devices 43 and 44 on `PanellanZx2d80Ce02S` with 2MB registered,
both at 81,908; pictures of 9,923 to 24,512 bytes, mean 20,502, over the 117
approved on their account.

**Written after the code, and proven by mutation rather than by having failed
first.** The headers, the test and the gate were written together, so neither
ever ran against an unfixed implementation. What stands in for that: fourteen
mutations, each gated on `cmp` so one that did not apply cannot read as a pass -
MEMORY's own note that CRLF defeats an anchored pattern earned its keep here,
three of them silently failing to apply on the first run. Six against the rules
and eight against the call site, each caught.

| mutation | caught by |
|---|---|
| PSRAM approves below the ALWAYSINTERNAL threshold | a 4,096-byte picture refused on Proto 2's pool |
| `<=` to `<` on the reserve boundary | a threshold-sized picture allowed on a pool sized for it |
| the zero-byte refusal removed | a zero-byte picture refused on a pool holding exactly the reserve |
| the comparison written as an addition on the picture | a `SIZE_MAX` picture refused rather than wrapping into allowed |
| an underived demand passes `handshakePossible()` | a zeroed `TlsDemand` refused |
| the measurement graded on the total rather than the floor | Proto 2's 17,396 allowed |
| the bare 8-bit figure into `fitsBesideTls()` | the gate's pool-argument check |
| the bare 8-bit figure into `leavesHandshakePossible()` | the same |
| placement read from `CONFIG_SPIRAM` rather than the heap | the gate's runtime-read check |
| the chunked grow drops `externalOnly` | the gate's grow-count comparison |
| the refusal stops releasing its buffer | the gate's release check |
| the measurement no longer gated on `!budgeted` | the gate's scope check |
| a hardcoded demand in the first budget call | the gate's demand-count comparison |
| a hardcoded demand in the second | the same |

The gate strips `//` comments before it reads anything, because its own remarks
name every function and argument it checks and would otherwise satisfy it: an
emptied `fetchToRamImpl()` passed an earlier draft on the strength of the prose
above it. The demand check counts calls rather than looking for one occurrence,
after a mutation replaced the demand in one call with a literal and the
surviving `kTlsDemand` in the other satisfied the check.

**What none of this says.** Nothing here has been on glass. No device has
fetched a house-hunt picture to RAM, decoded one from RAM, or drawn the band
from one, and no device was flashed or commanded. The gate is a text check: it
knows which function each figure comes from, that placement is read from the
heap rather than the build, and that a refusal releases - it cannot see whether
`heap_caps_realloc` actually placed the bytes in PSRAM. And which binary a
deployed device is running is not known from here: CI builds stock while the
workstation's core overlay builds asymmetric, so device 12's own demand is
**Inferred** from its behavior rather than read off the device.

---

## 17. The panel's text state, and the function that left a scale behind (2026-11-01)

`App/Display.cpp`. LovyanGFX keeps a selected face, datum and text scale until
something sets them again, so whatever one screen leaves behind is what the next
one draws in. `restoreDefaultTextState()` (named `restoreDefaultFont()` until
this pass) put the face and the datum back at the end of every draw function and
left the scale alone. `showStatus()` set a scale of 2 for its headline and put 1
back only inside `if (detail.length() > 0)`, and `detail` defaults to `""`.

`forceUpdateCheck()` in `App.ino` is the reachable path: it draws
`showStatus("Already up to date", "")`, waits 1,500 ms, and calls
`CardManager::redraw()`. `showSportsBoard()`, `showSportsCard()` and
`showBannerCard()` set no scale of their own, so the card that comes back draws
at 2. On the board that is 44px glyphs in 35px rows, because `sansTierIndex()`
reads a `FontId` and cannot see a scale, so `layoutLine()`'s ladder measures the
scaled face as though it were the plain one.

TEXT_LAYOUT_DESIGN.md section 14 has the contract, the three-function table and
the two tables of callers.

### 17a. The gate, its two rules, and the mutations each was proven against

`ci/check-text-state.sh`, called from `ci/build-firmware.sh` beside the two text
greps there, and now with `ci/check-board-columns-call.sh` after it as the
fourth gate - see 18e. It runs standalone in under a second, which matters:
there is no host C++ compiler on this workstation, so none of the four host
tests can run here, and the only other thing that can is a twenty-minute
`arduino-cli` build.

```
bash ci/check-text-state.sh
```

**Rule 1, the restore is not inside a branch.** Run against the code as it stood
before the fix, it named `showStatus()` and exited 1. Four mutations, each run on
this workstation:

| mutation | gate |
|---|---|
| Delete `showStatus()`'s `restoreDefaultTextState()` - the defect itself | fails, names `showStatus()` |
| Nest that call inside `if (detail.length() > 0)` | fails, names `showStatus()` |
| Add a new draw function that sets a scale of 2 and returns | fails, names the new function |
| Empty `restoreDefaultTextState()` of its `setTextSize(1)`, keeping the name | fails on the first limb, by name |

A fifth was tried and the gate passed it, correctly: moving `showFailure()`'s
restore inside a branch. That function sets `setTextSize(1)` unconditionally for
its own detail line, so the scale really is 1 on return and there was nothing to
catch. It is recorded because it is the shape that looks like a miss and is not.

**Rule 1 on its own is weaker than the property above it, measured rather than
reasoned about (2026-11-01).** It compares two line numbers, so a function that
sets a scale of 2, takes an early return, and restores at the bottom has its
restore on the larger line number and passes clean. That is a path out with
scale 2 selected, and it is the original `showStatus()` defect in a different
arrangement. `App/Display.cpp` has eight public functions with real early
returns and all eight are right today, which is why this was a gate that would
not have caught the regression it exists to catch rather than a live defect.

**Rule 2, no early return while a scale is selected.** Reading a function top to
bottom, no `return` may stand between a line that selects a scale other than 1
and the next line that puts 1 back. Comments are stripped first, so a sentence
with the word "return" in it is not read as a path out.

| mutation | branch | run | gate |
|---|---|---|---|
| `if (detail.length() == 0) return;` in `showStatus()` above the restore | `mut/ts1-early-return` | 41318684486 | fails, names `showStatus()` and the line that set the scale |

Rule 1 passes that same branch, which is the point of it.

The whole file passes both rules unchanged, and the reason is narrower than
"every function restores before returning". Exactly three functions select a
scale other than 1 - `showStatus()`, `showFailure()` and `showClockDate()` - and
none of the three contains a `return` at all, so each reaches its restore on
every path. `showMoonPhaseCard()` has a `restoreDefaultTextState()` at
`App/Display.cpp:3965` directly above an early `return`, which is the shape that
looks like the interesting case, but it never selects a scale other than 1 and
so never sets rule 2's flag. **No function in the file exercises rule 2 today.**
It is there for the next one that selects a scale, which is what the mutation
above stands in for.

**Neither rule models control flow, and section 14d of the design document now
says so rather than leaving it to be found.** A restore inside a branch clears
rule 2's flag for every `return` after it, so a function that restores under
`if (a)` and returns under `if (b)` passes both rules and is wrong on the path
where `a` is false. No function in the file has that shape today. Catching it
needs the branch structure, and an `awk` over 6,700 lines is the wrong tool.

**Restore with `git checkout -- App/Display.cpp` and `touch` the file
afterwards.** A restored file keeps its old mtime, and an Arduino build then
skips the rebuild and runs the mutated binary.

### 17b. What a build proves, and what it does not

A build proves the gate runs and the file compiles. It proves nothing about the
panel. The gate reads source: it knows which function put the scale back, not
what any screen looks like.

### 17c. Known gaps, recorded rather than discovered later

- **Nothing here has been on glass.** The defect was read out of the source, not
  reproduced on a device, and the fix has not been photographed. No device was
  flashed or commanded in this pass. The owner's photograph of a scoreboard with
  wrong columns is consistent with this leak and does not establish it: the
  geometry fix that shipped separately is in the same frame.
- **The bench check.** Press BOOT on a device showing the sports board, with a
  policy whose sport field is empty so no pill is drawn, and let
  "Already up to date" clear. The board that comes back must have the same row
  heights and column widths it had before the press. Then the same with a sport
  set, which should look right either way, since `drawSportsPill()` sets a scale
  of 1 before its own label. Photograph both.
- **No assertion covers the rendering**, here or anywhere in this repository.
  `Display.cpp` names LovyanGFX in one block and draws through its own `Canvas`
  interface, so a fake canvas recording the state at each `drawString()` would
  turn section 14a's tables into assertions. That has not been written, and it is
  the only thing that would catch the next card to read a scale it did not set.
- **Only the scale is gated.** The face and the datum have the same restore and
  no check, because the private helpers that make up a card select a face
  mid-card by design and a gate on those would fire on code that is right.

---

## 18. The scoreboard's status column, and the width it lost shipping (2026-11-01)

`showSportsBoard()` in `App/Display.cpp`. The geometry rewrite in `57df3d8`,
shipped in `v2026.10.31.0002` and on the fleet now, replaced a per-state status
width with a flat `kStatusWidth` of 56. The expression it replaced gave a
scoreless row 84px on the landscape panel, and it existed because a start time is
78px of `FreeSansBold9pt7b`. 78 into 56 does not go, and `Sans9` is the last
entry in `kSansTiers` with `roomBelow` at 0, so `layoutLine()` has no smaller face
and no second line and falls through to an ellipsis.

The 78 is measured. The comment this section was written from said "about 72px",
and section 10d of TEXT_LAYOUT_DESIGN.md already held the real figures -
`10:00 PM` is 77, `12:00 AM` is 78 - three hundred lines above the section that
used the estimate. 15a has the re-measurement and the method.

The release note for that release says start times now fit. On the landscape panel
the column went the wrong way, 84 to 56. On the portrait panel it went 4 to 56,
which is better and still short of 78. The same note's example of a name that was
being cut, the Blackhawks, is a row that **has** scores, and that row's name column
went 86 to 83 on the panel the note describes.

TEXT_LAYOUT_DESIGN.md section 15 has the derivation, the table at both panel
widths against both previous versions, and the two trades this fix makes.

### 18a. What a build checks, which is new

`boardColumns(screenW, scoreless)` is `constexpr`, and fourteen `static_assert`s
hold its output at 320 and at 240 in every build on both boards. That is the
first automated coverage the scoreboard's geometry has had. They live in
`App/CardColumns.h` with the function; 18d is the move and the host run it made
possible.

**They evaluate the literals 320 and 240, so they speak for those two panels and
for nothing else.** This section said the floor assertions would say something
when a third board arrived; they cannot. 18d's sweep is what covers a width
nobody has built.

| mutation | build |
|---|---|
| `statusWidth = kBoardStatusWidth` - the shipped regression, put back | fails: `a scoreless row's status holds a start time, and the widest is 78px`, at 320 and at 240 |
| Charge a flat 56 to `fixedWidth` while drawing 80 - the double-spend `57df3d8` had | fails: `320 scoreless stays apart` and `240 scoreless stays apart`, the separation going to -12 |
| `kBoardStatusWidth` 56 to 70, squeezing the names | fails: `a row with scores keeps the 83px name column of v2026.10.31.0002`, at 320 and at 240 |

All three were run on this workstation with `arduino-cli`, each stopping the build
with no `Sketch uses` line. **Restore with `git checkout -- App/Display.cpp` and
`touch` the file**: a restored file keeps its old mtime, Arduino skips the
rebuild, and the next run links the mutated object.

### 18b. What a build still does not prove

A `static_assert` checks a column's width at the two widths written into it. It
cannot walk a range, it cannot find where the `kBoardMinNameWidth` floor engages,
and it cannot tell a column sized against a measurement from one trimmed down to
it. 18d is those three. What no automated check in either repository can do is
call `textWidth()`: the 78px for a start time is read off a glyph table by hand
and written down, which is a model of `LGFXBase::text_width()` and not that
function running.

### 18c. Known gaps, recorded rather than discovered later

- **Nothing here has been on glass.** No device was flashed or commanded. The
  regression was found by arithmetic against both versions of the code, and the
  fix is checked by arithmetic.
- **The bench check.** A scoreboard with at least one game that has not started,
  on both boards. Read the start time off the glass whole, in landscape and in
  portrait, and confirm no row's status is `10:00...`. Then a board of finished
  games, and confirm the names are no shorter than the shipped release draws
  them. Photograph all four.
- **The portrait panel's scoreless names are 57px** on a row holding a start
  time, down from the 69 that shipped. That is the start-time column's 24 extra
  pixels, split two ways, and it has not been looked at. 57px at
  `FreeSansBold9pt7b` is roughly six characters, so a long club name on a
  portrait board will ellipsize where it did not before. The trade is deliberate
  and section 15c says why. 18e takes a live row there to between 31 and 36 and
  hands a postponed one back to 69, and section 15h has every figure.
- **The bench check 18e adds.** A board with a game in progress, on both boards.
  Read the period off the glass whole in landscape, and confirm the two team
  names beside it are still readable at 63 to 76 pixels. In portrait a
  `HALFTIME` row is still cut and the names are at 31, which is the one row this
  change does not fix and the one most worth photographing.
- **The column arithmetic now has a host test and the rest of `App/Display.cpp`
  still does not.** The move this section called the right next step is done:
  `boardColumns()`, the constants it reads and `kCardMargin` are in
  `App/CardColumns.h`, which names no graphics library, and 18d is the `g++` test
  over it. Everything else in `Display.cpp` is still behind the driver block,
  which names LovyanGFX and a board pin map, and would need a fake `Canvas`.
- Nothing compiles `CAL.ino`, so neither the `cfgmenu` block nor the condition
  that now gates it has host coverage. 16e items 8 and 9 are the only checks on
  either, and both need a device.
- 16d proves the order of the App's two writes in the binary. It cannot prove
  what a device does when power is cut between them, which would mean cutting
  power to a device inside a window of a few milliseconds.
- Nothing on this change has been seen on glass. The gesture, the menu and both
  journal lines are unphotographed.

### 18d. The host run, and what the move into a header cost (2026-11-01)

`ci/test-card-columns.sh` compiles `App/CardColumns.h` with `g++` and runs
`tests/card-columns-test.cpp`.

**The move was possible and it cost four things, all small.** `boardColumns()`,
`boardStatusLeft()`, `boardSeparation()`, `BoardColumns`, the five `kBoard*`
constants and `kCardMargin` left `App/Display.cpp`'s unnamed namespace for
`namespace CardColumns` in a header of their own, with their assertions.

1. `kCardMargin` had to go with them, because the row is derived from it and the
   figure has one home. It has 67 call sites in `Display.cpp` and no other file
   reads it.
2. `Display.cpp` carries `using namespace CardColumns;` at file scope so those
   67 call sites, and the two `kBoardScoreWidth` sites outside the moved block,
   read as they did. A `using`-directive rather than ten `using`-declarations,
   on a header this small, named in a comment beside the include.
3. The literal `4` in `rowLeft` and `rowRight` is now `kBoardRowInset`, so the
   host test can assert the inset without writing a second copy of the figure.
4. `kBoardStartTimeWidth` is `kBoardStartTimeTextWidth + kBoardStartTimeSlack`,
   and `kBoardStateWordWidth` is new. All three are value-identical to what was
   there, so nothing the panel draws moves.

Nothing blocked it. The header needs no Arduino type, no font and no graphics
call, which was already true of the arithmetic - it was simply in a file that
could not be compiled anywhere.

**The byte delta is zero, on both boards and both binaries.** Read off two
`arduino-cli` runs of the same workflow, one on `origin/main` and one on this
branch, because CI builds stock and a local binary of the same commit is a
different thing:

| binary | `origin/main`, run 41318078896 | this branch, run 41318827193 |
|---|---|---|
| `CAL.ino.bin`, ESP32 | 1,375,712 | 1,375,712 |
| `CAL.ino.bin`, ESP32-S3 | 1,306,656 | 1,306,656 |
| `App.ino.bin`, ESP32 | 1,579,856 | 1,579,856 |
| `App.ino.bin`, ESP32-S3 | 1,476,576 | 1,476,576 |

Which is what a pass that adds tests should read. `kBoardStartTimeWidth` is the
same 80 whether it is written as 80 or as 78 plus 2, the `using`-directive
resolves at compile time, and a `constexpr` in a header has the internal linkage
it had in an unnamed namespace. The ESP32 figure is 95.42% of the binding
1,441,792-byte field factory ceiling, unchanged.

| mutation | branch | run | result |
|---|---|---|---|
| The start-time column trimmed to 79, keeping the 78px assertion satisfied | `mut/cc1-starttime-79` | 41318623049 | fails at `:82`, the column no longer carries its slack |
| The home block starts where the away block ends | `mut/cc2-no-block-gap` | 41318630908 | fails at `:126`, two names with no gap between them |
| The row runs to `kCardMargin` with no inset | `mut/cc3-no-row-inset` | 41318653767 | fails at `:101`, the 320px scoreless name column is 99 and not 97 |
| `kBoardMinNameWidth` raised to 30 | `mut/cc4-floor-thirty` | 41318668854 | fails at `:171`, the first width clear of the floor is 188 and not 176 |

**All four pass the fourteen `static_assert`s.** That is the point of them: each
one is a change the board builds would have accepted. The three mutations in 18a
are the other half, and those stop the build instead.

**What the host run found, which nothing else had.**

- The separation is `2 * kBoardColumnGap` on an even panel width and one pixel
  more on an odd one. Both shipped panels are even, so four assertions at those
  two widths read as a floor and the thirteenth pixel never showed.
  TEXT_LAYOUT_DESIGN.md 15c had the identity stated without it. **The parity is
  the row's leftover and not the panel's**, which 18e found when the status
  width stopped always being even; the phrasing here held only while it was.
- The first width whose name column clears the floor is 176 scoreless and 208
  with scores, not the 174 and 206 the document named. Those two are where the
  separation stops holding; the columns actually overlap below 162 and 194. Six
  figures where the document had two. The three "with scores" figures read 204,
  202 and 190 until section 18i widened the score column by two pixels; the
  scoreless three have no score column in them and did not move.
- The 78px measurement, and that the 56px column does not hold the periods a
  provider sends. TEXT_LAYOUT_DESIGN.md 15g has those figures. That one was live
  and is fixed in 18e, by taking the column's width from the string rather than
  from the row's scores.

**What it does not cover.** It reads `boardColumns()` and nothing that calls it.
`showSportsBoard()` is still behind the driver block, so what the row actually
draws into those columns, and the `scoreless` expression that chooses between
them, are both unasserted. TEXT_LAYOUT_DESIGN.md 15f is why that expression is
the wrong input to begin with.

### 18e. The status column measures its own string (2026-11-01)

`boardStatusWidthFor(screenW, scoreless, measuredTextWidth)` in
`App/CardColumns.h`, and one `textWidth()` call in `showSportsBoard()`. The
column's width used to come from the row's two score strings and the string
drawn into it had no say, which cut a live game's period on every display
showing one. TEXT_LAYOUT_DESIGN.md section 15h is the design, 15g the defect and
15f the second door the same change closes.

**The key assertion failed first, against the shipped behavior.** Branch
`test/scoreboard-status-column-red`, commit `e5ad473`, run 41321578006: the new
signatures are in the header and `boardStatusWidthFor()` returns the state's own
default width, which is what `v2026.11.01.0001` draws. The host step failed with
exit 134, a raised `assert()` rather than a compile error, at that commit's
`tests/card-columns-test.cpp:132` with `width` 320 and `scoreless` false - a
live row with scores on the landscape panel, which is 15g's row. Every other
host step in that run passed. That commit's assertion was a `>=`; the one that
shipped is an equality, which fails at all four width-and-state combinations
rather than at two, and `mut/sc4c-shipped-behavior` below is the same mutation
measured against the final file.

**Reading a CI log needs a token and `gh` hangs on this workstation**, so every
result below is the failing step's name and its exit code, read from the runs,
jobs and check-run annotation endpoints with `curl` against the public API. The
exit code is what separates the two halves: 134 is a raised `assert()` and 1 is
`g++` refusing to compile a `static_assert`. **Which** assertion fired is
derived from the mutation and the file rather than read from a log, and is
marked so.

Eighteen `static_assert`s join the fourteen already there, and the thirty-two run in
every `arduino-cli` build of both boards. They are also compiled by
`ci/test-card-columns.sh`, which is where the two below were measured, because
`g++` over a header that names no graphics library reads the same assertions an
`arduino-cli` build does and costs a minute rather than fifteen:

| mutation | branch | run | result |
|---|---|---|---|
| `boardStatusWidthFor()` ignores its measurement and returns `boardDefaultStatusWidth(scoreless)`, the shipped behavior | `mut/sc4c-shipped-behavior` | 41328405668 | exit 1, a compile error. Eight of the eighteen: the four `6TH INN` equalities, which the shipped 56 and 80 both miss, the two literals, and `a scheduled row that reported one score still holds a start time` at both widths |
| `kBoardNameFloor` raised to 56, which takes the 240px ceiling to 30 and then to the clamp | `mut/sc5c-floor-fiftysix` | 41328414829 | exit 1, a compile error. All four ceiling equalities, the `6TH INN` equality and the start-time assertion at 240, and `240 with scores holds the name floor at its ceiling` |
| The slack dropped, so the column is trimmed to the measurement | `mut/sc2c-no-slack` | 41328377013 | exit 1, a compile error on the four `6TH INN` equalities and on the literal 76: 74 asking for 74 |

All three stop the compile rather than raising an `assert()`, which is what
separates a mutation the assertions catch from one only the host cases do. The
slack is in that half because the `6TH INN` assertions became equalities: as
`>=` it was only the host test's business, and the pre-review run
`mut/sc2-no-slack` 41321971307 shows it there, exit 134 on the host case
instead. None of the three was run through `arduino-cli` on this workstation: a
board build evaluates the same `constexpr` calls, which is **Inferred** for the
board builds and **Verified** for the `g++` compile. **Restoring a file by
`git checkout --` keeps its old mtime**, so `touch` it before any `arduino-cli`
run or Arduino skips the rebuild and links the mutated object.

And the host cases, which are what a width nobody has built reads. All five
mutations were cut from the final tree and each changes one line; every one
failed only the scoreboard step, and every other host step in each run passed:

| mutation | branch | run | result |
|---|---|---|---|
| The ceiling drops its `kBoardStatusWidth` clamp, so a narrow panel's ceiling goes negative | `mut/sc1c-ceiling-unclamped` | 41328363076 | exit 134, at `:384` where the sweep reads the ceiling, on its first width: 28px gives -80 |
| `boardLeftoverPixel()` reads the panel width's parity, which is what 15c said | `mut/sc3e-panel-parity` | 41328391404 | exit 134, at `:412`, at width 155 scoreless with a 59px status, the first point the two parities disagree |

The same five against the pre-review tree are `mut/sc1-ceiling-unclamped`
41321965254, `mut/sc2-no-slack` 41321971307, `mut/sc3c-panel-parity`
41322137346, `mut/sc4-shipped-behavior` 41321984444 and
`mut/sc5-floor-fiftysix` 41321990795, and against the tree before the gate went
in `mut/sc1b` 41325285662, `mut/sc2b` 41325288672, `mut/sc3d` 41325290893,
`mut/sc4b` 41325293623 and `mut/sc5b` 41325296730. All ten failed the same step,
and their line numbers are their own tree's rather than this one's. `sc2`
raised an `assert()` where `sc2c` stops the compile, which is the `6TH INN`
assertions becoming equalities.

`mut/sc3-panel-parity`, 41321977339, is the parity mutation written without
`(void)` casts on the two parameters it stops reading: it failed at exit 1 on
`-Wunused-parameter -Werror`, which is a compile error and not the property, so
the `sc3c` and `sc3e` branches are the ones that measure it.

**The intermediate branches have been deleted and the runs have not.** A
workflow run outlives the branch it was pushed to, so every id above resolves;
`mut/sc1b`, `sc2b`, `sc3b`, `sc3d`, `sc4b`, `sc5b` and `test/sc-size-mine` are
gone from the remote, and `mut/sc1`-`sc5`, `sc3c`, `sc1c`-`sc5c`,
`test/sc-size-base`, `test/sc-size-final`, `test/sc-size-tip` and the two
`scoreboard-status-column` branches are what is left. mut-host-test.yml calls
these throwaway branches and they are.

**The byte delta is 64 bytes of App on the ESP32 and 48 on the ESP32-S3, and
nothing on CAL.** Read off two `arduino-cli` runs of the same workflow, one on
`origin/main` and one on this branch, because CI builds stock and a local binary
of the same commit is a different thing. **Reading a size off a CI log needs a
token here**, so the four figures are carried in an artifact's name, which the
artifacts endpoint returns without one. Two runs of this branch, one before the
assertions were strengthened and one after, report the same four figures, so the
rename and the extra assertions cost nothing. A third, `test/sc-size-tip` run
41328848039, covers the tip including `columns.scoreWidth` in place of the
constant and the new gate, and reports the same four again:

| binary | `origin/main`, run 41322668927 | this branch, runs 41322667127, 41325454056 and 41328848039 | delta |
|---|---|---|---|
| `CAL.ino.bin`, ESP32 | 1,375,712 | 1,375,712 | 0 |
| `CAL.ino.bin`, ESP32-S3 | 1,306,656 | 1,306,656 | 0 |
| `App.ino.bin`, ESP32 | 1,579,856 | 1,579,920 | +64 |
| `App.ino.bin`, ESP32-S3 | 1,476,576 | 1,476,624 | +48 |

CAL does not compile `App/`, so its two figures could not move and the 66,080
bytes of margin over the binding 1,441,792-byte field factory partition are
untouched. The App's 64 bytes are the `textWidth()` call and the clamp; it lands
in `ota_0`, which this table gives 2,097,152, so 517,232 bytes spare. The
`origin/main` figures match the ones 18d recorded from run 41318078896.

**What the host run found, which nothing else had.** The thirteenth pixel in
15c follows the row's leftover, not the panel width. Every status width in the
header was even - 80 and 56 - and both shipped panels are even, so the two
parities agreed at every point anybody had looked. A measured column gives
`6TH INN` 76 and `4TH QTR` 81, and the sweep over eight status widths is what
reads them apart. `boardLeftoverPixel()` is the corrected identity and 15c now
states it.

**And what a second pair of readers found, which neither half had.** Two agents
reviewed this change read-only, one re-deriving every figure from its own
reimplementations of the column arithmetic and of `LGFXBase::text_width()`, one
on the integration question. The arithmetic came back clean: all twenty-six
rows of 15h's table and all four ceilings, derived independently, with no
mismatch. What they found was in the assertions' strength and in the prose:

- Two of the new assertions read `boardMaxStatusWidth(...) >= kBoardStatusWidth`,
  which that function's own ternary makes true for every input including a
  negative panel width. They are now the four ceiling figures as equalities.
- Four read `boardStatusWidthFor(..., 74) >= 74`, and the two scoreless ones
  were satisfied by the 80px the shipped layout already handed those rows, so
  four assertions read as four properties and bit on two. They are now
  equalities at 76, and all four bite.
- The four name-floor assertions read `>= kBoardNameFloor` where the derived
  width is exactly 31 in all four cases. Now equalities, which is the tightest
  of the set.
- `boardColumns(240, measuredWidth)` - two arguments, written by somebody
  reaching for the measured form - compiled clean, discarded the measurement and
  returned the scoreless layout, and would have passed every assertion here and
  every case in the host test, because none of them reads the draw path. The
  measured forms are now `boardColumnsFor()`, `boardStatusLeftFor()` and
  `boardSeparationFor()`, so there is no two-arity overload set to fall into.
- The cross-row stagger above. Neither half of the automated check could have
  reported it, and it is the one thing here a photograph would show first.
- Eight statements in TEXT_LAYOUT_DESIGN.md that were false or loose, including
  section 15g's own "it is live and it is not fixed here" standing in the same
  commit that fixes it, and 10d's "is still cut". All corrected.


#### The gate over the draw path, and why it exists

`ci/check-board-columns-call.sh`, run by `ci/build-firmware.sh` beside
`check-text-state.sh` and as its own step in `mut-host-test.yml`'s host job.

**An adversarial reading could not break `App/CardColumns.h` and broke
`App/Display.cpp` five ways.** Thirty-two `static_assert`s and two sweeps over
every panel width from 28 to 1024 pin every constant and every operator in the
header that moves a pixel: of the twelve single-token mutations tried, ten fire
and two are equivalent mutants that change nothing. None of it reads
`App/Display.cpp`, and the four arguments the draw path hands that arithmetic
had no check at all. Each of these drew a visibly wrong row with nothing firing
anywhere:

| the change | what the panel drew |
|---|---|
| `boardColumnsFor(kScreenW, !scoreless, ...)` | a row with scores laid out for none, and the score digits painted through the away team's name |
| `boardStatusWidthFor(kScreenW, !scoreless, ...)` | `HALFTIME` on a 240px row with scores taking the scoreless ceiling, names clamped to 24 and the separation down to 10 |
| `kScreenW` to `kScreenH` in either call | the row laid out for the other panel: 80px of empty glass in landscape, the status column off the glass in profile |
| `lcd.textWidth(row.status)` to `row.awayName` | a 132px status column holding `FINAL`, both names at 45 |
| `FontId::Sans9` to `Sans12`, or the `setFont` deleted | the status measured in a face it is not drawn in, and with the line gone, rows 2 to 4 measured in whatever the scores left behind |

So the gate reads those four lines and says what they have to be: the face
selected on the line before the measurement, the measurement taken off
`row.status` for `kScreenW`, the layout taking the same panel, the same state
and the width just measured, and the score columns coming from
`columns.scoreWidth` rather than from `kBoardScoreWidth`. It is a text gate
rather than a test, in the same spirit as `check-text-state.sh`: it cannot see
whether the measurement is right, only that the arguments are the four the
assertions were written against.

**Nine mutations, each caught.** The five above, the two polarity flips
separately, the `setFont` moved after the measurement rather than deleted, and
the whole thing replaced by the two-argument `boardColumns(kScreenW,
scoreless)`. Run on this workstation, which needs no toolchain because the gate
is `awk` and `grep`: **Verified**, nine caught and the clean tree passing.

What it does not reach: the measurement's correctness, the row actually drawn,
and any argument that is spelled right and means something else. A photograph
is the check for those, and there has not been one.

#### What the adversarial reading found that stands

- **`kBoardStatusSlack` set to 0 passed all thirty of the assertions as they
  were.** The four `6TH INN` equalities carried the constant on both sides, so
  they moved with it, and the literal 76 appeared nowhere. Two assertions now
  read the 76 and the 80 as literals, which is why there are thirty-two.
- **Five host assertions were restatements of the code rather than properties**:
  `boardDefaultStatusWidth(true)` against the sum that defines it, the slack
  read as a `<=` of `min()`'s own right-hand side, `c.statusWidth` against the
  parameter copied into it, a `statusWidth == 0 || >= 56` that the three returns
  make tautological, and the 48 and 96 of the stagger subtracted from the test
  file's own literal arrays. The first two, and the stagger, now read literals
  or the function. The other two are left, named here.
- **`kBoardStateWordWidth` can be any value at or under 56 and nothing fails.**
  It is a measurement claim with nothing behind it, and it is not in the draw
  path, so nothing on the panel moves either way.
- **A status carrying a byte from 0x01 to 0x1F is measured and drawn at two
  different widths.** `LGFXBase::text_width()` drops control bytes and charges
  nothing for them; `layoutText()`'s own `copySlice()` turns each into a space
  and charges 5px. So `"6TH\x01INN"` measures 69, gets a 71px column, and then
  ellipsizes inside a column sized from its own string - section 15g's defect,
  re-entering through a byte the measurement throws away. Nothing scrubs control
  characters out of the period: `CheckIn.cpp`'s `copyBoundedMarked()` does not,
  and `Sports.cpp` copies it through. The fix belongs at the wire or in a
  measurement that reads the same string `layoutText()` will, and is not here.
- **An all-spaces status takes a column and draws nothing in it.** Twelve spaces
  measure 60, which clears 56, so the column becomes 62 and both names pay;
  `layoutText()` then draws no lines and logs that it drew none. The
  `row.status[0] != '\0'` guard does not catch it.
- **The UTF-8 decoder's state is per-object and the new `textWidth()` call runs
  before the names.** A period cut mid-sequence - `Sports.cpp` hard-cuts at byte
  8 - leaves `decodeUTF8` mid-sequence, and the next byte it sees is now the
  away name's first rather than the following row's. The leak predates this
  change; this moves it one field earlier and adds a second occurrence per row.
  It needs a non-ASCII period cut mid-sequence beside a non-ASCII name, so it is
  **Inferred** as reachable rather than seen.

**What this does not prove.**

- Nothing calls `LGFXBase::text_width()`. Every figure in 15h is read off the
  vendored `FreeSansBold9pt7b` glyph table by a reimplementation of that
  function at `size_x == 1`, which reproduces all five 12pt figures and all
  eight 9pt figures already written into TEXT_LAYOUT_DESIGN.md to the pixel.
  That makes it a sixth agreeing model and still a model. What runs on the
  device is `textWidth()` itself, and the assertions read the model.
- `showSportsBoard()` is still behind the driver block, so what is measured and
  what is drawn are both unasserted. `ci/check-board-columns-call.sh` reads the
  four lines as text and says what they have to be; it cannot say whether the
  measurement is right or whether the row looks like anything. What the host run
  checks is the arithmetic those four lines feed.
- **Three reviews by three separate agents, which is the owner's standard.** The
  correctness reading re-derived every figure in 15h from its own
  reimplementations of the arithmetic and of `LGFXBase::text_width()` and found
  the twenty-six rows and four ceilings clean; the integration reading found the
  wire untouched, the mandate not engaged, and the cross-row stagger; the
  adversarial reading could not break `App/CardColumns.h` and broke the draw
  path five ways, which is what the gate exists for. The red-then-green run and
  the ten mutation runs are the fourth reading and they are mine, which is one
  agent rather than a separate one.
- **`HALFTIME` at 94px is still cut on a 240px panel with scores.** Asserted as
  the gap it is, at `tests/card-columns-test.cpp` in the block after 15h's
  table, so a change that closes it has to come through that line. 15h has why
  that row has not got the pixels.
- **Name columns are narrower than `v2026.10.31.0002` draws on any row whose
  status did not fit.** On the landscape panel 83 down to 73 for `6TH INN`, 63
  for `HALFTIME` and 61 for `OVERTIME`; on the portrait one 43 down to 33, and
  to the 31px floor for anything 78px or wider. Held at or above
  `kBoardNameFloor` by the ceiling and by four of the new assertions, and
  unphotographed. 15h's second table has the rule and the bounds.
- **Four rows of a board no longer share a column grid**, because each row's
  status column is as wide as its own string and every edge but the status
  column's right one is derived from the name width. On a 320px panel with
  scores the home name's left edge is 157, 129, 119 or 109 depending on the
  status, and the home score's right edge 294, 238, 218 or 198: 48px and 96px
  of stagger between rows of one card. It differed two ways before, across
  `scoreless`, and those were the rows with no score digits to misalign, so
  nothing showed. A live row has digits. The host test holds the four grids as
  figures; no property can say they agree, because the function takes one row.
  **A board mixing finished games with one in progress is the photograph this
  change needs**, and it is the ordinary case for `sportsscores` rather than an
  edge. **18f closed this**, from exactly that photograph: the board folds its
  rows and lays all four on one grid, and the four figures above are what it
  picks the widest of.
- **The text-scale coupling is new and the gate does not cover it.** This is the
  first place in `App/Display.cpp` where the panel's text scale changes a
  *layout* rather than a glyph size: `textWidth()` multiplies by `size_x`, so a
  leaked scale of 2 would give `FINAL` a 112px column and squeeze both names.
  Section 14b is the contract that every function puts 1 back and 14d is clear
  that `ci/check-text-state.sh` reads indentation and early returns rather than
  control flow. `showSportsBoard()` sets no scale of its own and the three
  functions in the file that do restore unconditionally, so it is safe today and
  checked by a gate that does not model the path. The gate reads nothing about
  the *face* at all; the draw path selects `FontId::Sans9` before measuring
  rather than relying on it.
- **`SportsBoardRow::kMaxStatus` is 16 and nothing can fill it.** The widest
  status any path produces is eight characters: `Sports::kMaxPeriodLength` caps
  the period at 8 and `startTimeText()`'s widest output is `12:00 AM`. The
  comment on that constant still gives "the longest of those sets the size" as
  the reason for 16, which stopped being true when the period cap was set. Extra
  headroom rather than a defect, and not changed here.
- **Two documents in the other repository are now behind.**
  `SPORTS_CARDS_DESIGN.md` section 11 is cited by TEXT_LAYOUT_DESIGN.md 10d as
  giving the scoreless column 84px, which 15a already re-measured to 80 and 15h
  has now replaced with a measured width.
  `SPORTS_CARDS_V2_GENERIC_DESIGN.md` section 12 is cited by `App/Display.h` as
  the layout, and its "five fields of a row at fixed x positions" is false twice
  over. `App/Display.h`'s own remark says so; neither of those documents is in
  this repository and neither was touched.

### 18f. Four rows of one board are one table (2026-11-01)

`boardColumnsForBoard()` in `App/CardColumns.h`, and the two folds in
`App/Display.cpp` that feed it. 18e put the status width on the row's own string
and left `boardColumnsFor()` inside the row loop, so a row's status moved its own
home name and both of its score columns; a board of three finished games and one
in progress drew two ragged columns of digits. The board takes one grid now.
TEXT_LAYOUT_DESIGN.md section 15i is the design and 15h's "Four rows of one
board no longer share a column grid" is the defect.

**The source is a photograph, and half of what it shows is already fixed.** A
portrait CYD running `v2026.10.30.0001` shows four things: `Hurricanes` drawn
through `FINAL` with a score clipped at the right edge; the navigation chevron
across the third row's card; `Capitals` and `P1` touching with both in-progress
rows' home scores missing; and four rows on different grids. The first and the
third are one cause, `57df3d8`'s five literal column edges on a 240px panel they
were never measured for, and that is in `v2026.10.31.0002`: the home score
column's right edge was a flat 248 on a 240px panel, which is the clipped digit
and the two missing ones, and the home name column ran from 136 to 230 across a
status right-aligned on 226. Traced in the code at both versions and
**Verified** there rather than on a device, since the photographed build is
eight releases behind and no device has been flashed with either. The second and
the fourth are what this section fixes.

**The key assertion failed first, against `main`.** Before any of this change
existed, `tests/card-columns-test.cpp` got the property as four rows of one
320px board having one `homeNameLeft`, written against `main`'s own
`boardColumnsFor()`. `ci/test-card-columns.sh` printed
`Assertion failed: c.homeNameLeft == first.homeNameLeft, file
tests/card-columns-test.cpp, line 285` and no `PASS` line. The mutation was
gated on `cmp` against a byte-for-byte copy taken before the edit, and the file
was restored from that copy and `touch`ed afterwards. **Verified**, locally, on
this workstation's `g++ 16.2.0`.

The assertions that shipped are stronger than that one: the board's figures at
both panel widths, the grid being the narrowest-named of the four its rows could
have asked for and met by exactly one of them, and the fold itself.

Nine `static_assert`s join the thirty-two already there, and the forty-one run
in every `arduino-cli` build of both boards and in `ci/test-card-columns.sh`.
A tenth is in `App/Display.cpp`, on the chevron inset, where only a board build
reads it.

| mutation | result |
|---|---|
| `boardColumnsForBoard()` ignores its measurement and asks for `kBoardStatusWidth`, a fixed column instead of the board's | exit 1, a compile error on five of the nine: both 320px `HALFTIME` figures, both 240px name-floor figures and the 320px start-time figure. The two `FINAL` figures and the two score-column figures do not fire, which is the point of them: a board of state words is meant to draw exactly what the fleet draws |
| `widestBoardStatus()` keeps the narrowest measurement, which is `<` for `>` | `ci/check-board-columns-call.sh` fails on the comparison check. No host test reads `App/Display.cpp`, so the gate is the only thing that can see which way that comparison runs, and the check exists because of this mutation |
| `boardColumnsForBoard()` drops its `scoreless` argument and passes `false`, so a board of four scheduled games draws score columns | exit 1, on `a board of scheduled games draws no score column`, and on nothing else |
| `kChevronInset` raised to 4, which puts the chevron body back on the card | exit 1 on the `static_assert` beside it. **Verified** against `g++` over a standalone translation unit holding `kChevronWidth`, `kChevronInset` and the assertion, and **Inferred** for the `arduino-cli` build, which evaluates the same expression |
| `showSportsBoard()` declares `const BoardColumns columns` inside the row loop again, which is where 18e left it | `ci/check-board-columns-call.sh` exit 1, on `'const BoardColumns columns =' is not declared at showSportsBoard()'s own scope`. Nothing else fires: the call still reads `boardColumnsForBoard(kScreenW, scoreless, widestBoardStatus(rows, count))`, so the indentation is the only thing that can see it |
| the two folds moved above `count = kMaxRows;` | `ci/check-board-columns-call.sh` exit 1, on the ordering check |

The six in the table were each cut against a copy of the file in a scratch
directory and run by pointing the compiler's include path or the gate's
`TARGET` at that copy, so the tree was never mutated and nothing had to be
restored into it. Each one was gated on `cmp` against the unmutated copy first.
The red proof above is the one that was cut into the tree, and it was restored
from a byte-for-byte copy and `touch`ed.

**The gate caught one of its own.** The first cut of the in-loop mutation used
`\n` in a `perl` pattern against a working-tree file that is CRLF, so the
removal half matched nothing and only the insertion half landed. `cmp` said the
file had changed, the mutant held both declarations, and the gate passed on the
one still at function scope. That is exactly the shape the gate is meant to
catch going the other way, and it is why `cmp` saying "changed" is not the same
as "the mutation I meant landed": the second cut used `\r?\n` and the gate
failed. Restoring a file by `git checkout --` also keeps its old mtime, so a
mutation cut into the tree needs a `touch` afterwards or the next
`arduino-cli` run links the mutant; nothing here was cut into the tree.

**The board build is this workstation's, which is the overlaid core.** CI builds
stock, so a local binary and a CI binary of the same commit are not the same
thing, and no size delta against `origin/main` was measured because that would
be a second forty-minute build of a tree nothing else needs.
`arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs
--libraries ../libraries` over `App/` reports `Sketch uses 1578727 bytes (80%)`
and `Global variables use 115140 bytes (35%)`, with no errors and no warnings in
the log, read off the log rather than off the exit code. That run is what
evaluates the forty-one `static_assert`s in `App/CardColumns.h` and the one in
`App/Display.cpp` on a real board target; the ESP32-S3 was not built locally and
compiles the same two headers, which is **Inferred**.

The ESP32 figure for `origin/main` from CI, 18e's table, is 1,579,920 against a
stock core. The two are not comparable and the local figure is here as a record
of what was built, not as a delta.

**CI ran both halves twice and both passed both times.** `mut-host-test.yml`
triggers on `test/**`, so the tree went up on `test/scoreboard-one-grid` with
`[full-gate]` in the message: run **41356255370** before the three reviews and
run **41358639192** after them, the second carrying the eleven checks 18f-i
added. In each, `host-test` passed all six steps, including
`ci/check-board-columns-call.sh`, which settles the awk and line-ending
questions a local run cannot, and `full-gate` passed `ci/build-firmware.sh`,
which is CAL and the App on the ESP32 and the ESP32-S3 against the stock core.
So the forty-one `static_assert`s in `App/CardColumns.h` and the one in
`App/Display.cpp` are evaluated on both boards by a build that is not this
workstation's. Read off the runs and jobs endpoints with `curl`, because `gh`
hangs here. **Verified.**

The two commits after that are prose in this file and in
TEXT_LAYOUT_DESIGN.md and have no CI run of their own.

### 18f-i. Three reviews, and the sixteen mutations the first gate missed

Three agents read the change, one on the figures against the design document,
one adversarial, one on integration. Each one found something the other two did
not.

**The figures came back clean and the prose did not.** The correctness reviewer
reimplemented `LGFXBase::text_width()` and the whole column arithmetic from the
prose alone and diffed against the header over every width 28 to 1024 in both
states: identical everywhere, every figure in 15i and all nine new assertions
re-derived, and all six glyph widths 15i quotes reproduced along with
twenty-five control figures from 9b, 10d, 15a and 15g. That is a sixth agreeing
model of `text_width()`. What it found instead was six wrong sentences, all
fixed here: 15i credited `57df3d8` with literals it deleted rather than
`f2ae104` which added them; it called the clipped digit an away score when
`awayScoreRight` was 126 and only the home column reached past the glass; it had
`Hurricanes` drawing to 230 when `layoutLine()` has no face below `Sans9` and
ellipsizes it to 222, so the defect is three boxes over the same pixels rather
than a string overrunning one; it put the chevron on the third row when the body
spans 148 to 172 and catches four pixels of the second; 15d still said
thirty-two assertions; and `RELEASE_NOTES.md` quoted a 48px spread for a pair of
rows whose spread is 22, and a 36px name for every start time when 36 is
`7:00 PM`'s and `10:00 AM` gets the floor.

**The adversarial reviewer applied fifty-four mutations and the `cmp` gate fired
seven times**, twice because `\Q...\E` swallowed a `\r\n` and four times because
a `'\0'` replacement wrote a real NUL byte. Each of those would have read as a
test that does not bite. `App/CardColumns.h` held against all twenty-six cut at
it. `App/Display.cpp` was the hole: thirteen caught and **sixteen survivors**,
in four groups. `boardHasNoScores()`'s body was read by nothing at all, since no
host test compiles `Display.cpp` and the gate named only its call site.
`widestBoardStatus()` had its three lines read and not how they compose, so the
loop bound, the seed, the return and an added `else` all survived.
`showSportsBoard()`'s five `layoutLine()` calls were unread, so the home name
drawn at the away name's edge, the status painted over the home score and the
status given the name column's width all passed. And the `setFont` before the
status is **drawn** was unread, so measured in `Sans9` and drawn in `Sans12`
survived, which the gate's own header names as the thing it exists to prevent.

`ci/check-board-columns-call.sh` grew eleven checks for those, and every one was
proven against the mutation that motivated it:

| survivor | now caught by |
|---|---|
| `boardHasNoScores()` returns `true` on a scored row, or `false` at the end | the return-polarity check |
| its loop reads row 0 only | the loop-bound check |
| its `||` becomes `&&`, or the home-side test is deleted | the predicate check, which is the rule `boardScoreWidth()` states in prose |
| `widestBoardStatus()` reads row 0 only | the loop-bound check |
| its seed is 300, or it returns 0 | the seed-and-return check |
| it gains an `else` that keeps the last row's measurement | the no-second-branch check, since the other three pass on that one |
| the home name at `columns.awayNameLeft` | the five per-field draw checks |
| the status at `columns.homeScoreRight`, or in `columns.nameWidth` | the same |
| the away score in `columns.nameWidth` | the same |
| the status drawn in `Sans12` | the drawn-face check |
| the right chevron tip loses its `- 1`, or a tip is hardcoded | the chevron-tip check |

Sixteen of sixteen, run against copies outside the tree with the gate's `TARGET`
repointed, each gated on `cmp` and on a NUL-byte count. Two controls: a no-match
pattern reports `NOT APPLIED` rather than passing, and a comment-only edit
survives, so the gate is not over-fitted to the text it reads. **Verified.**

**The integration reviewer found nothing reaching CAL and one thing worth
naming.** Nothing in the diff touches the root sketch, there is no second copy
of any constant it changes, the touch hit-testing reads none of the chevron
figures, `flashNavEdge()`'s strip still contains the chevron at both positions,
and no card other than the scoreboard draws where the chevron now sits. The one
`static_assert` covers both chevrons rather than the left, because the `- 1` in
the right tip makes the two inequalities identical. `SportsBoardRow` is
byte-for-byte unchanged and the server holds no firmware layout constant
anywhere. What it named: a provider's `Period` text is uncapped server-side, so
the blast radius of that one wire field went from one row to four even though
the field did not change. It is bounded by the device's 8-character cut and by
`kBoardNameFloor`, so nothing can leave the glass.

### 18g. What 18f does not cover

- **Nothing has been on glass.** Every figure is arithmetic, a `g++` run or a
  compile. The one thing only a photograph can answer is whether the chevrons at
  a 2px inset clear the bezel on a cased device, and whether 36px names on a
  240px board read as a table or as four rows of ellipses.
- **No host test compiles `App/Display.cpp`.** It names LovyanGFX and a board
  pin map. `ci/check-board-columns-call.sh` is a text gate over the call, the
  two folds and the five draw calls, and it knows the arguments are the right
  arguments, not that the drawing is right. Twenty-one text checks and four
  not-found guards are not a test of that function; they are twenty-one
  single-token changes that now cost somebody a red build instead of a
  photograph.
- **`kChevronInset` is bounded from above and not from below.** 2 down to 0
  compiles clean, and 0 puts the chevron's tip on the first column of glass. The
  gate holds the two tips symmetric about the margins but nothing holds the
  inset itself at 2, because what the right figure is depends on a bezel no
  assertion can see.
- **The chevron change reaches every card, not just the scoreboard.** It was
  read against the scoreboard, which is the only card that fills the content
  band to its edge on every row. No other card was looked at, in a screenshot or
  otherwise.
- **`SPORTS_CARDS_V2_GENERIC_DESIGN.md` is in the other repository and is
  further behind than 18e left it.** `App/Display.h`'s remark now says what the
  five positions are; that document still says they are fixed.
- **A rollout draws the same payload two ways.** A device with this lays one
  grid and a device without lays four, from one `sportsscores` response. Nothing
  on the wire changed and no field was added, so the 6-month compatibility gate
  is untouched.
- **The portrait row has a second line it does not use, and this did not take
  it.** `kRowHeight` is `(kAgeY - kRowTop) / kMaxRows`, which is 56 on the
  240x320 panel against 35 in landscape, and the row draws one 22px line at
  `top + 9` either way. 212 pixels of width hold four text columns on that
  panel and the names get 36 of them; 56 pixels of height hold two lines and the
  row uses one. Putting the status, or the second team, on a line of its own in
  profile is the change that would give a portrait name its width back, and it
  is a layout this section did not attempt. `kBoardRowInset` is the other figure
  left alone: the status column's right edge sits 3px inside the card's, which
  reads as flush beside the chevron and still reads tight without it, and moving
  it moves every figure in 15h's two tables.
---

### 18h. Two lines and a brace, on the profile panel (2026-11-01)

`boardStackedColumnsForBoard()` and its five helpers in `App/CardColumns.h`,
`drawRowBrace()` and the orientation branch in `showSportsBoard()`. 18g's last
bullet is the thing this does: the profile row had 56px of height, drew one
22px line into it, and gave the names 36px of the 212 it had across. It draws
two lines now, one team each, joined by a drawn left brace, and the name column
goes to 97 on the photographed board. TEXT_LAYOUT_DESIGN.md section 15j is the
design.

**The source is a photograph of a portrait CYD running `v2026.11.01.0004`**,
which shows `Jets 0 Hur... 7 FINAL`, `Wild 5 Ca... 1 FINAL`, `Ma... 1 Fly... 5
P3` and `Oilers Rang... 6:00 PM`. Five of eight names cut, two of them to a
two-letter stub. The device is a release behind and draws the per-row grids 15i
replaced, so on `main` the same payload is worse rather than better: every row
lands on the one 36px column the 67px start time leaves, and seven of the eight
names are cut. Traced in the code and **Verified** by the host test at both
versions, not on a device.

**What the key test did against `main`, said plainly.** The design document and
`App/CardColumns.h` were written before `tests/card-columns-test.cpp`: the
arithmetic came first and the sweep second, and this says so rather than
leaving the other order to be assumed. What was checked against `main` is that
neither new check passes there:

- The host test as it now stands, compiled against `main`'s own
  `App/CardColumns.h`, fails to build with six `has not been declared in
  'CardColumns'` errors and no `PASS` line. **Verified** on this workstation's
  `g++ 16.2.0`.
- `ci/check-board-columns-call.sh` as it now stands, run against `main`'s own
  `App/Display.cpp`, prints three `ERROR:` lines and exits 1. **Verified**
  locally. `main`'s Display.cpp was put in place from `git show`, the gate run,
  and the file restored from a byte-for-byte copy and `cmp`'d afterwards.

**Counts, before and after.** `App/CardColumns.h` 43 `static_assert`s to 72.
`tests/card-columns-test.cpp` 112 `assert`s to 173, in one binary whose own
`PASS:` line is what `ci/test-card-columns.sh` reads. `App/Display.cpp` one
`static_assert` to two, the new one holding `drawRowBrace()`'s own geometry to
the `kBoardBraceWidth` the column arithmetic reserves. The gate goes from 21
text checks to 31. All seven host scripts pass by their own `PASS:` or
`Passed!` line, read off each runner rather than off an exit code.

#### The mutations, and what caught each one

Thirteen, each applied to a byte-for-byte copy taken first, each gated on `cmp`
saying the file actually changed before the check ran, each restored from that
copy and `cmp`'d and `touch`ed after. A mutation that did not land is recorded
as such rather than as a pass.

Two of them did not land on the first attempt, both because the pattern was
written with LF newlines against a CRLF tree, and `perl` reported "pattern
occurs 0 times" rather than replacing nothing quietly. That is the gate on the
gate, and it is why it is there.

| # | the change | caught by |
|---|---|---|
| A | `kBoardBraceWidth` 8 to 7 | `static_assert`, the 97px name |
| B | the ceiling subtracts two name floors, not one | `static_assert`, "240 two-line can spend 135" |
| C | `kBoardStackedScoreWidth` 20 to 26 | `static_assert`, the 97px name |
| D | the two-line name splits its remainder in half | `static_assert`, the 97px name |
| E | `kBoardShortStartTimeTextWidth` 67 to 68 | `static_assert`, the 97px name |
| F | the 56px rule fires at `<` rather than `<=` | the host test, line 681 |
| G | the two grid calls swapped | the gate |
| H | `kScreenH > kScreenW` reversed | the gate |
| I | the card-height term dropped from `stacked` | the gate |
| J | both team names on one line | the gate |
| K | the status back on the away team's line | the gate |
| L | the two score faces swapped | the gate |
| M | the `drawRowBrace()` call deleted | the gate |
| N | the brace spans one line rather than two | the gate |
| O | `drawRowBrace()`'s width assertion weakened to `<=` | the gate |
| P | `lineHeight` measured under `Sans12` | the gate |
| Q | the score drawn in a face it was not laid out for | the gate |

**Five of those were caught by a `static_assert` and not by the host test**,
which is worth writing down rather than counting as five test results: the
header's assertions fire at compile time and the sweep never runs. F is the one
that proves the sweep itself bites, and it was found by looking for a change no
assertion in the header reads. The rule table at line 681 was added for it, and
it pins `boardStackedStatusWidthFor()` at 0, 1, 55, 56, 57, 74 and 94, where
before it only bounded the answer.

#### What 18h does not cover

- **Nothing has been on glass.** Every figure here is arithmetic, a `g++` run
  or a compile. Whether two lines and a brace read as one game, whether an 8px
  brace is legible at 44px tall, whether a `Sans9` score is big enough to glance
  at, and whether four two-line rows read as a scoreboard or as a wall of text
  are all things only a photograph answers, and none has been taken. This is the
  open item.
- **No host test compiles `App/Display.cpp`**, for 18g's reason. The brace is
  drawn by eight calls into LovyanGFX that nothing here executes. Its width is
  held by a `static_assert` an `arduino-cli` build runs; its shape is held by
  nobody.
- **The score face drops a tier in profile**, `Sans12` to `Sans9`, and that is
  what keeps the board at four games. Two `Sans12` slots are 58px against a 52px
  card. The alternative costed in 15j is three rows of 74px, which drops a game.
  Nothing tests which of those a reader prefers.
- **A three-digit score is still cut.** 30px of `Sans9` into the 20px column,
  where landscape puts 40px of `Sans12` into 28. Carried over from the one-line
  row unchanged, not fixed and not worsened, and a basketball board is where it
  shows. The two landscape figures read 39 and 26 until section 18i measured
  them: `100` is 40 and the column is 28.
- **`kBoardScoreWidth` is a pixel or two short for any score holding a `1`, and
  that is on the fleet now.** Found while measuring the two-line column, not
  caused by it. `1` is the one digit in `FreeSansBold12pt7b` with an `xAdvance`
  of 14 rather than 13, so `11` measures 28 and `10` measures 27 against a 26px
  column, and `layoutLine()` draws them a tier down at `Sans9` beside
  full-size neighbours. **Verified** off the glyph table, not on a device. Left
  alone here because moving that constant moves every figure in 15b, 15h and
  15i. The two-line row is unaffected: all ten `Sans9` digits advance 10, so
  every two-digit score is exactly the 20px it is given.

  **Closed by section 18i** on 2026-11-02, which moved the constant to 28 and
  every figure with it. Still nothing on glass.
- **The landscape row was read, not photographed.** Every figure 15h and 15i
  wrote down is untouched and the assertions that hold them still pass, but no
  landscape board has been looked at since.
- **`stacked` is false on a panel whose card cannot hold two lines**, and no
  such panel exists to test it on. The branch is there so a narrower profile
  panel draws the landscape form rather than two lines through each other; it
  has never been taken.
- **A rollout draws the same payload two ways.** `SportsBoardRow` and
  `Sports::Game` are byte-for-byte what they were and no field was added,
  removed or reinterpreted, so the 6-month compatibility gate is untouched. A
  device without this draws one line to a row.
- **`SPORTS_CARDS_V2_GENERIC_DESIGN.md` is in the other repository** and says
  the row is five fields at fixed positions. It was already behind after 18e and
  18f and this moves it further.

### 18i. Two widths that were measured wrong (2026-11-02)

`kBoardScoreWidth` and `kBoardNameFloor` in `App/CardColumns.h`, and a new
`drawnTextWidth()` in `App/Display.cpp` read by `widestBoardStatus()` and by
`layoutLine()`'s overflow test. Two defects, both found by measurement and
neither by eye. TEXT_LAYOUT_DESIGN.md section 15k is the design.

**The score column was 26 and two digits of `FreeSansBold12pt7b` are 28.** `1`
is the only digit in that face with an `xAdvance` of 14 rather than 13, so
eighteen of the ninety two-digit scores measure 27 or 28 against a 26px column
and `layoutLine()`'s ladder drops them to `Sans9`. The column is 28 and
`kBoardNameFloor` is 29, which is the floor's own derivation re-run at the
corrected score width. The two-line row was already right: every `Sans9` digit
advances 10, so `kBoardStackedScoreWidth`'s 20 holds any two of them.

**A status carrying a control byte was measured at one width and drawn at
another.** `LGFXBase::text_width()` skips a byte below 0x20 for no pixels and
`copySlice()` turns it into a space, which is 5px at `Sans9`, so a period
arriving as `2ND<0x01>QTR` sized a 78px column and then drew `2ND...` into it.
`drawnTextWidth()` measures the string through `copySlice()` and both callers
read it. **Established before anything was changed**, by reading
`LGFXBase.cpp:2184` and `2350` against `App/Display.cpp`'s `layoutText()`, and
the mechanism is **Verified**. **That a device has ever sent such a status is
not.** `copyBoundedMarked()` in `App/CheckIn.cpp` bounds the length of `period`
and does not scrub control characters, so the route is open and no telemetry
says it has been used.

**The measurements.** `LGFXBase::text_width()` was reimplemented a fifth time at
`size_x == 1` by 9b's method, off the vendored `FreeSansBold9pt7b.h` and
`FreeSansBold12pt7b.h` glyph tables, and it reproduces all twenty-seven figures
this repository already carries - `FINAL` 55, `PPD` 37, `6TH INN` 74, `4TH QTR`
79, `3RD QTR` 80, `HALFTIME` 94, `OVERTIME` 97, `12:00 AM` 78, `7:30 PM` 67,
`2ND P...` 66, `2ND PERIOD` 112, `Hurricanes` 94, `Mammoth` 86, `Capitals` 70,
`Est. value` 112, `Sunrise` 89, `Sunset` 81, `$400K - $450K` 161,
`$9999K - $99999K` 200, the 323/235/83 compliance wrap, the two addresses at 384
and 518, and `$275/sq ft - updated Sep 25` 232 - to the pixel before it was
trusted on a digit. Four of those were checked at the wrong face on the first
attempt and came out 25% low, which is what a reproduction gate is for: the
figures were right and this file's reading of which face they belonged to was
not.

**What the key tests did against `main`, said plainly.** The design document
went in first, then the tests, then the code, in that order:

- The new score-column block in `tests/card-columns-test.cpp`, compiled against
  the tree with `kBoardScoreWidth` still at 26, fails at
  `score.sans12 <= kBoardScoreWidth` on line 214 with no `PASS` line.
  **Verified** on this workstation's `g++ 16.2.0`.
- The two new `widestBoardStatus()` checks in
  `ci/check-board-columns-call.sh`, run against `main`'s own `App/Display.cpp`,
  print two `ERROR:` lines and exit 1. **Verified** locally.
- The constant was changed after both. The `static_assert`s holding the score
  column against its measurement were added with it, so they are **not** a
  failing-first check and are not counted as one.

**Counts, before and after.** `App/CardColumns.h` 70 `static_assert`
declarations to 75; `grep -c static_assert` reads 72 to 78, because two comment
lines and then three name it in prose. `tests/card-columns-test.cpp` 173
`assert`s to 193, in one binary whose own `PASS:` line is what
`ci/test-card-columns.sh` reads. `ci/check-board-columns-call.sh` 31 text checks
to 44. `App/Display.cpp` gains one function and no `static_assert`. Every host
script passes by its own `PASS:` or `Passed!` line, read off each runner rather
than off an exit code.

Thirty-eight of those asserts and eight of the checks were added after the three
reviews, to close what the adversarial pass got past. The table further down
says which.

#### The mutations, and what caught each one

Thirteen that landed, each applied to a byte-for-byte copy taken first, each
gated on `cmp` saying the file actually changed before the check ran, each
restored from that copy and `cmp`'d and `touch`ed after. A mutation that did not
land is recorded as such rather than as a pass.

One did not land. `F` was first aimed at `App/CardColumns.h` when the table it
changes is in `tests/card-columns-test.cpp`, and `perl` reported "pattern occurs
0 times" rather than replacing nothing. That is the gate on the gate, and it is
why it is there. Re-aimed at the right file it landed and was caught.

| # | the change | caught by |
|---|---|---|
| A | `kBoardScoreWidth` 28 to 26, the defect restored | `static_assert`, "a one-line score column is the widest two digits of Sans12" |
| B | `kBoardScoreWidth` 28 to 40, wider than measured | `static_assert`, "a row with scores keeps the 81px name column" |
| C | `kBoardWidestTwoDigitScoreWidth` 28 to 27 | `static_assert`, the same equality |
| D | `kBoardNameFloor` 29 to 31, the floor not re-derived | `static_assert`, "a scheduled row that reported one score still holds a start time" |
| E | `kBoardStackedScoreWidth` 20 to 19 | `static_assert`, the two-line equality |
| F | the score table says `11` measures 26 at `Sans12` | the host test, `widestTwoDigit12 == kBoardWidestTwoDigitScoreWidth` |
| K | the score table says `100` fits `Sans12` | the host test, `score.sans12 > kBoardScoreWidth` |
| L | the photographed-name count back to seven | the host test, `cutOnOneLine == 8` |
| M | the score table says `11` is 25 at `Sans9` | the host test, `score.sans9 <= kBoardStackedScoreWidth` |
| G | `widestBoardStatus()` back to the raw width | the gate |
| H | `drawnTextWidth()` measures its argument | the gate |
| I | `drawnTextWidth()` stops going through `copySlice()` | the gate |
| J | `layoutLine()`'s overflow test back to the raw width | the gate |

**Five of the thirteen were caught by a `static_assert` and not by the host
test**, which is worth writing down rather than counting as five test results:
the header's assertions fire at compile time, so the sweep never runs and the
runtime block that reads the score table never executes. F, K, L and M are the
four that prove that block itself bites, and they were found by mutating the
measured table rather than the constant. The one run where the runtime
assertion on line 214 did fire is the failing-first run above, taken before the
`static_assert`s existed.

**G through J are the whole of the control-byte fix's coverage.** No host test
compiles `App/Display.cpp`, so a text gate is what holds those four lines, in
the same spirit as 18e's and 18f's. J is the one the gate did not have before
this section: `layoutLine()`'s overflow test was the second caller reading raw
bytes and nothing read it, so the check was added with the fix.

#### Three reviews, and the regression two of them found

Three separate agents, one review each: correctness against the design document,
adversarial, and integration. Each re-derived the figures rather than reading
them. They are what the rest of this section's second half comes from.

**Two of the three independently found a regression the first version
introduced, and the integration pass found it as well.** `drawnTextWidth()`
copied the whole string through `copySlice()`, which turns every byte at or
below 0x20 into a space, including the ones at the ends. `layoutText()` drops
those: it skips leading break space before it starts a line, and a line ends at
a word boundary, so trailing break space is never in the slice it draws. So the
new measurement was 5px per leading or trailing control byte **over** the drawn
width, where `lcd.textWidth()` had been exact for them. Measured: `FINAL<0x01>`
measured 60 and drew 55; `7:30 PM<0x0A><0x0A>` measured 77 and drew 67; a status
of one control byte measured 5 and drew nothing.

It reached the panel through the one caller that does not measure again.
`widestBoardStatus()` handed a board whose widest status was `FINAL<0x01>` a 62px
column instead of 56 and took 3px off each name, for a string the header asserts
in so many words costs the names no pixels. Two trailing control bytes on a
start time put the 240px one-line row on `kBoardNameFloor`. **Verified** by a
sixth reimplementation of `text_width()` run against both paths.

`drawnTextWidth()` strips leading and trailing break space before it copies now,
and the design document's claim that a control byte at either end costs nothing
is a claim about the code rather than about `text_width()` alone. Two gate
checks hold the two trim loops. The fix is **ready to review and not reviewed**:
the three reviews above read the version without it.

**Nine more mutations survived the first version's checks.** The adversarial
pass landed 38 and 27 survived; the ones worth closing are closed:

| what survived | what holds it now |
|---|---|
| `drawnTextWidth()` with the `!` dropped from its `copySlice()` guard, which returns the raw width on every string that fits the buffer | the gate pins the guard with its `!`, not the call |
| `layoutLine()`'s `text[0] != '\0'` flipped to `==`, which turns the fitting ladder off for every real string | the gate pins the whole four-term predicate |
| `strlen(text)` to `sizeof(text)`, which measures four bytes of every string | the gate pins `size_t to = strlen(text);` |
| `drawnTextWidth(rows[i].status) / 2`, which halves every status column | the gate pins the whole statement including its semicolon |
| `copySlice()`'s `? ' ' : c` dropped, which restores the mismatch from the other side | a gate check over `copySlice()`, matched on the raw text because `has()` strips the space out of the `' '` literal |
| `isBreakSpace()`'s `<= 0x20` to `< 0x20`, after which nothing in the binary wraps | a gate check over `isBreakSpace()` |
| `copySlice()`'s `length + 1 > capacity` to `length > capacity`, a one-byte overrun | a gate check over the bound |
| nine of the fourteen figures in the new score tables, which were read only as a ceiling, a maximum and a count | `scoreWidthByRule()` checks every entry in both tables against the per-digit rule the tables encode: 13 per digit at `Sans12` and 14 for a `1`, 10 per digit at `Sans9` |
| the three new score `static_assert`s, which were `>=` and passed when flipped to `<=` | they are `==`, so a column wider than the digits fails as well as one narrower |

All thirteen of those re-run and are caught, each gated on `cmp` first and
restored and `cmp`'d after. The score-table rule catches four on its own,
including a `kOneExtraAdvance12` of 0, which is the glyph table's own claim
about `1` stated as a figure a run reads.

**One mutation still survives and no equality in this file resists it.**
Flipping a `static_assert`'s `==` to `<=` passes, because both sides are equal.
That is a mutation of the assertion rather than of the code, and it is true of
every equality in `App/CardColumns.h`. What the `==` does buy over the `>=` it
replaced is the other direction: a score column of 40 now fails where it passed.

**What the reviews found and this section did not fix:**

- `copySlice()`'s own correctness beyond those two checks, and `isBreakSpace()`'s
  treatment of 0x7F, which is outside the predicate and so is not a break
  opportunity. `2ND<0x7F>QTR` is one unbroken word and gets hard-broken rather
  than wrapped. Pre-existing, not touched.
- `LGFXBase::decodeUTF8()` is stateful across calls, so a string ending in a
  truncated UTF-8 lead byte leaves the decoder mid-sequence and the next
  measurement eats a byte. Pre-existing; `drawnTextWidth()` makes one
  `textWidth()` call where the old code made one, so the call count is unchanged.
- `drawnTextWidth()`'s fallback for a string longer than the buffer is
  unreachable from either caller: a status is 16 bytes and `layoutLine()`'s
  callers are far inside 128. The first version's remark claimed the fallback
  could not change an answer, which is false for a string of mostly control
  bytes; it now says the branch is unreachable instead, which is what holds.
- `+128 bytes` of peak stack if the compiler keeps `drawnTextWidth()`'s buffer
  live across the `layoutText()` call. The two buffers are never both live by
  construction. Arithmetic against a 16384-byte loop task stack, not a
  watermark.

#### What 18i does not cover

- **Nothing has been on glass.** Every figure is a glyph table, a `g++` run or a
  compile. Whether a 28px score column beside a 41px name reads at arm's length,
  and whether the two pixels each name gave up can be seen, is what a photograph
  answers and none has been taken. This is the open item.
- **`Jets` is now cut on the landscape row.** The 240px one-line name column
  goes 36 to 34 and `Jets` measures 36, so the one name of 15i's photographed
  eight that the landscape row still spelled is cut. The two-line row's 97 holds
  all eight, so the panel that board was photographed on is unaffected. Counted
  by the host test rather than argued about, and not looked at.
- **A three-digit score is still cut**, 40px of `Sans12` into 28 and 30px of
  `Sans9` into 20, past every tier the ladder has, so `layoutLine()` falls
  through to `layoutText()`, which backs characters off until the line plus the
  mark fits and lands on a bare `...` with no digits in it. This bullet said it
  drew `10` for `105` when it was written; section 15k corrected that and this
  copy was missed. Asserted as the gap it is in `kThreeDigitScores`. A basketball
  board is where it shows, nothing in 18i fixes it, and **section 18j does.**
- **No host test compiles `App/Display.cpp`**, so `drawnTextWidth()` is held by
  a text gate and not by a run. The 5px a control byte costs is a figure in a
  design document and in this section; nothing executes
  `LGFXBase::text_width()` outside an `arduino-cli` build.
- **Three gate checks now stand over `copySlice()` and `isBreakSpace()`**, which
  nothing read until `drawnTextWidth()` depended on them. They pin the
  substitution, the `<= 0x20` boundary and the `length + 1 > capacity` bound.
  What they do not read is the rest of either function, and the gate is a
  substring matcher: it says the expression is present, not that nothing else
  is.
- **A control byte has never been sent.** The fix is for a payload the wire
  allows and no device has produced, so the before column of 15k's table is read
  off two routines rather than off a panel.
- **The 240px one-line row is not a panel the fleet has.** The 240px board is
  profile and draws the two-line form, so the figures that moved most are the
  arithmetic's rather than any fielded display's. The 320px landscape board is
  the one that changes on glass, by two pixels of name.
- **A rollout draws the same payload two ways.** `SportsBoardRow`,
  `Sports::Game` and the check-in payload are byte-for-byte what they were and
  no field was added, removed or reinterpreted, so the 6-month compatibility
  gate is untouched. A device without this draws a `10` a tier small.

### 18j. A three-digit score draws (2026-11-02)

The gap 18i recorded and asserted. `100` is 40px of `Sans12` and 30 of `Sans9`,
over both score columns and past every tier the ladder has, so `layoutLine()`
fell through to `layoutText()` and the ellipsis loop backed off to `...` with no
digits in it. A basketball board drew that wherever a side was over 99.
TEXT_LAYOUT_DESIGN.md section 15l is the design and the four shapes it was
chosen from.

**The shape.** The board's score state is an `enum class BoardScores { None,
TwoDigit, ThreeDigit }` where it was a `bool scoreless`, the score column is 30
in the three-digit state against 28 one-line and 20 two-line, and a board
carrying a three-digit score draws **every** score on it at `Sans9`. The face is
the board's decision rather than a row's or a score's, because a `105` drawn a
tier below the `98` beside it is the defect 18i fixed and deciding per score
would put it back.

**Why an enum and not a second bool.** Two bools admit a fourth combination that
means nothing, scoreless and three-digit at once, and let a caller swap them.
`App/CardColumns.h` already carries a scar about an `int` converting to `bool`
without a diagnostic. An `enum class` converts from nothing, so a forgotten or
swapped argument stops the compile; the conversion of 19 signatures and every
assertion is what that cost.

**The measurements**, off the vendored glyph tables by 9b's method, the
reimplementation re-run and reproducing all twenty-seven of this repository's
figures before anything new was read off it:

| digits | `Sans12` | `Sans9` |
|---|---|---|
| two | `11`, 28 | 20 |
| three | `111`, 42 | `100`, 30 |
| four | `1111`, 56 | 40 |

Every three-digit score is exactly 30 at `Sans9`, because all ten digits advance
10 in that face, so one column holds all 900. That is the whole reason the face
drop is cheap: 30 against the two-digit 28 is 2px, where keeping `Sans12` and
widening to 42 is 14.

**What it costs, and only when it engages.** A board with no three-digit score on
it is untouched in every figure, so nothing the owner has photographed changes:

| | two-digit | three-digit |
|---|---|---|
| 320 one-line name, board of `FINAL`s | 81 | 79 |
| 240 one-line name, board of `FINAL`s | 41 | 39 |
| 240 two-line name, board of `FINAL`s | 110 | 100 |
| 240 two-line name, photographed board | 97 | 87 |
| 320 two-line name, photographed board | 177 | 167 |
| one-line ceiling, 320 / 240 | 160 / 80 | 156 / 76 |
| two-line ceiling, 320 / 240 | 217 / 137 | 207 / 127 |

2px on a one-line row because the two names split it, 10 on a two-line row
because the one name pays the lot. **Every column edge is unchanged in all three
states**, which the host test sweeps rather than asserts at one width:
`nameWidth + scoreWidth` has no score width in it, so `homeNameLeft` is 129 and
`homeScoreRight` 238 on a 320px board of `FINAL`s whether the column is 0, 28 or
30.

**What the key tests did against the branch before the code, said plainly.** The
design document went in first, then the tests, then the code:

- The new three-digit block in `tests/card-columns-test.cpp`, compiled against
  the branch's own `App/CardColumns.h` before the enum existed, fails to build
  with 7 errors - `'BoardScores' has not been declared` five times and the two
  new constants undeclared - and prints no `PASS` line. **Verified** on this
  workstation's `g++ 16.2.0`. A new type cannot be asserted against before it
  exists, which is the same failing-first shape 18h recorded.
- The gate's `boardScoreState()` checks, run against the branch's own
  `App/Display.cpp` before the fold was renamed, print
  `boardHasNoScores() not found` and exit 1. **Verified** locally.
- The `static_assert`s for the three-digit state went in with the enum, not
  before it, and are not counted as a failing-first check.

**Counts, before and after this section.** `App/CardColumns.h` 75
`static_assert` declarations to **99**; `grep -c static_assert` 78 to 102.
`tests/card-columns-test.cpp` 193 `assert`s to **231**, and its sweeps run over
three score states where they ran over two, so the swept combinations rose by
half again. `ci/check-board-columns-call.sh` 44 text checks to **52**. Every host
script passes by its own `PASS:` or `Passed!` line.

**The binary.** `arduino-cli`'s own line for App on esp32:
`Sketch uses 1579571 bytes (80%) of program storage space`, against 1579399 for
the same sketch before this section - 172 bytes for the enum, the fold and the
face. Built with `--export-binaries` this time, so
`App/build/esp32.esp32.esp32/App.ino.bin` at 1,579,728 bytes is an artifact this
run produced rather than a stale one; 18i's note about those timestamps is why
the flag is there.

#### The mutations, and what caught each one

Seventeen, each applied to a byte-for-byte copy taken first, each gated on `cmp`
saying the file actually changed before any check ran, each restored from that
copy and `cmp`'d and `touch`ed after, with CR and LF byte counts compared on the
mutant as well as the restore.

**Four did not land on the first attempt**, all four because the pattern spanned
two lines and `perl -p` matches a line at a time, so it reported "pattern occurs
0 times" rather than replacing nothing. Re-aimed at a single line each, all four
landed and all four were caught. That is the gate on the gate doing its job, and
it is the third section running where it has earned its place.

**`perl -pi` strips every CR on this toolchain.** The harness puts them back on
the mutant before any check reads it, and the restore is from the copy, so the
tree never carries an LF-only file. It is the same hazard the brief names for
`sed -i`, and it bit twice in this section before the harness handled it.

| # | the change | caught by |
|---|---|---|
| L1 | the one-line three-digit arm returns the two-digit column | `static_assert`, "a three-digit board's one-line score column holds three digits of Sans9" |
| L2 | `kBoardThreeDigitScoreWidth` 30 to 29 | `static_assert`, the 79px name |
| L3 | `kBoardThreeDigitScoreWidth` 30 to 42, the `Sans12` figure | `static_assert`, the four-digit gap |
| L4 | both arms of the stacked column read the three-digit width | `static_assert`, the two-line two-digit equality |
| L5 | the `None` test becomes a `TwoDigit` test | `static_assert`, the scoreless start time |
| L6 | the fold reads only the away side | the gate |
| L7 | the three-digit boundary moves to `> 3` | the gate |
| L8 | the fold keeps the last rather than the longest | the gate |
| L9 | `bigScores` uses `||` instead of `&&` | the gate |
| L10 | `bigScores` tests `==` instead of `!=` | the gate |
| L11 | the face ignores `bigScores` | the gate |
| L12 | the 2px lift kept on a `Sans9` score | the gate |
| L13 | the grid gets a literal state instead of the fold's | the gate |
| L14 | the test's 240 two-line three-digit name says 97 | the host test |
| L15 | the one-line cost says 4px rather than 2 | the host test |
| L16 | the swept state list drops `ThreeDigit` | `static_assert`, `kScoreStateCount` |
| L17 | the cut-name count says none crosses | the host test |

**L16 is the one worth naming.** The sweeps run over an array, and an array that
drops a state sweeps two thirds of the behavior while every assertion in it
still passes. `kScoreStateCount` is checked against the enum's own last value,
so the array and the type cannot drift: a state added to `BoardScores` and not
to the array stops the compile. That check exists because this exact mutation was
the one I could not otherwise see.

**Five caught by a `static_assert` and not by the host run**, for 18i's reason:
the header's assertions fire at compile time, so the sweep never executes. L14,
L15 and L17 are the three that prove the three-digit block itself bites, and all
three mutate a measured figure rather than a constant.

#### Three reviews, and the twelve mutations they got past the checks

Three separate agents, one review each: correctness against the design document,
adversarial, and integration. All three read `ea42ce5` as well, which 18i left
unreviewed, and all three re-derived the figures rather than reading them. The
correctness pass rebuilt `text_width()` from scratch and reproduced 24 of this
repository's figures before trusting it on a digit; the adversarial pass landed
38 mutations.

**No drawn-pixel defect was found in the code.** The arithmetic, the fold, the
face, the lift and the trim all check out, and `ea42ce5`'s trim was checked
exhaustively over 19,607 strings of length 1-5 across `{A, B, 0x20, 0x01, 0x7F,
0x80, 0xC3}` with zero disagreements between `drawnTextWidth()` and the width of
the line `layoutText()` builds.

**What was found was twelve single-token changes the checks did not catch**, four
of them serious. All twelve are closed and re-run:

| what survived | why it mattered | what holds it now |
|---|---|---|
| `++from` to `++to` in the leading trim | **never terminates.** `from < to` stays true while `to` grows and the same byte is re-read, on a path every `layoutLine()` call reaches. A watchdog reset, not a wrong board. | the gate pins the loop with its body |
| `--to` to `--from` in the trailing trim | `from` underflows to `SIZE_MAX`, the loop exits, and `copySlice()` then reads `text[-1]`. **Out of bounds.** | the gate pins that loop with its body |
| `if (from == to)` to `!=` | every string with ink in it measures 0, so the fitting ladder is off for every card | the gate pins the guard and its return |
| `size_t from = 0` to `= 1` | every measurement drops its first glyph | the gate pins the seed |
| `copySlice()` terminating at `buffer[0]` | every string this file draws becomes empty | the gate pins the terminator beside the bound it needs |
| `longest = home` to `= away` in the fold, and the mirror | a board whose long score is on one side gets the other side's state, which lays out 28px and draws 40px of digits into it | the gate pins both comparisons with their assignments |
| `widest = measured` to `= 0` | no board draws a status column | the gate pins that assignment |
| `Align::Right` to `Left` on a score | `awayScoreRight` is a right edge, so the digits paint rightwards through the home name 6px on | the field loop carries the alignment, and splits on `|` because `Align::` has colons in it |
| the names drawn at `Sans12` | the name column is sized in `Sans9`, so two faces and two baselines land on one board | a `setFont` check before `layoutLine(row.awayName,` |
| `kAllScoreStates` repeating a state | takes a state out of every sweep with every assertion in them still passing | each enumerator is counted once |
| a `FourDigit` appended to `BoardScores` | falls through to the two-digit column with nothing firing, and a count check cannot see an append | `scoreStateName()` is a switch with no default, so `-Wswitch` under `-Werror` stops the compile |

The field loop's delimiter is worth naming: the patterns now carry `Align::Right`,
and `${pair%%:*}` truncated each one at the first colon, so the loop was checking
up to `Align` and the alignment was unpinned by the very edit that added it. It
splits on `|` now.

**Four of the twelve were pre-existing**, not introduced by this section: the
score alignment, the name face, and `copySlice()`'s terminator date from before
`dbb5adb`, and the two trim loops came in with `ea42ce5`.

**The documentation findings are the rest of the review**, and they were the bulk
of it. Corrected: four digits draw `1...` and not the bare `...` four places said
(the pre-15l three-digit case was the bare one); "every column edge is unchanged
in all three states" was unscoped and is now stated with the boundary, which the
host sweep finds rather than asserting past; the NBA count was 31 and 28-against-29
where there are 30 nicknames and it is 27-against-28, with the one crossing name
unchanged; `154px` for `174px` in a figure this branch edited; thirteen sentences
the parameter rename mangled; two stale `static_assert` counts in `ci/`; a 14px
cost in the gate's header that is 2px and 10px; a comment this section's own
commit invalidated; and a remark citing the `int`-to-`bool` hazard the enum class
closed, which now reads in the past tense.

#### What 18j does not cover

- **Nothing has been on glass**, and this section changes something the owner
  has seen: a landscape board's scores have drawn at `Sans12` since the card was
  written, and a basketball board's now draw at `Sans9`, the same size as the
  team names. The two-line portrait row already did that in 15j and was
  photographed; the landscape row in this state has not been looked at. This is
  the open item.
- **`Hurricanes` is cut on a portrait board with a three-digit score.** 94px
  against the 87px name a three-digit board leaves. No hockey board reaches the
  state so the photographed board cannot show it, and the host test counts the
  photographed eight against both columns rather than asserting a win. Of the 30
  NBA nicknames, 27 fit 87 against 28 at 97, so one crosses: `Mavericks` at 88.
  **Verified** off the glyph table, not on a device. The first count of this read
  31 and 28 against 29, which double-listed the Blazers; the one name that
  crosses is the load-bearing half and it did not move.
- **A 240px one-line board with three-digit scores cuts the widest start time**,
  80 to 76 against `12:00 AM`'s 78. Asserted as the loss it is, in the header
  and in the host test's two branches, so closing it has to come through those
  lines. The 240px panel on the fleet is portrait and draws the two-line form,
  so no fielded board has this shape.
- **Four digits still ellipsize**, 40px of `Sans9` against the 30px column, and
  they draw `1...` rather than the bare `...` a three-digit score drew before
  this section: `layoutText()` backs characters off until the line plus the mark
  fits, and at `Sans9` that keeps one digit. `1000` draws `1...` at 22px and
  `32767` draws `3...`. **Verified** by simulating `layoutText()`'s ellipsis loop
  against the glyph table. Asserted as a gap the way three digits were in 18i. No
  sport this card carries sends one.
- **No host test compiles `App/Display.cpp`**, so `boardScoreState()`, the face
  and the lift are held by the text gate rather than by a run. The gate is a
  substring matcher and says an expression is present, not that nothing else is.
- **The 2px and 10px are arithmetic, not a photograph.** Whether a 79px name
  reads differently from 81, and whether an 87px name on a portrait board reads
  differently from 97, is what a photograph answers.
- **Nothing on the wire changes.** `SportsBoardRow`, `Sports::Game` and the
  check-in payload are byte-for-byte what they were; `BoardScores` is derived on
  the device from score strings the row already carries. No field was added,
  removed or reinterpreted, so the 6-month compatibility gate is untouched. A
  device without this draws `...` for a three-digit score.

---

### 18k. A negative score is not a scoreline (2026-11-02)

18j left `-10` putting the whole board in the three-digit state, because
`boardScoreState()` keys on the score STRING and `-10` is three characters. The
board then drew every score on it at `Sans9` and took 2px off each one-line name
or 10 off each two-line one, for a board whose largest number is 10.
TEXT_LAYOUT_DESIGN.md section 15m closes it on the data rather than on a column
width: a score below zero is not a scoreline, so `CheckIn.cpp` stores
`Sports::kNoScore` for the whole negative range and both formatters in
`Sports.cpp` write an empty cell for it.

**The defect was read off the source before anything was changed.** `strlen` on
both score strings, `longest >= 3 ? ThreeDigit : TwoDigit`, and
`bigScores = !stacked && scores != BoardScores::ThreeDigit` at the call site, so
one three-character score on any row drops the face on all four. **Verified** by
reading `App/Display.cpp:3632` and `:3781`. A negative reaches that fold:
`CheckIn.cpp` parsed `game["homeScore"] | -1` into an `int16_t` with no bound,
and `writeScore()` formatted anything that was not exactly `-1` with `"%d"`.
**Verified** by reading the parse and the formatter. `-1` could not, because it
is `kNoScore` and already drew nothing.

#### The order, and the failing-first runs

The design document went in first, then the test, then the code.

1. TEXT_LAYOUT_DESIGN.md section 15m, and 15l's own paragraph on this case
   rewritten from a recorded gap to a decision, in the same pass. Section 15m
   was then rewritten again after the first review: six of its claims were wrong
   and the load-bearing one is below.
2. `CardColumns::boardScoreIsDrawable()` added carrying **the shipped rule**,
   `score != -1`, and the new block in `tests/card-columns-test.cpp` written
   against it.
3. That build fails on `static_assert(!boardScoreIsDrawable(-2))` with
   "and so does every other negative", exit 1, no `PASS` line. **Verified** on
   this workstation's `g++ 16.2.0`.
4. With the three new `static_assert`s removed so the compile gets far enough to
   run, the binary aborts on `Assertion failed: score >= 0, file
   tests/card-columns-test.cpp, line 1256` and prints no `PASS` line.
   **Verified**. That is the sweep finding `-2` drawable under the shipped rule.
5. The predicate changed to `score >= 0`. `ci/test-card-columns.sh` prints its
   own `PASS: scoreboard columns` line including section 15m's clause.
   **Verified**.

The predicate is new, so step 3 on its own is only the shape 18h and 18j record,
where a name that does not exist yet cannot be asserted against. Step 4 is the
one that bites: the same sweep, compiling, failing at a real assertion on the
behavior `main` shipped.

**The written justification was wrong where it mattered, and a review found it.**
The first 15m said a fold counting digits instead of characters would hand `-10`
a 28px column, go past every tier of the fitting ladder, and ellipsize to `-...`.
It would not. `layoutLine()` steps **down** `kSansTiers` from the caller's face:
`Sans12` measures 35 against 28 and is skipped, `Sans9` measures 26 and its 22px
block sits inside the 29px slot, so the score draws **whole, at `Sans9`, one tier
below the digits beside it**. That is section 15k's defect and 15l's rejected
option C, which is a better argument against the alternative than the one that
was written, and 15m's own measurement table had the 26 in it all along.
**Verified** by reading `layoutLine()` at `App/Display.cpp:1483` against the
measured widths. The same wrong mechanism had reached five places - 15m twice,
15l's rewrite, a `CardColumns.h` comment block and a `static_assert` message, and
a comment in the host test - and all five were corrected. The assertions
themselves still held; it was the explanations that were false.

**And the pixel case against a fourth state only holds on one row shape.** 15m
said a `BoardScores::NegativeTwoDigit` would cost 8px off each one-line name
against the face drop's 2, which is right, and then said 8px off a two-line name,
which is not: that row has one score column and it is already `Sans9`, so a
fourth state would be sized at 26 rather than 36 and would cost its one name 6px
against the face drop's 10. On the 240px portrait row - the only 240px shape the
fleet draws - the rejected alternative is the cheaper one. **Verified** through
`boardStackedColumnsFor()`'s own arithmetic: 110 at a 20px column, 104 at 26, 100
at 30. 15m now says the pixels do not settle it and rests the decision on what
does, which is that a fourth state threads a fourth case through nineteen
signatures to hold a number no sport this card carries can produce.

#### The mutations, and what caught each one

Each one applied to a byte-for-byte copy and **`cmp`-proved to have landed**
before the runner was read, then restored and `cmp`-proved identical again.
**Twenty-five mutations, twenty-five caught**, each by a check that names the
thing that moved.

`sed` and `awk` both strip every CR from these CRLF files in the same pass, so
two of the runs below changed the line endings as well as the token. A **control
run** is what makes them readable: CRs removed from all five files with **no**
token changed, and both `ci/check-score-guard.sh` and `ci/test-card-columns.sh`
pass by their own lines. Run again after the gate was hardened, because the
first control predated half of these checks. **Verified** both times.

**Ten of these came from the adversarial review**, which found the first version
of this gate green on nine ways of leaving the behavior wrong. An eleventh came
from this sweep finding a hole in its own gate: a `Log::printf(` pinned by
presence alone stayed present as `(void)0; if (false) Log::printf(...)`. The pin
is now the guard, the call immediately inside its brace, and the arguments.

| mutation | what it would draw | caught by |
|---|---|---|
| `score >= 0` to `score != -1` | the shipped defect, exactly | `ci/check-score-guard.sh`, and the host test's `static_assert` |
| `score >= 0` to `score > 0` | a nil-nil draw stops drawing | the gate, and `static_assert(boardScoreIsDrawable(0))` |
| a second `boardScoreIsDrawable(int16_t)` overload beside the pinned one | both formatters bind the overload and go back to `!= -1`; the pinned text never moves, and the host test passes an `int` | the gate's declaration count |
| `kNoScore` from `-1` to `0` in `App/Sports.h` | every game that has not started stores a drawable zero, so all four rows draw `0`, `None` becomes `TwoDigit`, and the score columns take their pixels off every name | the gate |
| `storedScore`'s parameter from `int` to `int16_t` | the narrowing moves ahead of the guard, so -65536 arrives as 0 and draws a nil | the gate |
| `\| static_cast<int>(Sports::kNoScore)` to `\| 0` | same picture as the `kNoScore` mutation, from the other end | the gate |
| ` && fromWire <= INT16_MAX` deleted | 65536 folds to 0 and draws a nil-nil | the gate |
| `if (fromWire < -1) return 0;` inserted above the guard | a `-10` stores 0 and draws a nil, logging nothing | the gate |
| `storedScore(game["homeScore"]` to `["awayScore"]` | the home line draws the away score | the gate |
| a second unguarded `slot.homeScore =` appended after the guarded one | the guard is bypassed and the last write wins, which is what a bad merge leaves | the gate's assignment count |
| `return Sports::kNoScore` to `return fromWire` | the parse guard does nothing | the gate |
| the parse guard negated | every drawable score stored as `kNoScore` | the gate |
| the log guard to `fromWire < 0` | nothing drawn differently, so nothing visible | the gate |
| `Log::printf` left present but unreachable | the same, and the pinned text is still in the file | the gate |
| the log's `away, home` swapped | the stream names the wrong game | the gate |
| the `!` dropped in `writeScore()` | every drawable score writes an empty cell, so no board draws scores | the gate |
| `writeScore()`'s guard deleted and left as one `// Was:` comment line | a `-10` writes a signed string and `kNoScore` writes `-1` | the gate, after it learned to strip comments |
| `"%d"` to `"%+d"` in `writeScore()` | a 10 writes `+10`: three characters, so the board drops a face with no negative anywhere | the gate |
| `scoreText()`'s arms swapped | the single-game card draws the sentinel and blanks real scores | the gate |
| `scoreText()`'s return deleted and left as a comment | the card prints `-1` for every game that has not started | the gate |
| `writeScore()` back to `score == kNoScore` | two rules for one question, and the two cards drift apart | the gate, twice |
| `kNoScore == score` instead | the same, written the other way round | the gate |
| a `'-'` case added to `boardScoreState()` | the symptom patched where the guard belongs | the gate |
| the same case written `== 0x2D` | the same, past a check that looked for the character | the gate, after it was pinned on the statement instead |
| either negative width constant drifting from the glyph table | a figure the design document reasons from stops being the measurement | the host test's two sum identities |

The log mutations are the ones with no drawn consequence, and they are pinned
anyway: the stream is a deployed device's one diagnostic channel, so a check-in
that drops a number and says nothing about it is the failure nobody can
investigate afterwards.

#### Counts, before and after this section

`App/CardColumns.h` 99 `static_assert` declarations to **108**;
`grep -c static_assert` 102 to 111. `tests/card-columns-test.cpp` 231 `assert`s
to **256**. `ci/check-board-columns-call.sh` stays at 52 text checks, and the new
`ci/check-score-guard.sh` carries **21**. Every host script passes by its own
`PASS:` or `Passed!` line, and every text gate by its own `==>` line.

`ci/check-score-guard.sh` is invoked from `ci/build-firmware.sh` beside the four
gates already there, and as its own step in `mut-host-test.yml`. That wiring is
part of this section rather than a detail: the adversarial review's last note was
that it had not checked whether anything ran the new gate, and nothing did.

#### What 18k does not cover

- **Nothing has been on glass, and nothing on glass changes for a real score.**
  All five sports `sportChipLabel()` maps - american-football, football,
  basketball, baseball, hockey - score zero or above, so no board the fleet is
  drawing moves a pixel. The scores this changes are ones the wire can carry and
  no league produces. **Verified** by reading `sportChipLabel()`. That is why no
  photograph was taken, and it is also why a photograph would not have shown
  anything a provider can produce.
- **One layout does move, and only a crafted payload reaches it.**
  `showSportsCard()` at `App/Display.cpp:3529` decides whether to reserve its
  76px score column from `homeScore.length() > 0 || awayScore.length() > 0`. A
  game whose two scores are both negative used to give two non-empty strings and
  keep the column; both are empty now, so the card draws the "not started" shape
  and the two club names take the whole panel. Correct for a figure the card
  refuses to draw, **Verified** by reading those lines, and **nobody has looked
  at it**. It was found by the integration review, not by the change.
- **The impression summary stops carrying the refused number.**
  `describeCardAt()` at `App/Sports.cpp:425` builds `Impressions::Entry::summary`
  through the same `scoreText()`, so what the device reports as having been on
  screen goes from `Penguins -5 @ Capitals` to `Penguins @ Capitals`. Free text,
  no schema moves, and it matches what the panel drew. **Verified.** It reaches
  the server's impression store, which is the one place this change is visible
  off the device.
- **The log has a ceiling and this can reach it.** Four cards of four games with
  both sides negative is 32 lines in one check-in; `Log`'s pending ring holds
  exactly 32 (`App/Log.cpp:181`), so that check-in's other buffered lines are
  evicted. Needs a server sending negatives on all sixteen games. Serial still
  carries everything. **Verified** from the two constants, not reproduced.
- **The gate strips line comments but not block comments.** `//` is taken out
  before matching, because a fixed-string pin is otherwise satisfied by a
  commented-out copy of the statement it pins - which an adversarial review
  demonstrated on two checks. `/* */` appears nowhere in the four files, so a
  pinned statement hidden in one would pass. Recorded rather than closed.
- **The gate holds spellings, not properties.** The sentinel check reads four
  ways of writing `score == kNoScore` and the predicate count reads one
  declaration; both came from an adversarial pass that got past the first
  versions. A fifth spelling is a fifth hole, and a fixed-string gate cannot do
  better than a list.
- **No host test compiles `App/CheckIn.cpp`, `App/Sports.cpp` or
  `App/Display.cpp`.** All three name an Arduino type or a graphics library. The
  predicate and its sweep are host-run; that the three files call it is held by
  `ci/check-score-guard.sh`, which is a fixed-string matcher over flattened
  function bodies and says an expression is present, not that nothing else is.
- **The three reviews were run, and all three changed the change.** Separate
  agents, one charter each. Correctness against the document reproduced every
  figure independently off the glyph tables and found six places where the prose
  was wrong, the load-bearing one being that a digit-counting fold would not have
  ellipsized `-10` at all. Adversarial found nine ways to leave the behavior
  wrong with both runners green. Integration found the `hasScores` layout, the
  impression summary, the log ceiling and the `Sports.h` invariant that was false
  above `INT16_MAX`. Every one of those is answered above or in section 15m.
  What the reviews did not have is a build: all three ran before
  `ci/build-firmware.sh` finished, so none of them compiled the three files that
  gained an include.
- **A negative score draws the cell a game that has not started draws.** There
  is no mark for "the server sent a figure this card will not draw", and 15m
  records why one was not invented. The stream carries the number instead. A
  reader looking at a board cannot tell the two apart.
- **A positive score over `INT16_MAX` is rejected, and that was a review
  finding rather than the first draft.** The first version guarded the sign
  only, and the narrowing into `int16_t` is modular: 32768 folded to -32768 and
  drew an empty cell, which the formatters' own reading of the predicate caught,
  but **65536 folded to 0 and drew a nil-nil**. A wrong scoreline, not a blank,
  and `App/Sports.h`'s stated invariant was false of exactly that case.
  `storedScore()` now tests `fromWire <= INT16_MAX` beside the sign, and the gate
  pins both halves. **Verified** by reading the fold through the real narrowing.
  What is still not bounded is the log: a 65536 is named in the stream, so the
  number is recoverable, but nothing distinguishes it there from an ordinary
  out-of-range value.
- **Four and five digit scores still ellipsize to `1...`**, which 18j costed and
  left open. This section narrows what can reach that case and does not close it.
- **Nothing on the wire changes.** `SportsBoardRow`, `Sports::Game` and the
  check-in payload are byte-for-byte what they were, and `BoardScores` is still
  derived on the device. No field was added, removed or reinterpreted, so the
  6-month compatibility gate is untouched. A device without this draws a signed
  string for a negative score and drops its board's face; a device with it draws
  an empty cell.

---

## 19. The brand splash's one length, at both ends (2026-11-01)

`Updater::cacheBrandAssets()` tested its write with `written <= 0`, so any 200
response carrying one byte or more was renamed over `/brand.565`: the good
splash removed, the response put in its place, and the journal reporting a
cached byte count. `Display::drawBrandImage()` then refused the file on size
every boot and left it on flash, so the device showed the neutral wordmark with
no route back without a cable.

The length is now one constant, `BrandSplash::kBytes`, read by the write and by
the draw. `ci/test-brand-splash.sh` is the host test.

### 19a. The host test

```
bash ci/test-brand-splash.sh
```

No stubs: `BrandSplash.h` includes only `<stddef.h>`. The truth table is
asserted twice over, as `static_assert` and again at runtime, so a widened
predicate stops the compile on any toolchain and also prints a `Failed!` line
under a runner.

The script reads that line rather than the binary's exit status, the way
`ci/test-config-request.sh` does - added 2026-11-03. Two host gates in `ci/`
still run their binary as a bare last command and read no part of its output,
`test-loader-order.sh` and `test-recovery-menu.sh`, and both of those binaries
print a `PASS:` line nothing looks at. Proven by dropping the
`Passed!` `printf` from `tests/brand-splash-test.cpp`, so `main()` returns 0
having printed only its `==>` label: the script before the change printed that
label and exited 0, and the script after it prints `ERROR: the brand splash test
exited 0 without printing its Passed line.` and exits 1. The mutation was
restored from a byte copy taken first, checked with `cmp`, and the file's mtime
refreshed.

Run on this workstation against the native `g++` 16.2.0 under
`C:\Tools\mingw64\bin`, which section 19b predates; a clean tree prints
`Passed! 16 checks, 0 failed`. CI compiles with GCC 13.3 on ubuntu-24.04 and
neither the green run nor the mutation was measured there.

| Check | What it pins |
|---|---|
| `bodyIsWholeAsset(57600)` | the asset is accepted |
| `bodyIsWholeAsset(1)`, `(512)`, `(57599)`, `(57601)` | a short error page, a dropped body and an oversized one are all refused |
| `bodyIsWholeAsset(0)`, `(-1)` | an empty body and an `HTTPClient` error code are refused |
| `declaredLengthCouldBeAsset(512)`, `(0)` | a stated `Content-Length` that is not the asset stops the run before any flash write |
| `declaredLengthCouldBeAsset(-1)` | a chunked response states no length, so it proceeds to the write |
| `storedSizeIsWholeAsset(512)`, `(0)`, `(115200)` | a file already on flash at the wrong length is not kept |

### 19b. The mutations, and what each one did

Run on this workstation with `xtensa-esp32-elf-g++ -fsyntax-only`, the cross
compiler from the pinned core, because there is no native `g++` here. Each one
restored with the file's mtime refreshed afterwards.

| Mutation | Result |
|---|---|
| `bodyIsWholeAsset` back to `return bytes > 0` - the shipped test | 4 assertions fail: `one byte is not the asset`, `one short`, `one over`, `a short HTML error page carrying a 200` |
| `declaredLengthCouldBeAsset` to `return true` | 2 assertions fail, `a stated 512 cannot be the asset` and `a stated 0`, plus an unused-parameter error under `-Werror` |
| `storedSizeIsWholeAsset` to `return true` | 3 assertions fail, covering an empty, a short and a doubled stored file |

The first mutation is the shipped defect exactly, and the cases it fails are the
four a captive portal, a proxy and a dropped connection produce.

### 19c. The bench check, which has not been run

1. **A short body replaces nothing.** Point a device's `brandAssetPath` at a
   route serving 512 bytes of HTML under a 200 on a device that already has a
   good splash. Expect the journal line `wrote 512 bytes not 57600 -
   discarded, the cached splash stands`, the branded splash still on the glass
   on the next boot, and no `/brand.tmp` left on the partition.
2. **A stated wrong length costs no write.** Same, with a `Content-Length`
   header. Expect `Content-Length 512 is not 57600` and no `wrote` line at all.
3. **A poisoned device heals.** Put a 512-byte `/brand.565` on a device by hand,
   then boot it. Expect the neutral wordmark, `cached splash is 512 bytes not
   57600 - removed (ok)` from whichever end ran first, and the branded splash
   back on the boot after the next refresh.
4. **A chunked response still works.** Serve the asset without a
   `Content-Length`. Expect `cached 57600 bytes`.

Photograph 1 and 3.

### 19d. Known gaps, recorded rather than discovered later

- **Nothing here has been on glass, and no device was flashed or commanded.**
  The defect was found by reading, and the fix is checked by compilation and
  arithmetic.
- **The host test covers the decision, not the filesystem sequence.** That
  `LittleFS.remove()` and `rename()` are not reached when the length is refused
  is read off the source. `Updater.cpp` pulls in `ArduinoJson`, `HTTPClient`,
  `Update`, `mbedtls` and four ESP-IDF headers, so a host build of that
  translation unit needs a stub set larger than the thing it would test. The
  predicate was moved into a header of its own for exactly the reason section
  18c gives for `boardColumns()`.
- **`Display::drawBrandImage()`'s unlink is a patch, not a commit.** The CAL
  `Display.cpp` was held by another pass; the change is written out in that
  pass's report. Until it lands, a poisoned device heals only on a boot where
  `mustContactServer()` is true, which a healthy device starting normally is not.
- **No measurement of the per-boot cost on a device.** The handshake count and
  the 115,200-byte partition peak are read off the partition table and the
  source, not off a running unit.
---

## 20. CAL's screens on a white panel (2026-11-01)

CAL's loader screens changed from white-on-black to black-on-white, matching the
App's day theme. `BOOT_SCREEN_OWNERSHIP.md` section 12 is the palette contract
and says what each colour has to satisfy.

**Nothing below has been seen on glass.** A compile says the constants resolve,
and that is all it says. Every colour here was chosen against a contrast
calculation and a photograph of a different screen, so the whole of this section
is a list of things a person has to look at, and the entries flagged as risks are
the ones the arithmetic could not settle.

**What makes a step fail.** Text you cannot read at arm's length, text the same
colour as what it sits on, a stroke or outline that has disappeared, a filled bar
you cannot tell from an empty one, or any black rectangle with an edge in the
middle of a white screen. Photograph every step, pass or fail, because the pass
photographs are the record that the screen was looked at.

### 20a. The three risks, looked at first

1. **The cached brand frame does not recolour itself, and this is the one that
   bites.** `/brand.565` is a raw RGB565 bitmap with one ground baked into its
   pixels, and the server lane is changing it to composite onto white. A device
   only refetches it in `Updater::cacheBrandAssets()` at `CAL.ino:693`, and a
   healthy device never reaches that line: the `if (!contactServer)` block above
   it hands over to the App and returns. So a working device keeps its
   black-ground frame on the new white panel, which is a black rectangle with a
   visible edge in the middle of the splash, and it keeps it until it next takes
   an update. That can be weeks. The size guard added alongside this change does
   not help, because a recoloured frame is the same 57,600 bytes as the old one.
   Look at a device that has a cached frame and has not updated since, and treat
   what it shows as the thing most households would see first. Whether the frame
   needs a version or a hash in the manifest so a ground change forces a refetch
   is a server-lane question this list cannot answer.
2. **The wordmark's blue.** `kAccent` (`0x2A78D6`) carries "Around Me" under
   "Discover" on the neutral splash, at text size 4 standalone and size 3 as the
   status backdrop. It measures 4.42:1 on white, and this panel renders white with
   a blue cast, which narrows the gap further. A photograph of a real unit on
   2026-10-23 is why `showConnected` stopped using accent blue on white for
   reading text. The wordmark is display type rather than reading text, which is
   why it stayed. Read it from across a room. If it reads as blue on pale blue,
   the change is one constant.
3. **The amber.** `kWarn` (`0xEDA100`) measures 2.17:1 on white, the largest
   single regression in the swap. It draws the "Reset identity" row, both
   confirmation headings, the Reset and Reload buttons, every `showFailure`
   headline, and "Do not unplug" at text size 1. It is unchanged because the App
   keeps the same amber outside its own day/night swap and already draws it on a
   white panel, and one amber across the handover is worth more than CAL being
   right alone. Read "Do not unplug" first, since size 1 is the hardest case.
   `0xB45309` measures 5.02:1 and is the replacement if it fails.

### 20b. A provisioned device, powered on

4. **The cached brand splash.** Power a device that has a `/brand.565` on flash.
   See 20a item 1 for why this is the step that matters most and why it will not
   look right until the device has refetched. Once it has, the logo sits on white
   with no visible frame edge. Photograph it before and after an update cycle.
5. **A poisoned frame heals itself.** Separate from the recolour. Write a file of
   the wrong length to `/brand.565`, boot, and confirm the journal reports the
   size and the removal and the screen falls back to the wordmark. Boot again and
   confirm the journal no longer mentions it, which is what proves the file was
   removed rather than refused twice.
6. **The neutral splash.** A device with no cached frame falls back to the
   two-line wordmark. "Discover" is `kInk`, "Around Me" is `kAccent`, "starting"
   is `kMuted` at size 2. All three legible, and see 20a item 2.
7. **The status ladder.** Watch a whole boot without touching it. "Looking for
   known networks", "Connecting to WiFi", "Checking the time", "Contacting
   service", then "Starting" with a version under it. Each draws the splash
   backdrop above it, so check the wordmark or logo and the status text together
   rather than one at a time. Headlines are `kInk` at size 2, details `kMuted` at
   size 1, and `kMuted` at size 1 on white is the hardest reading case after the
   amber.
8. **The handover.** The moment CAL hands to the App, watch for a flash. Both
   grounds are now `0xFFFFFF` and both inks `0x000000`, so there should be no
   visible change in the panel's white at the swap. A flash means the two binaries
   disagree on a value and section 12.1 is broken.

### 20c. Setting up a device that has no network

9. **The Wi-Fi join QR.** Erase identity or start an unprovisioned unit. The
   screen shows a QR, "Scan to join WiFi", the network name under it, and an
   "Open http://..." line at the bottom. The QR's own light card is `kQrLight`
   and its modules `kQrDark`, which do not come from the theme, so the code
   itself should look exactly as it did. What changed is that the card's edge is
   no longer visible against the panel. **Scan it with a phone.** The quiet zone
   is two modules inside the card and the panel now extends it, so scanning
   should be unaffected or better, and this step is what proves it.
10. **The joined screen.** Join the access point with a phone. `showConnected`
    draws "Connected" at size 3 with a detail line under it, then "Choose your
    WiFi" once the portal is open. This screen used to be the only one drawn dark
    on light, and that inversion was how somebody across a room knew the join had
    worked without reading it. Every screen is dark on light now, so look at
    whether the join still reads as an event from a distance. If it does not,
    that is a design question for the setup flow rather than a defect here.
11. **The registration QR.** A device the server has not claimed shows the
    enrollment QR with a server-written caption. The caption wraps to three lines
    when there is no subcaption and two when there is, so check nothing is cut
    mid-word and the fallback URL at the bottom is typeable. `kInk` at size 1 for
    the fallback is the line somebody types by hand.
12. **The service-provided QR, and "Waiting for setup".** The screens a
    claimed-but-unconfigured device sits on. Server-supplied wording, so read them
    for wrapping as well as colour.

### 20d. The recovery menu and the diagnostics screens

13. **The hold prompts.** Hold BOOT through startup. "Keep holding BOOT to set up
    WiFi" with "Release now to cancel" under it, then the identity-erase prompt on
    a longer hold. Both are `showStatus` over the splash backdrop.
14. **The menu.** Reach the configuration menu by the touch gesture. Five rows,
    each a rounded outline with its label in the same colour as the outline: "Set
    up Wi-Fi" and "Diagnostics" in `kInk`, "Reset identity" in `kWarn`, row 3 in
    `kInk` as "Reload application" or `kWarn` as "Cancel reload", and "Back" in
    `kMuted`. Check every outline is visible and the `kMuted` Back button has not
    faded into the panel. Photograph the menu in both states of row 3.
15. **Both confirmation screens.** Tap "Reset identity", then back out and tap
    "Reload application". Heading in `kWarn`, a body paragraph in `kInk` at size 2
    over six lines, a `kWarn` action button and a `kMuted` Cancel. The identity
    paragraph is 125 characters and the length is load-bearing, so confirm it
    still ends in a full sentence and has not ellipsized.
16. **The presence sensor screen.** Diagnostics, then "Presence sensor". On a
    board with a mapped sensor the signal line reads "Signal HIGH" in `kActive`
    (`0x2E7D32`, the App's green) or "Signal LOW" in `kMuted`, with a bar below
    whose outline is `kMuted` and whose fill is `kActive`. Trigger the sensor and
    confirm the filled bar is unmistakable against the unfilled outline. Then the
    history row of twelve small bars at the bottom, which is the subtlest thing on
    any CAL screen: `kActive` for a high reading, `kMuted` for a low one, and
    `kTrack` (`0xE0E0E0`) for a slot with no reading at all. `kTrack` is meant to
    be nearly invisible and the slot it marks must not look filled. Its
    predecessor was near-black, which on a white panel would have read as the most
    filled bar in the row, so this is the one place in the swap where a wrong
    colour inverts a meaning rather than dimming it.
17. **The presence screen on a Panellan.** That board maps no fixed sensor pin, so
    the same screen shows "No sensor input mapped", "Panellan ESP32-S3" in `kInk`,
    and two `kMuted` lines under it, with the bar drawn as an empty outline.
    Confirm the empty outline is visible.
18. **The GPIO monitor.** Diagnostics, then "GPIO monitor". Both modes, and page
    through all pages of each. Per row: the pin number at size 2 in `kInk`, or in
    `kActive` for two seconds after a change, a value and a HIGH/LOW beside it, a
    small bar whose outline is `kMuted` and whose fill follows the row colour, and
    a `kMuted` line under it. Toggle a pin and confirm the colour change is
    visible, because `kActive` and `kInk` are both dark on white where before one
    was a bright green against black. This is the step most likely to show that
    two dark colours no longer read as different. Also check the "Changes" and
    "All pins" mode button, Previous, Back and Next, all `kMuted` outlines with
    `kInk` labels at size 1.

### 20e. Updating, and failing

19. **The update progress screen.** Offer an update the device can take and watch
    `showUpdateProgress`. "Updating" at size 3 in `kInk`, the version in `kMuted`
    at size 1, a progress bar outlined `kMuted` and filled `kAccent`, a percentage
    in `kMuted`, and "Do not unplug" in `kWarn` at size 1. The `kAccent` fill is a
    solid block rather than text so its 4.42:1 is ample, but see 20a item 3 for the
    amber line beneath it. Catch it part-filled, not only at 0 and 100, since a
    partly filled bar is what proves the fill reads against the outline.
20. **A failure screen.** Easiest to reach by pulling the network during the boot,
    which lands on `showFailure` through `haltWithFailure`. Headline in `kWarn` at
    size 2 over up to three lines, with a `kMuted` size 1 instruction under it. The
    amber headline and the grey instruction are the two weakest contrasts in the
    change sitting on one screen, which is why this is the last step rather than an
    afterthought.

### 20f. What this section does not cover

- **The SelfTest sketch is a separate binary and keeps its own dark palette.**
  `SelfTest/Display.cpp` has its own `kBg`/`kInk`/`kMuted`/`kPass`/`kFail`/`kWarn`
  and is not part of CAL's loader. It was read and deliberately left alone. If the
  product look is meant to reach it too, that is its own change with its own bench
  pass.
- **No host test covers any of this.** There is no native `g++` on this
  workstation and nothing compiles `CAL.ino` outside `arduino-cli`, so a colour
  constant has no automated coverage of any kind. The contrast ratios in section
  12.3 were computed by hand and are arithmetic, not a test.
- **No device has been touched.** Every step above is a human at a bench with a
  powered unit, and none of them has been performed.
---

## 21. A reported count that rolled over (2026-11-03)

`Assets.cpp`'s `recordDecodeFailure()` incremented `gDecodeFailureCount` with a
bare `++` on a `uint8_t`. `gAllocationRefusals`, declared nine lines below it in
the same anonymous namespace, guarded the same type with
`if (gAllocationRefusals < UINT8_MAX)`. The file held two shapes for one
problem, and the unguarded one was on the counter whose value leaves the device.

The figure rides the check-in request as `assetDecodeFailureCount`, and the
4-id cap beside it is a cap on the id list rather than on the count: the dedup
drops a repeat of an id already listed, so each listed asset counts once, and a
fifth bad asset never reaches the list and counts again on every redraw. One
card failing all day is what takes the figure past 255. Wrapped it reads as
fewer failures than occurred, and at exactly 256, 512 and so on it reads as 0 -
which is what `CheckIn.cpp` tests, twice. A count of 0 withholds
`assetDecodeFailureIds` with it and skips `clearDecodeFailures()` after a
successful round trip, so the ids pin and the next cycle opens on a count that
is already wrong.

Both counters now go through `ReportedCounter::countOne()` in
`App/ReportedCounter.h`, which holds at `ReportedCounter::kCeiling`. 255 means
"255 or more", and the refusal log line says `or more` when it is there.
`gCheapRefetchCount` is deliberately left alone: it is compared against
`kMaxCheapRefetches` before it is incremented, so it cannot reach the end of its
type, and that comparison is also the per-boot fetch budget.

### 21a. The host test

```
bash ci/test-reported-counter.sh
```

No stubs: `ReportedCounter.h` includes only `<stdint.h>`. `countOne()` and
`atCeiling()` are `constexpr`, so every case is asserted twice over - as a
`static_assert` over a `constexpr` walk and again at runtime - and a widened
predicate stops the compile on any toolchain as well as printing a failure under
a runner.

| Check | What it pins |
|---|---|
| `after(n) == n` for every `n` from 0 to 255 | every occurrence inside the range is counted. A ceiling that bit early would be the id-list cap again by another name |
| `after(256) == 255`, and `!= afterWrapping(256)` | the occurrence that matters most. `CheckIn.cpp` reads 0 as nothing to report, so this is where the id list and the clear were lost |
| `after(n) == 255` for the 64 occurrences past the end | it holds, rather than holding once and then moving |
| `afterWrapping(257) == 1`, `(260) == 4`, `(511) == 255` | a wrapped figure is a plausible small number at every step, which is why nothing in a report says it is not the real one |
| `fromCeiling(1)`, `fromCeiling(50)` | counting on from a value already at the ceiling, which is the state the next occurrence arrives in |
| `kCeiling == UINT8_MAX` | the ceiling is the field's own limit. One chosen beside the type can be lower than what the field carries |
| `atCeiling(0)`, `(254)`, `(255)` | what lets the log line say "or more" |

### 21b. The call-site gate

```
bash ci/check-asset-counters.sh
```

Nothing in 21a reads `App/Assets.cpp`, and nothing can: that translation unit
names `SD`, `HTTPClient`, `NetworkClientSecure`, `mbedtls` and two ESP-IDF
headers, so a host build of it needs a stub set larger than the thing it would
test - the reason section 19d gives for `BrandSplash`. So a bare `++` on either
counter compiles, links, passes every host test and reports a wrapped figure.
This gate is what reads the increments. It checks the include, that neither
reported counter is written any other way, that every `countOne()` call in
the file names a counter the gate knows, that the count still runs after the
id-list branch closes, and that the refetch budget keeps its cap check.

**Amended 2026-11-03, after it passed the wrap it exists to prevent.** The write
check enumerated `++x`, `x++` and `x += n`, so
`gAllocationRefusals = gAllocationRefusals + 1;` placed beside the kept
`countOne()` call printed `PASS` and exited 0. It now reads four shapes: `++x`
or `x--` on either side, a compound assignment of any operator, and a plain
assignment of anything but `0`. The `= 0` exception is the declaration and the
clear `CheckIn.cpp` performs after a reported cycle; the comparison in `x == 0`
is kept out by reading one character past the `=`.

The flatten was brought in line with `ci/check-content-key-resolvers.sh` at the
same time: `tr -d ' \t\r\n'` rather than `tr -d ' \n'`. Nothing was passing
wrongly because of it on either runner reached so far, since Git Bash's `sed`
drops the carriage returns on the way through and CI checks the files out LF. A
tab or a carriage return left inside a joined statement stops every pattern below
it from matching, and the gate would then pass by finding nothing rather than by
checking anything.

### 21c. The runs

Read off each runner's own line, not an exit status. `ci/check-asset-counters.sh`
was written and run **before** the fix, against the shipped `Assets.cpp`: 7
`ERROR` lines and no `PASS` line. The host test came after the header, and the
header was then mutated back to the shipped shape to prove the test bites, which
is 21d.

| Run | Result |
|---|---|
| `ci/check-asset-counters.sh` on the shipped `Assets.cpp`, before the fix | 7 `ERROR` lines, no `PASS` line |
| `ci/test-reported-counter.sh` on `ReportedCounter.h` as it ships | `PASS: reported counter - every occurrence to 255 counted, and the 64 past it hold` |
| `ci/check-asset-counters.sh` after the fix | `PASS: asset counters - both reported counters go through countOne(), the count stays outside the id-list branch, the refetch budget keeps its cap check` |

### 21d. The mutations, and what each one did

Run on this workstation's `g++ 16.2.0`. Each mutation was gated on `cmp`
against a copy of the file before it was judged, because a pattern that never
applied reads as a passing test, and each file was restored and its mtime
refreshed afterwards.

Against `App/ReportedCounter.h`:

| Mutation | Result |
|---|---|
| `countOne()` back to a bare `++count` - the shipped defect | 7 `static_assert`s fail, and the compile stops before the runtime walk. The reductions printed are `(0 == 255)` at 256, `(1 == 255)` at 257, `(232 == 255)` at 1,000 |
| `kCeiling` lowered to 200 | 12 `static_assert`s fail, starting at `after(254) == 254`. A ceiling under the field withholds counts the report has room for, and `ci/check-asset-counters.sh` names it too |

Against `App/Assets.cpp`, each judged by the gate:

| Mutation | Result |
|---|---|
| `gDecodeFailureCount++` put back | 3 `ERROR` lines: incremented directly, never reaches `countOne()`, and the count no longer sits after the id-list branch |
| `gAllocationRefusals` guarded inline with `if (... < UINT8_MAX)` instead | 2 `ERROR` lines. A second copy of the ceiling is a second thing to keep right, so the gate refuses it even though it counts correctly today |
| the count moved inside the `kMaxReportedDecodeFailures` branch | 1 `ERROR` line. This is the defect no ceiling can see: the figure would stop at 4 and say nothing the id list does not already say |
| `#include "ReportedCounter.h"` dropped | 1 `ERROR` line |
| `gCheapRefetchCount >= kMaxCheapRefetches` dropped | 1 `ERROR` line. That comparison is both the fetch budget and the reason the third counter needs no ceiling |

Five more on 2026-11-03, against the amended gate. The first is the one that
passed before it:

| Mutation | Result |
|---|---|
| `gAllocationRefusals = gAllocationRefusals + 1;` beside the kept `countOne()` call | 1 `ERROR` line naming the assignment. Before the amendment this printed `PASS` and exited 0 |
| the same assignment in place of the `countOne()` call | 2 `ERROR` lines: the assignment, and the counter reaching `countOne()` nowhere |
| `--gAllocationRefusals;` beside it | 1 `ERROR` line naming the decrement |
| `gAllocationRefusals \|= 0x80;` beside it | 1 `ERROR` line naming the compound assignment |
| `if (gAllocationRefusals == 0) { return; }` beside it | `PASS`, correctly. A comparison is not a write, and the check reads one character past the `=` to tell them apart |

### 21e. Known gaps, recorded rather than discovered later

- **No device has been touched, flashed or commanded.** The defect was found by
  reading and the fix is checked by compilation and arithmetic.
- **Nothing has reported 255.** No device has been observed at or past the
  ceiling; the figures in 21a are the type's range, not a measurement. What a
  wrapped count looked like on the wire is read off `CheckIn.cpp`, not out of a
  stored request.
- **The gate cannot see what the device reports.** It knows which function each
  increment runs through. That `assetDecodeFailureCount` carries the value
  `decodeFailureCount()` returns is read off the source.
- **The gate is a text match, not a parser.** Its placement check reads that the
  count comes after the id-list branch closes, which a `countOne()` wrapped in
  some other condition further down would also satisfy. It catches the move
  into the branch, which is the mutation it was written for, and it would pass a
  count made conditional on something else.
- **The wire contract is unchanged and the server was not touched.** The field
  is the same optional 8-bit integer it was, omitted when the count is 0. What
  changes is which values it can hold: 255 now means "255 or more" where before
  the figure could be any wrapped value. No `CheckInResponse` or simulator
  change goes with it. A server that wanted to tell 255 from 4,000 apart would
  need an additive optional field of its own, and nothing asks for one.
- **One board compiled, and not on CI.** `arduino-cli compile` of `App` for
  `esp32:esp32:esp32:PartitionScheme=min_spiffs` finished with 0 errors: 1,581,551
  bytes of program storage (80%) and 115,460 bytes of globals. The S3 was not
  compiled. This workstation builds against the core overlay and CI builds
  stock, so that figure is this machine's, not CI's, and CI is still the gate.
- **That compile ran over a tree holding another lane's work in progress.**
  `App/PersonalPhotos.cpp` was being edited by a second lane at the time. It
  proves `Assets.cpp` and `ReportedCounter.h` compile; it is not a clean-tree
  measurement of anything else.

---

## 22. The content key a card reports, and the cards that report none (2026-11-03)

`App/PersonalPhotos.cpp` registered `fetch`, `itemCount`, `draw`, `order` and
`dwellSeconds` and no `listingId`. Its photographs arrive on the check-in
response rather than the card policy, so the `assetId` and `qrData` fallbacks
`CardManager::noteShowing()` applies after the resolver were both empty, the key
came out empty, `CheckIn.cpp` omitted the whole attributes object at its test for
a non-empty key, and the server stored an empty document.

Measured on production on 2026-11-03: 1,022 draws of the card across five
displays, 0 carrying a content key, 1 distinct attributes document. The same
table holds 263,243 of 263,299 `graphic` draws and 309,930 of 326,526 `listings`
draws with a key, through the same wire field and the same ingest, which is what
places the gap on the card rather than on the contract or the store.

What it costs a reader is in CARD_IMPRESSIONS_DESIGN.md: `card_impression_periods`
is unique on `(DeviceId, CardId, ContentKey, PeriodStartUtc)`, so every
photograph a household chose collapses into one keyless row per quarter hour. A
report can say the card was up and not which picture was up, and the draws are
pruned at ninety days.

`cardListingId()` now answers the item's asset id, and it is gated on the two
conditions `draw()` itself tests - the item is cached on SD, or it is the one
held in heap. A display with no SD card holds one picture at a time and the rest
of the set draws the no-content state, so an ungated id would name a photograph
that was not on the glass. `noteShowing()` runs immediately before the draw at
all five of its call sites, which is what makes the resolver and the draw read
the same state.

### 22a. The host test

```
bash ci/test-personal-photos-key.sh
```

Ten cases, built and run twice. `App/PersonalPhotos.cpp` compiles as it ships
against `tests/personal-photos-stubs/Arduino.h`; `Assets.h`, `Cards.h`,
`Display.h`, `Log.h` and `PersonalPhotos.h` are the shipped declarations and the
functions the module calls are defined in the test. The `Cards::registerCard`
stub keeps the descriptor, which is how the test reaches a resolver sitting in an
anonymous namespace - the same route `noteShowing()` takes.

The `Log::printf` stub formats into the same 256-byte scratch production uses and
keeps the lines, so the last three cases read back what reaches the remote stream
rather than only what the resolver answered.

**The second build is bounds-checked**, added 2026-11-03.
`cardListingId()` opens with `itemIndex >= gCount`, and the drawability condition
under it answers empty for every index past the set that the module's own state
can reach, so the case about an item past the set read the same with that guard
neutered to `itemIndex > 60000`: ten cases passed. What the guard holds is the
read of `gOnCard[item]` and `gIds[item]`, not the answer, and the case asks for
`kMaxPhotos` itself, which is one past both arrays. So the gate compiles a second
binary with `-fsanitize=bounds -fsanitize-undefined-trap-on-error` and requires a
`PASS` line out of that one too. The trap form needs no sanitizer runtime, which
is what makes it run on this workstation's MinGW as well as on CI, and a trap
stops the binary before its `PASS` line.

- the card registers a resolver at all
- a photograph on the card reports its own asset id, at three items
- a cardless display reports the one picture in heap and nothing for the other
  two, over three fetch passes, which also pins that the key follows the buffer
  rather than staying on item zero
- a fetch that failed reports no key for any item
- an item past the end of the set reports nothing, including at `kMaxPhotos`
- a set that changed reports nothing until the next fetch
- a photograph that failed to decode stops reporting its id, and its neighbour
  keeps reporting
- a draw that put no photograph up writes one line naming the photograph, which
  item of the set it was and which item holds the heap buffer instead, and the
  item that did draw writes no such line - added 2026-11-03, and it failed before
  the line existed
- a cardless display holding nothing says nothing is held rather than naming an
  item, added 2026-11-03. The line has two arms and both carry the word "heap",
  so the earlier case asserting on that word alone passed with the branch forced
  either way. Both cases now read the arm's own words and assert the other arm's
  are absent
- an item past the end of the set writes a line carrying the item and the set
  size, added the same day

The `Arduino.h` stub is a `String` class rather than the
`using String = std::string` the stubs beside it use, because
`invalidateDeparted()` appends an `unsigned` to a `String` and `std::string` has
no `operator+=` for one.

### 22b. The resolver-coverage gate

```
bash ci/check-content-key-resolvers.sh
```

Every file in `App/` calling `Cards::registerCard` is in one of three lists and
the gate fails on a file in none of them: cards that resolve their own key
(`Aircraft`, `Announcement`, `FeaturedListings`, `HomeValue`, `Listings`,
`PersonalPhotos`, `Sports`), cards whose content arrives on the policy so the
`assetId` and `qrData` fallbacks answer for them (`Graphic`, `QrText`), and cards
with nothing to name (`Calendar`, `ClockDate`, `Forecast`, `IssFlyover`,
`MoonPhase`, `SunMoon`, `Tides`). `Announcement` and `HomeValue` were in the last
list until the owner's 2026-11-03 call; 23b below is the move. A new card is a new
file calling `registerCard`, which lands in no list, so the decision gets made
rather than defaulted.

**The grading is per card descriptor, amended 2026-11-03.** It read each file as
one run of text and asked whether `spec.listingId=` appeared anywhere in it, which
left two ways through. A second card inside a file already listed as resolving
passed on its neighbour's resolver, which is how `sportslive` reported nothing on
907 draws out of `Sports.cpp`; and a resolver assigned through any other lvalue,
`Cards::CardSpec& s = spec; s.listingId = &tideListingId;` in a file listed as
keyless, satisfied the search for `spec.listingId=` and passed as well.

Every `Cards::CardSpec` a file declares is now graded on its own. Each has to be
handed to `Cards::registerCard()` by its own name, and the file's `listingId`
assignments are counted and matched against the ones landing on a descriptor the
gate can name, so a reference, a pointer or a dereference parts the two tallies
and fails. A file whose cards genuinely differ would need these lists split per
card id, and the failure message says so.

Both directions. A file listed as resolving whose descriptor assigns no
`listingId` fails, and a file listed as keyless that has gained one fails too, because then
the list is what is wrong.

It also pins what the host test cannot reach. `CardManager.cpp` names a graphics
library through `Display.h` and does not compile on a host, so
`noteShowing()`'s resolver call, its two fallbacks and the `beginShowing()`
hand-off are held as text, as is the attribute name `CheckIn.cpp` writes the key
under. Dropping the `assetId` fallback takes the key off every graphic and every
QR card in the fleet while both modules keep compiling and keep drawing.

The gate prints how many registering files it graded and how many card
descriptors it found in them, and fails below 16 files or 17 descriptors, which
were the counts on 2026-11-03. A text gate that stops matching passes by finding
nothing, and the counts are the only way to see that from the outside. Both
figures are printed because the file count alone stayed at 16 through the whole
time `sportslive` was reporting nothing.

### 22c. What was mutated, and what each mutation did

Nine, each gated on `cmp` against a byte-for-byte copy taken first, with the
file restored from that copy and `touch`ed afterwards. A mutation that never
applied reads exactly like a passing test.

| Mutation | Caught by |
|---|---|
| `App/PersonalPhotos.cpp` replaced with `git show HEAD:` of itself, the card as it shipped | the host test, case 1 |
| the drawability gate replaced with `if (false)`, so every item reports an id | the host test, the cardless case |
| the `spec.listingId` line deleted | the gate |
| the `spec.listingId` line commented out rather than deleted | the gate, which strips line comments before matching |
| a new `App/ZzNewCard.cpp` registering a card with no resolver | the gate, naming it as in none of the three lists |
| the whole file rewritten as CRLF | neither, correctly: the gate still found the resolver |
| CRLF and the commented-out line together | the gate, which is what says the CRLF pass above was not a pattern that stopped matching |
| the `assetId` fallback in `noteShowing()` replaced with an empty string | the gate's fallback pin |
| the wire attribute renamed from `contentKey` | the gate's wire pin |

Eight more on 2026-11-03, against the per-descriptor gate and the bounds-checked
build. Four of them are the ones that passed before the amendments, and each was
reproduced on the shipped files before anything was changed:

| Mutation | Result |
|---|---|
| `Cards::CardSpec& s = spec; s.listingId = &tideListingId;` added to `App/Tides.cpp`, listed keyless | 1 `ERROR` line: one `listingId` assignment, none of them landing on a descriptor the gate can name. Before the amendment this printed the file and descriptor counts and its pass line, and exited 0 |
| the same through a pointer, `p->listingId = ...`, and through `(*p).listingId = ...` | 1 `ERROR` line each, the same one |
| `live.listingId` dropped from `App/Sports.cpp`, the `sportslive` defect put back | 1 `ERROR` line naming the descriptor `live`. Before the amendment this printed `16 files` and passed |
| `live.listingId = nullptr;` instead | the same `ERROR` line. `= nullptr` is the one value that assigns no resolver |
| `cardListingId()`'s `itemIndex >= gCount` neutered to `itemIndex > 60000` | the bounds-checked build, which traps and prints no `PASS` line. The first build still passes all ten cases, which is the measurement that says the guard holds a read rather than an answer |
| the no-content line's `gRamItem == kNothingHeld` branch forced to each arm in turn | the host test, both times. Before the amendment both forced arms printed `PASS: personal photos content key (9 cases)` |

### 22d. What this does not cover

- **No wire change and no server change.** The key travels in the attributes
  object that already exists, under the name
  `CardImpressionRollup.ContentKeyNames` already reads, so an older server
  handles the new rows the way it already handles `graphic`. Nothing was added to
  `CheckInRequest`.
- **Keyless rows already written stay keyless.** The device is the only party
  that knew which photograph was up and it never sent it, so there is nothing to
  backfill from.
- **No label.** `ContentLabel` on a period row comes from a `summary`, `label`,
  `title` or `name` attribute and the firmware holds only the id. The names a
  household typed live in the server's own asset rows, so the label belongs in
  the rollup and needs no firmware release. `graphic` reports no label either.
- **Not seen on a device.** The resolver has not run on hardware. What is
  measured is the host test and the gate; the first device reading would be a
  `personalphotos` row arriving with a content key in it. The same goes for the
  no-content line: it is measured on a host and has not run on a display.
- **The line explains the count and does not correct it.** A draw that put no
  photograph up still records an impression carrying the full dwell, so `Shows`
  counts slots and not pictures, and nothing in this change alters that.
  `CARD_IMPRESSIONS_DESIGN.md` under "What an impression does not say" carries the
  measurement and describes the additive optional field that would report it
  upward. Nothing here builds one, and the wire is unchanged.
- **`HomeValue` and `Announcement` were listed as keyless and are not the same
  case as a clock.** Both reported a summary on every draw and no key, so their
  period rows carried a label and grouped as one. The owner's call on 2026-11-03
  is that all six owe a key; section 23 below is that change, and both files sit
  in the gate's `RESOLVES` list now.
- **Nothing was compiled with `arduino-cli`.** The host test and the gate are
  what ran. This workstation builds against the core overlay and CI builds
  stock, so CI is still the gate for either board.
- **The other lane's two gates were not run.** `ci/test-reported-counter.sh` and
  `ci/check-asset-counters.sh` were mid-edit in the same tree.

---

## 23. The key a home-value and an announcement draw reports (2026-11-03)

`App/HomeValue.cpp` and `App/Announcement.cpp` registered `describe` and no
`listingId`, so every draw of the home-value card and of the five announcement
slots reported a summary and no content key. Read off production on 2026-11-03,
inside a read-only transaction: 128,455 `homevalue` draws, every one carrying a
`summary` and none carrying a `contentKey`, and 24,543 draws across
`announcement` and `announcement`2-5 with the same shape. `summary` is the only
attribute name any of the six has ever sent. There is no `announcement1` id -
the first slot is plain `announcement`, which is also what `KnownCards.cs`
advertises.

`card_impression_periods` is unique on
`(DeviceId, CardId, ContentKey, PeriodStartUtc)`, so each address production
holds and each notice a tenant writes collapsed into one keyless row
per quarter hour. The owner's call is that all six owe a key.

**The home-value key is the subject address.** It arrives on the check-in
response as `homeValueAddress`, filled from `HomeValueResult.SubjectAddress`, and
no subject id crosses the wire, so the address is the only thing identifying the
property. The address and not the figure: `provider_homevalue_cache` holds five
rows, read on 2026-11-03 at 11:23 UTC, and 300 Eatons Landing Dr sits in two of
them under two `SubjectKey` spellings, `DR` and `DRIVE`, carrying the same
834,000. 104 Virginia Ave has one row. Keying on the estimate would start a new
row every time RentCast moved, so the figure keeps riding the summary.

**The announcement key is a 64-bit FNV-1a digest of the text**, sixteen lowercase
hex characters. The text is all a notice has: `CardPolicyEntry` carries `Id`,
`Kind`, `Order`, `DwellSeconds`, `Text`, the asset, QR and location fields and an
effectivity window, and none of them identifies a notice. Text runs to
`Cards::kMaxTextLength` (280) and a content key is bounded at 64 in both
`Impressions::kMaxContentKey` and `CardImpressionRollup.ContentOf`. Six distinct
announcement texts have drawn in production, 74 to 77 characters each, re-read
2026-11-03 at 11:30 UTC, so sending the text would have been truncated on the
ordinary draw. That part is measured. Two notices sharing a prefix grouping as
one row is what a prefix key allows rather than something these six do: they
diverge by character 14, and a 64-character prefix tells all six apart. A digest
does not parse as a GUID, so the rollup's asset-name lookup leaves the label
alone and the reported summary stands.

Both resolvers are gated on the conditions their own `draw()` puts content on the
glass under, which is `PersonalPhotos.cpp`'s rule: home value answers nothing
until it holds a real estimate and an address, and an announcement answers
nothing when its instance's text is empty.

No wire change. The key rides the attributes object that already exists, under
the name `CardImpressionRollup.ContentKeyNames` already reads.

### 23a. The host test

```
bash ci/test-announcement-and-home-value-keys.sh
```

Run by `build-and-release.yml` and `mut-host-test.yml` beside
`ci/test-personal-photos-key.sh`.

Eighteen cases, and the first sixteen were written before the resolvers. The first
run failed on case 1 with "the home-value card registers no listingId". Cases 11
and 15 were added after the adversarial review found what they close, and both
failed before the review's own mutations were reverted.
`App/HomeValue.cpp` and `App/Announcement.cpp` compile as they ship against
`tests/announcement-and-home-value-stubs/Arduino.h`; `Cards.h`, `Display.h`,
`Log.h`, `HomeValue.h` and `Announcement.h` are the shipped declarations. The
`Cards::registerCard` stub keeps every descriptor, which is how the test reaches
resolvers sitting in an anonymous namespace, and it also serves `Cards::indexOf`
and `Cards::at`, which is how `Announcement.cpp` reads the text on its own policy
entry.

Home value:

- the card registers a resolver at all
- a draw reports the address the estimate was computed for, at two addresses
- an estimate that moved reports the same key for the same house
- a draw with no estimate reports no key
- a valuation resolved from a position fix reports no key
- an address that changed reports the new one
- an item past the card's one reports nothing, at index 1 and at 97

Announcements:

- all five instances register a resolver
- a configured notice reports sixteen lowercase hex characters
- the digest matches the published FNV-1a 64 vector for a known notice
- an accented notice digests its bytes unsigned
- two different notices report different keys
- two notices sharing their first 64 characters report different keys
- the same notice on two instances reports the same key
- two instances holding different notices report different keys
- an unconfigured notice reports no key
- a notice that changed reports the new key with no fetch
- an item past the instance's one reports nothing

The two golden vectors were computed outside this tree rather than by the
algorithm under test. The published FNV-1a 64 vectors for `""`, `"a"` and `"abc"`
are `cbf29ce484222325`, `af63dc4c8601ec8c` and `e71fa2190541574b`, an independent
implementation reproduces all three, and it gives `abacb6c3112e90e4` for "Trash
goes out tonight". Without that case the digest could change shape in a later
edit while every other case here still passed, and every period row written
before the change would stop matching the ones after it.

The second vector exists because the first cannot see the `static_cast<uint8_t>`
in `textDigest()`. `char` is signed on this target, so without the cast every
byte above 0x7F sign-extends into the hash, and every ASCII case here passes
either way - which is every notice in production today and every notice an
English-speaking household writes. The accented case hashes the bytes
`50 69 c3 b1 61 74 61 20 61 20 6c 61 73 20 73 65 69 73`, written into the test as
explicit escapes so what is hashed does not depend on how the file was saved, and
expects `0db4397fe9544d9f`. Dropping the cast gives `705e2c0f2b64b19f`, which the
same independent implementation produces when it reads each byte as a signed
char. A notice in Spanish or French is where that edit would otherwise have
changed every key in the fleet with nothing failing.

The `Arduino.h` stub carries `operator+` overloads returning `String`, which is
what keeps `HomeValue.cpp`'s `hasRange() ? a + b : "--"` compiling: with only
`std::string`'s own operators in scope the two arms come out as `std::string` and
`String`, each convertible to the other. It also shims `gmtime_r`, and only
under `_WIN32`. That function is POSIX, so glibc declares it and the ESP32
core's newlib defines it; MinGW's `time.h` carries `gmtime_s` instead, which
takes its two arguments the other way round and answers an error code rather
than a pointer.

The shim was unguarded when the gate first ran on main, which compiled on this
workstation and failed on `ubuntu-latest` two ways at once: glibc declares
`gmtime_r` `noexcept`, so an unguarded definition clashes on the exception
specifier, and `gmtime_s` is a Microsoft interface glibc has no declaration for.
Run `41633721293` on commit `8cb5023` failed there and skipped both board builds
and the release. Reproduced two ways without a Linux host: this workstation's
g++ 16.2.0 with a force-included header shaped like glibc's `time.h`, which
raises both errors, and the ESP32 toolchain's own GCC 14.2.0, whose newlib
headers declare `gmtime_r` and raise the `gmtime_s` one. That second compiler
also defines no `_WIN32`, so it is what says the guard leaves the platform's own
`gmtime_r` in place.

Both halves of that read off glibc's own `time/time.h`, which declares
`extern struct tm *gmtime_r (const time_t *__restrict, struct tm *__restrict)
__THROW;` and carries no `gmtime_s` anywhere in the file. `__THROW` expands to
`noexcept (true)` under C++11 and later, per `misc/sys/cdefs.h`.

### 23b. The resolver-coverage gate

```
bash ci/check-content-key-resolvers.sh
```

`Announcement.cpp` and `HomeValue.cpp` moved from `KEYLESS` to `RESOLVES`. Before
the code change the gate printed "16 files in App/ register a card" and its pass
line. With the resolvers in and the lists untouched it named both files as listed
keyless and now assigning `spec.listingId`, which is the reverse direction it
checks. After the lists moved it prints the same count and the same pass line.

The `RESOLVES` remark now says why these two belong there without walking a feed:
an announcement's words arrive on the card policy and a valuation's address on
the check-in response, and neither lands in `assetId` or `qrData`, which are the
only two fields `noteShowing()` falls back to.

### 23c. What was mutated, and what each mutation did

Eight here, plus fourteen more from the adversarial review, each gated on `cmp`
against a byte-for-byte copy taken first, with the file restored from that copy,
`cmp`ed again and `touch`ed afterwards.

| Mutation | Caught by |
|---|---|
| `spec.listingId = cardListingId;` deleted from `HomeValue.cpp` | the gate, naming it as listed resolving and assigning none. The host test did not reach case 1: the orphaned resolver trips `-Werror=unused-function` first |
| `spec.listingId = &listingId;` commented out in `Announcement.cpp` | the gate, which strips line comments before matching, and the host test's case 8 |
| the home-value estimate and address gate reduced to the item check | the host test, the no-estimate case |
| the FNV prime changed from `0x100000001b3` to `0x100000001b5` | the host test, the golden vector |
| the digest stopped hashing after 64 characters | the host test, the shared-prefix case, which is the case that case exists for |
| the announcement empty-text gate removed | the host test, the unconfigured-notice case |
| the `static_cast<uint8_t>` dropped from the digest | the host test, the accented case, which is the case that case exists for |
| the resolver reading slot one's text instead of its own instance's | the host test, the two-instances-differ case |

A further attempt did not apply: a perl pattern anchored on `'\0'` inside the
digest loop matched nothing, and `cmp` reported the file unchanged rather than the
run reporting a pass. It was retried against a different anchor, which is the
mutation in row five.

Both files are CRLF in this checkout, so the commented-out row above is also what
says the gate's comment stripping works on a carriage-return file rather than
passing by finding nothing.

**Three mutations nothing caught, all of them about the gate rather than these
two cards.** They are the gate's own shape and they were there before this
change:

- A second card registered inside a file already listed in `RESOLVES`, with no
  resolver of its own. The gate's unit is the file: one `spec.listingId=`
  anywhere in the text marks the whole file as resolving, so a keyless card
  hidden in any of the seven `RESOLVES` files reports nothing and nothing fails.
  That is the `personalphotos` failure mode the gate exists to stop.
- A new `App/` file registering through a namespace alias, `namespace Registry =
  Cards;` then `Registry::registerCard(spec)`. The `grep -l 'Cards::registerCard'`
  never sees it, the count still reads 16 rather than 17, and the file lands in no
  list without the "decide what it reports" error firing. Matching
  `registerCard(` instead of the qualified name would close it.
- Dropping only the address half of the home-value gate. Behavior-equivalent
  rather than a test weakness: `setValue()` assigns `gAddress` on any change
  including an empty one, so returning `gAddress` with no address returns the same
  empty String the guard returns. The clause is defensive and starts to matter if
  the resolver ever returns something derived from the address.

### 23d. What this does not cover

- **No wire change and no server change.** The key travels in the attributes
  object that already exists, under the name the rollup already reads. Nothing
  was added to `CheckInRequest`, and the 6-month backward-compatibility mandate
  is untouched.
- **Keyless rows already written stay keyless.** The device never sent the key,
  so there is nothing to backfill from.
- **No new label.** Both cards already send a `summary` on every draw and that is
  what fills `ContentLabel`. The announcement digest does not parse as a GUID, so
  the rollup's asset lookup leaves it alone.
- **Not seen on a device.** Neither resolver has run on hardware. What is
  measured is the host test, the gate and the production read. The first device
  reading would be a `homevalue` or `announcement` row arriving with a
  `contentKey` in it, which needs a release carrying this and a display running
  it.
- **The announcement key is not resolvable back to its words on the server.**
  `BOOT_BUTTON_AND_IMPRESSIONS_DESIGN.md` used to describe a short hash the server
  could resolve to the exact string it had at the time; nothing stores that
  mapping, and that paragraph now says so. The words are recoverable from the
  summary on the same draw, and for a notice over 200 characters the summary is a
  prefix, so a recompute from it would not reproduce the key. Nothing here builds
  a registry.
- **The address key carries the 64-character bound.** The device copies
  `homeValueAddress` with no cap, `HomeValueCacheEntry.SubjectAddress` allows 300,
  and `Impressions::beginShowing` and the server's `ContentOf` each cut at 64, so
  both sides store the same cut-off string and nothing overflows. What two
  properties agreeing for 64 characters lose is their separate rows. The longest
  address in `provider_homevalue_cache` on 2026-11-03 is 55 characters and carries
  no estimate, so it never draws a key, and the longest with one is 42. No test
  covers an address over 64 characters, because the clamp is in
  `Impressions::beginShowing` rather than in the resolver and the host test does
  not reach it.
- **A quarter in which the address or the notice changes writes two period rows.**
  The unique index takes the key, so that is correct rather than a collision, and
  the `impressions` subject's join to `device_periods` then repeats
  `CardsInRotation`, `ActiveMs` and `TotalShows` across both. That fan-out already
  exists for every keyed card.
- **The simulator cannot drive the home-value key.** Its impressions textarea
  splits attribute pairs on commas and every production address carries two or
  three, so `homevalue contentKey=300 Eatons Landing Dr, Annapolis, MD 21401` is
  parsed as `contentKey=300 Eatons Landing Dr` and the rest dropped. The
  announcement digest goes through it end to end. That is a server-tree gap and
  nothing here changes it.
- **The `impressions` report subject's help for that column is now partly wrong.**
  It reads "The listing's own identifier, for matching against a press", and
  neither new key matches a press. `ReportQuerySeeder` leaves an existing subject
  alone, so the text production serves would need editing rather than reseeding.
  Server tree, not touched here.
- **The draw-count caveat is unchanged.** A home-value draw with no estimate and
  an announcement draw with no text still record an impression carrying the full
  dwell, so `Shows` counts slots and not content. The key narrows that for these
  six the way it does for `personalphotos` and does not close it.
- **Only the two gates and the two host tests above were run.** The rest of
  `ci/` was left alone.
- **It compiles for the ESP32 and the S3 was not built.** `arduino-cli compile
  --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs --libraries ../libraries`
  over `App/` reports 1,582,791 bytes of program storage (80%) and 115,460 bytes
  of globals. `compile-app.cmd` passes no `--libraries` and cannot build this tree
  at all - `App/ConfigRequest.cpp` includes `<ConfigGesture.h>` from
  `libraries/DamBoards`, which `ci/build-firmware.sh` puts on the path and that
  wrapper does not. This workstation builds against the core overlay and CI builds
  stock, so CI is still the gate for either board.

---

## 24. The key the live sports card reports, and three gates that passed what they exist to catch (2026-11-03)

`App/Sports.cpp` registers four cards. Three go through `registerOne()`, which
assigns `spec.listingId`, `spec.describe` and `spec.frameCountFor` from the
`listingIdFor()`, `describeFor()` and `frameCountFor()` switches; the fourth,
`sportslive`, is built by hand so it can carry its own item count and its notable
dwell, and that descriptor assigned none of the three. So `sportsteam` and
`sportsscores` reported a key on every draw and the live card out of the same
file reported nothing, over 907 draws and 161 period rows counted on 2026-11-03.

The owner's call that day is that it owes one. `listingId3`, `describe3` and
`frameCount3` already existed and were already the `default` arm of each switch,
so the change is three assignments. `CARD_IMPRESSIONS_DESIGN.md`, "Which cards
report a key", and `BOOT_BUTTON_AND_IMPRESSIONS_DESIGN.md`, "The content key, per
card family", are the record and were updated first.

What makes the key mean something was on the glass is the card's own item count.
`countLive()` returns zero unless `anyLive(3)` finds a game in progress, so the
card leaves the rotation rather than drawing a card that says nothing is on, and
`listingIdCardAt()` returns nothing for an index past the stored games or for a
game with neither side named. That is `PersonalPhotos.cpp`'s rule reached through
the item count rather than a per-item drawability test.

### 24a. The three gates, and what each one passed

Each hole was reproduced on the shipped files before anything was changed. Every
mutation was gated on `cmp` against a byte-for-byte copy taken first, restored
from that copy, confirmed with `cmp` again and `touch`ed, and applied by
byte-literal `index`/`substr` replacement read from files rather than by `sed` or
`perl` substitution: Git Bash's `sed` drops the carriage returns on a CRLF file,
so a pattern built from one does not match its real bytes.

| Gate | What it passed | What it reads now |
|---|---|---|
| `ci/check-content-key-resolvers.sh` | a resolver assigned through any other lvalue, and a keyless card beside a keyed one in the same file | every `Cards::CardSpec` a file declares, graded on its own, registered by its own name, with the file's `listingId` assignments counted and matched against the ones landing on a named descriptor |
| `ci/check-asset-counters.sh` | `gAllocationRefusals = gAllocationRefusals + 1;` beside the kept `countOne()` call | `++x` and `x--` either side, a compound assignment of any operator, and a plain assignment of anything but `0` |
| `ci/test-personal-photos-key.sh` | `cardListingId()`'s bounds guard neutered, and the no-content line's branch forced either way | a second bounds-checked build, and both arms of the line asserted on their own words with the other arm's absent |

Sections 21b, 22a, 22b and 22c carry the per-gate detail and the mutation
results.

### 24b. The runs, read off each runner's own line

| Run | Before | After |
|---|---|---|
| `ci/check-content-key-resolvers.sh` | `16 files in App/ register a card`, pass line, exit 0, with `sportslive` keyless | `16 files in App/ register a card, between them 17 card descriptors`, pass line |
| `ci/check-asset-counters.sh` | `PASS` with the plain assignment in | `PASS` on the shipped file, and 1 `ERROR` line with that assignment back |
| `ci/test-personal-photos-key.sh` | `PASS: personal photos content key (9 cases)` with the bounds guard neutered | `PASS: personal photos content key (10 cases)` from both builds |
| the other twelve gates in `ci/` | not run | all pass, including the other lane's `ci/test-announcement-and-home-value-keys.sh` at 18 cases |

### 24c. What this does not cover

- **Nothing on the wire changed.** The key rides the existing attributes object
  under the existing name `contentKey`, so the six-month backward-compatibility
  mandate is untouched and an older server handles a keyed `sportslive` row the
  way it already handles `sportsteam`.
- **Not seen on a device.** No device was flashed or commanded. The first
  reading would be a `sportslive` period row arriving with a key in it.
- **The bounds guard's stated consequence was not reproducible.** A stale
  `gOnCard[item]` past a shrunk set would read `gIds[item]`, but no sequence of
  this module's own calls reaches that state: `fetch()` clears every slot from
  `gCount` up on the same pass it fills the ones below, `setFromCheckIn()` clears
  all ten, and `gRamItem` is only ever set to an index under `gCount`. So the
  guard is a bounds check on the arrays rather than a condition the answer
  depends on, and the bounds-checked build is what holds it. No behavioral case
  can.
- **`sportslive`'s own key has no host test.** `Sports.cpp` names LovyanGFX
  through `Display.h` and does not compile on a host, so what holds it is the
  per-descriptor gate plus the reading that `listingIdCardAt(3, i)` is the same
  function the three keyed cards reach. The three-sibling path is the measured
  one.
- **The draw-count caveat is unchanged.** A `sportslive` draw still records an
  impression carrying the full dwell, so `Shows` counts slots and not games.
- **The key cannot reach the 64-character bound, so nothing is cut.**
  `listingIdCardAt()` writes `YYYY-MM-DD:away@home` and `kMaxTeamNameLength` is
  20, so the longest key this card can produce is 52 characters against
  `Impressions::kMaxContentKey` of 64. Read off `App/Sports.h` and
  `App/Impressions.h`.
- **`live.frameCountFor` changes nothing on its own.** `frameCount3` returns 1
  for a card that is not the board, and `CardManager::noteShowing()`'s frame loop
  runs from index 1, so one frame and a `nullptr` hook behave identically. It is
  assigned so the hand-built descriptor carries what `registerOne()` fills in,
  rather than for an effect.
- **It compiles for the ESP32 and the S3 was not built.** `arduino-cli compile
  --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs --libraries ../libraries`
  over `App/` reports 1,582,931 bytes of program storage (80%) and 115,460 bytes
  of globals, with no error or warning line, against `ota_0` at 2,097,152. This
  workstation builds against the core overlay and CI builds stock, so CI is still
  the gate for either board.

## 25. The pull-request lane, and the gates it did not run (2026-11-03)

A host test shipped to main with a `gmtime_r` shim defined over `gmtime_s`, which
is Microsoft's. It passed here and failed on `ubuntu-latest` twice over: glibc has
no `gmtime_s` to call, and it already declares `gmtime_r` `noexcept`, so the
definition clashed on the exception specifier. `-Werror` made both of those
errors. CI went red on main at `8cb5023`, every later step was skipped and no
release published. `c7be107` put the shim behind `#ifdef _WIN32` and
`v2026.11.03.0004` published at 13:27 UTC.

What let it reach main is that `bench-cal.yml`, the pull-request lane, ran four of
the gates the release lane runs and reached none of them by any route. Read off
`HEAD` at `c7be107`: neither `bench-cal.yml` nor `ci/build-firmware.sh` names
`ci/test-reported-counter.sh`, `ci/test-personal-photos-key.sh`,
`ci/test-announcement-and-home-value-keys.sh` or
`ci/check-content-key-resolvers.sh`. `mut-host-test.yml` runs the last three, and
its own header says it never runs on main and triggers on `mut/**` and `test/**`
alone. So for work arriving by pull request the first execution of those steps was
on main, where failing one costs the release.

### 25a. What each lane runs now

Measured by globbing each workflow's `run:` lines and `ci/build-firmware.sh`'s
own calls, rather than read. "wrapper" means the gate arrives through
`ci/build-firmware.sh`, which all three lanes invoke; on `mut-host-test.yml` that
is the `full-gate` job, which is opt-in by `[full-gate]` in the commit message, so
an ordinary mutation push does not run it.

| Gate | bench-cal | release | mut-host |
|---|---|---|---|
| `check-asset-counters.sh` | wrapper | step + wrapper | step + wrapper |
| `check-board-columns-call.sh` | wrapper | wrapper | step + wrapper |
| `check-content-key-resolvers.sh` | step + wrapper | step + wrapper | step + wrapper |
| `check-host-test-portability.sh` | step + wrapper | step + wrapper | wrapper |
| `check-ram-budget-call.sh` | step | step | step |
| `check-score-guard.sh` | wrapper | wrapper | step + wrapper |
| `check-text-state.sh` | wrapper | wrapper | step + wrapper |
| `test-announcement-and-home-value-keys.sh` | step | step | step |
| `test-brand-splash.sh` | step | step | - |
| `test-card-columns.sh` | step | step | step |
| `test-config-request.sh` | step | step | step |
| `test-key-policy.sh` | step | step | step |
| `test-loader-order.sh` | step | step | step |
| `test-personal-photos-key.sh` | step | step | step |
| `test-ram-picture-budget.sh` | step | step | step |
| `test-recovery-menu.sh` | step | step | step |
| `test-reported-counter.sh` | step | step | step |

Four rows changed: the three content-key gates and `test-reported-counter.sh` read
`-` under bench-cal before this. `check-host-test-portability.sh` is new, below.
`test-brand-splash.sh` still runs on neither mutation job, which is older than
this and is left alone.

The four new steps carry `if: ${{ hashFiles('<script>') != '' }}`, the way
`mut-host-test.yml` guards its own, so a branch cut before one of these scripts
existed runs the rest rather than failing on a file it never had. The release lane
keeps them unguarded, because there a missing gate script is itself the defect.

### 25b. The paths filter

`bench-cal.yml`'s `paths:` decides whether the lane runs at all, and a gate in a
workflow that does not trigger reads nothing. The filter named 27 patterns and
reached 11 of the 36 files the four gates read.

Each gate's real input set was taken from the compiler rather than from the
scripts' own comments: `g++ -MM` over each host test's translation units, and
`grep -l 'Cards::registerCard' App/*.cpp` for the resolver gate, which grades
every file that registers a card and also pins `App/CardManager.cpp` and
`App/CheckIn.cpp`. Then a script translated each filter pattern to an anchored
regex under GitHub's own rules - `*` stops at `/`, `**` does not - and matched it
against those 36 paths. It reports 36 of 36 after the change and reported 11
before. The same script over a control list puts `README.md` and
`.github/workflows/build-and-release.yml` outside the filter and `Recovery.h`,
`RecoveryMenu.cpp` and `libraries/DamBoards/deep/nested/Board.h` inside it, which
is what says the matcher is matching rather than agreeing.

Eleven patterns were added. `App/*.cpp` is one of them, in place of the sixteen
card files it matches today, because a new card is a new file and a list of
sixteen here is the one place nobody updates; it also covers `CardManager.cpp` and
`CheckIn.cpp`. Seven headers the host tests compile against are named
individually, the way the filter already names `App/Display.h`.

The filter also missed `App/partitions.csv`, `App/partitions-panellan-8mb.csv` and
the root `partitions-panellan-8mb.csv`. The bare `partitions.csv` already there is
CAL's own at the repository root and matches nothing under `App/` - `*` does not
cross a `/`. `ci/build-firmware.sh` reads the table out of the build directory and
compares the compiled size against it, so a resized App partition changed what
that check allows without the bench running. All three are in the filter now. That
is a second hole in the same filter rather than part of the four gates, and it is
called out here so it is not read as one of them.

### 25c. The stub portability gate

`ci/check-host-test-portability.sh` is new. It reads every `.h`, `.cpp` and `.hpp`
under `tests/` and fails on `gmtime_s`, `localtime_s`, `ctime_s`, `asctime_s`,
`strcpy_s`, `sprintf_s` or `_snprintf` outside an `#ifdef _WIN32` block. Those
seven are in Microsoft's C runtime and not in glibc, and the first two shadow a
POSIX function that takes its arguments the other way round.

It strips line and block comments before searching, because these stubs name the
Windows function they stand in for and a scan of the raw text reports every such
remark. It tracks the preprocessor conditionals as a stack, so the `#else` of an
`#ifdef _WIN32` and the body of an `#ifndef _WIN32` both count as outside. It
strips `\r` per line, because a carriage return at the end of a directive stops
the patterns matching and the gate would then pass by recognizing nothing. It
prints how many files and stub directories it read, with a floor of 20 and 5 as of
this date, for the same reason.

It runs as a step on `bench-cal.yml` and from `ci/build-firmware.sh`, which puts
it on all three lanes and on any local wrapper run. The wrapper is where it earns
its keep: on `ubuntu-latest` the host test fails on its own, so the machine that
needs telling is the one with the Microsoft interface to reach for.

### 25d. The runs, read off each runner's own line

`g++` 16.2.0 at `C:\Tools\mingw64\bin`, from the repository root, the way each
workflow invokes it.

| Run | Output |
|---|---|
| `ci/test-personal-photos-key.sh` | `PASS: personal photos content key (10 cases)`, twice, the second build bounds-checked |
| `ci/test-announcement-and-home-value-keys.sh` | `PASS: announcement and home value content keys (18 cases)` |
| `ci/check-content-key-resolvers.sh` | `16 files in App/ register a card, between them 17 card descriptors`, then its pass line |
| `ci/test-reported-counter.sh` | `PASS: reported counter - every occurrence to 255 counted, and the 64 past it hold` |
| `ci/check-host-test-portability.sh` | `20 source files under tests/, across 5 stub directories`, then `PASS: host test stubs use no Windows-only interface outside an #ifdef _WIN32 block` |

The portability gate was made to fail before it was made to pass. A throwaway
stub directory under `tests/` carried seven cases at once, and the gate reported
four and only four: an unguarded `gmtime_s`, which is the shipped defect's own
shape; a `ctime_s` inside `#ifndef _WIN32`; a `strcpy_s` in the `#else` of an
`#ifdef _WIN32`; and a `_snprintf`. It stayed quiet on a line comment naming
`gmtime_s` and `strcpy_s`, on a block comment spanning two lines naming
`localtime_s` and `sprintf_s`, on the real guarded shim, and on a plain C99
`snprintf`. Repeated with the whole probe rewritten CRLF: the same four, at the
same line numbers. The probe was deleted and `git status` confirmed the tracked
tree unchanged, so nothing tracked was mutated to run this.

### 25e. What this does not cover

- **No workflow has run.** GitHub Actions cannot be driven from here and nothing
  was pushed. That each script runs and passes from the repository root is
  measured; that `bench-cal.yml` now catches the next one of these is read off the
  file. The first evidence either way is a pull request's own step output.
- **The YAML was not parsed by a YAML parser.** There is none on this
  workstation - no Python, Node, Ruby, `yq` or cached `YamlDotNet`, and
  `bench-cal.yml`'s own image check calls `python3`, which exists on the runner
  and not here. What was checked is structural: no tabs, every indent even, every
  `${{ }}` closing on its line, no key repeated inside one mapping, the `paths:`
  sequence resolving to 38 entries, and the `steps:` sequence resolving to 17
  steps with the five new ones between `check-ram-budget-call.sh` and the compile,
  each carrying an `if` and a `run`. A real parse is still owed.
- **The two host tests were not added to `ci/build-firmware.sh`.** Only the two
  text gates were. That script has never called a host test: it is the build
  procedure, it is meant to run wherever `arduino-cli` runs, and every gate in it
  is a `grep`, `sed` or `awk` over the tree. A host test there would make the
  build need a host `g++` as well. The host tests reach both CI lanes as steps
  instead.
- **`mut-host-test.yml` was not touched.** It already ran three of the four, and
  the portability gate reaches its `full-gate` job through the wrapper. Its
  `host-test` job still does not read `tests/` for this.
- **The gate holds seven names and not a category.** A Windows-only interface
  nobody here has met yet passes it, and so does a difference in behaviour rather
  than in availability. The seven are the ones that have cost something or sit one
  keystroke from a POSIX function.
- **GCC 13.3 against 16.2.0 is not covered by any of this.** A warning only one
  of the two compilers emits is the same class of problem as the shim and this
  gate does not see it. `ubuntu-latest` is still the only reading of what the
  release lane's compiler says.

---

## 26. The release body that publishes twice (2026-11-03)

RELEASE_NOTES.md's `## Unreleased` section is what `build-and-release.yml`
composes a release body from, and nothing empties it, so an entry left there
goes out again under the next tag. `v2026.10.18.0001` and `v2026.10.22.0001`
published prose differing only in their checksum lines. `v2026.11.03.0004` did
it again: its two entries were still under Unreleased after it shipped, and the
file carried no `## v2026.11.03.0004` heading at all. The extraction the
workflow runs over the shipped file reproduced 0004's published body byte for
byte, both 1,286 bytes, same MD5.

Both entries now sit under that heading, with a third for commit `1da34cc`,
which shipped inside 0004 and published no entry of its own.

### 26a. The gate

```
gh release view --json body --jq .body > prev-release-body.md
PREV_BODY_FILE=prev-release-body.md bash ci/check-release-notes-rolled.sh
```

It compares `### ` heading lines. The previous release's body is the record of
what published, so a heading out of it still sitting under Unreleased means that
entry was never moved under its dated heading. The generated `### Installing
this` and `### Checksums` are skipped, an empty Unreleased section passes, and a
previous body carrying no entries passes.

Five cases, run on this workstation with the real 0004 body off
`api.github.com` and the real file at three states.

| Case | Read |
|---|---|
| `PREV_BODY_FILE` unset | exit 1, naming the variable and how to fill it |
| 0004's body against the rolled file | `PASS: release notes rolled, 2 shipped heading(s) checked against Unreleased` |
| an empty previous body | passes, saying the previous release published no entries |
| 0004's body against the file as it stood before the roll | exit 1, naming both headings and `2 entry/entries would publish a second time` |
| the same, plus one new entry above them | exit 1, naming the same two and leaving the new one alone |

The fourth case is the shipped defect exactly. The fifth is the shape the next
one would take, since somebody adding an entry to a section nobody emptied gets
a body that is not a byte-for-byte repeat, and a check comparing whole bodies
would pass it.

### 26b. Known gaps, recorded rather than discovered later

- **No workflow calls it.** The step is written out in the lane's report and
  nothing in `.github/` runs it, so the gate holds nothing until that lands. It
  belongs in `build-and-release.yml` ahead of the compile, and reads the same on
  a pull-request lane, where it would fail before main moves rather than after.
- **It reads headings, not prose.** An entry re-worded into a new heading and
  left under Unreleased publishes twice and passes this. A heading repeated on
  purpose across two releases fails it.
- **`gh` hangs on this workstation**, so every run here fed the body in from a
  file fetched with `curl`. The `gh release view` line in the usage comment has
  not been run anywhere.
- **Not run on the release runner.** The script is `awk`, `sed`, `grep` and
  `tr`; what CI's bash and coreutils make of it is unmeasured, the way
  GCC 13.3 against 16.2.0 is unmeasured for section 19a.

## 27. Two host gates that read none of their binary's output (2026-11-03)

`ci/test-loader-order.sh` and `ci/test-recovery-menu.sh` each ran their compiled
binary as a bare command and gated on exit status. Both binaries print a `PASS:`
line that nothing looked at, `tests/loader-order-test.cpp:117` and
`tests/recovery-menu-test.cpp:460`, so a `main()` returning 0 having printed
nothing passed both. Section 19's `ci/test-brand-splash.sh` had the same gap and
closed it an hour earlier; these two carry the same guard now, spelled the way
`ci/test-config-request.sh` spells it.

Both gates are steps in `build-and-release.yml` and `bench-cal.yml`, so until now
neither lane could tell a passing test from a test that stopped running.

### 27a. What each gate reads now

| Gate | Pattern it requires | What it says when the pattern is absent |
|---|---|---|
| `ci/test-loader-order.sh` | `*"PASS: loader NVS write order"*` | `ERROR: the loader order test exited 0 without printing its PASS line.` |
| `ci/test-recovery-menu.sh` | `*"PASS: recovery gesture"*`, per board | `ERROR: the recovery menu test exited 0 at $BOARD without printing its PASS line.` |

The recovery guard sits inside the `for BOARD` loop, so each target's run is read
on its own and the message names the board. A build that prints the line at
`esp32` and not at `esp32s3` is caught at the second iteration rather than
averaged away.

### 27b. The mutations, and what each one did

The `PASS:` line was commented out in each test source so `main()` returned 0
having printed only what the script's own `echo` put there. Every mutation was
gated on `cmp` against a byte-for-byte copy taken first, restored from that copy,
confirmed identical with `cmp` again and `touch`ed, and applied by byte-literal
replacement through `[System.IO.File]::ReadAllBytes` / `WriteAllBytes` with
`GetEncoding(28591)` round-tripping rather than by `sed` or `perl`: Git Bash's
`sed` drops the carriage returns on a CRLF file, so a pattern built from one does
not match its real bytes.

| Run | Output |
|---|---|
| `ci/test-loader-order.sh`, before the guard, `PASS` line dropped | `==> Loader NVS write order` and nothing after it, exit 0 |
| `ci/test-loader-order.sh`, after the guard, same mutation | the `ERROR` line above |
| `ci/test-loader-order.sh`, after the guard, source restored | `PASS: loader NVS write order, all three paths and a missing factory partition` |
| `ci/test-recovery-menu.sh`, before the guard, `PASS` line dropped | `Controller RAM: 20 bytes (host ABI)` at both boards and no `PASS` line, exit 0 |
| `ci/test-recovery-menu.sh`, after the guard, same mutation | the `ERROR` line above, naming `esp32`, stopping at the first board |
| `ci/test-recovery-menu.sh`, after the guard, source restored | `PASS: recovery gesture, ...` then `Controller RAM: 20 bytes (host ABI)`, at both boards |

The recovery run is the clearer of the two. The mutated binary still printed a
line, so a gate asking whether anything came back would have passed it. What the
guard requires is that specific line.

Byte counts across the pass. `tests/loader-order-test.cpp` read 4,315 bytes and
118 CRs before, 4,318 and 118 mutated, 4,315 and 118 restored.
`tests/recovery-menu-test.cpp` read 20,039 and 462 before, 20,042 and 462
mutated, 20,039 and 462 restored. The two gate scripts went from 644 bytes and 15
CRs to 1,142 and 27, and from 585 and 14 to 1,222 and 28. Both are stored LF in
the index and checked out CRLF here, so the release runner sees LF.

### 27c. The rest of `ci/`, read for the same shape

Nineteen scripts in `ci/`. Nine build and run a host binary; seven of those
already read its output and the two above were the rest, so nothing else in that
family is open. Four findings of the other shape, where a check can pass by
matching nothing rather than by checking something, were left alone:

- **`ci/build-firmware.sh`'s `RestartCause` comparison.** Lines 238-243 `diff`
  two `sed`/`grep` extractions of the enum and nothing asserts either came back
  non-empty. `diff` of two empty streams returns 0. The pattern is pinned to
  exactly two leading spaces, so re-indenting either header, wrapping either in a
  namespace or renaming the enum turns the gate off while it stays green. Twelve
  values extract from each header today, so it has real work to do. This is the
  one worth closing next, because what it holds is two binaries on one device
  agreeing on a number nothing else can notice.
- **`ci/build-firmware.sh`'s `Motion` entry-point list.** Lines 164-182 loop over
  a literal heredoc of six names rather than over `App/Motion.h`, which declares
  twelve. A seventh entry point lands in no list. The zero-caller arm below it is
  unreachable under `set -euo pipefail`, so an unwired entry point exits non-zero
  with no message rather than with its own.
- **`ci/check-release-notes-rolled.sh`'s Unreleased anchor.** Line 52 extracts the
  section with a pattern anchored `^## Unreleased$` and never tests the result for
  emptiness. Reword that heading and the comparison matches nothing for every
  heading, while the pass line still reports a count taken from the previous
  release's body.
- **`ci/check-text-state.sh`'s scanned-unit count.** Both rules are absence checks
  and neither prints how many function units it read. The column-zero opener means
  a namespace wrap scans nothing. It does assert positively that
  `restoreDefaultTextState()` exists and sets scale 1, which is a real anchor, so
  this is the weakest of the four.

`.github/workflows/mut-host-test.yml` guards every step with `hashFiles`, so a
step whose script is missing is skipped rather than failed. That is deliberate and
written down at its lines 21-24, and that workflow never runs on main.

### 27d. What this does not cover

- **Not run on the release runner.** Everything here ran under Git Bash with
  GCC 16.2.0 from `C:\Tools\mingw64\bin`. CI compiles GCC 13.3 on ubuntu-24.04.
  The change is shell rather than C++, so what moves is `bash`'s `case` and
  `$(...)`, and that is unmeasured there.
- **The four findings in 27c are reported, not fixed.** None was reproduced with a
  mutation; they are readings of the scripts.
- **Nothing was committed, pushed or deployed.** The two commits already held
  unpushed in this tree, `01eb8a1` and `831e991`, were left alone.
- **No device was flashed or commanded.**

## 28. Six ways past the stub portability gate, and one false alarm (2026-11-03)

Section 25c records `ci/check-host-test-portability.sh` as written earlier today.
An adversarial read of it found six shapes it reported nothing on and one it
reported wrongly. All seven were reproduced against the script as committed at
`874f033` before anything changed.

Each probe was an untracked file under `tests/`, written byte by byte with
`WriteAllBytes` and `GetEncoding(28591)`, copied in with `cp`, gated on `cmp`
against the copy, and deleted after. No tracked file was mutated to run any of
this, and `git status` named only this lane's own edits at both ends. Every probe
was CRLF, CR count equal to LF count, and `App/partitions.csv` was unmodified
throughout.

The baseline on the clean tree read `20 source files under tests/, across 5 stub
directories` and then the pass line, exit 0.

### 28a. The seven, reproduced before anything changed

| Probe shape | Before | After |
|---|---|---|
| `gmtime_s` and `strcpy_s` in `tests/probe-portability.cc` | pass, exit 0, and the count still printed 20 | both reported at lines 8 and 9, exit 1 |
| `static const char* kBlockOpen = "/*";` then a bare `gmtime_s` below it | pass, exit 0 | reported at line 7, exit 1 |
| `#define WINFN(a, b) a##b` then `WINFN(gmtime, _s)(&t, &w);` | pass, exit 0 | pass, exit 0, open by decision |
| `gmtime_s` under `#if defined(_WIN32) \|\| defined(__GLIBC__)` | pass, exit 0 | reported at line 7, exit 1 |
| `gmtime_s` under `#ifdef _WIN32_WINNT_VISTA` | pass, exit 0 | reported at line 7, exit 1 |
| `#if 1 /* keep */ && (gmtime_s(&t, &w) == 0)` | pass, exit 0 | reported at line 5, exit 1 |
| `static const char* kNote = "this stub stands in for gmtime_s on Windows";` | reported at line 2, exit 1 | pass, exit 0 |

### 28b. What changed, and what was left alone

The compiler on ubuntu-24.04 is what decides this. It reads the macros, expands
them and evaluates the conditionals, and its answer already arrives in the host
test step. A text scan chasing token pasting, string concatenation, typedefs and
function pointers is a preprocessor written in awk, and it would be wrong in ways
nobody has listed yet. So five of the six closed on one missing piece each, the
sixth is written into the script's header rather than chased, and the header now
states what the script reads and what it leaves to the compiler. Its job is to
name the reason in one line from `ci/build-firmware.sh` before a push, ahead of
the step that decides, rather than to be a second opinion in CI.

Comments, string literals and character literals come out of each line before
the search. That closes the false alarm and the block-comment shape together: a
name in a log string or a test name no longer fails the lane, and a `/*` inside a
literal no longer opens a comment that runs to the end of the file.

A quote with no partner on the same line stays in the output as itself rather
than taking the rest of the line with it. A digit separator inside a numeric
literal is one apostrophe, and swallowing from it would have hidden the code
after it. That gap was opened by the literal handling, found by probe
`keep-h.h`, and closed before the literal handling was kept.

Ten extensions are read rather than three: `.h`, `.hpp`, `.hh`, `.hxx`, `.c`,
`.cc`, `.cxx`, `.cpp`, `.inc` and `.ipp`.

One enumeration of `tests/` is split between those extensions and a list of
extensions that hold no code, so every file is either read or named, and a third
spelling fails rather than going unread. The printed line reads `20 of 20 files
under tests/ read, across 5 stub directories`, and the first figure is the scan's
own count rather than what `find` handed it.

A level is exempt only when its whole condition is one `_WIN32` test:
`#ifdef _WIN32`, `#if defined(_WIN32)`, `#if _WIN32`, and the negated spelling of
each. The old test was a substring, so `_WIN32_WINNT_VISTA` exempted its body and
so did `defined(_WIN32) || defined(__GLIBC__)`. A condition mixing the test with
another term is now read rather than exempted, which reports rather than skips,
and nesting is the way to exempt such a body.

A name on an `#if` or `#elif` line is read, except as the operand of `defined`,
which asks whether a name exists rather than calling it.

### 28c. The shapes that had to keep working, re-run after

Nine more probes, same method. `#if defined(_WIN32)` without `#ifdef` stays
quiet. A name nested under `#ifndef _WIN32` is reported. The `#else` of an
`#ifdef _WIN32` is reported. Comments naming the functions stay quiet, and so
does a plain C99 `snprintf`. A `#define` body holding `strcpy_s` is reported.
`#if defined(gmtime_s)` stays quiet. A compound `_WIN32` condition is reported,
which is the rule above rather than a defect. A bare `gmtime_s` is reported,
which is the control. Reading `20 of 20` and the pass line on the clean tree
before and after the whole sweep.

Three more probes cover the counting. A file with an extension in neither list
prints `20 of 21` and fails, naming the file. A `.md` file prints `20 of 21`,
names itself as holding no code, and passes. A zero-byte header prints `20 of 21`
and fails, because 21 files reached the scan and 20 produced a line.

### 28d. Step order on the two lanes

The portability step sat below the three host tests on `bench-cal.yml`, with a
comment saying it ran before the compile so a Microsoft-only stub would not fail
those tests with a wall of g++ output. A failing step ends the job, so the host
test failed first and that diagnostic never arrived. It is now the first step
after the two `uses:` lines on that lane.

The release lane reached the gate only through `ci/build-firmware.sh`, which is
its compile step, well below its own host tests, so the same ordering held there.
It is now an unguarded step straight after checkout, which is how that lane runs
every other gate, and the table in 25a reads `step + wrapper` under release for
that row.

`mut-host-test.yml` still reaches it through the wrapper in the `full-gate` job
alone, which is opt-in by `[full-gate]` in the commit message. Left as it was.

### 28e. What this does not cover

- The token paste is open by decision. `#define WINFN(a, b) a##b` then
  `WINFN(gmtime, _s)(&t, &w);` passes, and the script's header names token
  pasting, a typedef, an alias and a function pointer among what it leaves to
  the compiler.
- No workflow ran. GitHub Actions can't be driven from here, and nothing was
  committed or pushed. The three commits held unpushed in this tree, `01eb8a1`,
  `831e991` and `874f033`, were left alone.
- The script ran under Git Bash with GNU Awk 5.4.1. The awk program sticks to
  POSIX features, with no `gensub`, no `ARGIND` and no three-argument `match`,
  but which implementation `awk` resolves to on ubuntu-24.04 is unmeasured here.
- A raw string literal, `R"(...)"`, is not understood. Its contents read as code,
  which over-reports rather than under-reports, and nothing under `tests/` uses
  one.
- The other gate scripts under `ci/` were not re-read for the same two shapes.
- No host test was compiled or run. What changed is one shell script and two
  workflow files.
- No device was flashed or commanded.

## 29. Five ways past the content-key resolver gate (2026-11-03)

`ci/check-content-key-resolvers.sh` grades per card descriptor as of section 24,
which closed a keyless `sportslive` hiding in a file the gate called resolving.
The grading only ever reached descriptors it could name, though, and the search
for registering files read `App/*.cpp`. Five ways of registering a card went
ungraded, and all five printed the gate's own baseline figures and its pass line.

That is the same shape as the `sportslive` defect. The printed descriptor count
stayed at 17 and the floor was `-lt 17`, exactly as the file count stayed at 16
before. A floor that can't move when the thing it counts is hidden isn't a floor.

### 29a. The five, reproduced on the shipped files

Each was applied to `App/Aircraft.cpp`, which is CRLF and listed under `RESOLVES`,
or written as a new file. Every mutation was gated on `cmp` against a byte-for-byte
copy taken first, restored from that copy, confirmed with `cmp` again and
`touch`ed. They were applied by byte-literal `ReadAllBytes`/`WriteAllBytes`
through `[System.Text.Encoding]::GetEncoding(28591)` with CRLF in the anchor, not
by `sed`: an LF anchor against a CRLF file matches nothing and writes nothing,
and the run after it reads as a passing gate.

The control for all five is a keyless `Cards::CardSpec extra;` registered as
`Cards::registerCard(extra)`, which the gate already caught: `between them 18
card descriptors` and `ERROR: App/Aircraft.cpp: is listed as resolving its own
content key and its descriptor 'extra' assigns no listingId.`

| What hid the descriptor | Why it hid | What the gate printed |
|---|---|---|
| the type `Cards::CardSpec` and the name `extra` on separate lines | `grep -oE` in `specs_in()` works per record | `16 files ... between them 17 card descriptors`, pass line |
| `auto extra = Cards::CardSpec{};` | the regex wants `[[:space:]]+` after the type and `{` sits where that whitespace has to be | `16 files ... between them 17 card descriptors`, pass line |
| `using Spec = Cards::CardSpec;` then `Spec extra;` | the name of the type at the declaration isn't `Cards::CardSpec` | `16 files ... between them 17 card descriptors`, pass line |
| `App/ProbeCard.h` calling `Cards::registerCard(spec)` | `grep -l 'Cards::registerCard' App/*.cpp` reads no headers | `16 files ... between them 17 card descriptors`, pass line |
| `App/extra/ProbeCard.cpp` | the glob is one directory level deep | `16 files ... between them 17 card descriptors`, pass line |

In the first three the hidden descriptor assigns no `listingId`, so
`ASSIGNED_ANYWHERE` and `ASSIGNED_TO_SPECS` stay equal and that tally never parts
either. In the last two nothing about the file reached the gate at all.

### 29b. What the gate reads now

The registration calls are the census and the declarations are only how a
registered card gets graded. Those two can disagree in one direction only: a
descriptor spelled in a way the patterns don't recognise still has to be handed to
`Cards::registerCard()` by name to become a card. So `Cards::registerCard(` is
counted straight, every argument it names has to be a descriptor the gate
recognised, and a spelling nobody thought of raises the registration tally
without raising the descriptor tally. The two part company and the file fails
naming the argument it can't place.

Four declaration spellings are recognised: `Cards::CardSpec name;` including
across a line break, `auto name = Cards::CardSpec{}`, and either alias form
followed by `Alias name;`. Whitespace is squeezed to one space before any of that
runs, so a wrapped statement reads like one that fitted while the space the type
needs after it still keeps `Cards::CardSpec&` and `Cards::CardSpec*` out.

`registerCard(` is also accounted for in full across every file swept. Each
occurrence is either the declaration in `Cards.h`, the definition in
`CardManager.cpp`, or a qualified call, and anything left over fails. That is what
catches a call reaching the registry under another name.

The file sweep is recursive and reads headers as well as translation units.
`App/build` is pruned: it's gitignored output holding generated copies of the same
files, and reading it would grade a stale duplicate of every card.

Every figure printed is one the run measured, and all four floors come from the
measurement:

| Figure | On 2026-11-03 | What a fall means |
|---|---|---|
| source files swept under `App/` | 97 | files were removed, or the `find` stopped matching |
| files registering a card | 16 | cards were removed, or the search for `Cards::registerCard` stopped matching |
| card descriptors graded | 17 | cards were removed, or the declaration patterns stopped matching |
| `Cards::registerCard()` calls | 17 | cards were removed, or the census the grading runs against stopped matching |

The descriptor and registration counts also have to be equal, which is the figure
a hidden descriptor can't leave alone. The `listingId` assignment total prints
without a floor: it moves whenever a card legitimately changes lists, and a
resolver going missing is already a named failure from the per-descriptor
grading.

### 29c. The runs, read off the gate's own lines

| Run | Output |
|---|---|
| shipped files, before | `16 files in App/ register a card, between them 17 card descriptors`, pass line |
| shipped files, after | `97 source files under App/, 16 of them register a card` / `17 card descriptors graded, 17 Cards::registerCard() calls, 8 listingId assignments`, pass line |
| control, after | `18 card descriptors graded, 18 Cards::registerCard() calls`, 1 `ERROR` naming `'extra'` with no `listingId` |
| type and name on separate lines | `18 ... 18`, the same `ERROR` naming `'extra'` |
| `auto extra = Cards::CardSpec{};` | `18 ... 18`, the same `ERROR` naming `'extra'` |
| `using Spec = Cards::CardSpec;` then `Spec extra;` | `18 ... 18`, the same `ERROR` naming `'extra'` |
| `App/ProbeCard.h` | `98 source files ... 17 of them register a card` / `18 ... 18`, `ERROR: App/ProbeCard.h: registers a card and is in none of this gate's three lists.` |
| `App/extra/ProbeCard.cpp` | `98 source files ... 17 of them register a card` / `18 ... 18`, `ERROR: App/extra/ProbeCard.cpp: registers a card and is in none of this gate's three lists.` |

Every descriptor the new gate names matches what `specs_in()` named before it, one
file at a time across all sixteen, which is how the rewrite was checked against
the behaviour it replaces. The only descriptor it sees that the old one didn't is
`gCards` in `CardManager.cpp`, the registry array itself, in a file that registers
nothing and is therefore graded and counted nowhere.

### 29d. Three more spellings, tried against the new gate

None of these were on the list. They're what says the imbalance is a backstop
rather than decoration.

| Spelling | What the gate said |
|---|---|
| `Cards::CardSpec first, extra;` with both registered | `18 card descriptors graded, 19 Cards::registerCard() calls`, then `hands 'extra' to Cards::registerCard() and this gate cannot see where 'extra' was declared as a Cards::CardSpec` |
| `using Cards::registerCard;` then `registerCard(extra);` | `18 ... 17`, then `reaches registerCard() 1 time(s) other than as Cards::registerCard(), as Cards.h's declaration, or as CardManager.cpp's definition` |
| `Cards::registerCard(makeExtraSpec());` | `17 ... 18`, then `registers a Cards::CardSpec built in the call itself` |

A declarator list names only the first descriptor, by decision. The second name
then reaches the registration tally and the file fails on the imbalance, rather
than on a parse of C++ declarators a shell script is in no position to write.

### 29e. What it costs

Wall clock on this workstation is unreadable right now with other lanes
compiling, so this is CPU time, `user` plus `sys`, two runs apiece from the same
shell.

| | CPU time |
|---|---|
| before | 9.65s and 10.42s |
| after | 2.00s and 2.07s |

It got cheaper rather than dearer. The old version spawned `sed`, `grep`, `awk`
and `sort` per file and then per descriptor; this one makes a single `awk` pass
over all 97 files and does the list work in the shell with no further processes.
The gate runs as a step on `build-and-release.yml` and `bench-cal.yml`, in the
`full-gate` job of `mut-host-test.yml`, and from `ci/build-firmware.sh`, so the
saving lands on every lane.

### 29f. What this does not cover

- A text gate is still what this is, and its patterns have now been widened twice
  after one of them stopped matching. What would hold it outright is the registry:
  `registerCard()` is the one function every card reaches, it has every descriptor
  in its hand, and a decision passed to it as an argument would be checked by the
  compiler at every call site whatever the spelling and whatever file it sat in.
  `CardManager.cpp` names a graphics library and doesn't compile on a host, so
  nothing in CI can walk that registry today, and that change would alter
  `registerCard()`'s signature and all sixteen call sites. It needs the owner's
  call and a design-document change first, and it wasn't made here.
- A card that never calls `Cards::registerCard()` under any spelling of the name
  is still invisible. The accounting above catches a `using` declaration and a
  function pointer by count, but a registry reached some other way would leave
  every figure where it is.
- Comment stripping is a scan, not a parse. A `//` inside a string literal
  truncates the rest of that line, which can only drop text and so can only
  over-report a missing registration, and no raw string literal under `App/`
  carries one of these patterns today. Every block comment under `App/` was read
  for `Cards::CardSpec`, `Cards::registerCard` and `listingId` before the stripping
  was added, and none carries any of the three, so no count moved because of it.
- The script ran under Git Bash with GNU Awk 5.4.1. The awk program was also run
  under `gawk --posix` and `gawk --traditional`, emitting the same 150 records
  each time, so it reaches for no gawk extension. Which implementation `awk`
  resolves to on ubuntu-24.04 is still unmeasured here.
- The `97`, `16`, `17` and `17` floors were read on this date from this tree. Any
  file added under `App/` raises the first and leaves the gate passing; removing
  one trips it, which is the intent.
- No firmware was compiled. What changed is one shell script. No workflow ran,
  nothing was committed or pushed, and the three commits held unpushed in this
  tree, `01eb8a1`, `831e991` and `874f033`, were left alone.
- `App/partitions.csv` was unmodified at both ends, confirmed with `git status`.
- No device was flashed or commanded.

## 30. Two gates in the firmware build that passed on nothing (2026-11-03)

`ci/build-firmware.sh` holds two checks that reported nothing while the thing
they check was in front of them. Both were reproduced against the script as
committed at `f206a2c` before anything changed, by extracting the block's own
lines out of the live file and running those lines alone — the script swaps
`partitions-panellan-8mb.csv` over `partitions.csv` at line 551 and restores at
612, so an interrupted end-to-end run leaves the wrong partition table in the
tree.

Every mutation was a tracked file rewritten with `WriteAllBytes` and
`GetEncoding(28591)`, gated on `cmp` against a byte-for-byte copy taken first,
restored from that copy, confirmed byte-identical with `cmp`, and the timestamp
pushed forward. CR count equalled LF count on every file at both ends:
`BootDiag.h` 147, `App/BootDiag.h` 375, `App/Motion.h` 141, `App/Motion.cpp`
531, `App/Telemetry.cpp` 601. `partitions.csv` and `App/partitions.csv` were
unmodified at both ends, confirmed with `git status`.

### 30a. The RestartCause comparison, reproduced

The extraction was pinned to exactly two leading spaces, and `diff` of two empty
streams exits 0. Re-indenting **one** header by one space fails the gate, because
the other header still matches. Re-indenting **both**, which is what a formatter
pass over the repository does, took both readings to zero values and the gate
printed its heading and passed, with twelve values sitting in each file.

| Mutation | Before | After |
|---|---|---|
| Both enum bodies re-indented by one space | heading, exit 0, nothing compared | `12 numbered values read from BootDiag.h, 12 from App/BootDiag.h`, exit 0 |
| `enum class RestartCause` renamed in both headers | heading, exit 0 | both readings 0, named as nothing to compare, exit 1 |
| `CannotDraw = 11` loses its number in both headers | heading, exit 0 | 11 against a floor of 12, and 12 members against 11 numbered, exit 1 |
| One value removed from one header | exit 1 | exit 1, list diff printed |

### 30b. The Motion entry-point loop, reproduced

The loop read a heredoc of six names. `App/Motion.h` declared twelve, so six were
outside it: `hasEverDetectedMotion`, `state`, `fault`, `eventsSinceReport`,
`lastDetectedAgeSeconds` and `operatingState`. A thirteenth declaration,
`uint32_t stuckHighHoldSeconds();`, added to the header with no caller anywhere
under `App/`, went unreported and the gate exited 0.

### 30b-i. The entry point that reads as dead from inside this repository

Widening the loop made it name `Motion::hasEverDetectedMotion()`, declared in
`App/Motion.h` and defined in `App/Motion.cpp` with no caller anywhere under
`App/`. It was read as the same shape as the `noteTouchAndShouldSwallow()` failure
this gate exists for, and **it was removed from both files, which was wrong.**

`CARD_ATTENTION_DESIGN.md` names it. That document is in the server tree at the
root of `C:\Users\Administrator\Documents\Claude-working folder`, not in this
repository, so a grep run from here cannot see it. Its section 2 excludes a
display that has never confirmed motion from every denominator rather than
counting it as a zero, and carries that fact into the day row as
`SensorConfirmed`, naming this accessor as the only thing in the firmware that can
support a claim of `Present`. Section 3 lists it again among the five `Motion`
entry points that design reads, with a note that it adds no second reader of
GPIO35. `SensorConfirmed` was not built as of 2026-10-29 because the day row was
not built, so the accessor has no caller yet by schedule rather than by neglect.
Re-deriving `Present` from the pin is what both documents forbid, so removing the
accessor would have left that design unbuildable as written.

Both hunks were restored with `git checkout --`, confirmed against `HEAD` with
`git diff`, confirmed byte-identical with `cmp` against the copy taken before any
edit, and timestamped forward. `App/Motion.h` is back at 6,840 bytes and 141 CRs,
`App/Motion.cpp` at 24,473 bytes and 531 CRs, CR equal to LF in both.

The reading to carry forward is that "no caller in this repository" and "unused"
are different statements, and what sits between them is a second repository. The
gate's error text now says so, and names the exemption list as the third answer
beside wiring it and removing it.

### 30b-ii. The exemption list, and the readings that keep it from rotting

The gate carries one exemption, as a name and its reason on one line, in
`ci/build-firmware.sh` beside the loop, because that is the file somebody opens
when they want the name off the list. The reason prints on every run, so a passing
build log carries it:

```
    12 entry points read out of App/Motion.h
    11 of 12 called from outside the module, 1 exempt and still uncalled
    exempt: hasEverDetectedMotion: CARD_ATTENTION_DESIGN.md sections 2 and 3, server tree: SensorConfirmed on the day row, not built
```

| Mutation | Before | After |
|---|---|---|
| `uint32_t stuckHighHoldSeconds();` added, no caller | exit 0, unreported | named, `11 of 13 called from outside the module, 1 exempt`, exit 1 |
| Three declarations taken out of the header | exit 0 | `8 entry points` against a floor of 12, exit 1 |
| `using namespace Motion;` added to `App/Telemetry.cpp` | exit 0 | the using declaration named with its line, exit 1 |
| Exempt declaration removed from the header | exit 0 | the floor fires first at `11 entry points`, exit 1 |
| Exempt declaration renamed to `hasConfirmedMotion()`, count still 12 | exit 0 | `the exemption list names Motion::hasEverDetectedMotion(), which App/Motion.h no longer declares`, reason printed, exit 1 |
| A `Motion::hasEverDetectedMotion()` call added to `App/Telemetry.cpp` | exit 0 | `on the exemption list and now has a caller, so the exemption is spent`, exit 1 |
| The exemption line dropped while the name is still uncalled | n/a | the ordinary arm names it, exit 1 |

The last one ran against a copy of the block's own lines with the single exemption
line filtered out, so no file in the repository was mutated for it. The exemption
shelters exactly the name on it: a thirteenth uncalled entry point is still
reported with the list in place.

The `CALLERS -eq 0` arm was unreachable. `grep`'s exit 1 on no match propagated
through the pipeline under `pipefail` and killed the script at the assignment, so
the function was never named and the build log carried no reason. Probed directly
with a name nothing declares: no output and exit 1, where the arm should have
printed. `|| true` on the pipeline is what makes the arm reachable, and the
thirteenth-entry-point mutation above is what shows it printing.

### 30c. Why both stayed text gates

The two `RestartCause` enums carry the same fully qualified name,
`BootDiag::RestartCause`, so no translation unit can see both and a
`static_assert` has nothing to stand in. Generating both from one list would hand
the comparison to the compiler and would also undo the separation
`CAL_OTA_DESIGN.md` section 13 asks for, along with the per-value documentation
each file keeps in its own words. A list of `static_assert`s would be a third
hand-written census of the same names, which is what went wrong here. For the
Motion loop the compiler answers nothing at all: an uncalled non-static function
in a linked module is legal C++ and warns about nothing, which the gate's own
header already says.

So both read text, and what they read is counted and printed. The Motion list is
now derived from `App/Motion.h` rather than written in the script, so the figure
moves on its own, and the floor is what says the reading worked.

### 30d. What this does not cover

- **No firmware was compiled.** `App/Motion.h` and `App/Motion.cpp` are back at
  their committed contents, so nothing in the App changed and only the shell
  script and this file did. Neither Motion file is reachable from a host test,
  because `App/Motion.cpp` needs the core, so no compile was run either way.
- **The script was not run end to end.** Each gate was run as its own lines, for
  the partition-table reason above. Nothing else in `build-firmware.sh` was
  exercised.
- **Everything ran under Git Bash with GCC 16.2.0 from `C:\Tools\mingw64\bin`.**
  CI compiles GCC 13.3 on ubuntu-24.04. What changed is shell, so what moves
  there is `grep -E`, `sed -E` and the heredoc, and that is unmeasured.
- **Both floors are 12, read on this date from this tree.** Adding an entry point
  or a cause raises the reading and leaves the gate passing; removing one trips
  it, which is the intent.
- **The exemption list is one name deep and is not a mechanism for finding more.**
  Nothing here sweeps the server tree for other entry points a design document
  claims. The next one will read as dead the same way this one did, and the error
  text asking the reader to check is all that stands between it and a removal.
- **`MOTION_AWARE_DISPLAY_DESIGN.md` was reported missing earlier today and it is
  not missing.** It is at the root of the server tree, 46,100 bytes, thirteen
  sections, section 9 being the one the gate cites. The first reading was a grep
  run from inside this repository, which is the same blind spot that nearly took
  `hasEverDetectedMotion()` out. It does not name that accessor;
  `CARD_ATTENTION_DESIGN.md` does. Six files here cite it, `App/CheckIn.cpp`,
  `App/Motion.cpp`, `App/Motion.h`, `CAL_OTA_DESIGN.md`, `ci/build-firmware.sh`
  and this file, and the gate's error text now says where it lives.
- **Nothing was committed or pushed.** No device was flashed or commanded.

## 31. A sweep of every gate under ci/, and four more that passed on nothing (2026-11-03)

Twenty `.sh` files under `ci/` read one at a time, against one question: when the
pattern a gate greps for stops matching, does it say so or does it print its pass
line? Sections 24, 27, 28, 29 and 30 cover the gates found that way earlier
today. This is the rest of them.

Four more were holed. Each was reproduced with a mutation, fixed, and the same
mutation re-run against the fix. Every mutation was a tracked file rewritten with
`WriteAllBytes` and `GetEncoding(28591)`, gated on `cmp` against a byte-for-byte
copy taken first, restored from that copy, confirmed byte-identical with `cmp`,
and the timestamp pushed forward. `App/partitions.csv` and `partitions.csv` were
md5 `d1c88bba83a060df5add8cca68a975b3` at both ends and both clean under
`git status`. `ci/build-firmware.sh` was never run end to end, for the reason
section 30 gives.

CR counts equalled LF counts on every CRLF file touched, at both ends:
`App/Display.cpp` 7465, `App/CardColumns.h` 962, `ci/check-text-state.sh` 203,
`ci/check-board-columns-call.sh` 587, `ci/check-score-guard.sh` 341,
`ci/check-release-notes-rolled.sh` 108. `RELEASE_NOTES.md` is LF and carried 0 CR
at both ends.

### 31a. The text-state gate read past whatever carried a qualifier

`ci/check-text-state.sh` asserts `restoreDefaultTextState()` exists and sets a
scale of 1, and that assertion is a real anchor. Its other two rules report
absence over "function units", and the opener that finds a unit refused any line
beginning `static`, `inline`, `constexpr`, `const`, `extern` or `template`. For a
definition qualified `static` that meant no unit opened, so every line of its body
was read with nothing open and both rules passed it whatever it did with the
scale. Nothing was printed about how many units either rule read, and there was no
floor, so a namespace indenting the file past the first column read nothing and
reported nothing.

Reproduced on `App/Display.cpp`. `showFailure()`'s in-function
`lcd.setTextSize(1);` became a second `setTextSize(2)` and its
`restoreDefaultTextState();` went, which is a function leaving a scale of 2 on the
panel. Rule 1 named it at line 2385, so the rule works. Adding `static` to the
same signature and changing nothing else produced `==> App/Display.cpp returns the
default text scale from every draw function` and exit 0.

The namespace shape was reproduced separately: lines 3000 to 4300 indented two
spaces, which is what a wrap leaves. The gate as committed at `f206a2c` passed
that file. Its opener saw 93 units where it had seen 109, and said neither figure.

Fixed. The opener steps over a leading `static` or `inline` rather than refusing
the line, which is 129 units against the old 109 on this tree. Both rules count
what they open, rule 1 prints the figure, the two counts have to agree, and a
reading below 120 fails. Under the indent mutation the fixed gate reports 113 and
fails. Under the `static` mutation it names line 2385 again.

### 31b. The board-columns gate could not tell a statement from a comment

`ci/check-board-columns-call.sh` is twenty-odd fixed-string pins over eight
function bodies in `App/Display.cpp`. Its matcher took spaces and newlines out and
left the comments in, which `ci/check-score-guard.sh` and the resolver gate beside
it both strip and for the reason their own remarks give. So a pinned statement
deleted and left behind on one line as a comment satisfied the pin.

Reproduced. `widestBoardStatus()`'s fold became

```
    // if (measured > widest) { widest = measured; }
    widest = measured;
```

which sizes the board's status column off the last row rather than the widest, and
is the defect that check's error text describes. The gate printed
`==> showSportsBoard() lays one grid for the board, measured in the face it draws
in` and exited 0.

Fixed. The matcher is now the same `flatten()` the two gates beside it use, which
takes line comments out before the spacing, and tab and carriage return with it.
The three raw pipes that bypassed the matcher go through it too, the ordering awk
skips comment lines, and the `copySlice()` space-substitution check strips
comments while keeping its spacing, because the space is the whole substitution.
It also prints how many of its eight pinned functions it located and how many
lines it read. Under the same mutation the fixed gate names the fold and exits 1.

### 31c. The release-notes gate counted the previous release's headings

`ci/check-release-notes-rolled.sh` extracts the `## Unreleased` section with an
awk keyed on that exact heading and never tests what came back. Reword or re-level
the heading and the extraction yields nothing, every `grep -Fxq` matches nothing,
and the pass line prints a count read off the previous release body rather than
off the thing that stopped matching.

Reproduced with a previous-release body holding
`### Your reports can say which home and which notice were on screen - App` and
that same heading left under Unreleased. The gate named it and exited 1, so the
comparison works. Changing `## Unreleased` to `## Unreleased changes` and leaving
the stale entry where it was produced `PASS: release notes rolled, 1 shipped
heading(s) checked against Unreleased` and exit 0. The 1 is the previous body's
heading count. The entry that would publish twice was still there, and the body
the workflow would compose was empty.

Fixed. The heading has to appear exactly once as `^## Unreleased$` before the
extraction runs, and the gate prints how many lines and how many entry headings it
read out of that section. The pass line now carries both counts. Under the
reworded heading it fails naming the spelling; under the stale entry it still
names the entry, now with `4 line(s) under Unreleased, 1 of them an entry heading`
above it.

### 31d. The bounded-string gate in the firmware build, reported not fixed

`ci/build-firmware.sh` lines 383 to 396 check that every bounded string in
`App/Display.cpp` goes through `layoutText()`, by grepping `lcd.drawString(` and
subtracting the literal draws and the `LAYOUT-PRIMITIVE` ones. It is absence only.
It prints no figure, and there is no floor on a census it could take: five
`lcd.drawString(` call sites in that file today, all five sanctioned.

Reproduced by adding to `showFailure()`

```
  auto& panel = lcd;
  panel.drawString(whatToDo, 10, 200);
```

which is a server-supplied string drawn raw into no box, the failure the gate's
own error text describes as how "Houston Astros" reached a wall reading "Houston
Astr". Run as its own lines, the gate found nothing. The `lcd.drawString(` census
stayed at five and the gate says no such number.

Not fixed. `ci/build-firmware.sh` is another lane's file today. What it needs is
the census printed and floored the way the Motion gate beside it now does, and the
pattern widened from `lcd.` to any receiver, so a second name for the panel raises
the count rather than leaving it.

### 31e. What the other sixteen read

Judged sound, each requiring a positive match and reporting what it read:

- `ci/check-partition-fit.sh`, `ci/check-asset-counters.sh`,
  `ci/check-ram-budget-call.sh`, `ci/check-host-test-portability.sh` and
  `ci/check-score-guard.sh`. Each one floors its counts or compares two tallies
  against each other, so a renamed symbol fails rather than passing.
- The ten `ci/test-*.sh` runners and `ci/test-key-policy.sh`. All eleven read
  their binary's own PASS or `Passed!` line rather than its exit status, build
  with `-Werror`, and `ci/test-key-policy.sh` reads the object file's presence in
  both directions plus the compiler's own diagnostic, so a compile that failed for
  an unrelated reason does not read as a refusal.

One thing fixed in `ci/check-score-guard.sh` while reading it: its `count()` ran
under `pipefail` with no `|| true`, so a fixed string that had gone to zero ended
the script at exit 1 before the comparison could name which one. That is a failure
rather than a pass, but it arrives with no message. It now takes grep's 1 as the
answer and reports `declared 0 times, not once`. The predicate mutation
`score >= 0` to `score > 0` was run against the fixed gate and it names the
predicate.

### 31f. What this does not cover

- **No firmware was compiled and no device was involved.** Every gate was run on
  its own. `ci/build-firmware.sh` was not run end to end.
- **Two host runners are red for a reason outside this sweep.**
  `ci/test-announcement-and-home-value-keys.sh` and
  `ci/test-personal-photos-key.sh` fail to link against
  `Cards::registerCard(Cards::CardSpec const&, Cards::KeyPolicy)`, which is the
  registry change in flight in another lane. Their stub directories have not
  caught up. Not reproduced against a clean tree and not this sweep's to fix.
- **`ci/check-content-key-resolvers.sh` was rewritten under this sweep.** The
  version read here was the census-anchored one; it is now five pinned statements
  with a floor of 5, and the per-card decision moved to the compiler through
  `ci/test-key-policy.sh`. The reading above is of the file as it stood, not as it
  stands.
- **Everything ran under Git Bash with GCC 16.2.0 from `C:\Tools\mingw64\bin`.**
  Both the `gawk` 5.4.1 and the `sed` on this workstation strip a carriage return
  from a line they read, confirmed by byte count. CI runs GNU tools on
  ubuntu-24.04 against an LF checkout, so what the `tr -d '\r'` in these gates is
  for is unmeasured here either way.
- **The floors are readings from this tree on this date.** 120 against 129 units
  for the text-state gate, and 8 pinned functions for the board-columns gate.
  Adding a draw function raises the reading and leaves the gate passing; a wrap or
  a rename drops it and trips the floor, which is the intent.
- **Nothing was committed or pushed.** No device was flashed or commanded.

## 32. The compiler holds the content-key decision (2026-11-03)

Section 29f named the change that would hold outright what a text scan over C++
kept nearly holding, and said it was not made there. This is it.
`Cards::registerCard()` takes a second argument naming what a card's draws
report, with no default, so a registration that names nothing does not compile.

### 32a. The API

In `App/Cards.h`:

| What | Shape |
|---|---|
| the decision | `enum class KeyPolicy : uint8_t { Resolves, PolicyContent, Keyless }` |
| the rule | `constexpr bool keyPolicyHolds(const CardSpec&, KeyPolicy)` |
| the name in a log line | `constexpr const char* keyPolicyName(KeyPolicy)` |
| the registry | `bool registerCard(const CardSpec& spec, KeyPolicy keyPolicy)` |

The three values are the three groupings the retired `RESOLVES`,
`POLICY_CONTENT` and `KEYLESS` lists held. `keyPolicyHolds()` is one expression:
`Resolves` wants `listingId` assigned and the other two want it null.

Nothing stores the policy on the descriptor. `noteShowing()` reads
`CardSpec::listingId` itself, and a copy of the decision on the card would be a
second thing to keep in step with the first.

Seventeen call sites across sixteen modules now state one. Read off each
module's own source and then checked against the list the gate carried, which
agreed in all seventeen:

| Policy | Modules |
|---|---|
| `Resolves` | `Aircraft.cpp`, `Announcement.cpp`, `FeaturedListings.cpp`, `HomeValue.cpp`, `Listings.cpp`, `PersonalPhotos.cpp`, `Sports.cpp` (both registrations) |
| `PolicyContent` | `Graphic.cpp`, `QrText.cpp` |
| `Keyless` | `Calendar.cpp`, `ClockDate.cpp`, `Forecast.cpp`, `IssFlyover.cpp`, `MoonPhase.cpp`, `SunMoon.cpp`, `Tides.cpp` |

`Sports.cpp`'s two registrations both assign a resolver as of section 24, the
shared `registerOne()` and the hand-built `live` descriptor, so both state
`Resolves`.

### 32b. Where the refusal lives, and why both halves exist

A registration makes two claims, and the compiler can only read one of them.

The first is that a decision was made at all. That is the argument, and it is
settled at compile time for every call site whatever the descriptor is called,
however it was declared, and wherever its file sits. Five of the six spellings
section 29 found were about declarations; none of them can hide the call.

The second is that the descriptor agrees with what was stated. That one cannot
be a `static_assert`, and the proposer was right about why: registration runs at
static-init over a descriptor filled in field by field, and `Sports.cpp` goes
further by taking its resolver from a switch on a runtime index. There is no
constant expression to read. `registerCard()` checks it when the registration
runs, refuses a disagreement in either direction, and logs the card id with the
policy and which way round. Refused means the card is not stored, so it stays
out of the rotation rather than drawing and reporting nothing.

Compile time for the decision, registration for the contents, and nothing at
either end that restates the other.

### 32c. The compile failure, which is the point of the change

`ci/test-key-policy.sh` compiles `tests/key-policy-signature.cpp` twice from one
source. The second run defines `KEY_POLICY_OMITTED`, which drops the argument,
and the compiler has to refuse it:

```
==> A registration naming its key policy
    compiled, 31733 bytes of object
==> The same registration with the key policy left out
    tests/key-policy-signature.cpp: In function 'bool registerSomething()':
    tests/key-policy-signature.cpp:26:29: error: too few arguments to function 'bool Cards::registerCard(const CardSpec&, KeyPolicy)'
       26 |   return Cards::registerCard(spec);
          |          ~~~~~~~~~~~~~~~~~~~^~~~~~
    In file included from tests/key-policy-signature.cpp:13:
    App/Cards.h:558:6: note: declared here
      558 | bool registerCard(const CardSpec& spec, KeyPolicy keyPolicy);
          |      ^~~~~~~~~~~~
PASS: a card registers with a key policy or it does not compile
```

Both runs are read off what they produced rather than off an exit status: the
object file has to exist for the first and not exist for the second. The second
also has to say something about the argument count, because a compile that
failed for its own reason - a stub that stopped parsing, a header that moved -
refuses the call too and would read as this test passing.

The first run of the script is `tests/key-policy-test.cpp`, which drives
`keyPolicyHolds()` over all three policies against a descriptor with a resolver
and one without, nine runtime cases and two `static_assert`s, and prints its own
PASS line. `tests/key-policy-stubs/Arduino.h` is the sixth stub directory under
`tests/`; it supplies `String` as `std::string`, which is enough because
`Cards.h` performs no String operation of its own.

The test was written after the header change, not before. What stands in for the
usual order is 32d.

### 32d. What bites it

Every mutation was gated on `cmp` against a byte-for-byte copy taken first,
restored from that copy and `touch`ed, and the restore confirmed with
`git diff --numstat`.

| Mutation | What failed |
|---|---|
| `KeyPolicy keyPolicy = KeyPolicy::Keyless` in `Cards.h` | `ERROR: Cards::registerCard(spec) compiled with no key policy named.` |
| `keyPolicyHolds()` returns `true` always | `static assertion failed: an empty descriptor cannot satisfy Resolves` |
| the second parameter deleted from the declaration | `error: too many arguments to function`, then `ERROR: tests/key-policy-signature.cpp did not compile` |
| `keyPolicyName(PolicyContent)` returns `"Keyless"` | `FAIL: PolicyContent names itself in the refusal line`, `1 case(s) failed`, no PASS line |
| `if (false && !keyPolicyHolds(...))` in `CardManager.cpp` | `ERROR: App/CardManager.cpp: registerCard() does not open with ...` |
| `PersonalPhotos.cpp` states `Keyless` while assigning a resolver | `Assertion failed: keyPolicyHolds(spec, keyPolicy), file tests/personal-photos-key-test.cpp, line 446` |

The last one is the contents half caught on a host, in a test that was already
there. Both existing content-key host tests fake `Cards::registerCard()`, and
both fakes now take the policy and `assert(keyPolicyHolds(spec, keyPolicy))`
through the same predicate production reaches, so a module they compile that
states a policy its descriptor does not satisfy stops the test. That is also
what the API change broke: the two fakes carried the one-argument signature and
would not link. After the fix they read 10 cases and 18 cases off their own PASS
lines, which is what they read before it.

### 32e. What ci/check-content-key-resolvers.sh holds now

The three lists are gone, and so are the file sweep, the descriptor grading, the
registration census and its four floors. All of that was about which cards owe a
key, which is the compiler's answer now, and a second record of it beside the
first is how `sportslive` sat between a list and a document.

What is left is five fixed-string pins across two files that do not compile on a
host, four of them already there:

| Pin | What it holds |
|---|---|
| `if(!keyPolicyHolds(spec,keyPolicy)){logKeyPolicyRefusal(spec,keyPolicy);returnfalse;}` | the registry reads the argument the compiler required |
| `StringcontentKey=card.listingId!=nullptr?card.listingId(gCurrent.item):String();` | a card's own resolver is asked first, and asked by item |
| the `assetId` then `qrData` fallback | `Graphic` and `QrText` report a key without a resolver |
| `Impressions::beginShowing(card.id,contentKey.c_str(),...)` | a key computed is a key passed |
| `attributes["contentKey"]=drawn.contentKey;` in `CheckIn.cpp` | the name the rollup reads |

It prints `5 statements pinned across App/CardManager.cpp and App/CheckIn.cpp`
and floors that count at 5, so a pin deleted trips it.

`ci/check-host-test-portability.sh`'s floors moved with the three new files under
`tests/`: 20 files to 23, and 5 stub directories to 6.

### 32f. Both boards

Compiled in a detached `git worktree` at `f206a2c` holding only this lane's
files, because three other lanes were live in the shared tree and the first
attempt there failed on `'state' is not a member of 'Motion'` from a mid-edit
`App/Motion.h` that has the name in it.

| Board | FQBN | Program | Globals |
|---|---|---|---|
| Panellan ZX2D80CE02S | `esp32:esp32:esp32s3:PSRAM=enabled,CDCOnBoot=cdc,PartitionScheme=custom` with `build.flash_size=8MB` | 1,480,911 bytes, 8% of 16,777,216 | 113,328 bytes, 214,352 left |
| 4MB fleet board | `esp32:esp32:esp32:PartitionScheme=min_spiffs` | 1,583,215 bytes, 80% of 1,966,080 | 115,460 bytes, 212,220 left |

`ci/check-partition-fit.sh` reads the Panellan image at 1,481,072 bytes against
`ota_0` at 2,621,440, 56%, and the fleet image at 1,583,376 against `ota_0` at
2,097,152, 75%, each from the table in its own build directory. The Panellan
table swap happened inside the worktree only; `App/partitions.csv` in the
working tree was never touched.

The two compiles were first launched together and collided in Arduino's shared
sketch cache: the second run removed object files the first was still linking
against, and the fleet board's run died on `ld.exe: cannot find
.../Aircraft.cpp.o` and twenty more. Nothing to do with this change, and they
were re-run one at a time.

### 32g. What this does not cover

- The three lines inside `registerCard()` that act on the refusal are not
  compiled on a host. `App/CardManager.cpp` names a graphics library through
  `Display.h`'s drawing path, and moving the registry to its own translation
  unit would mean routing twenty-five direct reads of `gCards` in the scheduler
  through `Cards::at()`, which is a change to a central file for a different
  reason. So the rule those lines apply is host-tested and their wiring is
  pinned as text, which is the proposer's second limit standing in part rather
  than falling.
- A card that reaches the registry without calling `Cards::registerCard()` is
  still outside all of this. The old gate counted strays through a `using`
  declaration or a function pointer; nothing counts them now, and a card that
  never registers draws nothing anyway.
- Whether a module's chosen policy is the right answer for that card is a
  judgement nothing here makes. The compiler requires a decision and the
  registry requires the descriptor to match it. Writing `Keyless` on a card that
  walks a feed compiles, registers and reports nothing, which is what
  `BOOT_BUTTON_AND_IMPRESSIONS_DESIGN.md`'s table is for.
- No wire shape changed. `KeyPolicy` never leaves the device, the check-in
  request and response are byte-identical, and the six-month backward
  compatibility mandate is untouched.
- CI compiles GCC 13.3 on ubuntu-24.04, and this ran under MinGW-w64 GCC 16.2.0
  locally and xtensa GCC 14.2.0 for the boards. `too few arguments to function`
  is a GCC diagnostic of long standing, and the script also accepts
  `no matching function` and `candidate expects`, but the ubuntu wording is
  unmeasured here.
- Nothing was committed or pushed. No device was flashed or commanded.
