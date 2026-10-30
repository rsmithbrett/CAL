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

Nothing yet.

---

## v2026.10.30.0001

### A display can show the household's own photographs — App

A family uploads pictures on their own page and ticks which ones they want up. The
display draws those, one at a time, in with the rest of its cards. Nothing is
configured on the device and an operator never picks the pictures.

The card appears only once somebody has added it to that display's rotation, so
installing this release changes nothing on its own.

A display with no SD card holds one photograph in memory at a time and moves to the
next one each time it refreshes, so the set still cycles. A picture that cannot be
loaded says so on screen for its turn rather than leaving the previous card up.

### The Panellan ZX2D80CE02S has firmware to install — CAL and App

Every release until this one carried images for the ESP32-2432S028 only, so a
Panellan panel had nothing it could be given. Both boards build now and both
appear in every release.

The Panellan's files carry the board in their names and the ESP32-2432S028's are
unchanged, so an existing procedure, bookmark or script keeps working. For a
Panellan being set up from scratch over a cable, use
`CAL.ino.merged.panellan-zx2d80ce02s.bin`; for an ESP32-2432S028, keep using
`CAL.ino.merged.bin`. The checksums below name all sixteen files.

One thing to know before reaching for a cable: the wired install page's flashing
step was worked out against an ESP32 behind a USB-serial bridge chip, and the
Panellan is an ESP32-S3 with native USB. Nobody has measured its bootloader-entry
and reset sequence yet, so that page is not the route for this board today. Until
somebody does, a Panellan takes its loader from esptool by hand:

```
esptool.py --chip esp32s3 write_flash 0x0 CAL.ino.merged.panellan-zx2d80ce02s.bin
```

---

## v2026.10.26.0004 and v2026.10.26.0005

One piece of work over two releases. The first three entries shipped in 0004;
the fourth is 0005, which went out as soon as the restart was spotted.

### An airline logo reaches displays that have no memory card — App

Two displays in service have no memory card, and the aircraft card's airline
logo was only ever read from one. Those two showed the flight, the distance
and the route with a blank space where the logo goes, every single time. They
now pull the logo over the network and draw it the way they already pull their
picture cards, so it appears with no card in the slot. It is fetched again on
each refresh rather than kept, which is what having nowhere to keep it means.

### The airline logo fits a display standing on its end — App

On an upright display the logo was drawn off the right-hand edge: a square
airline mark missed the glass entirely, and a wide one showed a sliver of its
left edge jammed against the border. It is now placed from the display's own
width and sits inside the right margin either way, at full size.

Upright, it also moves to a line of its own above the airline name instead of
sitting beside it. Side by side there was only room for about half a name -
"Southwest Airlines" came out as "Southwe..." - and an upright display has
empty space below the card to spend instead. The name now reads in full with
the logo above it. Displays lying flat keep the layout they have.

A sighting whose airline has no logo on file no longer holds that space empty.
The name gets the whole width on any display.

### A logo that will not draw no longer restarts the display — App

A display restarts itself after a run of pictures it could not draw, which is
how it recovers from running out of memory. A logo that would not draw counted
toward that run every time the aircraft card came round, and on the two
displays with no memory card nothing else ever reset the count, so the display
would have rebooted every half hour for a picture. It now tries a logo twice
at most and then leaves the space empty.

### A logo the server has replaced reaches a display that stored the old one — App

Every airline logo was re-encoded into a form these displays can draw after
startup. A display that had already stored the old version kept using it,
because a stored image was checked for being there and never for being
current, so it went on failing to draw the same picture every few minutes. A
small stored image that will not draw is now dropped once per startup, and the
next fetch brings whatever the server holds now.

---

## v2026.10.26.0003

### A price drop says what the price dropped from — App

A card headed "Price drop" showed one number, so there was no way to tell what
the price used to be. It now carries a second, smaller line under the asking
price: "was $465,000".

The line appears only when the email that brought that listing was a reduction
and named both figures. A property that has held the same price for months
shows nothing extra, and nothing on the display works out that a price moved -
it draws what the email said, and stops drawing it when a later email says
something else. On a display lying flat with a button on the card there is no
room for a second line, so the asking price keeps the space and the "was" is
left out.

---

## v2026.10.26.0001 and v2026.10.26.0002

Two releases of one commit. The second carried a one-line correction to a
reference table and nothing a household sees.

### The forecast card shows the whole town name again — App

With a button on the card, the town was cut to a few letters and a dot:
"An..." where "Annapolis, MD" belongs, and the same for Severna Park and Fort
Washington. The town gets the room back. On a display standing on its end the
card keeps the layout it has without a button, because there was room there all
along; lying flat, the "Updated" in front of the time is what gives way, so the
line beside the town now reads "59 min ago".

