# Changelog

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
