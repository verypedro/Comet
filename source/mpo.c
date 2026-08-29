#include "mpo.h"
#include "stb_image_write.h"

typedef struct {
    u8    *data;
    size_t len;
    size_t cap;
} ByteBuf;

static void bytebuf_append(void *context, void *chunk, int size)
{
    ByteBuf *b = (ByteBuf *)context;
    size_t need = b->len + (size_t)size;
    if (need > b->cap) {
        size_t newCap = b->cap ? b->cap * 2 : 4096;
        while (newCap < need) newCap *= 2;
        u8 *grown = (u8 *)realloc(b->data, newCap);
        if (!grown) return;
        b->data = grown;
        b->cap = newCap;
    }
    memcpy(b->data + b->len, chunk, (size_t)size);
    b->len += (size_t)size;
}

static bool encode_jpeg(const RGBImage *img, ByteBuf *out)
{
    memset(out, 0, sizeof(*out));
    int ok = stbi_write_jpg_to_func(bytebuf_append, out,
                                     img->width, img->height, 3,
                                     img->pixels, JPEG_QUALITY);
    return ok != 0 && out->data != NULL && out->len > 0;
}

// ---------------------------------------------------------------------
// stb_image_write's JPEG encoder outputs a bare-bones stream (SOI,
// tables, scan data, EOI) with no JFIF, EXIF, or MPF markers. Real
// cameras (and the 3DS's own decoder, it turns out) expect at least a
// JFIF header to recognize the format, an EXIF DateTime so the album
// doesn't show 1/1/1900, and -- critically for a *stereo* photo -- an
// MPF (Multi-Picture Format, CIPA DC-007) APP2 segment that explicitly
// declares "this file contains 2 related images" and how they're typed
// and located. Without MPF, decoders have no reason to treat the second
// JPEG stream as anything but garbage trailing the first image's EOI.
// ---------------------------------------------------------------------

#define JFIF_APP0_SIZE 18
#define EXIF_APP1_SIZE 56
#define MPF_APP2_SIZE  90

static void build_jfif_app0(u8 out[JFIF_APP0_SIZE])
{
    size_t i = 0;
    out[i++] = 0xFF; out[i++] = 0xE0;             // APP0 marker
    out[i++] = 0x00; out[i++] = 0x10;             // segment length = 16
    out[i++] = 'J'; out[i++] = 'F'; out[i++] = 'I'; out[i++] = 'F'; out[i++] = 0x00;
    out[i++] = 0x01; out[i++] = 0x01;             // JFIF version 1.1
    out[i++] = 0x00;                              // no density units
    out[i++] = 0x00; out[i++] = 0x01;             // Xdensity = 1
    out[i++] = 0x00; out[i++] = 0x01;             // Ydensity = 1
    out[i++] = 0x00;                              // thumbnail width = 0
    out[i++] = 0x00;                              // thumbnail height = 0
}

static void build_exif_app1(const char *exifDateTime19, u8 out[EXIF_APP1_SIZE])
{
    size_t i = 0;
    out[i++] = 0xFF; out[i++] = 0xE1;             // APP1 marker
    out[i++] = 0x00; out[i++] = 0x36;             // segment length = 54

    memcpy(&out[i], "Exif\0\0", 6); i += 6;

    out[i++] = 'I'; out[i++] = 'I';               // little-endian TIFF
    out[i++] = 0x2A; out[i++] = 0x00;
    out[i++] = 0x08; out[i++] = 0x00; out[i++] = 0x00; out[i++] = 0x00; // IFD0 @ 8

    out[i++] = 0x01; out[i++] = 0x00;             // 1 entry

    out[i++] = 0x32; out[i++] = 0x01;             // tag 0x0132 DateTime
    out[i++] = 0x02; out[i++] = 0x00;             // type ASCII
    out[i++] = 0x14; out[i++] = 0x00; out[i++] = 0x00; out[i++] = 0x00; // count 20
    out[i++] = 0x1A; out[i++] = 0x00; out[i++] = 0x00; out[i++] = 0x00; // value @ 26

    out[i++] = 0x00; out[i++] = 0x00; out[i++] = 0x00; out[i++] = 0x00; // next IFD

    char dt[20] = {0};
    memcpy(dt, exifDateTime19, 19); // dt[19] stays the zero-initializer's null terminator
    memcpy(&out[i], dt, 20); i += 20;
}

