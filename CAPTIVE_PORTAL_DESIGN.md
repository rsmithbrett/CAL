# The WiFi setup portal, as built — 2026-10-24

What CAL does between "no network remembered" and "credentials accepted", why
each step is where it is, and what a real iPad measured against it.

Code: `Provisioning.cpp`. Measurements: a journal dump from
`8C:94:DF:4E:E6:30` on `v2026.10.24.0001`, two consecutive boots.

Sections 1–7 below preserve the original **as-built findings**: verified from
source or the recorded journal unless explicitly qualified. Section 8 is a
later **recommendation and experiment plan**, not a claim that any proposed
firmware behavior has been built or verified.

---

## 1. The sequence

`Provisioning::run()`, in order:

1. **Radio to STA, screen says "Finding WiFi networks".**
2. **Scan, once.** The result is rendered to `<option>` elements and kept in
   `cachedNetworkOptionsHtml`. Measured at **3,394 ms / 881 bytes** and
   **3,593 ms / 1,009 bytes** on the two boots.
3. **Radio to AP+STA.**
4. **Raise the access point, open.** No passphrase since 2026-10-04. An iPhone
   has to finish a WPA association before iOS raises its captive sheet, so a
   passphrase bought a spinner and little else: it was "dam" plus three MAC
   bytes, two of which the SSID already broadcasts.
5. **Wildcard DNS on port 53.** Every name resolves to `192.168.4.1`.
6. **Routes.** `/` draws the form. `/save` takes the POST. `/generate_204`,
   `/hotspot-detect.html` and `/connecttest.txt` are the Android, Apple and
   Windows probes. Everything else falls to the same probe handler.
7. **Draw the QR**, payload `WIFI:S:<name>;T:nopass;;`, with the SSID and
   `http://192.168.4.1` printed underneath.
8. **Serve** for up to 15 minutes: `dns.processNextRequest()`,
   `server.handleClient()`, `delay(2)`.
9. **On save**, remember the network, wait 400 ms so the acknowledgement
   reaches the handset, then stop the server, stop DNS, drop the AP.

### Why the scan happens before the access point

A scan takes the radio away. Doing it inside `handleRoot()` meant the phone's
first request stalled for three and a half seconds, which is long enough to
interrupt iOS's captive detection. Scanning first and caching the HTML means
the form is assembled before anyone can ask for it, and the radio is never
taken away again while the portal is up.

The cost is that the QR appears about 3.5 seconds after the portal starts.
That is before the household scans anything, so it does not add to the time
from scan to form.

### Why the probe redirect is relative

`handleProbe` answers `302` with `Location: /`.

`v2026.10.23.0001` made it absolute, `http://192.168.4.1/`, to keep
`captive.apple.com` out of the address bar. The portal then stopped appearing:
an iPhone joined and sat with no form. Wildcard DNS has already pointed the
probe host at this device, so `/` keeps the handset on the host it asked for.
An absolute redirect is a different host mid-probe, which iOS reads as leaving
the captive context.

That was restored in `8510fce`. **It was not the cause of the current problem**
— see §3.

### What the form is

About 1.8 KB, built in `handleRoot`. Inline CSS, no JavaScript, no cookies, no
external resources, no images. A `<select>` of the cached scan, a text field
for a hidden or missing network, a password field, a submit button. That is
deliberately the shape Apple's captive browser can render, which has
JavaScript and cookies disabled.

---

## 2. What the device actually did

Two boots, from the journal.

| | Boot 1 | Boot 2 |
| --- | --- | --- |
| Scan | 3,394 ms, 881 B | 3,593 ms, 1,009 B |
| First probe | +23,809 ms | +31,284 ms |
| Probes | 7 | 6 |
| Form served | every time, 3-4 ms | every time, 3-4 ms |
| Outcome | abandoned at 15 min | credentials accepted, `SMITH_5G` |

Boot 2's timing is the whole story:

```
+31,284 ms   probe, form served in 3 ms
+32,316 ms   probe, form served in 4 ms
+32,416 ms   probe, form served in 3 ms
             ... 9 minutes 32 seconds of nothing ...
+604,661 ms  probe, form served in 3 ms
+612,070 ms  probe, form served in 3 ms
+612,446 ms  probe, form served in 3 ms
             credentials accepted for SSID 'SMITH_5G'
```

The iPad asked three times in the first 32 seconds, got a valid form every
time, then went quiet for nine and a half minutes. The burst at +604 s is a
person coming back and forcing it.

