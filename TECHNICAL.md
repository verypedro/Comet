# Technical notes

## Lineage

Comet is a ground-up UI rebuild on top of the engine originally built
for **MPOid**, a simpler screenshot→MPO converter. The screenshot
scanning, BMP decoding, MPF/EXIF-tagged MPO writing, background-
threaded texture loading, and the verified Morton/Z-order GPU texture
swizzle all carry over unchanged. What's new in Comet is the
interaction model: a grid instead of a list, batch delete, filtering
by 2D/3D/date, and a visual pass aimed at feeling as polished as
Nintendo's own Switch Album app.

## Hardware quirks worth knowing

- **GPU_RGBA8 stores bytes as A,B,G,R in memory**, not R,G,B,A, on this
  little-endian hardware — a real, verified hardware quirk, not a
  guess. Get it wrong and everything renders with a pink tint.
- **Texture data must be pre-swizzled into 8x8 Z-order tiles** before
  upload; citro3d does not do this for you. The swizzle table is taken
  directly from the [Citra](https://github.com/PabloMK7/citra)
  emulator's own `video_core/utils.h`, since it has to be byte-exact
  to render real commercial games correctly.
- **Background-thread loading**: a worker thread does file I/O + BMP
  decode + swizzle (pure memory/math, no citro2d/citro3d calls, which
  aren't safe to call from multiple threads); the main thread does only
  the fast part (texture allocation + one bulk memcpy). The worker runs
  at a *higher* priority than main (`0x25` vs `0x30`), since without
  New3DS core entitlements it shares the same single CPU core as main
  and needs to be favored by the scheduler to stay responsive.
- **`bmp_load()` reads the whole pixel block in one `fread()` call**,
  not one per row — per-call filesystem overhead multiplied by ~240
  rows was a real, measurable cost.
- MPO files need a proper MPF (Multi-Picture Format) APP2 segment
  declaring the stereo pair relationship, plus EXIF DateTime and a
  JFIF header — the 3DS Camera app won't recognize a file as a 3D
  photo without them. Plain 2D screenshots export as ordinary `.JPG`
  with the same EXIF/JFIF headers minus the MPF segment.
- **Icons are pre-swizzled at build time, not runtime.** The button
  glyphs, badges, and header icon are processed once (see
  `process_icons.py`) into `source/assets/icons_data.c` — the exact
  same swizzle format as screenshot textures, just computed ahead of
  time from source PNGs instead of decoded from the SD card. Unlike
  screenshots, these preserve real per-pixel alpha rather than forcing
  full opacity, since they're alpha-blended UI elements, not photos.
- **Custom fonts are Nintendo's bitmap format (BCFNT), not TTF.**
  citro2d can load a custom font via `C2D_FontLoad`, but not a raw
  `.ttf` — `mkbcfnt` (part of the `tex3ds` package) converts Poppins
  ahead of time. If the converted `.bcfnt` files are ever missing,
  `C2D_TextFontParse(..., NULL, ...)` is citro2d's own documented
  fallback to the system font, so a missing font never crashes
  anything, it just silently renders differently.

## DS screenshots (nds-bootstrap)

- nds-bootstrap stores DS captures in a single **uncompressed TAR** at
  `sd:/_nds/nds-bootstrap/screenshots.tar` with exactly **50 fixed
  slots**. Unused slots are still present as full-size entries, written
  as **entirely 0x00 bytes** -- so a slot holds a real screenshot iff
  its payload starts with the `BM` signature. Verified against a real
  tar: 50 entries, every one 98374 bytes.
- Because TAR is uncompressed, individual screenshots can be read
  straight out of the archive by seeking to an entry's payload offset
  -- no extraction to disk, no decompression.
- **DS captures are 16-bit RGB565 BMPs** (256x192, BI_BITFIELDS),
  where Luma's 3DS captures are 24-bit BI_RGB. `bmp.c` handles both.
  The 565->888 expansion replicates low bits rather than plain-shifting,
  so white lands on exactly 255 instead of 248.
- **There is no capture timestamp anywhere in the format.** Filenames
  are bare slot numbers (`screenshot01.bmp`), and every tar entry
  carries the same hardcoded mtime (2021-07-29) regardless of when it
  was taken. Comet therefore stamps each screenshot with its *import*
  time -- which is why DS mode's date filter is labelled "By Extraction
  Date" rather than by capture date.
- Imported files are named `{importstamp}_slotNN.bmp`. The slot suffix
  matters: importing up to 50 files can easily finish inside one
  second, so a timestamp alone wouldn't be unique.
- Clearing **blanks every slot's payload in place** rather than
  deleting the file. Two reasons: nds-bootstrap has to rebuild the
  whole 50-slot archive from scratch if it's missing, which noticeably
  slows the next game launch; and blanking *all* slots (rather than
  picking individual ones out) sidesteps the counting quirk below.
  Verified on a real tar: 7 real screenshots -> 0, file size and all
  50 entry headers unchanged.
- **Don't blank individual slots.** Testing showed nds-bootstrap counts
  real screenshots by scanning from slot 1 and stopping at the first
  blank one, so a gap in the middle makes it lose track of everything
  after it. All-or-nothing is the safe pattern.

## Hidden shortcuts

- **L+R on the detail screen** duplicates the current screenshot
  (both modes), with a confirmation popup. Mostly a testing
  convenience for building up a large library quickly.
- **L on a DS screenshot's detail screen** toggles a widescreen
  stretch: DS widescreen patches render 16:10 content into the same
  256x192 buffer, so it looks squashed at native size. The stretch
  keeps the native 192px height and widens to 307px (192 * 16/10),
  point-sampled so it stays crisp rather than smeared.
- The widescreen flag lives on each `DSScreenshot` entry, not as a
  standalone variable -- it follows the specific file as you browse,
  rather than leaking onto whatever's currently selected.
- It also **survives rescans** (extract/delete/duplicate), via
  `ds_rescan_preserving_widescreen()`: before rescanning, it records
  the filenames currently marked widescreen; after, it matches by
  filename and restores the flag on whichever entries still exist.
  Matching by filename rather than array position matters because a
  rescan re-sorts newest-first, so a new import can shift every
  existing file to a different index -- matching by position would
  silently apply the flag to whatever unrelated file landed at that
  slot instead.
- It also **survives closing the app** -- Comet's first-ever persisted
  setting. `sd:/3ds/Comet/widescreen.txt` holds one full path per line
  for every widescreen-marked screenshot, rewritten on every toggle.
  `load_widescreen_prefs()` reads it once at startup, before any scan
  has run; the same filename-matching restore logic above then applies
  it to whatever gets scanned. One ordering detail worth knowing:
  `ds_rescan_preserving_widescreen()`'s capture-from-memory step is
  skipped when `s_dsCount == 0` (nothing scanned yet), since
  unconditionally running it on the very first scan would immediately
  overwrite the disk-loaded state with an empty list before it ever
  got used.

## DS mode tabs (nds-bootstrap / SD Card)

DS mode browses two separate libraries via a tab bar: screenshots
still sitting in nds-bootstrap's `screenshots.tar`, and ones already
extracted to `sd:/3ds/Comet/ds_screenshots/`. They get entirely
separate backing arrays -- tar items share one file at different byte
offsets, extracted ones are ordinary standalone files -- so a single
list carrying both shapes would have been messier than two.

- `item_top_path()` / `item_data_offset()` are the seam: for the tar
  tab, every item returns the *same* path (the tar) with a different
  offset. `bmp_load_at()` and the background loader both take that
  offset, so tar screenshots render in the grid and preview without
  ever being extracted.
- L/R switch tabs; L+R together still exits to 3DS mode. Switching to
  an empty tab is blocked (it would just show an empty grid).
- **Start is repurposed as Extract All**, but only on the tar tab's
  plain browse screen -- everywhere else it still exits the app, which
  is the 3DS homebrew convention. The global Start-to-exit check is
  conditional on exactly that state.
- The tar tab has no Filter option: `item_timestamp()` returns `""`
  there, because nds-bootstrap records no per-shot capture time at all
  (every tar entry shares one hardcoded mtime). Batch Select takes
  Filter's footer slot, and Extract All fills the freed one.
- The More screen gains a third option (Extract Screenshot) on the tar
  tab, shifting Copy/Delete down one. The dispatch normalises this to
  a shared action id rather than branching on raw menu index.

### Widescreen marks on tar screenshots

Tar slots have no persistent identity to key a preference against --
slot 3 today may hold a different screenshot tomorrow, since deleting
compacts everything above it down. So tar widescreen marks are keyed
on `ds_slot_fingerprint()`: a hash of four 128-byte spans sampled
through the payload, folded together with the payload size. The
fingerprint travels with the *image*, wherever it ends up in the
archive. Two byte-identical screenshots would collide, but they'd be
visually identical anyway, so treating them the same is harmless.
(Verified distinct across a real 7-screenshot tar.)

Extracted (SD Card tab) screenshots don't need this -- they're
ordinary files with stable paths, so those marks key on filename.

### Exporting from the tar

Copying an nds-bootstrap item to the 3DS Album reads it with
`bmp_load_at()` at its tar offset -- `bmp_load()` would start at byte
0 of the archive and hit the tar's own header block, which is what
produced the "bad signature" error. If the item is marked widescreen,
the stretch is baked into the pixels (`stretch_to_widescreen`, 192px
height -> 307px width, point-sampled) *before* encoding, so the
exported file matches what the preview showed.

### Deleting tar slots in batch

`ds_delete_tar_slot()` compacts survivors down into slots 1..N after
each removal, which renumbers every index *above* the one removed.
Batch delete therefore walks the selection **highest index first** --
forward order would delete the wrong slots after the first one.
(Verified by simulation: deleting indices 1 and 3 of A,B,C,D,E
forwards removes B and E instead of B and D.)

### Whole-pixel icon placement

`draw_icon`/`draw_icon_tinted` round their destination to whole
pixels. Footer icons otherwise land on fractional coordinates -- the
Y is `213 + (27-12)/2 = 220.5`, and X accumulates from measured text
widths -- and linear filtering then samples between texels, leaving a
faint ghost row under the glyph. It showed under Select on the
nds-bootstrap tab specifically because that tab's different preceding
labels put it on a different subpixel offset than elsewhere.

## Grid navigation with a ragged last row

When the item count isn't a multiple of `GRID_COLS`, the last row has
fewer items than a full row (e.g. 14 items in a 4-column grid leaves
2 in the last row). Pressing Down from a column with nothing directly
below it -- but where a next row genuinely exists -- snaps to the last
available item rather than refusing to move, which previously made
those columns unable to reach the last row via Down at all.

This has to distinguish two cases that both compute an out-of-range
`next` index: "a row below exists, but it's ragged" (snap to the last
item) vs. "already in the last row, no row below at all" (don't move).
Comparing current row to total row count is what tells them apart --
comparing only against `s_visibleCount` isn't enough, since being
already-in-the-last-row also fails that check. (Caught this exact
distinction via simulation before shipping it: an earlier version of
the fix moved the cursor sideways within the last row when Down was
pressed from its first item, which was a regression on its own.)

Single shared fix in `APP_BROWSE`'s input handling, so it covers 3DS
mode, DS mode (both tabs), and Batch Select all at once -- they all
share this code path and `GRID_COLS`.

## Filter behaviour

- Every level of the date filter (Year, Month, Day) has an explicit
  "All" option. Year = All means no date narrowing at all, and hides
  the Month/Day rows entirely -- there's no year for them to narrow
  within. This is also the only way to clear a filter in DS mode,
  where By Date is the whole filter UI.
- Entering By Date defaults Year to the most recent available, but
  only when a date filter isn't already active -- otherwise picking
  "All" would get silently undone the next time the screen opened.

## License details

Comet's own source code is MIT-licensed — see `LICENSE`.

Two bundled things carry their own separate licenses and aren't
covered by the MIT grant:

- **Poppins** (`romfs/Poppins-*.bcfnt`, converted from the original
  `.ttf`) — [SIL Open Font License](https://openfontlicense.org/).
- **`source/stb_image_write.h`** — public domain / MIT dual-licensed,
  from [nothings/stb](https://github.com/nothings/stb). Not included
  in this repo; downloaded fresh from its source at build time per its
  own preferred distribution method (see BUILDING.md).
