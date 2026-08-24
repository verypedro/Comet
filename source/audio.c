#include "audio.h"
#include <3ds.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// One ndsp channel per sound. With only five sounds this is simpler
// than a channel pool, and it gives each sound predictable behaviour:
// re-triggering one restarts it rather than layering on top of itself.
typedef struct {
    u8         *data;      // linear-allocated PCM, DSP-visible
    u32         byteLen;
    u32         sampleRate;
    u16         channels;
    ndspWaveBuf wave;
    bool        ok;
} Sfx;

static Sfx  s_sfx[SFX_COUNT];
static bool s_audioOk = false;

static const char *const SFX_PATHS[SFX_COUNT] = {
    [SFX_BUTTON]     = "romfs:/sfx/ButtonPress.wav",
    [SFX_POPUP]      = "romfs:/sfx/Pop-Up.wav",
    [SFX_COPY]       = "romfs:/sfx/Copy.wav",
    [SFX_DELETE]     = "romfs:/sfx/Delete.wav",
    [SFX_THUMB_LOAD] = "romfs:/sfx/SmallThumbLoad.wav",
};

static u32 rd32(const u8 *p) { return p[0] | (p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }

// Minimal RIFF/WAVE reader: walks the chunk list looking for "fmt " and
// "data". Deliberately strict -- only uncompressed 16-bit PCM is
// accepted, which is what the DSP plays natively.
static bool load_wav(const char *path, Sfx *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long fileLen = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fileLen < 44) { fclose(f); return false; }

    u8 *raw = (u8 *)malloc((size_t)fileLen);
    if (!raw) { fclose(f); return false; }
    if (fread(raw, 1, (size_t)fileLen, f) != (size_t)fileLen) {
        free(raw); fclose(f); return false;
    }
    fclose(f);

    if (memcmp(raw, "RIFF", 4) != 0 || memcmp(raw + 8, "WAVE", 4) != 0) {
        free(raw); return false;
    }

    u16 fmtTag = 0, channels = 0, bits = 0;
    u32 rate = 0;
    const u8 *pcm = NULL;
    u32 pcmLen = 0;

    size_t pos = 12;
    while (pos + 8 <= (size_t)fileLen) {
        const u8 *id = raw + pos;
        u32 sz = rd32(raw + pos + 4);
        size_t body = pos + 8;
        if (body + sz > (size_t)fileLen) break;

        if (memcmp(id, "fmt ", 4) == 0 && sz >= 16) {
            fmtTag   = rd16(raw + body + 0);
            channels = rd16(raw + body + 2);
            rate     = rd32(raw + body + 4);
            bits     = rd16(raw + body + 14);
        } else if (memcmp(id, "data", 4) == 0) {
            pcm = raw + body;
            pcmLen = sz;
        }
        pos = body + sz + (sz & 1); // chunks are word-aligned
    }

    if (fmtTag != 1 || bits != 16 || !pcm || pcmLen == 0 ||
        (channels != 1 && channels != 2)) {
        free(raw);
        return false;
    }

    // The DSP can only read from linear memory.
    out->data = (u8 *)linearAlloc(pcmLen);
    if (!out->data) { free(raw); return false; }
    memcpy(out->data, pcm, pcmLen);
    DSP_FlushDataCache(out->data, pcmLen);
    free(raw);

    out->byteLen    = pcmLen;
    out->sampleRate = rate;
    out->channels   = channels;
    out->ok         = true;
    return true;
}

void audio_init(void)
{
    // ndspInit() fails when the console has no DSP firmware dumped
    // (sdmc:/3ds/dspfirm.cdc). That's common enough that audio has to
    // be strictly optional -- everything below no-ops if this fails.
    if (R_FAILED(ndspInit())) {
        s_audioOk = false;
        return;
    }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);

    for (int i = 0; i < SFX_COUNT; i++) {
        memset(&s_sfx[i], 0, sizeof(s_sfx[i]));
        if (!load_wav(SFX_PATHS[i], &s_sfx[i])) continue;

        ndspChnReset(i);
        ndspChnSetInterp(i, NDSP_INTERP_LINEAR);
        ndspChnSetRate(i, (float)s_sfx[i].sampleRate);
        ndspChnSetFormat(i, s_sfx[i].channels == 2 ? NDSP_FORMAT_STEREO_PCM16
                                                    : NDSP_FORMAT_MONO_PCM16);

        // Set the mix explicitly rather than trusting ndspChnReset's
        // defaults: front-left and front-right at full volume.
        float mix[12];
        memset(mix, 0, sizeof(mix));
        mix[0] = 1.0f; // front left
        mix[1] = 1.0f; // front right
        ndspChnSetMix(i, mix);
    }
    s_audioOk = true;
}

void audio_exit(void)
{
    if (!s_audioOk) return;
    for (int i = 0; i < SFX_COUNT; i++) {
        if (!s_sfx[i].ok) continue;
        ndspChnWaveBufClear(i);
        linearFree(s_sfx[i].data);
        s_sfx[i].ok = false;
    }
    ndspExit();
    s_audioOk = false;
}

void audio_play(SfxId id)
{
    if (!s_audioOk || id < 0 || id >= SFX_COUNT) return;
    Sfx *s = &s_sfx[id];
    if (!s->ok) return;

    u32 bytesPerSample = 2u * s->channels;

    ndspChnWaveBufClear(id);
    memset(&s->wave, 0, sizeof(s->wave));
    s->wave.data_vaddr = s->data;
    s->wave.nsamples   = s->byteLen / bytesPerSample;
    s->wave.looping    = false;
    s->wave.status     = NDSP_WBUF_FREE;
    ndspChnWaveBufAdd(id, &s->wave);
}
