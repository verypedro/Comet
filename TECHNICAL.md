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

## Date-subfolder screenshots (Nexus3DS "save in date folders")

`fs_scan_screenshot_pairs` scans the root screenshots directory plus
one level of subdirectories -- Nexus3DS's optional date-folder layout
(`luma/screenshots/2026-08-27/`) puts each day's captures in their own
folder there. Subfolder names aren't validated as looking like a date
at all; any subdirectory found gets scanned the same way the root
does, so this doesn't depend on matching one exact naming convention
and stays correct if a different fork names its folders differently.
Only one level deep -- there's no legitimate reason for a screenshot
to be nested further, and bounding the recursion keeps the scan cost
predictable.

The scanning logic itself (`scan_one_dir`) is unchanged from before,
just now called once per directory instead of only for the root, and
each call constructs companion-file paths relative to *its own*
directory -- so a screenshot's `_top_right`/`_bot` companions are
always looked for in the same folder the `_top` file was found in,
never accidentally cross-referenced against the root or a different
date folder.

Verified end-to-end by compiling the actual scanner against a real
mixed directory tree (flat root-level Luma files alongside Nexus3DS
date-subfoldered ones, including the title-ID-embedded naming from
the fix above, nested inside a subfolder) and confirming every
screenshot was found, correctly paired, correctly scoped to its own
folder, and correctly sorted newest-first across all of it -- plus a
missing screenshots directory, an empty subfolder, and a stray
non-matching file all handled without incident.

## Crash fixed: dangling pointer in the detail menu labels

Real hardware crash (data abort, reading address 0x13) when opening
the More screen. Root cause: `detail_menu_item_count()` growing a
third possible shape (3DS Merge support) had been wired up with the
label selection rewritten as a real `if (s_dsMode) { ... } else { ... }`
block, with the `(const char *[]){...}` compound literals created
*inside* those braces.

That's a genuine, confirmed C bug, not a hypothetical one -- a
compound literal's storage is scoped to its own *innermost enclosing
block*, not to the function. Once the if/else block closes, that
memory is technically invalid, even though `labels` (declared outside
the block) still holds its address. It can appear to work by pure
luck of what the compiler does with the freed stack space next --
which is exactly why this passed earlier testing (a different,
actually-safe ternary-based pattern was used then) and only broke
once real code ran between building `labels` and using it.

Reproduced directly before trusting the fix: the exact buggy pattern,
compiled with optimization, crashes with a segfault and GCC's own
`-Wdangling-pointer=` warning flags the exact line. Fixed by collapsing
into a single ternary expression at the point of declaration, so every
compound literal shares `labels`' own (correct, function-lifetime)
scope rather than a block that closes before the literal is actually
read. Re-ran the same reproduction against the fixed pattern -- now in
a version verified against the *actual* function shape (labels built
and used within one function, never returned to a caller, matching
`draw_detail_menu` exactly) -- and confirmed no warning, no crash,
across all four label-set combinations (3DS mergeable/non-mergeable,
DS both tabs).

## Nexus3DS filename compatibility, verified for filtering specifically

The buffer-size fix above was verified against the companion-file
pairing at the time. Separately confirmed the date-filtering path
specifically: `parse_timestamp_ymd` (the same function used for both
display and the Year/Month/Day filter) correctly extracts
2026/08/27 from `2026-08-27_01-59-26.437_000400000FF3AA00` -- `sscanf`
stops matching once its own format is satisfied, so the trailing
title ID is simply ignored rather than causing a parse failure.

## Merge Top/Bottom Screens (3DS only)

