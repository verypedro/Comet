#include "fs_utils.h"
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static int pair_cmp_newest_first(const void *a, const void *b)
{
    const ScreenshotPair *pa = (const ScreenshotPair *)a;
    const ScreenshotPair *pb = (const ScreenshotPair *)b;
    return strcmp(pb->timestamp, pa->timestamp);
}

// Scans the screenshot folder(s) in a single enumeration per directory.
//
// Two properties matter here, and the first version of this got the
// second one wrong:
//   1. Companion files (_top_right / _bot) are answered from an in-memory
//      sorted list, not one stat() each -- that's what made the original
//      scan slow on every delete.
//   2. When there are more screenshots than the caller has room for, keep
//      the NEWEST, not whichever happened to be listed first. Directory
//      order on FAT is oldest-first, and the old code stopped at the cap in
//      that order -- so past the limit the newest screenshots silently
//      never appeared, which is exactly what a user with years of
//      screenshots saw ("only my old ones show up").
// No fixed-size name buffers either: the lists grow as needed, so neither
// a big folder nor a lot of Nexus3DS date folders can be silently cut off.

#define MAX_ENTRY_NAME 128
#define NAMELIST_HARD_MAX 20000   // sanity ceiling (~2.5MB), not a feature limit

typedef struct {
    char (*names)[MAX_ENTRY_NAME];
    int   count, cap;
} NameList;

static int name_cmp(const void *a, const void *b)      { return strcmp((const char *)a, (const char *)b); }
static int name_cmp_desc(const void *a, const void *b) { return strcmp((const char *)b, (const char *)a); }

static bool namelist_push(NameList *l, const char *s)
{
    if (l->count >= NAMELIST_HARD_MAX) return false;
    if (l->count == l->cap) {
        int ncap = l->cap ? l->cap * 2 : 256;
        void *np = realloc(l->names, (size_t)ncap * MAX_ENTRY_NAME);
        if (!np) return false;
        l->names = np;
        l->cap = ncap;
    }
    size_t n = strlen(s);
    if (n >= MAX_ENTRY_NAME) n = MAX_ENTRY_NAME - 1;
    memcpy(l->names[l->count], s, n);
    l->names[l->count][n] = '\0';
    l->count++;
    return true;
}

static void namelist_free(NameList *l)
{
    free(l->names);
    l->names = NULL;
    l->count = l->cap = 0;
}

// Reads every entry in one pass, splitting files from subdirectories.
// dirent's d_type is used where the filesystem reports it (FAT does),
// falling back to stat() only for entries it calls unknown.
static void enumerate_dir(const char *dirPath, NameList *files, NameList *dirs)
{
    DIR *d = opendir(dirPath);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue; // skip . and ..
        if (strlen(ent->d_name) >= MAX_ENTRY_NAME) continue;

        bool isDir;
        if (ent->d_type == DT_DIR)       isDir = true;
        else if (ent->d_type == DT_REG)  isDir = false;
        else {
            char full[600];
            snprintf(full, sizeof(full), "%s/%s", dirPath, ent->d_name);
            struct stat st;
            if (stat(full, &st) != 0) continue;
            isDir = S_ISDIR(st.st_mode);
        }

        if (isDir) { if (dirs) namelist_push(dirs, ent->d_name); }
        else       { namelist_push(files, ent->d_name); }
    }
    closedir(d);
}

// Sorted ascending, so bsearch works for the companion lookups.
static bool name_present(const char *needle, const NameList *files)
{
    return bsearch(needle, files->names, (size_t)files->count, MAX_ENTRY_NAME, name_cmp) != NULL;
}

static void fill_pair(ScreenshotPair *p, const char *dirPath, const char *name,
                      const char *ts, bool isCmb, const NameList *files)
{
    memset(p, 0, sizeof(*p));
    p->isCombined = isCmb;
    snprintf(p->timestamp, sizeof(p->timestamp), "%s", ts);
    snprintf(p->topPath, sizeof(p->topPath), "%s/%s", dirPath, name);

    // Combined images are self-contained -- no companions to find.
    if (isCmb) return;

    char candidate[MAX_ENTRY_NAME];
    snprintf(candidate, sizeof(candidate), "%s_top_right.bmp", ts);
    if (name_present(candidate, files)) {
        snprintf(p->topRightPath, sizeof(p->topRightPath), "%s/%s", dirPath, candidate);
        p->has3D = true;
    }
    snprintf(candidate, sizeof(candidate), "%s_bot.bmp", ts);
    if (name_present(candidate, files)) {
        snprintf(p->botPath, sizeof(p->botPath), "%s/%s", dirPath, candidate);
    }
}

static int oldest_index(const ScreenshotPair *out, int count)
{
    int m = 0;
    for (int i = 1; i < count; i++) {
        if (strcmp(out[i].timestamp, out[m].timestamp) < 0) m = i;
    }
    return m;
}