**The device answered every request it received, in 3 to 4 milliseconds, and
dropped none.**

---

## 3. What that rules out

Each of these was a live hypothesis and each is dead.

**The redirect.** Ruled out by the probe lines. The phone follows the 302 and
fetches `/` every time. Seven probes, seven root requests.

**The radio, AP+STA and channel hopping.** Ruled out. The theory was that the
STA side keeps retrying a remembered network and drags the access point off
channel. This device had `networks=0` — nothing remembered, nothing to retry —
and it served every request.

**Socket exhaustion.** Ruled out. `CONFIG_LWIP_MAX_SOCKETS=16` in this build,
and nothing queued or failed. Espressif's note about phones opening many
simultaneous probe connections is real, and the three-probes-in-a-second
bursts are that behavior, but 16 sockets absorbed it.

**The form itself.** Ruled out as far as the wire goes: 200, correct length,
3 ms, self-contained, within what the captive browser renders.

**What is left** is that iOS received a valid portal page and did not put it on
screen. An Apple Developer Forums thread describes the same shape on iOS 26 and
not on iOS 18: the sheet fails with "the network connection was lost", the page
"starts reading, but then stops mid-read", and the portal loads normally when
Safari is opened by hand. No Apple reply, no fix.

That poster was **already answering the probe URL directly with the portal
page, no redirect, and it still failed.** So changing our 302 to a 200 is not
a fix. That is the second firmware change this investigation has killed before
it shipped.

**Not established:** the iOS version on the iPad, and whether the sheet opened
and was dismissed or never opened. Both would narrow it further.

---

## 4. Why a hotel's portal behaves better

Not the hardware. The captive sheet never learns what served the page.

Commercial gear advertises the portal address in DHCP itself, option 114,
RFC 8910. Apple processes it automatically from iOS 14 and Big Sur, instead of
guessing from probes. The softAP's DHCP server does not send it.

**It is not a fix we can take.** Apple's documented form points at an HTTPS
captive-portal API endpoint with a valid certificate. A device on
`192.168.4.1`, with no public name, cannot have one. Field reports also have
iOS receiving option 114 and still falling back to the legacy probe on first
join, honoring the option only after a rejoin.

So hotels hit the same flakiness. They have a mitigation a LAN-IP device does
not.

---

## 5. The one lever that does not depend on detection

A second QR, encoding `http://192.168.4.1`, drawn once a phone has associated —
`WiFi.softAPgetStationNum()` goes above zero.

A camera reading it opens the browser at that address. No probe, no sheet, no
heuristic, nothing for Apple to regress, and the same behavior on Android. It
is a different mechanism rather than a better-tuned version of the same one.

This is **not built** and needs a wired reflash, as every CAL change does.

---

## 6. Two screen defects found by photograph

Neither is visible to any test.

**The MAC is drawn twice** on the enrollment screen. `Display::showQr`'s
contract is "for a regular QR, the payload is printed too", and `CAL.ino`
passes the MAC as the payload *and* as the subcaption. Dropping the third
argument removes the duplicate.

**The caption is cut off**: "Waiting for an administrator to set t...". It is
the server's message rendered verbatim and it is longer than the caption area.
The owner chose to fix this by wrapping on the device rather than shortening
the message server-side, so the screen fits whatever the server sends.

---

## 7. Where this meets the self-service claim

`SELF_SERVICE_DEVICE_CLAIM_DESIGN.md` §7 already specifies the next change to
this screen: `/api/enrollment/register` gains an optional `setupUrl` on its
`pending` response, and CAL draws it in place of the bare MAC when it is there.

The server builds the whole string from `Enrollment:SetupUrlTemplate`, and CAL
never constructs one, because CAL is the binary that cannot be recovered
remotely and a policy compiled into it is permanent. The template ships empty,
so a device that takes the new loader before the server work lands behaves
exactly as it does today.

That design also records that the MAC was chosen over a one-time pairing code
with the trade stated: a MAC is guessable within a vendor prefix, so the claim
is gated on the device being unclaimed and recently seen, and an adoption is
reversible by a System Administrator.

---

## 8. Explicit recommendations: reliable iPhone/iPad provisioning (2026-10-24)

**Decision:** Keep the currently working pre-AP network scan and relative
HTTP redirect. Do not modify GoDaddy DNS. Run two isolated CAL firmware
experiments (nonempty redirect body, then optional DHCP captive-portal
advertisement), instrument the complete iOS experience, and retain a direct
URL/second-QR fallback if automatic presentation cannot be made reliable.

