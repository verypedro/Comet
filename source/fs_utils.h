#pragma once

#include "common.h"

int fs_scan_screenshot_pairs(ScreenshotPair *out, int max);
bool fs_ensure_dir_exists(const char *path);
bool fs_next_dcim_slot(char *outDir, size_t outDirSize,
                        char *outBaseName, size_t outBaseNameSize);
bool fs_write_file(const char *path, const void *data, size_t len);

// Deletes the underlying BMP file(s) for a screenshot pair (top,
// top_right if present, bot if present). Best-effort on the optional
// files -- returns true as long as the primary (top) file was removed.
bool fs_delete_pair(const ScreenshotPair *p);
