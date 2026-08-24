#include "ui.h"
#include "bmp.h"
#include "mpo.h"
#include "fs_utils.h"
#include "assets/icons_data.h"
#include "audio.h"
#include <stdarg.h>
#include <math.h>
#include <3ds/thread.h>
#include <3ds/synchronization.h>
#define PI_F 3.14159265f

// ---------------------------------------------------------------------
// v5.2: stereo 3D live preview is back (safe now that threading is
// confirmed working), and the expensive per-pixel swizzle computation
// moved from the main thread to the background worker -- it's pure
// memory/math with no citro2d/citro3d calls involved, so it was safe
// to move all along; only the actual GPU texture allocation needs to
// stay on the main thread. The worker now hands over an
// already-swizzled buffer, and the main thread just does a fast bulk
// memcpy into the texture instead of a per-pixel loop. See README
// changelog for full history.
// ---------------------------------------------------------------------

typedef enum {
    APP_BROWSE,
    APP_DETAIL,               // single-item Copy to Album / Delete menu
    APP_DETAIL_DELETE_CONFIRM,
    APP_DETAIL_DELETING,
    APP_CONVERTING,
    APP_RESULT,
    APP_BATCH_CONVERTING,
    APP_BATCH_RESULT,         // now doubles as the "copied -- delete originals?" prompt
    APP_BATCH_DELETE_CONFIRM, // standalone batch delete (X), no copy involved
    APP_BATCH_DELETING,       // shared by both the standalone and post-copy delete paths
    APP_FILTER_MENU,          // All Screenshots / 3D Only / 2D Only / By Date
    APP_FILTER_BY_DATE,       // Year / Month / Day rows (Day only shown once Month is specific)
    APP_FILTER_MONTH_PICKER,
    APP_FILTER_YEAR_PICKER,
    APP_FILTER_DAY_PICKER,
} AppState;

static C3D_RenderTarget *s_top, *s_topRight, *s_bot;
static C2D_TextBuf s_dynBuf;
static C2D_Font s_fontRegular = NULL;  // Poppins Regular 12 -- footer, menus, popups
static C2D_Font s_fontSemiBold = NULL; // Poppins SemiBold 12 -- header title only

// The fonts are generated with `mkbcfnt -s 12`, so one glyph-em is
// ~12px at scale 1.0. The design spec is Poppins 12 nearly everywhere
// and Poppins 9 for the header's right-hand label, so these map
// directly. (The old 0.32-0.46 values were tuned for the 3DS system
// font, whose glyph cell is 30px -- applying those to a 12px bitmap
// font is what made everything render tiny.)
#define TEXT_12 0.64f
#define TEXT_9  0.5f

static AppState s_state = APP_BROWSE;

static ScreenshotPair s_pairs[MAX_PAIRS];
static int s_pairCount = 0;

// Filter system: s_visibleIndices[0..s_visibleCount) are indices into
// s_pairs that pass the current filter. All list navigation/selection
// operates on this filtered view; s_selected indexes into it, not into
// s_pairs directly.
typedef enum {
    FILTER_ALL,
    FILTER_3D_ONLY,
    FILTER_2D_ONLY,
    FILTER_BY_DATE,
} FilterMode;

static FilterMode s_filterMode = FILTER_ALL;
static int s_filterYear = 0;  // 0 = not yet set (Year has no "All" option -- always required)
static int s_filterMonth = 0; // 1-12, 0 = "All" (a real, selectable option, not just "unset")
static int s_filterDay = 0;   // 1-31, 0 = "All"; irrelevant/ignored whenever month is "All"
static int s_filterCursor = 0; // meaning depends on current screen (menu row / month / year / day index)

static const char *const MONTH_NAMES[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

// Parses Luma's "YYYY-MM-DD_HH-MM-SS.mmm" timestamp into year/month/day
// (same sscanf pattern already proven in mpo.c's EXIF DateTime
// conversion). Returns false if the string doesn't parse.
static bool parse_timestamp_ymd(const char *ts, int *outYear, int *outMonth, int *outDay)
{
    int y, mo, d, h, mi, s;
    if (sscanf(ts, "%4d-%2d-%2d_%2d-%2d-%2d", &y, &mo, &d, &h, &mi, &s) < 3) return false;
    *outYear = y;
    *outMonth = mo;
    *outDay = d;
    return true;
}

static bool pair_matches_filter(int idx)
{
    switch (s_filterMode) {
    case FILTER_ALL:      return true;
    case FILTER_3D_ONLY:  return s_pairs[idx].has3D;
    case FILTER_2D_ONLY:  return !s_pairs[idx].has3D;
    case FILTER_BY_DATE: {
        int y, mo, d;
        if (!parse_timestamp_ymd(s_pairs[idx].timestamp, &y, &mo, &d)) return false;
        if (y != s_filterYear) return false;
        if (s_filterMonth != 0 && mo != s_filterMonth) return false;
        if (s_filterMonth != 0 && s_filterDay != 0 && d != s_filterDay) return false;
        return true;
    }
    }
    return true;
}

// Distinct years actually present in the screenshot library, sorted
// ascending, most-recent last -- so the Year picker only ever shows
// real options instead of an arbitrary fixed range.
#define MAX_FILTER_YEARS 16
static int s_availableYears[MAX_FILTER_YEARS];
static int s_availableYearCount = 0;

static void scan_available_years(void)
{
    s_availableYearCount = 0;
    for (int i = 0; i < s_pairCount && s_availableYearCount < MAX_FILTER_YEARS; i++) {
        int y, mo, d;
        if (!parse_timestamp_ymd(s_pairs[i].timestamp, &y, &mo, &d)) continue;
        bool found = false;
        for (int j = 0; j < s_availableYearCount; j++) {
            if (s_availableYears[j] == y) { found = true; break; }
        }
        if (!found) s_availableYears[s_availableYearCount++] = y;
    }
    // Small N -- plain insertion sort, ascending.
    for (int i = 1; i < s_availableYearCount; i++) {
        int key = s_availableYears[i], j = i - 1;
        while (j >= 0 && s_availableYears[j] > key) {
            s_availableYears[j + 1] = s_availableYears[j];
            j--;
        }
        s_availableYears[j + 1] = key;
    }
}

// Distinct months actually present for a given year -- same reasoning
// as scan_available_years()/scan_available_days(). Without this, it
// was possible to pick a month with zero screenshots, which then left
// the Day picker with nothing to show but "All".
#define MAX_FILTER_MONTHS 12
static int s_availableMonths[MAX_FILTER_MONTHS];
static int s_availableMonthCount = 0;

static void scan_available_months(int year)
{
    s_availableMonthCount = 0;
    for (int i = 0; i < s_pairCount && s_availableMonthCount < MAX_FILTER_MONTHS; i++) {
        int y, mo, d;
        if (!parse_timestamp_ymd(s_pairs[i].timestamp, &y, &mo, &d)) continue;
        if (y != year) continue;
        bool found = false;
        for (int j = 0; j < s_availableMonthCount; j++) {
            if (s_availableMonths[j] == mo) { found = true; break; }
        }
        if (!found) s_availableMonths[s_availableMonthCount++] = mo;
    }
    for (int i = 1; i < s_availableMonthCount; i++) {
        int key = s_availableMonths[i], j = i - 1;
        while (j >= 0 && s_availableMonths[j] > key) {
            s_availableMonths[j + 1] = s_availableMonths[j];
            j--;
        }
        s_availableMonths[j + 1] = key;
    }
}

// Distinct days actually present for a given year+month, same
// reasoning as scan_available_years(). Only ever called once a
// specific month is selected (Day isn't offered while Month is "All").
#define MAX_FILTER_DAYS 32
static int s_availableDays[MAX_FILTER_DAYS];
static int s_availableDayCount = 0;

static void scan_available_days(int year, int month)
{
    s_availableDayCount = 0;
    for (int i = 0; i < s_pairCount && s_availableDayCount < MAX_FILTER_DAYS; i++) {
        int y, mo, d;
        if (!parse_timestamp_ymd(s_pairs[i].timestamp, &y, &mo, &d)) continue;
        if (y != year || mo != month) continue;
        bool found = false;
        for (int j = 0; j < s_availableDayCount; j++) {
            if (s_availableDays[j] == d) { found = true; break; }
        }
        if (!found) s_availableDays[s_availableDayCount++] = d;
    }
    for (int i = 1; i < s_availableDayCount; i++) {
        int key = s_availableDays[i], j = i - 1;
        while (j >= 0 && s_availableDays[j] > key) {
            s_availableDays[j + 1] = s_availableDays[j];
            j--;
        }
        s_availableDays[j + 1] = key;
    }
}

static int  s_visibleIndices[MAX_PAIRS];
static int  s_visibleCount = 0;
static int  s_selected = 0;

static void rebuild_visible_list(void)
{
    s_visibleCount = 0;
    for (int i = 0; i < s_pairCount; i++) {
        if (!pair_matches_filter(i)) continue;
        s_visibleIndices[s_visibleCount++] = i;
    }
    if (s_selected >= s_visibleCount) s_selected = s_visibleCount > 0 ? s_visibleCount - 1 : 0;
    if (s_selected < 0) s_selected = 0;
}

static ScreenshotPair *current_pair(void)
{
    if (s_selected < 0 || s_selected >= s_visibleCount) return NULL;
    return &s_pairs[s_visibleIndices[s_selected]];
}

// Batch Select mode.
static bool s_batchMode = false;
static bool s_batchSelected[MAX_PAIRS];
static int  s_batchTotal = 0;
static int  s_batchSucceeded = 0;
static int  s_batchIndex = 0;

static int s_flashFrames = 0;

static bool s_lastSuccess = false;
static char s_lastMessage[256];
static char s_lastOutputPath[256];

// Single-item detail view.
static int s_detailMenuSelection = 0; // 0 = Copy to 3DS Album, 1 = Delete
static int s_confirmSelection = 0;    // 0 = Cancel, 1 = Delete (defaults safe)

static float s_spinnerAngle = 0.0f;

// After a copy/delete sound fires, the grid often rebuilds and would
// otherwise fire the thumbnail click once per frame for every visible
// cell, drowning it out. This suppresses the click briefly.
static int s_thumbSfxMute = 0;

// Blocking operations (convert / delete) used to finish inside a single
// frame, so the spinner rendered once and vanished -- it never appeared
// to spin at all. These hold the popup for a minimum number of frames,
// running the actual work on frame 2 so the popup paints first.
#define OP_MIN_FRAMES 32

// Popups drop in over ~8 frames from a few pixels above their resting
// position. Purely cosmetic, but it stops them from snapping into
// existence.
#define POPUP_ANIM_FRAMES 8
static int s_popupAnim = POPUP_ANIM_FRAMES;

static bool state_has_popup(AppState st)
{
    switch (st) {
    case APP_CONVERTING:
    case APP_RESULT:
    case APP_DETAIL_DELETE_CONFIRM:
    case APP_DETAIL_DELETING:
    case APP_BATCH_CONVERTING:
    case APP_BATCH_RESULT:
    case APP_BATCH_DELETE_CONFIRM:
    case APP_BATCH_DELETING:
        return true;
    default:
        return false;
    }
}

static float popup_y_offset(void)
{
    if (s_popupAnim >= POPUP_ANIM_FRAMES) return 0.0f;
    float t = (float)s_popupAnim / (float)POPUP_ANIM_FRAMES;
    float ease = 1.0f - (1.0f - t) * (1.0f - t); // ease-out
    return -10.0f * (1.0f - ease);
}
static int      s_opFrames = 0;
static bool     s_opDone = false;
static AppState s_opNextState = APP_BROWSE;

static void op_enter(AppState st) { s_state = st; s_opFrames = 0; s_opDone = false; }
static void op_complete(AppState next) { s_opNextState = next; s_opDone = true; }

// Returns true on the single frame the caller should perform its work.
static bool op_tick(void)
{
    s_opFrames++;
    if (s_opDone) {
        if (s_opFrames >= OP_MIN_FRAMES) s_state = s_opNextState;
        return false;
    }
    return s_opFrames == 2;
}

static u32 COLOR_BG, COLOR_PANEL, COLOR_ACCENT, COLOR_TEXT, COLOR_DIM;
static u32 COLOR_OK, COLOR_ERR, COLOR_YELLOW, COLOR_POPUP_BG, COLOR_POPUP_TEXT, COLOR_OVERLAY;
static u32 COLOR_SEP_FRAME, COLOR_SEP_ROW, COLOR_SEP_POPUP;

// --- swizzle math (shared by worker thread and, if needed, a
//     synchronous fallback) -------------------------------------------
//
// Taken from the Citra 3DS emulator's own source (video_core/utils.h),
// which has to reproduce the GPU's 8x8 Z-order tile layout byte-for-byte
// to render real commercial games correctly.
static u32 next_pow2(u32 v)
{
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
    return v + 1;
}

static u32 morton_interleave(u32 x, u32 y)
{
    static const u32 xlut[8] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15};
    static const u32 ylut[8] = {0x00, 0x02, 0x08, 0x0a, 0x20, 0x22, 0x28, 0x2a};
    return xlut[x & 7] + ylut[y & 7];
}

