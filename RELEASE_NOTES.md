# Release notes

**What changed for whoever uses the device — not what changed in the code.** Every entry
answers "what will somebody notice, and what should they do about it". A reader of a GitHub
release is deciding whether to install this and what to expect afterwards; a summary of
which function moved tells them neither.

Both CAL and the App ship from one release (see `VERSIONING.md`), so both appear here, and
an entry says which one it is wherever that changes what a reader does.

Keep **Unreleased** at the top. The build writes whatever is under it into the release body
above the checksums, so an entry written here before the merge is the entry that ships.

**And empty it once it has shipped.** Nothing clears this section automatically, so entries
left here go out again on the next release, and the one after that. `v2026.10.18.0001` and
`v2026.10.22.0001` published byte-identical prose for exactly that reason: the only
difference between the two release bodies was the four checksum lines, so the later one
described work that had gone out four days earlier and said nothing about what was in it.
Publishing is two steps, not one. Write the entry before the merge, then move it under a
dated heading after.

---

## Unreleased

### The setup screen follows along, and a tap brings the code back — CAL

While somebody sets a display up, the screen now says where they have got to:
the code to scan, then that their phone has joined, then that the setup page
is opening, then that it is open and ready to fill in.

It also stopped saying "Connected" after a phone had gone. It was reading a
list the display keeps of who is joined, and a phone that goes to sleep or
walks away stays on that list for a while. It now listens for the phone
leaving, which happens straight away.

And touching the screen puts the code back, whatever it was showing. If the
screen is ever wrong, that is the way out.

The writing on the white screen is near-black rather than blue, which is
easier to read across a room on this panel.

### The display says when your phone has joined — CAL

While a display is waiting to be set up it shows the code to scan. The moment
a phone joins, the screen turns white and says so, in large blue letters, so
somebody looking up from their handset can tell the join worked from across
the room. If the phone drops off, the code comes back.

The setup form also reads in the order people fill it: the network, then its
password, then the box for a network that was not in the list. The password
sat under that last box before, where it looked like it belonged to it.

And the setup network's own key is printed under the code. The code already
carries it and a camera fills it in, but a scan that did not work left nothing
to type.

### The setup code names the display again, and the network joins — CAL

The last build advertised every display as "Discover-Setup-0000" instead of
naming it, and a display could not join a household network afterwards even
with the right password. Both came from the same change and both are fixed.

### The setup network stops getting worse the longer it is open — CAL

A display that had been given a network name it cannot reach kept trying to
reach it while the setup screen was up. Both share one radio, so the setup
network got less reliable the longer somebody spent on it: a phone that held
the connection for three minutes early on was dropping it after four seconds
later in the same session.

The display now leaves that alone until setup is finished. It changes the
permanent CAL loader, not the downloadable App.

### Why a phone could not join, in the log — CAL

The display recorded that a phone arrived or left and nothing about why. It now
records the reason the radio gives, so a refused password can be told apart
from a phone that simply moved away.

This changes the permanent CAL loader, not the downloadable App, and adds no
behaviour: the setup network and the setup page work exactly as before.

### The setup network is protected again, as a bench test — CAL

The temporary network a new display puts up while you set it up has had no
password since 4 October. It has one again: a random key, different on every
display, kept so the phone never holds a stale one. The QR code carries it, so
there is still nothing to type — scan, tap Join, and the setup page follows.

This is being measured rather than assumed. The password was taken off in
October because it was thought to be slowing the phone down, and that was never
actually timed. The display now records when a phone joins, so the two can be
compared on the same bench with the same handset.

### The code on the waiting screen opens the right page — CAL

A display waiting to be set up showed a code that, when scanned, gave a camera
nothing but a hardware address. It now opens the page where that display is
claimed, when the server supplies the address; where it does not, the screen is
unchanged.

Both change the permanent CAL loader, not the downloadable App, and neither has
run on hardware. To reflash over a cable, follow the wired install at
https://discoveraroundme.com/cal

---

## v2026.10.24.0002

### Two changes aimed at the Wi-Fi setup form appearing by itself — CAL