// Builds the APP2 "MPF" segment for a 2-image stereo pair. `leftFinalLen`
// and `rightFinalLen` are the *final* sizes of each JPEG stream (after
// all marker insertion), and `rightOffsetFromMpfBase` is the byte offset
// from just after the "MPF\0" tag to the right image's SOI marker.
//
// Layout mirrors the standard CIPA DC-007 example almost byte-for-byte:
// TIFF header -> MP Index IFD (MPFVersion, NumberOfImages, MPEntry) ->
// the 2x16-byte MP Entry array itself.
static void build_mpf_app2(u32 leftFinalLen, u32 rightFinalLen,
                            u32 rightOffsetFromMpfBase, u8 out[MPF_APP2_SIZE])
{
    size_t i = 0;
    out[i++] = 0xFF; out[i++] = 0xE2;             // APP2 marker
    out[i++] = 0x00; out[i++] = 0x58;             // segment length = 88

    memcpy(&out[i], "MPF\0", 4); i += 4;          // MPF base starts right after this

    out[i++] = 'I'; out[i++] = 'I';               // little-endian TIFF
    out[i++] = 0x2A; out[i++] = 0x00;
    out[i++] = 0x08; out[i++] = 0x00; out[i++] = 0x00; out[i++] = 0x00; // IFD0 @ 8

    // MP Index IFD: 3 entries
    out[i++] = 0x03; out[i++] = 0x00;

    // MPFVersion (0xB000), UNDEFINED, count 4, value "0100" (fits inline)
    out[i++] = 0x00; out[i++] = 0xB0;
    out[i++] = 0x07; out[i++] = 0x00;
    out[i++] = 0x04; out[i++] = 0x00; out[i++] = 0x00; out[i++] = 0x00;
    out[i++] = '0'; out[i++] = '1'; out[i++] = '0'; out[i++] = '0';

    // NumberOfImages (0xB001), LONG, count 1, value 2 (fits inline)
    out[i++] = 0x01; out[i++] = 0xB0;
    out[i++] = 0x04; out[i++] = 0x00;
    out[i++] = 0x01; out[i++] = 0x00; out[i++] = 0x00; out[i++] = 0x00;
    out[i++] = 0x02; out[i++] = 0x00; out[i++] = 0x00; out[i++] = 0x00;

    // MPEntry (0xB002), UNDEFINED, count 32 (2 entries x 16 bytes),
    // value offset = 50 (relative to MPF base -- right after this IFD)
    out[i++] = 0x02; out[i++] = 0xB0;
    out[i++] = 0x07; out[i++] = 0x00;
    out[i++] = 0x20; out[i++] = 0x00; out[i++] = 0x00; out[i++] = 0x00;
    out[i++] = 0x32; out[i++] = 0x00; out[i++] = 0x00; out[i++] = 0x00;

    out[i++] = 0x00; out[i++] = 0x00; out[i++] = 0x00; out[i++] = 0x00; // next IFD

    // --- MP Entry array (16 bytes each) ---

    // Entry 1: left/primary image. Attribute = Representative(bit 29) |
    // Format=JPEG | Type=0x020002 (Multi-frame Disparity, per CIPA spec
    // for stereo pair individual images).
    u32 attr0 = 0x20000000u | 0x00000000u | 0x00020002u;
    out[i++] = (u8)(attr0);       out[i++] = (u8)(attr0 >> 8);
    out[i++] = (u8)(attr0 >> 16); out[i++] = (u8)(attr0 >> 24);
    out[i++] = (u8)(leftFinalLen);       out[i++] = (u8)(leftFinalLen >> 8);
    out[i++] = (u8)(leftFinalLen >> 16); out[i++] = (u8)(leftFinalLen >> 24);
    out[i++] = 0x00; out[i++] = 0x00; out[i++] = 0x00; out[i++] = 0x00; // offset 0 = "this file"
    out[i++] = 0x00; out[i++] = 0x00; // dependent 1
    out[i++] = 0x00; out[i++] = 0x00; // dependent 2

    // Entry 2: right/secondary image. Same Type (Disparity), no
    // Representative flag.
    u32 attr1 = 0x00000000u | 0x00000000u | 0x00020002u;
    out[i++] = (u8)(attr1);       out[i++] = (u8)(attr1 >> 8);
    out[i++] = (u8)(attr1 >> 16); out[i++] = (u8)(attr1 >> 24);
    out[i++] = (u8)(rightFinalLen);       out[i++] = (u8)(rightFinalLen >> 8);
    out[i++] = (u8)(rightFinalLen >> 16); out[i++] = (u8)(rightFinalLen >> 24);
    out[i++] = (u8)(rightOffsetFromMpfBase);       out[i++] = (u8)(rightOffsetFromMpfBase >> 8);
    out[i++] = (u8)(rightOffsetFromMpfBase >> 16); out[i++] = (u8)(rightOffsetFromMpfBase >> 24);
    out[i++] = 0x00; out[i++] = 0x00;
    out[i++] = 0x00; out[i++] = 0x00;
}