static u32 tiled_offset_rgba8(u32 x, u32 y, u32 texW)
{
    u32 tilesPerRow = texW >> 3;
    u32 tileNum = (y >> 3) * tilesPerRow + (x >> 3);
    return (tileNum * 64 + morton_interleave(x, y)) * 4;
}

// Decodes a BMP and immediately swizzles it into a plain malloc'd RGBA8
// buffer ready to be memcpy'd straight into a GPU texture. Pure CPU/
// memory work -- no citro2d/citro3d calls -- so this is safe to run on
// the background thread.
static bool decode_and_swizzle(const char *path, u8 **outData,
                                u32 *outTexW, u32 *outTexH, u32 *outImgW, u32 *outImgH)
{
    RGBImage img;
    char err[128];
    if (!bmp_load(path, &img, err, sizeof(err))) return false;

    u32 texW = next_pow2((u32)img.width);
    u32 texH = next_pow2((u32)img.height);
    if (texW < 64) texW = 64;
    if (texH < 64) texH = 64;
    if (texW > 1024) texW = 1024;
    if (texH > 1024) texH = 1024;

    u8 *data = (u8 *)malloc((size_t)texW * texH * 4);
    if (!data) { bmp_free(&img); return false; }
    memset(data, 0, (size_t)texW * texH * 4);

    for (int y = 0; y < img.height; y++) {
        for (int x = 0; x < img.width; x++) {
            const u8 *src = &img.pixels[((size_t)y * img.width + x) * 3];
            u32 off = tiled_offset_rgba8((u32)x, (u32)y, texW);
            // GPU_RGBA8 stores bytes as A,B,G,R in memory on this
            // little-endian hardware, not R,G,B,A.
            data[off + 0] = 0xFF;
            data[off + 1] = src[2];
            data[off + 2] = src[1];
            data[off + 3] = src[0];
        }
    }

    int imgW = img.width, imgH = img.height;
    bmp_free(&img);

    *outData = data;
    *outTexW = texW; *outTexH = texH;
    *outImgW = (u32)imgW; *outImgH = (u32)imgH;
    return true;
}

// --- background preview loader --------------------------------------------

#define LOADER_STACK_SIZE (48 * 1024)

static Thread     s_loaderThread;
static LightLock   s_loaderLock;
static LightEvent  s_loaderWake;
static volatile bool s_loaderRunning = false;
static bool s_loaderAvailable = false;

typedef struct {
    char pathLeft[256];
    char pathRight[256];
    bool needRight;
} LoaderJob;

typedef struct {
    u8  *dataLeft;  u32 texWLeft,  texHLeft,  imgWLeft,  imgHLeft;
    u8  *dataRight; u32 texWRight, texHRight, imgWRight, imgHRight; // dataRight NULL if not needed/failed
} LoaderResult;

static LoaderJob s_pendingJob;
static int  s_pendingJobId = 0;
static bool s_hasPendingJob = false;

static LoaderResult s_result;
static int  s_resultId = 0;
static bool s_resultValid = false;

static int s_nextJobId = 1;

static void free_loader_result(LoaderResult *r)
{
    if (r->dataLeft)  { free(r->dataLeft);  r->dataLeft = NULL; }
    if (r->dataRight) { free(r->dataRight); r->dataRight = NULL; }
}

static void loader_thread_main(void *arg)
{
    (void)arg;
    while (s_loaderRunning) {
        LightEvent_Wait(&s_loaderWake);
        if (!s_loaderRunning) break;

        LoaderJob job;
        int jobId = 0;
        bool have = false;

        LightLock_Lock(&s_loaderLock);
        if (s_hasPendingJob) {
            job = s_pendingJob;
            jobId = s_pendingJobId;
            s_hasPendingJob = false;
            have = true;
        }
        LightLock_Unlock(&s_loaderLock);

        if (!have) continue;

        LoaderResult res = {0};
        bool okL = decode_and_swizzle(job.pathLeft, &res.dataLeft, &res.texWLeft, &res.texHLeft, &res.imgWLeft, &res.imgHLeft);
        if (!okL) continue; // couldn't even load the left eye -- skip this job

        if (job.needRight) {
            decode_and_swizzle(job.pathRight, &res.dataRight, &res.texWRight, &res.texHRight, &res.imgWRight, &res.imgHRight);
            // If this fails, res.dataRight stays NULL -- caller just falls back to a flat 2D preview.
        }

        LightLock_Lock(&s_loaderLock);
        if (s_resultValid) free_loader_result(&s_result); // drop a stale unconsumed result
        s_result = res;
        s_resultValid = true;
        s_resultId = jobId;
        LightLock_Unlock(&s_loaderLock);
    }
}

static void loader_init(void)
{
    LightLock_Init(&s_loaderLock);
    LightEvent_Init(&s_loaderWake, RESET_ONESHOT);
    s_loaderRunning = true;

    // Priority is numerically LOWER = scheduled HIGHER on this platform.
    // Without New3DS core entitlements this thread shares the same
    // single CPU core as the main thread, so giving it higher priority
    // than main (0x30) means the scheduler favors it whenever it has
    // work queued, rather than depending on same-priority round-robin
    // timing that doesn't interact predictably with citro3d's own
    // frame pacing.
    s_loaderThread = threadCreate(loader_thread_main, NULL, LOADER_STACK_SIZE, 0x25, -1, false);
    s_loaderAvailable = (s_loaderThread != NULL);
    if (!s_loaderAvailable) s_loaderRunning = false;
}

static void loader_exit(void)
{
    if (s_loaderAvailable) {
        s_loaderRunning = false;
        LightEvent_Signal(&s_loaderWake);
        threadJoin(s_loaderThread, U64_MAX);
        threadFree(s_loaderThread);
    }
    if (s_resultValid) {
        free_loader_result(&s_result);
        s_resultValid = false;
    }
}

static int loader_request(const char *pathLeft, const char *pathRight)
{
    LoaderJob job;
    snprintf(job.pathLeft, sizeof(job.pathLeft), "%s", pathLeft);
    job.needRight = (pathRight != NULL);
    if (job.needRight) snprintf(job.pathRight, sizeof(job.pathRight), "%s", pathRight);

    int id = ++s_nextJobId;

    if (s_loaderAvailable) {
        LightLock_Lock(&s_loaderLock);
        s_pendingJob = job;
        s_pendingJobId = id;
        s_hasPendingJob = true;
        LightLock_Unlock(&s_loaderLock);
        LightEvent_Signal(&s_loaderWake);
    } else {
        // Synchronous fallback if the worker thread never started.
        LoaderResult res = {0};
        if (decode_and_swizzle(job.pathLeft, &res.dataLeft, &res.texWLeft, &res.texHLeft, &res.imgWLeft, &res.imgHLeft)) {
            if (job.needRight) {
                decode_and_swizzle(job.pathRight, &res.dataRight, &res.texWRight, &res.texHRight, &res.imgWRight, &res.imgHRight);
            }
            s_result = res;
            s_resultValid = true;
            s_resultId = id;
        }
    }
    return id;
}

static bool loader_poll_result(int expectedId, LoaderResult *out)
{
    bool got = false;
    LightLock_Lock(&s_loaderLock);
    if (s_resultValid) {
        if (s_resultId == expectedId) {
            *out = s_result;
            got = true;
        } else {
            free_loader_result(&s_result);
        }
        s_resultValid = false;
    }
    LightLock_Unlock(&s_loaderLock);
    return got;
}

// --- inline grid thumbnails -------------------------------------------

#define THUMB_COLS 48
#define THUMB_ROWS 30

// --- top-screen live preview: real GPU textures, one per eye -------------

typedef struct {
    C3D_Tex tex;
    Tex3DS_SubTexture subtex;
    C2D_Image image;
    bool valid;
} EyeTexture;

static EyeTexture s_previewLeft, s_previewRight;
static int s_previewLoadedPairIndex = -1;
static int s_previewRequestedPairIndex = -1;
static int s_previewRequestId = 0;

// The bottom-screen capture Luma also saves (Rosalina's "_bot.bmp"),
// shown while holding R in the single-item detail view. Loaded once
// when entering that view (not continuously, unlike the top-screen
// live preview), reusing the same loader/texture pipeline.
static EyeTexture s_bottomCapture;
static int  s_bottomCaptureRequestId = 0;
static bool s_bottomCaptureRequested = false;

static void free_eye_texture(EyeTexture *et)
{
    if (et->valid) {
        C3D_TexDelete(&et->tex);
        et->valid = false;
    }
}

// Takes an already-swizzled buffer (from decode_and_swizzle, on either
// thread) and turns it into a real texture. The only genuinely
// GPU-API-specific work left here is C3D_TexInit + a bulk memcpy +
// C3D_TexFlush -- fast compared to redoing the per-pixel swizzle here.
static void apply_prepared_texture(u8 *swizzledData, u32 texW, u32 texH, u32 imgW, u32 imgH, EyeTexture *et)
{
    free_eye_texture(et);
    if (!swizzledData) return;

    if (!C3D_TexInit(&et->tex, texW, texH, GPU_RGBA8)) {
        free(swizzledData);
        return;
    }
    C3D_TexSetFilter(&et->tex, GPU_LINEAR, GPU_NEAREST);
    C3D_TexSetWrap(&et->tex, GPU_CLAMP_TO_BORDER, GPU_CLAMP_TO_BORDER);
    et->tex.border = 0xFFFFFFFF;

    memcpy(et->tex.data, swizzledData, (size_t)texW * texH * 4);
    C3D_TexFlush(&et->tex);

    et->subtex.width  = (u16)imgW;
    et->subtex.height = (u16)imgH;
    et->subtex.left   = 0.0f;
    et->subtex.top    = 1.0f;
    et->subtex.right  = (float)imgW / (float)texW;
    et->subtex.bottom = 1.0f - (float)imgH / (float)texH;

    et->image.tex    = &et->tex;
    et->image.subtex = &et->subtex;
    et->valid = true;

    free(swizzledData);
}

// --- static embedded icons (Comet logo, 3D/2D badges, button glyphs) -----
//
// Unlike screenshots, these are known at compile time (baked in via
// process_icons.py -> assets/icons_data.c, using the same verified
// swizzle as everywhere else) and are alpha-blended, not opaque -- so
// they get their own loader with a transparent border instead of the
// opaque-white one used for screenshot previews.
static EyeTexture s_icons[ICON_COUNT];

static void load_static_icons(void)
{
    for (int i = 0; i < ICON_COUNT; i++) {
        const EmbeddedIcon *src = &g_embeddedIcons[i];
        EyeTexture *et = &s_icons[i];

        if (!C3D_TexInit(&et->tex, src->texW, src->texH, GPU_RGBA8)) continue;
        C3D_TexSetFilter(&et->tex, GPU_LINEAR, GPU_NEAREST);
        C3D_TexSetWrap(&et->tex, GPU_CLAMP_TO_BORDER, GPU_CLAMP_TO_BORDER);
        et->tex.border = 0x00000000; // transparent, not white -- these are alpha-blended glyphs

        memcpy(et->tex.data, src->data, (size_t)src->texW * src->texH * 4);
        C3D_TexFlush(&et->tex);

        et->subtex.width  = src->imgW;
        et->subtex.height = src->imgH;
        et->subtex.left   = 0.0f;
        et->subtex.top    = 1.0f;
        et->subtex.right  = (float)src->imgW / (float)src->texW;
        et->subtex.bottom = 1.0f - (float)src->imgH / (float)src->texH;

        et->image.tex    = &et->tex;
        et->image.subtex = &et->subtex;
        et->valid = true;
    }
}

static void free_static_icons(void)
{
    for (int i = 0; i < ICON_COUNT; i++) free_eye_texture(&s_icons[i]);
}

// Draws an icon at its native pixel size, top-left at (x,y).
static void draw_icon(IconId id, float x, float y)
{
    EyeTexture *et = &s_icons[id];
    if (!et->valid) return;
    C2D_DrawImageAt(et->image, x, y, 0.0f, NULL, 1.0f, 1.0f);
}

