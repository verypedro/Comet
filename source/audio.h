#pragma once

#include "common.h"

typedef enum {
    SFX_BUTTON,      // any face/shoulder button press
    SFX_POPUP,       // a popup appearing
    SFX_COPY,        // copy-to-album finished
    SFX_DELETE,      // delete finished
    SFX_THUMB_LOAD,  // a grid thumbnail finished loading
    SFX_EASTER_EGG,  // the secret screen's own jingle
    SFX_COUNT,
} SfxId;

// Safe to call even if audio is unavailable -- every function becomes a
// no-op if ndspInit() fails (which it does when the console has no DSP
// firmware dumped). The app never depends on audio succeeding.
void audio_init(void);
void audio_exit(void);
void audio_play(SfxId id);
