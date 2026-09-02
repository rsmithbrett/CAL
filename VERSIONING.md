# Versioning

Every published release is tagged `vYYYY.MM.DD.NNNN`:

- **Major = the 4-digit year.**
- **Minor = the 2-digit month.**
- **Patch = the 2-digit day.**
- **Quick Fix (QFR) = a 4-digit, zero-padded counter**, starting at `0001` for
  the first release cut on that calendar date. A second release published the
  same day - an urgent same-day fix - is `0002`, and so on.

There is nothing to edit and no version to decide by hand: the date comes
from the clock CI runs on, and the counter comes from how many releases
already exist for that date. Pushing to `main` is the entire ceremony.

Every push publishes two things:

- The **`latest`** release, reused (not recreated) on every push, so its
  download URLs never change. This is what a device-setup page should always
  link to.
- A new, **permanent** `vYYYY.MM.DD.NNNN` release with the same artifacts.
  That tag is never reused or overwritten by a later push - it is the
  historical record of exactly what shipped as that release, which `latest`
  by its nature cannot be, since `latest` always points at whatever main most
  recently built.
