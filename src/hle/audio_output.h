/*
 * audio_output.h -- host audio output for replaced DirectSound buffers.
 *
 * From doaxbv-re (https://github.com/NoRain211/doaxbv-re,
 * recomp-runtime/audio_output.h), GPL-3.0. One behaviour change: output is on
 * with mix headroom unless RECOMP_AUDIO_GAIN says otherwise (doaxbv-re defaults to
 * muted while it validates its decoders).
 */
#ifndef XBOXRECOMP_AUDIO_OUTPUT_H
#define XBOXRECOMP_AUDIO_OUTPUT_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Transient device failures retry at bounded intervals; explicit mute and
   shutdown stay disabled. RECOMP_AUDIO_GAIN is the host
   master gain (0..1); absent uses the shared default below. Zero opens no device. */
/* -6.02 dB at the output: preserve the game's Music/Sound slider curves and
 * balance, with a quieter 100% level. Explicit host gain overrides stay exact. */
#define RECOMP_AUDIO_DEFAULT_GAIN 0.5f
#define RECOMP_AUDIO_RETRY_MS 1000u
#define RECOMP_AUDIO_SLOT_MOVIE 272u

void recomp_audio_output_initialize(void);
void recomp_audio_output_shutdown(void);
void recomp_audio_output_reset_voice(uint32_t slot);
/* Only 3D voices enable this. It changes routing, never source PCM/cursor units.
 * pan is -1 left, +1 right; disabled restores ordinary mono/stereo output. */
void recomp_audio_output_set_spatial(uint32_t slot, float pan, int enabled);
/* Environmental obstruction (0..1) and bounded early-reflection send. */
void recomp_audio_output_set_acoustics(uint32_t slot,float obstruction,float room);
void recomp_audio_output_set_ambience(uint32_t slot,int enabled);
/* Consume a notification that the host discarded a voice unexpectedly.
   Explicit reset clears it. Call before reading/submitting a continuous
   timeline, then reanchor to its current clock before feeding a new voice. */
int recomp_audio_output_take_discontinuity(uint32_t slot);

/* Copies PCM before returning; never retains a guest-memory pointer.
   Slots: 0..255 for buffers, 256..271 for streams, 272
   (RECOMP_AUDIO_SLOT_MOVIE) for a movie's sound. PCM: 1000..200000 Hz,
   mono/stereo, unsigned 8 or signed 16 bit.
   Each submission is frame-aligned and at most 160000 bytes.

   Returns 1 when the sound was taken and 0 when it was not -- a full queue,
   a device that is gone, PCM it cannot play. A caller feeding a continuous
   stream must not treat 0 as delivered: the sound it describes will never be
   heard, so it has to be offered again rather than stepped over, or the
   stream slips behind by exactly what was dropped. */
int recomp_audio_output_submit(
    uint32_t slot, const uint8_t *pcm, uint32_t bytes,
    uint32_t sample_rate, uint32_t channels, uint32_t bits_per_sample,
    int32_t volume_hundredth_db);

/* Where this slot actually is: bytes of PCM the device has played since the
   voice was created (reset_voice destroys it, so the count restarts at zero
   with the next submission), and how many submissions are still queued.
   Returns 0 when there is no voice, when output is off, or when the count
   cannot be read -- the caller must then fall back to its own clock.

   This is the device's clock, not the wall clock, and it is the one the
   listener hears: a stream whose packets complete on the wall clock drifts
   away from what is coming out of the speakers, by the device's rate error
   and by every gap the queue ever ran dry for. */
int recomp_audio_output_position(uint32_t slot, uint64_t *played_bytes,
                                 uint32_t *queued_buffers);

/* The slot's volume, now, for everything it has queued: a voice whose level
   changes while it plays (a 3D sound moving) follows without waiting for its
   next submission. 0 when there is no voice. */
int recomp_audio_output_set_volume(uint32_t slot, int32_t volume_hundredth_db);

/* What the engine says about itself, for the DirectSound diagnostics:
   latency is XAudio2's CurrentLatencyInSamples (from a source buffer to the
   speakers) at the mastering rate; the create/destroy times are what
   CreateSourceVoice and DestroyVoice cost the calling (game) thread. */
typedef struct RecompAudioEngineStats {
    uint32_t latency_samples, sample_rate, glitches;
    uint32_t active_voices, total_voices;
    uint64_t voices_created, voices_destroyed;
    /* Lifetime counters, even when no device is available. Refused submissions
       are retryable, not necessarily lost sound. Device/voice failures count
       operations; unavailable/retry_wait count refused submission attempts. */
    uint64_t queue_full, invalid_pcm, unavailable, retry_wait, allocation_failed;
    uint64_t voice_failed, device_failed, open_attempts, opens;
    double   create_ms_total, create_ms_max, destroy_ms_total, destroy_ms_max;
} RecompAudioEngineStats;
/* Shared final format for both adapters; lifetime counters survive teardown. */
static inline void recomp_audio_output_log_final_health(const RecompAudioEngineStats *s)
{
    fprintf(stderr, "[audio-output] final health: queue_full=%llu invalid_pcm=%llu "
            "unavailable=%llu retry_wait=%llu allocation_failed=%llu voice_failed=%llu "
            "device_failed=%llu open_attempts=%llu opens=%llu\n",
            (unsigned long long)s->queue_full, (unsigned long long)s->invalid_pcm,
            (unsigned long long)s->unavailable, (unsigned long long)s->retry_wait,
            (unsigned long long)s->allocation_failed, (unsigned long long)s->voice_failed,
            (unsigned long long)s->device_failed, (unsigned long long)s->open_attempts,
            (unsigned long long)s->opens);
}
int recomp_audio_output_engine_stats(RecompAudioEngineStats *stats);

#ifdef __cplusplus
}
#endif

#endif
