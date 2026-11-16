# CAL

Published firmware for DiscoverAroundMe displays. Each release carries two
programs, built for two boards:

- CAL, the Client Application Loader, which lives in the factory partition.
- App, the application the display runs.

The boards are the ESP32-2432S028 and the Panellan. Panellan files carry
`.panellan-zx2d80ce02s` in their names; the rest are for the ESP32-2432S028.

## Release names

Each build is published as `vYYYY.MM.DD.NNNN`: the date it was built, then a
counter that starts at `0001` each day. Those releases never change.

The `latest` release always holds the newest build, and its download links stay
the same from build to build. To install over a cable, use
https://discoveraroundme.com/cal.

## Files in a release

- `CAL.ino.bin` and `App.ino.bin` are the images a display installs over the
  air.
- `*.merged.bin` is a complete flash image for a wired install.
- `*.bootloader.bin` and `*.partitions.bin` are the bootloader and partition
  table that go with them.
- `combined-checksums.txt` lists the SHA-256 of every file in the release.

The source code isn't published here.
