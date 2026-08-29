#include "ds_utils.h"
#include "fs_utils.h"
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

// ---- TAR walking --------------------------------------------------
//
// TAR is uncompressed: a 512-byte header per entry, then the payload
// padded up to the next 512-byte boundary. Only two header fields
// matter here -- the size (offset 124, as octal ASCII) and the type
// flag (offset 156). Everything else is skipped.
//
// The type flag check matters: some tar writers interleave extended
// metadata entries ('x'/'g') between the real files, and treating one
// of those as a screenshot slot would throw off every subsequent
// offset. Only '0' and '\0' mean "regular file".

#define TAR_BLOCK 512

static bool tar_block_is_zero(const u8 *blk)
{
    for (int i = 0; i < TAR_BLOCK; i++) if (blk[i]) return false;
    return true;
}

static long tar_parse_octal(const char *field, int len)
{
    long v = 0;
    for (int i = 0; i < len; i++) {
        char ch = field[i];
        if (ch == ' ' || ch == '\0') break;
        if (ch < '0' || ch > '7') continue;
        v = v * 8 + (ch - '0');
    }
    return v;
}

// Calls back once per regular-file entry with its payload offset+size.
// Returning false from the callback stops the walk early.
typedef bool (*TarEntryFn)(const char *name, long dataOffset, long size, void *ctx);

static bool tar_walk(FILE *f, TarEntryFn fn, void *ctx)
{
    u8 header[TAR_BLOCK];
    if (fseek(f, 0, SEEK_SET) != 0) return false;

    while (fread(header, 1, TAR_BLOCK, f) == TAR_BLOCK) {
        if (tar_block_is_zero(header)) break; // end-of-archive marker

        char name[101];
        memcpy(name, header, 100);
        name[100] = '\0';

        long size = tar_parse_octal((const char *)&header[124], 12);
        if (size < 0) break;

        char type = (char)header[156];
        long dataOffset = ftell(f);

        if (type == '0' || type == '\0') {
            if (!fn(name, dataOffset, size, ctx)) return true;
        }

        long padded = ((size + TAR_BLOCK - 1) / TAR_BLOCK) * TAR_BLOCK;
        if (fseek(f, dataOffset + padded, SEEK_SET) != 0) break;
    }
    return true;
}

// A slot holds a real screenshot iff its payload opens with "BM".
static bool slot_is_real(FILE *f, long dataOffset, long size)
{
    if (size < 2) return false;
    long save = ftell(f);
    u8 sig[2] = {0, 0};
    bool ok = (fseek(f, dataOffset, SEEK_SET) == 0) &&
              (fread(sig, 1, 2, f) == 2);
    fseek(f, save, SEEK_SET);
    return ok && sig[0] == 'B' && sig[1] == 'M';
}

// ---- counting -----------------------------------------------------

typedef struct { FILE *f; int count; } CountCtx;

static bool count_cb(const char *name, long off, long size, void *vctx)
{
    (void)name;
    CountCtx *ctx = (CountCtx *)vctx;
    if (slot_is_real(ctx->f, off, size)) ctx->count++;
    return true;
}

int ds_count_tar_screenshots(void)
{
    FILE *f = fopen(SD_ROOT DS_TAR_PATH, "rb");
    if (!f) return 0;
    CountCtx ctx = { f, 0 };
    tar_walk(f, count_cb, &ctx);
    fclose(f);
    return ctx.count;
}

// ---- extraction ---------------------------------------------------

typedef struct {
    FILE *f;
    int   written;
    int   slot;        // 1-based position within the tar
    char  stamp[24];   // shared across one import run
    char  err[128];
    bool  failed;
} ExtractCtx;

