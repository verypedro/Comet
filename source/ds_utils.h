#pragma once

#include "common.h"

// nds-bootstrap keeps DS screenshots in a single uncompressed TAR with
// exactly 50 fixed slots. Unused slots are present but written as all
// zero bytes -- so a slot is "real" iff its payload starts with the
// BMP signature. (Verified against a real screenshots.tar: 50 entries,
// every one 98374 bytes, unused ones entirely 0x00.)
#define DS_TAR_PATH        "/_nds/nds-bootstrap/screenshots.tar"
#define DS_SCREENSHOTS_DIR "/3ds/Comet/ds_screenshots"

#define MAX_DS_SHOTS 256

typedef struct {
    char path[256];       // .../ds_screenshots/{stamp}_slotNN.bmp
    char timestamp[32];   // "YYYY-MM-DD_HH-MM-SS" -- when Comet IMPORTED it.
                          // nds-bootstrap records no capture time anywhere
                          // (every tar entry carries the same hardcoded
                          // mtime), so this is the only date available.
    bool widescreen;       // UI-only display preference, not file data --
                          // ds_scan_extracted() always resets this to
                          // false for every entry, since array positions
                          // can be reassigned to a different file after
                          // any rescan (extract/delete/duplicate all
                          // trigger one). Letting a stale value survive
                          // a reorder would silently misapply it to the
                          // wrong screenshot, which is worse than not
                          // remembering it at all.
} DSScreenshot;

// How many non-blank screenshots the tar currently holds (0 if the tar
// is missing/unreadable). Cheap: seeks the tar's entry headers and
// peeks 2 bytes per slot, never decoding any image data.
int ds_count_tar_screenshots(void);

// A single occupied slot in screenshots.tar, addressable in place.
typedef struct {
    long dataOffset;      // byte offset of the BMP payload within the tar
    long size;
    int  slot;            // 1-based position, for filenames on extract
    bool widescreen;      // UI-only, session-scoped (not persisted -- a
                          // tar slot has no stable identity to persist
                          // against once any deletion happens)
} DSTarSlot;

#define MAX_DS_TAR_SLOTS 50

// Lists occupied slots (payload starts with "BM"), in tar order.
int ds_list_tar_slots(DSTarSlot *out, int max);

// Extracts one slot to DS_SCREENSHOTS_DIR.
bool ds_extract_slot(const DSTarSlot *slot, char *outErr, size_t outErrSize);

// Removes one screenshot from the tar and closes the gap it leaves.
//
// Compaction is not optional here: nds-bootstrap counts screenshots by
// scanning from slot 1 and stopping at the first blank one, so a hole
// in the middle makes it believe the library ends there -- and it will
// then happily overwrite every real screenshot past the hole as new
// ones are taken. Repacking the survivors into slots 1..N keeps that
// count honest.
bool ds_delete_tar_slot(int slotIndex);

// A content fingerprint for one tar slot, stable across compaction.
//
// Tar slots have no persistent identity to key a preference against:
// slot 3 today may hold a different screenshot tomorrow, because
// deleting renumbers everything above it. Hashing a sample of the
// pixel data instead means the fingerprint travels with the *image*,
// wherever it ends up. (Two byte-identical screenshots would collide,
// but they'd be visually identical anyway, so treating them the same
// is harmless.)
unsigned long ds_slot_fingerprint(const DSTarSlot *slot);

// How many screenshots have already been extracted to SD.
int ds_count_extracted(void);

// Extracts every non-blank slot into DS_SCREENSHOTS_DIR, naming each
// file with the current time plus its slot number (so repeated imports
// in the same second can't collide). Returns the number written.
int ds_extract_all(char *outErr, size_t outErrSize);

// Blanks every slot's payload in place, leaving the archive itself
// intact. Deliberately not a delete: nds-bootstrap has to rebuild the
// whole 50-slot file from scratch if it's missing, which noticeably
// slows the next game launch -- zeroing the payloads leaves it with
// nothing to do. Blanking *all* slots (rather than picking some out)
// also sidesteps nds-bootstrap's sequential slot counting, which stops
// at the first blank slot it finds.
//
// Only meaningful after a successful extract; callers own that ordering.
bool ds_clear_tar(void);

// Lists extracted screenshots, newest first.
int ds_scan_extracted(DSScreenshot *out, int max);

bool ds_delete(const DSScreenshot *s);
