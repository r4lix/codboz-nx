/* music.c -- streamed MP3 playback for the s3eAudio API.
 *
 * Separate from the sample mixer in audio.c because it is a different problem
 * wearing similar clothes. Effects are short ADPCM blobs the game hands over
 * whole, decoded once into memory; music is a multi-megabyte MP3 on the card
 * that has to be decoded as it plays. A three-minute track at 48 kHz stereo is
 * over thirty megabytes decoded, so it streams.
 *
 * The game names its tracks directly in the binary -- blackops-music/*.mp3 and
 * deadops-music/%s.mp3 -- and calls s3eAudioPlay with those paths. Nothing was
 * ever playing because the files had simply never been extracted from the APK
 * onto the card, not because anything here was missing.
 *
 * Decoding happens on the mixer thread, one MP3 frame at a time, pulled by the
 * resampler as it needs source frames. Reading the file from that thread is
 * safe in a way that reading GUEST memory would not be: this is a host file
 * opened by us, with nothing else touching it.
 */
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_STDIO
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "audio.h"

#define OUT_RATE 48000u
#define IN_CHUNK 16384u

typedef struct {
    FILE       *f;
    mp3dec_t    dec;
    uint8_t     in[IN_CHUNK * 2u];
    size_t      in_n;

    int16_t     frame[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int         frame_n;        /* source frames decoded */
    int         frame_pos;
    int         channels;
    int         rate;

    /* Linear interpolation across a stream: hold the two source frames either
     * side of the current position rather than indexing a buffer, because
     * there is no buffer -- the next frame may not be decoded yet. */
    int16_t     cur[2], nxt[2];
    uint32_t    frac;           /* Q16 position between cur and nxt */
    uint32_t    step;           /* Q16 source frames per output frame */

    uint32_t    volume;         /* 256 is unity, as for samples */
    uint8_t     active;
    uint8_t     paused;
    uint8_t     loop;
    char        path[192];
} Music;

static Music g_m;
static Mutex g_mlock;
static int   g_minit;

static void music_lock(void) {
    if (!g_minit) {
        mutexInit(&g_mlock);
        g_minit = 1;
    }
    mutexLock(&g_mlock);
}

/* Decode one MP3 frame, topping the input buffer up as needed. Returns 0 at
 * end of stream. */
static int decode_more(Music *m) {
    for (;;) {
        mp3dec_frame_info_t info;
        int n;
        if (m->in_n < IN_CHUNK && m->f) {
            size_t got = fread(m->in + m->in_n, 1, sizeof m->in - m->in_n, m->f);
            m->in_n += got;
        }
        if (!m->in_n)
            return 0;
        n = mp3dec_decode_frame(&m->dec, m->in, (int)m->in_n, m->frame, &info);
        if (info.frame_bytes > 0) {
            memmove(m->in, m->in + info.frame_bytes, m->in_n - (size_t)info.frame_bytes);
            m->in_n -= (size_t)info.frame_bytes;
        }
        if (n > 0) {
            m->frame_n = n;
            m->frame_pos = 0;
            m->channels = info.channels > 1 ? 2 : 1;
            if (info.hz > 0 && info.hz != m->rate) {
                m->rate = info.hz;
                m->step = (uint32_t)(((uint64_t)info.hz << 16) / OUT_RATE);
            }
            return 1;
        }
        if (info.frame_bytes == 0)
            return 0;           /* needs more data and there is none */
    }
}

/* One source frame, or 0 at end of stream. */
static int pull(Music *m, int16_t out[2]) {
    const int16_t *p;
    if (m->frame_pos >= m->frame_n && !decode_more(m))
        return 0;
    p = m->frame + (size_t)m->frame_pos * (size_t)m->channels;
    out[0] = p[0];
    out[1] = m->channels > 1 ? p[1] : p[0];
    m->frame_pos++;
    return 1;
}

static void rewind_stream(Music *m) {
    if (!m->f)
        return;
    fseek(m->f, 0, SEEK_SET);
    m->in_n = 0;
    m->frame_n = m->frame_pos = 0;
    mp3dec_init(&m->dec);
    m->cur[0] = m->cur[1] = m->nxt[0] = m->nxt[1] = 0;
    m->frac = 0;
    pull(m, m->cur);
    pull(m, m->nxt);
}

static void close_stream(Music *m) {
    if (m->f)
        fclose(m->f);
    m->f = NULL;
    m->active = 0;
}

void snd_music_mix(int32_t *acc, unsigned frames) {
    Music *m = &g_m;
    unsigned f;
    uint32_t vol;
    music_lock();
    if (!m->active || m->paused) {
        mutexUnlock(&g_mlock);
        return;
    }
    vol = m->volume;
    for (f = 0; f < frames; f++) {
        int32_t l = m->cur[0] + (((int32_t)(m->nxt[0] - m->cur[0]) *
                                  (int32_t)m->frac) >> 16);
        int32_t r = m->cur[1] + (((int32_t)(m->nxt[1] - m->cur[1]) *
                                  (int32_t)m->frac) >> 16);
        acc[f * 2u + 0u] += (l * (int32_t)vol) >> 8;
        acc[f * 2u + 1u] += (r * (int32_t)vol) >> 8;
        m->frac += m->step;
        while (m->frac >= 0x10000u) {
            m->frac -= 0x10000u;
            m->cur[0] = m->nxt[0];
            m->cur[1] = m->nxt[1];
            if (!pull(m, m->nxt)) {
                if (m->loop) {
                    rewind_stream(m);
                } else {
                    close_stream(m);
                    mutexUnlock(&g_mlock);
                    return;
                }
            }
        }
    }
    mutexUnlock(&g_mlock);
}

int snd_music_play(const char *path, int loop, uint32_t volume) {
    Music *m = &g_m;
    FILE *f;
    char full[256];
    if (!path || !*path)
        return 0;
    /* The game asks for "blackops-music/mus_gameover.mp3"; the assets live
     * under the same directory everything else on the card does. */
    snprintf(full, sizeof full, "sdmc:/switch/boz/%s", path);
    f = fopen(full, "rb");
    if (!f) {
        printf("  [mus  ] no such track: %s\n", full);
        return 0;
    }
    music_lock();
    close_stream(m);
    m->f = f;
    snprintf(m->path, sizeof m->path, "%s", path);
    m->volume = volume ? volume : 256u;
    m->loop = loop ? 1u : 0u;
    m->paused = 0;
    m->rate = 0;
    m->step = (uint32_t)(((uint64_t)44100 << 16) / OUT_RATE);  /* until the first frame says */
    rewind_stream(m);
    m->active = 1;
    printf("  [mus  ] playing %s (%d Hz, %d ch, %s)\n", path, m->rate,
           m->channels, m->loop ? "looping" : "once");
    mutexUnlock(&g_mlock);
    return 1;
}

void snd_music_stop(void) {
    music_lock();
    close_stream(&g_m);
    mutexUnlock(&g_mlock);
}

void snd_music_pause(int paused) {
    music_lock();
    g_m.paused = paused ? 1u : 0u;
    mutexUnlock(&g_mlock);
}

void snd_music_set_volume(uint32_t volume) {
    music_lock();
    g_m.volume = volume;
    mutexUnlock(&g_mlock);
}

int snd_music_playing(void) {
    int r;
    music_lock();
    r = g_m.active && !g_m.paused;
    mutexUnlock(&g_mlock);
    return r;
}
