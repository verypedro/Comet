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

        char rightName[280];
        snprintf(rightName, sizeof(rightName), "%s%s/%s_top_right.bmp",
                 SD_ROOT, SCREENSHOTS_DIR, p->timestamp);
        if (file_exists(rightName)) {
            snprintf(p->topRightPath, sizeof(p->topRightPath), "%s", rightName);
            p->has3D = true;
        }

        char botName[280];
        snprintf(botName, sizeof(botName), "%s%s/%s_bot.bmp",
                 SD_ROOT, SCREENSHOTS_DIR, p->timestamp);
        if (file_exists(botName)) {
            snprintf(p->botPath, sizeof(p->botPath), "%s", botName);
        }

        count++;
    }
    closedir(d);

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
    return ok;
}