static void timestamp_to_exif_datetime(const char *ts, char out[20])
{
    int y = 2000, mo = 1, d = 1, h = 0, mi = 0, s = 0;
    sscanf(ts, "%4d-%2d-%2d_%2d-%2d-%2d", &y, &mo, &d, &h, &mi, &s);
    snprintf(out, 20, "%04d:%02d:%02d %02d:%02d:%02d", y, mo, d, h, mi, s);
}

static bool jpeg_insert_after_soi(ByteBuf *jpeg, const u8 *insert, size_t insertLen)
{
    if (jpeg->len < 2 || jpeg->data[0] != 0xFF || jpeg->data[1] != 0xD8) return false;

    size_t newLen = jpeg->len + insertLen;
    u8 *newData = (u8 *)malloc(newLen);
    if (!newData) return false;

    memcpy(newData, jpeg->data, 2);
    memcpy(newData + 2, insert, insertLen);
    memcpy(newData + 2 + insertLen, jpeg->data + 2, jpeg->len - 2);

    free(jpeg->data);
    jpeg->data = newData;
    jpeg->len  = newLen;
    jpeg->cap  = newLen;
    return true;
}

bool mpo_write(const RGBImage *left, const RGBImage *right,
                const char *timestamp,
                const char *outPath, char *outErr, size_t outErrSize)
{
    if (left->width != right->width || left->height != right->height) {
        snprintf(outErr, outErrSize,
                 "Left/right images differ in size (%dx%d vs %dx%d)",
                 left->width, left->height, right->width, right->height);
        return false;
    }

    ByteBuf leftJpeg, rightJpeg;
    if (!encode_jpeg(left, &leftJpeg)) {
        snprintf(outErr, outErrSize, "Failed to JPEG-encode left frame");
        return false;
    }
    if (!encode_jpeg(right, &rightJpeg)) {
        snprintf(outErr, outErrSize, "Failed to JPEG-encode right frame");
        free(leftJpeg.data);
        return false;
    }

    // Work out final sizes/offsets *before* building the MPF segment --
    // every size involved (marker segment sizes) is fixed/known ahead of
    // time, so there's no circular dependency.
    u32 originalLeftLen  = (u32)leftJpeg.len;
    u32 originalRightLen = (u32)rightJpeg.len;

    u32 leftInsertLen  = EXIF_APP1_SIZE + JFIF_APP0_SIZE + MPF_APP2_SIZE;
    u32 rightInsertLen = JFIF_APP0_SIZE;

    u32 finalLeftLen  = originalLeftLen  + leftInsertLen;
    u32 finalRightLen = originalRightLen + rightInsertLen;

    // MPF base = position right after "MPF\0", measured from the start
    // of the left JPEG: SOI(2) + EXIF(56) + JFIF(18) + APP2 marker&len(4) + "MPF\0"(4)
    u32 mpfBaseAbsolute = 2 + EXIF_APP1_SIZE + JFIF_APP0_SIZE + 4 + 4;
    u32 rightOffsetFromMpfBase = finalLeftLen - mpfBaseAbsolute;

    char exifDate[20];
    timestamp_to_exif_datetime(timestamp, exifDate);
    u8 exifApp1[EXIF_APP1_SIZE];
    build_exif_app1(exifDate, exifApp1);

    u8 jfifApp0[JFIF_APP0_SIZE];
    build_jfif_app0(jfifApp0);

    u8 mpfApp2[MPF_APP2_SIZE];
    build_mpf_app2(finalLeftLen, finalRightLen, rightOffsetFromMpfBase, mpfApp2);

    u8 leftMarkers[EXIF_APP1_SIZE + JFIF_APP0_SIZE + MPF_APP2_SIZE];
    memcpy(leftMarkers, exifApp1, EXIF_APP1_SIZE);
    memcpy(leftMarkers + EXIF_APP1_SIZE, jfifApp0, JFIF_APP0_SIZE);
    memcpy(leftMarkers + EXIF_APP1_SIZE + JFIF_APP0_SIZE, mpfApp2, MPF_APP2_SIZE);

    if (!jpeg_insert_after_soi(&leftJpeg, leftMarkers, sizeof(leftMarkers))) {
        snprintf(outErr, outErrSize, "Failed to attach metadata to left frame");
        free(leftJpeg.data);
        free(rightJpeg.data);
        return false;
    }

    if (!jpeg_insert_after_soi(&rightJpeg, jfifApp0, JFIF_APP0_SIZE)) {
        snprintf(outErr, outErrSize, "Failed to attach metadata to right frame");
        free(leftJpeg.data);
        free(rightJpeg.data);
        return false;
    }

    FILE *f = fopen(outPath, "wb");
    if (!f) {
        snprintf(outErr, outErrSize, "Couldn't create %s", outPath);
        free(leftJpeg.data);
        free(rightJpeg.data);
        return false;
    }

    bool ok = fwrite(leftJpeg.data, 1, leftJpeg.len, f) == leftJpeg.len &&
              fwrite(rightJpeg.data, 1, rightJpeg.len, f) == rightJpeg.len;

    fclose(f);
    free(leftJpeg.data);
    free(rightJpeg.data);

    if (!ok) {
        snprintf(outErr, outErrSize, "Failed writing MPO data to disk");
        return false;
    }
    return true;
}