// Half-opacity variant for disabled footer actions.
static void draw_icon_tinted(IconId id, float x, float y, bool dim)
{
    EyeTexture *et = &s_icons[id];
    if (!et->valid) return;
    if (!dim) { C2D_DrawImageAt(et->image, x, y, 0.0f, NULL, 1.0f, 1.0f); return; }
    C2D_ImageTint tint;
    C2D_AlphaImageTint(&tint, 0.5f); // citro2d helper made for exactly this
    C2D_DrawImageAt(et->image, x, y, 0.0f, &tint, 1.0f, 1.0f);
}

static float icon_width(IconId id)  { return s_icons[id].subtex.width; }
static float icon_height(IconId id) { return s_icons[id].subtex.height; }

static void free_preview_textures(void)
{
    free_eye_texture(&s_previewLeft);
    free_eye_texture(&s_previewRight);
}

// Grid thumbnails, as real (tiny) GPU textures instead of a grid of
// solid-color rectangles. The underlying sampling is unchanged --
// still the fast bmp_load_thumbnail() that reads only a handful of
// rows off the SD card -- but letting the GPU's bilinear filtering
// smooth between those samples looks dramatically better than drawing
// each sample as a hard-edged rectangle, for the same source data.
// This is also considerably *cheaper* to draw: one textured quad per
// cell instead of up to ~1,400 individual rectangle draws.
typedef struct {
    bool attempted;
    EyeTexture tex;
} Thumbnail;

static Thumbnail s_thumbs[MAX_PAIRS];

static void build_thumbnail(const char *bmpPath, Thumbnail *out)
{
    out->attempted = true;

    u8 rgb[THUMB_COLS * THUMB_ROWS * 3];
    char err[64];
    if (!bmp_load_thumbnail(bmpPath, THUMB_COLS, THUMB_ROWS, rgb, err, sizeof(err))) {
        free_eye_texture(&out->tex);
        return;
    }

    u32 texW = next_pow2((u32)THUMB_COLS); if (texW < 8) texW = 8;
    u32 texH = next_pow2((u32)THUMB_ROWS); if (texH < 8) texH = 8;

    u8 *data = (u8 *)malloc((size_t)texW * texH * 4);
    if (!data) { free_eye_texture(&out->tex); return; }
    memset(data, 0, (size_t)texW * texH * 4);

    for (int y = 0; y < THUMB_ROWS; y++) {
        for (int x = 0; x < THUMB_COLS; x++) {
            const u8 *src = &rgb[(y * THUMB_COLS + x) * 3];
            u32 off = tiled_offset_rgba8((u32)x, (u32)y, texW);
            data[off + 0] = 0xFF;    // A
            data[off + 1] = src[2];  // B
            data[off + 2] = src[1];  // G
            data[off + 3] = src[0];  // R
        }
    }

    apply_prepared_texture(data, texW, texH, THUMB_COLS, THUMB_ROWS, &out->tex);
}

static void draw_thumbnail(const Thumbnail *t, float x, float y, float w, float h)
{
    if (!t->tex.valid) {
        C2D_DrawRectSolid(x, y, 0.0f, w, h, COLOR_PANEL);
        return;
    }
    float scaleX = w / t->tex.subtex.width;
    float scaleY = h / t->tex.subtex.height;
    C2D_DrawImageAt(t->tex.image, x, y, 0.0f, NULL, scaleX, scaleY);
}

static void free_all_thumbnails(void)
{
    for (int i = 0; i < MAX_PAIRS; i++) free_eye_texture(&s_thumbs[i].tex);
}

// Called every frame while browsing: kicks off a background load when
// the selection changes (both eyes, if the pair is 3D-capable), and
// applies the result once ready.
static void update_live_preview(void)
{
    ScreenshotPair *p = current_pair();
    if (!p) return;

    int idx = s_visibleIndices[s_selected];
    if (idx != s_previewLoadedPairIndex && idx != s_previewRequestedPairIndex) {
        s_previewRequestId = loader_request(p->topPath, p->has3D ? p->topRightPath : NULL);
        s_previewRequestedPairIndex = idx;
    }

    LoaderResult result;
    if (loader_poll_result(s_previewRequestId, &result)) {
        apply_prepared_texture(result.dataLeft, result.texWLeft, result.texHLeft,
                                result.imgWLeft, result.imgHLeft, &s_previewLeft);
        if (result.dataRight) {
            apply_prepared_texture(result.dataRight, result.texWRight, result.texHRight,
                                    result.imgWRight, result.imgHRight, &s_previewRight);
        } else {
            free_eye_texture(&s_previewRight);
        }
        s_previewLoadedPairIndex = s_previewRequestedPairIndex;
    }
}

// Kicks off a background load of the bottom-screen capture for the
// given pair (if it has one) -- called once when entering the detail
// view, not continuously.
static void request_bottom_capture(const ScreenshotPair *p)
{
    free_eye_texture(&s_bottomCapture);
    s_bottomCaptureRequested = false;
    if (p && p->botPath[0]) {
        s_bottomCaptureRequestId = loader_request(p->botPath, NULL);
        s_bottomCaptureRequested = true;
    }
}

// Call every frame while the detail view is open; applies the result
// once the background load finishes.
static void poll_bottom_capture(void)
{
    if (!s_bottomCaptureRequested) return;
    LoaderResult result;
    if (loader_poll_result(s_bottomCaptureRequestId, &result)) {
        apply_prepared_texture(result.dataLeft, result.texWLeft, result.texHLeft,
                                result.imgWLeft, result.imgHLeft, &s_bottomCapture);
        if (result.dataRight) free(result.dataRight); // shouldn't happen (we pass pathRight=NULL), but safe
        s_bottomCaptureRequested = false;
    }
}

// --- procedural spinner ---------------------------------------------------

static void draw_spinner_sized(float cx, float cy, float radius, float dotSize)
{
    const int DOTS = 8;
    for (int i = 0; i < DOTS; i++) {
        float angle = s_spinnerAngle + i * (2.0f * PI_F / DOTS);
        float dx = cx + radius * cosf(angle);
        float dy = cy + radius * sinf(angle);
        u8 alpha = (u8)(255 - i * (255 / DOTS));
        u32 color = C2D_Color32(0xFD, 0xCC, 0x28, alpha); // Comet yellow
        C2D_DrawRectSolid(dx - dotSize / 2, dy - dotSize / 2, 0.0f, dotSize, dotSize, color);
    }
}

static void draw_spinner(float cx, float cy, float radius)
{
    draw_spinner_sized(cx, cy, radius, 5.0f);
}

// --- text / popup helpers -------------------------------------------------

static void draw_text(float x, float y, float scale, u32 color, const char *fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    C2D_Text text;
    C2D_TextFontParse(&text, s_fontRegular, s_dynBuf, buf);
    C2D_TextOptimize(&text);
    C2D_DrawText(&text, C2D_WithColor, x, y, 0.0f, scale, scale, color);
}

static void draw_text_right_aligned(float rightEdge, float y, float scale, u32 color, const char *str)
{
    C2D_Text t;
    C2D_TextFontParse(&t, s_fontRegular, s_dynBuf, str);
    C2D_TextOptimize(&t);
    float tw, th;
    C2D_TextGetDimensions(&t, scale, scale, &tw, &th);
    C2D_DrawText(&t, C2D_WithColor, rightEdge - tw, y, 0.0f, scale, scale, color);
}

static void draw_centered_text(float x, float y, float w, float h, float scale, u32 color, const char *str)
{
    C2D_Text t;
    C2D_TextFontParse(&t, s_fontRegular, s_dynBuf, str);
    C2D_TextOptimize(&t);
    float tw, th;
    C2D_TextGetDimensions(&t, scale, scale, &tw, &th);
    C2D_DrawText(&t, C2D_WithColor, x + (w - tw) / 2, y + (h - th) / 2, 0.0f, scale, scale, color);
}

// citro2d has no native rounded-rect primitive. This is the standard
// technique: a horizontal band (full width, inset top/bottom by
// radius) + a vertical band (full height, inset left/right by radius)
// covers the whole rect except the four corners, which are filled by
// circles of that same radius. No gaps, no overlap artifacts.
static void draw_rounded_rect(float x, float y, float w, float h, float radius, u32 color)
{
    if (radius <= 0.0f || radius * 2 > w || radius * 2 > h) {
        C2D_DrawRectSolid(x, y, 0, w, h, color);
        return;
    }
    C2D_DrawRectSolid(x + radius, y, 0, w - 2 * radius, h, color);
    C2D_DrawRectSolid(x, y + radius, 0, w, h - 2 * radius, color);
    C2D_DrawCircleSolid(x + radius,         y + radius,         0, radius, color);
    C2D_DrawCircleSolid(x + w - radius,     y + radius,         0, radius, color);
    C2D_DrawCircleSolid(x + radius,         y + h - radius,     0, radius, color);
    C2D_DrawCircleSolid(x + w - radius,     y + h - radius,     0, radius, color);
}

// Radio/check marker: an outlined circle, or a filled yellow disc with
// a check when active. Used by the filter rows and the grid's batch
// checkboxes so both read identically.
static void draw_check_circle(float cx, float cy, float radius, bool active)
{
    cy -= 1.0f; // nudged up 1px to sit optically centred
    if (active) {
        C2D_DrawCircleSolid(cx, cy, 0, radius, COLOR_YELLOW);
        // Checkmark: two strokes, short down-right then long up-right.
        C2D_DrawLine(cx - radius * 0.42f, cy + radius * 0.04f, COLOR_BG,
                     cx - radius * 0.10f, cy + radius * 0.42f, COLOR_BG, 1.8f, 0);
        C2D_DrawLine(cx - radius * 0.10f, cy + radius * 0.42f, COLOR_BG,
                     cx + radius * 0.46f, cy - radius * 0.38f, COLOR_BG, 1.8f, 0);
    } else {
        C2D_DrawCircleSolid(cx, cy, 0, radius, COLOR_DIM);
        C2D_DrawCircleSolid(cx, cy, 0, radius - 1.4f, COLOR_BG);
    }
}

// Hairline between menu rows.
static void draw_separator(float y)
{
    C2D_DrawRectSolid(8, y, 0, 304, 1, COLOR_SEP_ROW);
}

// Brighter full-width rule fencing the content area off from the
// header and footer bars.
static void draw_frame_rule(float y)
{
    C2D_DrawRectSolid(0, y, 0, 320, 1, COLOR_SEP_FRAME);
}

// Left-aligned text, vertically centred in a row of height h. C2D's
// text origin is the top of the line box, so centring has to account
// for the measured line height rather than guessing an offset.
static void draw_text_vcenter(float x, float y, float h, float scale, u32 color, const char *str)
{
    C2D_Text t;
    C2D_TextFontParse(&t, s_fontRegular, s_dynBuf, str);
    C2D_TextOptimize(&t);
    float tw, th;
    C2D_TextGetDimensions(&t, scale, scale, &tw, &th);
    C2D_DrawText(&t, C2D_WithColor, x, y + (h - th) / 2, 0.0f, scale, scale, color);
}

static void draw_text_right_vcenter(float rightEdge, float y, float h, float scale, u32 color, const char *str)
{
    C2D_Text t;
    C2D_TextFontParse(&t, s_fontRegular, s_dynBuf, str);
    C2D_TextOptimize(&t);
    float tw, th;
    C2D_TextGetDimensions(&t, scale, scale, &tw, &th);
    C2D_DrawText(&t, C2D_WithColor, rightEdge - tw, y + (h - th) / 2, 0.0f, scale, scale, color);
}

static float measure_text(float scale, const char *str)
{
    C2D_Text t;
    C2D_TextFontParse(&t, s_fontRegular, s_dynBuf, str);
    C2D_TextOptimize(&t);
    float w, h;
    C2D_TextGetDimensions(&t, scale, scale, &w, &h);
    return w;
}

// Footer hints lay themselves out left-to-right from a fixed left
// margin, each one placed after measuring the previous label. The old
// version used hardcoded x positions tuned for the (too small) font
// scale, so at the correct size the labels collided.
typedef struct { IconId icon; const char *label; bool disabled; } FooterHint;

static const float FOOTER_Y = 213.0f, FOOTER_H = 27.0f;

// Shared by the three By Date screens, which have identical footers.
// (Declared here rather than inside the switch: unbraced `case` labels
// share a single scope, so per-case locals with the same name collide.)
static const FooterHint g_dateHints[] = {
    { ICON_BTN_A, "OK" }, { ICON_BTN_B, "Back" }, { ICON_BTN_Y, "Apply Filter" },
};

