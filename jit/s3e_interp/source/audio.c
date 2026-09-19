/* audio.c -- a small mixer feeding libnx audout.
 *
 * See audio.h for what the game hands us and how that was established.
 *
 * Shape of the thing: the guest thread starts voices from inside an HLE
 * handler, a mixer thread turns whatever is active into 48 kHz stereo and
 * keeps audout fed. The two meet at g_ch under one mutex, and nowhere else --
 * in particular the mixer never touches guest memory, because PCM is copied
 * when a voice starts. The alternative, holding the guest pointer, means
 * reading an address the game is free to reuse the instant Play returns, from
 * a thread that has no idea what the guest is doing. Copying an SFX is a few
 * kilobytes; the race is not worth the memcpy it saves.
 */
#include <malloc.h>
#include <string.h>
#include <switch.h>

#include "audio.h"

/* 1024 frames at 48 kHz is ~21 ms per buffer, four buffers ~85 ms of slack
 * before a late mixer thread is audible. Small enough that a gunshot does not
 * lag the muzzle flash; large enough that the thread can miss a scheduling
 * slot without a gap. 1024 * 2ch * 2 bytes is exactly 4096, so the 0x1000
 * alignment audout requires costs no padding. */
#define OUT_RATE    48000u
#define OUT_FRAMES  1024u
#define OUT_BUFSZ   (OUT_FRAMES * 2u * sizeof(int16_t))
#define OUT_BUFS    4u

typedef struct {
    int16_t *pcm;           /* our copy */
    uint32_t samples;
    uint32_t cap;           /* allocated frames, so a reused voice can skip malloc */
    /* Q16 position within pcm. 64-bit: a 32-bit Q16 index tops out at 65535
     * samples and wrapped back to 0, so any sound longer than ~3 s at 22 kHz
     * (repair_00, buy_debris) never drained and looped forever. */
    uint64_t pos;
    uint32_t step;          /* Q16 source frames per output frame */
    uint32_t volume;        /* as the game set it; 256 taken as unity */
    uint8_t  active;
    uint8_t  paused;
} Voice;

static Voice           g_ch[SND_OUT_CHANNELS];
static Mutex           g_lock;
static Thread          g_thread;
static AudioOutBuffer  g_buf[OUT_BUFS];
static int             g_live;
static volatile int    g_running;

/* Voices that finished since the last time anyone asked.
 *
 * The mixer runs on its own thread and the callback queue is the guest
 * thread's, so a drained voice cannot be turned into a guest callback here --
 * it is recorded and collected from the guest side. One bit per voice, taken
 * and cleared together, so a sound that starts and finishes between two
 * collections still reports exactly once. */
static uint32_t        g_drained;

/* Mixer liveness. A dead mixer thread is indistinguishable from a broken one
 * from the outside: voices never drain, so channels never come free, so the
 * game stops asking for them -- silence that looks like the game's decision
 * rather than ours. This counts buffers actually filled. */
static uint64_t        g_mix_calls;
static int             g_music_on = 1;
static uint32_t        g_master = 256u;   /* s3eSoundSetInt VOLUME, 256 = unity */


/* ---- IMA ADPCM ---------------------------------------------------------
 *
 * The game does not hand us PCM. It hands us 4-bit IMA ADPCM in 512-byte
 * blocks, which the sample descriptor states plainly once the fields are read
 * correctly: 11264 compressed bytes, 22451 decoded samples, 1017 samples per
 * block, 512-byte blocks -- and 11264/512 is exactly 22 blocks. 1017 samples
 * from a 512-byte block is the IMA signature: a 4-byte block header followed
 * by 508 bytes of nibbles, two samples each, plus the seed the header carries.
 *
 * Played as though it were PCM it is a loud noisy burst about four times too
 * long, which is exactly what it sounded like, and what finally identified it:
 * the duration was wrong by the compression ratio.
 *
 * The step and index tables below are the ones from the IMA/DVI definition.
 */
static const int16_t ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
    45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190,
    209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724,
    796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272,
    2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132,
    7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767
};

static const int8_t ima_index[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
};