// Builds pairs from one directory's (sorted) file list, newest first, into
// `out` (capacity `max`, `count` already filled). Once full, a candidate
// only gets in by displacing the oldest one held -- and because this
// directory is walked newest-first, the first candidate that isn't newer
// than the oldest held means everything left here is older too, so it stops.
static int build_pairs_from_names(const char *dirPath, const NameList *files,
                                  ScreenshotPair *out, int max, int count)
{
    static const char TOP_SUFFIX[] = "_top.bmp";
    static const char CMB_SUFFIX[] = "_cmb.bmp";
    const size_t topLen = sizeof(TOP_SUFFIX) - 1;
    const size_t cmbLen = sizeof(CMB_SUFFIX) - 1;

    for (int i = files->count - 1; i >= 0; i--) {
        const char *name = files->names[i];
        size_t nameLen = strlen(name);

        bool isTop = nameLen > topLen && strcmp(name + nameLen - topLen, TOP_SUFFIX) == 0;
        bool isCmb = nameLen > cmbLen && strcmp(name + nameLen - cmbLen, CMB_SUFFIX) == 0;
        if (!isTop && !isCmb) continue;

        char ts[MAX_ENTRY_NAME];
        size_t tsLen = nameLen - (isCmb ? cmbLen : topLen);
        memcpy(ts, name, tsLen);
        ts[tsLen] = '\0';

        if (count < max) {
            fill_pair(&out[count++], dirPath, name, ts, isCmb, files);
        } else {
            int victim = oldest_index(out, count);
            if (strcmp(ts, out[victim].timestamp) <= 0) break; // the rest of this folder is older still
            fill_pair(&out[victim], dirPath, name, ts, isCmb, files);
        }
    }
    return count;
}

// Sorting the records themselves moves ~n log n of them, 834 bytes apiece --
// about 19MB of copying at 2048 entries, on every scan (boot, every delete).
// Sort a plain index array instead and move each record exactly once.
static const ScreenshotPair *s_sortBase;
static int idx_cmp_newest_first(const void *a, const void *b)
{
    return strcmp(s_sortBase[*(const int *)b].timestamp, s_sortBase[*(const int *)a].timestamp);
}

static void sort_newest_first(ScreenshotPair *out, int count)
{
    if (count < 2) return;
    int *order = (int *)malloc((size_t)count * sizeof(int));
    ScreenshotPair *tmp = (ScreenshotPair *)malloc((size_t)count * sizeof(ScreenshotPair));
    if (!order || !tmp) { // low on memory: the slow way still works
        free(order); free(tmp);
        qsort(out, (size_t)count, sizeof(ScreenshotPair), pair_cmp_newest_first);
        return;
    }
    for (int i = 0; i < count; i++) order[i] = i;
    s_sortBase = out;
    qsort(order, (size_t)count, sizeof(int), idx_cmp_newest_first);
    for (int i = 0; i < count; i++) tmp[i] = out[order[i]];
    memcpy(out, tmp, (size_t)count * sizeof(ScreenshotPair));
    free(order); free(tmp);
}

int fs_scan_screenshot_pairs(ScreenshotPair *out, int max)
{
    char rootPath[300];
    snprintf(rootPath, sizeof(rootPath), "%s%s", SD_ROOT, SCREENSHOTS_DIR);

    NameList files = {0}, dirs = {0};
    enumerate_dir(rootPath, &files, &dirs);
    qsort(files.names, (size_t)files.count, MAX_ENTRY_NAME, name_cmp);
    int count = build_pairs_from_names(rootPath, &files, out, max, 0);
    namelist_free(&files);

    // One level of subdirectories -- Nexus3DS's optional "save screenshots
    // in date folders" setting puts each day's captures in their own folder
    // directly under the root. Not validated as looking like a date: any
    // subfolder found gets scanned. Newest-named first, which usually means
    // the ones that can still displace something get seen before the ones
    // that can't.
    qsort(dirs.names, (size_t)dirs.count, MAX_ENTRY_NAME, name_cmp_desc);
    for (int i = 0; i < dirs.count; i++) {
        char subName[MAX_ENTRY_NAME];
        memcpy(subName, dirs.names[i], MAX_ENTRY_NAME);
        subName[MAX_ENTRY_NAME - 1] = '\0';

        char subPath[600];
        snprintf(subPath, sizeof(subPath), "%s/%s", rootPath, subName);

        NameList subFiles = {0};
        enumerate_dir(subPath, &subFiles, NULL); // only one level deep
        qsort(subFiles.names, (size_t)subFiles.count, MAX_ENTRY_NAME, name_cmp);
        count = build_pairs_from_names(subPath, &subFiles, out, max, count);
        namelist_free(&subFiles);
    }
    namelist_free(&dirs);

    sort_newest_first(out, count);
    return count;
}