// Footer hints double as touch targets. The rects are captured while
// drawing and consulted on the next frame's input pass -- a one-frame
// lag that isn't perceptible, and it keeps a single source of truth for
// where each hint actually sits.
typedef struct { float x, y, w, h; u32 key; bool disabled; } FooterHitbox;
static FooterHitbox s_footerHits[8];
static int s_footerHitCount = 0;

static u32 icon_to_key(IconId id)
{
    switch (id) {
    case ICON_BTN_A:      return KEY_A;
    case ICON_BTN_B:      return KEY_B;
    case ICON_BTN_X:      return KEY_X;
    case ICON_BTN_Y:      return KEY_Y;
    case ICON_BTN_L:      return KEY_L;
    case ICON_BTN_R:      return KEY_R;
    case ICON_BTN_SELECT: return KEY_SELECT;
    default:              return 0;
    }
}

static void draw_footer_hints(const FooterHint *hints, int count)
{
    const float gapIconLabel = 5.0f, gapItems = 14.0f;
    float x = 8.0f;
    s_footerHitCount = 0;
    for (int i = 0; i < count; i++) {
        float iw = icon_width(hints[i].icon), ih = icon_height(hints[i].icon);
        // Unavailable actions render at half opacity rather than
        // disappearing, so the footer layout stays stable.
        float startX = x;
        u32 tint = hints[i].disabled ? C2D_Color32(0xFD, 0xFD, 0xFD, 0x80) : COLOR_TEXT;
        draw_icon_tinted(hints[i].icon, x, FOOTER_Y + (FOOTER_H - ih) / 2, hints[i].disabled);
        x += iw + gapIconLabel;
        draw_text_vcenter(x, FOOTER_Y, FOOTER_H, TEXT_12, tint, hints[i].label);
        float labelW = measure_text(TEXT_12, hints[i].label);

        if (s_footerHitCount < 8) {
            FooterHitbox *hb = &s_footerHits[s_footerHitCount++];
            hb->x = startX - 4;                       // a little padding for fingertips
            hb->y = FOOTER_Y;
            hb->w = (x + labelW) - startX + 8;
            hb->h = FOOTER_H;
            hb->key = icon_to_key(hints[i].icon);
            hb->disabled = hints[i].disabled;
        }

        x += labelW + gapItems;
    }
}

static bool point_in_rect(int px, int py, float x, float y, float w, float h)
{
    return px >= x && px < x + w && py >= y && py < y + h;
}

// Popup geometry, measured from the mockups: the box is 218x101 and
// the button row is a strip flush with its bottom edge, split into
// equal cells. The buttons share the popup's fill colour -- what marks
// the selected one is purely the yellow ring, which is why filling them
// with the page colour looked wrong.
#define POPUP_W      218.0f
#define POPUP_H      101.0f
#define POPUP_BTN_H  24.0f

static float popup_x(void) { return (320.0f - POPUP_W) / 2.0f; }
static float popup_y(void) { return 66.0f + popup_y_offset(); }

static void popup_button_rect(int index, int count, float *x, float *y, float *w, float *h)
{
    *h = POPUP_BTN_H;
    *y = popup_y() + POPUP_H - POPUP_BTN_H;
    *w = POPUP_W / (float)count;
    *x = popup_x() + index * (*w);
}

// Hairlines separating the message from the button strip, and the
// buttons from each other. Drawn before the buttons so a selected
// button's ring sits on top of the divider.
static void draw_popup_button_rules(int count)
{
    float top = popup_y() + POPUP_H - POPUP_BTN_H;
    C2D_DrawRectSolid(popup_x(), top, 0, POPUP_W, 1, COLOR_SEP_POPUP);
    for (int i = 1; i < count; i++) {
        float x = popup_x() + (POPUP_W / (float)count) * i;
        C2D_DrawRectSolid(x, top, 0, 1, POPUP_BTN_H, COLOR_SEP_POPUP);
    }
}

static void draw_popup_button(int index, int count, const char *label, bool selected)
{
    float x, y, w, h;
    popup_button_rect(index, count, &x, &y, &w, &h);
    if (selected) {
        draw_rounded_rect(x, y, w, h, 2, COLOR_YELLOW);
        draw_rounded_rect(x + 2, y + 2, w - 4, h - 4, 2, COLOR_POPUP_BG);
    }
    draw_centered_text(x, y, w, h, TEXT_12, COLOR_TEXT, label);
}

// Message lines are centred in the area above the button strip.
static void draw_popup_lines(const char *l1, const char *l2, const char *l3, bool reserveButtons)
{
    float top = popup_y();
    float areaH = POPUP_H - (reserveButtons ? POPUP_BTN_H : 0.0f);
    int n = (l1 ? 1 : 0) + (l2 ? 1 : 0) + (l3 ? 1 : 0);
    if (n == 0) return;
    const float lineH = 18.0f;
    float blockTop = top + (areaH - n * lineH) / 2.0f;
    const char *lines[3] = { l1, l2, l3 };
    int row = 0;
    for (int i = 0; i < 3; i++) {
        if (!lines[i]) continue;
        draw_centered_text(popup_x(), blockTop + row * lineH, POPUP_W, lineH,
                            TEXT_12, COLOR_POPUP_TEXT, lines[i]);
        row++;
    }
}

static void draw_popup(const char *line1, const char *line2, bool showSpinner, bool showOkButton)
{
    C2D_DrawRectSolid(0, 0, 0, 320, 240, COLOR_OVERLAY);
    draw_rounded_rect(popup_x(), popup_y(), POPUP_W, POPUP_H, 2, COLOR_POPUP_BG);

    if (showSpinner) {
        // No buttons in this variant, so the message sits high and the
        // spinner takes the space beneath it.
        draw_popup_lines(line1, line2, NULL, true);
        draw_spinner(popup_x() + POPUP_W / 2, popup_y() + POPUP_H - 30, 15);
    } else {
        draw_popup_lines(line1, line2, NULL, showOkButton);
    }

    if (showOkButton) {
        draw_popup_button_rules(1);
        draw_popup_button(0, 1, "OK", true);
    }
}

static void draw_confirm_popup(const char *line1, const char *line2,
                                const char *leftLabel, const char *rightLabel, int selected)
{
    C2D_DrawRectSolid(0, 0, 0, 320, 240, COLOR_OVERLAY);
    draw_rounded_rect(popup_x(), popup_y(), POPUP_W, POPUP_H, 2, COLOR_POPUP_BG);

    draw_popup_lines(line1, line2, NULL, true);

    draw_popup_button_rules(2);
    draw_popup_button(0, 2, leftLabel,  selected == 0);
    draw_popup_button(1, 2, rightLabel, selected == 1);
}

static void draw_top_screen(void);
static void draw_bottom_screen(void);

void ui_init(void)
{
    gfxInitDefault();
    gfxSet3D(false);
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(16384);
    C2D_Prepare();

    // romfs holds the two Poppins .bcfnt files (converted from the .ttf
    // via mkbcfnt -- see README). If this fails, or the font files
    // aren't present yet, C2D_FontLoad below just returns NULL, and
    // C2D_TextFontParse(..., NULL, ...) is citro2d's own documented
    // fallback to the system font -- so a missing/failed font never
    // crashes anything, it just silently renders with the system font.
    romfsInit();
    s_fontRegular  = C2D_FontLoad("romfs:/Poppins-Regular.bcfnt");
    s_fontSemiBold = C2D_FontLoad("romfs:/Poppins-SemiBold.bcfnt");

    s_top      = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    s_topRight = C2D_CreateScreenTarget(GFX_TOP, GFX_RIGHT);
    s_bot      = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    s_dynBuf = C2D_TextBufNew(4096);

    COLOR_BG        = C2D_Color32(0x2D, 0x2D, 0x2D, 0xff); // exact spec
    COLOR_PANEL     = C2D_Color32(0x28, 0x28, 0x2e, 0xff); // not specified -- left as-is, see note
    COLOR_ACCENT    = C2D_Color32(0xe0, 0x5a, 0x2b, 0xff); // not specified -- left as-is, see note
    COLOR_TEXT      = C2D_Color32(0xFD, 0xFD, 0xFD, 0xff); // exact spec
    COLOR_DIM       = C2D_Color32(0xaa, 0xaa, 0xaa, 0xff); // not specified -- left as-is, see note
    COLOR_OK        = C2D_Color32(0x4c, 0xaf, 0x50, 0xff); // not specified -- left as-is, see note
    COLOR_ERR       = C2D_Color32(0xe5, 0x39, 0x35, 0xff); // not specified -- left as-is, see note
    COLOR_YELLOW    = C2D_Color32(0xFD, 0xCC, 0x28, 0xff); // exact spec
    COLOR_POPUP_BG  = C2D_Color32(0x43, 0x43, 0x43, 0xff); // exact spec (now dark, was light)
    COLOR_POPUP_TEXT= C2D_Color32(0xFD, 0xFD, 0xFD, 0xff); // now same as COLOR_TEXT -- popup bg is dark now, so the old separate dark-on-light popup text color no longer applies
    COLOR_OVERLAY   = C2D_Color32(0x00, 0x00, 0x00, 0x90);
    COLOR_SEP_FRAME = C2D_Color32(0xFD, 0xFD, 0xFD, 0xff); // under the header / above the footer
    COLOR_SEP_ROW   = C2D_Color32(0x79, 0x79, 0x79, 0xff); // between menu rows
    COLOR_SEP_POPUP = C2D_Color32(0x8E, 0x8E, 0x8E, 0xff); // inside popups: above and between buttons

    load_static_icons();
    audio_init();

    loader_init();

    int n = fs_scan_screenshot_pairs(s_pairs, MAX_PAIRS);
    s_pairCount = (n < 0) ? 0 : n;
    rebuild_visible_list();
}

void ui_exit(void)
{
    audio_exit();
    loader_exit();
    free_preview_textures();
    free_all_thumbnails();
    free_eye_texture(&s_bottomCapture);
    free_static_icons();
    C2D_FontFree(s_fontRegular);
    C2D_FontFree(s_fontSemiBold);
    romfsExit();
    C2D_TextBufDelete(s_dynBuf);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
}

// ---- state transitions -------------------------------------------------

static void begin_filter_menu(void)
{
    s_filterCursor = (s_filterMode == FILTER_BY_DATE) ? 3
                    : (s_filterMode == FILTER_2D_ONLY) ? 2
                    : (s_filterMode == FILTER_3D_ONLY) ? 1 : 0;
    s_state = APP_FILTER_MENU;
}

static void begin_filter_by_date(void)
{
    scan_available_years();
    // Only Year gets a smart default (most recent available) -- Month
    // and Day default to "All" and stay that way unless the user
    // deliberately narrows down, now that "All" is a real, useful
    // option rather than just an "unset" placeholder.
    if (s_filterYear == 0 && s_availableYearCount > 0) {
        s_filterYear = s_availableYears[s_availableYearCount - 1];
    }
    s_filterCursor = 0;
    s_state = APP_FILTER_BY_DATE;
}

// Month picker list is 13 entries: index 0 = "All", indices 1-12 =
// January..December -- index equals s_filterMonth directly, no
// translation needed.
static void begin_month_picker(void)
{
    scan_available_months(s_filterYear);
    s_filterCursor = 0; // "All"
    for (int i = 0; i < s_availableMonthCount; i++) {
        if (s_availableMonths[i] == s_filterMonth) { s_filterCursor = i + 1; break; }
    }
    s_state = APP_FILTER_MONTH_PICKER;
}

static void begin_year_picker(void)
{
    s_filterCursor = 0;
    for (int i = 0; i < s_availableYearCount; i++) {
        if (s_availableYears[i] == s_filterYear) { s_filterCursor = i; break; }
    }
    s_state = APP_FILTER_YEAR_PICKER;
}

// Day picker is only reachable once Month is specific (not "All").
static void begin_day_picker(void)
{
    scan_available_days(s_filterYear, s_filterMonth);
    s_filterCursor = 0; // index 0 = "All"
    for (int i = 0; i < s_availableDayCount; i++) {
        if (s_availableDays[i] == s_filterDay) { s_filterCursor = i + 1; break; }
    }
    s_state = APP_FILTER_DAY_PICKER;
}

// Applies the currently-configured By Date year/month/day as the
// active filter. Year is the only hard requirement -- Month and Day
// are both allowed to be "All".
static void apply_date_filter_if_ready(void)
{
    if (s_filterYear <= 0) return;
    s_filterMode = FILTER_BY_DATE;
    rebuild_visible_list();
    s_flashFrames = 3;
    s_state = APP_BROWSE;
}

static void enter_detail_view(void)
{
    ScreenshotPair *p = current_pair();
    if (!p) return;
    s_detailMenuSelection = 0; // default to Copy to 3DS Album
    request_bottom_capture(p);
    s_state = APP_DETAIL;
}