static bool extract_cb(const char *name, long off, long size, void *vctx)
{
    (void)name;
    ExtractCtx *ctx = (ExtractCtx *)vctx;
    ctx->slot++;

    if (ctx->failed) return false;
    if (!slot_is_real(ctx->f, off, size)) return true;

    u8 *buf = (u8 *)malloc((size_t)size);
    if (!buf) {
        snprintf(ctx->err, sizeof(ctx->err), "Out of memory reading slot %d", ctx->slot);
        ctx->failed = true;
        return false;
    }

    long save = ftell(ctx->f);
    bool ok = (fseek(ctx->f, off, SEEK_SET) == 0) &&
              (fread(buf, 1, (size_t)size, ctx->f) == (size_t)size);
    fseek(ctx->f, save, SEEK_SET);

    if (ok) {
        // Slot number in the filename guarantees uniqueness even when
        // several slots are written within the same second.
        char path[320];
        snprintf(path, sizeof(path), "%s%s/%s_slot%02d.bmp",
                 SD_ROOT, DS_SCREENSHOTS_DIR, ctx->stamp, ctx->slot);
        ok = fs_write_file(path, buf, (size_t)size);
        if (ok) ctx->written++;
    }

    free(buf);

    if (!ok) {
        snprintf(ctx->err, sizeof(ctx->err), "Failed writing slot %d to SD", ctx->slot);
        ctx->failed = true;
        return false;
    }
    return true;
}

int ds_extract_all(char *outErr, size_t outErrSize)
{
    if (!fs_ensure_dir_exists(SD_ROOT DS_SCREENSHOTS_DIR)) {
        snprintf(outErr, outErrSize, "Couldn't create %s", DS_SCREENSHOTS_DIR);
        return -1;
    }

    FILE *f = fopen(SD_ROOT DS_TAR_PATH, "rb");
    if (!f) {
        snprintf(outErr, outErrSize, "Couldn't open screenshots.tar");
        return -1;
    }

    ExtractCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.f = f;

    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    if (lt) {
        strftime(ctx.stamp, sizeof(ctx.stamp), "%Y-%m-%d_%H-%M-%S", lt);
    } else {
        snprintf(ctx.stamp, sizeof(ctx.stamp), "0000-00-00_00-00-00");
    }

    tar_walk(f, extract_cb, &ctx);
    fclose(f);

    if (ctx.failed) {
        snprintf(outErr, outErrSize, "%s", ctx.err);
        return ctx.written > 0 ? ctx.written : -1;
    }
    return ctx.written;
}

typedef struct { FILE *f; int blanked; bool failed; } BlankCtx;

static bool blank_cb(const char *name, long off, long size, void *vctx)
{
    (void)name;
    BlankCtx *ctx = (BlankCtx *)vctx;
    if (!slot_is_real(ctx->f, off, size)) return true;

    // Overwrite the payload with zeros, matching exactly what an
    // untouched slot looks like. The tar's own headers and overall
    // length are never touched.
    static u8 zeros[4096];
    long remaining = size;
    if (fseek(ctx->f, off, SEEK_SET) != 0) { ctx->failed = true; return false; }
    while (remaining > 0) {
        size_t chunk = (remaining > (long)sizeof(zeros)) ? sizeof(zeros) : (size_t)remaining;
        if (fwrite(zeros, 1, chunk, ctx->f) != chunk) { ctx->failed = true; return false; }
        remaining -= (long)chunk;
    }
    ctx->blanked++;
    return true;
}

bool ds_clear_tar(void)
{
    FILE *f = fopen(SD_ROOT DS_TAR_PATH, "r+b");
    if (!f) return false;

    BlankCtx ctx = { f, 0, false };
    tar_walk(f, blank_cb, &ctx);
    fflush(f);
    fclose(f);
    return !ctx.failed;
}

// ---- extracted-folder listing --------------------------------------

static bool ds_name_to_timestamp(const char *filename, char *out, size_t outSize)
{
    // "YYYY-MM-DD_HH-MM-SS_slotNN.bmp" -> "YYYY-MM-DD_HH-MM-SS"
    if (strlen(filename) < 19) return false;
    size_t n = 19;
    if (n >= outSize) n = outSize - 1;
    memcpy(out, filename, n);
    out[n] = '\0';
    return true;
}

static int cmp_ds_desc(const void *a, const void *b)
{
    const DSScreenshot *x = (const DSScreenshot *)a;
    const DSScreenshot *y = (const DSScreenshot *)b;
    return strcmp(y->path, x->path); // newest first (names sort lexically by time)
}