/* Decode one nibble, advancing the predictor and step index in place. */
static int16_t ima_nibble(unsigned n, int32_t *pred, int *idx) {
    const int32_t step = ima_step[*idx];
    int32_t diff = step >> 3;
    if (n & 1u) diff += step >> 2;
    if (n & 2u) diff += step >> 1;
    if (n & 4u) diff += step;
    if (n & 8u) diff = -diff;
    *pred += diff;
    if (*pred > 32767) *pred = 32767;
    if (*pred < -32768) *pred = -32768;
    *idx += ima_index[n];
    if (*idx < 0) *idx = 0;
    if (*idx > 88) *idx = 88;
    return (int16_t)*pred;
}

/* Returns how many samples were written, at most `max`. */
static uint32_t ima_decode(const uint8_t *in, uint32_t bytes, uint32_t block,
                           int16_t *out, uint32_t max) {
    uint32_t off = 0, n = 0;
    if (block < 5u)
        block = 512u;
    while (off + 4u <= bytes && n < max) {
        /* Block header: seed sample, then the step index to start from. A
         * fresh predictor per block is what makes the stream seekable, and
         * also what stops one bad nibble ruining everything after it. */
        int32_t pred = (int16_t)((uint16_t)in[off] | ((uint16_t)in[off + 1u] << 8));
        int idx = in[off + 2u];
        uint32_t end = off + block;
        uint32_t k;
        if (idx > 88) idx = 88;
        if (end > bytes) end = bytes;
        out[n++] = (int16_t)pred;
        for (k = off + 4u; k < end && n + 1u < max; k++) {
            out[n++] = ima_nibble(in[k] & 0x0Fu, &pred, &idx);
            out[n++] = ima_nibble(in[k] >> 4, &pred, &idx);
        }
        off += block;
    }
    return n;
}

/* Mix one buffer's worth of every active voice.
 *
 * int32 accumulator and a clamp at the end rather than dividing each voice
 * down: sixteen simultaneous gunshots would otherwise each be a sixteenth as
 * loud whether or not anything else was playing, and the common case of one or
 * two voices would be inaudible. Clipping when they genuinely all fire at once
 * is the better failure. */
static void mix(int16_t *out) {
    unsigned i, f;
    /* Interleaved L,R. The sample voices are mono and land in both, but music
     * is a stereo stream and would lose its image if everything were summed to
     * one channel first. */
    static int32_t acc[OUT_FRAMES * 2u];

    memset(acc, 0, sizeof acc);
    mutexLock(&g_lock);
    for (i = 0; i < SND_OUT_CHANNELS; i++) {
        Voice *v = &g_ch[i];
        uint64_t pos;
        uint32_t step, vol;
        if (!v->active || v->paused || !v->pcm)
            continue;
        pos = v->pos;
        step = v->step;
        vol = (v->volume * g_master) >> 8;
        for (f = 0; f < OUT_FRAMES; f++) {
            uint64_t idx = pos >> 16;
            int32_t s;
            if (idx >= v->samples) {
                v->active = 0;      /* drained; snd_out_busy stops reporting it */
                g_drained |= 1u << i;
                break;
            }
            /* Linear interpolation between neighbours. 22050 -> 48000 is not
             * an integer ratio, so nearest-neighbour would add a buzz on top
             * of every sound at the resampling error frequency. */
            {
                int32_t a = v->pcm[idx];
                int32_t b = (idx + 1u < v->samples) ? v->pcm[idx + 1u] : a;
                uint32_t frac = pos & 0xFFFFu;
                s = a + (((b - a) * (int32_t)frac) >> 16);
            }
            {
                const int32_t v = (s * (int32_t)vol) >> 8;
                acc[f * 2u + 0u] += v;
                acc[f * 2u + 1u] += v;
            }
            pos += step;
        }
        v->pos = pos;
    }
    mutexUnlock(&g_lock);

    /* Music on top, under its own lock -- it streams from a file and must not
     * hold the sample lock while it decodes. Switchable so it can be taken out
     * of the path without a rebuild, which is the only way to tell a mixer
     * broken BY the music code from one broken beside it. */
    if (g_music_on)
        snd_music_mix(acc, OUT_FRAMES);
    g_mix_calls++;

    for (f = 0; f < OUT_FRAMES * 2u; f++) {
        int32_t s = acc[f];
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        out[f] = (int16_t)s;
    }
}