### The line across the forecast card no longer cuts through the words above it — App

The thin rule that separates today's weather from the week ran straight through
the bottom of "Updated 59 min ago". It sits lower now, clear of the text, and
the week below it moved down with it into space that was already empty.

### The sun card draws the dot and the arrows it was missing — App

"SUN · TODAY" came out as "SUN  TODAY" with a gap in the middle, and the up and
down arrows in front of SUNRISE and SUNSET did not draw at all — they left the
sunrise label pushed sideways from the time under it. The heading reads
"SUN | TODAY" now and the labels line up with their times. The moon card's
heading and the sports card's period-and-clock line had the same gap for the
same reason and are fixed the same way.

---

## v2026.10.25.0009 and v2026.10.25.0010

### The moon card says when the moon comes up next — App

One line at the bottom of the card, and it's whichever comes next: `Moonrise
8:14 PM` or `Moonset 6:02 AM`. Both orientations show it. On the rare day with
no moonrise and no moonset at all the card says so in words instead of showing
a blank time, and a display whose location the server can't work out yet draws
the card exactly as it did before, with no empty line on it.

Nothing to set up. The time arrives with everything else the display already
asks the server for.

---

## v2026.10.25.0008

### A display standing on its end pages left and right — App

Tap the left side of the screen to go back and the right side to go forward,
the same as a display lying flat. It used to page on the top and bottom edges
instead, which nobody expects and which nothing on the screen told you about.
The side strips are narrower on a display standing on its end, so the card
still has the middle half of the glass to itself, and the small arrows at the
edges now point at the strips that do something.

### The moon card fits a display standing on its end — App

Turned on its end, the card used to cut off almost everything it said: the
heading, the name of the phase, and all four of the phases coming up. It shows
a big moon now with the phase under it, how much of it is lit, and today's
date. The four upcoming phases stay on a display lying flat, where there's
room for them.

### A long town name no longer prints through the temperature — App

On the forecast card the town sat on the same few rows of pixels as the big
number under it, so anything longer than about "Annapolis, MD" ran across the
72. The town gets its own row now and the number starts below it. This
happened whichever way the display was turned, not only on one standing on its
end.

---

## v2026.10.25.0007

### A display that can't draw a picture stops restarting itself — App

A display that couldn't open one of its pictures restarted itself to try
again, and then kept doing it: one display restarted 31 times, roughly every
twenty minutes, for a picture that was never going to open on it. It tries
three times now and then stays on. It keeps showing every card it can draw
and keeps checking in, so whoever set it up can see what's wrong instead of
watching it reboot all day. If the picture starts working again, the display
gets its three tries back.

The reason it gives for those restarts changed too. It used to say it was
short of memory, which sent people looking in the wrong place — the display
had plenty. It now says it couldn't draw a picture, which is what happened.
Restarts recorded before this release still read the old way.

---

## v2026.10.25.0006

### A house that isn't shown no longer costs the details — App

On a display standing on its end with a button on the card, the house was
left off and two lines of detail went with it, so the card said less than it
did before it could show houses at all. It measures the space first now. If
the house fits, the card draws the house and one line of detail; if it
doesn't, the card draws all three lines.

Same answer when a picture won't open. On a display standing on its end the
details come back into the space the house was going to take, and on one
lying flat the words spread back across the whole card instead of sitting in
a narrow column beside a gap.

### A picture stays put when a display is short of memory — App

A display with no room to open a picture threw its copy away and fetched it
again, and the new copy failed the same way next time that card came round.
It keeps the copy now and tries again later, so a display in that state stops
downloading the same house over and over.

---

## v2026.10.25.0005

### The home value card fits a turned panel — App

On a display standing on its end, the estimate's range read "Rang$325K -
$904K": the word and the figure were drawn on top of each other. They now
share the row by measuring what each needs, so the label reads "Range" and
the figure is whole.

---

## v2026.10.25.0003

### A turned panel shows the house — App

A display standing on its end drew the words of a property alert and no
picture at all. It draws the house now, across the bottom, under the address
and the price.

Most of these cards have been seen on a turned panel: 8,019 of the 10,762
times this card has been on screen. So "no picture" was the usual way it
looked, not a corner case.

A card carrying a picture now shows one line of detail rather than three. The
bedrooms and the price stay; bathrooms and living area come off, because they
were what left no room for the house. A card with no picture still shows all
three.

A line of detail also measures its own number and gives the label whatever is
left, so "Bathrooms" no longer reads "Bathroo...".

---

## v2026.10.25.0002

### A featured listing price reads as money again — App

A price on a featured listing card showed as '$514000'. It now reads
'$514,000', the same as every other price the display draws.

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
