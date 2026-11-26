# Changelog

Changes in this fork of MPC-HC (`SNBogdanov/mpc-hc`) relative to upstream
`clsid2/mpc-hc`. Newest first. The commit hashes refer to the fork history.

## [Unreleased]

### Added

- Trusted playlist duration: a fixed total time taken from MediaInfo or a saved
  playlist can be shown instead of the filter graph's own duration. (`09901cfd2`)
- Background probe that waits for a paused file to become readable again before
  the long-pause reload rebuilds the graph. (`d48d02707`)

### Changed

- Resume after a long pause now probes the file first and reloads only once it
  answers, so the rebuild no longer blocks the UI on a stale network session.
  (`d48d02707`)
- A repeated playlist is no longer stopped after five failed opens. Every item is
  tried once, and playback stops only when a whole pass fails to open, with an
  on-screen notice. (`d48d02707`)
- x64 LAV ffmpeg is linked statically, so no MinGW runtime DLLs are shipped.
  (`56cae05db`)
- The platform toolset is forced through `MPCHC_FORCE_TOOLSET`. (`56cae05db`)
- Ignore rules were added for local build output, tooling and settings backups.
  (`56cae05db`)

### Fixed

- A repeated playlist whose every item is unopenable no longer loops through the
  list indefinitely. (`d48d02707`)
- The seek bar, OSD and LCD ranges use the graph's own duration again; the
  trusted duration is only the status bar total time, so the ranges stay in step
  with the live position. (`d48d02707`)
- The wait cursor is held across a whole run of failed opens instead of being
  shown again for each item. (`09901cfd2`)

[Unreleased]: https://github.com/SNBogdanov/mpc-hc/commits/develop
