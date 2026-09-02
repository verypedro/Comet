#include "bmp.h"

static u16 rd_u16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd_u32(const u8 *p) { return (u32)(p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24)); }
static s32 rd_s32(const u8 *p) { return (s32)rd_u32(p); }

typedef struct {
    u32  dataOffset;
    s32  width;
    s32  height;   // always positive
    u16  bpp;      // 24 (Luma 3DS captures) or 16 (nds-bootstrap DS captures)
    bool topDown;
} BmpInfo;

// nds-bootstrap writes RGB565; expand a pixel to full 8-bit channels.
// The low-bit replication (rather than a plain shift) keeps white at
// exactly 255 instead of 248/252, which a bare shift would produce.
static inline void rgb565_to_rgb888(u16 v, u8 *r, u8 *g, u8 *b)
{
    u8 r5 = (u8)((v >> 11) & 0x1F);
    u8 g6 = (u8)((v >> 5)  & 0x3F);
    u8 b5 = (u8)( v        & 0x1F);
    *r = (u8)((r5 << 3) | (r5 >> 2));
    *g = (u8)((g6 << 2) | (g6 >> 4));
    *b = (u8)((b5 << 3) | (b5 >> 2));
}

// Shared by bmp_load() and bmp_load_thumbnail_at(): reads and
// validates just the file/DIB header, leaving the read position at the
// start of the header bytes (callers seek to dataOffset themselves).
// `base` is the byte offset at which this BMP starts within the file
// -- 0 for a standalone .bmp, or the payload offset of a TAR entry for
// a DS screenshot read in place. All of the BMP's own offsets are
// relative to its own start, so they get rebased against this.
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
    // 24bpp/BI_RGB is what Luma writes for 3DS screenshots; 16bpp
    // RGB565/BI_BITFIELDS is what nds-bootstrap writes for DS ones.
    // (The 565 channel masks are assumed rather than parsed -- that's
    // the only layout nds-bootstrap emits, and a mask mismatch would
    // show up immediately as visibly wrong colours.)
    if (bpp != 24 && bpp != 16) {
        snprintf(outErr, outErrSize, "Unsupported BMP bit depth (%u, expected 24 or 16)", bpp);
        return false;
    }
    if (bpp == 24 && compression != 0) {
        snprintf(outErr, outErrSize, "Unsupported BMP compression (%lu, expected 0/BI_RGB)", (unsigned long)compression);
        return false;
    }
    if (bpp == 16 && compression != 3 && compression != 0) {
        snprintf(outErr, outErrSize, "Unsupported 16bpp BMP compression (%lu)", (unsigned long)compression);
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
    info->bpp        = bpp;
    info->topDown    = topDown;
    return true;
}

bool bmp_load(const char *path, RGBImage *out, char *outErr, size_t outErrSize)
{
    return bmp_load_at(path, 0, out, outErr, outErrSize);
}

// Writes a plain 24bpp uncompressed BMP (BITMAPINFOHEADER, BI_RGB,
// bottom-up rows) -- deliberately the simplest possible valid BMP,
// since the only thing that has to read it back is Comet's own
// reader above, which already handles exactly this shape.
bool bmp_write(const char *path, const RGBImage *img, char *outErr, size_t outErrSize)
{
    u32 rowBytes = ((u32)img->width * 3 + 3) & ~3u; // rows padded to a multiple of 4
    u32 pixelDataSize = rowBytes * (u32)img->height;
    u32 fileSize = 14 + 40 + pixelDataSize; // file header + BITMAPINFOHEADER + pixels

    // Build the whole file in memory and write it in ONE fwrite, for
    // the same reason bmp_load does one big fread: each individual
    // stdio call carries fixed per-call overhead down through the FS
    // layer to the SD card, and that overhead dominates when it's
    // repeated once per row. A 400x480 merge is 480 rows, so the old
    // row-at-a-time version paid that cost 480 times over for what is
    // really a single ~576KB linear write.
    u8 *fileBuf = (u8 *)malloc(fileSize);
    if (!fileBuf) {
        snprintf(outErr, outErrSize, "Out of memory building BMP");
        return false;
    }
    memset(fileBuf, 0, fileSize);

    u8 *fileHeader = fileBuf;
    fileHeader[0] = 'B'; fileHeader[1] = 'M';
    fileHeader[2] = (u8)(fileSize);
    fileHeader[3] = (u8)(fileSize >> 8);
    fileHeader[4] = (u8)(fileSize >> 16);
    fileHeader[5] = (u8)(fileSize >> 24);
    fileHeader[10] = 54; // pixel data offset (14 + 40)

    u8 *infoHeader = fileBuf + 14;
    infoHeader[0] = 40;                                  // header size
    memcpy(&infoHeader[4],  &img->width,  4);
    memcpy(&infoHeader[8],  &img->height, 4);            // positive -> bottom-up
    infoHeader[12] = 1;                                  // planes = 1
    infoHeader[14] = 24;                                 // bpp = 24
    memcpy(&infoHeader[20], &pixelDataSize, 4);

    // Rows are stored bottom-to-top (BMP's classic row order) and BGR
    // per pixel, matching what bmp_load's own reader expects. Padding
    // bytes are already zero from the memset above.
    u8 *pixels = fileBuf + 54;
    for (int y = 0; y < img->height; y++) {
        const u8 *src = &img->pixels[(size_t)y * img->width * 3];
        u8 *dst = &pixels[(size_t)(img->height - 1 - y) * rowBytes];
        for (int x = 0; x < img->width; x++) {
            dst[x * 3 + 0] = src[x * 3 + 2]; // B
            dst[x * 3 + 1] = src[x * 3 + 1]; // G
            dst[x * 3 + 2] = src[x * 3 + 0]; // R
        }
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        snprintf(outErr, outErrSize, "Couldn't create %s", path);
        free(fileBuf);
        return false;
    }
    bool ok = fwrite(fileBuf, 1, fileSize, f) == fileSize;
    fclose(f);
    free(fileBuf);

    if (!ok) snprintf(outErr, outErrSize, "Failed writing BMP data to %s", path);
    return ok;
}

