#include "bmp.h"

static u16 rd_u16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd_u32(const u8 *p) { return (u32)(p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24)); }
static s32 rd_s32(const u8 *p) { return (s32)rd_u32(p); }

typedef struct {
    u32  dataOffset;
    s32  width;
    s32  height;   // always positive
    bool topDown;
} BmpInfo;

// Shared by both bmp_load() and bmp_load_thumbnail(): reads and
// validates just the file/DIB header, leaving the read position at the
// start of the header bytes (callers seek to dataOffset themselves).
static bool bmp_read_header(FILE *f, BmpInfo *info, char *outErr, size_t outErrSize)
{
    u8 header[14 + 40];
    if (fread(header, 1, sizeof(header), f) != sizeof(header)) {
        snprintf(outErr, outErrSize, "File too small to be a BMP");
        return false;
    }
    if (header[0] != 'B' || header[1] != 'M') {
        snprintf(outErr, outErrSize, "Not a BMP file (bad signature)");
        return false;
    }

    u32 dataOffset   = rd_u32(&header[10]);
    u32 dibSize      = rd_u32(&header[14]);
    s32 width        = rd_s32(&header[18]);
    s32 heightRaw    = rd_s32(&header[22]);
    u16 bpp          = rd_u16(&header[28]);
    u32 compression  = rd_u32(&header[30]);

    if (dibSize < 40) {
        snprintf(outErr, outErrSize, "Unsupported BMP DIB header (size %lu)", (unsigned long)dibSize);
        return false;
    }
    if (bpp != 24) {
        snprintf(outErr, outErrSize, "Unsupported BMP bit depth (%u, expected 24)", bpp);
        return false;
    }
    if (compression != 0) {
        snprintf(outErr, outErrSize, "Unsupported BMP compression (%lu, expected 0/BI_RGB)", (unsigned long)compression);
        return false;
    }
    if (width <= 0) {
        snprintf(outErr, outErrSize, "Invalid BMP width");
        return false;
    }

    bool topDown = heightRaw < 0;
    s32 height = topDown ? -heightRaw : heightRaw;
    if (height <= 0) {
        snprintf(outErr, outErrSize, "Invalid BMP height");
        return false;
    }

    info->dataOffset = dataOffset;
    info->width      = width;
    info->height     = height;
    info->topDown    = topDown;
    return true;
}

bool bmp_load(const char *path, RGBImage *out, char *outErr, size_t outErrSize)
{
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(outErr, outErrSize, "Couldn't open %s", path);
        return false;
    }

    BmpInfo info;
    if (!bmp_read_header(f, &info, outErr, outErrSize)) {
        fclose(f);
        return false;
    }

    u32 srcRowBytes = ((u32)info.width * 3 + 3) & ~3u;
    size_t totalSrcBytes = (size_t)srcRowBytes * (size_t)info.height;

    u8 *pixels = (u8 *)malloc((size_t)info.width * info.height * 3);
    if (!pixels) {
        snprintf(outErr, outErrSize, "Out of memory decoding %ldx%ld BMP", (long)info.width, (long)info.height);
        fclose(f);
        return false;
    }

    // One big read for the whole pixel block instead of one fread() per
    // row -- each small fread() carries fixed per-call overhead through
    // the filesystem layer, and for a ~240-row screenshot that adds up
    // to real, avoidable time. We can afford the temporary buffer (a
    // few hundred KB) for the win of a single bulk I/O call.
    u8 *rawBuf = (u8 *)malloc(totalSrcBytes);
    if (!rawBuf) {
        snprintf(outErr, outErrSize, "Out of memory (row buffer)");
        free(pixels);
        fclose(f);
        return false;
    }

    if (fseek(f, (long)info.dataOffset, SEEK_SET) != 0) {
        snprintf(outErr, outErrSize, "Couldn't seek to pixel data");
        free(pixels);
        free(rawBuf);
        fclose(f);
        return false;
    }

    bool ok = true;
    if (fread(rawBuf, 1, totalSrcBytes, f) != totalSrcBytes) {
        snprintf(outErr, outErrSize, "Unexpected end of file reading pixel data");
        ok = false;
    }

    if (ok) {
        for (s32 row = 0; row < info.height; row++) {
            const u8 *rowSrc = rawBuf + (size_t)row * srcRowBytes;
            s32 dstRow = info.topDown ? row : (info.height - 1 - row);
            u8 *dst = pixels + (size_t)dstRow * info.width * 3;

            for (s32 x = 0; x < info.width; x++) {
                u8 b = rowSrc[x * 3 + 0];
                u8 g = rowSrc[x * 3 + 1];
                u8 r = rowSrc[x * 3 + 2];
                dst[x * 3 + 0] = r;
                dst[x * 3 + 1] = g;
                dst[x * 3 + 2] = b;
            }
        }
    }

    free(rawBuf);
    fclose(f);

    if (!ok) {
        free(pixels);
        return false;
    }

    out->pixels = pixels;
    out->width  = info.width;
    out->height = info.height;
    return true;
}

void bmp_free(RGBImage *img)
{
    if (img->pixels) free(img->pixels);
    img->pixels = NULL;
    img->width = img->height = 0;
}

bool bmp_load_thumbnail(const char *path, int cols, int rows,
                         u8 *outRGB, char *outErr, size_t outErrSize)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(outErr, outErrSize, "Couldn't open %s", path);
        return false;
    }

    BmpInfo info;
    if (!bmp_read_header(f, &info, outErr, outErrSize)) {
        fclose(f);
        return false;
    }

    u32 srcRowBytes = ((u32)info.width * 3 + 3) & ~3u;
    u8 *rowBuf = (u8 *)malloc(srcRowBytes);
    if (!rowBuf) {
        snprintf(outErr, outErrSize, "Out of memory (row buffer)");
        fclose(f);
        return false;
    }

    // Key speedup vs. a full decode: we only ever read `rows` lines off
    // the SD card (one representative line per thumbnail row-band,
    // jumped to directly with fseek) instead of all `info.height` lines,
    // and we never allocate a full-image buffer at all. Within each line
    // we DO average a few samples horizontally, since that data's
    // already in memory at that point and costs no extra I/O.
    bool ok = true;
    for (int r = 0; r < rows && ok; r++) {
        int sy = (int)(((long)r * 2 + 1) * info.height / (rows * 2));
        if (sy >= info.height) sy = info.height - 1;
        int fileRow = info.topDown ? sy : (info.height - 1 - sy);
        long offset = (long)info.dataOffset + (long)fileRow * srcRowBytes;

        if (fseek(f, offset, SEEK_SET) != 0 ||
            fread(rowBuf, 1, srcRowBytes, f) != srcRowBytes) {
            snprintf(outErr, outErrSize, "Failed reading row %d for thumbnail", r);
            ok = false;
            break;
        }

        for (int c = 0; c < cols; c++) {
            int sx0 = (c * info.width) / cols;
            int sx1 = ((c + 1) * info.width) / cols;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            int stepX = (sx1 - sx0) > 3 ? (sx1 - sx0) / 3 : 1;

            int sumR = 0, sumG = 0, sumB = 0, n = 0;
            for (int sx = sx0; sx < sx1; sx += stepX) {
                const u8 *px = &rowBuf[sx * 3]; // BMP stores BGR
                sumB += px[0]; sumG += px[1]; sumR += px[2];
                n++;
            }

            u8 *dst = &outRGB[(r * cols + c) * 3];
            dst[0] = (u8)(sumR / n);
            dst[1] = (u8)(sumG / n);
            dst[2] = (u8)(sumB / n);
        }
    }

    free(rowBuf);
    fclose(f);
    return ok;
}
