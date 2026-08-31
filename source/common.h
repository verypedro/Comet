#pragma once

#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

// Prefix every real filesystem path needs on 3DS homebrew (libctru mounts
// the SD card as the "sdmc:" device automatically for 3dsx/cia apps).
#define SD_ROOT               "sdmc:"

// Folder where Luma3DS Rosalina saves raw screenshots
#define SCREENSHOTS_DIR       "/luma/screenshots"

// Where Comet drops converted files if the user doesn't want them
// injected straight into the 3DS Camera album
#define FALLBACK_OUTPUT_DIR   "/luma/screenshots/mpo"

// Root of the SD card's camera roll, as used by the built-in 3DS Camera app
#define DCIM_DIR              "/DCIM"

// JPEG quality used when re-encoding the raw BMP screenshots (0-100)
#define JPEG_QUALITY          90

// Max screenshot pairs we'll list in the picker at once
#define MAX_PAIRS             64

// One discovered "photo" -- a left-eye bmp, optionally paired with a
// right-eye bmp for stereoscopic 3D, plus the bottom-screen capture.
typedef struct {
    // 32 was sized for Luma's own format ("2026-08-04_12-34-56.789",
    // 23 chars). Forks like Nexus3DS insert a title ID between the
    // timestamp and suffix ("..._000400000FF3AA00"), which runs to
    // ~40 chars -- a title ID is always a fixed 16 hex digits in the
    // 3DS ecosystem, so 64 gives comfortable headroom without needing
    // to revisit this for a similar fork later. Truncating this field
    // doesn't just cut the display date short: it's also used to
    // build the expected top_right/bot filenames, so a truncated
    // value made those lookups search for files that don't exist --
    // silently breaking 3D detection and the bottom-screen capture
    // for every screenshot from an affected fork.
    char timestamp[64];     // e.g. "2026-08-04_12-34-56.789" (or longer, see above)
    char topPath[256];      // .../{timestamp}_top.bmp        (always present)
    char topRightPath[256]; // .../{timestamp}_top_right.bmp  (only if 3D was on)
    char botPath[256];      // .../{timestamp}_bot.bmp        (mono, no 3D)
    bool has3D;
    bool isCombined;        // true for _cmb.bmp entries -- set by
                             // scan_combined_in_dir(), never by the
                             // regular _top.bmp scan
} ScreenshotPair;
