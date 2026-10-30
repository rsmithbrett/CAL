# CAL presence and GPIO diagnostics

Enter the six-tap configuration menu, then **Diagnostics**. Choose
**Presence sensor** for the dedicated AM312 view or **GPIO monitor** for all
verified available inputs. Tests are local, use no network endpoints, and
never change device registration, saved Wi-Fi or App policy.

## Two board types

| Board | Inputs monitored simultaneously | Read method |
|---|---|---|
| Original ESP32-2432S028 | GPIO27, GPIO34, GPIO35 | GPIO27 digital; GPIO34/35 calibrated ADC1 millivolts. |
| Series 2 / Panellan ESP32-S3 | GPIO10, GPIO11, GPIO12, GPIO13, GPIO14, GPIO21 | GPIO10 calibrated ADC1 millivolts; the others digital. |

The S3 set comes from the vendor general-purpose header set already recorded
in `App/CardManager.cpp`. Apparently unused silicon pads are not assumed to be
verified breakouts. Other pins remain visible in **All pins**, labeled
**Reserved/unverified** or **Unavailable**. Reserved pins are never sampled,
reconfigured, driven or assigned pull resistors. This includes flash/PSRAM,
LCD, touch, USB, serial and boot/reset connections.

The monitor reads only the explicit board allowlist. Digital candidates use
`INPUT` without pull-up/down. Analog candidates use per-pin 11 dB attenuation
on ADC1; **ADC2 is never sampled while Wi-Fi is active**. Build-time SDK checks
verify the ADC1 mappings. A floating/unconnected input can change and is not
evidence of a fitted or working sensor. GPIO27's external button may need its
external bias for a meaningful idle reading in this no-pull diagnostic mode.

## GPIO monitor

- Every allowlisted input is sampled every 50 ms, including pins not on the
  displayed page. The screen updates at most four times a second.
- **Changes** is the default: a feed of the latest eight observed HIGH/LOW
  transitions across all monitored inputs, four per page. Newest events appear
  first. Previous/Next wrap through the pages, and the status line on the
  original board reads `Watching 3 inputs - changes page 1/2`.
- **That page count is derived, not fixed at two.** Both the paging and the
  status line divide `GpioMonitor::kEventCount` by the four rows a page holds,
  so raising the size of the feed adds pages. The figure was written into both
  places as a literal `2`, which was right only while the feed held exactly
  eight events: a larger feed would have left the extra events unreachable, with
  the screen still saying `2`.
- Each event shows GPIO number, value, HIGH/LOW, transition count and age. A
  changed row is green for two seconds. Initial readings establish baselines
  and do not generate false change events.
- **All pins** lists every GPIO number from 0–39 on the original chip and 0–48
  on the S3, four per page, with current readings or a not-sampled status.
  Previous/Next wrap through all pages. The top-right button toggles views.
  Changing pages/views does not reset counters or pause other inputs.
- A level must remain stable for 150 ms before a transition is recorded. ADC
  readings use the App's shared 1,500 mV threshold to classify HIGH/LOW. This
  is an explicit test threshold, not a measurement of the digital receiver's
  switching threshold. Voltage changes that stay on one side remain visible
  in All pins without flooding the feed with ADC noise.
- Each row includes a graphic bar. ADC bars show pin voltage on a 0–3,300 mV
  display scale (clipped above that); digital bars show full for HIGH and
  empty for LOW. Neither represents distance or confidence.
- **Back** stops sampling and returns to Diagnostics. Opening the monitor
  again starts a fresh session. Counters and the bounded feed live only in RAM.
  A sampled transition can be missed if shorter than the polling/debounce window;
  this is not a logic analyzer or interrupt capture.

## Dedicated presence view

On the original board, use the same GPIO35 ADC path, 1,500 mV threshold and
50 ms sampling interval as the App. Its graphic bar is green/full while the
raw signal is HIGH and empty while LOW. Show millivolts, raw threshold change
count, seconds since the last change, and a 48-column recent-history strip.
Columns are 250 ms each, covering 12 seconds. Dark columns mean no sample;
missed intervals are not invented as continuous HIGH/LOW.

Readings are visible during sensor power-up with a settling notice for the
first 30 seconds of CAL uptime. This screen is a raw electrical test, not the
App's debounced presence/session algorithm. It cannot determine whether the
App's motion policy is enabled; CAL labels that policy as not loaded.

On the S3 there is no dedicated PIR mapping in the App today. The presence
screen says so and performs no GPIO35 ADC read. Use the GPIO monitor to
identify activity on one of the S3's general-purpose inputs. Do not interpret
the dedicated screen as saying the whole S3 has no available inputs.

## Implementation and tests

`PresenceInput.h` in DamBoards shares the dedicated pin/threshold/timing with
the App. `GpioMonitor.h` contains a separate explicit board allowlist and a
bounded debounced input/event model. CAL's Recovery module handles sampling
and passes state to the display. No extra tasks, interrupts, images or
framebuffers are introduced; the title and navigation remain in place while
readings refresh. No sampling runs outside the selected live diagnostic. The
mode button is drawn from `Recovery::kGpioMode`, the rectangle its hit test
already asks, so the button and the target cannot drift apart. A `static_assert`
in `RecoveryState.h` holds the mode button clear of each of the three footer
buttons, and each footer button clear of its neighbor, in every build of both
boards.

HTTP and DNS stay serviced when a test is opened over the Wi-Fi portal.
A test outlasts the menu's 60-second idle timeout and closes itself after
**10 minutes** without a tap. A form submitted underneath waits until then, or
until somebody takes Back. With no limit at all, a screen left open parked the
portal loop that applies those credentials, and the enrollment loop that polls
for a device key, for as long as it stayed up.

Run `bash ci/test-recovery-menu.sh` for both target configurations. It tests
dedicated presence support/absence, threshold crossings, history gaps/wrap,
sample throttling, GPIO allowlists, no protected-pin hardware calls, stable
transitions through timer rollover, bounded event history, menu navigation,
sampling stop and counter persistence across pages. The PR bench workflow
builds both actual targets and checks real partition limits.

Before release, bench-test both physical boards: verify pin numbers against
the exact board revision, apply a known LOW/HIGH within the board's electrical
limits to each allowlisted input, compare ADC values with a meter, check
cross-pin events and paging, and keep the portal/touch/display working during
the test. Check floating inputs and pulse/debounce behavior. Verify Back stops
sampling and both builds fit their factory/staging partitions. Runtime heap
and exact firmware size are measured by build/bench, not inferred from host tests.
