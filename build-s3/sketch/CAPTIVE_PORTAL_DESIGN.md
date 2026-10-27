#line 1 "C:\\Users\\Administrator\\Documents\\CAL\\CAPTIVE_PORTAL_DESIGN.md"
# The WiFi setup portal, as built — 2026-10-24

What CAL does between "no network remembered" and "credentials accepted", why
each step is where it is, and what a real iPad measured against it.

Code: `Provisioning.cpp`. Measurements: a journal dump from
`8C:94:DF:4E:E6:30` on `v2026.10.24.0001`, two consecutive boots.

Everything below is **Verified** from the source or from that dump unless it
says otherwise.

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
