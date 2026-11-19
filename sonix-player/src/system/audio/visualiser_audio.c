#include "visualiser_audio.h"

#include <stdatomic.h>
#include <time.h>

#define FRAME_QUEUE 64
#define MAX_DELAY_MS 1400

typedef struct {
	visualiser_audio_frame_t pcm;
	unsigned epoch;
	uint32_t audible_at;
} queued_frame_t;

static atomic_bool active;
static atomic_bool screen_on = true;
static atomic_uint generation = 1;
// One producer (playback) and one consumer (LVGL). Each owns its index;
// release/acquire protects the non-atomic payload between them.
static atomic_uint head, tail;
static queued_frame_t queue[FRAME_QUEUE];

void visualiser_audio_reset(void) {
	atomic_fetch_add_explicit(&generation, 1, memory_order_relaxed);
}

void visualiser_audio_set_active(bool on) {
	atomic_store_explicit(&active, on, memory_order_relaxed);
	if (!on) {
		visualiser_audio_reset();
	}
}

void visualiser_audio_set_screen_on(bool on) {
	atomic_store_explicit(&screen_on, on, memory_order_relaxed);
	if (!on) {
		visualiser_audio_reset();
	}
}

uint32_t visualiser_audio_now_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)t.tv_sec * 1000u + (uint32_t)t.tv_nsec / 1000000u;
}

unsigned visualiser_audio_epoch(void) {
	if (!atomic_load_explicit(&active, memory_order_relaxed) ||
		!atomic_load_explicit(&screen_on, memory_order_relaxed)) {
		return 0;
	}
	return atomic_load_explicit(&generation, memory_order_relaxed);
}

bool visualiser_audio_can_capture(unsigned epoch) {
	if (!epoch || visualiser_audio_epoch() != epoch) {
		return false;
	}
	unsigned at = atomic_load_explicit(&head, memory_order_relaxed);
	return (at + 1) % FRAME_QUEUE != atomic_load_explicit(&tail, memory_order_acquire);
}

void visualiser_audio_capture(const void *pcm, size_t frames, int channels, int rate, int bits,
							  unsigned epoch, uint32_t written_at, long queued_frames) {
	if (channels < 1 || rate < 1 || (bits != 16 && bits != 32) || !visualiser_audio_can_capture(epoch)) {
		return;
	}

	// Average groups of frames before reducing the sample rate. The 32-bit
	// playback buffer holds full-scale PCM before conversion to a DAC format.
	int step = rate / 12000;
	step = step > 0 ? step : 1;
	size_t count = frames / (size_t)step;
	if (count > VISUALISER_SAMPLE_COUNT) {
		count = VISUALISER_SAMPLE_COUNT;
	}
	if (count < 16) {
		return;
	}

	unsigned at = atomic_load_explicit(&head, memory_order_relaxed);
	queued_frame_t *sample = &queue[at];
	const int16_t *s16 = pcm;
	const int32_t *s32 = pcm;
	int right = channels > 1 ? 1 : 0;
	for (size_t i = 0; i < count; i++) {
		int sum = 0;
		for (int j = 0; j < step; j++) {
			size_t left = (i * step + j) * channels;
			if (bits == 16) {
				sum += s16[left] + s16[left + right];
			} else {
				sum += (s32[left] >> 16) + (s32[left + right] >> 16);
			}
		}
		sample->pcm.samples[i] = (int16_t)(sum / (2 * step));
	}
	sample->pcm.count = count;
	sample->pcm.rate = rate / step;
	sample->epoch = epoch;

	// ALSA reports the delay of the next frame to be written. Back up from
	// that position to the midpoint of the part sampled from this write.
	int64_t delay = ((int64_t)queued_frames - (int64_t)frames + (int64_t)count * step / 2) * 1000 / rate;
	uint32_t delay_ms = delay <= 0 ? 0 : delay > MAX_DELAY_MS ? MAX_DELAY_MS : (uint32_t)delay;
	sample->audible_at = written_at + delay_ms;
	atomic_store_explicit(&head, (at + 1) % FRAME_QUEUE, memory_order_release);
}

bool visualiser_audio_take(visualiser_audio_frame_t *out, uint32_t now) {
	unsigned at = atomic_load_explicit(&tail, memory_order_relaxed);
	unsigned end = atomic_load_explicit(&head, memory_order_acquire);
	unsigned epoch = atomic_load_explicit(&generation, memory_order_relaxed);
	const queued_frame_t *latest = NULL;
	while (at != end) {
		const queued_frame_t *sample = &queue[at];
		if (sample->epoch == epoch) {
			if ((int32_t)(now - sample->audible_at) < 0) {
				break;
			}
			latest = sample;
		}
		at = (at + 1) % FRAME_QUEUE;
	}
	if (latest) {
		*out = latest->pcm;
	}
	atomic_store_explicit(&tail, at, memory_order_release);
	return latest && epoch == atomic_load_explicit(&generation, memory_order_relaxed);
}