bool fs_ensure_dir_exists(const char *path)
{
    char buf[512];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *start = buf;
    if (strncmp(start, SD_ROOT, strlen(SD_ROOT)) == 0)
        start += strlen(SD_ROOT);

    for (char *p = start + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, 0777) != 0 && errno != EEXIST) return false;
            *p = '/';
        }
    }
    if (mkdir(buf, 0777) != 0 && errno != EEXIST) return false;
    return true;
}

bool fs_next_dcim_slot(char *outDir, size_t outDirSize,
                        char *outBaseName, size_t outBaseNameSize)
{
    if (!fs_ensure_dir_exists(SD_ROOT DCIM_DIR)) return false;

    DIR *d = opendir(SD_ROOT DCIM_DIR);
    int maxFolder = 0;
    int maxHni = 0;

    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            size_t len = strlen(ent->d_name);
            if (len != 8) continue;
            if (strcmp(ent->d_name + 3, "NIN03") != 0) continue;
            if (!isdigit((unsigned char)ent->d_name[0]) ||
                !isdigit((unsigned char)ent->d_name[1]) ||
                !isdigit((unsigned char)ent->d_name[2])) continue;

            int num = (ent->d_name[0] - '0') * 100 +
                      (ent->d_name[1] - '0') * 10 +
                      (ent->d_name[2] - '0');
            if (num > maxFolder) maxFolder = num;

            char sub[300];
            snprintf(sub, sizeof(sub), "%s%s/%s", SD_ROOT, DCIM_DIR, ent->d_name);
            DIR *sd = opendir(sub);
            if (sd) {
                struct dirent *sent;
                while ((sent = readdir(sd)) != NULL) {
                    if (strncmp(sent->d_name, "HNI_", 4) != 0) continue;
                    int idx = atoi(sent->d_name + 4);
                    if (idx > maxHni) maxHni = idx;
                }
                closedir(sd);
            }
        }
        closedir(d);
    }

    int targetFolder = maxFolder > 0 ? maxFolder : 100;

    char targetPath[300];
    snprintf(targetPath, sizeof(targetPath), "%s%s/%03dNIN03", SD_ROOT, DCIM_DIR, targetFolder);
    int filesInTarget = 0;
    DIR *td = opendir(targetPath);
    if (td) {
        struct dirent *ent;
        while ((ent = readdir(td)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            filesInTarget++;
        }
        closedir(td);
    }
    if (filesInTarget >= 100) targetFolder++;

    int nextHni = maxHni + 1;
    if (nextHni < 1) nextHni = 1;
    if (nextHni > 9999) nextHni = 9999;

    snprintf(outDir, outDirSize, "%s%s/%03dNIN03", SD_ROOT, DCIM_DIR, targetFolder);
    if (!fs_ensure_dir_exists(outDir)) return false;

    snprintf(outBaseName, outBaseNameSize, "HNI_%04d", nextHni);
    return true;
}

bool fs_write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    return written == len;
}

bool fs_delete_pair(const ScreenshotPair *p)
{
    bool ok = true;
    if (p->topPath[0]) {
        if (remove(p->topPath) != 0) ok = false;
    }
    if (p->topRightPath[0]) remove(p->topRightPath); // best-effort
    if (p->botPath[0])      remove(p->botPath);       // best-effort

    // Belt-and-braces: if either companion path wasn't recorded (a
    // stale struct, or a file that appeared after the scan), derive it
    // from topPath's own directory and try again. Deriving from
    // topPath rather than the screenshots root matters for Nexus3DS's
    // date-subfolder layout, where the companions live beside the
    // _top.bmp rather than at the root. Costs one stat per missing
    // field and can't touch anything outside that same folder.
    //
    // Never for a combined (_cmb.bmp) entry: those never have real
    // companions at all, by design, and a merged screenshot
    // deliberately shares its timestamp and directory with the
    // original pair it came from. Guessing a companion path for it
    // reconstructs the *original's* real _top_right.bmp/_bot.bmp --
    // which genuinely exists, so it would get deleted right along
    // with the merged copy, silently stripping the original of its
    // 3D and bottom-screen data.
    if (!p->isCombined &&
        (!p->topRightPath[0] || !p->botPath[0]) && p->topPath[0] && p->timestamp[0]) {
        const char *lastSlash = strrchr(p->topPath, '/');
        if (lastSlash) {
            int dirLen = (int)(lastSlash - p->topPath);
            char candidate[300];
            if (!p->topRightPath[0]) {
                snprintf(candidate, sizeof(candidate), "%.*s/%s_top_right.bmp",
                         dirLen, p->topPath, p->timestamp);
                if (file_exists(candidate)) remove(candidate);
            }
            if (!p->botPath[0]) {
                snprintf(candidate, sizeof(candidate), "%.*s/%s_bot.bmp",
                         dirLen, p->topPath, p->timestamp);
                if (file_exists(candidate)) remove(candidate);
            }
        }
    }
    return ok;
}