bool bmp_load_at(const char *path, long base, RGBImage *out, char *outErr, size_t outErrSize)
{
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(outErr, outErrSize, "Couldn't open %s", path);
        return false;
    }
    if (base != 0 && fseek(f, base, SEEK_SET) != 0) {
        snprintf(outErr, outErrSize, "Couldn't seek to image data");
        fclose(f);
        return false;
    }

    BmpInfo info;
    if (!bmp_read_header(f, &info, outErr, outErrSize)) {
        fclose(f);
        return false;
    }

    u32 bytesPerPx  = (u32)(info.bpp / 8);
    u32 srcRowBytes = ((u32)info.width * bytesPerPx + 3) & ~3u;
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

    if (fseek(f, base + (long)info.dataOffset, SEEK_SET) != 0) {
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

            if (info.bpp == 24) {
                for (s32 x = 0; x < info.width; x++) {
                    u8 b = rowSrc[x * 3 + 0];
                    u8 g = rowSrc[x * 3 + 1];
                    u8 r = rowSrc[x * 3 + 2];
                    dst[x * 3 + 0] = r;
                    dst[x * 3 + 1] = g;
                    dst[x * 3 + 2] = b;
                }
            } else { // 16bpp RGB565
                for (s32 x = 0; x < info.width; x++) {
                    u16 v = rd_u16(&rowSrc[x * 2]);
                    rgb565_to_rgb888(v, &dst[x * 3 + 0], &dst[x * 3 + 1], &dst[x * 3 + 2]);
                }
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

bool bmp_load_thumbnail_at(const char *path, long base, int cols, int rows,
                            bool letterbox, u8 *outRGB, char *outErr, size_t outErrSize)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(outErr, outErrSize, "Couldn't open %s", path);
        return false;
    }
    if (base != 0 && fseek(f, base, SEEK_SET) != 0) {
        snprintf(outErr, outErrSize, "Couldn't seek to image data");
        fclose(f);
        return false;
    }

    BmpInfo info;
    if (!bmp_read_header(f, &info, outErr, outErrSize)) {
        fclose(f);
        return false;
    }

    // Letterbox mode: rather than mapping the source's full width/
    // height onto the whole cols x rows buffer (which distorts any
    // image whose aspect ratio differs from the buffer's -- barely
    // noticeable for a normal ~400x240 screenshot against a ~48x30
    // buffer, very noticeable for something shaped like a 400x480
    // merged screenshot), compute a smaller, centred sub-region that
    // preserves the source's true aspect ratio, and only sample into
    // that -- the buffer is pre-zeroed, so everything outside the
    // sub-region stays black automatically.
    int dstCols = cols, dstRows = rows, dstX0 = 0, dstY0 = 0;
    if (letterbox) {
        memset(outRGB, 0, (size_t)cols * rows * 3);
        float srcAspect = (float)info.width / (float)info.height;
        float boxAspect  = (float)cols / (float)rows;
        if (srcAspect > boxAspect) {
            dstCols = cols;
            dstRows = (int)((float)cols / srcAspect + 0.5f);
            if (dstRows < 1) dstRows = 1;
        } else {
            dstRows = rows;
            dstCols = (int)((float)rows * srcAspect + 0.5f);
            if (dstCols < 1) dstCols = 1;
        }
        dstX0 = (cols - dstCols) / 2;
        dstY0 = (rows - dstRows) / 2;
    }

    u32 bytesPerPx  = (u32)(info.bpp / 8);
    u32 srcRowBytes = ((u32)info.width * bytesPerPx + 3) & ~3u;
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
    for (int r = 0; r < dstRows && ok; r++) {
        int sy = (int)(((long)r * 2 + 1) * info.height / (dstRows * 2));
        if (sy >= info.height) sy = info.height - 1;
        int fileRow = info.topDown ? sy : (info.height - 1 - sy);
        long offset = base + (long)info.dataOffset + (long)fileRow * srcRowBytes;

        if (fseek(f, offset, SEEK_SET) != 0 ||
            fread(rowBuf, 1, srcRowBytes, f) != srcRowBytes) {
            snprintf(outErr, outErrSize, "Failed reading row %d for thumbnail", r);
            ok = false;
            break;
        }

        for (int c = 0; c < dstCols; c++) {
            int sx0 = (c * info.width) / dstCols;
            int sx1 = ((c + 1) * info.width) / dstCols;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            int stepX = (sx1 - sx0) > 3 ? (sx1 - sx0) / 3 : 1;

            int sumR = 0, sumG = 0, sumB = 0, n = 0;
            for (int sx = sx0; sx < sx1; sx += stepX) {
                if (info.bpp == 24) {
                    const u8 *px = &rowBuf[sx * 3]; // BMP stores BGR
                    sumB += px[0]; sumG += px[1]; sumR += px[2];
                } else {
                    u8 r, g, b;
                    rgb565_to_rgb888(rd_u16(&rowBuf[sx * 2]), &r, &g, &b);
                    sumR += r; sumG += g; sumB += b;
                }
                n++;
            }

            u8 *dst = &outRGB[((dstY0 + r) * cols + (dstX0 + c)) * 3];
            dst[0] = (u8)(sumR / n);
            dst[1] = (u8)(sumG / n);
            dst[2] = (u8)(sumB / n);
        }
    }

    free(rowBuf);
    fclose(f);
    return ok;
}
