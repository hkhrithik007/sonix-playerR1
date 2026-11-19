#ifndef SLEEPTIMER_H
#define SLEEPTIMER_H

#include <stdbool.h>

// One monotonic, one-shot sleep timer for local music, streaming, books,
// podcasts and radio. A source change or pause never resets its deadline.
// Legacy kind parameters are aliases retained for the existing settings pages.
// SLEEPTIMER_COUNT marks unsupported playback (receiver / Game Boy) or pause.
typedef enum {
	SLEEPTIMER_MUSIC = 0,
	SLEEPTIMER_AUDIOBOOK,
	SLEEPTIMER_PODCAST,
	SLEEPTIMER_COUNT,
} sleeptimer_kind_t;

// Load the shared settings, or read a legacy profile without writing at boot.
void sleeptimer_init(void);
bool sleeptimer_enabled(sleeptimer_kind_t kind);
void sleeptimer_set_enabled(sleeptimer_kind_t kind, bool on);
int sleeptimer_minutes(sleeptimer_kind_t kind);
void sleeptimer_set_minutes(sleeptimer_kind_t kind, int minutes);
// Explicit preset selection restarts even when the duration is unchanged.
void sleeptimer_start(int minutes);
void sleeptimer_note_playing(sleeptimer_kind_t kind);
bool sleeptimer_running(sleeptimer_kind_t kind);
bool sleeptimer_any_running(void);
// True once at the deadline; disables the shared timer. Caller pauses playback.
bool sleeptimer_expired(sleeptimer_kind_t kind);
int sleeptimer_remaining(sleeptimer_kind_t kind);
// Thread-safe cancellation stamp for deferred network playback.
unsigned sleeptimer_expiration_serial(void);

#endif /* SLEEPTIMER_H */