When a new display shows its Wi-Fi setup code, a phone that joins is supposed
to pop the setup form up on its own. On a recent iPad it did not: the display
answered every request the iPad made, in a few milliseconds each, and the form
still never appeared. The address printed under the code worked when typed by
hand.

Two things change in how the display answers, both aimed at that.

The display's redirect now carries a short page rather than an empty one, and
the network it hands out now names the setup address outright instead of
leaving the phone to work out that there is one.

Neither has been run on hardware. This changes the permanent CAL loader, not
the downloadable App, so bench-test it before reflashing a deployed display.
To reflash over a cable, follow the wired install at
https://discoveraroundme.com/cal

### A clearer enrollment screen — CAL

The screen a display shows while it waits to be set up printed its own address
twice, and cut the message off mid-word. It prints the address once and gives
the message the room it needs.


---

## v2026.10.24.0001

### A new display shows its setup form again — CAL

v2026.10.23.0001 left an iPhone that scanned the Wi-Fi setup QR joined to the
display's setup network with no form to fill in. The form comes back in this
release.

The setup address printed under the QR code, and the network name beside it,
were drawn grey and blue on a blue panel, which made the two things somebody
has to read off the screen the two hardest to read. Both are dark now.

This changes the permanent CAL loader, not the downloadable App. Neither fix
has been run on hardware, and a loader that misbehaves can't fix itself, so
bench-test it before reflashing a deployed display. To reflash over a cable,
follow the wired install at https://discoveraroundme.com/cal

### An aircraft card no longer says "Other" where an airline should be — App

A flight whose airline isn't in our directory used to draw the word "Other"
where the carrier's name goes. "Other" describes a gap in the list rather than
anything about the flight, so the card now puts the callsign in the headline
and leaves the airline line out. A small private aircraft still says
"Private", because that tells you something real.

Fifteen carriers were added at the same time, so a number of flights that used
to fall into that gap now show their airline by name.

### Aircraft cards hold the screen for the right length of time — App

The display used to work out "is this plane close enough to linger on" from
the search radius. Widening the search therefore made the aircraft card take
the long twenty-second dwell far more often, which pushed every other card
down the rotation. The server now says what counts as close, separately from
how far it looks, and the display uses that.

A display on older firmware keeps working the old way, so nothing has to be
updated for this to be safe.

---

## v2026.10.23.0001

### Connecting a new display to Wi-Fi is easier to finish — CAL

When a new display shows its Wi-Fi setup QR, an iPhone no longer has to wait
for the display to scan all nearby networks *after* the phone has connected.
The display prepares the network list first, before it advertises its temporary
setup network, so the configuration page can load straight away.

If the iPhone joins the setup network but doesn't open the setup form
automatically, the display now shows a direct browser address underneath the
QR code. Open that address on the phone while connected to the setup network.
A hidden or missing home Wi-Fi name can also be entered manually.

This changes the permanent CAL loader, not the downloadable App. To reflash a
display over a cable, follow the wired install at
https://discoveraroundme.com/cal

**This release also left the setup form unreachable on an iPhone**, which
v2026.10.24.0001 fixes. A display still on this loader needs that one to
finish Wi-Fi setup from a phone.

---

## v2026.10.18.0001 and v2026.10.22.0001

**Both tags carry these same entries**, because the section above was never emptied after
the first of them. Recorded once here against both rather than duplicated. What genuinely
differed between the two builds is the binaries, and each release's own checksums say so.

### Displays stopped restarting themselves every couple of minutes — App

A display showing a scoreboard, a listing or any card with a picture on it was rebooting
roughly every two minutes, over and over, and had been since the 10th. It's fixed, and the
fix is the one a household will notice first: the card you were looking at stays on screen.

What was happening was that the memory a picture needs has to be one unbroken run, and
drawing a card was leaving that memory in small pieces rather than one piece. The next card
couldn't find a run big enough, gave up, and the display restarted itself rather than sit
there blank. A bench display that had been rebooting every two minutes ran for just under
four hours after this went on.