**Status correction — action already performed by the owner:**
**"Forget This Network" on the iPhone/iPad has ALREADY BEEN DONE.** Do not
give this as the next step, block the work on it, or require repeating it in
order to try the recommendations below. Whether Auto-Login was enabled after
the fresh join and the exact iOS/iPadOS version have not yet been recorded.
The continued portal-display problem is the reason for this investigation.

These recommendations update the implementation choices in §4: a *standards-
compliant* RFC 8910/8908 captive-portal API requires HTTPS and is not presently
available on the local-only CAL AP. However, Espressif's Arduino-ESP32 **3.3.11**
code exposes an **experimental, noncompliant HTTP Option 114 advertisement**
through \`WiFi.AP.enableDhcpCaptivePortal()\`. We can evaluate it in a bench
build without committing to it for production or treating it as guaranteed
Apple behavior.

### 8.1 What we must preserve (no regressions)

- **Keep** the STA scan before \`WiFi.softAP()\`; keep cached SSIDs in
  \`cachedNetworkOptionsHtml\`. Never call \`WiFi.scanNetworks()\` in an HTTP
  handler while a customer is connected to the AP.
- **Keep** wildcard local DNS, \`dns.start(53, "*", WiFi.softAPIP())\`,
  and the current relative \`Location: /\` redirect. The earlier *absolute*
  redirect to \`http://192.168.4.1/\` visibly regressed iPhone behavior; the
  rollback in \`8510fce\` must not be undone during these experiments.
- Keep the existing routes \`/\`, \`POST /save\`, \`/hotspot-detect.html\`,
  \`/generate_204\`, \`/connecttest.txt\` and the not-found fallback.
- Keep the Wi-Fi join QR payload \`WIFI:S:<name>;T:nopass;;\` and the readable
  local fallback URL beneath it. Do **not** redirect or intercept HTTPS
  connections to Apple, Google or arbitrary websites.
- Do not change AP password/encryption, certificate handling, credential
  storage, tenant configuration, or GoDaddy records as part of this narrow
  captive-portal experiment.
- Current journal times (3–4 ms to serve root) measure the **device handler**.
  They do **not** by themselves establish that iOS received the complete
  response or displayed the login sheet. Avoid declaring the client-side
  issue conclusively diagnosed.

### 8.2 First firmware experiment (P1): nonempty body on the HTTP redirect

**Rationale:** In the current \`Provisioning.cpp\`, \`handleProbe()\` sends
\`302\` with \`Location: /\` and an empty \`text/plain\` body. Espressif's
ESP-IDF captive-portal example sends a relative redirect **with content** and
explicitly notes an iOS compatibility reason. This is a small change that
can be tested independently; it is **not** a proven fix for the observed iPad.

**Only change the final response in \`handleProbe()\`:**

\`\`\`cpp
// Preserve sendNoCacheHeaders(), probe logging and the relative redirect.
server.sendHeader("Location", "/", true);
server.send(302, "text/html; charset=utf-8",
            "<!doctype html><html><body>"
            "Opening DiscoverAroundMe WiFi setup."
            "</body></html>");
\`\`\`

Keep \`handleRoot()\` returning the actual self-contained configuration form
as HTTP 200. Do not change the redirect destination, do not redirect to HTTPS,
and do not replace \`302\` with \`200\` in the same experiment.

**Proof required:** Compare against the unchanged baseline on the *same*
device and iPhone/iPad. Log whether the captive window **automatically
appears**, stays visible, and allows submission; a 302 followed by a 200 in
the serial log alone is not a passing result. Record response headers and the
iOS/iPadOS version. If worse, restore the empty body without further changes.

### 8.3 Second firmware experiment (P2): advertise DHCP Option 114

**Rationale:** The pinned \`arduino-esp32 3.3.11\` source contains
\`APClass::enableDhcpCaptivePortal()\` (behind an ESP-IDF >= 5.4.2
compile-time condition). It stops the AP DHCP service briefly, sets the
\`ESP_NETIF_CAPTIVEPORTAL_URI\` DHCP option, and restarts DHCP. Its advertised
address is an **HTTP** URL such as \`http://192.168.4.1\`.

In a **separate, opt-in test build** of \`Provisioning.cpp\`, after
\`WiFi.softAP(name.c_str())\` succeeds and **before** the QR is shown or
the user associates, try:

\`\`\`cpp
// EXPERIMENT ONLY: requires the matching Arduino-ESP32 / ESP-IDF API.
const bool advertised = WiFi.AP.enableDhcpCaptivePortal();
Journal::printf("[portal] experimental DHCP option 114: %s",
                advertised ? "enabled" : "failed");
\`\`\`

Keep existing wildcard DNS and HTTP handlers in place. A failed optional
advertisement should **not** prevent ordinary portal setup. Verify the
actual DHCP OFFER/ACK in a packet capture contains **Option 114** and the
expected URI; a successful function return alone is insufficient.

**Standards warning:** RFC 8910 says Option 114 identifies an RFC 8908
Captive Portal **API endpoint**, not simply an HTML setup form. RFC 8908
requires an **HTTPS** endpoint and \`application/captive+json\` API data,
with a TLS-protected user portal. The Espressif helper's local HTTP URI does
**not** satisfy that design. Modern iOS may ignore or mishandle it.
Do not characterize this helper as a standards-compliant deployment or
merge it based only on an improved first test.

**Proof required:** Observe whether iOS receives Option 114 and whether
automatic form display improves relative to P1. Look for duplicate or failed
DHCP leases, reconnect loops, portal suppression, and Android regressions.
If it offers no measurable improvement or causes a regression, remove it.
A fully compliant HTTPS captive-portal API should be a **separate design
decision** with certificate, reachability and trust constraints evaluated
explicitly, not casually added to the factory CAL.

### 8.4 DNS recommendation (P0 diagnostic, not a blind configuration change)

**Do not modify GoDaddy or public \`discoveraroundme.com\` DNS.** These records
are unrelated to captive discovery inside the ESP32's isolated temporary
setup network.

Keep the ESP32 wildcard \`DNSServer\` as the local responder for legacy
HTTP captive probes. Before proposing changes to \`softAPConfig()\` or DNS:

1. Capture a **DHCP OFFER/ACK** and verify the leased phone IP, gateway
   (\`192.168.4.1\` or the actual softAP IP), DNS server(s) (**DHCP
   Option 6**) and, in the Option-114 variant, captive URI.
2. Verify from iOS Wi-Fi network details that DNS is **Automatic** and note
   which DNS server(s) it actually received. Check for unexpected manually
   configured DNS, active VPN, encrypted DNS profiles, or Private Relay
   effects *as diagnostics only*, not prerequisites to customer setup.
3. Confirm an HTTP request to a test hostname resolves to the ESP32 and
   returns a response. Distinguish DNS timeout, TCP connection, server
   response, and iOS captive-sheet presentation.
4. **Only if the DHCP/DNS capture is wrong**, propose a targeted local
   DHCP Option-6 / softAP DNS fix and test it separately. If correct,
   changing DNS addresses is not supported by current evidence.

A wildcard DNS response of \`192.168.4.1\` does not guarantee that the
client used that resolver on every request; capture the packet sequence.

### 8.5 Required instrumentation before declaring a solution

In \`Provisioning.cpp\` / the existing journal, record timestamps relative
to AP readiness for: AP created; associated station count transitions;
first observed DNS query (if bounded/cheap to log); captive probe
(path and status); \`GET /\` start/finish; \`POST /save\`; AP shutdown.
Avoid logging submitted passwords or secrets. Keep journal volume bounded
(especially bursts of captive probes).

Capture *phone-side* observations for every attempt:

- Device model and exact iOS/iPadOS version/build.
- Joined via Camera QR vs Wi-Fi Settings, whether AP actually associated.
- Whether the captive sheet appeared automatically, appeared then
  disappeared, or never appeared.
- Seconds from successful Wi-Fi association to a visible **usable** form.
- Whether selecting SSID and posting credentials succeeded.
- Whether manually opening \`http://192.168.4.1\` in Safari worked
  immediately while the automatic sheet failed.

If available, capture DHCP/DNS/HTTP traffic on a test network to distinguish
an ESP32 socket/send result from what arrived at the phone. A UI failure
without failed network packets should be reported as a *client presentation
failure hypothesis*, not asserted as a proven Apple defect.

### 8.6 Bench A/B test matrix and acceptance gate

Run each variant on a **USB-recoverable** CAL device, changing only the
identified variable:

| Variant | Redirect body | DHCP Option 114 | Expected purpose |
| --- | --- | --- | --- |
| A — current baseline | Empty | Off | Establish actual success rate |
| B — P1 | Nonempty HTML | Off | Isolate redirect-body behavior |
| C — P2 | Empty | On | Isolate DHCP-advertisement behavior |
| D — conditional | Nonempty HTML | On | Test combination only if B/C justify it |

For each variant, perform **at least ten association/setup attempts**
on the same iPhone/iPad, record its exact OS version, and repeat the
promising candidate on one other iOS generation and one Android device.
Restart provisioning cleanly between attempts; record whether the phone
reconnected automatically. **Do not tell the owner to repeat Forget This
Network** — that step has been completed. Record any new saved-network
state changes that occur naturally, without hiding them from the results.

**Proposed rollout threshold (not a measured result):** At least 19/20
automatic displays of a usable form across validation iOS devices, with
median association-to-form under 5 seconds and no Android regression.
If neither B nor C passes, keep the known-good baseline and make the
manual fallback genuinely one-tap/scannable rather than continually tuning
DNS without supporting evidence.

Test hidden SSID/manual entry, multi-AP mesh names, repeated visits,
a wrong home Wi-Fi password, and AP timeout to avoid unrelated CAL
regressions.

### 8.7 Fallback if the automatic captive sheet remains unreliable (P3)

Build the previously proposed **two-stage QR** on the LCD only after
measuring the P1/P2 results:

1. Show existing \`WIFI:S:<name>;T:nopass;;\` to join \`Discover-Setup-XXXX\`.
2. Keep the first QR visible while the phone associates; watch
   \`WiFi.softAPgetStationNum()\` for a transition above zero.
3. Once connected, show a new QR whose **payload is the actual**
   \`http://<WiFi.softAPIP()>/\` address. Print the URL in legible text.
   Leave enough time to avoid a flashing screen; account for multiple
   connected phones and disconnect/reconnect.
4. Scanning this second QR should open Safari (or the system browser)
   **directly**, bypassing automatic captive-sheet detection. Keep a
   route back to the join QR, and make the URL available in text throughout.
5. Validate this separately: a second QR makes manual recovery easier but
   is **not** automatic portal presentation and does not guarantee iOS will
   stay attached to a no-internet AP.

Avoid moving to an app-only onboarding path; CAL must stay usable on an
unconfigured factory device with no phone app installed.

### 8.8 Scope, rollback, and delivery rules

- **Not implemented by this document:** The P1 redirect body, P2 DHCP
  advertisement, additional instrumentation, and P3 second QR remain
  proposed. The owner performed the phone's Forget This Network step.
- Do not bundle P1 and P2 in a single unmeasurable firmware change.
  Use separate commits/build variants and record results in this MD.
- This repository's push-to-main workflow **builds and publishes CAL/App
  firmware**. Keep research and untested changes on a review branch
  until the hardware tests are signed off.
- CAL runs in the factory partition. A CAL firmware change needs a
  deliberate **wired flashing** / recovery plan, unlike an ordinary App OTA.
  Do not attempt remote automatic CAL deployment on the assumption that
  it behaves like an App update.
- Preserve the measured 3–4 ms root response behavior. If P1 or P2 slows
  it, record and diagnose the regression before rolling out.
- The duplicated MAC and clipped enrollment caption (§6), and the
  separate self-service URL design (§7), are **independent tasks** and
  must not be used to broaden the captive portal experiment.

### 8.9 Source references (technical rationale, not proof on this hardware)

- Apple Developer, *How to modernize your captive network*:
  https://developer.apple.com/news/?id=q78sq5rv
- Apple Support, *Use captive Wi-Fi networks on your iPhone or iPad*
  (including Auto-Login behavior):
  https://support.apple.com/en-us/102554
- RFC 8910, DHCP captive-portal **Option 114** and API-URI requirement:
  https://www.rfc-editor.org/rfc/rfc8910.html
- RFC 8908, Captive Portal API (HTTPS, JSON):
  https://www.rfc-editor.org/rfc/rfc8908.html
- Espressif ESP-IDF captive-portal example (relative redirect with
  nonempty response content):
  https://github.com/espressif/esp-idf/blob/master/examples/protocols/http_server/captive_portal/main/main.c
- Espressif Arduino-ESP32 **3.3.11** implementation
  (\`APClass::enableDhcpCaptivePortal\`, HTTP URL):
  https://github.com/espressif/arduino-esp32/blob/3.3.11/libraries/WiFi/src/AP.cpp
- CAL's own measured baseline: §§1–3 of this document, with firmware
  \`v2026.10.24.0001\` journal timings.

