#ifndef VISUALISER_AUDIO_H
#define VISUALISER_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VISUALISER_SAMPLE_COUNT 256

typedef struct {
	int rate;
	size_t count;
	int16_t samples[VISUALISER_SAMPLE_COUNT];
} visualiser_audio_frame_t;

void visualiser_audio_set_active(bool active);
void visualiser_audio_set_screen_on(bool on);
void visualiser_audio_reset(void);
uint32_t visualiser_audio_now_ms(void);

// The playback thread snapshots the epoch before a PCM write (0 when idle).
// A transport change can invalidate the write while it is in progress.
unsigned visualiser_audio_epoch(void);
bool visualiser_audio_can_capture(unsigned epoch);
void visualiser_audio_capture(const void *pcm, size_t frames, int channels, int rate, int bits,
							  unsigned epoch, uint32_t written_at, long queued_frames);

// The UI takes the newest frame whose estimated playback time has arrived.
bool visualiser_audio_take(visualiser_audio_frame_t *frame, uint32_t now);

#endif
