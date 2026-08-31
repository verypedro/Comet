#pragma once

#include "common.h"

typedef struct {
    u8  *pixels;   // width * height * 3 bytes, row-major, top-down, RGB
    int  width;
    int  height;
} RGBImage;

// Full decode -- used for the actual conversion pipeline.
bool bmp_load(const char *path, RGBImage *out, char *outErr, size_t outErrSize);

// Same, but for a BMP embedded at byte offset `base` inside a larger
// file -- used to read DS screenshots straight out of screenshots.tar
// without extracting them first.
bool bmp_load_at(const char *path, long base, RGBImage *out, char *outErr, size_t outErrSize);

// Writes a plain 24bpp BMP -- used by the top/bottom merge feature.
bool bmp_write(const char *path, const RGBImage *img, char *outErr, size_t outErrSize);
void bmp_free(RGBImage *img);

// Cheap thumbnail sampler for UI lists: reads only the rows it needs
// straight off the SD card (skipping the rest via fseek) instead of
// decoding the whole image, and writes directly into a caller-owned
// `cols * rows * 3` RGB buffer. Much faster than bmp_load() + downsample
// when you only need a small preview.
//
// letterbox=true preserves the source's true aspect ratio within the
// cols x rows buffer, centred, with the surrounding area left black --
// use this for anything that isn't shaped like a normal screenshot
// (e.g. a merged top+bottom image), since plain stretch-to-fill
// distorts it visibly. letterbox=false is the original behaviour:
// the source's full width/height maps directly onto cols x rows.
bool bmp_load_thumbnail_at(const char *path, long base, int cols, int rows,
                            bool letterbox, u8 *outRGB, char *outErr, size_t outErrSize);

bool bmp_load_thumbnail(const char *path, int cols, int rows,
                         u8 *outRGB, char *outErr, size_t outErrSize);
