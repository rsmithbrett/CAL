# Versioning

`VERSION` holds four numbers: `MAJOR.MINOR.PATCH.QUICKFIX`.

- **Major** — larger features, or an operating system, architecture, or
  security change.
- **Minor** — released periodically, contains new features, no significant
  architectural change. Inclusive of the prior minor release of the same
  major, and of the patch release available one month prior.
- **Patch** — an accumulation of tested and fixed defects issued between
  minor releases. Defect relief and urgent security fixes only.
- **Quick Fix (QFR)** — immediately addresses a critical defect or security
  issue, intended for use on a specific version rather than a general release.

To ship a numbered version: edit `VERSION`, commit, push to `main`. Nothing
else - no build command, no release command. CI does the rest:

- Every push builds CAL and republishes it to the **`latest`** release, which
  is reused (not recreated) on every push, so its download URLs never change.
  This is what a device-setup page should always link to.
- A push that changes `VERSION` additionally publishes a **new, permanent**
  release tagged `v<the new VERSION>` with the same artifacts. That tag is
  never reused or overwritten by a later push - it is the historical record
  of exactly what shipped as that version, which `latest` by its nature
  cannot be, since `latest` always points at whatever main most recently built.
