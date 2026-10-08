# Release notes

**What changed for whoever uses the device — not what changed in the code.** Every entry
answers "what will somebody notice, and what should they do about it". A reader of a GitHub
release is deciding whether to install this and what to expect afterwards; a summary of
which function moved tells them neither.

Both CAL and the App ship from one release (see `VERSIONING.md`), so both appear here, and
an entry says which one it is wherever that changes what a reader does.

Keep **Unreleased** at the top. The build writes whatever is under it into the release body
above the checksums, so an entry written here before the merge is the entry that ships.

---

## Unreleased

### The overhead card names a military aircraft — App

When a military aircraft is in range, the card shows that one rather than whichever
airliner happens to be nearest, and it names the service and the aircraft: "U.S. Navy -
MH-60 Seahawk" where an airline's name usually sits. The callsign, the distance and the
altitude read the same as always.

A service the callsign does not identify reads "Military", which is what is actually
known. An ordinary sky is unchanged: the nearest aircraft, with its airline.

Not yet seen running on a device.


### A notice with no button can be cleared by touching it — App

Tap the middle of the screen while a notice is showing and it goes away, the same as
pressing a notice's button does. Before this, a notice without a button stayed up until it
reached the end date whoever posted it set, or until somebody cleared it from the website.

Notices that *do* have a button are not affected. Those still need the button pressed,
because the press is what sends the message the button was set up to send - tapping
elsewhere on a notice like that does nothing.

Clearing a notice this way is reported the next time the device checks in, so it can take
up to one check-in interval before the website shows it as read. If the device is unplugged
in between, the notice comes back and needs tapping again.

**Not yet seen running on a device.** Neither the tap nor the reporting has been tried on
real hardware, so treat this one as worth checking on a device you can watch before relying
on it.

### WiFi setup is reachable without a power cycle — CAL

Hold the button on the back for three seconds at any point while the device is starting up,
including while it is working through the networks it already knows, and it stops trying and
shows the setup code.

Until now the button only worked if it was already held down at the moment the device was
plugged in. A household whose WiFi had changed had to watch it fail through every remembered
network, wait for it to give up and restart, and then catch the next power-on with the
button already pressed. That is the step people were getting stuck on.

The screen tells you which gesture you are heading for while you hold, and letting go early
cancels. Holding for ten seconds **from power-on** still erases the device's identity, as
before — that is a separate, deliberate action for moving a device to a different account,
and it needs an administrator to issue a new key afterwards.

### Joining the setup network is faster on iPhone — CAL

The device's own setup network no longer has a password. Scanning the code on screen now
brings up the WiFi form almost immediately; before, the phone had to complete an encrypted
connection first, which left people watching a spinner before they were shown anything to
fill in.

The password it used was derived from the device's own address, two bytes of which the
network name already broadcast, so it was never much of a secret. While the setup screen is
up, anyone within WiFi range could now point the device at a network of their choosing — it
shows no stored passwords and nothing else is reachable — which needs them standing within
range of a device its owner is already standing over.

---

## How to write an entry

- **Lead with what someone will notice.** "WiFi setup is reachable without a power cycle",
  not "added a polled GPIO detector to the join loop".
- **Say what to do differently**, if anything. Most entries have an action: hold the button
  for three seconds, re-scan the code, nothing at all.
- **Name the consequence when there is one**, including an unwelcome one. A release note
  that only lists improvements is one a reader learns to skim.
- **Say which half it is**, CAL or App, when that changes what a reader does. CAL is the
  loader that runs before anything else and handles setup and recovery; the App is what
  draws the cards.
- **Leave out anything only a maintainer can act on.** Refactors, test coverage and internal
  renames belong in the commit, which is linked from the release anyway.
