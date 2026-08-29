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

int fs_scan_screenshot_pairs(ScreenshotPair *out, int max)
{
    DIR *d = opendir(SD_ROOT SCREENSHOTS_DIR);
    if (!d) return -1;

    int count = 0;
    struct dirent *ent;
    static const char SUFFIX[] = "_top.bmp";
    const size_t suffixLen = sizeof(SUFFIX) - 1;

    while ((ent = readdir(d)) != NULL && count < max) {
        size_t nameLen = strlen(ent->d_name);
        if (nameLen <= suffixLen) continue;
        if (strcmp(ent->d_name + nameLen - suffixLen, SUFFIX) != 0) continue;

        ScreenshotPair *p = &out[count];
        memset(p, 0, sizeof(*p));

        size_t tsLen = nameLen - suffixLen;
        if (tsLen >= sizeof(p->timestamp)) tsLen = sizeof(p->timestamp) - 1;
        memcpy(p->timestamp, ent->d_name, tsLen);
        p->timestamp[tsLen] = '\0';

        snprintf(p->topPath, sizeof(p->topPath), "%s%s/%s",
                 SD_ROOT, SCREENSHOTS_DIR, ent->d_name);

<<<<<<< Updated upstream
        char rightName[280];
        snprintf(rightName, sizeof(rightName), "%s%s/%s_top_right.bmp",
                 SD_ROOT, SCREENSHOTS_DIR, p->timestamp);
        if (file_exists(rightName)) {
            snprintf(p->topRightPath, sizeof(p->topRightPath), "%s", rightName);
=======
        snprintf(p->topRightPath, sizeof(p->topRightPath), "%s/%s_top_right.bmp", dirPath, p->timestamp);
        if (file_exists(p->topRightPath)) {
>>>>>>> Stashed changes
            p->has3D = true;
        } else {
            p->topRightPath[0] = '\0';
        }

<<<<<<< Updated upstream
        char botName[280];
        snprintf(botName, sizeof(botName), "%s%s/%s_bot.bmp",
                 SD_ROOT, SCREENSHOTS_DIR, p->timestamp);
        if (file_exists(botName)) {
            snprintf(p->botPath, sizeof(p->botPath), "%s", botName);
=======
        snprintf(p->botPath, sizeof(p->botPath), "%s/%s_bot.bmp", dirPath, p->timestamp);
        if (!file_exists(p->botPath)) {
            p->botPath[0] = '\0';
>>>>>>> Stashed changes
        }

        count++;
    }
    closedir(d);
<<<<<<< Updated upstream
=======
    return count;
}

// Scans one directory for standalone _cmb.bmp files -- Nexus3DS's own
// "combine top/bottom screenshots" output format (400x480, self-
// contained), and what Comet's own Merge Top/Bottom Screens feature
// also writes now, using the same convention. No companion file to
// look for -- the whole image already stands alone.
static int scan_combined_in_dir(const char *dirPath, ScreenshotPair *out, int max, int startCount)
{
    DIR *d = opendir(dirPath);
    if (!d) return startCount;

    int count = startCount;
    struct dirent *ent;
    static const char SUFFIX[] = "_cmb.bmp";
    const size_t suffixLen = sizeof(SUFFIX) - 1;

    while ((ent = readdir(d)) != NULL && count < max) {
        size_t nameLen = strlen(ent->d_name);
        if (nameLen <= suffixLen) continue;
        if (strcmp(ent->d_name + nameLen - suffixLen, SUFFIX) != 0) continue;

        ScreenshotPair *p = &out[count];
        memset(p, 0, sizeof(*p));
        p->isCombined = true;

        size_t tsLen = nameLen - suffixLen;
        if (tsLen >= sizeof(p->timestamp)) tsLen = sizeof(p->timestamp) - 1;
        memcpy(p->timestamp, ent->d_name, tsLen);
        p->timestamp[tsLen] = '\0';

        // The combined image stands in as "topPath" -- it's what gets
        // loaded for both the preview and the thumbnail. No separate
        // right-eye or bottom capture exists in this format.
        snprintf(p->topPath, sizeof(p->topPath), "%s/%s", dirPath, ent->d_name);

        count++;
    }
    closedir(d);
    return count;
}

int fs_scan_screenshot_pairs(ScreenshotPair *out, int max)
{
    char rootPath[300];
    snprintf(rootPath, sizeof(rootPath), "%s%s", SD_ROOT, SCREENSHOTS_DIR);

    int count = scan_one_dir(rootPath, out, max, 0);
    count = scan_combined_in_dir(rootPath, out, max, count);

    // One level of subdirectories -- Nexus3DS's optional "save
    // screenshots in date folders" setting puts each day's captures
    // in their own folder directly under the root (e.g.
    // luma/screenshots/2026-08-27/). Not validated as looking like a
    // date -- any subfolder found here just also gets scanned, so
    // this doesn't depend on matching one exact naming scheme, and
    // stays correct if a different fork names them differently.
    DIR *d = opendir(rootPath);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL && count < max) {
            if (ent->d_name[0] == '.') continue; // skip . and ..

            char subPath[600];
            snprintf(subPath, sizeof(subPath), "%s/%s", rootPath, ent->d_name);

            struct stat st;
            if (stat(subPath, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

            count = scan_one_dir(subPath, out, max, count);
            count = scan_combined_in_dir(subPath, out, max, count);
        }
        closedir(d);
    }
>>>>>>> Stashed changes

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