// Deletes the currently selected pair's files, then does a full rescan
// rather than trying to shift s_pairs/s_thumbs in place. This is
// deliberate: EyeTexture's `image` field points back into its own
// `tex`/`subtex` fields, so a raw struct-copy shift (as part of
// removing one array element) would leave those pointers dangling.
// Deletion is a rare, deliberate action, so the cost of a fresh scan
// plus lazily-rebuilt thumbnails is a non-issue.
static void do_delete_current_pair(void)
{
    ScreenshotPair *p = current_pair();
    if (p) fs_delete_pair(p);

    free_eye_texture(&s_bottomCapture);
    s_bottomCaptureRequested = false;

    free_all_thumbnails();
    memset(s_thumbs, 0, sizeof(s_thumbs));

    int n = fs_scan_screenshot_pairs(s_pairs, MAX_PAIRS);
    s_pairCount = (n < 0) ? 0 : n;
    rebuild_visible_list();

    free_preview_textures();
    s_previewLoadedPairIndex = -1;
    s_previewRequestedPairIndex = -1;

    audio_play(SFX_DELETE);
    s_thumbSfxMute = 45; // let the delete sound breathe
    op_complete(APP_BROWSE);
}

static void begin_convert(void)
{
    ScreenshotPair *p = current_pair();
    if (!p) return;
    op_enter(APP_CONVERTING);
}

static void do_convert(void)
{
    ScreenshotPair *p = current_pair();
    if (!p) { s_state = APP_BROWSE; return; }

    char err[128] = {0};
    RGBImage left, right;
    bool haveRight = false;

    if (!bmp_load(p->topPath, &left, err, sizeof(err))) {
        s_lastSuccess = false;
        snprintf(s_lastMessage, sizeof(s_lastMessage), "Couldn't load screenshot:");
        snprintf(s_lastOutputPath, sizeof(s_lastOutputPath), "%s", err);
        op_complete(APP_RESULT);
        return;
    }
    if (p->has3D && bmp_load(p->topRightPath, &right, err, sizeof(err))) haveRight = true;

    char dir[280], base[32], outPath[320];
    bool ok = false;
    if (fs_next_dcim_slot(dir, sizeof(dir), base, sizeof(base))) {
        // 3D pairs become .MPO; 2D screenshots are written as a plain
        // .JPG, which the Camera app reads natively -- there's no
        // reason to refuse them just because there's no second eye.
        if (haveRight) {
            snprintf(outPath, sizeof(outPath), "%s/%s.MPO", dir, base);
            ok = mpo_write(&left, &right, p->timestamp, outPath, err, sizeof(err));
        } else {
            snprintf(outPath, sizeof(outPath), "%s/%s.JPG", dir, base);
            ok = jpg_write(&left, p->timestamp, outPath, err, sizeof(err));
        }
    } else {
        snprintf(err, sizeof(err), "Couldn't allocate a DCIM slot.");
    }

    bmp_free(&left);
    if (haveRight) bmp_free(&right);

    s_lastSuccess = ok;
    if (ok) {
        snprintf(s_lastMessage, sizeof(s_lastMessage), "Screenshot copied to");
        snprintf(s_lastOutputPath, sizeof(s_lastOutputPath), "3DS Album Camera");
        audio_play(SFX_COPY);
        s_thumbSfxMute = 45;
    } else {
        snprintf(s_lastMessage, sizeof(s_lastMessage), "Copy failed:");
        snprintf(s_lastOutputPath, sizeof(s_lastOutputPath), "%s", err);
    }
    op_complete(APP_RESULT);
}

// ---- batch select / convert / delete --------------------------------------

static void begin_batch_convert(void)
{
    s_batchTotal = 0;
    for (int i = 0; i < s_pairCount; i++) {
        if (s_batchSelected[i]) s_batchTotal++;
    }
    // Nothing selected is now prevented up front -- the Copy hint is
    // greyed out and L is ignored -- so there's no error popup here.
    if (s_batchTotal == 0) return;

    s_batchSucceeded = 0;
    s_batchIndex = 0;
    op_enter(APP_BATCH_CONVERTING);
}

static void do_batch_convert_step(void)
{
    while (s_batchIndex < s_pairCount && !s_batchSelected[s_batchIndex]) {
        s_batchIndex++;
    }

    if (s_batchIndex >= s_pairCount) {
        s_lastSuccess = true;
        snprintf(s_lastMessage, sizeof(s_lastMessage), "%d screenshot%s copied to 3DS Album.",
                 s_batchSucceeded, s_batchSucceeded == 1 ? "" : "s");
        snprintf(s_lastOutputPath, sizeof(s_lastOutputPath), "Delete them?");
        audio_play(SFX_COPY);
        s_thumbSfxMute = 45;
        s_confirmSelection = 0; // default to "Back" -- keep the originals unless asked
        op_complete(APP_BATCH_RESULT);
        return;
    }

    ScreenshotPair *p = &s_pairs[s_batchIndex];
    RGBImage left, right;
    char err[128];
    bool haveLeft = false, haveRight = false, ok = false;

    if (bmp_load(p->topPath, &left, err, sizeof(err))) {
        haveLeft = true;
        if (p->has3D && bmp_load(p->topRightPath, &right, err, sizeof(err))) haveRight = true;

        char dir[280], base[32], outPath[320];
        if (fs_next_dcim_slot(dir, sizeof(dir), base, sizeof(base))) {
            if (haveRight) {
                snprintf(outPath, sizeof(outPath), "%s/%s.MPO", dir, base);
                ok = mpo_write(&left, &right, p->timestamp, outPath, err, sizeof(err));
            } else {
                snprintf(outPath, sizeof(outPath), "%s/%s.JPG", dir, base);
                ok = jpg_write(&left, p->timestamp, outPath, err, sizeof(err));
            }
        }
    }
    if (haveRight) bmp_free(&right);
    if (haveLeft)  bmp_free(&left);

    if (ok) s_batchSucceeded++;
    s_batchIndex++;
}

// Standalone Batch Delete (X), independent of Copy to Album.
static void begin_batch_delete(void)
{
    int count = 0;
    for (int i = 0; i < s_pairCount; i++) if (s_batchSelected[i]) count++;
    if (count == 0) return;

    snprintf(s_lastMessage, sizeof(s_lastMessage), "%d screenshot%s will be deleted",
             count, count == 1 ? "" : "s");
    s_confirmSelection = 0; // Cancel is the safe default
    s_state = APP_BATCH_DELETE_CONFIRM;
}

// Deletes every currently-checked pair. Shared by both the standalone
// Batch Delete path and the "copied -- delete originals?" prompt after
// a batch copy, since the actual action is identical either way.
// Rescans from scratch afterward (same reasoning as single-item
// delete: EyeTexture's `image` field self-references its own
// `tex`/`subtex`, so shifting array elements in place would leave
// dangling pointers).
static void do_batch_delete_selected(void)
{
    for (int i = 0; i < s_pairCount; i++) {
        if (s_batchSelected[i]) fs_delete_pair(&s_pairs[i]);
    }

    free_all_thumbnails();
    memset(s_thumbs, 0, sizeof(s_thumbs));

    int n = fs_scan_screenshot_pairs(s_pairs, MAX_PAIRS);
    s_pairCount = (n < 0) ? 0 : n;

    s_batchMode = false;
    memset(s_batchSelected, 0, sizeof(s_batchSelected));
    rebuild_visible_list();

    free_preview_textures();
    s_previewLoadedPairIndex = -1;
    s_previewRequestedPairIndex = -1;

    audio_play(SFX_DELETE);
    s_thumbSfxMute = 45; // let the delete sound breathe
    op_complete(APP_BROWSE);
}

// ---- drawing -------------------------------------------------------------

// Grid: 4 cols x 4 rows of 66x40 cells fits the bottom screen almost
// exactly once the header (26px) and footer (32px) are accounted for --
// this is clearly the layout the mockup's "66x40" labels were designed
// around, so building to that spec rather than eyeballing a different one.
#define GRID_COLS 4
#define GRID_ROWS 4
static const float CELL_W = 73.0f;
static const float CELL_H = 42.0f;
static const float CELL_SPACING = 4.0f;
static const float GRID_LEFT = 8.0f;  // 4*73 + 3*4 = 304 -> spans 8..312
static const float GRID_TOP  = 31.0f; // 4*42 + 3*4 = 180 -> ends at 211, just above the footer rule
static const float CHECKBOX_SIZE = 14.0f;

// Which grid row is scrolled to the top, given the current cursor
// position -- shared by drawing and touch hit-testing so they can never
// disagree about where a cell actually is on screen.
static int compute_grid_start_row(void)
{
    int cursorRow = s_selected / GRID_COLS;
    int totalRows = (s_visibleCount + GRID_COLS - 1) / GRID_COLS;
    int startRow = cursorRow >= GRID_ROWS ? cursorRow - GRID_ROWS + 1 : 0;
    int maxStartRow = totalRows > GRID_ROWS ? totalRows - GRID_ROWS : 0;
    if (startRow > maxStartRow) startRow = maxStartRow;
    if (startRow < 0) startRow = 0;
    return startRow;
}

// Hit-tests a bottom-screen touch point against the currently visible
// grid cells. Returns true and fills *outVisPos if a real (non-empty)
// cell was hit.
static bool grid_hit_test(int touchX, int touchY, int *outVisPos)
{
    int startRow = compute_grid_start_row();
    for (int r = 0; r < GRID_ROWS; r++) {
        for (int c = 0; c < GRID_COLS; c++) {
            float x = GRID_LEFT + c * (CELL_W + CELL_SPACING);
            float y = GRID_TOP + r * (CELL_H + CELL_SPACING);
            if (point_in_rect(touchX, touchY, x, y, CELL_W, CELL_H)) {
                int visPos = (startRow + r) * GRID_COLS + c;
                if (visPos < s_visibleCount) { *outVisPos = visPos; return true; }
                return false;
            }
        }
    }
    return false;
}

static void draw_eye_image(const EyeTexture *et)
{
    if (!et->valid) return;
    float w = 400, h = 240;
    float imgAspect = (float)et->subtex.width / (float)et->subtex.height;
    float boxAspect = w / h;
    float drawW = w, drawH = h;
    if (imgAspect > boxAspect) { drawH = w / imgAspect; }
    else { drawW = h * imgAspect; }
    float drawX = (w - drawW) / 2, drawY = (h - drawH) / 2;

    float scaleX = drawW / et->subtex.width;
    float scaleY = drawH / et->subtex.height;
    C2D_DrawImageAt(et->image, drawX, drawY, 0.0f, NULL, scaleX, scaleY);
}

static void draw_top_screen(void)
{
    switch (s_state) {
    case APP_BATCH_CONVERTING:
    case APP_BATCH_RESULT:
    case APP_BATCH_DELETE_CONFIRM:
    case APP_BATCH_DELETING:
    case APP_FILTER_MENU:
    case APP_FILTER_BY_DATE:
    case APP_FILTER_MONTH_PICKER:
    case APP_FILTER_YEAR_PICKER:
    case APP_FILTER_DAY_PICKER:
        gfxSet3D(false);
        C2D_TargetClear(s_top, COLOR_BG);
        C2D_SceneBegin(s_top);
        return;
    default:
        break;
    }

    if (s_state == APP_BROWSE) {
        update_live_preview();
    }

    bool stereo = s_previewRight.valid;
    gfxSet3D(stereo);

    C2D_TargetClear(s_top, COLOR_BG);
    C2D_SceneBegin(s_top);
    draw_eye_image(&s_previewLeft);

    if (s_state == APP_BROWSE &&
        s_previewRequestedPairIndex != s_previewLoadedPairIndex) {
        draw_spinner_sized(382, 20, 6, 3.5f);
    }

    if (stereo) {
        C2D_TargetClear(s_topRight, COLOR_BG);
        C2D_SceneBegin(s_topRight);
        draw_eye_image(&s_previewRight);
    }
}

#define HEADER_H 27.0f

static void draw_header(const char *subtitle, const char *rightLabel)
{
    C2D_DrawRectSolid(0, 0, 0, 320, HEADER_H, COLOR_BG);

    float iconX = 8, iconY = (HEADER_H - icon_height(ICON_COMET)) / 2;
    draw_icon(ICON_COMET, iconX, iconY);

    C2D_Text title;
    C2D_TextFontParse(&title, s_fontSemiBold, s_dynBuf, "Comet");
    C2D_TextOptimize(&title);
    float titleW, titleH;
    C2D_TextGetDimensions(&title, TEXT_12, TEXT_12, &titleW, &titleH);
    C2D_DrawText(&title, C2D_WithColor, iconX + icon_width(ICON_COMET) + 5,
                 (HEADER_H - titleH) / 2, 0.0f, TEXT_12, TEXT_12, COLOR_TEXT);

    if (rightLabel) draw_text_right_vcenter(312, 0, HEADER_H, TEXT_9, COLOR_TEXT, rightLabel);
    draw_frame_rule(HEADER_H);
    if (subtitle) draw_text(10, HEADER_H + 4, TEXT_9, COLOR_DIM, "%s", subtitle);
}

