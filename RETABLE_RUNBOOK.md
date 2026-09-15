# Moving a device onto the new partition table

This is for the 2026-09-15 table change, which grew `factory` from 1,441,792 to
1,703,936 bytes. It needs a USB cable touching the device **once**. It cannot be
done over the air — `partitions.csv` explains why at length, and the short version
is that the pinned ESP32 core panics rather than let firmware write the partition
table, and that even if it did, there is a millisecond window with no valid table
and no redundant copy.

**A unit left on the old table is not broken.** It keeps working, with less room
for CAL to grow. There is no deadline; there is only the fact that CAL is
installed over USB alone, so a device nobody can reach can never take a new CAL
anyway.

---

## Part 1 — what the person holding the device does

They need **Chrome or Edge** and a **USB cable**. They do not need esptool,
Python, Arduino, or anything installed. Send them this and nothing else:

> 1. Plug the device into your computer with a USB cable.
> 2. Open **https://discoveraroundme.com/cal** in Chrome or Edge.
>    (Safari and Firefox cannot talk to USB devices — it has to be one of those two.)
> 3. Click **Connect and Flash**. Pick the device from the list that appears.
> 4. Wait. It takes a few minutes and shows a progress bar. **Do not unplug it.**
> 5. When it says the firmware is written, **unplug the USB cable and plug it back
>    in** (or press the reset button). The device does NOT restart by itself after
>    flashing - nothing happens until you do this.
> 6. It will then come up asking to be set up. Follow the on-screen setup to join
>    it to your WiFi.
> 7. It will then say something about an administrator needing to re-issue a key.
>    **That is expected — you are done. Tell me and I will finish it from here.**

Three things worth telling them in advance, because all three look like failure:

- **When the flash finishes, nothing visible happens.** The device stays halted
  until it is power-cycled - screen dark, or frozen on whatever was there before.
  This is the one that gets reported as "the flash didn't work", and it is the
  normal end state. Confirmed on the bench 2026-09-15: the page asks the adapter
  for a hard reset and this hardware ignores it.

- **If the device list is empty**, they need the USB driver. The page's own
  "If something isn't working" section links it (Silicon Labs CP210x). Install,
  replug, try again.
- **If the flash stops partway**, nothing is damaged. The ESP32's first-stage
  bootloader lives in ROM and cannot be erased, so the chip always accepts
  another attempt. Click Connect and Flash again.

## Part 2 — what you do, from anywhere

The flash writes the full 4MB image, which blanks `nvs`. That is where the device
kept its secret and its remembered WiFi, so the device comes back not knowing who
it is. It asks the server for its secret by MAC address and is refused:

```
MacRegistrationOutcome.AlreadyIssued
```

That refusal is correct and deliberate — a MAC that has collected its secret once
cannot silently collect another, or anyone who learned a MAC could impersonate a
device. Clearing it is an operator decision, so it is an operator action:

1. Go to **`/diag/deviceregistry`** as a SysAdmin.
2. Find the device.
3. Click **Allow re-registration** (the amber button on that device's row).

   It confirms with: "Device N can register itself again - power-cycle it (or wait
   for its next retry) and it will pick up a fresh secret on its own."

That calls `ResetForReRegistrationAsync`, which clears `SecretRetrievedAtUtc`
*and* the stale hash. CAL is already retrying on its own backoff loop, so it picks
up a fresh secret with no further help from the person at the other end, then
downloads the App into the new `ota_0` and starts serving cards.

**Do this after they flash, not before.** The reset clears the secret hash, so
doing it early would stop the device checking in while it is still sitting there
waiting for its turn with the cable.

`RegenerateSecretAsync` is the wrong button and will not work: it mints a new
secret but does not touch `SecretRetrievedAtUtc`, so it cannot unblock this no
matter how many times it is pressed. That is documented on the method itself.

## What survives, and what does not

| | Survives? |
|---|---|
| Device name, id, history, card policy, brand/account placement | **Yes** — server-side, keyed by MAC |
| Telemetry and reboot history | **Yes** — server-side |
| Device secret | No — re-minted after the reset above |
| Remembered WiFi network | No — re-entered during setup |
| Installed App | No — CAL re-downloads it |
| Boot journal (`callog`), brand splash (`spiffs`) | No — blanked, and both refill on their own |

Nothing in the first two rows is at risk, because none of it was ever on the
device. That is why this is a safe operation rather than a nervous one.

## Doing it yourself, with a cable

If the device is on your own bench, the browser route above works identically and
is still the easiest path. If you would rather use esptool directly, two writes
are enough, and this version **keeps `nvs`** — so no reset and no WiFi re-entry:

```
esptool --chip esp32 --port COM5 write_flash 0x8000 CAL.ino.partitions.bin 0x10000 CAL.ino.bin
```

```
esptool --chip esp32 --port COM5 erase_region 0xE000 0x2000
```

The second erases `otadata` so the fall back to CAL is unconditional, rather than
depending on the bootloader rejecting the garbage now sitting at the old `ota_0`
address. It should reject it anyway — it validates the image header — but the
cable is already attached and removing the doubt costs nothing.

Do **not** use `CAL.ino.merged.bin` here. It is a full 4MB image written from
`0x0` with `0xFF` at `0x9000`, so it blanks `nvs` and puts you back in Part 2.
That is the right behaviour for `/cal`, which exists to set up brand-new
hardware — it is just not what you want on a device that is already enrolled.

## Confirming it worked

The new table is in place if the device's own serial output stops complaining
that `callog` is missing — `Journal.cpp` looks the partition up by name, disables
itself when it is absent, and says so. On the old table it says it is disabled; on
the new one it writes a sector per boot.

Server-side, the device reappears in `/diag/deviceregistry` with a check-in and
its telemetry resumes, and `/diag/health`'s device path stops grading it Silent.
