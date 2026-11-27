#include "playcount.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lvgl/lvgl.h"

#include "src/system/audio/audio.h"
#include "src/system/library/library.h"
#include "src/system/listenbrainz/listenbrainz.h"
#include "src/system/playback/audiobook.h"
#include "src/system/playback/device_state.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/radio.h"

#define TICK_MS 1000

// A longer step between two ticks than this is the device having been asleep,
// not listening.
#define STEP_MAX_SECS 5.0

// Back at the start of the same file, after being this far into it: played
// again.
#define AGAIN_NEAR_START_SECS 5.0
#define AGAIN_BACK_SECS 10.0

// A track of at least 30 seconds, listened to for half its length or four
// minutes, whichever comes first.
#define COUNT_MIN_LENGTH 30.0
#define COUNT_ENOUGH 240.0

// 2020-01-01 UTC. A clock before it has not been set since the last power-off.
#define CLOCK_VALID_AFTER 1577836800LL

#define FIELD_MAX 256

// GUI thread only. `file` is "" while no play is under way.
static struct {
	char file[512];
	unsigned generation;
	bool skip;
	bool counted; // already added to the library's play count
	bool was_playing;
	double listened;
	double last_tick;
	double last_position;
	double boot_start;
	long long real_start;
	double duration;
	char artist[FIELD_MAX];
	char title[FIELD_MAX];
	char album[FIELD_MAX];
} play;

static double boot_seconds(void) {
	struct timespec ts;
#ifdef CLOCK_BOOTTIME
	if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0)
#endif
		clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void copy_field(char *dst, const char *src) { snprintf(dst, FIELD_MAX, "%s", src ? src : ""); }

bool playcount_play_counts(double length, double listened) {
	if (length < COUNT_MIN_LENGTH) {
		return false;
	}
	double enough = length / 2 < COUNT_ENOUGH ? length / 2 : COUNT_ENOUGH;
	return listened >= enough;
}

static void end_play(void) {
	if (play.file[0]) {
		play.file[0] = '\0';
		play.generation++;
	}
	play.was_playing = false;
}

static void start_play(const char *file, double position, bool skip, double t) {
	snprintf(play.file, sizeof(play.file), "%s", file);
	play.generation++;
	play.skip = skip;
	play.counted = false;
	play.was_playing = true;
	play.listened = 0;
	play.last_tick = t;
	play.last_position = position;
	play.boot_start = t;
	long long now = (long long)time(NULL);
	play.real_start = now >= CLOCK_VALID_AFTER ? now : 0;
	play.duration = 0;
	play.artist[0] = play.title[0] = play.album[0] = '\0';
}

// The tags can come in after the file has started; they are taken once, when
// the loaded ones are for the file being played.
static void take_tags(void) {
	if (play.title[0]) {
		return;
	}
	const char *tags_file = NULL;
	const song_metadata_t *m = device_state_loaded_metadata(&tags_file);
	if (!m || !tags_file || strcmp(tags_file, play.file) != 0 || !m->title[0]) {
		return;
	}
	copy_field(play.artist, m->artist[0] ? m->artist : m->album_artist);
	copy_field(play.title, m->title);
	copy_field(play.album, m->album);
}

static void tick_cb(lv_timer_t *timer) {
	(void)timer;
	char file[512];
	audio_get_current_file(file, sizeof(file));
	bool playing = audio_get_status() == AUDIO_STATUS_PLAYING;
	double position = 0.0, duration = 0.0;
	audio_get_progress(&position, &duration);
	bool skip = radio_is_active() || audiobook_is_playing() || podcastcache_is_episode(file);

	double t = boot_seconds();
	bool same = play.file[0] && strcmp(play.file, file) == 0;
	bool again = same && position < AGAIN_NEAR_START_SECS && position + AGAIN_BACK_SECS < play.last_position;

	if (playing && file[0] && (!same || again)) {
		start_play(file, position, skip, t);
	} else if (!same) {
		end_play();
	} else {
		if (playing && play.was_playing) {
			double step = t - play.last_tick;
			if (step > 0 && step < STEP_MAX_SECS) {
				play.listened += step;
			}
		}
		play.was_playing = playing;
		play.last_tick = t;
		play.last_position = position;
	}

	bool live = play.file[0] != '\0';
	bool counts = false;
	if (live) {
		if (duration > 0) {
			play.duration = duration;
		}
		take_tags();
		counts = !play.skip && playcount_play_counts(play.duration, play.listened);
		if (counts && !play.counted) {
			play.counted = true;
			library_note_play(play.file);
		}
	}

	bool listenbrainz = listenbrainz_active();
	if (!listenbrainz) {
		return;
	}
	playcount_play_t now = {
		.generation = play.generation,
		.playing = live && playing,
		.skip = live && play.skip,
		.counts = counts,
		.boot_start = play.boot_start,
		.real_start = play.real_start,
		.duration = live ? (int)(play.duration + 0.5) : 0,
		.artist = live ? play.artist : "",
		.title = live ? play.title : "",
		.album = live ? play.album : "",
	};
	if (listenbrainz) {
		listenbrainz_note_play(&now);
	}
}

void playcount_init(void) { lv_timer_create(tick_cb, TICK_MS, NULL); }
