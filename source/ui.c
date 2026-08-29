#include "ui.h"
#include "bmp.h"
#include "mpo.h"
#include "fs_utils.h"
#include "assets/icons_data.h"
#include "assets/easter_egg_data.h"
#include "audio.h"
#include "ds_utils.h"
#include <stdarg.h>
#include <math.h>
#include <time.h>  // duplicate/import timestamps
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
    // DS import flow: prompt -> extracting -> prompt -> clearing
    APP_DS_INTRO_POPUP,
    APP_DS_EXTRACT_PROMPT,      // Extract All (Start)
    APP_DS_EXTRACTING,
    APP_DS_CLEAR_PROMPT,        // "...delete from nds-bootstrap?" after Extract All
    APP_DS_CLEARING,
    // Single-screenshot equivalents, from the More screen
    APP_DS_ONE_EXTRACT_PROMPT,
    APP_DS_ONE_EXTRACTING,
    APP_DS_ONE_DELETE_PROMPT,
    APP_DS_ONE_DELETING,
    APP_QUIT_CONFIRM,
<<<<<<< Updated upstream
=======
    APP_MERGE_CONFIRM,
    APP_MERGING,
    APP_EASTER_EGG,
>>>>>>> Stashed changes
    APP_MODE_SWITCHING,
    // Hidden L+R shortcut from the detail screen -- duplicates the
    // current screenshot, which is mostly a testing convenience.
    APP_DUPLICATE_CONFIRM,
    APP_DUPLICATING,
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
#define TEXT_10 0.55f

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
// Defined further down with the DS-mode state; declared here because
// the filter code below runs against whichever mode is active.
static int  item_count(void);
static void refresh_ds_availability(void);
static const char *item_timestamp(int idx);
static bool item_has_3d(int idx);
static int  detail_menu_item_count(void);

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
    case FILTER_3D_ONLY:  return item_has_3d(idx);
    case FILTER_2D_ONLY:  return !item_has_3d(idx);
    case FILTER_BY_DATE: {
        if (s_filterYear == 0) return true; // Year = "All" -- no date narrowing
        int y, mo, d;
        if (!parse_timestamp_ymd(item_timestamp(idx), &y, &mo, &d)) return false;
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
    for (int i = 0; i < item_count() && s_availableYearCount < MAX_FILTER_YEARS; i++) {
        int y, mo, d;
        if (!parse_timestamp_ymd(item_timestamp(i), &y, &mo, &d)) continue;
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
    for (int i = 0; i < item_count() && s_availableMonthCount < MAX_FILTER_MONTHS; i++) {
        int y, mo, d;
        if (!parse_timestamp_ymd(item_timestamp(i), &y, &mo, &d)) continue;
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
    for (int i = 0; i < item_count() && s_availableDayCount < MAX_FILTER_DAYS; i++) {
        int y, mo, d;
        if (!parse_timestamp_ymd(item_timestamp(i), &y, &mo, &d)) continue;
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

// ---- DS mode -----------------------------------------------------
// Comet runs in one of two modes. They deliberately share nothing but
// the drawing code: DS screenshots have no stereo pair, no 2D/3D
// badge, different dimensions, and only an import date rather than a
// capture date -- so keeping two separate lists is far simpler than
// making one list carry both shapes.
static bool          s_dsMode = false;
static DSScreenshot  s_dsShots[MAX_DS_SHOTS];
static int           s_dsCount = 0;
static bool          s_dsAvailable = false;   // any DS content at all? (drives the header hint)
static int           s_dsTarCount = 0;        // unextracted screenshots sitting in the tar
static int           s_dsExtractedThisRun = 0;

// DS mode has two libraries, not one: screenshots still sitting in
// nds-bootstrap's tar, and ones already extracted to Comet's own
// folder. They're different enough (tar items share one file at
// different byte offsets; extracted ones are ordinary standalone
// files) that they get entirely separate backing arrays.
typedef enum { DS_TAB_NDS_BOOTSTRAP, DS_TAB_SD_CARD } DSTab;
static DSTab     s_dsTab = DS_TAB_SD_CARD; // every open after the first defaults here
static DSTarSlot s_dsTarSlots[MAX_DS_TAR_SLOTS];
static int       s_dsTarSlotCount = 0;

// Mode-aware accessors. Everything downstream (thumbnails, preview,
// filtering, batch ops) goes through these rather than touching
// s_pairs/s_dsShots/s_dsTarSlots directly.
static int item_count(void)
{
    if (!s_dsMode) return s_pairCount;
    return (s_dsTab == DS_TAB_SD_CARD) ? s_dsCount : s_dsTarSlotCount;
}

static const char *item_top_path(int idx)
{
    if (!s_dsMode) return (idx >= 0 && idx < s_pairCount) ? s_pairs[idx].topPath : NULL;
    if (s_dsTab == DS_TAB_SD_CARD) return (idx >= 0 && idx < s_dsCount) ? s_dsShots[idx].path : NULL;
    // Every nds-bootstrap item lives in the same file, at a different
    // byte offset -- see item_data_offset().
    return (idx >= 0 && idx < s_dsTarSlotCount) ? (SD_ROOT DS_TAR_PATH) : NULL;
}

// 0 for a standalone file (3DS pairs, extracted DS shots); a tar
// entry's payload offset for the nds-bootstrap tab.
static long item_data_offset(int idx)
{
    if (s_dsMode && s_dsTab == DS_TAB_NDS_BOOTSTRAP && idx >= 0 && idx < s_dsTarSlotCount)
        return s_dsTarSlots[idx].dataOffset;
    return 0;
}

static const char *item_right_path(int idx)
{
    if (s_dsMode) return NULL; // DS captures are single-screen, never stereo
    return (idx >= 0 && idx < s_pairCount && s_pairs[idx].has3D) ? s_pairs[idx].topRightPath : NULL;
}

static const char *item_timestamp(int idx)
{
    if (!s_dsMode) return (idx >= 0 && idx < s_pairCount) ? s_pairs[idx].timestamp : "";
    if (s_dsTab == DS_TAB_SD_CARD) return (idx >= 0 && idx < s_dsCount) ? s_dsShots[idx].timestamp : "";
    // nds-bootstrap tar entries carry no per-shot date at all (every
    // entry shares one fixed, meaningless mtime -- see TECHNICAL.md),
    // so this tab has no date filter to feed.
    return "";
}

static bool item_has_3d(int idx)
{
    if (s_dsMode) return false;
    return (idx >= 0 && idx < s_pairCount) && s_pairs[idx].has3D;
}
static int  s_selected = 0;

static void rebuild_visible_list(void)
{
    s_visibleCount = 0;
    int total = item_count();
    for (int i = 0; i < total; i++) {
        if (!pair_matches_filter(i)) continue;
        s_visibleIndices[s_visibleCount++] = i;
    }
    if (s_selected >= s_visibleCount) s_selected = s_visibleCount > 0 ? s_visibleCount - 1 : 0;
    if (s_selected < 0) s_selected = 0;
}

// Finds the entry with the given path in the current visible list and
// moves the cursor there. Shared by every operation that needs to
// land on a specific item after a rescan reshuffles indices -- matching
// by path is exact regardless of how qsort happens to order entries
// that share a timestamp (merge and duplicate both produce one).
static bool select_item_by_path(const char *path)
{
    if (!path) return false;
    for (int i = 0; i < s_visibleCount; i++) {
        const char *itemPath = item_top_path(s_visibleIndices[i]);
        if (itemPath && strcmp(itemPath, path) == 0) {
            s_selected = i;
            return true;
        }
    }
    return false;
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

// Where APP_RESULT's OK button returns to. Defaults to APP_BROWSE
// (the original, only behavior) -- an operation that wants to stay on
// the More screen instead (Copy to Album) sets this just before
// completing to APP_RESULT, and it's reset back to the default the
// moment it's consumed, so it can never leak into an unrelated later
// use of the same result screen (e.g. a Merge failure right after).
static AppState s_resultReturnState = APP_BROWSE;
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

// DS-only: some DS games have widescreen patches that render 16:10
// content into the same 256x192 framebuffer, so it looks squashed at
// native size. Lives on each DSScreenshot entry (see ds_utils.h)
// rather than as a standalone flag here -- it needs to follow the
// specific file, not "whatever's currently selected", or toggling one
// screenshot would incorrectly carry over to the next one you look at.
// Remembers which filenames are currently marked widescreen, then
// restores that after a fresh scan. Static rather than stack-allocated
// -- MAX_DS_SHOTS full paths would be a meaningful chunk of the 3DS's
// limited stack, and this only ever needs one instance.
static char s_dsWidescreenPaths[MAX_DS_SHOTS][256];
static int  s_dsWidescreenPathCount = 0;

#define WIDESCREEN_PREFS_PATH SD_ROOT "/3ds/Comet/widescreen.txt"
#define DS_INTRO_SEEN_PATH    SD_ROOT "/3ds/Comet/ds_intro_seen.txt"
#define TAR_WIDESCREEN_PATH   SD_ROOT "/3ds/Comet/widescreen_tar.txt"

// Widescreen marks for tar screenshots are keyed on a content
// fingerprint, not slot position -- see ds_slot_fingerprint().
static unsigned long s_tarWidescreenFps[MAX_DS_TAR_SLOTS];
static int           s_tarWidescreenFpCount = 0;

static void load_tar_widescreen_prefs(void)
{
    s_tarWidescreenFpCount = 0;
    FILE *f = fopen(TAR_WIDESCREEN_PATH, "r");
    if (!f) return;
    unsigned long v;
    while (s_tarWidescreenFpCount < MAX_DS_TAR_SLOTS && fscanf(f, "%lu", &v) == 1) {
        s_tarWidescreenFps[s_tarWidescreenFpCount++] = v;
    }
    fclose(f);
}

static void save_tar_widescreen_prefs(void)
{
    // Rebuild the in-memory list as well as the file. apply_...() reads
    // the in-memory copy, so leaving it stale here meant every
    // re-listing (tab switch, mode switch, delete) re-applied the
    // startup snapshot and silently cleared whatever was just toggled.
    s_tarWidescreenFpCount = 0;
    for (int i = 0; i < s_dsTarSlotCount && s_tarWidescreenFpCount < MAX_DS_TAR_SLOTS; i++) {
        if (s_dsTarSlots[i].widescreen) {
            s_tarWidescreenFps[s_tarWidescreenFpCount++] = ds_slot_fingerprint(&s_dsTarSlots[i]);
        }
    }

    fs_ensure_dir_exists(SD_ROOT "/3ds/Comet");
    FILE *f = fopen(TAR_WIDESCREEN_PATH, "w");
    if (!f) return;
    for (int i = 0; i < s_tarWidescreenFpCount; i++) {
        fprintf(f, "%lu\n", s_tarWidescreenFps[i]);
    }
    fclose(f);
}

// Re-applies saved marks after any tar listing.
static void apply_tar_widescreen_prefs(void)
{
    for (int i = 0; i < s_dsTarSlotCount; i++) {
        unsigned long fp = ds_slot_fingerprint(&s_dsTarSlots[i]);
        for (int j = 0; j < s_tarWidescreenFpCount; j++) {
            if (s_tarWidescreenFps[j] == fp) { s_dsTarSlots[i].widescreen = true; break; }
        }
    }
}

static bool ds_intro_already_seen(void)
{
    FILE *f = fopen(DS_INTRO_SEEN_PATH, "r");
    if (!f) return false;
    fclose(f);
    return true;
}

static void mark_ds_intro_seen(void)
{
    fs_ensure_dir_exists(SD_ROOT "/3ds/Comet");
    FILE *f = fopen(DS_INTRO_SEEN_PATH, "w");
    if (f) { fputs("1", f); fclose(f); }
}

static void save_widescreen_prefs(void)
{
    // Cheap defensive guard -- extraction always creates this folder
    // first in practice, but this costs nothing if it already exists
    // and removes any dependency on that ordering holding forever.
    fs_ensure_dir_exists(SD_ROOT "/3ds/Comet");
    FILE *f = fopen(WIDESCREEN_PREFS_PATH, "w");
    if (!f) return; // best-effort -- losing this preference isn't worth surfacing an error over
    for (int i = 0; i < s_dsCount; i++) {
        if (s_dsShots[i].widescreen) fprintf(f, "%s\n", s_dsShots[i].path);
    }
    fclose(f);
}

static void load_widescreen_prefs(void)
{
    s_dsWidescreenPathCount = 0;
    FILE *f = fopen(WIDESCREEN_PREFS_PATH, "r");
    if (!f) return; // no saved prefs yet

    char line[256];
    while (fgets(line, sizeof(line), f) && s_dsWidescreenPathCount < MAX_DS_SHOTS) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (len == 0) continue;
        snprintf(s_dsWidescreenPaths[s_dsWidescreenPathCount], 256, "%s", line);
        s_dsWidescreenPathCount++;
    }
    fclose(f);
}

static void ds_rescan_preserving_widescreen(void)
{
    // Only re-capture from the in-memory list once something has
    // actually been scanned. On the very first scan after launch,
    // s_dsCount is 0 -- load_widescreen_prefs() already seeded
    // s_dsWidescreenPaths from disk at startup, and capturing here
    // would immediately wipe that out with an empty list.
    if (s_dsCount > 0) {
        s_dsWidescreenPathCount = 0;
        for (int i = 0; i < s_dsCount && s_dsWidescreenPathCount < MAX_DS_SHOTS; i++) {
            if (s_dsShots[i].widescreen) {
                snprintf(s_dsWidescreenPaths[s_dsWidescreenPathCount], 256, "%s", s_dsShots[i].path);
                s_dsWidescreenPathCount++;
            }
        }
    }

    s_dsCount = ds_scan_extracted(s_dsShots, MAX_DS_SHOTS);

    for (int i = 0; i < s_dsCount; i++) {
        for (int j = 0; j < s_dsWidescreenPathCount; j++) {
            if (strcmp(s_dsShots[i].path, s_dsWidescreenPaths[j]) == 0) {
                s_dsShots[i].widescreen = true;
                break;
            }
        }
    }
}

static bool current_ds_widescreen(void)
{
    if (!s_dsMode || s_selected < 0 || s_selected >= s_visibleCount) return false;
    int idx = s_visibleIndices[s_selected];
    if (s_dsTab == DS_TAB_SD_CARD) return (idx >= 0 && idx < s_dsCount) && s_dsShots[idx].widescreen;
    return (idx >= 0 && idx < s_dsTarSlotCount) && s_dsTarSlots[idx].widescreen;
}

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
    case APP_DS_EXTRACT_PROMPT:
    case APP_DS_EXTRACTING:
    case APP_DS_CLEAR_PROMPT:
    case APP_DS_CLEARING:
    case APP_DS_INTRO_POPUP:
    case APP_DS_ONE_EXTRACT_PROMPT:
    case APP_DS_ONE_EXTRACTING:
    case APP_DS_ONE_DELETE_PROMPT:
    case APP_DS_ONE_DELETING:
    case APP_QUIT_CONFIRM:
    case APP_MODE_SWITCHING:
    case APP_DUPLICATE_CONFIRM:
    case APP_DUPLICATING:
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
static bool decode_and_swizzle(const char *path, long base, u8 **outData,
                                u32 *outTexW, u32 *outTexH, u32 *outImgW, u32 *outImgH)
{
    RGBImage img;
    char err[128];
    if (!bmp_load_at(path, base, &img, err, sizeof(err))) return false;

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
    long baseLeft;   // 0 for a standalone file, or a tar entry's payload offset
    long baseRight;
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
        bool okL = decode_and_swizzle(job.pathLeft, job.baseLeft, &res.dataLeft, &res.texWLeft, &res.texHLeft, &res.imgWLeft, &res.imgHLeft);
        if (!okL) continue; // couldn't even load the left eye -- skip this job

        if (job.needRight) {
            decode_and_swizzle(job.pathRight, job.baseRight, &res.dataRight, &res.texWRight, &res.texHRight, &res.imgWRight, &res.imgHRight);
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

static int loader_request_at(const char *pathLeft, long baseLeft, const char *pathRight, long baseRight)
{
    LoaderJob job;
    snprintf(job.pathLeft, sizeof(job.pathLeft), "%s", pathLeft);
    job.baseLeft = baseLeft;
    job.baseRight = baseRight;
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
        if (decode_and_swizzle(job.pathLeft, job.baseLeft, &res.dataLeft, &res.texWLeft, &res.texHLeft, &res.imgWLeft, &res.imgHLeft)) {
            if (job.needRight) {
                decode_and_swizzle(job.pathRight, job.baseRight, &res.dataRight, &res.texWRight, &res.texHRight, &res.imgWRight, &res.imgHRight);
            }
            s_result = res;
            s_resultValid = true;
            s_resultId = id;
        }
    }
    return id;
}

// Plain-file convenience wrapper -- every non-tar caller still just
// wants "load this path", with no offset to think about.
static int loader_request(const char *pathLeft, const char *pathRight)
{
    return loader_request_at(pathLeft, 0, pathRight, 0);
}

// Both the top-screen preview and the bottom-screen capture poll this
// one shared result slot. A poll must NOT throw away a result just
// because it isn't its own -- it may belong to the other consumer,
// which hasn't polled yet this frame. Discarding it there is what
// left the preview (or the R-held bottom capture) spinning forever
// waiting for a job that had already completed and been destroyed.
static int s_previewRequestId = 0;
static int s_bottomCaptureRequestId = 0;

static bool loader_poll_result(int expectedId, LoaderResult *out)
{
    bool got = false;
    LightLock_Lock(&s_loaderLock);
    if (s_resultValid) {
        if (s_resultId == expectedId) {
            *out = s_result;
            got = true;
            s_resultValid = false;
        } else if (s_resultId == s_previewRequestId ||
                   s_resultId == s_bottomCaptureRequestId) {
            // Belongs to the other live consumer -- leave it in place
            // for them rather than freeing it out from under them.
        } else {
            // Genuinely stale (superseded request): safe to drop.
            free_loader_result(&s_result);
            s_resultValid = false;
        }
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

// True only once the top-screen preview actually matches the current
// selection -- not merely requested. Without this gate, moving the
// cursor and immediately pressing A (or tapping) could open the More
// screen for the newly-selected item while the top screen still shows
// the previous one's image, which is exactly the kind of mismatch
// that could lead to deleting the wrong screenshot.
static bool current_preview_ready(void)
{
    if (s_selected < 0 || s_selected >= s_visibleCount) return false;
    return s_visibleIndices[s_selected] == s_previewLoadedPairIndex;
}
static int s_previewRequestedPairIndex = -1;

// The bottom-screen capture Luma also saves (Rosalina's "_bot.bmp"),
// shown while holding R in the single-item detail view. Loaded once
// when entering that view (not continuously, unlike the top-screen
// live preview), reusing the same loader/texture pipeline.
static EyeTexture s_bottomCapture;
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

// --- easter egg: two dog photos + a pixel-art avatar, all baked in at
// compile time (assets/easter_egg_data.c) the same way the icons
// above are -- never read from the SD card. Loaded once, up front,
// same as the icons, since three images this size are cheap to keep
// resident for the app's whole lifetime.
static EyeTexture s_easterEggTex[EASTER_EGG_IMG_COUNT];

static void load_easter_egg_images(void)
{
    for (int i = 0; i < EASTER_EGG_IMG_COUNT; i++) {
        const EasterEggImage *src = &g_easterEggImages[i];
        EyeTexture *et = &s_easterEggTex[i];

        if (!C3D_TexInit(&et->tex, src->texW, src->texH, GPU_RGBA8)) continue;
        C3D_TexSetFilter(&et->tex, GPU_LINEAR, GPU_NEAREST);
        C3D_TexSetWrap(&et->tex, GPU_CLAMP_TO_BORDER, GPU_CLAMP_TO_BORDER);
        et->tex.border = 0x00000000; // transparent border, matches the avatar's own transparency

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
    // Whole-pixel placement -- see draw_icon_tinted.
    C2D_DrawImageAt(et->image, floorf(x + 0.5f), floorf(y + 0.5f), 0.0f, NULL, 1.0f, 1.0f);
}

// Half-opacity variant for disabled footer actions.
static void draw_icon_tinted(IconId id, float x, float y, bool dim)
{
    EyeTexture *et = &s_icons[id];
    if (!et->valid) return;
    x = floorf(x + 0.5f);
    y = floorf(y + 0.5f);
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

static void build_thumbnail(const char *bmpPath, long base, Thumbnail *out)
{
    out->attempted = true;

    u8 rgb[THUMB_COLS * THUMB_ROWS * 3];
    char err[64];
    if (!bmp_load_thumbnail_at(bmpPath, base, THUMB_COLS, THUMB_ROWS, rgb, err, sizeof(err))) {
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
    if (s_selected < 0 || s_selected >= s_visibleCount) return;

    int idx = s_visibleIndices[s_selected];
    if (idx != s_previewLoadedPairIndex && idx != s_previewRequestedPairIndex) {
        s_previewRequestId = loader_request_at(item_top_path(idx), item_data_offset(idx), item_right_path(idx), 0);
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

// SemiBold variant -- now used for exactly one thing: the page title
// in the header's left slot. It's the only place SemiBold appears
// anywhere in the app since the old centred "Comet" wordmark (which
// used to be the sole SemiBold user) was replaced by the icon alone.
static void draw_text_vcenter_semibold(float x, float y, float h, float scale, u32 color, const char *str)
{
    C2D_Text t;
    C2D_TextFontParse(&t, s_fontSemiBold, s_dynBuf, str);
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

// Same, but for an explicit font -- the path display needs to measure
// against SemiBold, which is wider than Regular at the same scale, so
// measuring with the wrong font would wrap in the wrong place.
static float measure_text_font(C2D_Font font, float scale, const char *str)
{
    C2D_Text t;
    C2D_TextFontParse(&t, font, s_dynBuf, str);
    C2D_TextOptimize(&t);
    float w, h;
    C2D_TextGetDimensions(&t, scale, scale, &w, &h);
    return w;
}

static void draw_text_semibold(float x, float y, float scale, u32 color, const char *str)
{
    C2D_Text t;
    C2D_TextFontParse(&t, s_fontSemiBold, s_dynBuf, str);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, x, y, 0.0f, scale, scale, color);
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
    const float gapIconLabel = 5.0f, gapItems = 12.0f;
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

// Draws the message block starting at an explicit Y, for layouts where
// centring in the remaining space isn't what's wanted.
static void draw_popup_lines_at(const char *l1, const char *l2, float top)
{
    const float lineH = 18.0f;
    int row = 0;
    const char *lines[2] = { l1, l2 };
    for (int i = 0; i < 2; i++) {
        if (!lines[i]) continue;
        draw_centered_text(popup_x(), top + row * lineH, POPUP_W, lineH,
                            TEXT_12, COLOR_POPUP_TEXT, lines[i]);
        row++;
    }
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
        // Message sits in the upper third, spinner gets the rest --
        // the two were colliding once the SD-card warning added a
        // second line.
        draw_popup_lines_at(line1, line2, popup_y() + 12.0f);
        draw_spinner(popup_x() + POPUP_W / 2, popup_y() + POPUP_H - 32, 15);
    } else {
        draw_popup_lines(line1, line2, NULL, showOkButton);
    }

    if (showOkButton) {
        draw_popup_button_rules(1);
        draw_popup_button(0, 1, "OK", true);
    }
}

// Three-line variant of draw_popup -- the DS intro message needs the
// extra line, and no spinner (it's informational, not an operation).
static void draw_popup3(const char *line1, const char *line2, const char *line3,
                         bool showSpinner, bool showOkButton)
{
    (void)showSpinner; // three lines plus a spinner wouldn't fit the box
    C2D_DrawRectSolid(0, 0, 0, 320, 240, COLOR_OVERLAY);
    draw_rounded_rect(popup_x(), popup_y(), POPUP_W, POPUP_H, 2, COLOR_POPUP_BG);

    draw_popup_lines(line1, line2, line3, showOkButton);

    if (showOkButton) {
        draw_popup_button_rules(1);
        draw_popup_button(0, 1, "OK", true);
    }
}

static void draw_confirm_popup3(const char *line1, const char *line2, const char *line3,
                                 const char *leftLabel, const char *rightLabel, int selected)
{
    C2D_DrawRectSolid(0, 0, 0, 320, 240, COLOR_OVERLAY);
    draw_rounded_rect(popup_x(), popup_y(), POPUP_W, POPUP_H, 2, COLOR_POPUP_BG);

    draw_popup_lines(line1, line2, line3, true);

    draw_popup_button_rules(2);
    draw_popup_button(0, 2, leftLabel,  selected == 0);
    draw_popup_button(1, 2, rightLabel, selected == 1);
}

static void draw_confirm_popup(const char *line1, const char *line2,
                                const char *leftLabel, const char *rightLabel, int selected)
{
    draw_confirm_popup3(line1, line2, NULL, leftLabel, rightLabel, selected);
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
    load_easter_egg_images();
    audio_init();

    loader_init();

    int n = fs_scan_screenshot_pairs(s_pairs, MAX_PAIRS);
    s_pairCount = (n < 0) ? 0 : n;
    rebuild_visible_list();

    // Decides whether the L+R "DS Mode" hint shows at all.
    refresh_ds_availability();

    // Seeds s_dsWidescreenPaths from disk, before any DS scan has run.
    load_widescreen_prefs();
    load_tar_widescreen_prefs();
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

static void begin_filter_by_date(void); // fwd -- DS mode jumps straight there

static void begin_filter_menu(void)
{
    if (s_dsMode) {
        // Nothing to choose between in DS mode: no stereo variants, so
        // "By Extraction Date" is the whole filter menu.
        begin_filter_by_date();
        return;
    }
    s_filterCursor = (s_filterMode == FILTER_BY_DATE) ? 3
                    : (s_filterMode == FILTER_2D_ONLY) ? 2
                    : (s_filterMode == FILTER_3D_ONLY) ? 1 : 0;
    s_state = APP_FILTER_MENU;
}

static void begin_filter_by_date(void)
{
    scan_available_years();
    // Year defaults to the most recent available -- but only on a fresh
    // entry, never when a date filter is already active. Otherwise
    // deliberately choosing "All" would get silently undone the next
    // time this screen opened.
    if (s_filterMode != FILTER_BY_DATE && s_filterYear == 0 && s_availableYearCount > 0) {
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
    // Row 0 is "All" (no date filtering at all), so real years start
    // at index 1. Having an All here is what makes a date filter
    // clearable from inside the picker -- which matters most in DS
    // mode, where By Date is the only filter screen there is.
    s_filterCursor = 0;
    for (int i = 0; i < s_availableYearCount; i++) {
        if (s_availableYears[i] == s_filterYear) { s_filterCursor = i + 1; break; }
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
    // Year = "All" isn't an incomplete selection -- it's an explicit
    // "show everything", i.e. clearing the filter.
    s_filterMode = (s_filterYear <= 0) ? FILTER_ALL : FILTER_BY_DATE;
    rebuild_visible_list();
    s_flashFrames = 3;
    s_state = APP_BROWSE;
}

static void enter_detail_view(void)
{
    if (s_selected < 0 || s_selected >= s_visibleCount) return;
    s_detailMenuSelection = 0; // default to Copy to 3DS Album
    // DS captures have no companion bottom-screen frame to peek at.
    if (!s_dsMode) {
        ScreenshotPair *p = current_pair();
        if (p) request_bottom_capture(p);
    }
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
    int deletedAt = s_selected; // remember position before the list shrinks

    if (s_dsMode) {
        int idx = (s_selected >= 0 && s_selected < s_visibleCount)
                    ? s_visibleIndices[s_selected] : -1;
        if (s_dsTab == DS_TAB_NDS_BOOTSTRAP) {
            // Deleting from the tar means a compacting rewrite, not a
            // file removal -- and the index refers to a tar slot, not
            // an s_dsShots entry.
            if (idx >= 0 && idx < s_dsTarSlotCount) ds_delete_tar_slot(idx);
        } else if (idx >= 0 && idx < s_dsCount) {
            ds_delete(&s_dsShots[idx]);
        }
    } else {
        ScreenshotPair *p = current_pair();
        if (p) fs_delete_pair(p);
    }

    free_eye_texture(&s_bottomCapture);
    s_bottomCaptureRequested = false;

    free_all_thumbnails();
    memset(s_thumbs, 0, sizeof(s_thumbs));

    if (s_dsMode) {
        if (s_dsTab == DS_TAB_NDS_BOOTSTRAP) {
            s_dsTarSlotCount = ds_list_tar_slots(s_dsTarSlots, MAX_DS_TAR_SLOTS);
            apply_tar_widescreen_prefs();
            refresh_ds_availability();
        } else {
            ds_rescan_preserving_widescreen();
        }
    } else {
        int n = fs_scan_screenshot_pairs(s_pairs, MAX_PAIRS);
        s_pairCount = (n < 0) ? 0 : n;
    }
    rebuild_visible_list();

    free_preview_textures();
    s_previewLoadedPairIndex = -1;
    s_previewRequestedPairIndex = -1;

    audio_play(SFX_DELETE);
    s_thumbSfxMute = 45; // let the delete sound breathe

    if (s_visibleCount > 0) {
        // Land on whatever was directly before the deleted item --
        // clamped to the front if it was already first, and to the
        // new end in case the list shrank further than expected.
        s_selected = deletedAt - 1;
        if (s_selected < 0) s_selected = 0;
        if (s_selected >= s_visibleCount) s_selected = s_visibleCount - 1;

        // Menu selection may not fit the new item's shape (Merge only
        // shows when there's a real bottom capture).
        int newCount = detail_menu_item_count();
        if (s_detailMenuSelection >= newCount) s_detailMenuSelection = newCount - 1;

        // Same bottom-capture request enter_detail_view() does --
        // without it, R would peek at the deleted screenshot's now-
        // stale capture instead of the newly-landed-on one's.
        if (!s_dsMode) {
            ScreenshotPair *np = current_pair();
            if (np) request_bottom_capture(np);
        }
        op_complete(APP_DETAIL);
    } else {
        op_complete(APP_BROWSE);
    }
}

// Reloads whichever library the active mode points at, and resets all
// the per-item caches (thumbnails, preview) since indices now mean
// something completely different.
static void reload_current_mode(void)
{
    free_all_thumbnails();
    memset(s_thumbs, 0, sizeof(s_thumbs)); // clears the `attempted` flags too
    free_preview_textures();
    s_previewLoadedPairIndex = -1;
    s_previewRequestedPairIndex = -1;
    s_selected = 0;
    s_batchMode = false;
    memset(s_batchSelected, 0, sizeof(s_batchSelected));

    if (s_dsMode) {
        // Always refresh BOTH libraries, not just the active tab's --
        // the tab bar shows both counts simultaneously, and a stale
        // zero makes the other tab look empty and un-switchable.
        ds_rescan_preserving_widescreen();
        s_dsTarSlotCount = ds_list_tar_slots(s_dsTarSlots, MAX_DS_TAR_SLOTS);
        apply_tar_widescreen_prefs();
    } else {
        int n = fs_scan_screenshot_pairs(s_pairs, MAX_PAIRS);
        s_pairCount = (n < 0) ? 0 : n;
    }

    // Filters don't carry across modes -- a 3D-only filter is
    // meaningless for DS, and the date sets are unrelated. The
    // nds-bootstrap tab has no filter screen at all (see item_timestamp).
    s_filterMode = FILTER_ALL;
    s_filterYear = s_filterMonth = s_filterDay = 0;
    rebuild_visible_list();
}

// Refreshes whether the DS toggle should appear at all.
static void refresh_ds_availability(void)
{
    s_dsTarCount = ds_count_tar_screenshots();
    s_dsAvailable = (s_dsTarCount > 0) || (ds_count_extracted() > 0);
}

static void enter_ds_mode(void)
{
    s_dsMode = true;

    int tarCount = ds_count_tar_screenshots();
    int sdCount  = ds_count_extracted();
    bool firstTime = !ds_intro_already_seen();

    if (firstTime) {
        s_dsTab = DS_TAB_NDS_BOOTSTRAP;
        mark_ds_intro_seen();
    } else {
        // Defaults to SD Card, but won't land on an empty tab if the
        // other one actually has something to show.
        s_dsTab = (sdCount > 0 || tarCount == 0) ? DS_TAB_SD_CARD : DS_TAB_NDS_BOOTSTRAP;
    }

    if (firstTime) {
        // Deliberately NOT loading the grid yet -- scanning and
        // building thumbnails underneath the popup caused visible lag
        // and dropped inputs on the popup itself. The load happens
        // when the popup is dismissed.
        s_dsTarSlotCount = ds_list_tar_slots(s_dsTarSlots, MAX_DS_TAR_SLOTS);
        apply_tar_widescreen_prefs();
        s_dsCount = 0;
        s_visibleCount = 0;
        s_state = APP_DS_INTRO_POPUP;
    } else {
        reload_current_mode();
        s_state = APP_BROWSE;
    }
}

static void exit_ds_mode(void)
{
    s_dsMode = false;
    reload_current_mode();
    s_state = APP_BROWSE;
}

static void do_ds_extract(void)
{
    char err[128] = {0};
    int n = ds_extract_all(err, sizeof(err));

    if (n > 0) {
        s_dsExtractedThisRun = n;
        // The extracted copies now live in the SD Card tab, but we
        // stay on the nds-bootstrap tab's *data* until the user
        // actually chooses to clear it -- only the popup text refers
        // to what just happened.
        s_dsTarSlotCount = ds_list_tar_slots(s_dsTarSlots, MAX_DS_TAR_SLOTS);
        apply_tar_widescreen_prefs();
        free_all_thumbnails();
        memset(s_thumbs, 0, sizeof(s_thumbs));
        rebuild_visible_list();
        audio_play(SFX_COPY);
        s_thumbSfxMute = 45;
        s_confirmSelection = 0; // default to Cancel -- clearing is destructive
        op_complete(APP_DS_CLEAR_PROMPT);
    } else {
        s_lastSuccess = false;
        snprintf(s_lastMessage, sizeof(s_lastMessage), "Extraction failed:");
        snprintf(s_lastOutputPath, sizeof(s_lastOutputPath), "%s",
                 err[0] ? err : "No screenshots found.");
        op_complete(APP_RESULT);
    }
}

// ---- single-screenshot extract / delete (More screen) ------------

// Which tar slot the cursor is currently on, or -1.
static int current_tar_slot_index(void)
{
    if (!s_dsMode || s_dsTab != DS_TAB_NDS_BOOTSTRAP) return -1;
    if (s_selected < 0 || s_selected >= s_visibleCount) return -1;
    int idx = s_visibleIndices[s_selected];
    return (idx >= 0 && idx < s_dsTarSlotCount) ? idx : -1;
}

static void do_ds_extract_one(void)
{
    int idx = current_tar_slot_index();
    if (idx < 0) { op_complete(APP_BROWSE); return; }

    char err[128] = {0};
    bool ok = ds_extract_slot(&s_dsTarSlots[idx], err, sizeof(err));

    if (ok) {
        // The extracted copy now exists in the SD Card tab's folder.
        // Refresh that list so its count/tab is immediately accurate,
        // but stay on this tab -- the user hasn't been asked about
        // removing the original yet.
        ds_rescan_preserving_widescreen();
        refresh_ds_availability();
        audio_play(SFX_COPY);
        s_thumbSfxMute = 45;
        s_confirmSelection = 0; // default to Cancel -- deleting is destructive
        op_complete(APP_DS_ONE_DELETE_PROMPT);
    } else {
        s_lastSuccess = false;
        snprintf(s_lastMessage, sizeof(s_lastMessage), "Extraction failed:");
        snprintf(s_lastOutputPath, sizeof(s_lastOutputPath), "%s", err[0] ? err : "Unknown error.");
        op_complete(APP_RESULT);
    }
}

// After extracting one screenshot from the tar -- whether the user
// then cancels or confirms clearing the tar copy -- land on that
// file's own More page in the SD Card tab, rather than dropping to
// the grid.
static void do_ds_delete_one(void)
{
    int deletedAt = s_selected; // remember position before the list shrinks

    int idx = current_tar_slot_index();
    if (idx >= 0) {
        // Compacting delete -- repacks survivors into slots 1..N so
        // nds-bootstrap's "count until the first blank slot" logic
        // stays honest. See TECHNICAL.md.
        ds_delete_tar_slot(idx);
    }

    s_dsTarSlotCount = ds_list_tar_slots(s_dsTarSlots, MAX_DS_TAR_SLOTS);
    apply_tar_widescreen_prefs();
    free_all_thumbnails();
    memset(s_thumbs, 0, sizeof(s_thumbs));
    rebuild_visible_list();
    refresh_ds_availability();

    free_preview_textures();
    s_previewLoadedPairIndex = -1;
    s_previewRequestedPairIndex = -1;

    audio_play(SFX_DELETE);
    s_thumbSfxMute = 45;

    if (s_visibleCount > 0) {
        // Same "land on the previous item, same tab" pattern as the
        // regular delete flow -- unlike Cancel, there's no "same slot"
        // to stay on here, since compaction genuinely removed it.
        s_selected = deletedAt - 1;
        if (s_selected < 0) s_selected = 0;
        if (s_selected >= s_visibleCount) s_selected = s_visibleCount - 1;

        int newCount = detail_menu_item_count();
        if (s_detailMenuSelection >= newCount) s_detailMenuSelection = newCount - 1;

        op_complete(APP_DETAIL);
    } else {
        op_complete(APP_BROWSE);
    }
}

static void do_ds_clear(void)
{
    // Blanks every slot in place rather than deleting the tar --
    // nds-bootstrap has to rebuild the whole 50-slot file from
    // scratch if it's missing, which noticeably slows the next game
    // launch, and blanking everything sidesteps the slot-counting
    // quirk that makes a partial clear unsafe (see TECHNICAL.md).
    ds_clear_tar();
    refresh_ds_availability();
    audio_play(SFX_DELETE);

    // The tab just emptied out -- hand off to SD Card, where the
    // freshly-extracted screenshots actually are.
    s_dsTab = DS_TAB_SD_CARD;
    reload_current_mode();
    op_complete(APP_BROWSE);
}

// ---- duplicate (hidden L+R shortcut on the detail screen) --------
//
// Mostly a testing convenience: makes it easy to build up a large
// library without taking dozens of real screenshots.
static bool copy_file_to(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in) return false;
    fseek(in, 0, SEEK_END);
    long len = ftell(in);
    fseek(in, 0, SEEK_SET);
    if (len <= 0) { fclose(in); return false; }

    u8 *buf = (u8 *)malloc((size_t)len);
    if (!buf) { fclose(in); return false; }
    bool ok = fread(buf, 1, (size_t)len, in) == (size_t)len;
    fclose(in);

    if (ok) ok = fs_write_file(dst, buf, (size_t)len);
    free(buf);
    return ok;
}

static void current_time_stamp(char *out, size_t outSize)
{
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    if (lt) strftime(out, outSize, "%Y-%m-%d_%H-%M-%S", lt);
    else    snprintf(out, outSize, "0000-00-00_00-00-00");
}

static void do_duplicate(void)
{
    int idx = (s_selected >= 0 && s_selected < s_visibleCount)
                ? s_visibleIndices[s_selected] : -1;
    if (idx < 0) { op_complete(APP_DETAIL); return; }

    char stamp[24];
    current_time_stamp(stamp, sizeof(stamp));
    bool ok = false;
    char newPath[320] = "";

    if (s_dsMode) {
        // ".dup" keeps the copy from colliding with a real import made
        // in the same second.
        snprintf(newPath, sizeof(newPath), "%s%s/%s_dup%02d.bmp",
                 SD_ROOT, DS_SCREENSHOTS_DIR, stamp, idx);
        ok = copy_file_to(s_dsShots[idx].path, newPath);
        if (ok) ds_rescan_preserving_widescreen();
    } else {
        ScreenshotPair *p = &s_pairs[idx];
        char dst[320];
        // Every file in the set has to share one timestamp, or the
        // scanner won't recognise them as belonging to the same shot.
        snprintf(newPath, sizeof(newPath), "%s%s/%s.000_top.bmp", SD_ROOT, SCREENSHOTS_DIR, stamp);
        ok = copy_file_to(p->topPath, newPath);
        if (ok && p->has3D && p->topRightPath[0]) {
            snprintf(dst, sizeof(dst), "%s%s/%s.000_top_right.bmp", SD_ROOT, SCREENSHOTS_DIR, stamp);
            copy_file_to(p->topRightPath, dst);
        }
        if (ok && p->botPath[0]) {
            snprintf(dst, sizeof(dst), "%s%s/%s.000_bot.bmp", SD_ROOT, SCREENSHOTS_DIR, stamp);
            copy_file_to(p->botPath, dst);
        }
        if (ok) {
            int n = fs_scan_screenshot_pairs(s_pairs, MAX_PAIRS);
            s_pairCount = (n < 0) ? 0 : n;
        }
    }

    // Indices shifted, so cached thumbnails no longer line up.
    free_all_thumbnails();
    memset(s_thumbs, 0, sizeof(s_thumbs));
    rebuild_visible_list();

    // Land on the new duplicate, matching how merge does it -- for
    // consistency, per explicit request, even though this is really
    // just a debug convenience.
    if (ok) select_item_by_path(newPath);

    free_preview_textures();
    s_previewLoadedPairIndex = -1;
    s_previewRequestedPairIndex = -1;

    if (ok) {
        audio_play(SFX_COPY);
        s_thumbSfxMute = 45;
    }
    op_complete(APP_BROWSE);
}

static void begin_convert(void)
{
    if (s_selected < 0 || s_selected >= s_visibleCount) return;
    op_enter(APP_CONVERTING);
}

// Bakes the widescreen stretch into the pixels before export, so a
// screenshot marked widescreen in Comet looks the same in the 3DS
// Album. Point-sampled to match how it's previewed on the top screen.
static bool stretch_to_widescreen(RGBImage *img)
{
    if (!img->pixels || img->width <= 0 || img->height <= 0) return false;
    int newW = (int)((float)img->height * 16.0f / 10.0f + 0.5f);
    if (newW <= img->width) return true; // already at least that wide

    u8 *dst = (u8 *)malloc((size_t)newW * img->height * 3);
    if (!dst) return false;

    for (int y = 0; y < img->height; y++) {
        const u8 *srcRow = &img->pixels[(size_t)y * img->width * 3];
        u8 *dstRow = &dst[(size_t)y * newW * 3];
        for (int x = 0; x < newW; x++) {
            int sx = (int)((long)x * img->width / newW);
            if (sx >= img->width) sx = img->width - 1;
            memcpy(&dstRow[x * 3], &srcRow[sx * 3], 3);
        }
    }

    free(img->pixels);
    img->pixels = dst;
    img->width  = newW;
    return true;
}

// True if the item at `idx` is currently marked widescreen.
static bool item_is_widescreen(int idx)
{
    if (!s_dsMode || idx < 0) return false;
    if (s_dsTab == DS_TAB_SD_CARD) return idx < s_dsCount && s_dsShots[idx].widescreen;
    return idx < s_dsTarSlotCount && s_dsTarSlots[idx].widescreen;
}

<<<<<<< Updated upstream
=======
// ---- top/bottom merge (3DS only) ----------------------------------
//
// Deliberately simple: no stereo 3D, always writes a plain BMP the
// existing reader already fully understands, and reuses the existing
// scanner's naming convention rather than adding any special-casing
// there. "_merged" is inserted before the "_top.bmp" suffix so the
// output can never collide with the source pair's own filename, while
// still starting with the source's real timestamp -- since sscanf
// (used everywhere this gets parsed) only reads the leading date/time
// and ignores anything after, the merged entry still sorts, filters,
// and displays by its true original date.
static void do_merge_top_bottom(void)
{
    int idx = (s_selected >= 0 && s_selected < s_visibleCount)
                ? s_visibleIndices[s_selected] : -1;
    if (s_dsMode || idx < 0 || idx >= s_pairCount) { s_state = APP_DETAIL; return; }

    ScreenshotPair *p = &s_pairs[idx];
    char err[128] = {0};
    RGBImage top, bot;
    bool haveTop = false, haveBot = false, ok = false;
    char outPath[320] = "";

    haveTop = bmp_load(p->topPath, &top, err, sizeof(err));
    if (haveTop && p->botPath[0]) {
        haveBot = bmp_load(p->botPath, &bot, err, sizeof(err));
    }

    if (haveTop) {
        RGBImage merged;
        merged.width = 400;
        merged.height = 480;
        merged.pixels = (u8 *)calloc((size_t)400 * 480 * 3, 1); // black by default

        if (merged.pixels) {
            // Top half: the top screen is already exactly 400 wide,
            // straight copy, no scaling.
            for (int y = 0; y < 240 && y < top.height; y++) {
                memcpy(&merged.pixels[(size_t)y * 400 * 3],
                       &top.pixels[(size_t)y * top.width * 3],
                       (size_t)(top.width < 400 ? top.width : 400) * 3);
            }
            // Bottom half: the bottom screen is 320 wide, centred
            // within the 400-wide canvas -- the 40px margin on each
            // side stays black from the calloc above.
            if (haveBot) {
                int xOff = (400 - bot.width) / 2;
                if (xOff < 0) xOff = 0;
                for (int y = 0; y < 240 && y < bot.height; y++) {
                    memcpy(&merged.pixels[((size_t)(240 + y) * 400 + xOff) * 3],
                           &bot.pixels[(size_t)y * bot.width * 3],
                           (size_t)(bot.width < 400 ? bot.width : 400) * 3);
                }
            }

            // Write into whatever directory the source screenshot
            // actually came from -- root for a flat Luma layout, the
            // same date subfolder for Nexus3DS's optional layout.
            // Derived from topPath's own directory rather than
            // hardcoding SCREENSHOTS_DIR, so this needs no special
            // case for either layout.
            const char *lastSlash = strrchr(p->topPath, '/');
            if (lastSlash) {
                int dirLen = (int)(lastSlash - p->topPath);
                snprintf(outPath, sizeof(outPath), "%.*s/%s_cmb.bmp",
                         dirLen, p->topPath, p->timestamp);
            } else {
                // topPath is always an absolute path in practice, but
                // fall back to the known root rather than fail outright.
                snprintf(outPath, sizeof(outPath), "%s%s/%s_cmb.bmp",
                         SD_ROOT, SCREENSHOTS_DIR, p->timestamp);
            }
            ok = bmp_write(outPath, &merged, err, sizeof(err));
            free(merged.pixels);
        } else {
            snprintf(err, sizeof(err), "Out of memory");
        }
    }

    if (haveTop) bmp_free(&top);
    if (haveBot) bmp_free(&bot);

    if (ok) {
        int n = fs_scan_screenshot_pairs(s_pairs, MAX_PAIRS);
        s_pairCount = (n < 0) ? 0 : n;
        free_all_thumbnails();
        memset(s_thumbs, 0, sizeof(s_thumbs));
        rebuild_visible_list();

        // Land on the newly created merged file, not the original --
        // matching by path rather than a positional guess, since the
        // merged entry shares the original's exact timestamp and
        // qsort's tie-breaking between them isn't guaranteed.
        select_item_by_path(outPath);

        // Force a fresh preview load rather than relying on the
        // loaded-index check to notice the list changed -- a
        // coincidental re-sort could in principle leave that check
        // fooled into thinking the stale preview was still correct.
        free_preview_textures();
        s_previewLoadedPairIndex = -1;
        s_previewRequestedPairIndex = -1;

        s_lastSuccess = true;
        snprintf(s_lastMessage, sizeof(s_lastMessage), "Screenshot merged");
        snprintf(s_lastOutputPath, sizeof(s_lastOutputPath), "Added to your gallery");
        audio_play(SFX_COPY);
        s_thumbSfxMute = 45;
    } else {
        s_lastSuccess = false;
        snprintf(s_lastMessage, sizeof(s_lastMessage), "Merge failed:");
        snprintf(s_lastOutputPath, sizeof(s_lastOutputPath), "%s", err[0] ? err : "Unknown error.");
    }
    op_complete(APP_RESULT);
}

>>>>>>> Stashed changes
static void do_convert(void)
{
    int idx = (s_selected >= 0 && s_selected < s_visibleCount)
                ? s_visibleIndices[s_selected] : -1;
    const char *srcPath = item_top_path(idx);
    const char *srcStamp = item_timestamp(idx);
    // nds-bootstrap items have no real timestamp at all (item_timestamp
    // returns "" -- see TECHNICAL.md), which parsed as a zeroed EXIF
    // date and landed the export at 1/1/2000 in the Album. Same
    // resolution as extraction: stamp it with when Comet acted on it.
    char tarStampBuf[24];
    if (!srcStamp[0]) {
        current_time_stamp(tarStampBuf, sizeof(tarStampBuf));
        srcStamp = tarStampBuf;
    }
    if (!srcPath) { s_state = APP_BROWSE; return; }

    char err[128] = {0};
    RGBImage left, right;
    bool haveRight = false;

    // bmp_load_at, not bmp_load -- an nds-bootstrap item lives at a
    // byte offset inside screenshots.tar, so reading from offset 0
    // would hit the tar's own header block ("bad signature").
    if (!bmp_load_at(srcPath, item_data_offset(idx), &left, err, sizeof(err))) {
        s_lastSuccess = false;
        snprintf(s_lastMessage, sizeof(s_lastMessage), "Couldn't load screenshot:");
        snprintf(s_lastOutputPath, sizeof(s_lastOutputPath), "%s", err);
        op_complete(APP_RESULT);
        return;
    }
    const char *rightPath = item_right_path(idx);
    if (rightPath && bmp_load(rightPath, &right, err, sizeof(err))) haveRight = true;

    if (item_is_widescreen(idx)) stretch_to_widescreen(&left);

    char dir[280], base[32], outPath[320];
    bool ok = false;
    if (fs_next_dcim_slot(dir, sizeof(dir), base, sizeof(base))) {
        // 3D pairs become .MPO; 2D screenshots are written as a plain
        // .JPG, which the Camera app reads natively -- there's no
        // reason to refuse them just because there's no second eye.
        if (haveRight) {
            snprintf(outPath, sizeof(outPath), "%s/%s.MPO", dir, base);
            ok = mpo_write(&left, &right, srcStamp, outPath, err, sizeof(err));
        } else {
            snprintf(outPath, sizeof(outPath), "%s/%s.JPG", dir, base);
            ok = jpg_write(&left, srcStamp, outPath, err, sizeof(err));
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
    s_resultReturnState = APP_DETAIL;
    op_complete(APP_RESULT);
}

// ---- batch select / convert / delete --------------------------------------

static void begin_batch_convert(void)
{
    s_batchTotal = 0;
    int totalItems = item_count();
    for (int i = 0; i < totalItems; i++) {
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
    // item_count(), not s_pairCount -- in DS mode the batch runs over
    // whichever library the active tab points at.
    int total = item_count();
    bool isTarTab = s_dsMode && s_dsTab == DS_TAB_NDS_BOOTSTRAP;

    while (s_batchIndex < total && !s_batchSelected[s_batchIndex]) {
        s_batchIndex++;
    }

    if (s_batchIndex >= total) {
        s_lastSuccess = true;
        if (isTarTab) {
            snprintf(s_lastMessage, sizeof(s_lastMessage), "%d screenshot%s extracted to",
                     s_batchSucceeded, s_batchSucceeded == 1 ? "" : "s");
            snprintf(s_lastOutputPath, sizeof(s_lastOutputPath), "/3ds/Comet/ds_screenshots");
            ds_rescan_preserving_widescreen();
            refresh_ds_availability();
        } else {
            snprintf(s_lastMessage, sizeof(s_lastMessage), "%d screenshot%s copied to 3DS Album.",
                     s_batchSucceeded, s_batchSucceeded == 1 ? "" : "s");
            snprintf(s_lastOutputPath, sizeof(s_lastOutputPath), "Delete them?");
        }
        audio_play(SFX_COPY);
        s_thumbSfxMute = 45;
        s_confirmSelection = 0; // default to "Back" -- keep the originals unless asked
        op_complete(APP_BATCH_RESULT);
        return;
    }

    // On the nds-bootstrap tab, L means "extract to SD", not "copy to
    // the 3DS Album" -- different destination entirely.
    if (isTarTab) {
        char exErr[128];
        if (s_batchIndex < s_dsTarSlotCount &&
            ds_extract_slot(&s_dsTarSlots[s_batchIndex], exErr, sizeof(exErr))) {
            s_batchSucceeded++;
        }
        s_batchIndex++;
        return;
    }

    const char *srcPath  = item_top_path(s_batchIndex);
    const char *srcStamp = item_timestamp(s_batchIndex);
    char batchTarStampBuf[24];
    if (!srcStamp[0]) {
        current_time_stamp(batchTarStampBuf, sizeof(batchTarStampBuf));
        srcStamp = batchTarStampBuf;
    }
    const char *rightPath = item_right_path(s_batchIndex);
    RGBImage left, right;
    char err[128];
    bool haveLeft = false, haveRight = false, ok = false;

    if (srcPath && bmp_load_at(srcPath, item_data_offset(s_batchIndex), &left, err, sizeof(err))) {
        haveLeft = true;
        if (rightPath && bmp_load(rightPath, &right, err, sizeof(err))) haveRight = true;
        if (item_is_widescreen(s_batchIndex)) stretch_to_widescreen(&left);

        char dir[280], base[32], outPath[320];
        if (fs_next_dcim_slot(dir, sizeof(dir), base, sizeof(base))) {
            if (haveRight) {
                snprintf(outPath, sizeof(outPath), "%s/%s.MPO", dir, base);
                ok = mpo_write(&left, &right, srcStamp, outPath, err, sizeof(err));
            } else {
                snprintf(outPath, sizeof(outPath), "%s/%s.JPG", dir, base);
                ok = jpg_write(&left, srcStamp, outPath, err, sizeof(err));
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
    int total = item_count();
    for (int i = 0; i < total; i++) if (s_batchSelected[i]) count++;
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
    int total = item_count();
    if (s_dsMode && s_dsTab == DS_TAB_NDS_BOOTSTRAP) {
        // Deleting tar slots compacts the remaining ones down, which
        // renumbers every index *above* the one removed. Walking
        // highest-to-lowest means each index is still valid when its
        // turn comes -- forwards would delete the wrong slots after
        // the first one.
        for (int i = total - 1; i >= 0; i--) {
            if (s_batchSelected[i]) ds_delete_tar_slot(i);
        }
    } else {
        for (int i = 0; i < total; i++) {
            if (!s_batchSelected[i]) continue;
            if (s_dsMode) ds_delete(&s_dsShots[i]);
            else          fs_delete_pair(&s_pairs[i]);
        }
    }

    free_all_thumbnails();
    memset(s_thumbs, 0, sizeof(s_thumbs));

    if (s_dsMode) {
        if (s_dsTab == DS_TAB_NDS_BOOTSTRAP) {
            s_dsTarSlotCount = ds_list_tar_slots(s_dsTarSlots, MAX_DS_TAR_SLOTS);
            apply_tar_widescreen_prefs();
            refresh_ds_availability();
        } else {
            ds_rescan_preserving_widescreen();
        }
    } else {
        int n = fs_scan_screenshot_pairs(s_pairs, MAX_PAIRS);
        s_pairCount = (n < 0) ? 0 : n;
    }

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
static const float CELL_W_3DS = 73.0f, CELL_W_DS = 62.0f;
static const float CELL_H_3DS = 42.0f, CELL_H_DS = 46.0f;
static const float CELL_SPACING_3DS = 4.0f, CELL_SPACING_DS_X = 11.0f, CELL_SPACING_DS_Y = 3.0f;

// DS mode fits fewer, taller rows in the same content area.
#define CELL_W        (s_dsMode ? CELL_W_DS : CELL_W_3DS)
#define CELL_H        (s_dsMode ? CELL_H_DS : CELL_H_3DS)
#define CELL_SPACING_X (s_dsMode ? CELL_SPACING_DS_X : CELL_SPACING_3DS)
#define CELL_SPACING_Y (s_dsMode ? CELL_SPACING_DS_Y : CELL_SPACING_3DS)
#define GRID_ROWS_CUR  (s_dsMode ? 3 : GRID_ROWS)
static const float GRID_LEFT_3DS = 8.0f;   // 4*73 + 3*4  = 304 -> spans 8..312
static const float GRID_LEFT_DS  = 19.5f;  // 4*62 + 3*11 = 281 -> centred, spans 19.5..300.5
static const float GRID_TOP_3DS  = 31.0f;  // 4*42 + 3*4  = 180 -> ends 211, just under the footer rule
// DS: 3*49 + 2*8 = 163 in a 184px content area -> (184-163)/2 = 10.5 above and below.
// Tab bar sits between the header and the grid, only in DS mode.
// 2 rows no longer fit the 3-row layout used before tabs existed --
// the bar eats space the grid used to have -- so DS mode drops to 2
// rows while the tab bar is showing.
#define TAB_W 140.0f
#define TAB_H 24.0f
static const float TAB_Y = 32.0f;
static const float TAB_LEFT_X  = 8.0f;
static const float TAB_RIGHT_X = 312.0f - TAB_W;   // mirrors the left edge
static const float TAB_HAIRLINE_Y = TAB_Y + TAB_H + 4.0f;   // 2px higher than before
static const float GRID_TOP_DS   = TAB_HAIRLINE_Y + 6.0f;

#define GRID_LEFT (s_dsMode ? GRID_LEFT_DS : GRID_LEFT_3DS)
#define GRID_TOP  (s_dsMode ? GRID_TOP_DS  : GRID_TOP_3DS)
static const float CHECKBOX_SIZE = 14.0f;

// Which grid row is scrolled to the top, given the current cursor
// position -- shared by drawing and touch hit-testing so they can never
// disagree about where a cell actually is on screen.
static int compute_grid_start_row(void)
{
    int cursorRow = s_selected / GRID_COLS;
    int totalRows = (s_visibleCount + GRID_COLS - 1) / GRID_COLS;
    int visRows = GRID_ROWS_CUR;
    int startRow = cursorRow >= visRows ? cursorRow - visRows + 1 : 0;
    int maxStartRow = totalRows > visRows ? totalRows - visRows : 0;
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
    int visRows = GRID_ROWS_CUR;
    for (int r = 0; r < visRows; r++) {
        for (int c = 0; c < GRID_COLS; c++) {
            float x = GRID_LEFT + c * (CELL_W + CELL_SPACING_X);
            float y = GRID_TOP + r * (CELL_H + CELL_SPACING_Y);
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

    if (s_dsMode) {
        // Native 1:1, centred. Upscaling a 256x192 DS capture to fill
        // the 3DS panel would only add blur, so it's letterboxed.
        //
        // Widescreen mode stretches horizontally to 16:10 (192 * 16/10
        // = 307px) for games running a widescreen patch, which squeeze
        // 16:10 content into the same 256x192 buffer. Point sampling
        // keeps that stretch crisp instead of smeared.
        float dh = et->subtex.height;
        bool wide = current_ds_widescreen();
        float dw = wide ? (dh * 16.0f / 10.0f) : et->subtex.width;
        float sx = dw / et->subtex.width;

        C3D_TexSetFilter(&((EyeTexture *)et)->tex,
                         wide ? GPU_NEAREST : GPU_LINEAR, GPU_NEAREST);

        C2D_DrawImageAt(et->image, (400.0f - dw) / 2.0f, (240.0f - dh) / 2.0f,
                        0.0f, NULL, sx, 1.0f);
        return;
    }

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

// Deliberately not draw_eye_image: that function's behaviour branches
// on s_dsMode (widescreen stretch, native-size letterbox for DS
// content), which doesn't apply here regardless of whether the user
// triggered this from the 3DS or DS grid. These are always plain
// 400x240 stereo photos, drawn full-screen like any normal 3D capture.
static void draw_easter_egg_eye(EyeTexture *et)
{
    if (!et->valid) return;
    C2D_DrawImageAt(et->image, 0, 0, 0.0f, NULL,
                     400.0f / et->subtex.width, 240.0f / et->subtex.height);
}

static void draw_top_screen(void)
{
    switch (s_state) {
    case APP_EASTER_EGG:
        // Always stereo -- both eyes are baked in, nothing to load.
        gfxSet3D(true);
        C2D_TargetClear(s_top, COLOR_BG);
        C2D_SceneBegin(s_top);
        draw_easter_egg_eye(&s_easterEggTex[EASTER_EGG_DOGLEFT]);
        C2D_TargetClear(s_topRight, COLOR_BG);
        C2D_SceneBegin(s_topRight);
        draw_easter_egg_eye(&s_easterEggTex[EASTER_EGG_DOGRIGHT]);
        return;
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

    // Also in APP_DETAIL, since Left/Right now moves between
    // screenshots from that screen and the top screen has to follow.
    if (s_state == APP_BROWSE || s_state == APP_DETAIL) {
        update_live_preview();
    }

    bool stereo = s_previewRight.valid;
    gfxSet3D(stereo);

    C2D_TargetClear(s_top, COLOR_BG);
    C2D_SceneBegin(s_top);
    draw_eye_image(&s_previewLeft);

    if ((s_state == APP_BROWSE || s_state == APP_DETAIL) &&
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

// Where the L+R mode-toggle hint was last drawn, so it can be tapped.
// Captured at draw time rather than hardcoded, and cleared on any
// screen that doesn't show it.
static float s_modeToggleX = 0.0f, s_modeToggleW = 0.0f;

// Set in ui_frame (where touch coordinates are actually accessible),
// read in draw_bottom_screen's peekingBottomCapture check.
static bool s_touchHoldingShowBottom = false;

// Triple-tap-the-header-icon-in-1-second easter egg trigger. A ring
// buffer of the last 3 tap frame-numbers: if all 3 fall within a
// 60-frame (1 second at 60fps) window, it fires. Sentinel value keeps
// the check correctly failing until 3 real taps have actually
// happened, rather than comparing against stale zeros.
#define ICON_TAP_WINDOW_FRAMES 60
static int  s_iconTapFrames[3] = { -100000, -100000, -100000 };
static int  s_iconTapNext = 0;
static long s_frameCounter = 0;

// Registers a tap and returns true exactly on the one that completes
// a triple-tap within the window. Shared by both directions (entering
// and leaving the easter egg) rather than duplicated, since the
// detection logic is identical either way.
static bool register_icon_tap_and_check_triple(void)
{
    s_iconTapFrames[s_iconTapNext] = (int)s_frameCounter;
    s_iconTapNext = (s_iconTapNext + 1) % 3;
    int oldest = s_iconTapFrames[0];
    for (int i = 1; i < 3; i++) if (s_iconTapFrames[i] < oldest) oldest = s_iconTapFrames[i];
    if ((int)s_frameCounter - oldest <= ICON_TAP_WINDOW_FRAMES) {
        s_iconTapFrames[0] = s_iconTapFrames[1] = s_iconTapFrames[2] = -100000;
        return true;
    }
    return false;
}
static bool  s_modeToggleVisible = false;

// Header layout: the screen's own name sits left, the Comet mark is
// centred, and the right slot carries either a context label or the
// L+R mode-toggle hint. (The old "[icon] Comet" wordmark is gone --
// the icon alone identifies the app, freeing the left slot for
// something useful on every screen.)
static void draw_header_full(const char *leftLabel, const char *rightLabel,
                              bool showModeToggle, const char *modeLabel)
{
    s_modeToggleVisible = false;
    C2D_DrawRectSolid(0, 0, 0, 320, HEADER_H, COLOR_BG);

    float iw = icon_width(ICON_COMET);
    draw_icon(ICON_COMET, (320.0f - iw) / 2.0f,
              (HEADER_H - icon_height(ICON_COMET)) / 2.0f);

    if (leftLabel) draw_text_vcenter_semibold(8, 0, HEADER_H, TEXT_9, COLOR_TEXT, leftLabel);

    if (showModeToggle && modeLabel) {
        // [L] + [R] <label>, right-aligned.
        float lw = icon_width(ICON_BTN_L), rw = icon_width(ICON_BTN_R);
        float plusW  = measure_text(TEXT_9, "+");
        float labelW = measure_text(TEXT_9, modeLabel);
        const float g = 3.0f;
        float total = lw + g + plusW + g + rw + 5.0f + labelW;
        float x = 312.0f - total;
        float iconY = (HEADER_H - icon_height(ICON_BTN_L)) / 2.0f;

        // Remember where this landed so a tap on it can toggle modes
        // too -- derived from the same numbers used to draw, so the
        // touch target always matches what's actually visible.
        s_modeToggleX = x - 4.0f;
        s_modeToggleW = (312.0f - s_modeToggleX) + 4.0f;
        s_modeToggleVisible = true;

        draw_icon(ICON_BTN_L, x, iconY);                       x += lw + g;
        draw_text_vcenter(x, 0, HEADER_H, TEXT_9, COLOR_TEXT, "+"); x += plusW + g;
        draw_icon(ICON_BTN_R, x, iconY);                       x += rw + 5.0f;
        draw_text_vcenter(x, 0, HEADER_H, TEXT_9, COLOR_TEXT, modeLabel);
    } else if (rightLabel) {
        draw_text_right_vcenter(312, 0, HEADER_H, TEXT_9, COLOR_TEXT, rightLabel);
    }

    draw_frame_rule(HEADER_H);
}

static void draw_header(const char *leftLabel, const char *rightLabel)
{
    draw_header_full(leftLabel, rightLabel, false, NULL);
}

// Formats the currently-configured Year/Month/Day (regardless of
// whether By Date is the *active* filter right now) with progressively
// more detail: just the year, or "Month Year", or "Month Day, Year".
static void format_date_ymd(char *buf, size_t bufSize)
{
    if (s_filterYear <= 0) {
        snprintf(buf, bufSize, "All");
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
    int total = item_count();
    for (int i = 0; i < total; i++) {
        int y, mo, d;
        if (!parse_timestamp_ymd(item_timestamp(i), &y, &mo, &d)) continue;
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
        snprintf(buf, sizeof(buf), s_dsMode ? "DS Screenshots (%d)" : "All Screenshots (%d)", s_visibleCount);
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

// Colours per spec: inactive fill matches page BG, active fill/stroke
// are new one-off values not shared with anything else in the app
// (the existing yellow selection colour would visually compete with
// the tab bar rather than complement it).
static u32 tab_inactive_fill(void)  { return COLOR_BG; }
static u32 tab_inactive_stroke(void){ return C2D_Color32(0x6D, 0x6D, 0x6D, 0xff); }
static u32 tab_active_fill(void)    { return C2D_Color32(0x1E, 0x1C, 0x16, 0xff); }
static u32 tab_active_stroke(void)  { return C2D_Color32(0xDD, 0x98, 0x20, 0xff); }

static void draw_ds_tab(float x, const char *label, int count, bool active, bool enabled, IconId glyph, bool glyphOnLeft)
{
    u32 fill   = active ? tab_active_fill()   : tab_inactive_fill();
    u32 stroke = active ? tab_active_stroke() : tab_inactive_stroke();
    u32 text   = active ? COLOR_TEXT : tab_inactive_stroke(); // spec: inactive text matches its stroke colour
    if (!enabled) text = C2D_Color32(0x6D, 0x6D, 0x6D, 0x80); // half-opacity when a tab has nothing in it

    draw_rounded_rect(x, TAB_Y, TAB_W, TAB_H, 2, stroke);
    draw_rounded_rect(x + 1, TAB_Y + 1, TAB_W - 2, TAB_H - 2, 2, fill);

    // Shoulder glyph sits against the tab's outer edge -- L on the
    // left tab, R on the right -- mirroring which button selects it.
    const float pad = 6.0f;
    float gw = icon_width(glyph);
    float gx = glyphOnLeft ? (x + pad) : (x + TAB_W - pad - gw);
    draw_icon_tinted(glyph, gx, TAB_Y + (TAB_H - icon_height(glyph)) / 2.0f, !enabled);

    // Label centres within whatever's left after the glyph.
    float textX = glyphOnLeft ? (x + pad + gw) : x;
    float textW = TAB_W - pad - gw;

    char buf[32];
    snprintf(buf, sizeof(buf), "%s (%d)", label, count);
    draw_centered_text(textX, TAB_Y, textW, TAB_H, TEXT_10, text, buf);
}

// Tab bar: nds-bootstrap on the left, SD Card on the right. Visible
// throughout DS mode, but only switchable (L/R or touch) from the
// unfiltered browse screen -- batch select and detail views show it
// as context, not a live control.
static void draw_ds_tabs(void)
{
    draw_ds_tab(TAB_LEFT_X,  "nds-bootstrap", s_dsTarSlotCount,
                s_dsTab == DS_TAB_NDS_BOOTSTRAP, s_dsTarSlotCount > 0, ICON_TAB_L, true);
    draw_ds_tab(TAB_RIGHT_X, "SD Card", s_dsCount,
                s_dsTab == DS_TAB_SD_CARD, s_dsCount > 0, ICON_TAB_R, false);
    // Hairline (row-separator style, #797979 inset), not the brighter
    // full-width frame rule used around the header and footer.
    draw_separator(TAB_HAIRLINE_Y);
}

static void draw_grid(void)
{
    // No subtitle row here -- the grid is tight enough (4 rows of 40px
    // cells fits the available height almost exactly) that there's no
    // room to spare for a second header line.
    if (s_batchMode) {
        draw_header("Batch Select", NULL);
    } else {
        // The mode toggle only advertises itself when there's actually
        // something on the other side, and only on the album screen.
        // The SD Card tab supports filtering, so it shows the same
        // "<filter> (count)" label 3DS mode uses. The nds-bootstrap tab
        // has no filter, so it keeps the plain mode name.
        const char *leftLabel;
        if (!s_dsMode)                            leftLabel = filter_mode_label();
        else if (s_dsTab == DS_TAB_SD_CARD)       leftLabel = filter_mode_label();
        else                                      leftLabel = "Nintendo DS Mode";

        draw_header_full(leftLabel, NULL,
                         s_dsAvailable, s_dsMode ? "3DS Mode" : "DS Mode");
    }

    if (s_dsMode) draw_ds_tabs();

    if (s_visibleCount == 0) {
        if (!state_has_popup(s_state)) draw_text(18, GRID_TOP + 6, TEXT_12, COLOR_DIM,
                  item_count() == 0
                      ? (s_dsMode ? "Nothing here yet."
                                  : "None yet -- take one from Rosalina.")
                      : "No screenshots match this filter.");
        return;
    }

    int startRow = compute_grid_start_row();
    int totalRows = (s_visibleCount + GRID_COLS - 1) / GRID_COLS;
    int visRows = GRID_ROWS_CUR;

    bool builtOneThisFrame = false;
    for (int r = 0; r < visRows; r++) {
        int gridRow = startRow + r;
        for (int c = 0; c < GRID_COLS; c++) {
            int visPos = gridRow * GRID_COLS + c;
            if (visPos >= s_visibleCount) continue;
            int idx = s_visibleIndices[visPos];

            float x = GRID_LEFT + c * (CELL_W + CELL_SPACING_X);
            float y = GRID_TOP + r * (CELL_H + CELL_SPACING_Y);
            bool sel = (visPos == s_selected);

            if (!builtOneThisFrame && !s_thumbs[idx].attempted) {
                build_thumbnail(item_top_path(idx), item_data_offset(idx), &s_thumbs[idx]);
                builtOneThisFrame = true;
                if (s_thumbs[idx].tex.valid && s_thumbSfxMute == 0) audio_play(SFX_THUMB_LOAD);
            }

            if (sel) {
                draw_rounded_rect(x - 2, y - 2, CELL_W + 4, CELL_H + 4, 2, COLOR_YELLOW);
            }

            draw_thumbnail(&s_thumbs[idx], x, y, CELL_W, CELL_H);

            // 3D/2D badge, top-left corner of the cell, over the thumbnail.
            // 3DS mode badges the stereo/flat distinction; DS mode
            // badges which library the item came from instead.
            if (!s_dsMode) {
                draw_icon(item_has_3d(idx) ? ICON_BADGE_3D : ICON_BADGE_2D, x + 2, y + 2);
            } else {
                draw_icon(s_dsTab == DS_TAB_NDS_BOOTSTRAP ? ICON_BADGE_DS_TAR
                                                          : ICON_BADGE_DS_EXTRACTED,
                          x + 2, y + 2);
            }

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

    if (totalRows > visRows) {
        float trackH = GRID_ROWS_CUR * (CELL_H + CELL_SPACING_Y) - CELL_SPACING_Y;
        float maxStartRow = (float)(totalRows - visRows);
        float thumbH = trackH * visRows / totalRows;
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

// The nds-bootstrap tab gets a third option (Extract Screenshot) ahead
// of the usual two -- everywhere else, item 0/1 keep their existing
// meaning (Copy / Delete).
static int detail_menu_item_count(void)
{
<<<<<<< Updated upstream
    return (s_dsMode && s_dsTab == DS_TAB_NDS_BOOTSTRAP) ? 3 : 2;
=======
    if (s_dsMode) return (s_dsTab == DS_TAB_NDS_BOOTSTRAP) ? 3 : 2;
    // Merge only makes sense when there's a real bottom capture to
    // merge with -- this also rules out offering it on an entry
    // that's already a combined image (a _cmb.bmp-sourced entry has
    // no separate botPath, same as a shot that just never captured
    // a bottom frame), which would otherwise double-merge garbage.
    //
    // s_previewLoadedPairIndex, not s_selected-derived -- see
    // draw_detail_menu for why.
    int idx = s_previewLoadedPairIndex;
    bool canMerge = idx >= 0 && idx < s_pairCount && s_pairs[idx].botPath[0];
    return canMerge ? 3 : 2; // Copy / [Merge] / Delete
>>>>>>> Stashed changes
}

static void draw_detail_menu(void)
{
    // Deliberately s_previewLoadedPairIndex, not s_visibleIndices[s_selected]:
    // the latter changes the instant Left/Right is pressed, before the
    // new preview has actually loaded. Using the loaded index means
    // this panel keeps describing the *previous* screenshot until the
    // new one's preview genuinely finishes, instead of jumping ahead
    // of what the top screen is showing.
    int selIdx = s_previewLoadedPairIndex;
    char dateBuf[32] = "";
    // Tar-tab items have no meaningful per-shot date (item_timestamp
    // returns "" there), so the header's right slot is just left blank
    // rather than showing a formatted empty string.
    if (selIdx >= 0 && item_timestamp(selIdx)[0]) {
        format_display_date(item_timestamp(selIdx), dateBuf, sizeof(dateBuf));
    }
    draw_header("More Info", dateBuf);

    int count = detail_menu_item_count();
    const char **labels = (count == 3)
        ? (const char *[]){"Extract Screenshot", "Copy to 3DS Album", "Delete"}
        : (const char *[]){"Copy to 3DS Album", "Delete"};

    for (int i = 0; i < count; i++) {
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

    // Source path, bottom-anchored above the footer rule. Worth
    // showing now that screenshots come from several different places
    // (Luma's folder, a Nexus3DS date subfolder, inside
    // screenshots.tar, or Comet's own extracted folder) rather than
    // always the one location.
    {
        char pathBuf[320];
        if (s_dsMode && s_dsTab == DS_TAB_NDS_BOOTSTRAP) {
            // Tar entries aren't real files on disk -- show the
            // archive plus which slot inside it, which is the closest
            // meaningful "where did this come from".
            int slot = (selIdx >= 0 && selIdx < s_dsTarSlotCount)
                         ? s_dsTarSlots[selIdx].slot : 0;
            snprintf(pathBuf, sizeof(pathBuf), "%s/screenshot%02d.bmp", DS_TAR_PATH, slot);
        } else {
            const char *full = item_top_path(selIdx);
            if (!full) full = "";
            // Strip the "sdmc:" prefix -- it's the same for everything
            // and just eats horizontal space.
            if (strncmp(full, SD_ROOT, strlen(SD_ROOT)) == 0) full += strlen(SD_ROOT);
            snprintf(pathBuf, sizeof(pathBuf), "%s", full);
        }

        const float pathLeft = 8.0f, pathRight = 312.0f;
        const float maxW = pathRight - pathLeft;
        const float lineH = 11.0f;
        const float bottomGap = 10.0f; // clear of the footer rule

        if (measure_text_font(s_fontSemiBold, TEXT_9, pathBuf) <= maxW) {
            draw_text_semibold(pathLeft, FOOTER_Y - bottomGap - lineH, TEXT_9, COLOR_DIM, pathBuf);
        } else {
            // Too long for one line: break after the last folder
            // separator, keeping the '/' on the first line so it still
            // reads as a path. Nexus3DS's date-subfolder layout plus a
            // title-ID filename is what actually hits this.
            char head[320], tail[320];
            const char *lastSlash = strrchr(pathBuf, '/');
            if (lastSlash) {
                int headLen = (int)(lastSlash - pathBuf) + 1; // include the '/'
                snprintf(head, sizeof(head), "%.*s", headLen, pathBuf);
                snprintf(tail, sizeof(tail), "%s", lastSlash + 1);
            } else {
                snprintf(head, sizeof(head), "%s", pathBuf);
                tail[0] = '\0';
            }
            draw_text_semibold(pathLeft, FOOTER_Y - bottomGap - lineH * 2, TEXT_9, COLOR_DIM, head);
            if (tail[0]) {
                draw_text_semibold(pathLeft, FOOTER_Y - bottomGap - lineH, TEXT_9, COLOR_DIM, tail);
            }
        }
    }
}

// Hit-tests a touch point against the detail-menu items.
static bool detail_menu_hit_test(int touchX, int touchY, int *outIndex)
{
    int count = detail_menu_item_count();
    for (int i = 0; i < count; i++) {
        if (point_in_rect(touchX, touchY, DETAIL_MENU_X, detail_menu_item_y(i), DETAIL_MENU_W, DETAIL_MENU_H)) {
            *outIndex = i;
            return true;
        }
    }
    return false;
}

static void draw_bottom_capture(void)
{
    ScreenshotPair *p = s_dsMode ? NULL : current_pair();
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
    draw_header("Filters", filter_mode_label());

    int countAll = s_pairCount, count3D = 0, count2D = 0;
    for (int i = 0; i < s_pairCount; i++) { if (s_pairs[i].has3D) count3D++; else count2D++; }
    (void)count3D; (void)count2D;

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

// Must match what draw_by_date_menu() actually renders, or the cursor
// can land on a hidden row: Year alone when Year = "All", plus Month,
// plus Day once a specific month is picked.
static int by_date_row_count(void)
{
    if (s_filterYear <= 0) return 1;      // Year only
    return s_filterMonth != 0 ? 3 : 2;    // + Month, + Day
}

static void draw_by_date_menu(void)
{
    draw_header(s_dsMode ? "Filters > By Extraction Date" : "Filters > By Date", filter_mode_label());

    char yearBuf[8], monthBuf[16], dayBuf[8];
    if (s_filterYear > 0) snprintf(yearBuf, sizeof(yearBuf), "%d", s_filterYear);
    else snprintf(yearBuf, sizeof(yearBuf), "All");
    snprintf(monthBuf, sizeof(monthBuf), "%s", s_filterMonth == 0 ? "All" : MONTH_NAMES[s_filterMonth - 1]);

    draw_filter_row(FILTER_LIST_TOP + 0 * FILTER_ROW_HEIGHT, "Year", yearBuf, false, false, s_filterCursor == 0);

    // With Year = "All" there's no year to narrow within, so Month and
    // Day aren't shown at all.
    if (s_filterYear > 0)
        draw_filter_row(FILTER_LIST_TOP + 1 * FILTER_ROW_HEIGHT, "Month", monthBuf, false, false, s_filterCursor == 1);

    // Day only makes sense (and is only shown) once a specific month
    // is chosen -- "any day within all of 2026" isn't a meaningful row.
    if (s_filterYear > 0 && s_filterMonth != 0) {
        snprintf(dayBuf, sizeof(dayBuf), "%s", s_filterDay == 0 ? "All" : "");
        if (s_filterDay != 0) snprintf(dayBuf, sizeof(dayBuf), "%d", s_filterDay);
        draw_filter_row(FILTER_LIST_TOP + 2 * FILTER_ROW_HEIGHT, "Day", dayBuf, false, false, s_filterCursor == 2);
    }
}

static void draw_month_picker(void)
{
    draw_header("Select Month", filter_mode_label());
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
    draw_header("Select Year", filter_mode_label());
    if (s_availableYearCount == 0) {
        draw_text(18, FILTER_LIST_TOP + 6, TEXT_12, COLOR_DIM, "No dated screenshots found.");
        return;
    }
    int total = s_availableYearCount + 1; // "All" + real years
    int start = compute_list_start(s_filterCursor, total);
    for (int i = 0; i < FILTER_VISIBLE_ROWS && start + i < total; i++) {
        int idx = start + i;
        char buf[8];
        bool active;
        if (idx == 0) {
            snprintf(buf, sizeof(buf), "All");
            active = (s_filterYear == 0);
        } else {
            snprintf(buf, sizeof(buf), "%d", s_availableYears[idx - 1]);
            active = (s_availableYears[idx - 1] == s_filterYear);
        }
        draw_filter_row(FILTER_LIST_TOP + i * FILTER_ROW_HEIGHT, buf, NULL,
                         true, active, idx == s_filterCursor);
    }
    if (total > FILTER_VISIBLE_ROWS) draw_filter_scrollbar(start, total);
}

static void draw_day_picker(void)
{
    draw_header("Select Day", filter_mode_label());
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


// Deliberately plain: a static thank-you screen, not a data display,
// so nothing here needs to be mode-aware or touch anything about
// s_dsMode/s_pairs/etc at all.
static void draw_easter_egg_screen(void)
{
    char dateBuf[32] = "";
    // The dog photo's own capture date, not "now" -- this screen is
    // meant to preserve that specific day, not describe the moment
    // someone happens to find the egg.
    format_display_date("2026-08-29_14-10-21.136", dateBuf, sizeof(dateBuf));
    draw_header(NULL, dateBuf);

    typedef struct { const char *text; bool semibold; } EggLine;
    static const EggLine lines[] = {
        { "Comet: Observatory of Screenshots", true },
        { "Concept by Pedro Verri", false },
        { "", false },
        { "Thank you so much for using", false },
        { "my app, I hope you enjoy it!", false },
        { "", false },
        { "That's my dog, Pudim.", false },
        { "She is the best dog ever!", false },
    };
    const int lineCount = sizeof(lines) / sizeof(lines[0]);
    const float textX = 16.0f, textTop = 40.0f, lineH = 18.0f;

    for (int i = 0; i < lineCount; i++) {
        if (!lines[i].text[0]) continue; // blank line: just leaves the gap
        float y = textTop + i * lineH;
        if (lines[i].semibold) draw_text_semibold(textX, y, TEXT_12, COLOR_TEXT, lines[i].text);
        else                   draw_text(textX, y, TEXT_12, COLOR_TEXT, lines[i].text);
    }

    // Avatar, vertically centred against the text block, right-aligned
    // with the same margin the text block's left margin mirrors.
    EyeTexture *avatar = &s_easterEggTex[EASTER_EGG_AVATAR];
    if (avatar->valid) {
        float textBlockH = lineCount * lineH;
        float ax = 320.0f - 16.0f - avatar->subtex.width - 12.0f;
        float ay = textTop + (textBlockH - avatar->subtex.height) / 2.0f + 16.0f;
        C2D_DrawImageAt(avatar->image, ax, ay, 0.0f, NULL, 1.0f, 1.0f);
    }
}

static void draw_bottom_screen(void)
{
    C2D_TargetClear(s_bot, COLOR_BG);
    C2D_SceneBegin(s_bot);

    // The bottom-capture peek (holding R in the detail view) fills the
    // whole 320x240 screen per the mockup, so it skips the footer bar.
    // 3DS-only: DS captures never request a bottom-capture load (see
    // request_bottom_capture's !s_dsMode guard), so this used to leave
    // an unresolvable spinner on screen if R was held in DS mode --
    // removed rather than kept as a joke, since a spinner with no
    // outcome reads as a hang, not a wink.
    bool peekingBottomCapture = !s_dsMode && (s_state == APP_DETAIL) &&
                                ((hidKeysHeld() & KEY_R) || s_touchHoldingShowBottom);
    if (!peekingBottomCapture) {
        C2D_DrawRectSolid(0, FOOTER_Y, 0, 320, FOOTER_H, COLOR_BG);
        draw_frame_rule(FOOTER_Y);
    }

    switch (s_state) {
    case APP_EASTER_EGG: {
        draw_easter_egg_screen();
        const FooterHint eggHints[] = { { ICON_BTN_B, "Back", false } };
        draw_footer_hints(eggHints, 1);
        break;
    }
    case APP_BROWSE:
        draw_grid();
        if (s_batchMode) {
            bool anySelected = false;
            int total = item_count();
            for (int i = 0; i < total; i++) if (s_batchSelected[i]) { anySelected = true; break; }
            bool isTarTab = s_dsMode && s_dsTab == DS_TAB_NDS_BOOTSTRAP;
            const FooterHint batchHints[] = {
                { ICON_BTN_A, "OK", false }, { ICON_BTN_B, "Back", false },
                { ICON_BTN_X, "Delete", !anySelected },
                { ICON_BTN_L, isTarTab ? "Extract" : "Copy to Album", !anySelected },
            };
            draw_footer_hints(batchHints, 4);
        } else if (s_dsMode && s_dsTab == DS_TAB_NDS_BOOTSTRAP) {
            // No date filter on this tab (nothing meaningful to filter
            // by -- see item_timestamp), so Batch Select takes Filter's
            // old spot, and Extract All fills the slot that leaves free.
            const FooterHint tarHints[] = {
                { ICON_BTN_A, "More", false },
                { ICON_BTN_SELECT, "Batch Select", false },
                { ICON_BTN_START, "Extract All", s_dsTarSlotCount == 0 },
            };
            draw_footer_hints(tarHints, 3);
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
            if (s_dsMode) {
                const FooterHint dsDetailHints[] = {
                    { ICON_BTN_A, "OK", false }, { ICON_BTN_B, "Back", false },
                    { ICON_BTN_L, current_ds_widescreen() ? "Normal" : "Widescreen", false },
                };
                draw_footer_hints(dsDetailHints, 3);
            } else {
                // Merged screenshots have no separate bottom capture
                // (the whole image already contains both screens), so
                // there's nothing for "Show Bottom" to do -- offering
                // it there would just be a dead-end option.
                bool isMerged = s_previewLoadedPairIndex >= 0 &&
                                s_previewLoadedPairIndex < s_pairCount &&
                                s_pairs[s_previewLoadedPairIndex].isCombined;
                if (isMerged) {
                    const FooterHint detailHints[] = {
                        { ICON_BTN_A, "OK", false }, { ICON_BTN_B, "Back", false },
                    };
                    draw_footer_hints(detailHints, 2);
                } else {
                    const FooterHint detailHints[] = {
                        { ICON_BTN_A, "OK", false }, { ICON_BTN_B, "Back", false },
                        { ICON_BTN_R, "Show Bottom", false },
                    };
                    draw_footer_hints(detailHints, 3);
                }
            }
        }
        break;

    case APP_DETAIL_DELETE_CONFIRM:
        draw_detail_menu();
        draw_confirm_popup("The file will be deleted", NULL, "Cancel", "Delete", s_confirmSelection);
        break;

    case APP_DETAIL_DELETING:
        draw_detail_menu();
        draw_popup("Deleting...", "Don't remove SD card!", true, false);
        break;

    case APP_CONVERTING:
        draw_grid();
        draw_popup("Copying...", "Don't remove SD card!", true, false);
        break;

    case APP_RESULT:
        draw_grid();
        draw_popup(s_lastMessage, s_lastOutputPath, false, true);
        static const FooterHint resultHints[] = { { ICON_BTN_A, "Continue" } };
        draw_footer_hints(resultHints, 1);
        break;

    case APP_BATCH_CONVERTING:
        draw_grid();
        draw_popup("Copying...", "Don't remove SD card!", true, false);
        break;

    case APP_BATCH_RESULT:
        draw_grid();
        draw_confirm_popup(s_lastMessage, s_lastOutputPath, "Back", "Delete", s_confirmSelection);
        break;

    case APP_BATCH_DELETE_CONFIRM:
        draw_grid();
        draw_confirm_popup(s_lastMessage, NULL, "Cancel", "Delete", s_confirmSelection);
        break;

    case APP_DUPLICATE_CONFIRM:
        draw_detail_menu();
        draw_confirm_popup("Duplicate this screenshot?", NULL,
                            "Cancel", "Duplicate", s_confirmSelection);
        break;

    case APP_DUPLICATING:
        draw_detail_menu();
        draw_popup("Duplicating...", "Don't remove SD card!", true, false);
        break;

    case APP_QUIT_CONFIRM:
        draw_grid();
        draw_confirm_popup("Quit Comet?", NULL, "Cancel", "Yes", s_confirmSelection);
        break;

    case APP_MODE_SWITCHING:
        draw_grid();
        draw_popup("Loading...", NULL, true, false);
        break;

    case APP_DS_INTRO_POPUP:
        draw_grid();
        draw_popup3("nds-bootstrap keeps a max of",
                     "50 screenshots at a time. Here you",
                     "can view, manage and extract them.", false, true);
        break;

    case APP_DS_ONE_EXTRACT_PROMPT:
        draw_detail_menu();
        draw_confirm_popup("Extract this screenshot?", NULL,
                            "Cancel", "Extract", s_confirmSelection);
        break;

    case APP_DS_ONE_EXTRACTING:
        draw_detail_menu();
        draw_popup("Extracting...", "Don't remove SD card!", true, false);
        break;

    case APP_DS_ONE_DELETE_PROMPT:
        draw_detail_menu();
        draw_confirm_popup3("Screenshot extracted to", "/3ds/Comet/ds_screenshots",
                             "Delete from nds-bootstrap?",
                             "Cancel", "Delete", s_confirmSelection);
        break;

    case APP_DS_ONE_DELETING:
        draw_detail_menu();
        draw_popup("Deleting...", "Don't remove SD card!", true, false);
        break;

    case APP_DS_EXTRACT_PROMPT:
        draw_grid();
        draw_confirm_popup("Extract all screenshots?", NULL,
                            "Cancel", "Extract", s_confirmSelection);
        break;

    case APP_DS_EXTRACTING:
        draw_grid();
        draw_popup("Extracting...", "Don't remove SD card!", true, false);
        break;

    case APP_DS_CLEAR_PROMPT:
        draw_grid();
        draw_confirm_popup3("Screenshots extracted to", "/3ds/Comet/ds_screenshots",
                             "Delete from nds-bootstrap?",
                             "Cancel", "Delete", s_confirmSelection);
        break;

    case APP_DS_CLEARING:
        draw_grid();
        draw_popup("Clearing...", "Don't remove SD card!", true, false);
        break;

    case APP_BATCH_DELETING:
        draw_grid();
        draw_popup("Deleting...", "Don't remove SD card!", true, false);
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
    s_frameCounter++;

    touchPosition touch;
    hidTouchRead(&touch);
    bool tapped = (kDown & KEY_TOUCH) != 0;

    // Start normally exits the app -- except on the nds-bootstrap tab's
    // plain browse screen, where it's repurposed as Extract All (see
    // the per-state handling below). Checked against last frame's
    // state, same as everything else here that reads s_state before
    // this frame's own transitions run.
    bool startRepurposed = s_dsMode && s_dsTab == DS_TAB_NDS_BOOTSTRAP &&
                            s_state == APP_BROWSE && !s_batchMode;
    if ((kDown & KEY_START) && !startRepurposed && s_state != APP_QUIT_CONFIRM) {
        s_confirmSelection = 0; // default to Cancel -- quitting is destructive of nothing, but still deliberate
        s_state = APP_QUIT_CONFIRM;
    }

    // Face and shoulder buttons click, but only if the press actually
    // did something -- see the snapshot/compare after the input switch
    // below for the other half of this.
    bool nonDirectionalPressed = (kDown & (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_L | KEY_R | KEY_SELECT)) != 0;

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

    // Whether an unconsumed tap is heading into the switch below, to
    // be handled by state-specific code (a grid cell, a DS tab, a
    // filter row, etc.) -- as opposed to one already consumed and
    // sounded by the footer-hit check above. The automatic sound
    // check further down only recognises A/B/X/Y/L/R/Select
    // (KEY_TOUCH was never in that set), so without this, every one
    // of these pure-touch interactions was silently exempt from ever
    // playing a sound, even though they clearly change state.
    bool tappedGoingIntoSwitch = tapped;

    // Touch-hold equivalent of physically holding R, specifically for
    // "Show Bottom" -- the footer-hit consumption above is edge-
    // triggered (kDown), firing once on the initial tap-down, so a
    // sustained hold was never actually detected the way
    // hidKeysHeld() detects a sustained physical R hold. That's why
    // the tap sound played correctly (it's consumed above) but the
    // peek itself never engaged. R has exactly one meaning anywhere
    // in the app (Show Bottom), so matching any KEY_R-mapped hitbox
    // is unambiguous.
    s_touchHoldingShowBottom = false;
    if (hidKeysHeld() & KEY_TOUCH) {
        for (int i = 0; i < s_footerHitCount; i++) {
            FooterHitbox *hb = &s_footerHits[i];
            if (hb->key == KEY_R && !hb->disabled &&
                point_in_rect(touch.px, touch.py, hb->x, hb->y, hb->w, hb->h)) {
                s_touchHoldingShowBottom = true;
                break;
            }
        }
    }

    int  batchSelSnapshot = 0;
    for (int i = 0; i < MAX_PAIRS; i++) if (s_batchSelected[i]) batchSelSnapshot += i + 1;
    int  selectedBefore   = s_selected;
    bool batchModeBefore  = s_batchMode;
    FilterMode filterModeBefore = s_filterMode;
    int  filterYearBefore  = s_filterYear;
    int  filterMonthBefore = s_filterMonth;
    int  filterDayBefore   = s_filterDay;
    int  filterCursorBefore = s_filterCursor;
    int  detailSelBefore   = s_detailMenuSelection;
    int  confirmSelBefore  = s_confirmSelection;
    DSTab dsTabBefore      = s_dsTab;

    switch (s_state) {
    case APP_EASTER_EGG: {
        // Triple-tap the icon again also exits, matching how it
        // entered -- but with the regular button sound instead of the
        // jingle, so leaving doesn't feel like re-triggering the joke.
        if (tapped && point_in_rect(touch.px, touch.py, 140, 0, 40, HEADER_H)) {
            if (register_icon_tap_and_check_triple()) {
                // Explicit here because this is a touch-only
                // interaction -- the automatic post-switch sound only
                // covers A/B/X/Y/L/R/Select, not KEY_TOUCH.
                audio_play(SFX_BUTTON);
                s_state = APP_BROWSE;
            }
            tapped = false;
        }

        // B, or a tap on the footer's "Back" hint -- the latter is
        // already converted into KEY_B by the generic footer-hit
        // check above (which also already plays SFX_BUTTON for that
        // path), so this one line covers both.
        if (kDown & KEY_B) s_state = APP_BROWSE;
        break;
    }

    case APP_BROWSE:
        if (kDown & KEY_DOWN) {
            int next = s_selected + GRID_COLS;
            int totalRowsNav = (s_visibleCount + GRID_COLS - 1) / GRID_COLS;
            int currentRowNav = s_selected / GRID_COLS;
            if (next < s_visibleCount) {
                s_selected = next;
            } else if (currentRowNav + 1 < totalRowsNav) {
                // A row below exists, but it's ragged (fewer items
                // than a full row) and this column has nothing in it
                // -- land on the last available item instead of
                // refusing to move. Without this, columns past
                // whatever the last row's actual width is were simply
                // unreachable via Down.
                s_selected = s_visibleCount - 1;
            }
            // else: already in the last row -- no row below at all,
            // so no move, same as before.
        }
        if (kDown & KEY_UP) {
            int next = s_selected - GRID_COLS;
            if (next >= 0) s_selected = next;
        }
        if (kDown & KEY_RIGHT && s_selected + 1 < s_visibleCount) s_selected++;
        if (kDown & KEY_LEFT && s_selected > 0) s_selected--;

        if (s_batchMode) {
            if (kDown & KEY_A) {
                if (s_selected >= 0 && s_selected < s_visibleCount) {
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
            int selTotal = item_count();
            for (int i = 0; i < selTotal; i++) if (s_batchSelected[i]) { anySel = true; break; }
            if ((kDown & KEY_X) && anySel) begin_batch_delete();
            if ((kDown & KEY_L) && anySel) begin_batch_convert();
        } else {
            if ((kDown & KEY_A) && current_preview_ready()) enter_detail_view();
            if (tapped) {
                int visPos;
                if (grid_hit_test(touch.px, touch.py, &visPos)) {
                    // Tapping moves the cursor there, and opens it
                    // directly only if that item's preview is already
                    // loaded -- tapping a cell that isn't yet selected
                    // just selects it, same as a fresh cursor move
                    // would; a second tap (or A) opens it once ready.
                    s_selected = visPos;
                    if (current_preview_ready()) enter_detail_view();
                }
            }
            if (kDown & KEY_SELECT) {
                s_batchMode = true;
                memset(s_batchSelected, 0, sizeof(s_batchSelected));
            }
            if (kDown & KEY_Y && !(s_dsMode && s_dsTab == DS_TAB_NDS_BOOTSTRAP)) begin_filter_menu();

            if (s_dsMode && s_dsTab == DS_TAB_NDS_BOOTSTRAP &&
                (kDown & KEY_START) && s_dsTarSlotCount > 0) {
                s_confirmSelection = 1; // default to Extract
                s_state = APP_DS_EXTRACT_PROMPT;
            }

            // L+R together switches 3DS/DS libraries. Checked with
            // keysHeld so the two shoulders don't have to land on the
            // exact same frame.
            bool lrCombo = s_dsAvailable && (kDown & (KEY_L | KEY_R)) &&
                           (hidKeysHeld() & KEY_L) && (hidKeysHeld() & KEY_R);

            // Tapping the header hint does the same thing. Hit-tested
            // against the rect captured while drawing it, so the touch
            // target always matches what's actually on screen.
            bool modeTapped = s_dsAvailable && s_modeToggleVisible && tapped &&
                              point_in_rect(touch.px, touch.py,
                                            s_modeToggleX, 0, s_modeToggleW, HEADER_H);
            if (modeTapped) tapped = false; // consumed -- don't also treat it as a grid tap

            // Triple-tap the header icon within 1 second -> easter egg.
            // The ring buffer naturally self-corrects each tap: since
            // the check requires ALL 3 stored taps to be within the
            // window, a stale old tap sitting in the buffer can't
            // contribute to a false trigger once it ages out, with no
            // separate "reset" logic needed.
            if (tapped && point_in_rect(touch.px, touch.py, 140, 0, 40, HEADER_H)) {
                if (register_icon_tap_and_check_triple()) {
                    audio_play(SFX_EASTER_EGG);
                    s_state = APP_EASTER_EGG;
                }
                tapped = false;
            }

            if (lrCombo || modeTapped) {
                // Deferred by a frame (op_enter/op_tick) so "Loading..."
                // actually renders before the blocking scan starts --
                // otherwise it'd only flash up after the wait it exists
                // to cover.
                op_enter(APP_MODE_SWITCHING);
            } else if (s_dsMode && !s_batchMode) {
                // A single L or R switches tabs -- L selects the left
                // (nds-bootstrap) tab, R the right (SD Card) one,
                // matching where their glyphs sit on each pill in the
                // mockup. A tap directly on a tab does the same.
                DSTab target = s_dsTab;
                if ((kDown & KEY_L) && !(hidKeysHeld() & KEY_R)) target = DS_TAB_NDS_BOOTSTRAP;
                if ((kDown & KEY_R) && !(hidKeysHeld() & KEY_L)) target = DS_TAB_SD_CARD;
                if (tapped && touch.py >= TAB_Y && touch.py < TAB_Y + TAB_H) {
                    if (touch.px >= TAB_LEFT_X && touch.px < TAB_LEFT_X + TAB_W) target = DS_TAB_NDS_BOOTSTRAP;
                    else if (touch.px >= TAB_RIGHT_X && touch.px < TAB_RIGHT_X + TAB_W) target = DS_TAB_SD_CARD;
                }

                bool targetHasContent = (target == DS_TAB_SD_CARD) ? (s_dsCount > 0) : (s_dsTarSlotCount > 0);
                if (target != s_dsTab && targetHasContent) {
                    s_dsTab = target;
                    reload_current_mode();
                }
            }
        }
        break;

    case APP_DETAIL: {
        u32 held = hidKeysHeld();
        // "Show bottom" is a 3DS-only peek -- DS captures have no
        // companion bottom frame, and R there is free for other use.
        bool rHeld = !s_dsMode && (held & KEY_R) != 0;
        poll_bottom_capture();

        // Hidden L+R: duplicate this screenshot. Checked before the
        // single-L widescreen toggle so holding both doesn't also flip
        // the preview on the way past.
        // Duplicating a tar item would mean writing a new slot into
        // screenshots.tar, which isn't supported -- so the shortcut
        // simply does nothing there rather than showing a dialog that
        // can't deliver.
        if ((kDown & (KEY_L | KEY_R)) && (held & KEY_L) && (held & KEY_R) &&
            !(s_dsMode && s_dsTab == DS_TAB_NDS_BOOTSTRAP)) {
            s_confirmSelection = 0; // default to Cancel
            s_state = APP_DUPLICATE_CONFIRM;
            break;
        }

        // DS only: L alone toggles the widescreen stretch.
        if (s_dsMode && (kDown & KEY_L) && !(held & KEY_R)) {
            if (s_selected >= 0 && s_selected < s_visibleCount) {
                int idx = s_visibleIndices[s_selected];
                if (s_dsTab == DS_TAB_SD_CARD) {
                    if (idx >= 0 && idx < s_dsCount) {
                        s_dsShots[idx].widescreen = !s_dsShots[idx].widescreen;
                        save_widescreen_prefs();
                    }
                } else if (idx >= 0 && idx < s_dsTarSlotCount) {
                    s_dsTarSlots[idx].widescreen = !s_dsTarSlots[idx].widescreen;
                    save_tar_widescreen_prefs();
                }
                // Not caught by the generic before/after diff (no
                // tracked field changes), so it gets its own explicit
                // sound here.
                audio_play(SFX_BUTTON);
            }
            break;
        }

        if (!rHeld) {
            int menuCount = detail_menu_item_count();
            if (kDown & KEY_DOWN) s_detailMenuSelection = (s_detailMenuSelection + 1) % menuCount;
            if (kDown & KEY_UP)   s_detailMenuSelection = (s_detailMenuSelection + menuCount - 1) % menuCount;
            if (kDown & KEY_B) s_state = APP_BROWSE;

            // Left/Right step through the gallery without leaving this
            // screen. Clamped rather than wrapping, matching how the
            // grid itself behaves at its edges.
            //
            // Gated on the current preview having actually finished
            // loading: without it, holding a direction races ahead of
            // the loader and the bottom screen ends up describing a
            // different screenshot than the top screen is showing --
            // the same mismatch that could otherwise lead to deleting
            // or exporting the wrong file. Same guard used for
            // entering this screen in the first place.
            if ((kDown & (KEY_LEFT | KEY_RIGHT)) && current_preview_ready()) {
                int next = s_selected + ((kDown & KEY_RIGHT) ? 1 : -1);
                if (next >= 0 && next < s_visibleCount && next != s_selected) {
                    s_selected = next;
                    // Menu shape can differ between screenshots (Merge
                    // only appears when there's a bottom capture), so
                    // re-clamp rather than leave the cursor past the end.
                    int newCount = detail_menu_item_count();
                    if (s_detailMenuSelection >= newCount) s_detailMenuSelection = newCount - 1;
                    // Same bottom-capture request the detail view does
                    // on entry -- without this, R would still peek at
                    // the previously-selected screenshot's bottom frame.
                    if (!s_dsMode) {
                        ScreenshotPair *np = current_pair();
                        if (np) request_bottom_capture(np);
                    }
                }
            }

            int chosen = -1;
            if (kDown & KEY_A) chosen = s_detailMenuSelection;
            if (tapped) {
                int idx;
                if (detail_menu_hit_test(touch.px, touch.py, &idx)) {
                    s_detailMenuSelection = idx;
                    chosen = idx;
                }
            }

            if (chosen >= 0) {
                // The tar tab prepends "Extract Screenshot", shifting
                // Copy/Delete down one -- normalise to a shared action
                // id so the dispatch below stays readable.
                int action = chosen;
                if (menuCount == 3) {
                    if (chosen == 0) action = 2; // Extract (tar tab only)
                    else             action = chosen - 1;
                }

                if (action == 0) {
                    begin_convert();
                } else if (action == 1) {
                    s_confirmSelection = 0;
                    s_state = APP_DETAIL_DELETE_CONFIRM;
                } else {
                    s_confirmSelection = 1; // default to Extract
                    s_state = APP_DS_ONE_EXTRACT_PROMPT;
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
            s_state = s_resultReturnState;
            s_resultReturnState = APP_BROWSE; // one-shot -- back to the default immediately
        }
        break;
    }

    case APP_DUPLICATE_CONFIRM: {
        float cx, cy, cw, ch, dx, dy, dw, dh;
        popup_button_rect(0, 2, &cx, &cy, &cw, &ch);
        popup_button_rect(1, 2, &dx, &dy, &dw, &dh);
        if (kDown & (KEY_LEFT | KEY_RIGHT)) s_confirmSelection ^= 1;

        bool pickedCancel = (kDown & KEY_B) ||
            ((kDown & KEY_A) && s_confirmSelection == 0) ||
            (tapped && point_in_rect(touch.px, touch.py, cx, cy, cw, ch));
        bool pickedDup    = ((kDown & KEY_A) && s_confirmSelection == 1) ||
            (tapped && point_in_rect(touch.px, touch.py, dx, dy, dw, dh));

        if (pickedDup)         op_enter(APP_DUPLICATING);
        else if (pickedCancel) s_state = APP_DETAIL;
        break;
    }

    case APP_DUPLICATING:
        if (op_tick()) do_duplicate();
        break;

    case APP_QUIT_CONFIRM: {
        float cx, cy, cw, ch, dx, dy, dw, dh;
        popup_button_rect(0, 2, &cx, &cy, &cw, &ch);
        popup_button_rect(1, 2, &dx, &dy, &dw, &dh);
        if (kDown & (KEY_LEFT | KEY_RIGHT)) s_confirmSelection ^= 1;

        bool pickedCancel = (kDown & KEY_B) ||
            ((kDown & KEY_A) && s_confirmSelection == 0) ||
            (tapped && point_in_rect(touch.px, touch.py, cx, cy, cw, ch));
        bool pickedQuit   = ((kDown & KEY_A) && s_confirmSelection == 1) ||
            (tapped && point_in_rect(touch.px, touch.py, dx, dy, dw, dh));

        if (pickedQuit)        return false; // actually exits
        else if (pickedCancel) s_state = APP_BROWSE;
        break;
    }

    case APP_DS_INTRO_POPUP: {
        // Informational only -- any of A/B or the OK button dismisses.
        float bx, by, bw, bh;
        popup_button_rect(0, 1, &bx, &by, &bw, &bh);
        if ((kDown & (KEY_A | KEY_B)) ||
            (tapped && point_in_rect(touch.px, touch.py, bx, by, bw, bh))) {
            reload_current_mode(); // deferred from enter_ds_mode -- see there
            s_state = APP_BROWSE;
        }
        break;
    }

    case APP_DS_ONE_EXTRACT_PROMPT: {
        float cx, cy, cw, ch, dx, dy, dw, dh;
        popup_button_rect(0, 2, &cx, &cy, &cw, &ch);
        popup_button_rect(1, 2, &dx, &dy, &dw, &dh);
        if (kDown & (KEY_LEFT | KEY_RIGHT)) s_confirmSelection ^= 1;

        bool pickedCancel  = (kDown & KEY_B) ||
            ((kDown & KEY_A) && s_confirmSelection == 0) ||
            (tapped && point_in_rect(touch.px, touch.py, cx, cy, cw, ch));
        bool pickedExtract = ((kDown & KEY_A) && s_confirmSelection == 1) ||
            (tapped && point_in_rect(touch.px, touch.py, dx, dy, dw, dh));

        if (pickedExtract)     op_enter(APP_DS_ONE_EXTRACTING);
        else if (pickedCancel) s_state = APP_DETAIL;
        break;
    }

    case APP_DS_ONE_EXTRACTING:
        if (op_tick()) do_ds_extract_one();
        break;

    case APP_DS_ONE_DELETE_PROMPT: {
        float cx, cy, cw, ch, dx, dy, dw, dh;
        popup_button_rect(0, 2, &cx, &cy, &cw, &ch);
        popup_button_rect(1, 2, &dx, &dy, &dw, &dh);
        if (kDown & (KEY_LEFT | KEY_RIGHT)) s_confirmSelection ^= 1;

        bool pickedCancel = (kDown & KEY_B) ||
            ((kDown & KEY_A) && s_confirmSelection == 0) ||
            (tapped && point_in_rect(touch.px, touch.py, cx, cy, cw, ch));
        bool pickedDelete = ((kDown & KEY_A) && s_confirmSelection == 1) ||
            (tapped && point_in_rect(touch.px, touch.py, dx, dy, dw, dh));

        if (pickedDelete) {
            op_enter(APP_DS_ONE_DELETING);
        } else if (pickedCancel) {
            // Nothing about the tar changed -- the extracted copy
            // exists alongside it now, but we're staying right where
            // we were, same slot, same tab.
            s_state = APP_DETAIL;
        }
        break;
    }

    case APP_DS_ONE_DELETING:
        if (op_tick()) do_ds_delete_one();
        break;

    case APP_MODE_SWITCHING:
        // op_tick fires this on frame 2, so frame 1 has already painted
        // the "Loading..." popup before the blocking scan begins. Both
        // functions set their own next state (enter_ds_mode may go to
        // the intro popup rather than the grid), so nothing further is
        // needed here.
        if (op_tick()) {
            if (s_dsMode) exit_ds_mode();
            else          enter_ds_mode();
        }
        break;

    case APP_DS_EXTRACT_PROMPT: {
        float cx, cy, cw, ch, dx, dy, dw, dh;
        popup_button_rect(0, 2, &cx, &cy, &cw, &ch);
        popup_button_rect(1, 2, &dx, &dy, &dw, &dh);
        if (kDown & (KEY_LEFT | KEY_RIGHT)) s_confirmSelection ^= 1;

        bool pickedCancel  = (kDown & KEY_B) ||
            ((kDown & KEY_A) && s_confirmSelection == 0) ||
            (tapped && point_in_rect(touch.px, touch.py, cx, cy, cw, ch));
        bool pickedExtract = ((kDown & KEY_A) && s_confirmSelection == 1) ||
            (tapped && point_in_rect(touch.px, touch.py, dx, dy, dw, dh));

        if (pickedExtract) {
            op_enter(APP_DS_EXTRACTING);
        } else if (pickedCancel) {
            // Cancelling with nothing already extracted means there's
            // nothing to browse -- fall back to 3DS mode rather than
            // showing an empty DS album.
            if (s_dsCount == 0) exit_ds_mode();
            else s_state = APP_BROWSE;
        }
        break;
    }

    case APP_DS_EXTRACTING:
        if (op_tick()) do_ds_extract();
        break;

    case APP_DS_CLEAR_PROMPT: {
        float cx, cy, cw, ch, dx, dy, dw, dh;
        popup_button_rect(0, 2, &cx, &cy, &cw, &ch);
        popup_button_rect(1, 2, &dx, &dy, &dw, &dh);
        if (kDown & (KEY_LEFT | KEY_RIGHT)) s_confirmSelection ^= 1;

        bool pickedCancel = (kDown & KEY_B) ||
            ((kDown & KEY_A) && s_confirmSelection == 0) ||
            (tapped && point_in_rect(touch.px, touch.py, cx, cy, cw, ch));
        bool pickedClear  = ((kDown & KEY_A) && s_confirmSelection == 1) ||
            (tapped && point_in_rect(touch.px, touch.py, dx, dy, dw, dh));

        if (pickedClear)       op_enter(APP_DS_CLEARING);
        else if (pickedCancel) s_state = APP_BROWSE;
        break;
    }

    case APP_DS_CLEARING:
        if (op_tick()) do_ds_clear();
        break;

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
        if (kDown & KEY_B) {
            if (s_dsMode) { s_state = APP_BROWSE; }
            else { s_filterCursor = 3; s_state = APP_FILTER_MENU; }
        }
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
        int total = s_availableYearCount + 1; // "All" + real years
        if (kDown & KEY_DOWN && s_filterCursor < total - 1) s_filterCursor++;
        if (kDown & KEY_UP && s_filterCursor > 0) s_filterCursor--;
        if (kDown & KEY_B) { s_filterCursor = 0; s_state = APP_FILTER_BY_DATE; }

        int chosen = -1;
        if (kDown & KEY_A) chosen = s_filterCursor;
        if (tapped) {
            int start = compute_list_start(s_filterCursor, total);
            int idx;
            if (list_row_hit_test(touch.py, start, total, &idx)) { chosen = idx; s_filterCursor = idx; }
        }
        if (chosen >= 0 && chosen < total) {
            int newYear = (chosen == 0) ? 0 : s_availableYears[chosen - 1];
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

    // The other half of the button-press sound: play it only if the
    // press actually changed something, comparing against the
    // snapshot taken before the switch. Catches the overwhelming
    // majority of real actions (navigation, selection, confirmation,
    // batch toggles) without needing an explicit call at every one of
    // the dozens of input branches above -- e.g. this is what makes B
    // silent on the plain Album screen, where it has no Back target.
    if (nonDirectionalPressed || tappedGoingIntoSwitch) {
        int batchSelAfter = 0;
        for (int i = 0; i < MAX_PAIRS; i++) if (s_batchSelected[i]) batchSelAfter += i + 1;

        bool changed = (s_state != stateBefore) ||
                       (s_selected != selectedBefore) ||
                       (s_batchMode != batchModeBefore) ||
                       (s_filterMode != filterModeBefore) ||
                       (s_filterYear != filterYearBefore) ||
                       (s_filterMonth != filterMonthBefore) ||
                       (s_filterDay != filterDayBefore) ||
                       (s_filterCursor != filterCursorBefore) ||
                       (s_detailMenuSelection != detailSelBefore) ||
                       (s_confirmSelection != confirmSelBefore) ||
                       (s_dsTab != dsTabBefore) ||
                       (batchSelAfter != batchSelSnapshot);
        if (changed) audio_play(SFX_BUTTON);
    }
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