// Formats the currently-configured Year/Month/Day (regardless of
// whether By Date is the *active* filter right now) with progressively
// more detail: just the year, or "Month Year", or "Month Day, Year".
static void format_date_ymd(char *buf, size_t bufSize)
{
    if (s_filterYear <= 0) {
        snprintf(buf, bufSize, "-");
    } else if (s_filterMonth == 0) {
        snprintf(buf, bufSize, "%d", s_filterYear);
    } else if (s_filterDay == 0) {
        snprintf(buf, bufSize, "%s %d", MONTH_NAMES[s_filterMonth - 1], s_filterYear);
    } else {
        snprintf(buf, bufSize, "%s %d, %d", MONTH_NAMES[s_filterMonth - 1], s_filterDay, s_filterYear);
    }
}

// How many screenshots the currently-configured Year/Month/Day would
// match if applied -- used for the Filter menu's "By Date" row, same
// spirit as the plain counts shown for All/3D Only/2D Only.
static int count_matching_by_date(void)
{
    if (s_filterYear <= 0) return 0;
    int count = 0;
    for (int i = 0; i < s_pairCount; i++) {
        int y, mo, d;
        if (!parse_timestamp_ymd(s_pairs[i].timestamp, &y, &mo, &d)) continue;
        if (y != s_filterYear) continue;
        if (s_filterMonth != 0 && mo != s_filterMonth) continue;
        if (s_filterMonth != 0 && s_filterDay != 0 && d != s_filterDay) continue;
        count++;
    }
    return count;
}

static const char *filter_mode_label(void)
{
    static char buf[48];
    switch (s_filterMode) {
    case FILTER_ALL:
        snprintf(buf, sizeof(buf), "All Screenshots (%d)", s_visibleCount);
        return buf;
    case FILTER_3D_ONLY:
        snprintf(buf, sizeof(buf), "3D Only (%d)", s_visibleCount);
        return buf;
    case FILTER_2D_ONLY:
        snprintf(buf, sizeof(buf), "2D Only (%d)", s_visibleCount);
        return buf;
    case FILTER_BY_DATE: {
        char d[24];
        format_date_ymd(d, sizeof(d));
        snprintf(buf, sizeof(buf), "%s (%d)", d, s_visibleCount);
        return buf;
    }
    }
    return NULL;
}

static void draw_grid(void)
{
    // No subtitle row here -- the grid is tight enough (4 rows of 40px
    // cells fits the available height almost exactly) that there's no
    // room to spare for a second header line.
    draw_header(NULL, filter_mode_label());

    if (s_visibleCount == 0) {
        draw_text(18, GRID_TOP + 6, TEXT_12, COLOR_DIM,
                  s_pairCount == 0 ? "None yet -- take one from Rosalina."
                                   : "No screenshots match this filter.");
        return;
    }

    int startRow = compute_grid_start_row();
    int totalRows = (s_visibleCount + GRID_COLS - 1) / GRID_COLS;

    bool builtOneThisFrame = false;
    for (int r = 0; r < GRID_ROWS; r++) {
        int gridRow = startRow + r;
        for (int c = 0; c < GRID_COLS; c++) {
            int visPos = gridRow * GRID_COLS + c;
            if (visPos >= s_visibleCount) continue;
            int idx = s_visibleIndices[visPos];

            float x = GRID_LEFT + c * (CELL_W + CELL_SPACING);
            float y = GRID_TOP + r * (CELL_H + CELL_SPACING);
            bool sel = (visPos == s_selected);

            if (!builtOneThisFrame && !s_thumbs[idx].attempted) {
                build_thumbnail(s_pairs[idx].topPath, &s_thumbs[idx]);
                builtOneThisFrame = true;
                if (s_thumbs[idx].tex.valid && s_thumbSfxMute == 0) audio_play(SFX_THUMB_LOAD);
            }

            if (sel) {
                draw_rounded_rect(x - 2, y - 2, CELL_W + 4, CELL_H + 4, 2, COLOR_YELLOW);
            }

            draw_thumbnail(&s_thumbs[idx], x, y, CELL_W, CELL_H);

            // 3D/2D badge, top-left corner of the cell, over the thumbnail.
            draw_icon(s_pairs[idx].has3D ? ICON_BADGE_3D : ICON_BADGE_2D, x + 2, y + 2);

            if (s_batchMode) {
                // Every screenshot is selectable now -- Delete applies to
                // 2D and 3D alike, and Copy to Album simply skips the
                // 2D ones. (Previously the checkbox was inert for 2D,
                // which made them look broken.)
                draw_check_circle(x + CELL_W - CHECKBOX_SIZE / 2 - 3,
                                  y + CHECKBOX_SIZE / 2 + 3,
                                  CHECKBOX_SIZE / 2, s_batchSelected[idx]);
            }
        }
    }

    if (totalRows > GRID_ROWS) {
        float trackH = GRID_ROWS * (CELL_H + CELL_SPACING) - CELL_SPACING;
        float maxStartRow = (float)(totalRows - GRID_ROWS);
        float thumbH = trackH * GRID_ROWS / totalRows;
        if (thumbH < 10) thumbH = 10;
        float thumbY = GRID_TOP + (trackH - thumbH) * startRow / maxStartRow;
        C2D_DrawRectSolid(313, GRID_TOP, 0, 3, trackH, COLOR_PANEL);
        C2D_DrawRectSolid(313, thumbY, 0, 3, thumbH, COLOR_DIM);
    }
}

static const float DETAIL_MENU_X = 8, DETAIL_MENU_W = 304;
static const float DETAIL_MENU_TOP = 34, DETAIL_MENU_H = 30, DETAIL_MENU_GAP = 0;

// Luma names files "YYYY-MM-DD_HH-MM-SS.mmm"; the UI shows a readable
// date instead of the raw filename stem.
static void format_display_date(const char *ts, char *out, size_t outSize)
{
    int y, mo, d, h, mi, s;
    if (sscanf(ts, "%4d-%2d-%2d_%2d-%2d-%2d", &y, &mo, &d, &h, &mi, &s) < 5) {
        snprintf(out, outSize, "%s", ts);
        return;
    }
    int h12 = h % 12; if (h12 == 0) h12 = 12;
    snprintf(out, outSize, "%02d/%02d/%04d %d:%02d %s", mo, d, y, h12, mi, h < 12 ? "a.m." : "p.m.");
}

static float detail_menu_item_y(int i) { return DETAIL_MENU_TOP + i * (DETAIL_MENU_H + DETAIL_MENU_GAP); }

static void draw_detail_menu(void)
{
    ScreenshotPair *p = current_pair();
    char dateBuf[32] = "";
    if (p) format_display_date(p->timestamp, dateBuf, sizeof(dateBuf));
    draw_header(NULL, dateBuf);

    const char *labels[2] = {"Copy to 3DS Album", "Delete"};
    for (int i = 0; i < 2; i++) {
        float y = detail_menu_item_y(i);
        bool sel = (i == s_detailMenuSelection);
        if (sel) {
            // Yellow ring only -- the row itself keeps the page
            // background, matching the mockup.
            draw_rounded_rect(DETAIL_MENU_X, y, DETAIL_MENU_W, DETAIL_MENU_H, 2, COLOR_YELLOW);
            draw_rounded_rect(DETAIL_MENU_X + 2, y + 2, DETAIL_MENU_W - 4, DETAIL_MENU_H - 4, 2, COLOR_BG);
        }
        draw_text_vcenter(DETAIL_MENU_X + 10, y, DETAIL_MENU_H, TEXT_12, COLOR_TEXT, labels[i]);
        draw_separator(y + DETAIL_MENU_H);
    }
}

// Hit-tests a touch point against the two detail-menu items.
static bool detail_menu_hit_test(int touchX, int touchY, int *outIndex)
{
    for (int i = 0; i < 2; i++) {
        if (point_in_rect(touchX, touchY, DETAIL_MENU_X, detail_menu_item_y(i), DETAIL_MENU_W, DETAIL_MENU_H)) {
            *outIndex = i;
            return true;
        }
    }
    return false;
}

static void draw_bottom_capture(void)
{
    ScreenshotPair *p = current_pair();
    if (s_bottomCapture.valid) {
        float w = 320, h = 240;
        float imgAspect = (float)s_bottomCapture.subtex.width / (float)s_bottomCapture.subtex.height;
        float boxAspect = w / h;
        float drawW = w, drawH = h;
        if (imgAspect > boxAspect) { drawH = w / imgAspect; }
        else { drawW = h * imgAspect; }
        float drawX = (w - drawW) / 2, drawY = (h - drawH) / 2;
        float scaleX = drawW / s_bottomCapture.subtex.width;
        float scaleY = drawH / s_bottomCapture.subtex.height;
        C2D_DrawImageAt(s_bottomCapture.image, drawX, drawY, 0.0f, NULL, scaleX, scaleY);
    } else if (p && !p->botPath[0]) {
        draw_centered_text(0, 108, 320, 24, TEXT_12, COLOR_DIM, "No bottom screen capture available");
    } else {
        draw_spinner(160, 120, 16);
    }
}

// --- filter menu screens ---------------------------------------------------

static const float FILTER_LIST_TOP = 31.0f;
static const float FILTER_ROW_HEIGHT = 32.0f;
static const int   FILTER_VISIBLE_ROWS = 5;

static int compute_list_start(int cursor, int total)
{
    int start = cursor >= FILTER_VISIBLE_ROWS ? cursor - FILTER_VISIBLE_ROWS + 1 : 0;
    int maxStart = total > FILTER_VISIBLE_ROWS ? total - FILTER_VISIBLE_ROWS : 0;
    if (start > maxStart) start = maxStart;
    if (start < 0) start = 0;
    return start;
}

// One row of a vertical selectable list: an optional radio-style marker
// (filled when `active`), a label, an optional right-aligned suffix
// (a count, or a current value), and a highlight bar under the cursor.
static void draw_filter_row(float y, const char *label, const char *suffix,
                             bool showMarker, bool active, bool cursor)
{
    if (cursor) {
        draw_rounded_rect(8, y, 304, FILTER_ROW_HEIGHT, 2, COLOR_YELLOW);
        draw_rounded_rect(10, y + 2, 300, FILTER_ROW_HEIGHT - 4, 2, COLOR_BG);
    }

    float markerCX = 292, markerR = 7;
    if (showMarker) draw_check_circle(markerCX, y + FILTER_ROW_HEIGHT / 2, markerR, active);

    draw_text_vcenter(18, y, FILTER_ROW_HEIGHT, TEXT_12, COLOR_TEXT, label);
    if (suffix) {
        float suffixRight = showMarker ? (markerCX - markerR - 8) : 304;
        draw_text_right_vcenter(suffixRight, y, FILTER_ROW_HEIGHT, TEXT_12, COLOR_DIM, suffix);
    }
    draw_separator(y + FILTER_ROW_HEIGHT);
}

static void draw_filter_scrollbar(int start, int total)
{
    float trackH = FILTER_ROW_HEIGHT * FILTER_VISIBLE_ROWS;
    float thumbH = trackH * FILTER_VISIBLE_ROWS / total;
    if (thumbH < 10) thumbH = 10;
    int maxStart = total - FILTER_VISIBLE_ROWS;
    float thumbY = FILTER_LIST_TOP + (trackH - thumbH) * start / (float)maxStart;
    C2D_DrawRectSolid(313, FILTER_LIST_TOP, 0, 3, trackH, COLOR_PANEL);
    C2D_DrawRectSolid(313, thumbY, 0, 3, thumbH, COLOR_DIM);
}

// Shared touch hit-test for the scrollable list screens (month/year
// pickers): given the current scroll window, finds which row (if any)
// a touch point landed on.
static bool list_row_hit_test(int touchY, int start, int total, int *outIndex)
{
    for (int i = 0; i < FILTER_VISIBLE_ROWS && start + i < total; i++) {
        float y = FILTER_LIST_TOP + i * FILTER_ROW_HEIGHT;
        if (touchY >= y && touchY < y + FILTER_ROW_HEIGHT) { *outIndex = start + i; return true; }
    }
    return false;
}