// Single-image JPEG for 2D screenshots. Same EXIF + JFIF headers the
// left eye of an MPO gets, minus the MPF segment that declares a
// stereo pair -- the 3DS Camera app reads plain JPEGs happily, so a
// 2D screenshot needs no special treatment beyond a .JPG extension.
bool jpg_write(const RGBImage *img, const char *timestamp,
                const char *outPath, char *outErr, size_t outErrSize)
{
    ByteBuf jpeg;
    if (!encode_jpeg(img, &jpeg)) {
        snprintf(outErr, outErrSize, "Failed to JPEG-encode screenshot");
        return false;
    }

    char exifDate[20];
    timestamp_to_exif_datetime(timestamp, exifDate);
    u8 exifApp1[EXIF_APP1_SIZE];
    build_exif_app1(exifDate, exifApp1);
    u8 jfifApp0[JFIF_APP0_SIZE];
    build_jfif_app0(jfifApp0);

    u8 markers[EXIF_APP1_SIZE + JFIF_APP0_SIZE];
    memcpy(markers, exifApp1, EXIF_APP1_SIZE);
    memcpy(markers + EXIF_APP1_SIZE, jfifApp0, JFIF_APP0_SIZE);

    if (!jpeg_insert_after_soi(&jpeg, markers, sizeof(markers))) {
        snprintf(outErr, outErrSize, "Failed to attach metadata");
        free(jpeg.data);
        return false;
    }

    FILE *f = fopen(outPath, "wb");
    if (!f) {
        snprintf(outErr, outErrSize, "Couldn't create %s", outPath);
        free(jpeg.data);
        return false;
    }
    bool ok = fwrite(jpeg.data, 1, jpeg.len, f) == jpeg.len;
    fclose(f);
    free(jpeg.data);

    if (!ok) {
        snprintf(outErr, outErrSize, "Failed writing JPEG data to disk");
        return false;
    }
    return true;
}