static void mixer_thread(void *arg) {
    (void)arg;
    while (g_running) {
        AudioOutBuffer *rel = NULL;
        u32 n = 0;
        if (R_FAILED(audoutWaitPlayFinish(&rel, &n, 1000000000ull)))
            continue;                      /* 1 s timeout, then re-check g_running */
        if (!rel)
            continue;
        mix((int16_t *)rel->buffer);
        audoutAppendAudioOutBuffer(rel);
    }
}

int snd_out_init(void) {
    unsigned i;
    if (g_live)
        return 1;
    mutexInit(&g_lock);
    if (R_FAILED(audoutInitialize()))
        return 0;
    if (R_FAILED(audoutStartAudioOut())) {
        audoutExit();
        return 0;
    }
    for (i = 0; i < OUT_BUFS; i++) {
        void *p = memalign(0x1000, OUT_BUFSZ);
        if (!p) {
            audoutStopAudioOut();
            audoutExit();
            return 0;
        }
        memset(p, 0, OUT_BUFSZ);
        g_buf[i].next = NULL;
        g_buf[i].buffer = p;
        g_buf[i].buffer_size = OUT_BUFSZ;
        g_buf[i].data_size = OUT_BUFSZ;
        g_buf[i].data_offset = 0;
        audoutAppendAudioOutBuffer(&g_buf[i]);
    }
    g_running = 1;
    /* Core 2: the guest owns core 0 and the control socket sits on 1. Priority
     * just above the control thread -- a late mixer is an audible gap, while a
     * late LS is nothing -- but still below the guest, which must not stutter
     * to keep sound fed. */
    if (R_FAILED(threadCreate(&g_thread, mixer_thread, NULL, NULL, 0x8000, 0x2E, 2)) ||
        R_FAILED(threadStart(&g_thread))) {
        g_running = 0;
        audoutStopAudioOut();
        audoutExit();
        return 0;
    }
    g_live = 1;
    return 1;
}

void snd_out_exit(void) {
    unsigned i;
    if (!g_live)
        return;
    g_running = 0;
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
    audoutStopAudioOut();
    audoutExit();
    for (i = 0; i < OUT_BUFS; i++) {
        free(g_buf[i].buffer);
        g_buf[i].buffer = NULL;
    }
    for (i = 0; i < SND_OUT_CHANNELS; i++) {
        free(g_ch[i].pcm);
        g_ch[i].pcm = NULL;
    }
    g_live = 0;
}

uint32_t snd_out_play(unsigned ch, const void *data, uint32_t bytes,
                      uint32_t block, uint32_t rate, uint32_t volume) {
    Voice *v;
    uint32_t need, got;
    if (!g_live || ch >= SND_OUT_CHANNELS || !data || !bytes)
        return 0;
    if (!rate)
        rate = 22050u;
    v = &g_ch[ch];
    /* Two samples per byte, plus one seed per block, plus slack. Decoding into
     * our own buffer rather than on the fly keeps the mixer free of codec
     * state and means a voice can be restarted mid-sound without unwinding
     * anything. */
    need = bytes * 2u + (bytes / (block ? block : 512u) + 2u);
    mutexLock(&g_lock);
    if (v->cap < need) {
        int16_t *p = (int16_t *)realloc(v->pcm, need * sizeof(int16_t));
        if (!p) {
            mutexUnlock(&g_lock);
            return 0;
        }
        v->pcm = p;
        v->cap = need;
    }
    got = ima_decode((const uint8_t *)data, bytes, block, v->pcm, need);
    if (!got) {
        mutexUnlock(&g_lock);
        return 0;
    }
    v->samples = got;
    v->pos = 0;
    v->step = (uint32_t)(((uint64_t)rate << 16) / OUT_RATE);
    v->volume = volume ? volume : 256u;
    v->paused = 0;
    v->active = 1;
    /* A drain left over from the sound this one replaces must not be reported
     * against it: that fired the NEW instance's end-of-sample handler. */
    g_drained &= ~(1u << ch);
    mutexUnlock(&g_lock);
    return got;
}

