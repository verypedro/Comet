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

// Scans one directory in a SINGLE pass.
//
// The previous version enumerated each directory three times (once for
// _top.bmp, once for _cmb.bmp, once looking for subfolders) and issued
// two stat() calls per screenshot to test for its _top_right/_bot
// companions, plus one stat() per entry just to ask "is this a
// directory?". For a flat 88-screenshot folder that worked out to
// roughly 440 filesystem lookups per scan -- and a scan runs on every
// single delete, which is exactly why deleting had become slow.
//
// Now: one enumeration collects every filename into memory, and all
// the companion questions are answered by string comparison against
// that list. Zero extra I/O.

#define MAX_DIR_ENTRIES 512
#define MAX_ENTRY_NAME  128

typedef struct {
    char (*names)[MAX_ENTRY_NAME];
    int   count;
} NameList;

static int name_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

// Both lists are static rather than stack-allocated: at 512 * 128 =
// 64KB each they'd blow the 3DS's comparatively small stack, and only
// one scan ever runs at a time.
static char s_entryNames[MAX_DIR_ENTRIES][MAX_ENTRY_NAME];
static char s_subDirNames[64][MAX_ENTRY_NAME];

// Reads every entry in one pass, splitting files from subdirectories.
// Uses dirent's d_type where the filesystem reports it (FAT does),
// falling back to stat() only for entries it reports as unknown --
// which is what removes the per-entry stat() call the subfolder scan
// used to make unconditionally.
static void enumerate_dir(const char *dirPath, int *outFileCount, int *outDirCount)
{
    *outFileCount = 0;
    *outDirCount  = 0;

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

        if (isDir) {
            if (*outDirCount < 64) {
                snprintf(s_subDirNames[*outDirCount], MAX_ENTRY_NAME, "%s", ent->d_name);
                (*outDirCount)++;
            }
        } else {
            if (*outFileCount < MAX_DIR_ENTRIES) {
                snprintf(s_entryNames[*outFileCount], MAX_ENTRY_NAME, "%s", ent->d_name);
                (*outFileCount)++;
            }
        }
    }
    closedir(d);
}

// Sorted list + bsearch, so companion lookups stay fast even with a
// few hundred files rather than degrading to a linear scan each time.
static bool name_present(const char *needle, int fileCount)
{
    return bsearch(needle, s_entryNames, fileCount, MAX_ENTRY_NAME, name_cmp) != NULL;
}

// Builds pairs from an already-enumerated, already-sorted name list.
static int build_pairs_from_names(const char *dirPath, int fileCount,
                                   ScreenshotPair *out, int max, int startCount)
{
    int count = startCount;
    static const char TOP_SUFFIX[] = "_top.bmp";
    static const char CMB_SUFFIX[] = "_cmb.bmp";
    const size_t topLen = sizeof(TOP_SUFFIX) - 1;
    const size_t cmbLen = sizeof(CMB_SUFFIX) - 1;

    for (int i = 0; i < fileCount && count < max; i++) {
        const char *name = s_entryNames[i];
        size_t nameLen = strlen(name);

        bool isTop = nameLen > topLen && strcmp(name + nameLen - topLen, TOP_SUFFIX) == 0;
        bool isCmb = nameLen > cmbLen && strcmp(name + nameLen - cmbLen, CMB_SUFFIX) == 0;
        if (!isTop && !isCmb) continue;

        ScreenshotPair *p = &out[count];
        memset(p, 0, sizeof(*p));
        p->isCombined = isCmb;

        size_t tsLen = nameLen - (isCmb ? cmbLen : topLen);
        if (tsLen >= sizeof(p->timestamp)) tsLen = sizeof(p->timestamp) - 1;
        memcpy(p->timestamp, name, tsLen);
        p->timestamp[tsLen] = '\0';

        snprintf(p->topPath, sizeof(p->topPath), "%s/%s", dirPath, name);

        // Combined images are self-contained -- no companions to find.
        if (!isCmb) {
            char candidate[MAX_ENTRY_NAME];

            snprintf(candidate, sizeof(candidate), "%s_top_right.bmp", p->timestamp);
            if (name_present(candidate, fileCount)) {
                snprintf(p->topRightPath, sizeof(p->topRightPath), "%s/%s", dirPath, candidate);
                p->has3D = true;
            }

            snprintf(candidate, sizeof(candidate), "%s_bot.bmp", p->timestamp);
            if (name_present(candidate, fileCount)) {
                snprintf(p->botPath, sizeof(p->botPath), "%s/%s", dirPath, candidate);
            }
        }

        count++;
    }
    return count;
}

int fs_scan_screenshot_pairs(ScreenshotPair *out, int max)
{
    char rootPath[300];
    snprintf(rootPath, sizeof(rootPath), "%s%s", SD_ROOT, SCREENSHOTS_DIR);

    int fileCount = 0, dirCount = 0;
    enumerate_dir(rootPath, &fileCount, &dirCount);

    // Copy the subdirectory names out before the buffer gets reused by
    // the per-subfolder enumerations below. Fixed-size copy rather than
    // snprintf("%s"): GCC can't bound a 2D-array row access through a
    // %s and assumes the whole array could be read, which is what
    // produced a spurious truncation warning here.
    char subDirs[64][MAX_ENTRY_NAME];
    int subDirCount = dirCount;
    for (int i = 0; i < subDirCount; i++) {
        memcpy(subDirs[i], s_subDirNames[i], MAX_ENTRY_NAME);
        subDirs[i][MAX_ENTRY_NAME - 1] = '\0';
    }

    qsort(s_entryNames, fileCount, MAX_ENTRY_NAME, name_cmp);
    int count = build_pairs_from_names(rootPath, fileCount, out, max, 0);

    // One level of subdirectories -- Nexus3DS's optional "save
    // screenshots in date folders" setting puts each day's captures
    // in their own folder directly under the root. Not validated as
    // looking like a date: any subfolder found gets scanned, so this
    // doesn't depend on one exact naming scheme.
    for (int i = 0; i < subDirCount && count < max; i++) {
        // Via a plain 1D buffer, for the same GCC-can't-bound-a-2D-row
        // reason as the copy above.
        char subName[MAX_ENTRY_NAME];
        memcpy(subName, subDirs[i], MAX_ENTRY_NAME);
        subName[MAX_ENTRY_NAME - 1] = '\0';

        char subPath[600];
        snprintf(subPath, sizeof(subPath), "%s/%s", rootPath, subName);

        int subFileCount = 0, subDirIgnored = 0;
        enumerate_dir(subPath, &subFileCount, &subDirIgnored); // only one level deep
        qsort(s_entryNames, subFileCount, MAX_ENTRY_NAME, name_cmp);
        count = build_pairs_from_names(subPath, subFileCount, out, max, count);
    }

    qsort(out, count, sizeof(ScreenshotPair), pair_cmp_newest_first);
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