int ds_scan_extracted(DSScreenshot *out, int max)
{
    DIR *dir = opendir(SD_ROOT DS_SCREENSHOTS_DIR);
    if (!dir) return 0;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && count < max) {
        const char *nm = ent->d_name;
        size_t len = strlen(nm);
        if (len < 5) continue;
        if (strcasecmp(nm + len - 4, ".bmp") != 0) continue;

        DSScreenshot *s = &out[count];
        snprintf(s->path, sizeof(s->path), "%s%s/%s", SD_ROOT, DS_SCREENSHOTS_DIR, nm);
        if (!ds_name_to_timestamp(nm, s->timestamp, sizeof(s->timestamp))) {
            snprintf(s->timestamp, sizeof(s->timestamp), "%s", nm);
        }
        s->widescreen = false; // safe default -- see the struct's own comment
        count++;
    }
    closedir(dir);

    if (count > 1) qsort(out, count, sizeof(DSScreenshot), cmp_ds_desc);
    return count;
}

int ds_count_extracted(void)
{
    DIR *dir = opendir(SD_ROOT DS_SCREENSHOTS_DIR);
    if (!dir) return 0;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *nm = ent->d_name;
        size_t len = strlen(nm);
        if (len >= 5 && strcasecmp(nm + len - 4, ".bmp") == 0) count++;
    }
    closedir(dir);
    return count;
}

bool ds_delete(const DSScreenshot *s)
{
    return remove(s->path) == 0;
}

// ---- addressing individual slots in place ------------------------

typedef struct { FILE *f; DSTarSlot *out; int max; int count; int slot; bool all; } ListCtx;

static bool list_cb(const char *name, long off, long size, void *vctx)
{
    (void)name;
    ListCtx *ctx = (ListCtx *)vctx;
    ctx->slot++;
    if (!ctx->all && !slot_is_real(ctx->f, off, size)) return true;
    if (ctx->count >= ctx->max) return false;
    DSTarSlot *s = &ctx->out[ctx->count++];
    s->dataOffset = off;
    s->size = size;
    s->slot = ctx->slot;
    s->widescreen = false;
    return true;
}

int ds_list_tar_slots(DSTarSlot *out, int max)
{
    FILE *f = fopen(SD_ROOT DS_TAR_PATH, "rb");
    if (!f) return 0;
    ListCtx ctx = { f, out, max, 0, 0, false };
    tar_walk(f, list_cb, &ctx);
    fclose(f);
    return ctx.count;
}

// Every physical slot, occupied or not, in tar order.
static int ds_list_all_slots(FILE *f, DSTarSlot *out, int max)
{
    ListCtx ctx = { f, out, max, 0, 0, true };
    tar_walk(f, list_cb, &ctx);
    return ctx.count;
}

bool ds_extract_slot(const DSTarSlot *slot, char *outErr, size_t outErrSize)
{
    if (!fs_ensure_dir_exists(SD_ROOT DS_SCREENSHOTS_DIR)) {
        snprintf(outErr, outErrSize, "Couldn't create %s", DS_SCREENSHOTS_DIR);
        return false;
    }

    FILE *f = fopen(SD_ROOT DS_TAR_PATH, "rb");
    if (!f) { snprintf(outErr, outErrSize, "Couldn't open screenshots.tar"); return false; }

    u8 *buf = (u8 *)malloc((size_t)slot->size);
    if (!buf) { fclose(f); snprintf(outErr, outErrSize, "Out of memory"); return false; }

    bool ok = (fseek(f, slot->dataOffset, SEEK_SET) == 0) &&
              (fread(buf, 1, (size_t)slot->size, f) == (size_t)slot->size);
    fclose(f);

    if (ok) {
        char stamp[24];
        time_t now = time(NULL);
        struct tm *lt = localtime(&now);
        if (lt) strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", lt);
        else    snprintf(stamp, sizeof(stamp), "0000-00-00_00-00-00");

        char path[320];
        snprintf(path, sizeof(path), "%s%s/%s_slot%02d.bmp",
                 SD_ROOT, DS_SCREENSHOTS_DIR, stamp, slot->slot);
        ok = fs_write_file(path, buf, (size_t)slot->size);
    }
    free(buf);


    if (!ok) snprintf(outErr, outErrSize, "Couldn't extract screenshot %d", slot->slot);
    return ok;
}

