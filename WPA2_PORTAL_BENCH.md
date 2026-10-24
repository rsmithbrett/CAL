# WPA2 captive-portal bench test — 2026-10-24

## Objective

Determine whether a WPA2-protected setup AP changes iPhone Camera-initiated **Wi-Fi QR → Join → captive webpage** behavior compared with the existing open AP. The observed delays were tens of seconds (or the webpage failed to appear), so the presumed WPA2 association penalty has not been established as the cause. Keep one scan and no typed *setup AP* password: the Wi-Fi QR contains a generated passphrase.

## Scope and safety

- CAL factory/recovery image is altered only on a disposable, USB-recoverable bench unit. Never push this experiment to main merely to publish a test firmware.
- No App, backend check-in, fleet firmware catalog, OTA registration, or global current CAL version changes.
- Do **not** replace or weaken the existing HTTP captive handlers, wildcard DNS, relative `Location: /` redirect, pre-AP scan, or manual `http://192.168.4.1` fallback in the same experiment.
- WPA2 encrypts the Wi-Fi link, including HTTP credential submission. HTTP is **still** HTTP, not HTTPS. WPA2 does not authenticate a malicious AP with the same SSID and a leaked passphrase.
- Use a per-device random 24-character hexadecimal setup passphrase, persisted in CAL's NVS namespace. Randomize only on first provisioning, **not** on every boot: reusing one SSID with a changing password conflicts with saved iPhone Wi-Fi credentials.
- Never log the passphrase or put it in a publicly downloadable page. The displayed QR is deliberately available to a person standing at the device; the access point lasts for the current setup window.
- Reject launching the AP if a strong password cannot be stored. Never silently fall back to an open AP.
- Keep the ability to hold BOOT at startup to reopen Wi-Fi setup and recover by USB.

## Code contract

1. `Identity::setupApPassword()` returns the saved 24-character uppercase-hex value or generates/stores one using ESP32 hardware randomness once the Wi-Fi radio is enabled. Return an empty string on NVS storage failure; do not log secrets.
2. `Provisioning::run()` starts `WiFi.softAP(name.c_str(), password.c_str())`, which defaults to WPA2 for a supported passphrase, and shows `WIFI:T:WPA;S:<name>;P:<password>;;`.
3. The setup form still asks only for the **household** Wi-Fi password. Scanning the initial QR supplies the **temporary AP** password; the user should not enter that one manually.
4. Log security mode, station joins/disconnects and captive HTTP requests, but not household passwords.
5. Build `CAL.ino.merged.bin` on a PR-only Actions job; download the artifact, extract the merged image, and select it in the explicit bench mode `/cal?bench=1` of the DiscoverAroundMe Host (requires its separate PR/deployment). That mode must not change what public `/cal` serves.

## Test protocol

On the same iPhone/iOS build and the same bench ESP32, run the baseline open-AP CAL binary and the WPA2 experiment in randomized alternating order. Record iPhone join approval → AP association, first probe, root page request, and **human-visible** captive page. A root request in logs is not proof that the phone displayed the page. Run at least 10 trials per variant; include a Settings → Wi-Fi control and Android regression. Record the exact CAL binary SHA and iOS version. Previous **Forget This Network** troubleshooting has already been performed and should not be used as the proposed fix.

Separate follow-on A/B trials: PaperColor HTTP 200 catch-all versus redirect; on-device two-stage QR fallback. Do not change those in the WPA2 trial.

## Required gates before any production release

- CI build succeeds and actual binary fits the factory (0x1A0000) and OTA staging (0x200000) partitions.
- Verify an actual original ESP32 joins the WPA2 AP by scanning with iPhone Camera, setup form saves, device joins 2.4GHz home Wi-Fi, enrollment and App installation function.
- Confirm browser test flasher accepts only merged images and public `/cal` continues to download the current catalog build.
- Assess whether WPA2 reliably improves Camera-based CNA visibility; otherwise keep a user-directed URL / QR fallback.

**State:** Bench experiment in a draft branch. Not production-verified or deployed.
