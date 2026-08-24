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