Deliberately simple, matching how [screenshot-merge](https://github.com/ihaveamac/screenshot-merge)
solves the same problem: composites the top screen (400x240) and
bottom screen (320x240, centred with black margins) into one 400x480
BMP, no stereo 3D. Written straight back into Luma's own screenshots
folder as `<original_timestamp>_merged_top.bmp` -- inserting "_merged"
before the suffix means it can never collide with the source pair's
own filename, while still starting with the real original timestamp,
so it sorts/filters/displays by the actual capture date rather than
merge time (the same sscanf-ignores-trailing-text behaviour the
Nexus3DS fix above depends on).

Output filename is `<original_timestamp>_cmb.bmp` -- the exact
original base string, whatever it was (title ID included, for a
Nexus3DS source), no disambiguating suffix needed. That's safe specifically
because `_cmb.bmp` is a different suffix than `_top.bmp`/
`_top_right.bmp`/`_bot.bmp`, so it can never collide with the source
pair's own files even though it starts with the identical timestamp
string; verified with a real scan where both existed side by side.
Merge is only offered on an item that actually has a real bottom
capture (`botPath` set) -- this also rules out merging an
already-combined entry, since a `_cmb.bmp`-sourced one never has a
separate `botPath` to begin with.

Written into whatever directory the source screenshot actually came
from -- the root for a flat Luma layout, the same date subfolder for
a Nexus3DS one -- derived from `topPath`'s own directory
(`strrchr(topPath, '/')`) rather than hardcoding the screenshots root.
No special-casing needed for either layout as a result. Verified with
both cases directly: a root-level source and a date-subfoldered one
with a title ID in its name, confirming the output lands in the right
folder and preserves the full original naming both times.

Needed a new `bmp_write()` -- Comet could only *read* BMP before this.
Deliberately the simplest valid shape (24bpp, uncompressed,
BITMAPINFOHEADER), verified with an actual write-then-read-back round
trip against the real reader before trusting it: built a distinctive
400x480 test image matching the real merge layout (red top half, blue
and green quadrants in the centred bottom region, black margins),
wrote it, read it back, and confirmed every pixel matched exactly,
including spot-checks of the margin boundaries specifically.

`fs_scan_screenshot_pairs` gained a second scan pass,
`scan_combined_in_dir()`, specifically for standalone `_cmb.bmp`
files -- this is also **Nexus3DS's own native format** for its
"combine top/bottom screenshots" option (400x480, confirmed same
dimensions as what Comet writes), which the scanner previously never
looked for *at all* -- not a parsing bug, those files were simply
invisible to it. Comet's own Merge output and a genuine Nexus3DS
combined screenshot are now handled by the exact same code path, no
special-casing between them. No separate companion lookup is
attempted for these -- the image is self-contained, so they always
come through as `has3D=false` with an empty `botPath`.

Verified against a real mixed directory: a normal Luma 3D pair, a
plain `_cmb.bmp`, a title-ID-embedded `_cmb.bmp` (confirming the
64-byte buffer fix above covers this suffix too, not just `_top.bmp`),
and a simulated Merge-feature output sharing the exact same timestamp
as the original pair -- all four discovered correctly, and the
shared-timestamp pair confirmed genuinely non-colliding (different
suffix, different file, both intact). Same story on display: the
existing preview code already does generic aspect-preserving
letterboxing based on whatever the loaded image's actual dimensions
are (not a fixed assumption of 400x240), which for a 400x480 image
already lands at exactly 200x240 centred -- and the grid thumbnail
path already does an unconditional stretch-to-fill for every
screenshot regardless of its real aspect ratio. Neither needed any
merged-image-specific code at all.

## Merged screenshots no longer look stretched in the grid

The distortion wasn't just the final draw-time scale -- it started
one step earlier. `bmp_load_thumbnail_at` maps the source's full
width/height directly onto the fixed 48x30 thumbnail buffer with no
aspect awareness at all, which is barely noticeable for a normal
~400x240 screenshot (its aspect is already close to the buffer's) but
very noticeable for a 400x480 merged image. Fixing only the final
draw call wouldn't have been enough -- the *sampled thumbnail texture
itself* was already the wrong shape.

Added a `letterbox` parameter: when set, it computes a centred
sub-region within the 48x30 buffer that preserves the source's true
aspect ratio, zeroes the whole buffer first, and only samples into
that sub-region -- everything outside it stays black automatically.
Wired up via the `isCombined` flag added above, so only merged
screenshots get this treatment; everything else keeps the exact
original behaviour.

Verified with a real compiled test rendering the actual pixel buffer
as ASCII art for three cases: a normal screenshot (fills the buffer
completely, unchanged), a merged one with letterboxing on (clear black
margins, content centred, matching the computed math), and the same
merged source with letterboxing off (fills completely -- directly
demonstrating the distortion this fixes, not just inferring it existed).

## Merged screenshots get their own grid badge

`ScreenshotPair` gained an explicit `isCombined` flag, set by
`scan_combined_in_dir()` at the point a `_cmb.bmp` entry is discovered
(never by the regular `_top.bmp` scan, which zeroes it via the
existing `memset`). Previously a combined screenshot was
indistinguishable from an ordinary flat one at the struct level --
both had `has3D=false` -- so it showed the generic 2D badge. The grid
now checks `isCombined` before falling back to the 3D/2D check, and
shows a dedicated badge instead. Verified with a real compiled scan
against a directory containing both a regular screenshot and a
`_cmb.bmp` one side by side: the flag came back false and true
respectively, as expected.

## Deleting a merged screenshot could delete the original's 3D/bottom data

Real, severe bug: a merged (`_cmb.bmp`) entry deliberately shares its
exact timestamp and directory with the original pair it came from --
that was the whole point of the "no disambiguator needed" naming
design a few rounds back, since `_cmb.bmp` is a different suffix and
can't collide with `_top.bmp`. But the defensive delete fallback added
later (guessing a companion path from topPath's directory + timestamp
when the recorded field is empty) didn't know that distinction. A
combined entry's `topRightPath`/`botPath` are *always* empty by
design, which the fallback read as "possibly orphaned, worth
guessing" -- and the path it guessed, `<same dir>/<same
timestamp>_top_right.bmp`, happens to be exactly the original pair's
real, still-in-use file. It existed, so it got deleted right along
with the merged copy.

Fixed by excluding combined entries from the fallback entirely --
their empty companion fields are permanent, not stale. Verified with
two real compiled tests: deleting a merged entry that shares a
timestamp with a real 3D pair now leaves the original's `_top_right`/
`_bot` untouched, and a genuinely orphaned companion on an ordinary
(non-combined) pair is still cleaned up as intended.

## After deleting from the More screen, stay there

Deleting used to always drop back to the grid. Now it lands on the
adjacent (previous) screenshot and stays on the More screen, only
falling back to the grid when nothing is left to show. Menu selection
is re-clamped for the new item's shape (Merge only appears with a
real bottom capture), and the bottom-capture request is redone for
whatever was landed on -- without that, R would peek at the just-
deleted screenshot's now-stale capture instead of the new one's.

## More screen info panel lagged behind the loaded preview

Related to the loader fix above, but a separate bug: `draw_detail_menu()`
and `detail_menu_item_count()` derived which screenshot to describe
from `s_visibleIndices[s_selected]`, which changes the instant
Left/Right is pressed -- before the new preview has actually finished
loading. So the path, date, and menu options (2 vs 3, depending on
whether the item has a bottom capture) would jump ahead to the newly
selected screenshot while the top screen was still showing the
previous one. Left/Right itself was already correctly gated on
`current_preview_ready()`, but that only stops a *second* press from
racing ahead -- it didn't stop the info panel from describing the
target of the first press immediately.

Fixed by driving both from `s_previewLoadedPairIndex` instead --
which only updates once the async load genuinely completes, not when
the selection changes. Verified by simulating the exact interleaving:
old logic describes the new screenshot while the top screen still
shows the previous one, new logic correctly holds at the previous one
until the load catches up.

## Loader result contention (preview vs. bottom capture)

The background loader has a single pending-result slot, and
`loader_poll_result()` used to free any result whose ID didn't match
what the caller asked for. That was safe while only one thing polled
it, but enabling live preview updates in `APP_DETAIL` (needed so
Left/Right navigation updates the top screen) added a second consumer
alongside the R-held bottom-screen capture. Whichever polled first
each frame would destroy the other's completed job, leaving that
consumer waiting on a result that no longer existed: a spinner that
never resolves.

Symptoms matched exactly: intermittent-but-reproducible-per-screenshot
(it depends on which job finishes first), affecting both the preview
and the R-held bottom capture, and **not** affecting DS mode at all --
`request_bottom_capture()` is only called when `!s_dsMode`, so DS mode
only ever has one consumer and no contention.

Fixed by having a poll leave results belonging to the *other* live
consumer in place, and only free genuinely stale ones (an ID matching
neither outstanding request, i.e. a superseded load). Verified by
simulating the exact interleaving: old behaviour destroys the
preview's completed job and spins forever, new behaviour delivers it.

## Merge feature polish: cursor/preview restoration after a merge

After a merge, the new `_cmb.bmp` entry shares the exact same
timestamp as the source screenshot, so it's easy for the rescan's
newest-first re-sort to place it right next to (or in place of)
where the cursor already was. The fix snapshots the *original*
screenshot's own `topPath` before rescanning (it has to happen before
-- the in-progress pointer into `s_pairs` gets overwritten in place by
the rescan, so reading it afterward would read back whatever the
rescan wrote at that slot, not the original data), then searches the
freshly rebuilt visible list for that same path and moves the cursor
there explicitly, rather than trusting a fixed positional offset.

That distinction matters: a simpler "shift the cursor by one position"
fix was considered and tested against both possible outcomes of C's
`qsort`, which doesn't guarantee how it breaks ties between equal
timestamps. The positional shift was only correct in one of the two
orderings -- it depends on which way qsort happens to place the two
equal-timestamp entries, which isn't something the code controls.
Matching by the file's own path is correct regardless of sort order.

Also explicitly resets the preview-loaded tracking state
(`s_previewLoadedPairIndex`/`s_previewRequestedPairIndex`) and frees
the current preview textures, rather than relying purely on the
normal index-mismatch check to notice a refresh is needed -- a
coincidental re-sort could in principle leave that check fooled.

## Nexus3DS (and other Luma forks) filename compatibility

The 3DS screenshot scanner strips the known suffix (`_top.bmp` etc.)
from a filename and reuses whatever's left both as the display
timestamp *and* as the key it constructs companion filenames from.
That's fine for Luma's own format (23 chars, fits comfortably) but
Nexus3DS -- an enhanced Luma3DS fork -- embeds a title ID between the
timestamp and suffix, running to ~40 chars. The old 32-byte
`ScreenshotPair.timestamp` truncated that mid-title-ID, so the
constructed `_top_right.bmp`/`_bot.bmp` lookups searched for filenames
that didn't actually exist on disk -- silently breaking 3D detection
and the bottom-screen capture for every screenshot from an affected
fork (`has3D` only ever flips true when the top_right lookup
succeeds). Fixed by enlarging the buffer to 64 -- title IDs are always
a fixed 16 hex digits in the 3DS ecosystem, so this isn't a moving
target. Verified against the exact reported filename, and confirmed
the old size reproduces the failure.

Downstream consumers (date parsing/display, EXIF embedding) were
already safe against the longer value -- they all take `const char *`
and use `sscanf` on just the leading date/time portion, which stops
matching once its own format string is satisfied regardless of
trailing text. Only the source field itself needed to grow.

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

### Whole-pixel rounded rects

`draw_rounded_rect` snaps its coordinates to whole pixels before
drawing, for the same reason icons do (below). DS mode's grid is
centred via a half-pixel origin (`GRID_LEFT_DS = 19.5`), and at the
2px radius used everywhere, a fractional circle centre rasterizes
asymmetrically -- one corner reads rounded, the opposite one sharp.
3DS mode's whole-pixel grid never exposed this. Confirmed the snap is
a no-op for every other caller (detail menu, filter rows, popups),
which all already use whole-pixel coordinates.

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


## Hidden easter egg

Triple-tap the header's Comet icon within 1 second, from the plain
(non-batch) 3DS or DS grid, to reach a static thank-you screen with a
stereo photo of the developer's dog. Same trigger tapped again from
inside the screen exits, with the regular button sound instead of the
jingle.

### Tap timing

Feasibility checked with real numbers before building anything: a
60-frame (1 second at 60fps) window comfortably fits even a slow,
400ms-per-tap triple-tap (needs only 48 frames), let alone a
comfortable one. Detection is a 3-slot ring buffer of tap frame
numbers -- a tap only counts as the third of a triple if the *oldest*
of the 3 stored taps is still within the window, which self-corrects
naturally as stale taps age out without needing separate reset logic.
Shared between entering and exiting via `register_icon_tap_and_check_triple()`
rather than duplicated.

Relies on `kDown & KEY_TOUCH` being edge-triggered (fires exactly once
per physical tap, confirmed from the existing `tapped` variable
elsewhere in the file) -- level-triggered touch would make counting
discrete taps unreliable.

### Compile-time embedded images

The two dog photos and the pixel-art avatar are baked into
`assets/easter_egg_data.c` via a build script
(`bake_easter_egg.py`) using the exact same Morton/Z-order swizzle
already verified correct for the icon pipeline -- just applied to
much larger source images (400x240 stereo pair -> 512x256 textures
each). Verified with real spot-checks (corners, edges, centre) between
the baked data and the original source images before trusting it, not
just "the script ran without error."

Loaded once at startup via `load_easter_egg_images()`, using the exact
same simple direct-upload pattern as `load_static_icons()` -- no
runtime file I/O or loader-thread involvement at all, since the data
is already in the right swizzled format at compile time.

Deliberately not run through `draw_eye_image()` for display -- that
function's behaviour branches on `s_dsMode` (widescreen stretch,
native-size letterbox for DS content) which doesn't apply here
regardless of which grid the user triggered this from. A dedicated
`draw_easter_egg_eye()` always draws the full 400x240 photo, unscaled.


## Button sound missing for pure-touch content interactions

The automatic button-sound check only ever recognised
`A/B/X/Y/L/R/Select` in its `nonDirectionalPressed` flag -- `KEY_TOUCH`
was never included. So any interaction that's *purely* a touch tap on
actual content (opening a screenshot's More page, switching a DS tab,
tapping a filter row) was invisible to it, even though it clearly
changes state. Footer hint taps already worked because they get an
explicit sound of their own at the point they're consumed, which
masked the gap elsewhere.

Fixed by capturing whether an unconsumed tap is heading into the input
switch (`tappedGoingIntoSwitch`, captured right after footer-hit
consumption specifically, so a footer tap's already-played sound
doesn't also double up here) and including that alongside the
existing button check. The underlying diff-check is unchanged --
this only widens what counts as "worth checking," not what counts as
"actually changed."

## Touch-holding "Show Bottom" didn't work

Physically holding R is detected via `hidKeysHeld()`, queried fresh
every frame. Touching and holding the "Show Bottom" footer hint has no
equivalent: footer-hit consumption is edge-triggered (`kDown`), firing
once on the initial tap-down -- so the tap correctly played its sound
(that part is consumed there) but never sustained anything afterward,
which is exactly why the sound played but the peek itself never
engaged.

Fixed by separately checking, every frame, whether the touchscreen is
currently held (`hidKeysHeld() & KEY_TOUCH`) over any footer hitbox
mapped to `KEY_R` -- R has exactly one meaning anywhere in the app
(confirmed by checking every use of `ICON_BTN_R`), so this can't be
ambiguous with anything else. Computed in `ui_frame` (where touch
coordinates are actually in scope) and read from `draw_bottom_screen`
(a separate function) via a small dedicated flag, set fresh every
frame before that function runs.


## Performance: scanning, deleting, and boot

Deleting a 3DS screenshot had become noticeably slow on a large
library. Profiling the actual filesystem calls (rather than guessing)
found the cost was in the directory scan that runs after every delete,
not in the delete itself:

- `scan_one_dir` and `scan_combined_in_dir` each opened and enumerated
  the *same* directory separately, and the subfolder loop enumerated
  it a third time
- Two `stat()` calls per screenshot to test for `_top_right`/`_bot`
- One more `stat()` per directory entry, purely to ask "is this a
  directory?"

For a flat 88-screenshot library (264 files) that worked out to about
**440 stat() calls and 3 full enumerations, on every delete**.

Rewritten to enumerate once into an in-memory, sorted name list, then
answer every companion question with `bsearch` against that list --
zero additional I/O. Subdirectories are identified from `dirent`'s
`d_type` where the filesystem reports it (FAT does), falling back to
`stat()` only for entries reported as unknown. Same library now costs
1 enumeration and 0 stat() calls. This speeds up boot and every grid
refresh too, since they all go through the same scan.

### Thumbnails survive a delete

Deleting used to `free_all_thumbnails()` and rebuild every thumbnail
from scratch -- each one being a file open plus ~30 seek/read pairs.
But deleting one screenshot leaves every *other* thumbnail perfectly
valid; only its index shifts. `remove_thumbnail_at()` now drops just
the deleted entry and shifts the rest down.

The catch: `EyeTexture` contains self-references (`image.tex` points
at its own `.tex` field), so a plain `memmove` leaves every shifted
entry's image pointing at the previous slot's texture -- the exact
hazard that made in-place shifting off-limits before. They're repaired
explicitly right after the move, which is what makes it safe. Verified
by simulation that the shifted thumbnail array stays aligned with the
rescanned pair list, including when deleting the first and last items.

### Boot-time DS availability

`refresh_ds_availability()` only ever compares its result against
zero, but was calling `ds_count_tar_screenshots()`, which walks all 50
tar slots (~50 seeks into a 5MB file), plus a full enumeration of the
extracted folder. Added `ds_has_any_tar_screenshot()` /
`ds_has_any_extracted()`, which stop at the first hit -- in the common
case where slot 1 is occupied, a single seek instead of fifty.


## Bottom-screen capture is loaded lazily

The remaining "deleting a 3DS screenshot feels slow" complaint, after
the scanner work above, traced to something else entirely -- and the
giveaway was that DS mode deletes stayed fast throughout.

`request_bottom_capture()` was called eagerly on every navigation:
entering the More screen, moving with Left/Right, and after a delete.
Each call queues a full ~230KB BMP decode plus Morton swizzle, whether
or not the user ever actually presses R. Two things made that
expensive:

- It's pure speculative work. Most navigations never peek at the
  bottom screen at all.
- The loader has a **single** pending-result slot, so that speculative
  job competes with the top-screen preview -- and the More screen
  waits on the preview before it shows anything. So the user waits on
  a decode they never asked for.

DS mode never had either problem, because DS captures have no
companion bottom frame and `request_bottom_capture` is skipped
entirely there. That asymmetry is exactly what made 3DS deletes feel
slow next to DS ones despite sharing the same code path.

Now `invalidate_bottom_capture()` just drops the stale texture (cheap,
no I/O), and `maybe_request_bottom_capture()` performs the actual load
on the first frame the peek is genuinely engaged -- covering both
physical R and the touch-hold path. A `s_bottomCapturePairIndex`
tracker means releasing and re-pressing R on the same screenshot
doesn't reload anything.

Post-delete loader work drops from ~806KB to ~576KB (29%), and more
importantly the preview no longer queues behind a job nobody asked
for. The same saving applies to every Left/Right navigation and every
entry into the More screen.


## Startup time: easter egg assets moved to romfs

The easter egg's two dog photos and avatar were baked into
`assets/easter_egg_data.c` as C arrays -- about **1.06MB living in
.rodata**, which the 3DS loads into memory at *every* launch, plus
three texture uploads in `ui_init()` before the app was usable. All
for a screen most people will never open.

They're now pre-swizzled into plain binary files under `romfs/egg/`
(8-byte header of texW/texH/imgW/imgH, then the swizzled RGBA
payload), and loaded on first trigger instead of at startup. romfs is
read on demand rather than loaded wholesale at launch, so this removes
the 1.06MB from both the startup read *and* the resident memory
footprint until the egg is actually opened.

Because the payload is swizzled at build time, loading is still just a
header read plus one straight read into texture memory -- no
per-pixel work at runtime, same as when it was a C array.

Verified the on-disk format round-trips: read the header and payload
exactly as the C loader does, then spot-checked pixels (corners,
edges, centre) against the original images.


## Startup time: sounds load on first play

`audio_init()` loaded all six WAVs up front -- about **640KB read off
the card before the app could show anything**, and over a third of
that (252KB) was the easter egg jingle most people will never trigger.

Now `audio_init()` only initialises ndsp; each sound is loaded and
configured by `ensure_sfx_loaded()` the first time it's actually
played. An `attempted` flag means a failed load isn't retried on
every play. Individual files are small enough that the one-off hitch
is imperceptible, and the two largest (Copy at 215KB, Delete at 84KB)
only ever play during operations that already hold a progress popup
on screen.

### What's left, and why

Startup romfs reads across this session:

| | before | after |
|---|---|---|
| fonts | 1,053,264 | 1,053,264 |
| sound effects | 640,728 | 0 |
| easter egg assets | 1,114,136 | 0 |
| **total** | **2.68 MB** | **1.00 MB** |

The two Poppins `.bcfnt` files (526KB each) are the remaining eager
load and are deliberately left alone: both are needed to draw the very
first frame (Regular for body text, SemiBold for the header title), so
deferring them would just move the cost somewhere more visible. The
real lever there would be regenerating them with a restricted
character set via `mkbcfnt`, which would cut them substantially -- but
that changes the build inputs rather than the code, so it's a
deliberate call for the project owner rather than something to do
silently.


## Merge no longer shows a confirmation popup on success

Since merging already lands on the grid with the new merged
screenshot's thumbnail highlighted, the "Screenshot merged / Added to
your gallery / OK" popup afterward was pure friction -- the result was
already visible without it. Removed for the success path only;
failure still shows `APP_RESULT` with an error message, since a failed
merge has nothing visible in the grid to explain what happened the way
a new thumbnail does for success.


## Cleanup pass

Smaller findings from a survey of remaining per-frame work and dead
code:

- **The footer parsed every label twice per frame.** `draw_footer_hints`
  called `draw_text_vcenter` to draw each label, then `measure_text` on
  the same string to size its touch hitbox -- each parsing and
  optimizing the glyph list independently. Added
  `draw_text_vcenter_measured`, which does both in one pass. This
  matters more than it sounds because the footer draws on *every*
  screen, including while scrolling the grid.
- The header's `"+"` separator was re-measured every frame despite
  being a constant; cached on first use. (`modeLabel` next to it still
  varies, so it stays measured per frame.)
- Confirmed no file I/O is reachable from the per-frame drawing path --
  worth checking explicitly, since the tar fingerprinting in
  `apply_tar_widescreen_prefs()` does real I/O per slot. All six of its
  call sites are inside operation handlers, none in drawing.
- `enter_ds_mode` was calling the full-walk `ds_count_tar_screenshots()`
  and `ds_count_extracted()` on every mode switch, then only comparing
  the results against zero -- the same waste already fixed in
  `refresh_ds_availability()`. Switched to the early-exit variants;
  verified the rewritten boolean is logically identical across all four
  input combinations.
- Removed genuinely dead code: `bmp_load_thumbnail` (every caller now
  goes through `bmp_load_thumbnail_at` since the offset and letterbox
  parameters were added), plus `ds_count_tar_screenshots`,
  `ds_count_extracted`, and the `count_cb` callback that only existed
  to serve them.


## Grid fill: time budget instead of a fixed one-per-frame cap

`build_thumbnail` runs on the main thread inside `draw_grid`, so each
one costs real frame time -- which is why it was capped at exactly one
per frame. But that cap is a *fixed guess* at how much fits in a
frame, and it's pessimistic whenever the card is faster or the images
smaller than the guess assumed.

Replaced with a time budget: always build at least one (guaranteeing
progress), then keep building only while under ~8ms for the frame.
The important property is that this can never be **slower** than the
old behaviour -- if a single thumbnail already exceeds the budget, it
still builds exactly one per frame, identical to before. On anything
faster, the grid fills proportionally quicker, with no hardcoded
assumption about which case a given console is in.

Modelled across a range of per-thumbnail costs: at 1ms it builds 8 per
frame (grid fills in 2 frames instead of 16), at 8ms and above it
settles back to 1 per frame exactly as before.

One consequence worth handling: `SFX_THUMB_LOAD` fired per thumbnail
built. With several landing in one frame that would re-trigger the
same ndsp channel repeatedly and clip each into a mess, so it now
plays at most once per frame.


## On-disk thumbnail cache

Sampling one thumbnail means opening the source BMP and doing ~30
separate seek+read pairs to pull one representative row per output
row. The *result* is only 4KB. Caching those results
(`sd:/3ds/Comet/thumbcache.bin`, ~286KB for a full 64-screenshot
library) turns filling the grid into one sequential read instead of
dozens of scattered ones.

### Why this is safe

The property that makes a thumbnail cache safe rather than a source of
wrong-image bugs: **it is only ever consulted by the exact path of a
file the scanner just found on disk.** A stale entry -- for something
deleted outside Comet, say -- is therefore never queried and can never
be displayed. It simply occupies a slot until it ages out.

On top of that, entries are keyed by `path + byte offset` (the offset
matters for nds-bootstrap tar entries, which all share one path) and
validated against the source's file size, so a file replaced at the
same path with differently-sized content is a miss rather than a stale
hit. The file itself carries a magic number and version; anything
unrecognised is discarded and rebuilt rather than trusted.

### Keeping deletion fast

Deleting forgets the relevant entries **in memory only**
(`thumb_cache_forget`), gathered before the list changes while paths
still resolve -- single delete and batch delete alike. No file I/O
happens on the delete path at all.

The cache file is rewritten later, on the first frame where nothing
was built (i.e. the visible grid has settled). Writing after every new
thumbnail would have put a file write in the middle of the fill it
exists to speed up.

### Verified

Twelve behavioural checks compiled and run against the real logic:
hit/miss on path, offset and size; changed-size correctly missing
rather than returning stale data; re-storing replacing rather than
duplicating; `forget` removing exactly one entry and leaving others
intact; forgetting an absent entry being harmless; and eviction
keeping the newest while dropping the oldest once full. Plus a
file-format round trip confirming byte-exact reload, no struct padding
surprises (4584 bytes/entry as designed), and correct rejection of a
corrupt header.
