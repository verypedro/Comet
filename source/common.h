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
    char timestamp[32];     // e.g. "2026-08-04_12-34-56.789"
    char topPath[256];      // .../{timestamp}_top.bmp        (always present)
    char topRightPath[256]; // .../{timestamp}_top_right.bmp  (only if 3D was on)
    char botPath[256];      // .../{timestamp}_bot.bmp        (mono, no 3D)
    bool has3D;
} ScreenshotPair;
