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
2. Behaviour must be exactly as before: no heap marker in the stream, and a
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

## 7. CAL records why it restarted the device (`BootDiag.h`/`.cpp` at the repo root)

The thing being tested is a sentence, not a behaviour: after this change an App
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

Both must print, in order and identically:

```
None = 0, LowHeap = 1, Ota = 2, Reprovision = 3, SelfTest = 4,
Unreachable = 5, LowHeapResponse = 6, CalHandover = 7,
CalInstalledApp = 8, CalSelfInstall = 9
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

**What would be wrong:** a final boot reporting `OTA`. That was the behaviour
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
- **No automated test covers 7a.** The agreement between the two enums is held
  by a comment in each file and by this section. `ci/build-firmware.sh` already
  gates on `partitions.csv` matching between the two sketches for the same class
  of reason, and this invariant has no equivalent gate.
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
  behaviour `Hit::None` had before the change and it has to survive it.

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

  `ci/build-firmware.sh` reads the table rather than holding constants, for
  exactly this reason. A number typed into prose is a number that will be wrong
  the next time somebody re-tables a device.
- `strings` on the built image proves a literal is present. That is a real check
  and worth doing when a change is "the card must now be able to say X".
- It proves nothing about heap, TLS, NVS, timing, the panel, or any decision
  taken at runtime. Every item on this page is on this page for that reason.