static float filter_menu_row_y(int i) { return FILTER_LIST_TOP + (i < 3 ? i * FILTER_ROW_HEIGHT : 3.6f * FILTER_ROW_HEIGHT); }

static void draw_filter_menu(void)
{
    draw_header(NULL, "Filters");

    int countAll = s_pairCount, count3D = 0, count2D = 0;
    for (int i = 0; i < s_pairCount; i++) { if (s_pairs[i].has3D) count3D++; else count2D++; }

    // Counts read as part of the label -- "All Screenshots (88)" --
    // rather than sitting in a separate right-hand column.
    char lbl[48];
    snprintf(lbl, sizeof(lbl), "All Screenshots (%d)", countAll);
    draw_filter_row(filter_menu_row_y(0), lbl, NULL, true, s_filterMode == FILTER_ALL, s_filterCursor == 0);
    snprintf(lbl, sizeof(lbl), "3D Only (%d)", count3D);
    draw_filter_row(filter_menu_row_y(1), lbl, NULL, true, s_filterMode == FILTER_3D_ONLY, s_filterCursor == 1);
    snprintf(lbl, sizeof(lbl), "2D Only (%d)", count2D);
    draw_filter_row(filter_menu_row_y(2), lbl, NULL, true, s_filterMode == FILTER_2D_ONLY, s_filterCursor == 2);

    if (s_filterYear > 0) {
        char dateStr[24], dateBuf[40];
        format_date_ymd(dateStr, sizeof(dateStr));
        snprintf(dateBuf, sizeof(dateBuf), "%s (%d)", dateStr, count_matching_by_date());
        draw_filter_row(filter_menu_row_y(3), "By Date", dateBuf, true, s_filterMode == FILTER_BY_DATE, s_filterCursor == 3);
    } else {
        draw_filter_row(filter_menu_row_y(3), "By Date", NULL, true, s_filterMode == FILTER_BY_DATE, s_filterCursor == 3);
    }
}

static int by_date_row_count(void) { return s_filterMonth != 0 ? 3 : 2; }

static void draw_by_date_menu(void)
{
    draw_header(NULL, "Filters > By Date");

    char yearBuf[8], monthBuf[16], dayBuf[8];
    if (s_filterYear > 0) snprintf(yearBuf, sizeof(yearBuf), "%d", s_filterYear);
    else snprintf(yearBuf, sizeof(yearBuf), "-");
    snprintf(monthBuf, sizeof(monthBuf), "%s", s_filterMonth == 0 ? "All" : MONTH_NAMES[s_filterMonth - 1]);

    draw_filter_row(FILTER_LIST_TOP + 0 * FILTER_ROW_HEIGHT, "Year", yearBuf, false, false, s_filterCursor == 0);
    draw_filter_row(FILTER_LIST_TOP + 1 * FILTER_ROW_HEIGHT, "Month", monthBuf, false, false, s_filterCursor == 1);

    // Day only makes sense (and is only shown) once a specific month
    // is chosen -- "any day within all of 2026" isn't a meaningful row.
    if (s_filterMonth != 0) {
        snprintf(dayBuf, sizeof(dayBuf), "%s", s_filterDay == 0 ? "All" : "");
        if (s_filterDay != 0) snprintf(dayBuf, sizeof(dayBuf), "%d", s_filterDay);
        draw_filter_row(FILTER_LIST_TOP + 2 * FILTER_ROW_HEIGHT, "Day", dayBuf, false, false, s_filterCursor == 2);
    }
}

static void draw_month_picker(void)
{
    draw_header(NULL, "Select Month");
    if (s_availableMonthCount == 0) {
        draw_text(18, FILTER_LIST_TOP + 6, TEXT_12, COLOR_DIM, "No screenshots in this year.");
        return;
    }
    int total = s_availableMonthCount + 1; // "All" + real months
    int start = compute_list_start(s_filterCursor, total);
    for (int i = 0; i < FILTER_VISIBLE_ROWS && start + i < total; i++) {
        int idx = start + i;
        const char *label;
        bool active;
        if (idx == 0) {
            label = "All";
            active = (s_filterMonth == 0);
        } else {
            label = MONTH_NAMES[s_availableMonths[idx - 1] - 1];
            active = (s_availableMonths[idx - 1] == s_filterMonth);
        }
        draw_filter_row(FILTER_LIST_TOP + i * FILTER_ROW_HEIGHT, label, NULL, true, active, idx == s_filterCursor);
    }
    if (total > FILTER_VISIBLE_ROWS) draw_filter_scrollbar(start, total);
}

static void draw_year_picker(void)
{
    draw_header(NULL, "Select Year");
    if (s_availableYearCount == 0) {
        draw_text(18, FILTER_LIST_TOP + 6, TEXT_12, COLOR_DIM, "No dated screenshots found.");
        return;
    }
    int start = compute_list_start(s_filterCursor, s_availableYearCount);
    for (int i = 0; i < FILTER_VISIBLE_ROWS && start + i < s_availableYearCount; i++) {
        int idx = start + i;
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", s_availableYears[idx]);
        draw_filter_row(FILTER_LIST_TOP + i * FILTER_ROW_HEIGHT, buf, NULL,
                         true, s_availableYears[idx] == s_filterYear, idx == s_filterCursor);
    }
    if (s_availableYearCount > FILTER_VISIBLE_ROWS) draw_filter_scrollbar(start, s_availableYearCount);
}

static void draw_day_picker(void)
{
    draw_header(NULL, "Select Day");
    int total = s_availableDayCount + 1; // "All" + real days
    int start = compute_list_start(s_filterCursor, total);
    for (int i = 0; i < FILTER_VISIBLE_ROWS && start + i < total; i++) {
        int idx = start + i;
        char buf[8];
        const char *label;
        bool active;
        if (idx == 0) {
            label = "All";
            active = (s_filterDay == 0);
        } else {
            snprintf(buf, sizeof(buf), "%d", s_availableDays[idx - 1]);
            label = buf;
            active = (s_availableDays[idx - 1] == s_filterDay);
        }
        draw_filter_row(FILTER_LIST_TOP + i * FILTER_ROW_HEIGHT, label, NULL, true, active, idx == s_filterCursor);
    }
    if (total > FILTER_VISIBLE_ROWS) draw_filter_scrollbar(start, total);
}


static void draw_bottom_screen(void)
{
    C2D_TargetClear(s_bot, COLOR_BG);
    C2D_SceneBegin(s_bot);

    // The bottom-capture peek (holding R in the detail view) fills the
    // whole 320x240 screen per the mockup, so it skips the footer bar.
    bool peekingBottomCapture = (s_state == APP_DETAIL) && (hidKeysHeld() & KEY_R);
    if (!peekingBottomCapture) {
        C2D_DrawRectSolid(0, FOOTER_Y, 0, 320, FOOTER_H, COLOR_BG);
        draw_frame_rule(FOOTER_Y);
    }

    switch (s_state) {
    case APP_BROWSE:
        draw_grid();
        if (s_batchMode) {
            bool anySelected = false;
            for (int i = 0; i < s_pairCount; i++) if (s_batchSelected[i]) { anySelected = true; break; }
            const FooterHint batchHints[] = {
                { ICON_BTN_A, "OK", false }, { ICON_BTN_B, "Back", false },
                { ICON_BTN_X, "Delete", !anySelected },
                { ICON_BTN_L, "Copy to Album", !anySelected },
            };
            draw_footer_hints(batchHints, 4);
        } else {
            static const FooterHint browseHints[] = {
                { ICON_BTN_A, "More" }, { ICON_BTN_Y, "Filter" },
                { ICON_BTN_SELECT, "Batch Select" },
            };
            draw_footer_hints(browseHints, 3);
        }
        break;

    case APP_DETAIL:
        if (peekingBottomCapture) {
            draw_bottom_capture();
        } else {
            draw_detail_menu();
            static const FooterHint detailHints[] = {
                { ICON_BTN_A, "OK" }, { ICON_BTN_B, "Back" }, { ICON_BTN_R, "Show Bottom" },
            };
            draw_footer_hints(detailHints, 3);
        }
        break;

    case APP_DETAIL_DELETE_CONFIRM:
        draw_detail_menu();
        draw_confirm_popup("The file will be deleted", NULL, "Cancel", "Delete", s_confirmSelection);
        break;

    case APP_DETAIL_DELETING:
        draw_detail_menu();
        draw_popup("Deleting...", NULL, true, false);
        break;

    case APP_CONVERTING:
        draw_grid();
        draw_popup("Converting...", NULL, true, false);
        break;

    case APP_RESULT:
        draw_grid();
        draw_popup(s_lastMessage, s_lastOutputPath, false, true);
        static const FooterHint resultHints[] = { { ICON_BTN_A, "Continue" } };
        draw_footer_hints(resultHints, 1);
        break;

    case APP_BATCH_CONVERTING:
        draw_grid();
        draw_popup("Converting...", NULL, true, false);
        break;

    case APP_BATCH_RESULT:
        draw_grid();
        draw_confirm_popup(s_lastMessage, s_lastOutputPath, "Back", "Delete", s_confirmSelection);
        break;

    case APP_BATCH_DELETE_CONFIRM:
        draw_grid();
        draw_confirm_popup(s_lastMessage, NULL, "Cancel", "Delete", s_confirmSelection);
        break;

    case APP_BATCH_DELETING:
        draw_grid();
        draw_popup("Deleting...", NULL, true, false);
        break;

    case APP_FILTER_MENU:
        draw_filter_menu();
        static const FooterHint filterHints[] = {
            { ICON_BTN_A, "OK" }, { ICON_BTN_B, "Back" },
        };
        draw_footer_hints(filterHints, 2);
        break;

    case APP_FILTER_BY_DATE:
        draw_by_date_menu();
        draw_footer_hints(g_dateHints, 3);
        break;

    case APP_FILTER_MONTH_PICKER:
    case APP_FILTER_YEAR_PICKER:
    case APP_FILTER_DAY_PICKER:
        if (s_state == APP_FILTER_MONTH_PICKER) draw_month_picker();
        else if (s_state == APP_FILTER_YEAR_PICKER) draw_year_picker();
        else draw_day_picker();
        draw_footer_hints(g_dateHints, 3);
        break;
    }
}

