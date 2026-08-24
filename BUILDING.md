# Building Comet

## 1. Toolchain setup

Install [devkitPro](https://devkitpro.org/wiki/Getting_Started) with
the 3DS development packages (`3ds-dev` package group via `dkp-pacman`).

## 2. Add stb_image_write.h

```
curl -o source/stb_image_write.h https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h
```

(This is the one file not checked into the repo — its license asks
that it be downloaded fresh from the source rather than redistributed,
unlike the fonts/banner/icons, which are all baked in.)

## 3. Build

```
cd Comet
make
```

Produces **both** `Comet.3dsx` and `Comet.cia`:

- `Comet.3dsx` — copy to `sdmc:/3ds/Comet/Comet.3dsx`, launch from the
  Homebrew Launcher. No install needed.
- `Comet.cia` — an installable title with its own HOME Menu icon and
  banner; see below for what it needs.

If you only want one of the two (say, `makerom`/`bannertool` aren't
set up yet and you just want to quickly test a change):

```
make 3dsx   # .3dsx only
make cia    # .cia only
```

## 4. What the .cia needs

Two tools beyond what `3ds-dev` already installs:

- **makerom** — https://github.com/3DSGuy/Project_CTR/releases
- **bannertool** — the original (Steveice10/bannertool) is archived
  and its download links have been unreliable; use
  [Epicpkmn11/bannertool](https://github.com/Epicpkmn11/bannertool/releases)
  instead, which explicitly ships a macOS build.

Neither is part of the `3ds-dev` package group (unlike `mkbcfnt`), so
they need a separate download. After downloading: `chmod +x` each one,
clear the macOS quarantine flag (`xattr -d com.apple.quarantine
<path>`), and put both somewhere on your `PATH` — copying them into
`/opt/devkitpro/tools/bin/` alongside the other 3DS tools is the
simplest option.

Everything else is already checked into the project — `meta/app.rsf`
(permissions/metadata), `meta/banner.png` / `meta/banner.wav` (banner
art + jingle), and the icon (reuses `icon.png`). Once both tools are
on `PATH`, plain `make` (or `make cia` alone) picks everything up
automatically.

**Worth knowing:** `meta/app.rsf` explicitly declares which system
services Comet is allowed to use (filesystem, graphics, input, and
`dsp::DSP` for the SFX) — permissions a `.3dsx` never needs to
declare, since the Homebrew Launcher grants broad access
automatically. If `Comet.cia` installs but crashes or a feature
misbehaves that worked fine as a `.3dsx`, a missing service in that
file is the first place to look.

## Installing the .cia

Since it's unsigned, it needs a CFW with patched signature checks —
Luma3DS (already required for Rosalina screenshots) provides this.
Install via [FBI](https://github.com/Steveice10/FBI): copy `Comet.cia`
to the SD card, open FBI, navigate to it, and choose Install. FBI's
Remote Install → Scan QR Code also works, pointed at a direct download
link (e.g. a GitHub Releases asset URL).

## Project layout

```
Comet/
  Makefile
  icon.png
  meta/
    app.rsf            CIA permissions/metadata
    banner.png/.wav    HOME Menu banner art + jingle
  romfs/
    Poppins-*.bcfnt     converted fonts (see TECHNICAL.md)
    sfx/                UI sound effects
  source/
    main.c
    ui.c / ui.h         screen state machine + drawing (citro2d)
    bmp.c / bmp.h        BMP decoder + fast thumbnail sampler
    mpo.c / mpo.h        JPEG-encodes + writes MPF/EXIF-tagged .mpo/.jpg
    fs_utils.c/.h        screenshot discovery, DCIM slot allocation
    audio.c/.h           SFX playback (ndsp)
    assets/              generated icon texture data (see TECHNICAL.md)
    stb_impl.c           stb_image_write implementation TU
    stb_image_write.h    <- you add this (see step 2)
    common.h             shared constants/types
```
