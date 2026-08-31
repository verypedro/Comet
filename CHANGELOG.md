# Changelog

## v3.1

Performance and a small UX cleanup, no new features.

### Faster

- Deleting a 3DS screenshot is significantly faster on a large library
  than it had become. Fixed a scan that was hitting the SD card far
  more than it needed to on every delete, made deleting keep the
  thumbnails that didn't actually change instead of rebuilding all of
  them, and stopped a background load from happening speculatively on
  every navigation when it's usually never needed
- Faster startup: sound effects now load only when actually needed,
  instead of all of them being read every time the app opens

### Changed

- Merging no longer shows a confirmation popup on success. Landing on
  the grid with the new merged screenshot's thumbnail already visible
  is confirmation enough

## v3.0

### Nexus3DS support

[Nexus3DS](https://github.com/2b-zipper/Nexus3DS) is a Luma3DS fork
that names and organises screenshots differently. Comet now handles
both differences, alongside plain Luma screenshots on the same card:

- **Filenames with a title ID.** Nexus3DS puts the running game's
  title ID between the timestamp and the `_top` / `_top_right` /
  `_bot` suffix. Comet reads these correctly now, both for matching a
  screenshot with its 3D and bottom-screen companions and for the
  Year to Month to Day filter.
- **Date folders.** Nexus3DS can optionally save each day's
  screenshots into their own dated subfolder. Comet scans those as
  well as the usual flat folder.

### Merge Top/Bottom Screens

A new option on any 3DS screenshot's More screen, shown when there is
a bottom screen capture to merge. Combines both screens into a single
400x480 image and adds it to your gallery as its own entry, with a
dedicated badge so it is not mistaken for a plain 2D screenshot. No
stereo 3D. Similar in spirit to
[screenshot-merge](https://github.com/ihaveamac/screenshot-merge),
just done on the console instead of on a PC.

The merged file is saved next to the original, following whatever
naming and folder convention that screenshot already used, whether
that is plain Luma or Nexus3DS.

### Everything else

- Every screenshot's More screen now shows its full file path, useful
  now that screenshots can come from Luma's folder, a Nexus3DS date
  folder, inside nds-bootstrap's tar, or Comet's own extracted folder
- Move between screenshots directly from the More screen with Left/Right,
  no need to back out to the grid first
- New badges distinguishing nds-bootstrap tar screenshots from ones
  already extracted to SD, plus a dedicated badge for merged screenshots
- Redrawn badges throughout, fixing some stray white pixels visible on
  3DS XL screens
- Copying to the Album, extracting from the tar, and merging now all
  keep you where you were instead of dropping back to the grid
- Deleting a screenshot is now more thorough about removing its
  companion files, including inside date folders
- Fixed a few cases where sound effects didn't play, or played without
  the corresponding action actually happening, particularly around
  touch controls

## v2.0

Everything notable added since the `v1.0` GitHub release, which only
covered browsing/copying/deleting 3DS screenshots. This update's
headline addition is full DS screenshot support.

## DS Mode

A second library alongside 3DS screenshots, toggled with **L+R** from
the main grid. Browses, filters, previews (in real stereo where
applicable — DS obviously isn't 3D, so this just means the native
top-screen display), copies to the 3DS Camera album, and deletes DS
captures taken via nds-bootstrap/TWiLightMenu++, with the same
grid/filter/batch UI 3DS screenshots already use.

## Reading nds-bootstrap's screenshots.tar directly

nds-bootstrap keeps its screenshots in a single 50-slot
`screenshots.tar` rather than individual files. Comet reads this
directly — no manual extraction needed first:

- **Two tabs**: `nds-bootstrap` (still in the tar) and `SD Card`
  (already extracted to `sd:/3ds/Comet/ds_screenshots/`). Switch with
  a single L or R press, or by tapping a tab.
- Screenshots in the tar can be **viewed, filtered, copied to the
  Album, and exported individually or in bulk** without ever leaving
  Comet.
- **Extract All** (Start, from the nds-bootstrap tab) pulls everything
  out in one go, with the option to clear the tar afterward so
  nds-bootstrap has room to keep capturing.
- Clearing is done by **blanking the tar's slots in place** rather
  than deleting the file outright — nds-bootstrap would otherwise have
  to rebuild the whole 50-slot archive from scratch, which noticeably
  slows the next game launch.
- **Deleting a single screenshot compacts the tar** so the remaining
  ones stay contiguous from slot 1. This matters because nds-bootstrap
  counts its screenshots by scanning until the first empty slot —
  without compaction, deleting one from the middle would make
  nds-bootstrap think its library ends there, and it would start
  silently overwriting real screenshots after it on the next capture.

## Widescreen support

Some DS games have unofficial widescreen patches that render 16:10
content into the native 256×192 buffer. Comet can stretch these back
out for viewing and export, per screenshot, from the More screen. The
setting is remembered — including across app restarts and even after
deleting other screenshots from the tar, which required matching by
image content rather than file position, since the tar has no stable
per-slot identity once anything gets deleted.

## Everything else

- **Duplicate** (hidden L+R shortcut on the More screen) for quickly
  building up a test library, for both 3DS and SD Card DS screenshots
- Comet now remembers a handful of things across restarts — widescreen
  marks and whether you've seen the DS mode introduction — the first
  persistent settings of any kind
- Installable **.cia** build (HOME Menu icon, banner, and jingle)
  alongside the existing `.3dsx`, buildable with a single `make`
- A fixed-before-launch bug where opening the More screen right after
  moving the cursor could act on a different screenshot than the one
  currently shown on the top screen — the safety-relevant kind of bug,
  since it could have led to deleting the wrong file
- Sound effects only play when a button press actually does something,
  rather than on every press regardless of effect
- New icon set, banner, and app description ("Observatory of Luma
  Screenshots")