void snd_out_play_pcm(unsigned ch, const int16_t *pcm, uint32_t samples,
                      uint32_t rate, uint32_t volume) {
    Voice *v;
    if (!g_live || ch >= SND_OUT_CHANNELS || !pcm || !samples)
        return;
    if (!rate)
        rate = 22050u;
    v = &g_ch[ch];
    mutexLock(&g_lock);
    if (v->cap < samples) {
        int16_t *p = (int16_t *)realloc(v->pcm, samples * sizeof(int16_t));
        if (!p) {
            mutexUnlock(&g_lock);
            return;
        }
        v->pcm = p;
        v->cap = samples;
    }
    memcpy(v->pcm, pcm, samples * sizeof(int16_t));
    v->samples = samples;
    v->pos = 0;
    v->step = (uint32_t)(((uint64_t)rate << 16) / OUT_RATE);
    v->volume = volume ? volume : 256u;
    v->paused = 0;
    v->active = 1;
    /* A drain left over from the sound this one replaces must not be reported
     * against it: that fired the NEW instance's end-of-sample handler. */
    g_drained &= ~(1u << ch);
    mutexUnlock(&g_lock);
}

void snd_out_stop(unsigned ch) {
    if (!g_live || ch >= SND_OUT_CHANNELS)
        return;
    mutexLock(&g_lock);
    g_ch[ch].active = 0;
    g_drained &= ~(1u << ch);   /* a stopped sound does not also "finish" */
    mutexUnlock(&g_lock);
}

void snd_out_pause(unsigned ch, int paused) {
    if (!g_live || ch >= SND_OUT_CHANNELS)
        return;
    mutexLock(&g_lock);
    g_ch[ch].paused = paused ? 1u : 0u;
    mutexUnlock(&g_lock);
}

void snd_out_set_volume(unsigned ch, uint32_t volume) {
    if (!g_live || ch >= SND_OUT_CHANNELS)
        return;
    mutexLock(&g_lock);
    g_ch[ch].volume = volume;
    mutexUnlock(&g_lock);
}

/* Master sample volume (s3eSoundSetInt property 0), applied on top of each
 * voice's own. The mixer reads it once per voice per buffer; a torn read of
 * one word is harmless, so it takes no lock. */
void snd_out_set_master(uint32_t volume) {
    g_master = volume > 256u ? 256u : volume;
}

/* Samples still to play on a voice, and how long it is in total. Used to spot
 * a sound that is cut off: a Play or Stop arriving while a voice still has
 * most of its audio left is the game (or this port) ending it early. */
void snd_out_progress(unsigned ch, uint32_t *left, uint32_t *total) {
    *left = *total = 0;
    if (!g_live || ch >= SND_OUT_CHANNELS)
        return;
    mutexLock(&g_lock);
    if (g_ch[ch].active) {
        uint32_t done = (uint32_t)(g_ch[ch].pos >> 16);
        *total = g_ch[ch].samples;
        *left = done < g_ch[ch].samples ? g_ch[ch].samples - done : 0;
    }
    mutexUnlock(&g_lock);
}

int snd_out_busy(unsigned ch) {
    int r;
    if (!g_live || ch >= SND_OUT_CHANNELS)
        return 0;
    mutexLock(&g_lock);
    r = g_ch[ch].active && !g_ch[ch].paused;
    mutexUnlock(&g_lock);
    return r;
}

uint32_t snd_out_take_drained(void) {
    uint32_t m;
    if (!g_live)
        return 0;
    mutexLock(&g_lock);
    m = g_drained;
    g_drained = 0;
    mutexUnlock(&g_lock);
    return m;
}

uint64_t snd_out_mix_calls(void) {
    return g_mix_calls;
}

unsigned snd_out_active_voices(void) {
    unsigned i, n = 0;
    if (!g_live)
        return 0;
    mutexLock(&g_lock);
    for (i = 0; i < SND_OUT_CHANNELS; i++)
        if (g_ch[i].active)
            n++;
    mutexUnlock(&g_lock);
    return n;
}

int snd_out_music_enabled(void) {
    return g_music_on;
}

void snd_out_music_enable(int on) {
    g_music_on = on ? 1 : 0;
}
