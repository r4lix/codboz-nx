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
void snd_out_play(unsigned ch, const void *data, uint32_t bytes,
                  uint32_t block, uint32_t rate, uint32_t volume);

void snd_out_stop(unsigned ch);
void snd_out_pause(unsigned ch, int paused);
void snd_out_set_volume(unsigned ch, uint32_t volume);

/* Non-zero while the voice still has samples left. This is what the game polls
 * to decide a sound has finished and the channel can be reused, so it has to
 * reflect the mixer rather than the fact that Play was once called. */
int  snd_out_busy(unsigned ch);

/* Bitmask of voices that have finished since the last call, and clears it.
 * Collected on the guest thread, which is the only one allowed to turn this
 * into a guest callback. */
uint32_t snd_out_take_drained(void);

#endif /* AUDIO_H */
