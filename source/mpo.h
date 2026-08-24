#pragma once

#include "common.h"
#include "bmp.h"

bool mpo_write(const RGBImage *left, const RGBImage *right,
                const char *timestamp,
                const char *outPath, char *outErr, size_t outErrSize);

// Single-image JPEG, for 2D screenshots that have no right eye.
bool jpg_write(const RGBImage *img, const char *timestamp,
                const char *outPath, char *outErr, size_t outErrSize);