Nothing to do. A display that has been cycling will settle on its own once it takes this
update.

### A display can be mounted tall instead of wide — App

A display can now be turned ninety degrees and mounted taller than it is wide, and the
cards lay themselves out for whichever shape it is. The two shapes are called **profile**
(tall) and **landscape** (wide), and which one a display uses is a setting on the website,
sent to the device at its next check-in and remembered through a power cycle.

Paging works on whichever pair of edges is the long one, so a profile display pages by
tapping its top and bottom rather than its sides, and the small arrows that mark the live
edges move with it. The clock and the button row measure up from the bottom edge, wherever
that is.

Changing a display's shape after it's mounted is fine, but it needs one check-in to arrive
and one more to redraw, so give it a couple of minutes before deciding it didn't work.

**Which way up a profile display draws is settled on hardware now.** If you have been
running a pre-release build on a profile display, the picture was upside down on it — every
card inverted, while landscape displays were unaffected. Nothing to do but take this update,
and the first redraw after it installs comes up the right way round. Taps were never wrong
and are not changed by the correction: the paging edges follow the picture rather than the
panel, so the top of what you see has always been the top it responds to.

**The cards themselves are still catching up.** The chrome, the paging and the pictures are
right in profile; several cards still lay their text out for a wide panel, so expect some
crowding on the sun, moon and forecast cards until those are redrawn. Landscape is
unaffected.

### The sun, moon and tide cards look like what they're about — App

Sunrise and sunset now sit on a drawn sky with the sun or moon in it and hills along the
bottom, and the tide card is drawn as water with a dial that fills and empties. Both have a
day version and a night version and switch between them on their own.

The clock on these three cards is top right rather than bottom right, because the bottom of
each is now part of the picture.

### One scoreboard for every sport — App

The scoreboard card handles every league the same way and says which sport and which
competition it's showing, instead of a separate card per sport that didn't. Up to four games
fit on screen at once, and a game that hasn't started shows its start time where the score
would be rather than a pair of zeroes.

The website's reports now count each game on a scoreboard as its own showing, so a card with
four games on it reports four rather than one, and the seconds on screen are counted once
across the four rather than four times over.

### A tap while the card is redrawing isn't lost any more — App

Pressing a button or an edge during the second or so a card takes to draw, or during a
check-in, used to do nothing at all: the press landed in a gap where nothing was watching
the glass. Touch is now read on its own schedule and a press that arrives mid-draw is still
there when the draw finishes.

If a display has felt unreliable to the touch, this is why, and it needs nothing from you
beyond the update.

### The clock card shows the date again — App

The clock card was drawing the date on top of itself and the time twice. It now drops the
date to a smaller size when the long form doesn't fit, rather than overlapping.

### A display that can't install an update stops trying — App

A display handed an update it can't install used to retry it forever, which on a device with
a weak signal meant it spent all day downloading and never drew a card. It now tries three
times, then stops and says so on the next check-in, and carries on showing cards on the
version it already has.

The website shows the refusal, so a display stuck this way is visible rather than silently
busy.

### A display keeps its last few minutes of log even if nobody's listening — App

Turning on the debug stream for a display used to show only what happened from that moment
on, which was no use for anything that went wrong at startup — the interesting part was over
before anyone could ask for it. A display now keeps its last 32 lines whether or not the
stream is on, so switching it on shows what just happened as well as what happens next.

### A display that loses its WiFi waits for somebody to walk over — CAL

A display whose network has changed used to retry forever with nothing on screen worth
reading. It now says what's wrong and what to do, and holds that up rather than cycling.

### A button on the back puts the clock up — App

Pressing the physical button jumps straight to the clock card, from whatever was on screen.

---

## v2026.10.08

These four shipped in every release from v2026.10.08 onward.

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
- **Move the entry under a dated heading once it has shipped.** This is the step that gets
  skipped, and skipping it is invisible: the release publishes, it reads correctly, and the
  next one repeats it. Checking takes a moment - compare the published body against the
  previous release, and identical prose means the roll was missed.