bool ui_frame(void)
{
    hidScanInput();
    u32 kDown = hidKeysDown();
    s_spinnerAngle += 0.15f;

    touchPosition touch;
    hidTouchRead(&touch);
    bool tapped = (kDown & KEY_TOUCH) != 0;

    if (kDown & KEY_START) return false;

    // Face and shoulder buttons click; the D-Pad deliberately doesn't,
    // since it fires constantly while navigating a grid.
    if (kDown & (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_L | KEY_R | KEY_SELECT)) {
        audio_play(SFX_BUTTON);
    }

    // A tap on a footer hint acts as a press of that button, so every
    // on-screen prompt is genuinely touchable.
    if (tapped) {
        for (int i = 0; i < s_footerHitCount; i++) {
            FooterHitbox *hb = &s_footerHits[i];
            if (hb->key && !hb->disabled &&
                point_in_rect(touch.px, touch.py, hb->x, hb->y, hb->w, hb->h)) {
                kDown |= hb->key;
                tapped = false; // consumed -- don't also treat it as a content tap
                audio_play(SFX_BUTTON);
                break;
            }
        }
    }

    AppState stateBefore = s_state;

    switch (s_state) {
    case APP_BROWSE:
        if (kDown & KEY_DOWN) {
            int next = s_selected + GRID_COLS;
            if (next < s_visibleCount) s_selected = next;
        }
        if (kDown & KEY_UP) {
            int next = s_selected - GRID_COLS;
            if (next >= 0) s_selected = next;
        }
        if (kDown & KEY_RIGHT && s_selected + 1 < s_visibleCount) s_selected++;
        if (kDown & KEY_LEFT && s_selected > 0) s_selected--;

        if (s_batchMode) {
            if (kDown & KEY_A) {
                ScreenshotPair *p = current_pair();
                if (p) {
                    int idx = s_visibleIndices[s_selected];
                    s_batchSelected[idx] = !s_batchSelected[idx];
                }
            }
            if (tapped) {
                int visPos;
                if (grid_hit_test(touch.px, touch.py, &visPos)) {
                    int idx = s_visibleIndices[visPos];
                    s_batchSelected[idx] = !s_batchSelected[idx];
                }
            }
            if (kDown & (KEY_B | KEY_SELECT)) {
                s_batchMode = false;
                memset(s_batchSelected, 0, sizeof(s_batchSelected));
            }
            bool anySel = false;
            for (int i = 0; i < s_pairCount; i++) if (s_batchSelected[i]) { anySel = true; break; }
            if ((kDown & KEY_X) && anySel) begin_batch_delete();
            if ((kDown & KEY_L) && anySel) begin_batch_convert();
        } else {
            if (kDown & KEY_A) enter_detail_view();
            if (tapped) {
                int visPos;
                if (grid_hit_test(touch.px, touch.py, &visPos)) {
                    // Tapping both moves the cursor there and opens it directly.
                    s_selected = visPos;
                    enter_detail_view();
                }
            }
            if (kDown & KEY_SELECT) {
                s_batchMode = true;
                memset(s_batchSelected, 0, sizeof(s_batchSelected));
            }
            if (kDown & KEY_Y) begin_filter_menu();
        }
        break;

    case APP_DETAIL: {
        bool rHeld = (hidKeysHeld() & KEY_R) != 0;
        poll_bottom_capture();
        if (!rHeld) {
            if (kDown & (KEY_UP | KEY_DOWN)) s_detailMenuSelection = !s_detailMenuSelection;
            if (kDown & KEY_B) s_state = APP_BROWSE;
            if (kDown & KEY_A) {
                if (s_detailMenuSelection == 0) begin_convert();
                else { s_confirmSelection = 0; s_state = APP_DETAIL_DELETE_CONFIRM; }
            }
            if (tapped) {
                int idx;
                if (detail_menu_hit_test(touch.px, touch.py, &idx)) {
                    s_detailMenuSelection = idx;
                    if (idx == 0) begin_convert();
                    else { s_confirmSelection = 0; s_state = APP_DETAIL_DELETE_CONFIRM; }
                }
            }
        }
        break;
    }

    case APP_DETAIL_DELETE_CONFIRM: {
        if (kDown & (KEY_LEFT | KEY_RIGHT)) s_confirmSelection = !s_confirmSelection;
        if (kDown & KEY_B) s_state = APP_DETAIL;
        if (kDown & KEY_A) {
            if (s_confirmSelection == 0) s_state = APP_DETAIL; else op_enter(APP_DETAIL_DELETING);
        }
        if (tapped) {
            float cx, cy, cw, ch, dx, dy, dw, dh;
            popup_button_rect(0, 2, &cx, &cy, &cw, &ch);
            popup_button_rect(1, 2, &dx, &dy, &dw, &dh);
            if (point_in_rect(touch.px, touch.py, cx, cy, cw, ch)) {
                s_state = APP_DETAIL;
            } else if (point_in_rect(touch.px, touch.py, dx, dy, dw, dh)) {
                op_enter(APP_DETAIL_DELETING);
            }
        }
        break;
    }

    case APP_DETAIL_DELETING:
        if (op_tick()) do_delete_current_pair();
        break;

    case APP_CONVERTING:
        if (op_tick()) do_convert();
        break;

    case APP_RESULT: {
        // A or B both dismiss. The single-button popup has nothing to
        // choose between, so accepting B as well avoids any chance of
        // it feeling stuck.
        float bx, by, bw, bh;
        popup_button_rect(0, 1, &bx, &by, &bw, &bh);
        if ((kDown & (KEY_A | KEY_B)) ||
            (tapped && point_in_rect(touch.px, touch.py, bx, by, bw, bh))) {
            s_state = APP_BROWSE;
        }
        break;
    }

    case APP_BATCH_CONVERTING:
        s_opFrames++;
        if (!s_opDone) do_batch_convert_step();
        else if (s_opFrames >= OP_MIN_FRAMES) s_state = s_opNextState;
        break;

    case APP_BATCH_RESULT: {
        // Doubles as the "copied -- delete originals?" prompt.
        // "Back" (index 0, default) keeps the originals and stays in
        // batch mode with the selection cleared; "Delete" (index 1)
        // removes exactly the items that were just copied.
        if (kDown & (KEY_LEFT | KEY_RIGHT)) s_confirmSelection = !s_confirmSelection;
        float cx, cy, cw, ch, dx, dy, dw, dh;
        popup_button_rect(0, 2, &cx, &cy, &cw, &ch);
        popup_button_rect(1, 2, &dx, &dy, &dw, &dh);
        bool pickedBack = (kDown & KEY_A && s_confirmSelection == 0) ||
                           (tapped && point_in_rect(touch.px, touch.py, cx, cy, cw, ch));
        bool pickedDelete = (kDown & KEY_A && s_confirmSelection == 1) ||
                             (tapped && point_in_rect(touch.px, touch.py, dx, dy, dw, dh));
        if (pickedBack) {
            memset(s_batchSelected, 0, sizeof(s_batchSelected));
            s_state = APP_BROWSE; // stays in batch mode (s_batchMode untouched)
        } else if (pickedDelete) {
            op_enter(APP_BATCH_DELETING);
        }
        break;
    }

    case APP_BATCH_DELETE_CONFIRM: {
        if (kDown & (KEY_LEFT | KEY_RIGHT)) s_confirmSelection = !s_confirmSelection;
        if (kDown & KEY_B) s_state = APP_BROWSE; // cancel -- stays in batch mode, selection kept
        if (kDown & KEY_A) {
            if (s_confirmSelection == 0) s_state = APP_BROWSE; else op_enter(APP_BATCH_DELETING);
        }
        if (tapped) {
            float cx, cy, cw, ch, dx, dy, dw, dh;
            popup_button_rect(0, 2, &cx, &cy, &cw, &ch);
            popup_button_rect(1, 2, &dx, &dy, &dw, &dh);
            if (point_in_rect(touch.px, touch.py, cx, cy, cw, ch)) {
                s_state = APP_BROWSE;
            } else if (point_in_rect(touch.px, touch.py, dx, dy, dw, dh)) {
                op_enter(APP_BATCH_DELETING);
            }
        }
        break;
    }

    case APP_BATCH_DELETING:
        if (op_tick()) do_batch_delete_selected();
        break;

    case APP_FILTER_MENU: {
        if (kDown & KEY_DOWN && s_filterCursor < 3) s_filterCursor++;
        if (kDown & KEY_UP && s_filterCursor > 0) s_filterCursor--;
        if (kDown & KEY_B) s_state = APP_BROWSE;

        int activated = -1;
        if (kDown & KEY_A) activated = s_filterCursor;
        if (tapped) {
            for (int i = 0; i < 4; i++) {
                float y = filter_menu_row_y(i);
                if (touch.py >= y && touch.py < y + FILTER_ROW_HEIGHT) { activated = i; s_filterCursor = i; break; }
            }
        }
        if (activated == 0 || activated == 1 || activated == 2) {
            s_filterMode = activated == 0 ? FILTER_ALL : activated == 1 ? FILTER_3D_ONLY : FILTER_2D_ONLY;
            rebuild_visible_list();
            s_flashFrames = 3;
            s_state = APP_BROWSE;
        } else if (activated == 3) {
            begin_filter_by_date();
        }
        break;
    }

    case APP_FILTER_BY_DATE: {
        int rowCount = by_date_row_count();
        if (s_filterCursor >= rowCount) s_filterCursor = rowCount - 1; // clamp if Day just disappeared

        if (kDown & KEY_DOWN && s_filterCursor < rowCount - 1) s_filterCursor++;
        if (kDown & KEY_UP && s_filterCursor > 0) s_filterCursor--;
        if (kDown & KEY_B) { s_filterCursor = 3; s_state = APP_FILTER_MENU; }
        if (kDown & KEY_A) {
            if (s_filterCursor == 0) begin_year_picker();
            else if (s_filterCursor == 1) begin_month_picker();
            else if (s_filterCursor == 2) begin_day_picker();
        }
        if (tapped) {
            for (int i = 0; i < rowCount; i++) {
                float y = FILTER_LIST_TOP + i * FILTER_ROW_HEIGHT;
                if (touch.py >= y && touch.py < y + FILTER_ROW_HEIGHT) {
                    s_filterCursor = i;
                    if (i == 0) begin_year_picker();
                    else if (i == 1) begin_month_picker();
                    else if (i == 2) begin_day_picker();
                    break;
                }
            }
        }
        if (kDown & KEY_Y) apply_date_filter_if_ready();
        break;
    }

    case APP_FILTER_MONTH_PICKER: {
        int total = s_availableMonthCount + 1; // "All" + real months
        if (kDown & KEY_DOWN && s_filterCursor < total - 1) s_filterCursor++;
        if (kDown & KEY_UP && s_filterCursor > 0) s_filterCursor--;
        if (kDown & KEY_B) { s_filterCursor = 1; s_state = APP_FILTER_BY_DATE; }

        int chosen = -1;
        if (kDown & KEY_A) chosen = s_filterCursor;
        if (tapped) {
            int start = compute_list_start(s_filterCursor, total);
            int idx;
            if (list_row_hit_test(touch.py, start, total, &idx)) { chosen = idx; s_filterCursor = idx; }
        }
        if (chosen >= 0) {
            int newMonth = (chosen == 0) ? 0 : s_availableMonths[chosen - 1];
            // Changing the month invalidates any specific day from a
            // different month's context -- reset it to "All".
            if (newMonth != s_filterMonth) { s_filterMonth = newMonth; s_filterDay = 0; }
            s_filterCursor = 1; // back onto the Month row
            s_state = APP_FILTER_BY_DATE;
        }
        if (kDown & KEY_Y) apply_date_filter_if_ready();
        break;
    }

    case APP_FILTER_YEAR_PICKER: {
        if (kDown & KEY_DOWN && s_filterCursor < s_availableYearCount - 1) s_filterCursor++;
        if (kDown & KEY_UP && s_filterCursor > 0) s_filterCursor--;
        if (kDown & KEY_B) { s_filterCursor = 0; s_state = APP_FILTER_BY_DATE; }

        int chosen = -1;
        if (kDown & KEY_A) chosen = s_filterCursor;
        if (tapped) {
            int start = compute_list_start(s_filterCursor, s_availableYearCount);
            int idx;
            if (list_row_hit_test(touch.py, start, s_availableYearCount, &idx)) { chosen = idx; s_filterCursor = idx; }
        }
        if (chosen >= 0 && chosen < s_availableYearCount) {
            int newYear = s_availableYears[chosen];
            // Changing the year resets Month/Day to "All" -- a
            // previously-picked month/day may not even exist in the
            // new year, and starting broad again is the predictable
            // choice (same principle as month->day above).
            if (newYear != s_filterYear) {
                s_filterYear = newYear;
                s_filterMonth = 0;
                s_filterDay = 0;
            }
            s_filterCursor = 0; // back onto the Year row
            s_state = APP_FILTER_BY_DATE;
        }
        if (kDown & KEY_Y) apply_date_filter_if_ready();
        break;
    }

    case APP_FILTER_DAY_PICKER: {
        int total = s_availableDayCount + 1; // "All" + real days
        if (kDown & KEY_DOWN && s_filterCursor < total - 1) s_filterCursor++;
        if (kDown & KEY_UP && s_filterCursor > 0) s_filterCursor--;
        if (kDown & KEY_B) { s_filterCursor = 2; s_state = APP_FILTER_BY_DATE; }

        int chosen = -1;
        if (kDown & KEY_A) chosen = s_filterCursor;
        if (tapped) {
            int start = compute_list_start(s_filterCursor, total);
            int idx;
            if (list_row_hit_test(touch.py, start, total, &idx)) { chosen = idx; s_filterCursor = idx; }
        }
        if (chosen >= 0) {
            s_filterDay = (chosen == 0) ? 0 : s_availableDays[chosen - 1];
            s_filterCursor = 2; // back onto the Day row
            s_state = APP_FILTER_BY_DATE;
        }
        if (kDown & KEY_Y) apply_date_filter_if_ready();
        break;
    }
    }

    // A popup "appearing" is any transition into a state that shows
    // one -- that drives both the drop-in animation and its sound.
    if (s_state != stateBefore) {
        if (state_has_popup(s_state) && !state_has_popup(stateBefore)) {
            s_popupAnim = 0;
            audio_play(SFX_POPUP);
        }
    }
    if (s_popupAnim < POPUP_ANIM_FRAMES) s_popupAnim++;
    if (s_thumbSfxMute > 0) s_thumbSfxMute--;

    C2D_TextBufClear(s_dynBuf);

    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    draw_top_screen();
    draw_bottom_screen();
    if (s_flashFrames > 0) {
        C2D_SceneBegin(s_top);
        C2D_DrawRectSolid(0, 0, 0, 400, 240, C2D_Color32(0, 0, 0, 0xff));
        C2D_SceneBegin(s_bot);
        C2D_DrawRectSolid(0, 0, 0, 320, 240, C2D_Color32(0, 0, 0, 0xff));
        s_flashFrames--;
    }
    C3D_FrameEnd(0);

    return true;
}