// Writes `size` bytes from src offset to dst offset within one open
// file, chunked so a full 98KB screenshot doesn't need a 98KB buffer.
static bool move_payload(FILE *f, long from, long to, long size)
{
    if (from == to) return true;
    static u8 chunk[8192];
    long done = 0;
    while (done < size) {
        long n = size - done;
        if (n > (long)sizeof(chunk)) n = (long)sizeof(chunk);
        if (fseek(f, from + done, SEEK_SET) != 0) return false;
        if (fread(chunk, 1, (size_t)n, f) != (size_t)n) return false;
        if (fseek(f, to + done, SEEK_SET) != 0) return false;
        if (fwrite(chunk, 1, (size_t)n, f) != (size_t)n) return false;
        done += n;
    }
    return true;
}

static bool blank_payload(FILE *f, long at, long size)
{
    static u8 zeros[8192];
    long done = 0;
    if (fseek(f, at, SEEK_SET) != 0) return false;
    while (done < size) {
        long n = size - done;
        if (n > (long)sizeof(zeros)) n = (long)sizeof(zeros);
        if (fwrite(zeros, 1, (size_t)n, f) != (size_t)n) return false;
        done += n;
    }
    return true;
}

bool ds_delete_tar_slot(int slotIndex)
{
    FILE *f = fopen(SD_ROOT DS_TAR_PATH, "r+b");
    if (!f) return false;

    DSTarSlot all[MAX_DS_TAR_SLOTS];
    int allCount = ds_list_all_slots(f, all, MAX_DS_TAR_SLOTS);

    // Which physical slots currently hold a real screenshot.
    int occupied[MAX_DS_TAR_SLOTS];
    int occupiedCount = 0;
    for (int i = 0; i < allCount; i++) {
        if (slot_is_real(f, all[i].dataOffset, all[i].size)) {
            occupied[occupiedCount++] = i;
        }
    }

    if (slotIndex < 0 || slotIndex >= occupiedCount) { fclose(f); return false; }

    // Survivors, in order, minus the one being deleted.
    int survivors[MAX_DS_TAR_SLOTS];
    int survivorCount = 0;
    for (int i = 0; i < occupiedCount; i++) {
        if (i != slotIndex) survivors[survivorCount++] = occupied[i];
    }

    bool ok = true;

    // Repack into physical slots 0..survivorCount-1. A survivor is
    // never at a physical position lower than its destination index,
    // so every move goes backwards in the file and a forward loop can
    // never clobber data it hasn't read yet.
    for (int i = 0; i < survivorCount && ok; i++) {
        ok = move_payload(f, all[survivors[i]].dataOffset,
                             all[i].dataOffset,
                             all[survivors[i]].size);
    }

    // Blank everything past the repacked run, so the first blank slot
    // marks the true end of the library.
    for (int i = survivorCount; i < allCount && ok; i++) {
        ok = blank_payload(f, all[i].dataOffset, all[i].size);
    }

    fflush(f);
    fclose(f);
    return ok;
}

unsigned long ds_slot_fingerprint(const DSTarSlot *slot)
{
    FILE *f = fopen(SD_ROOT DS_TAR_PATH, "rb");
    if (!f) return 0;

    // Sample a few spans spread through the payload rather than
    // hashing all ~98KB -- enough to distinguish real screenshots,
    // cheap enough to run for every slot on every listing.
    const long offsets[] = { 512, 8192, 32768, 65536 };
    unsigned long h = 1469598103u; // FNV-ish seed
    u8 buf[128];

    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        long at = slot->dataOffset + offsets[i];
        if (offsets[i] + (long)sizeof(buf) > slot->size) break;
        if (fseek(f, at, SEEK_SET) != 0) break;
        size_t n = fread(buf, 1, sizeof(buf), f);
        for (size_t j = 0; j < n; j++) {
            h ^= buf[j];
            h *= 16777619u;
        }
    }
    fclose(f);

    // Fold the size in too, so differently-sized payloads can't collide.
    h ^= (unsigned long)slot->size;
    return h;
}
