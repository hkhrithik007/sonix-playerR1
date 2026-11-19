#include "playbackobserver.h"

#include <stdbool.h>
#include <string.h>

#include "lvgl/lvgl.h"
#include "src/system/audio/audio.h"
#include "src/system/playback/device_state.h"
#include "src/system/library/metadata.h"
#include "src/system/library/playcount.h"
#include "src/system/streaming/radio.h"
#include "src/system/playback/audiobook.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/listenbrainz/listenbrainz.h"

static void tick_cb(lv_timer_t *timer) {
	(void)timer;
	char file[512];
	audio_get_current_file(file, sizeof(file));
	bool playing = audio_get_status() == AUDIO_STATUS_PLAYING;
	double position = 0.0, duration = 0.0;
	audio_get_progress(&position, &duration);
	bool skip = radio_is_active() || audiobook_is_playing() || podcastcache_is_episode(file);

	playcount_playback_t counted = {
		.playing = playing, .file = file, .position = position, .duration = duration, .skip = skip};
	playcount_note_playback(&counted);

	const char *tags_file = NULL;
	const song_metadata_t *metadata = device_state_loaded_metadata(&tags_file);
	bool tags = file[0] && tags_file && strcmp(tags_file, file) == 0;
	const char *artist = tags ? (metadata->artist[0] ? metadata->artist : metadata->album_artist) : "";
	const char *title = tags ? metadata->title : "";
	const char *album = tags ? metadata->album : "";

	listenbrainz_playback_t lb = {0};
	lb.file = file;
	lb.playing = playing;
	lb.position = position;
	lb.duration = duration;
	lb.skip = skip;
	lb.artist = artist;
	lb.title = title;
	lb.album = album;
	listenbrainz_note_playback(&lb);
}

void playbackobserver_init(void) {
	static bool started;
	if (started) {
		return;
	}
	started = true;
	lv_timer_create(tick_cb, 1000, NULL);
}
