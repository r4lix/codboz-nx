/* audio.h -- sound output for the s3eSound channel API.
 *
 * The game mixes nothing itself: it hands s3eSoundChannelPlay a pointer to
 * 16-bit signed mono PCM and a sample count, sets the rate per channel, and
 * expects the platform to play it. That was established from the sample
 * descriptor the game keeps 48 bytes ahead of every buffer -- channels 1,
 * bytes-per-sample 2, rate 22050, byte length exactly samples*2 -- rather than
 * inferred from the waveform, which looked like noise because the first thing
 * anyone tests with is a gunshot.
 *
 * So this is a mixer: up to SND_OUT_CHANNELS voices resampled from whatever
 * rate the game set to the 48 kHz stereo the console wants.
 *
 * Every entry point is safe to call with audio unavailable; they become no-ops
 * and snd_out_busy() reports nothing playing, which is exactly the
 * working-but-idle sound system the HLE handlers already pretended to be.
 */
#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

#define SND_OUT_CHANNELS 16

/* Bring up audout and start the mixer thread. Returns non-zero on success;
 * failure is not fatal and leaves every call below inert. */
int  snd_out_init(void);
void snd_out_exit(void);

/* Start a voice from `bytes` of 4-bit IMA ADPCM in `block`-byte blocks.
 *
 * Decoded into our own storage here rather than referenced: the game owns that
 * buffer and may reuse it the moment this returns, and chasing a guest pointer
 * from another thread while the guest is running is not a race worth having. */
/* Returns the number of samples decoded, so the caller can compare that with
 * what the game's own sample descriptor promises. */
uint32_t snd_out_play(unsigned ch, const void *data, uint32_t bytes,
                      uint32_t block, uint32_t rate, uint32_t volume);

/* Start a voice from `samples` frames of 16-bit signed mono PCM, copied. This is
 * what s3eSoundChannelPlay really carries when no generator is registered, and
 * what the game's own generator callback produces when one is. */
void snd_out_play_pcm(unsigned ch, const int16_t *pcm, uint32_t samples,
                      uint32_t rate, uint32_t volume);

void snd_out_stop(unsigned ch);
void snd_out_pause(unsigned ch, int paused);
void snd_out_set_volume(unsigned ch, uint32_t volume);

/* Master sample volume, s3eSoundSetInt property 0 (256 = unity). */
void snd_out_set_master(uint32_t volume);

/* Non-zero while the voice still has samples left. This is what the game polls
 * to decide a sound has finished and the channel can be reused, so it has to
 * reflect the mixer rather than the fact that Play was once called. */
int  snd_out_busy(unsigned ch);

/* Samples left and total for a live voice (0,0 when idle). */
void snd_out_progress(unsigned ch, uint32_t *left, uint32_t *total);

/* Bitmask of voices that have finished since the last call, and clears it.
 * Collected on the guest thread, which is the only one allowed to turn this
 * into a guest callback. */
uint32_t snd_out_take_drained(void);

/* ---- streamed music (music.c) ---------------------------------------
 *
 * A single voice, separate from the sample channels: the game plays one
 * track at a time through s3eAudio, and it is an MP3 on the card rather
 * than something handed to us in memory. Mixed into the same accumulator
 * as the samples, in stereo, so a track keeps its own image. */
int  snd_music_play(const char *path, int loop, uint32_t volume);
void snd_music_stop(void);
void snd_music_pause(int paused);
void snd_music_set_volume(uint32_t volume);
int  snd_music_playing(void);
/* s3eAudio STATUS: 0 stopped, 1 playing, 2 paused. */
int  snd_music_status(void);

/* Mixer liveness and state, for the control socket. */
uint64_t snd_out_mix_calls(void);
unsigned snd_out_active_voices(void);
void     snd_out_music_enable(int on);
int      snd_out_music_enabled(void);

/* Called by the mixer with an interleaved L,R accumulator. */
void snd_music_mix(int32_t *acc, unsigned frames);

#endif /* AUDIO_H */
