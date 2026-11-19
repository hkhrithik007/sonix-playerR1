#include "sleeptimer.h"

#include <time.h>
#include <stdatomic.h>
#include "src/system/core/config.h"

#define SLEEP_MAX_MINUTES (23 * 60 + 59)

// Legacy kinds remain API aliases so every existing settings page edits one timer.
static bool enabled, counting, playing;
static int minutes;
static long long deadline_ms;
static atomic_uint expiration_serial;

static bool valid(sleeptimer_kind_t kind) { return kind >= 0 && kind < SLEEPTIMER_COUNT; }
static long long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static int clamp(long value) { return value < 0 ? 0 : value > SLEEP_MAX_MINUTES ? SLEEP_MAX_MINUTES : value; }
static void restart(void) {
	counting = enabled && minutes > 0 && playing;
	deadline_ms = counting ? now_ms() + (long long)minutes * 60000 : 0;
}
static void save(void) {
	config_set_int("sleep", "enabled", enabled);
	config_set_int("sleep", "minutes", minutes);
	config_save();
}
void sleeptimer_init(void) {
	long stored = config_get_int("sleep", "enabled", -1);
	enabled = false;
	minutes = 0;
	if (stored >= 0) {
		enabled = stored != 0;
		minutes = clamp(config_get_int("sleep", "minutes", 0));
	} else {
		// Prefer the first armed legacy profile; otherwise remember a useful length.
		const char *sections[] = {"player", "audiobook", "podcast"};
		for (int i = 0; i < 3; i++) {
			int value = clamp(config_get_int(sections[i], "sleep_minutes", 0));
			if (!minutes) minutes = value;
			if (value && config_get_int(sections[i], "sleep_timer", 0)) {
				minutes = value;
				enabled = true;
				break;
			}
		}
	}
	atomic_store_explicit(&expiration_serial, 0, memory_order_relaxed);
	counting = playing = false;
	deadline_ms = 0;
}
bool sleeptimer_enabled(sleeptimer_kind_t kind) { return valid(kind) && enabled; }
int sleeptimer_minutes(sleeptimer_kind_t kind) { return valid(kind) ? minutes : 0; }
void sleeptimer_set_enabled(sleeptimer_kind_t kind, bool on) {
	if (!valid(kind) || enabled == on) return;
	enabled = on;
	restart();
	save();
}
void sleeptimer_set_minutes(sleeptimer_kind_t kind, int value) {
	if (!valid(kind)) return;
	value = clamp(value);
	if (value == minutes) return;
	minutes = value;
	restart();
	save();
}
void sleeptimer_start(int value) {
	value = clamp(value);
	bool changed = minutes != value || enabled != (value > 0);
	minutes = value;
	enabled = value > 0;
	restart();
	if (changed) save();
}
void sleeptimer_note_playing(sleeptimer_kind_t kind) {
	playing = valid(kind);
	// Pause, track and source changes preserve the deadline. Only an explicit
	// new duration restarts it; an armed timer starts at the next playback.
	if (playing && !counting && enabled && minutes > 0) restart();
}
bool sleeptimer_running(sleeptimer_kind_t kind) { return valid(kind) && counting; }
bool sleeptimer_any_running(void) { return counting; }
bool sleeptimer_expired(sleeptimer_kind_t kind) {
	if (!valid(kind) || !counting || now_ms() < deadline_ms) return false;
	atomic_fetch_add_explicit(&expiration_serial, 1, memory_order_relaxed);
	counting = enabled = false;
	deadline_ms = 0;
	save(); // One shot: a later Play must not silently rearm yesterday's timer.
	return true;
}
int sleeptimer_remaining(sleeptimer_kind_t kind) {
	if (!valid(kind) || !counting) return 0;
	long long left = deadline_ms - now_ms();
	return left > 0 ? (int)((left + 999) / 1000) : 0;
}

unsigned sleeptimer_expiration_serial(void) { return atomic_load_explicit(&expiration_serial, memory_order_relaxed); }
