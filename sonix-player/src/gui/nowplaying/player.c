#include "player.h"
#include "src/gui/settings/screensaver.h"
#include "src/gui/shell/artview.h"
#include "src/gui/shell/bookresume.h"
#include "src/gui/shell/playbackplace.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/library/audiobookextras.h"
#include "src/gui/library/browser.h"
#include "src/gui/library/medialist.h"
#include "src/gui/library/searchall.h"
#include "src/gui/nowplaying/btline.h"
#include "src/gui/nowplaying/chapters.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/nowplaying/historypage.h"
#include "src/gui/nowplaying/lyricsearch.h"
#include "src/gui/nowplaying/npicons.h"
#include "src/gui/nowplaying/queueundo.h"
#include "src/gui/nowplaying/trackmenu.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/main_menu.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/quickpanel.h"
#include "src/gui/shell/scrolltext.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/sx_chain.h"
#include "src/gui/shell/sx_mini.h"
#include "src/gui/shell/sx_palette.h"
#include "src/gui/shell/sx_row.h"
#include "src/gui/shell/sx_sheet.h"
#include "src/gui/shell/sx_styles.h"
#include "src/gui/shell/sx_toast.h"
#include "src/gui/shell/sx_tokens.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/topbar.h"
#include "src/gui/shell/touch.h"
#include "src/system/audio/audio.h"
#include "src/system/audio/eq.h"
#include "src/system/audio/readahead.h"
#include "src/system/audio/replaygain.h"
#include "src/system/audio/usbaudio.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/bluetooth/btreceiver.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/device/led.h"
#include "src/system/device/perfstat.h"
#include "src/system/device/power.h"
#include "src/system/gearboy/gearboy.h"
#include "src/system/library/covertone.h"
#include "src/system/library/covertonefan.h"
#include "src/system/library/library.h"
#include "src/system/library/lrclib.h"
#include "src/system/library/lyrics.h"
#include "src/system/net/wifi.h"
#include "src/system/playback/audiobook.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playhistory.h"
#include "src/system/playback/playlist.h"
#include "src/system/playback/sleeptimer.h"
#include "src/system/remote/airplay.h"
#include "src/system/remote/dlna.h"
#include "src/system/streaming/podcast.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/podcaststate.h"
#include "src/system/streaming/podcastsubs.h"
#include "src/system/streaming/provider.h"
#include "src/system/streaming/qobuzcache.h"
#include "src/system/streaming/qobuzsync.h"
#include "src/system/streaming/radio.h"
#include "src/system/streaming/spotify.h"
#include "src/system/streaming/tidalcache.h"
#include "src/system/streaming/tidalsync.h"

#include "lvgl/lvgl.h"
#include "lvgl/src/core/lv_obj_event_private.h"
#include "lvgl/src/misc/lv_area_private.h"

lv_obj_t *player_screen;

static lv_obj_t *player_menu;
static lv_obj_t *play_btn;
static lv_obj_t *play_btn_icon;
static lv_obj_t *song_title_label;
static lv_obj_t *song_artist_label;
static lv_obj_t *format_label; // "16/44.1 FLAC", right of the artist line
static lv_obj_t *fav_btn_obj;  // the button it sits on, hidden on a book
static lv_obj_t *controls_row;
static lv_obj_t *fav_btn_icon; // the star above it

static void update_fav_button(void);
static void update_speed_button(void);
static void update_format_label(const device_state_t *state);
static void update_repeat_button(void);

// Sonix UI 2.0 (see the section of that name further down).
static int np_panel_h;			  // SX_NP_PANEL_H: the panel at the bottom
static lv_obj_t *cover_thumb_img; // the album's 240 px thumbnail, stretched, until the picture lands
static void cover_gesture_cb(lv_event_t *e);
static void np_chain_build(lv_obj_t *parent);
static void np_chain_refresh(const device_state_t *state);
static void np_file_short(const device_state_t *state, char *out, size_t size, bool compact);
static void np_chain_compose(const device_state_t *state, char *out, size_t size, bool *warn);
static void np_upnext_build(void);
static void upnext_update(void);
static void menu_style_for_layout(void);
static void np_paint_backgrounds(void);
static void chain_set_open(bool open);
static bool chain_is_open(void);
static void np_album_changed(const char *path, bool live);
static void np_thumb240_request(void);
static void np_thumb240_drop(void);
static void np_outside_watch(const device_state_t *state);
static void np_poll(const device_state_t *state);
static void np_layout_changed(void);
static void np_mini_from_cover(void);
static void mode_toast(playback_mode_t mode);
static bool np_outside(void);

static lv_color_t chrome_accent(void);
static lv_obj_t *progress_slider;
static lv_obj_t *elapsed_label;	  // start of the track, under the left end of the bar
static lv_obj_t *remaining_label; // end of the track, under the right end
// Where this track sits in the queue, between the two clocks: the row under the
// bar had an empty middle and the question -- how much of this is left -- is the
// same one the clocks answer.
static lv_obj_t *queue_position_label;
static lv_obj_t *repeat_btn_icon;
static lv_obj_t *prev_icon; // static so the over-cover recolor can reach them
static lv_obj_t *next_icon;
static lv_timer_t *progress_slider_timer;

// Runs only while a sleep timer is counting, which is the only time there is
// anything to ask. Paused otherwise, so an option nobody switched on does not
// wake the main loop once a second all night.
static lv_timer_t *sleep_timer;

// Where playback was when the poll last asked, and when that was. The tick is
// lv_tick_get()'s, which is the same clock the timer is driven by.
static lv_timer_t *smooth_timer;
static bool sheet_open; // the player panel is the page on show
static bool sheet_pull_active, sheet_side_active, sheet_side_close;
static int sheet_pull_origin;

static bool bar_always;
static int bar_height; // the status bar's, from the config
static void bar_over_player_update(void);
static double progress_anchor_secs;
static uint32_t progress_anchor_tick;
static bool progress_running;

// The smoothing runs only while there is a bar moving on a sheet that can be
// seen: sixteen wake-ups a second for a panel that is closed or dark are pure
// drain. Asked again by every poll, which is how it comes back after a wake,
// and at every opening and closing of the sheet.
static void smooth_timer_sync(void) {
	if (!smooth_timer) {
		return;
	}
	if (progress_running && sheet_open && power_screen_is_on()) {
		lv_timer_resume(smooth_timer);
	} else {
		lv_timer_pause(smooth_timer);
	}
}

// How often the UI reconciles itself against the playback thread. It polls
// slowly rather than not at all while stopped: the audio thread can report
// STOPPED for a moment in the middle of a track change, and a UI that stopped
// polling on that reading would stay frozen until the next touch.
#define POLL_PERIOD_PLAYING_MS 500
#define POLL_PERIOD_IDLE_MS 2000
#define POLL_PERIOD_DARK_PLAYING_MS 2000 // the panel off (apply_playback_status)
#define POLL_PERIOD_DARK_IDLE_MS 10000

// How far past their 56 px the buttons at the two ends of the transport row
// take a tap. Upwards and downwards the 84 px row still clips it; sideways the
// previous and next buttons start about 50 px further in, so 20 reaches
// neither.
#define CORNER_BTN_EXT_CLICK 6

// The repeat glyph while repeat is off: the handoff's 45 %.
#define NP_REPEAT_OFF_OPA 115

#define SMOOTH_PERIOD_MS 60

// How many fast polls to keep running after playback stops, so the queue gets
// its chance to start the next track before the timer relaxes.
#define POLLS_FAST_AFTER_STOP 4

static int polls_since_stop = POLLS_FAST_AFTER_STOP;

// Album art: an image on top of a placeholder panel. The panel is always
// there, so the layout doesn't jump between a track that has a cover and one
// that doesn't -- the image is simply hidden in the latter case.
static lv_obj_t *cover_panel;
static lv_obj_t *cover_img;
static lv_obj_t *cover_placeholder_icon;

// Live mode (internet radio). A stream is not a track: it cannot be paused,
// skipped or seeked, and it has no position and no length. Rather than leave
// controls on screen that would do nothing, the ones that make no sense are
// hidden and the play button becomes a stop button. These are kept so
// apply_live_mode() can reach them.
static lv_obj_t *prev_btn_obj;
static lv_obj_t *next_btn_obj;
static lv_obj_t *more_btn_obj;
static lv_obj_t *more_btn_icon; // ellipsis on a track, chapters on a book
static lv_obj_t *repeat_btn_obj;
static lv_obj_t *speed_btn_obj; // stands in the repeat button's place on a book
static lv_obj_t *speed_btn_icon;
static lv_obj_t *below_slider_obj; // the two clocks under the bar
// How far the four source marks keep from the corner of the artwork they
// share. One number, because they replace each other in that corner and a
// difference between them would read as the mark jumping.
#define BADGE_INSET 14

// Where the marks sit over the artwork: in its corner, under the status bar
// when the bar stays up.
//
// 2.0: under the "Up next" chip, which took the corner (badge_y()).
static int badge_y(void);
static int badge_top(void) { return badge_y(); }

static lv_obj_t *live_badge;	// live indicator, top right over the artwork
static lv_obj_t *service_badge; // the streaming service's mark (Qobuz, Tidal...), same corner
static lv_obj_t *podcast_badge; // and the podcast one, third in the same place
static bool live_mode;
static bool live_transport_hidden; // prev/next taken away on a stream
static bool live_custom_nav;	   // on a station from a list, where they change station

// Audiobook mode. A book is one long file, not a queue of songs: previous and
// next have nothing to move to, and what a listener wants from those two
// buttons is to go back ten seconds because they missed a sentence. So the
// transport becomes a pair of ten-second jumps, the overflow menu becomes the
// chapter list, and the star and the repeat mode -- both of which belong to
// the music library and its queue -- go away rather than sit there doing
// something invisible to a book.
static bool audiobook_mode;
// A podcast is not an audiobook, but the two skip buttons are the same. See
// apply_audiobook_mode(): the rest of the book dressing -- chapters, speed,
// book icon -- stays with books only.
static bool podcast_mode;
static bool audiobook_has_chapters;		 // whether the loaded book is marked up
static int audiobook_skip_shown_back;	 // the two jump sizes the buttons are drawn
static int audiobook_skip_shown_forward; // for, so changing the setting repaints
static bool audiobook_mode_valid;		 // false until the first apply, so it paints once

static cover_image_t current_cover;	   // pixels currently referenced by cover_img
static cover_image_t current_backdrop; // upside-down blurred copy behind the controls
static lv_obj_t *cover_art;
static int cover_box_w, cover_box_h; // the artwork spans the full screen width

// The progress bar: a thin track, and a knob that is a white ring around a
// smaller circle of the accent colour. The ring is a wide white border over an
// accent-coloured body rather than a second object, so it costs nothing and
// follows the knob on its own.
#define PROGRESS_TRACK_HEIGHT 6 // as thick as every other slider
#define PROGRESS_KNOB_GROW 7	// how far the knob grows past the track
#define PROGRESS_TOUCH_REACH 18 // how far past the track a press still seeks
#define PROGRESS_KNOB_RING 0

#define FAV_BTN_W 48
#define FAV_BTN_H 44

static uint32_t album_tone; // the sleeve's colour, 0 when there is no sleeve
static bool cover_accent;

static lv_obj_t *song_text_obj; // the title and artist, wherever they live now
static lv_obj_t *song_side_obj; // the star and the format, likewise
static void align_title_with_star(void);
static int backdrop_w, backdrop_h;

static double current_total_length = 0; // cached from the last device_state snapshot, so slider math works between polls
static char progress_label_text[32];

// The stretch of the file the progress bar and its two clocks stand for. The
// whole file, except on a book with chapters when Audiobooks -> Show duration
// is set to the chapter: then only the chapter being listened to, so the bar
// starts empty at every chapter and the right clock is the chapter's length.
// `view_length` is 0 while the length of the file is not known.
static double view_start;
static double view_length;

// True while the controls sit on a loaded cover's dark backdrop. The play
// button wears its white disc there (it has to stand out against artwork);
// on the plain no-cover panel it is flat, like prev and next.

// While a station connects, stop, previous and next are greyed out and do
// nothing: the connection is on its way, and pulling it down or starting the
// next one in the middle of it is a tap that only makes the wait longer. The
// side keys and the control centre ask radio_is_connecting() themselves.
static bool connecting_shown;

static void apply_connecting(void) {
	bool connecting = radio_is_connecting();
	if (connecting == connecting_shown) {
		return;
	}
	connecting_shown = connecting;
	lv_obj_t *const buttons[] = {play_btn, prev_btn_obj, next_btn_obj};
	for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
		if (!buttons[i]) {
			continue;
		}
		if (connecting) {
			lv_obj_add_state(buttons[i], LV_STATE_DISABLED);
		} else {
			lv_obj_remove_state(buttons[i], LV_STATE_DISABLED);
		}
	}
}

// Switches the transport between a track and a live stream. Cheap and
// idempotent: called from every now-playing refresh, and does nothing at all
// unless the mode actually changed.
static void apply_live_mode(bool live) {
	// Previous and next go away on a stream, playing or stopped: they would
	// start a local track in place of the station. On a station started from a
	// list -- radio.txt, a country, a search, the starred -- they stay and move
	// along that list, and the bearing under the bar says where in it the
	// station is (see update_queue_position()).
	bool custom_nav = live && radio_can_step();
	// 2.0 (the handoff's `ext`): a lone station plays beside the queue, and
	// previous and next go back to it -- so they stay while there is a queue
	// to go back to (player_screen_next/prev, live_step()).
	bool transport_hidden = live && !custom_nav && playlist_count() <= 0;

	if (live == live_mode && transport_hidden == live_transport_hidden && custom_nav == live_custom_nav && cover_placeholder_icon) {
		return;
	}
	live_mode = live;
	live_transport_hidden = transport_hidden;
	live_custom_nav = custom_nav;

	// A stream has no queue to look at, no length to scrub and no repeat mode
	// that means anything.
	//
	// Repeat/shuffle goes for a reason worth stating: it is not merely useless
	// on a stream, it is misleading. The mode it cycles belongs to the track
	// queue, so a tap there would silently change what happens to the music
	// the radio interrupted.
	//
	// 2.0: the bar stays, full and deaf to the finger, with "Live" where the
	// clock was (the handoff: no seeking on a live stream, "Прямой эфир"
	// instead of the time) -- the panel keeps its shape, and the menu stays
	// for the sleep timer.
	lv_obj_t *const hidden_when_live[] = {repeat_btn_obj};
	for (size_t i = 0; i < sizeof(hidden_when_live) / sizeof(hidden_when_live[0]); i++) {
		if (!hidden_when_live[i]) {
			continue;
		}
		if (live) {
			lv_obj_set_hidden(hidden_when_live[i], true);
		} else {
			lv_obj_set_hidden(hidden_when_live[i], false);
		}
	}
	if (progress_slider) {
		if (live) {
			lv_obj_set_clickable(progress_slider, false);
			lv_slider_set_value(progress_slider, 1000, LV_ANIM_OFF);
		} else {
			lv_obj_set_clickable(progress_slider, true);
		}
	}

	lv_obj_t *const transport[] = {prev_btn_obj, next_btn_obj};
	for (size_t i = 0; i < sizeof(transport) / sizeof(transport[0]); i++) {
		if (!transport[i]) {
			continue;
		}
		if (transport_hidden) {
			lv_obj_set_hidden(transport[i], true);
		} else {
			lv_obj_set_hidden(transport[i], false);
		}
	}

	if (live_badge) {
		if (live) {
			lv_obj_set_hidden(live_badge, false);
			lv_obj_move_foreground(live_badge);
		} else {
			lv_obj_set_hidden(live_badge, true);
		}
	}
	// The marks share one corner and can never go there together: a live
	// stream does not come from Qobuz.
	if (service_badge && live) {
		lv_obj_set_hidden(service_badge, true);
	}
	if (podcast_badge && live) {
		lv_obj_set_hidden(podcast_badge, true);
	}

	// A note means "track", a book means a book, and a station with no artwork
	// gets the radio. Coming back off a stream has to restore whichever of the
	// first two is right, not assume the note.
	if (cover_placeholder_icon) {
		lv_image_set_src(cover_placeholder_icon, live ? &icon_radio_player : (audiobook_mode ? &icon_book_headphones : &icon_music_note));
	}
}

// Dresses the player for a book or for a track. Cheap and idempotent, like
// apply_live_mode(): called from every now-playing refresh and from the poll,
// and does nothing at all unless the answer changed.
static void apply_audiobook_mode(bool book, bool podcast) {
	// Whether this book is marked up decides what the chapter button does, not
	// whether it is there. A folder book's files are its chapters.
	bool chapters = book && (audiobook_chapter_count() > 0 || audiobook_part_count() > 1);

	// Books and podcasts both get the second-jump buttons, but not the same
	// pair of sizes: a book's forward jump steps over a pause, a podcast's
	// over a half-minute sponsor read. They are two separate settings on two
	// separate pages (podcast settings, and Audiobooks -> change controls).
	//
	// Speed carries over too, as a setting of its own. Nothing else does: a
	// podcast has no chapters, is not a single file, and sits in a real queue
	// of episodes. Chapters and the book icon stay with books.
	bool skips = book || podcast;
	int back = podcast && !book ? podcast_skip_back() : audiobook_skip_back();
	int forward = podcast && !book ? podcast_skip_forward() : audiobook_skip_forward();

	if (audiobook_mode_valid && book == audiobook_mode && podcast == podcast_mode && chapters == audiobook_has_chapters && back == audiobook_skip_shown_back && forward == audiobook_skip_shown_forward) {
		return;
	}
	audiobook_mode = book;
	podcast_mode = podcast;
	audiobook_has_chapters = chapters;
	audiobook_skip_shown_back = back;
	audiobook_skip_shown_forward = forward;
	audiobook_mode_valid = true;

	if (prev_icon) {
		lv_image_set_src(prev_icon, !skips ? &icon_skip_back : back == AUDIOBOOK_SKIP_HUGE ? &icon_prev_60 : back == AUDIOBOOK_SKIP_LONG ? &icon_prev_30 : &icon_prev_10);
	}
	if (next_icon) {
		lv_image_set_src(next_icon, !skips ? &icon_skip_forward : forward == AUDIOBOOK_SKIP_HUGE ? &icon_next_60 : forward == AUDIOBOOK_SKIP_LONG ? &icon_next_30 : &icon_next_10);
	}
	// A book has a menu of its own behind the usual glyph: chapters,
	// bookmarks, summary. On a podcast the same button goes straight to the
	// episodes, the only thing wanted from it, rather than to a menu whose one
	// useful entry would always be picked.
	if (more_btn_icon) {
		lv_image_set_src(more_btn_icon, podcast && !book ? &icon_podcast_episodes : &icon_ellipsis_vertical);
	}

	// The star and the repeat mode are about the music library and its queue.
	// On a book they are not merely useless: starring would write a row into
	// the music favourites for a file the music library does not index, and
	// the repeat mode would silently change what happens to the queue the book
	// interrupted.
	//
	// On a podcast the star stays but means something else: it follows the
	// podcast, putting it under the subscribed feeds -- the same promise the
	// star makes everywhere, "find this again without searching". Repeat still
	// goes: a queue of episodes is not an album, and looping the last twenty
	// instalments is nothing anybody asks for.
	if (fav_btn_obj) {
		update_fav_button();
	}
	if (repeat_btn_obj) {
		if (book || podcast) {
			lv_obj_set_hidden(repeat_btn_obj, true);
		} else {
			lv_obj_set_hidden(repeat_btn_obj, false);
		}
	}

	if (speed_btn_obj) {
		if (book || podcast) {
			lv_obj_set_hidden(speed_btn_obj, false);
			update_speed_button();
		} else {
			lv_obj_set_hidden(speed_btn_obj, true);
		}
	}

	// A book with no artwork gets the book and an episode the microphone, not
	// a musical note -- each the glyph its own page uses, drawn at the size
	// this one is, which is why the podcast row's 40 px copy is not the one
	// named here. Only touched when a station is not up: live mode owns this
	// icon while it is on.
	if (cover_placeholder_icon && !live_mode) {
		const lv_image_dsc_t *mark = book ? &icon_book_headphones : podcast ? &icon_podcast_cover : &icon_music_note;
		lv_image_set_src(cover_placeholder_icon, mark);
	}
}

// ---------------------------------------------------------------------------
// Stopping on its own: at the end of a chapter, or after a while
//
// Both live here, in the poll that already runs twice a second, because both
// are questions about where playback has got to -- and neither wants a timer
// of its own that could fire while the book is paused, or after it has been
// swapped for a song.
// ---------------------------------------------------------------------------

// Where the chapter being listened to ends, or 0 when the book has no chapters
// or is on its last one. Worked out per poll rather than remembered, so a jump
// to another chapter is picked up without anything having to be told.
static double current_chapter_end(double position) {
	int index = audiobook_chapter_at(position);
	if (index < 0) {
		return 0;
	}
	double start = 0;
	if (!audiobook_chapter(index + 1, NULL, 0, &start)) {
		return 0; // the last chapter ends where the file does
	}
	return start;
}

static void pause_book_at(double seconds) {
	device_state_seek(seconds);
	if (audio_get_status() == AUDIO_STATUS_PLAYING) {
		audiobook_suppress_rewind_once();
		device_state_toggle_play_pause();
	}
	// Written through: stopping on purpose is exactly the moment somebody puts
	// the player down.
	audiobook_note_position(seconds, 0, true);
}

// Runs on every poll while a book is loaded.
//
// Only the chapter boundary: the sleep timers live in sleeptimer.c, where all
// three behave the same way.
static bool audiobook_auto_stop(const device_state_t *state) {
	if (state->status != AUDIO_STATUS_PLAYING || !audiobook_stop_at_chapter_end()) {
		return false;
	}
	double end = current_chapter_end(state->progress_current_secs);
	if (end > 0 && state->progress_current_secs >= end - 0.5) {
		pause_book_at(end);
		audiobook_set_stop_at_chapter_end(false);
		config_save();
		return true;
	}
	return false;
}

// The jump both the on-screen buttons and the side keys make. Clamped at both
// ends: asking for -10 s in the first second means the beginning, and running
// off the end of the file would stop the book instead of moving in it.
static void audiobook_seek_by(double delta, const device_state_t *state) {
	double target = state->progress_current_secs + delta;
	if (target < 0) {
		target = 0;
	}
	double length = state->progress_total_secs > 0 ? state->progress_total_secs : current_total_length;
	if (length > 1.0 && target > length - 1.0) {
		target = length - 1.0;
	}

	device_state_seek(target);
	// Written through immediately: a jump is a deliberate move, and the ten
	// second throttle would lose it if the player were switched off next.
	audiobook_note_position(target, length, true);
}

// Applies a playback status to the play/pause button and the progress timer.
// Called both optimistically (right after a button press, before the audio
// thread has caught up) and from update_progress() every poll, which is what
// keeps the UI from going stale if the track stops on its own.
// Which sleep timer, if any, owns what is playing.
//
// Books and podcasts have their own because they are listened to differently: a
// stretch that is right for falling asleep to music is not the one somebody
// chose for a chapter, and a podcast timer set at bedtime should not be counting
// down over an album the next morning. Everything else the transport plays --
// the card, Tidal, Qobuz -- is one thing and gets one timer.
//
// Three sources are none of them, and for the same reason: they do not go
// through the player's transport at all. The radio, the Game Boy and the
// Bluetooth receiver push their sound out through the external path, and there
// is no pause to give them -- stopping a station is a disconnection, stopping
// the emulator is stopping the game, and stopping the receiver is silence in a
// room where somebody else's phone is still playing. Only the radio ever
// reports playing here; the other two are written down so that it stays true if
// they ever start to.
//
// 2.0: a station counts with the music. The player's menu offers the sleep
// timer on a station too (the handoff), and falling asleep to the radio is
// the commonest use of one; stopping it is the stop the button makes
// (sleep_timer_cb). The Game Boy and the receiver stay outside.
static sleeptimer_kind_t sleeptimer_kind_playing(void) {
	if (radio_is_active()) {
		return SLEEPTIMER_MUSIC;
	}
	if (gearboy_running() || btreceiver_is_active()) {
		return SLEEPTIMER_COUNT;
	}
	if (audiobook_is_playing()) {
		return SLEEPTIMER_AUDIOBOOK;
	}
	char path[512];
	audio_get_current_file(path, sizeof(path));
	if (podcastcache_is_episode(path)) {
		return SLEEPTIMER_PODCAST;
	}
	return SLEEPTIMER_MUSIC;
}

static void apply_playback_status(audio_status_t status) {
	bool playing = (status == AUDIO_STATUS_PLAYING);

	// The glyph is the accent colour, always: play and pause are the same
	// control in two states. Only the disc under it changes (see
	// set_over_cover): white over artwork and in the dark theme, dark in the
	// light theme when there is no cover to stand out against.
	//
	// On a live stream there is no pause -- stopping and starting again does
	// not resume, it reconnects -- so the button says stop while it is on.
	// Stopped, it goes back to play: the station stays loaded, and pressing
	// it opens a fresh connection to the same one.
	//
	// This runs from every poll, twice a second: written only when it changes.
	sx_set_src(play_btn_icon, live_mode ? (playing ? &icon_stop : &icon_play) : (playing ? &icon_pause : &icon_play));
	// 2.0: the glyph on the white disc is the album's dark ink (np_ink).
	sx_set_recolor(play_btn_icon, sx_pal()->mini_on_action);

	// Keep polling either way, just less often when nothing is playing. The
	// timer must never be paused here: a track change is briefly reported as
	// STOPPED, and a timer paused on that reading leaves nothing running to
	// correct the progress bar and the play/pause icon.
	//
	// Dropping straight to the slow period on STOPPED is wrong too: the end of
	// a track is reported as STOPPED and the queue only advances on the next
	// poll, so the gap between two tracks would become the slow period, seconds
	// of silence reading as a player that stopped instead of going on. Stay on
	// the fast period for a moment after playback stops, which is exactly the
	// window the handover needs.
	if (playing) {
		polls_since_stop = 0;
	} else if (polls_since_stop < POLLS_FAST_AFTER_STOP) {
		polls_since_stop++;
	}

	bool settling = polls_since_stop < POLLS_FAST_AFTER_STOP;
	uint32_t period = (playing || settling) ? POLL_PERIOD_PLAYING_MS : POLL_PERIOD_IDLE_MS;
	// 2.0, the battery: with the panel dark there is nothing to draw and the
	// end of a track is pushed (track_end_hook), so the poll only has the
	// card's bookkeeping left -- read-ahead, "remember track", a book's
	// position -- which a slower beat serves as well. Paused in the dark, it
	// barely wakes at all. power.c runs it at once when the panel comes back.
	if (!power_screen_is_on()) {
		period = (playing || settling) ? POLL_PERIOD_DARK_PLAYING_MS : POLL_PERIOD_DARK_IDLE_MS;
	}
	lv_timer_set_period(progress_slider_timer, period);
	lv_timer_resume(progress_slider_timer);

	// The status bar shows the same state, and told here rather than left to
	// wait for its own five-second poll; so does the mini player.
	topbar_refresh_playback();
	sx_mini_set_playing(playing);
	sx_eq_set_playing(playing);

	// Every route into and out of playing comes through here -- the button, the
	// side keys, the end of a track, a remote, the poll putting the screen back
	// in step with the engine -- so it is the one place the sleep timers can be
	// told without being told twice. It is also the one place that knows what
	// is playing well enough to say which of the three it belongs to.
	sleeptimer_note_playing(playing ? sleeptimer_kind_playing() : SLEEPTIMER_COUNT);
	if (sleep_timer) {
		if (sleeptimer_any_running()) {
			lv_timer_resume(sleep_timer);
		} else {
			lv_timer_pause(sleep_timer);
		}
	}
}

// "3/24": which track of the queue is playing.
//
// It reads the queue itself, so it says the same thing whatever filled it --
// tracks off the card, a podcast's episodes, a Tidal or Qobuz album.
//
// This counter and the queue page use the same actual playback order.
static int shown_queue_position(void) { return playlist_current_index() + 1; }

bool player_queue_position(int *position, int *count) {
	*position = 0;
	*count = 0;
	if (live_mode) {
		int index;
		int total;
		if (live_custom_nav && radio_list_position(&index, &total)) {
			*position = index + 1;
			*count = total;
			return true;
		}
		return false;
	}
	int total = playlist_count();
	int index = playlist_current_index();
	if (total <= 1 || index < 0 || audiobook_mode || dlna_owns_playback()) {
		return false;
	}
	*position = shown_queue_position();
	*count = total;
	return true;
}

static void update_queue_position(void) {
	if (!queue_position_label) {
		return;
	}

	if (live_mode) {
		int index;
		int count;
		if (live_custom_nav && radio_list_position(&index, &count)) {
			char text[32];
			snprintf(text, sizeof(text), "%d/%d", index + 1, count);
			sx_set_text(queue_position_label, text);
		} else {
			// 2.0: a lone station says it is on air where a track says its place.
			sx_set_text(queue_position_label, tr("np_on_air"));
		}
		return;
	}

	int count = playlist_count();
	int index = playlist_current_index();
	bool worth_saying = count > 1 && index >= 0 && !audiobook_mode && !dlna_owns_playback();

	// From every poll: set only when it changes (sx_set_text).
	if (!worth_saying) {
		sx_set_text(queue_position_label, "");
		return;
	}
	char text[32];
	snprintf(text, sizeof(text), "%d/%d", shown_queue_position(), count);
	sx_set_text(queue_position_label, text);
}

// Works out view_start and view_length for playback at `position` in a file
// `total` seconds long. Asked on every poll rather than at a chapter change:
// a seek, a skip or the book simply playing on moves into another chapter, and
// nothing has to be told.
static void view_update(double position, double total) {
	view_start = 0;
	view_length = total > 0 ? total : 0;
	if (view_length <= 0 || !audiobook_mode || !audiobook_has_chapters || !audiobook_duration_per_chapter()) {
		return;
	}
	int index = audiobook_chapter_at(position);
	double start = 0;
	if (index < 0 || !audiobook_chapter(index, NULL, 0, &start)) {
		return;
	}
	double end = total;
	if (!audiobook_chapter(index + 1, NULL, 0, &end) || end > total) {
		end = total; // the last chapter ends where the file does
	}
	if (start < 0 || end - start < 1.0) {
		return; // a mark with nothing after it: the whole file is the better answer
	}
	view_start = start;
	view_length = end - start;
}

// Where `position` sits along the bar, 0 to 1000, the slider's range.
static int view_value(double position) {
	if (view_length <= 0) {
		return 0;
	}
	double fraction = (position - view_start) / view_length;
	if (fraction < 0) {
		fraction = 0;
	}
	if (fraction > 1) {
		fraction = 1;
	}
	return (int)(fraction * 1000);
}

// The other way round: the second of the file that slider value `value` is.
static double view_seconds(int value) { return view_start + view_length * value / 1000.0; }

bool player_mini_seek_valid(const player_mini_seek_t *preview) { return preview && power_screen_is_on() && preview->revision == device_state_metadata_revision(); }

bool player_mini_seek_begin(player_mini_seek_t *preview) {
	if (!preview || !power_screen_is_on())
		return false;
	device_state_t state;
	device_state_get(&state);
	if (state.live || !state.current_file[0] || state.status == AUDIO_STATUS_STOPPED)
		return false;
	double total = state.progress_total_secs;
	if (!isfinite(total) || !isfinite(state.progress_current_secs) || total <= 1 || total > UINT32_MAX)
		return false;
	view_update(state.progress_current_secs, total);
	if (!isfinite(view_start) || !isfinite(view_length) || view_length <= 1)
		return false;
	*preview = (player_mini_seek_t){.position = state.progress_current_secs, .start = view_start, .length = view_length, .total = total, .revision = device_state_metadata_revision()};
	return true;
}

bool player_mini_seek_commit(const player_mini_seek_t *preview, double position) {
	if (!player_mini_seek_valid(preview) || !isfinite(position))
		return false;
	device_state_t state;
	device_state_get(&state);
	if (state.live || state.status == AUDIO_STATUS_STOPPED || !state.current_file[0] || !player_mini_seek_valid(preview))
		return false;
	double end = preview->start + preview->length - 0.001;
	if (position < preview->start)
		position = preview->start;
	if (position > end)
		position = end;
	device_state_seek(position);
	if (audiobook_is_playing())
		audiobook_note_position(position, state.progress_total_secs, true);
	return true;
}

// Writes the two clocks that sit under the ends of the progress bar: elapsed
// on the left, total length on the right -- both of the file, or both of the
// chapter (see view_start).
static void set_progress_label(double current_secs, double total_secs) {
	// A live stream has neither: the bar full, and the words for it where the
	// clock would be.
	if (live_mode) {
		lv_slider_set_value(progress_slider, 1000, LV_ANIM_OFF);
		const char *live = tr("np_live");
		if (strcmp(lv_label_get_text(elapsed_label), live) != 0) {
			lv_label_set_text(elapsed_label, live);
			lv_label_set_text(remaining_label, "");
		}
		update_queue_position();
		return;
	}
	view_update(current_secs, total_secs);
	double shown = 0;
	if (view_length > 0) {
		lv_slider_set_value(progress_slider, view_value(current_secs), LV_ANIM_OFF);
		shown = current_secs - view_start;
		if (shown < 0) {
			shown = 0;
		}
	}

	// Set only when they change (sx_set_text): this runs twice a second, and
	// the total never moves within a track.
	formatDoubleSeconds(shown, progress_label_text, sizeof(progress_label_text));
	sx_set_text(elapsed_label, progress_label_text);

	formatDoubleSeconds(view_length, progress_label_text, sizeof(progress_label_text));
	sx_set_text(remaining_label, progress_label_text);

	// Written from here because this is the one place that runs on every poll
	// and on every track change, which is exactly when the answer can move.
	update_queue_position();
}

// True while a decode is in flight at the worker.
static bool cover_request_outstanding;

// What the artwork on screen IS, as cover_source_id() reports it, so the next
// track can be asked "the same picture?" before anything is thrown away. 0 when
// it is not known -- the answer has not come back yet, or there is no artwork.
static uint64_t cover_shown_id;

// The id of the picture now being decoded, when it was already known before the
// decode was asked for. Saves hashing the same file twice.
static uint64_t cover_incoming_id;

// Hashing never happens on this thread: it reads the picture out of the file,
// which is the work the loader exists to keep off the interface. So there is a
// question outstanding, and two reasons to have asked it.
static enum {
	COVER_ID_IDLE,
	COVER_ID_NEW_TRACK, // is the next track's picture the one already up?
	COVER_ID_MEASURE,	// what is the picture that just went up?
} cover_id_state;

// How often the loader's answers are looked for while one is outstanding. The
// progress poll comes round only every half second (two seconds when paused),
// and a track change asks twice -- what the picture is, then the picture -- so
// waiting for it added up to a second, or four, on top of the work itself.
// This timer runs only while a question is out and stops itself after.
#define COVER_POLL_MS 30
static lv_timer_t *cover_poll_timer;
static void cover_poll_start(void);
static void prefetch_next_cover(void);

static int cover_art_side(void) { return cover_box_h - 132; }

static void cover_art_layout(void) {
	if (!cover_art)
		return;
	int side = cover_art_side();
	lv_obj_set_size(cover_art, side, side);
	lv_obj_align(cover_art, LV_ALIGN_TOP_MID, 0, 124);
	lv_obj_set_style_radius(cover_art, SX_ROW_THUMB_R, 0);
	lv_obj_set_style_bg_color(cover_art, sx_pal()->mini_cover_tone, 0);
	if (cover_thumb_img)
		lv_obj_set_size(cover_thumb_img, side, side);
	lv_obj_set_style_bg_color(cover_panel, sx_pal()->np_bg, 0);
}

static void cover_show(bool have_cover) {
	if (!cover_img) {
		return;
	}
	if (have_cover) {
		lv_obj_set_hidden(cover_img, false);
	} else {
		lv_obj_set_hidden(cover_img, true);
	}
	if (!cover_placeholder_icon) {
		return;
	}
	if (!have_cover) {
		lv_obj_set_hidden(cover_placeholder_icon, false);
	} else {
		lv_obj_set_hidden(cover_placeholder_icon, true);
	}
}

static void paint_seek_track(void) {
	if (!progress_slider)
		return;
	lv_obj_set_style_bg_color(progress_slider, sx_pal()->np_text2, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(progress_slider, theme_is_dark() ? LV_OPA_50 : LV_OPA_70, LV_PART_MAIN);
}

static void paint_player_chrome(void) {
	lv_obj_t *const text_objs[] = {song_title_label, elapsed_label, remaining_label};
	lv_obj_t *const dim_objs[] = {song_artist_label, format_label, queue_position_label};
	lv_obj_t *const icon_objs[] = {prev_icon, next_icon, more_btn_icon};
	for (size_t i = 0; i < sizeof(text_objs) / sizeof(text_objs[0]); i++)
		if (text_objs[i])
			lv_obj_remove_local_style_prop(text_objs[i], LV_STYLE_TEXT_COLOR, 0);
	for (size_t i = 0; i < sizeof(dim_objs) / sizeof(dim_objs[0]); i++)
		if (dim_objs[i])
			lv_obj_set_style_text_color(dim_objs[i], sx_pal()->np_text2, 0);
	for (size_t i = 0; i < sizeof(icon_objs) / sizeof(icon_objs[0]); i++) {
		if (!icon_objs[i])
			continue;
		lv_obj_remove_local_style_prop(icon_objs[i], LV_STYLE_IMAGE_RECOLOR, 0);
		lv_obj_remove_local_style_prop(icon_objs[i], LV_STYLE_IMAGE_RECOLOR_OPA, 0);
	}
	if (play_btn) {
		lv_obj_set_style_bg_color(play_btn, sx_pal()->mini_action, 0);
		lv_obj_set_style_bg_opa(play_btn, LV_OPA_COVER, 0);
	}
	if (play_btn_icon) {
		lv_obj_set_style_image_recolor(play_btn_icon, sx_pal()->mini_on_action, 0);
		lv_obj_set_style_image_recolor_opa(play_btn_icon, LV_OPA_COVER, 0);
	}
	back_btn_over_cover(false);
	paint_seek_track();
}

// Told before the loaded track's artwork is freed. The screensaver borrows
// those pixels and can still be up behind a blanked panel when a track
// changes, so it has to let go first.
static void (*cover_release_cb)(void);

void player_set_cover_release_cb(void (*cb)(void)) { cover_release_cb = cb; }

void player_cover_box(int *w, int *h) {
	if (w) {
		*w = cover_box_w;
	}
	if (h) {
		*h = cover_box_h;
	}
}

bool player_cover_copy(int size, cover_image_t *out, uint64_t *id) {
	*id = 0;
	// Only a settled cover: while one is coming, or a track change is still
	// asking whether its picture is the same, what is up may be the last
	// track's.
	if (!current_cover.pixels || cover_request_outstanding || cover_id_state != COVER_ID_IDLE) {
		return false;
	}
	if (!cover_scaled_copy(&current_cover, size, out)) {
		return false;
	}
	*id = cover_shown_id;
	return true;
}

// The path whose artwork is currently up. A live stream refreshes its text
// several times a minute (every new on-air title), and re-decoding the same
// station icon each time would be pointless work on the UI thread.
static char cover_shown_path[512];

static void reload_cover(const char *filepath);

// Shows the placeholder and asks the worker for the album art of `filepath`.
// Nothing is read or decoded here: on the X1600E that work is seconds long,
// and doing it on this thread at every track change -- including the automatic
// one when a song ends -- froze whatever the user was doing at that moment,
// the file browser most of all. The finished pictures are collected by
// apply_cover_result(), from cover_poll_cb() and the progress timer tick.
// Rec. 601 luma, which is what "how bright does this look" means to an eye and
// not to a sum of three channels.
static int luma_of(int r, int g, int b) { return (r * 299 + g * 587 + b * 114) / 1000; }

// The sleeve's colour with its strength cut to `sat` per cent of the distance
// from grey and its brightness moved to `want`. Darkening is a plain ratio, so
// no channel can clip; brightening walks each channel the same fraction of the
// way to white, which keeps the hue where it was.
static lv_color_t tone_shaped(int sat, int want) {
	uint32_t raw = album_tone;
	if (!raw) {
		lv_color_t accent = theme()->accent;
		raw = ((uint32_t)accent.red << 16) | ((uint32_t)accent.green << 8) | accent.blue;
	}
	int r = (int)((raw >> 16) & 0xFF), g = (int)((raw >> 8) & 0xFF), b = (int)(raw & 0xFF);

	int grey = luma_of(r, g, b);
	r = grey + (r - grey) * sat / 100;
	g = grey + (g - grey) * sat / 100;
	b = grey + (b - grey) * sat / 100;

	int luma = luma_of(r, g, b);
	if (luma > want && luma > 0) {
		r = r * want / luma;
		g = g * want / luma;
		b = b * want / luma;
	} else if (luma < want && luma < 255) {
		int step = (want - luma) * 255 / (255 - luma);
		r += (255 - r) * step / 255;
		g += (255 - g) * step / 255;
		b += (255 - b) * step / 255;
	}
	return lv_color_make((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

static lv_color_t cover_mark(void) { return tone_shaped(72, theme_is_dark() ? 196 : 70); }

static lv_color_t chrome_accent(void) {
	if (cover_accent && album_tone) {
		return cover_mark();
	}
	// 2.0: the accent of the always-dark surfaces -- the cover's own unless a
	// preset is chosen (sx_palette.h).
	return sx_pal()->np_accent;
}

static void lyrics_repaint_lit(void);

// The two glyphs that wear the accent, repainted whenever the accent of the
// moment can have moved: the play/pause disc and the repeat/shuffle mode.
static void paint_transport_tint(void) {
	if (play_btn_icon) {
		lv_obj_set_style_image_recolor(play_btn_icon, sx_pal()->mini_on_action, 0);
		lv_obj_set_style_image_recolor_opa(play_btn_icon, LV_OPA_COVER, 0);
	}
	if (repeat_btn_icon) {
		update_repeat_button();
	}
	update_speed_button();
	if (progress_slider) {
		lv_obj_set_style_bg_color(progress_slider, chrome_accent(), LV_PART_INDICATOR);
		lv_obj_set_style_bg_color(progress_slider, chrome_accent(), LV_PART_KNOB);
	}
	lyrics_repaint_lit(); // the sung line wears it too
}

// Hides the artist's pill when there is no artist. An empty pill is a blank
// lozenge sitting on the sleeve for no reason, and a stream between titles has
// no artist for seconds at a time.

static void paint_player_tint(void) {
	paint_player_chrome();
	update_fav_button();
	paint_transport_tint();
}

// Whether what is playing is an ordinary file on the card.
//
// Asked of the state and the caches rather than of a flag raised at the start,
// for the reason written at apply_audiobook_mode(): a flag has to be cleared on
// every route by which the track can change, and the first route forgotten
// leaves the player dressed as the wrong thing.
//
// Everything that is not a file on the card is somebody else's audio passing
// through: the radio has no length, Qobuz, Tidal, a podcast and a phone pushing
// over DLNA all play out of a cache folder that is emptied behind them, and
// AirPlay does not go through the decoder at all -- during an AirPlay session
// `current_file` still names whatever local track was loaded last, so it has to
// be asked separately rather than inferred from the path.
//
// An audiobook is the one exception that IS a file on the card. It is left out
// anyway: the page is already dressed as a book -- chapters instead of a queue,
// skips instead of tracks, no star -- and none of what the alternative
// arrangement adds means anything to one. A shape drawn across nine hours of
// speech is a flat stripe, and the nine hours would have to be decoded to find
// that out.
static bool playing_local_file(const device_state_t *state) {
	if (state->live || !state->current_file[0] || audiobook_is_playing()) {
		return false;
	}
	airplay_state_t air;
	airplay_get_state(&air);
	if (air.playing) {
		return false;
	}
	const char *f = state->current_file;
	return !stream_provider_owns(f) && !podcastcache_is_episode(f) && !dlna_owns_path(f) && !dlna_owns_playback();
}

static lv_obj_t *lyrics_background;
#define LYRICS_MARGIN 14

#define LYRICS_FADE_PX 56
#define LYRICS_LINE_GAP 16
#define LYRICS_DIM_OPA LV_OPA_40
#define LYRICS_USER_HOLD_MS 4000
#define LYRICS_STEP_MS 320 // one line moving up into the middle
#define LYRICS_SLIDE_MS 220
// The page comes or goes once the finger has carried it this fraction of the
// way; short of it, it goes back.
#define LYRICS_COMMIT_FRACTION 4
#define LYRICS_DRAG_PX 10 // sideways past this, a swipe and not a tap
// Behind the words on a track with no artwork: dark in either theme, since
// the words are white.
#define LYRICS_FULL 1000 // lyrics_progress with the page all the way in

static bool lyrics_wanted;				// pulled in by a swipe
static bool lyrics_allowed;				// wanted, and a file on the card playing
static bool lyrics_now;					// on screen: allowed, and the file has words
static lv_obj_t *lyrics_pane;			// the page: the column and the note
static cover_image_t lyrics_screen_pic; // the blurred sleeve at the shape of the screen
static unsigned lyrics_bg_generation;
static int32_t lyrics_progress; // 0 the sleeve, LYRICS_FULL the words
// A line tapped: where it plays from, and until when the column waits for the
// player to get there rather than follow the position from before the jump.
static int32_t lyrics_tap_ms = -1;
static uint32_t lyrics_tap_until;
#define LYRICS_TAP_WAIT_MS 2000
static int lyrics_pane_x; // where the page rests
static lv_obj_t *lyrics_view;
static lv_obj_t *lyrics_note; // "no lyrics", in the middle of the column
static lyrics_t lyrics_cur;
static bool lyrics_loaded;		   // lyrics_cur belongs to lyrics_path, found or not
static char lyrics_path[1024];	   // the file the view is for
static unsigned lyrics_generation; // stale answers from the loader are dropped
static int lyrics_lit = -1;		   // the line drawn as being sung
static uint32_t lyrics_user_until; // no following until then: a finger is reading
static bool lyrics_centred;		   // the lit line is where it belongs
static lv_image_dsc_t lyrics_mask;
static uint8_t *lyrics_mask_pixels;
static int lyrics_view_h;

// Timed lines have room above the first and below the last, so either can be
// brought to the middle; untimed ones read from the top.
static void lyrics_pad(void) {
	int around = lyrics_cur.synced ? lyrics_view_h / 2 - 20 : LYRICS_FADE_PX / 2;
	lv_obj_set_style_pad_top(lyrics_view, around, 0);
	lv_obj_set_style_pad_bottom(lyrics_view, lyrics_cur.synced ? around : LYRICS_FADE_PX, 0);
}

// An A8 mask the size of the column, opaque but for a ramp at the top and at
// the bottom. Built again only when the size changes.
static void lyrics_mask_build(int w, int h) {
	if (lyrics_mask_pixels && (int)lyrics_mask.header.w == w && (int)lyrics_mask.header.h == h) {
		return;
	}
	if (lyrics_view) {
		lv_obj_set_style_bitmap_mask_src(lyrics_view, NULL, 0);
	}
	free(lyrics_mask_pixels);
	lyrics_mask_pixels = NULL;
	if (w <= 0 || h <= 2 * LYRICS_FADE_PX) {
		return;
	}
	lyrics_mask_pixels = malloc((size_t)w * (size_t)h);
	if (!lyrics_mask_pixels) {
		return;
	}
	for (int y = 0; y < h; y++) {
		int edge = y < h - 1 - y ? y : h - 1 - y;
		uint8_t a = edge >= LYRICS_FADE_PX ? 255 : (uint8_t)(edge * 255 / LYRICS_FADE_PX);
		memset(lyrics_mask_pixels + (size_t)y * (size_t)w, a, (size_t)w);
	}
	memset(&lyrics_mask, 0, sizeof(lyrics_mask));
	lyrics_mask.header.magic = LV_IMAGE_HEADER_MAGIC;
	lyrics_mask.header.cf = LV_COLOR_FORMAT_A8;
	lyrics_mask.header.w = (uint32_t)w;
	lyrics_mask.header.h = (uint32_t)h;
	lyrics_mask.header.stride = (uint32_t)w;
	lyrics_mask.data = lyrics_mask_pixels;
	lyrics_mask.data_size = (uint32_t)((size_t)w * (size_t)h);
	lv_obj_set_style_bitmap_mask_src(lyrics_view, &lyrics_mask, 0);
}

static void lyrics_note_show(const char *tag) {
	if (!lyrics_note) {
		return;
	}
	if (tag) {
		lv_label_set_text(lyrics_note, tr(tag));
		lv_obj_set_hidden(lyrics_note, false);
	} else {
		lv_obj_set_hidden(lyrics_note, true);
	}
}

// The column's scroll position, for its step animation.
static void lyrics_scroll_exec(void *obj, int32_t y) { lv_obj_scroll_to_y(obj, y, LV_ANIM_OFF); }

// The column's lines, one label each, from lyrics_cur.
static void lyrics_fill(void) {
	if (!lyrics_view) {
		return;
	}
	lv_obj_clean(lyrics_view);
	lyrics_pad();
	lyrics_lit = -1;
	lyrics_user_until = 0;
	lyrics_centred = false;
	lv_anim_delete(lyrics_view, lyrics_scroll_exec);
	lv_obj_scroll_to_y(lyrics_view, 0, LV_ANIM_OFF);

	if (!lyrics_loaded) {
		lyrics_note_show(NULL); // still being read
		return;
	}
	if (lyrics_cur.count == 0) {
		lyrics_note_show("lyrics_none");
		return;
	}
	lyrics_note_show(NULL);

	for (int i = 0; i < lyrics_cur.count; i++) {
		lv_obj_t *line = lv_label_create(lyrics_view);
		lv_label_set_text(line, lyrics_cur.lines[i].text[0] ? lyrics_cur.lines[i].text : " ");
		lv_label_set_long_mode(line, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(line, lv_pct(100));
		lv_obj_set_style_text_align(line, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_text_font(line, &font_ui_26, 0);
		lv_obj_set_style_text_opa(line, lyrics_cur.synced ? LYRICS_DIM_OPA : LV_OPA_COVER, 0);
		lv_obj_set_event_bubble(line, true);
		lv_obj_set_clickable(line, false);
	}
}

typedef struct {
	unsigned generation;
	char path[1024];
	char title[256];
	lyrics_t lyrics;
	bool found;
} lyrics_job_t;

static void lyrics_settle(void);

static void lyrics_arrived(void *user) {
	lyrics_job_t *job = user;
	if (job->generation == lyrics_generation && strcmp(job->path, lyrics_path) == 0) {
		lyrics_free(&lyrics_cur);
		lyrics_cur = job->lyrics; // handed over, found or empty
		lyrics_loaded = true;
		lyrics_fill();
		lyrics_settle();
	} else {
		lyrics_free(&job->lyrics);
	}
	free(job);
}

static void *lyrics_worker(void *arg) {
	lyrics_job_t *job = arg;
	job->found = lyrics_load(job->path, job->title, &job->lyrics);
	if (!gui_post(lyrics_arrived, job)) {
		lyrics_free(&job->lyrics);
		free(job);
	}
	return NULL;
}

// Only a file on the card has words to show.
static bool lyrics_available(void) {
	device_state_t state;
	device_state_get(&state);
	return playing_local_file(&state);
}

// The page is up, or on its way in or out under a finger.
static bool lyrics_on_screen(void) { return lyrics_pane && !lv_obj_is_hidden(lyrics_pane); }

// Words being read are not an idle screen: while the lyrics are up on an open
// player, the idle timer does not blank the panel under them. Asked again
// wherever either half changes; the hold itself ignores a call that changes
// nothing.
static void lyrics_screen_hold_update(void) { power_hold_screen_for_view(sheet_open && lyrics_on_screen()); }

// The words of the file playing are known before any swipe: on a track with
// none the swipe asks to download them, and the page goes away by itself on
// one.
static bool lyrics_has_words(void) { return lyrics_loaded && lyrics_cur.count > 0; }

// The lyrics of the file playing, read on a thread of their own as soon as it
// starts: the tags of a FLAC are read whole, pictures included, and that is
// not the interface's to wait for. Asked again only when the file changes.
static void lyrics_request(const device_state_t *state) {
	if (!state->current_file[0] || !playing_local_file(state)) {
		return;
	}
	if (strcmp(state->current_file, lyrics_path) == 0) {
		return;
	}
	snprintf(lyrics_path, sizeof(lyrics_path), "%s", state->current_file);
	lyrics_free(&lyrics_cur);
	lyrics_loaded = false;
	lyrics_generation++;
	lyrics_fill();

	lyrics_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		return;
	}
	job->generation = lyrics_generation;
	snprintf(job->path, sizeof(job->path), "%s", state->current_file);
	snprintf(job->title, sizeof(job->title), "%s", state->metadata.title);
	pthread_t thread;
	if (pthread_create(&thread, NULL, lyrics_worker, job) == 0) {
		pthread_detach(thread);
	} else {
		free(job);
	}
}

// Brings `label` to the middle of the column, always in the same short time
// whatever the distance, so the words step up like a typewriter's paper.
static void lyrics_centre(lv_obj_t *label) {
	lv_obj_update_layout(lyrics_view);
	lv_area_t line_area;
	lv_area_t view_area;
	lv_obj_get_coords(label, &line_area);
	lv_obj_get_coords(lyrics_view, &view_area);
	int32_t from = lv_obj_get_scroll_y(lyrics_view);
	int32_t to = from + (line_area.y1 + line_area.y2) / 2 - (view_area.y1 + view_area.y2) / 2;
	if (to < 0) {
		to = 0;
	}

	lv_anim_delete(lyrics_view, lyrics_scroll_exec);
	if (to == from) {
		return;
	}
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, lyrics_view);
	lv_anim_set_exec_cb(&a, lyrics_scroll_exec);
	lv_anim_set_values(&a, from, to);
	lv_anim_set_duration(&a, LYRICS_STEP_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_start(&a);
}

static void lyrics_paint_line(int index, bool lit) {
	if (!lyrics_view || index < 0 || index >= (int)lv_obj_get_child_count(lyrics_view)) {
		return;
	}
	lv_obj_t *label = lv_obj_get_child(lyrics_view, index);
	// Unlit, the line wears the column's colour (see lyrics_look()).
	if (lit) {
		lv_obj_set_style_text_color(label, chrome_accent(), 0);
	} else {
		lv_obj_remove_local_style_prop(label, LV_STYLE_TEXT_COLOR, 0);
	}
	lv_obj_set_style_text_opa(label, lit ? LV_OPA_COVER : LYRICS_DIM_OPA, 0);
}

static void lyrics_repaint_lit(void) { lyrics_paint_line(lyrics_lit, true); }

// The line being sung, lit and held in the middle of the column. Once a
// finger has scrolled the words, the column comes back to it a few seconds
// after the finger stops -- or, with the music paused, once it plays again:
// until then the words stay where the finger left them.
static void lyrics_follow(double seconds) {
	if (!lyrics_on_screen() || !lyrics_view || !lyrics_cur.synced || lyrics_cur.count == 0) {
		return;
	}
	int32_t ms = (int32_t)(seconds * 1000.0);
	if (lyrics_tap_ms >= 0) {
		// Until the player reports a position near the tapped line, the one
		// it reports is from before the jump: following it would send the
		// column back there and then forward again.
		if ((int32_t)(lv_tick_get() - lyrics_tap_until) < 0 && LV_ABS(ms - lyrics_tap_ms) > 1500) {
			return;
		}
		lyrics_tap_ms = -1;
	}
	int line = lyrics_line_at(&lyrics_cur, ms);
	if (line != lyrics_lit) {
		lyrics_paint_line(lyrics_lit, false);
		lyrics_lit = line;
		lyrics_paint_line(line, true);
		lyrics_centred = false;
	}

	if (lyrics_centred) {
		return;
	}
	if (lyrics_user_until && ((int32_t)(lv_tick_get() - lyrics_user_until) < 0 || audio_get_status() != AUDIO_STATUS_PLAYING)) {
		return;
	}
	lyrics_user_until = 0;
	lyrics_centred = true;
	if (line >= 0 && line < (int)lv_obj_get_child_count(lyrics_view)) {
		lyrics_centre(lv_obj_get_child(lyrics_view, line));
	} else {
		lyrics_centre(lv_obj_get_child(lyrics_view, 0));
	}
}

static void lyrics_scroll_cb(lv_event_t *e) {
	(void)e;
	// A finger, not the column following the music: that scrolls from a timer.
	if (lv_indev_active()) {
		lv_anim_delete(lyrics_view, lyrics_scroll_exec);
		lyrics_user_until = lv_tick_get() + LYRICS_USER_HOLD_MS;
		lyrics_centred = false;
	}
}

static const lv_image_dsc_t *lyrics_backdrop(void) { return lyrics_screen_pic.pixels ? &lyrics_screen_pic.dsc : NULL; }

// Keeps the title and artist at the top saying what the row below says.

static bool lyrics_look_on;

static void lyrics_look(bool on) {

	const lv_image_dsc_t *backdrop = lyrics_backdrop();
	if (on && !backdrop) {
		on = false;
	}
	if (on) {
		sx_artview_bind(lyrics_background, backdrop, lyrics_screen_pic.display_generation);
		lv_obj_set_hidden(lyrics_background, false);
		lv_obj_move_background(lyrics_background);
		lv_obj_set_style_bg_opa(cover_panel, LV_OPA_TRANSP, 0);
		lv_obj_set_style_bg_opa(player_menu, LV_OPA_TRANSP, 0);
		lv_obj_set_style_bg_image_opa(player_menu, LV_OPA_TRANSP, 0);
	} else if (lyrics_look_on) {
		lv_obj_set_hidden(lyrics_background, true);
		lv_obj_remove_local_style_prop(cover_panel, LV_STYLE_BG_OPA, 0);
		lv_obj_remove_local_style_prop(player_menu, LV_STYLE_BG_OPA, 0);
		lv_obj_remove_local_style_prop(player_menu, LV_STYLE_BG_IMAGE_OPA, 0);
	}
	lyrics_look_on = on;
	// The words are white over a blurred sleeve, and in the theme's own
	// colour over the page's own background where there is no picture.
	// 2.0: the player is dark in both themes, so the words are always white.
	lv_color_t ink = lv_color_white();
	lv_obj_set_style_text_color(lyrics_view, ink, 0);
	lv_obj_set_style_text_color(lyrics_note, ink, 0);
}

// What travels with the sleeve: it goes out to the left as the words come in.
static void lyrics_shift(lv_obj_t *obj, int32_t x) {
	if (!obj) {
		return;
	}
	if (x == 0) {
		lv_obj_remove_local_style_prop(obj, LV_STYLE_TRANSLATE_X, 0);
	} else {
		lv_obj_set_style_translate_x(obj, x, 0);
	}
}

// The button at the top right while the words are all the way in: it looks them
// up on LRCLIB again, to pick others in their place. Beside the star or the
// ellipsis where an arrangement keeps one up there, in the corner otherwise.
#define LYRICS_FIND_BTN 56
static lv_obj_t *lyrics_find_btn;
static lv_obj_t *lyrics_find_icon;

// The names at the top give up the button's width on both sides, so a long
// title stays centred and stops short of it.

static void lyrics_find_sync(bool full) {
	if (!lyrics_find_btn)
		return;
	if (!full || !lyrics_now) {
		lv_obj_set_hidden(lyrics_find_btn, true);
		return;
	}
	lv_obj_set_pos(lyrics_find_btn, cover_box_w - LYRICS_MARGIN - LYRICS_FIND_BTN, back_btn_centre_y() - LYRICS_FIND_BTN / 2);
	lv_obj_set_style_image_recolor(lyrics_find_icon, lyrics_backdrop() ? lv_color_white() : theme()->text_primary, 0);
	lv_obj_set_hidden(lyrics_find_btn, false);
	lv_obj_move_foreground(lyrics_find_btn);
}

// Everything the swipe moves, at `progress` of the way from the sleeve (0)
// to the words (LYRICS_FULL). Only positions change from one frame to the
// next; at 0 every style touched is handed back, so the arrangement is
// exactly what it was before the first swipe.
static void lyrics_progress_apply(int32_t progress) {
	if (!lyrics_pane) {
		return;
	}
	if (progress < 0) {
		progress = 0;
	}
	if (progress > LYRICS_FULL) {
		progress = LYRICS_FULL;
	}
	lyrics_progress = progress;
	lyrics_find_sync(progress == LYRICS_FULL);
	bool rest = progress == 0;
	int32_t travel = cover_box_w - lyrics_pane_x;

	if (rest) {
		lv_obj_set_hidden(lyrics_pane, true);
	} else if (lv_obj_is_hidden(lyrics_pane)) {
		lv_obj_set_hidden(lyrics_pane, false);
		lv_obj_move_foreground(lyrics_pane);
	}
	lyrics_screen_hold_update();
	lv_obj_set_x(lyrics_pane, lyrics_pane_x + travel * (LYRICS_FULL - progress) / LYRICS_FULL);
	if (rest || !lyrics_look_on) {
		lyrics_look(!rest);
	}

	int32_t away = -(int32_t)cover_box_w * progress / LYRICS_FULL;
	lyrics_shift(cover_img, away);
	lyrics_shift(cover_placeholder_icon, away);
}

// While the page moves, the fade at its edges is left off: it is drawn by
// rendering the column into a layer of its own and masking it, which is fine
// once a scroll step and far too much for every frame of a swipe.
static void lyrics_fade(bool on) {
	if (lyrics_view && lyrics_mask_pixels) {
		lv_obj_set_style_bitmap_mask_src(lyrics_view, on ? &lyrics_mask : NULL, 0);
	}
}

static void lyrics_progress_exec(void *obj, int32_t value) {
	(void)obj;
	lyrics_progress_apply(value);
}

static void lyrics_slid_cb(lv_anim_t *a) {
	(void)a;
	lyrics_fade(true);
}

static void lyrics_slide_to(int32_t progress) {
	lv_anim_delete(lyrics_pane, lyrics_progress_exec);
	if (lyrics_progress == progress) {
		lyrics_progress_apply(progress);
		lyrics_fade(true);
		return;
	}
	lyrics_fade(false);
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, lyrics_pane);
	lv_anim_set_exec_cb(&a, lyrics_progress_exec);
	lv_anim_set_values(&a, lyrics_progress, progress);
	lv_anim_set_duration(&a, LYRICS_SLIDE_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_completed_cb(&a, lyrics_slid_cb);
	lv_anim_start(&a);
}

static void lyrics_layout(void) {
	if (!lyrics_pane)
		return;
	int top = back_btn_centre_y() + 28;
	int w = cover_box_w, h = cover_box_h - top;
	int view_w = w - 2 * LYRICS_MARGIN;
	lyrics_pane_x = 0;
	lv_anim_delete(lyrics_pane, lyrics_progress_exec);
	lyrics_progress_apply(0);
	lv_obj_set_y(lyrics_pane, 0);
	lv_obj_set_size(lyrics_pane, w, cover_box_h);
	lv_obj_set_pos(lyrics_view, LYRICS_MARGIN, top);
	lv_obj_set_size(lyrics_view, view_w, h);
	lyrics_view_h = h;
	lyrics_pad();
	lyrics_mask_build(view_w, h);
	lv_obj_set_size(lyrics_note, view_w, LV_SIZE_CONTENT);
	lv_obj_set_pos(lyrics_note, LYRICS_MARGIN, top + h / 2 - 20);
	lyrics_progress_apply(lyrics_now ? LYRICS_FULL : 0);
}

// Brings the page up, or puts it away, and remembers it either way: it stays
// up from one track to the next.
static void lyrics_commit(bool on) {
	lyrics_wanted = on;
	device_state_t state;
	device_state_get(&state);
	lyrics_allowed = on && playing_local_file(&state);
	lyrics_now = lyrics_allowed && lyrics_has_words();
	if (lyrics_now) {
		lyrics_slide_to(LYRICS_FULL);
		lyrics_request(&state);
		lyrics_follow(state.progress_current_secs);
	} else {
		lyrics_slide_to(0);
	}
}

// The words for the file playing have been read: a page wanted and up stays
// up if there are any and goes if there are none; one wanted and put away for
// a track without words comes back on the first track that has some.
static void lyrics_settle(void) {
	bool now = lyrics_allowed && lyrics_has_words();
	if (now == lyrics_now || !lyrics_pane) {
		return;
	}
	lyrics_now = now;
	lyrics_slide_to(now ? LYRICS_FULL : 0);
	if (now) {
		device_state_t state;
		device_state_get(&state);
		lyrics_follow(state.progress_current_secs);
	}
}

// ---------------------------------------------------------------------------
// Words from LRCLIB, for a file with none
// ---------------------------------------------------------------------------

typedef struct {
	char path[1024];
	char title[256];
	char artist[256];
	char album[256];
	int duration;
	lrclib_result_t result;
	lrclib_record_t *records;
	int count;
} lyrics_fetch_t;

static bool lyrics_fetching; // one search at a time
static pthread_mutex_t lyrics_fetch_lock = PTHREAD_MUTEX_INITIALIZER;
static lyrics_fetch_t *lyrics_fetch_done;
static lv_timer_t *lyrics_fetch_timer;

void player_lyrics_reload(const char *path) {
	device_state_t state;
	device_state_get(&state);
	if (!path || strcmp(state.current_file, path) != 0) {
		return;
	}
	// Read again, from the .lrc now beside the file. Words that were up stay up
	// and change in place; otherwise the page is wanted, and comes in when the
	// words arrive (lyrics_settle()).
	bool up = lyrics_now;
	lyrics_path[0] = '\0';
	lyrics_request(&state);
	if (!up) {
		lyrics_commit(true);
	}
}

static void lyrics_fetched(void *user) {
	lyrics_fetch_t *job = user;
	lyrics_fetching = false;
	toast_busy_end();

	switch (job->result) {
	case LRCLIB_OK: {
		// The list is for the track it was asked about, over the player it was
		// asked from; with either gone it is no longer wanted.
		device_state_t state;
		device_state_get(&state);
		if (sheet_open && strcmp(state.current_file, job->path) == 0) {
			lyricsearch_open(job->path, job->records, job->count);
			job->records = NULL;
		}
		break;
	}
	case LRCLIB_INSTRUMENTAL:
		toast_plain("lyrics_instrumental");
		break;
	case LRCLIB_NOT_FOUND:
		toast_plain("lyrics_not_found");
		break;
	case LRCLIB_NO_TAGS:
		toast_error("lyrics_no_tags");
		break;
	default:
		toast_error("lyrics_download_failed");
		break;
	}
	lrclib_free(job->records, job->count);
	free(job);
}

static void lyrics_fetch_poll(lv_timer_t *timer) {
	pthread_mutex_lock(&lyrics_fetch_lock);
	lyrics_fetch_t *job = lyrics_fetch_done;
	lyrics_fetch_done = NULL;
	pthread_mutex_unlock(&lyrics_fetch_lock);
	if (job) {
		lv_timer_delete(timer);
		lyrics_fetch_timer = NULL;
		lyrics_fetched(job);
	}
}

static void *lyrics_fetch_worker(void *arg) {
	lyrics_fetch_t *job = arg;
	job->result = lrclib_search(job->title, job->artist, job->album, job->duration, &job->records, &job->count);
	pthread_mutex_lock(&lyrics_fetch_lock);
	lyrics_fetch_done = job;
	pthread_mutex_unlock(&lyrics_fetch_lock);
	return NULL;
}

static void lyrics_search_run(void);

// ---------------------------------------------------------------------------
// Wi-Fi switched on for a search
//
// With Wi-Fi off the question offers to switch it on instead of searching; the
// search then starts by itself once the link has an address, for the file that
// was playing when the question was answered.
// ---------------------------------------------------------------------------

#define LYRICS_WIFI_POLL_MS 500
#define LYRICS_WIFI_WAIT_MS 30000

static lv_timer_t *lyrics_wifi_timer;
static uint32_t lyrics_wifi_since;
static char lyrics_wifi_path[1024];

static bool lyrics_wifi_online(void) {
	wifi_status_t wifi;
	wifi_get_status(&wifi);
	return wifi.state == WIFI_STATE_CONNECTED && wifi.ip[0] != '\0';
}

static void lyrics_wifi_wait_end(void) {
	if (lyrics_wifi_timer) {
		lv_timer_delete(lyrics_wifi_timer);
		lyrics_wifi_timer = NULL;
	}
}

static void lyrics_wifi_tick_cb(lv_timer_t *t) {
	(void)t;
	device_state_t state;
	device_state_get(&state);
	// Another track, or Wi-Fi switched off again: the search is no longer wanted.
	if (!wifi_get_enabled() || strcmp(state.current_file, lyrics_wifi_path) != 0) {
		lyrics_wifi_wait_end();
		toast_busy_end();
		return;
	}
	if (lyrics_wifi_online()) {
		lyrics_wifi_wait_end();
		toast_busy_end();
		lyrics_search_run();
		return;
	}
	if (lv_tick_elaps(lyrics_wifi_since) >= LYRICS_WIFI_WAIT_MS) {
		lyrics_wifi_wait_end();
		toast_busy_end();
		toast_error("lyrics_wifi_failed");
	}
}

static void lyrics_enable_wifi_cb(void *user) {
	(void)user;
	device_state_t state;
	device_state_get(&state);
	if (!state.current_file[0] || !playing_local_file(&state)) {
		return;
	}
	snprintf(lyrics_wifi_path, sizeof(lyrics_wifi_path), "%s", state.current_file);
	wifi_set_enabled(true);
	lyrics_wifi_since = lv_tick_get();
	if (!lyrics_wifi_timer) {
		lyrics_wifi_timer = lv_timer_create(lyrics_wifi_tick_cb, LYRICS_WIFI_POLL_MS, NULL);
	}
	toast_busy_dismissable("lyrics_wifi_connecting");
}

// Wi-Fi is off and can be switched on: the question offers that in place of
// the search.
static bool lyrics_offer_wifi(void) {
	if (lyrics_wifi_timer) {
		toast_busy_dismissable("lyrics_wifi_connecting");
		return true;
	}
	if (wifi_get_enabled() || !wifi_available()) {
		return false;
	}
	confirm_show("lyrics_download_title", "lyrics_wifi_off_note", "lyrics_enable_wifi", lyrics_enable_wifi_cb, NULL);
	return true;
}

// The file playing looked up on LRCLIB, on a thread of its own; what is found
// is listed to pick from.
static void lyrics_search_start(void) {
	if (!lyrics_fetching && lyrics_offer_wifi()) {
		return;
	}
	lyrics_search_run();
}

static void lyrics_search_run(void) {
	if (lyrics_fetching) {
		toast_busy_dismissable("lyrics_downloading");
		return;
	}
	wifi_status_t wifi;
	wifi_get_status(&wifi);
	if (wifi.state != WIFI_STATE_CONNECTED) {
		toast_error("lyrics_no_wifi");
		return;
	}
	device_state_t state;
	device_state_get(&state);
	if (!state.current_file[0] || !playing_local_file(&state)) {
		return;
	}

	lyrics_fetch_t *job = calloc(1, sizeof(*job));
	if (!job) {
		return;
	}
	snprintf(job->path, sizeof(job->path), "%s", state.current_file);
	snprintf(job->title, sizeof(job->title), "%s", state.metadata.title);
	// The track's own performer is who LRCLIB files it under; the album's
	// artist only when the track names none.
	snprintf(job->artist, sizeof(job->artist), "%s", state.metadata.artist[0] ? state.metadata.artist : state.metadata.album_artist);
	snprintf(job->album, sizeof(job->album), "%s", state.metadata.album);
	job->duration = (int)(state.progress_total_secs + 0.5);

	lyrics_fetch_timer = lv_timer_create(lyrics_fetch_poll, 100, NULL);
	if (!lyrics_fetch_timer) {
		free(job);
		toast_error("lyrics_download_failed");
		return;
	}
	pthread_t thread;
	if (pthread_create(&thread, NULL, lyrics_fetch_worker, job) != 0) {
		lv_timer_delete(lyrics_fetch_timer);
		lyrics_fetch_timer = NULL;
		free(job);
		toast_error("lyrics_download_failed");
		return;
	}
	pthread_detach(thread);
	lyrics_fetching = true;
	// A tap puts the spinner away while LRCLIB is asked again after a failure;
	// the list still opens when the answer comes.
	toast_busy_dismissable("lyrics_downloading");
}

// The answer to the question below.
static void lyrics_download_cb(void *user) {
	(void)user;
	lyrics_search_start();
}

// The button over the words: other words for the same file, in their place.
static void lyrics_find_cb(lv_event_t *e) {
	(void)e;
	if (player_sheet_drag_active()) {
		return;
	}
	lyrics_search_start();
}

// A swipe to the words of a file that has none.
static void lyrics_offer_download(void) {
	if (lyrics_fetching) {
		toast_busy_dismissable("lyrics_downloading");
		return;
	}
	if (lyrics_offer_wifi()) {
		return;
	}
	confirm_show("lyrics_download_title", "lyrics_download_note", "lyrics_download", lyrics_download_cb, NULL);
}

// The swipe that pulls the page in (on the sleeve, right to left) or pushes it
// out (on the words, left to right). `opening` is the user data. Sideways
// only: a movement that reads as vertical is the column being scrolled, and
// a swipe the other way on the sleeve is the player being put away.
static bool lyrics_swiped; // the release that ends a swipe is not a tap on a line

static void lyrics_swiped_clear_cb(void *unused) {
	(void)unused;
	lyrics_swiped = false;
}

static void lyrics_drag_cb(lv_event_t *e) {
	static lv_point_t start;
	static bool tracking;
	static bool engaged;
	static bool asking; // no words to pull in: the swipe offers to download them

	bool opening = (bool)(uintptr_t)lv_event_get_user_data(e);
	lv_event_code_t code = lv_event_get_code(e);
	lv_indev_t *indev = lv_indev_active();
	if (!indev || !lyrics_pane) {
		return;
	}

	if (code == LV_EVENT_PRESSED) {
		tracking = opening ? !lyrics_now && lyrics_available() && lyrics_loaded : lyrics_now;
		asking = opening && !lyrics_has_words();
		engaged = false;
		lv_indev_get_point(indev, &start);
		return;
	}
	if (!tracking) {
		return;
	}

	lv_point_t p;
	lv_indev_get_point(indev, &p);
	int dx = p.x - start.x;
	int dy = p.y - start.y;

	if (code == LV_EVENT_PRESSING) {
		if (!engaged) {
			if (lv_indev_get_scroll_obj(indev)) {
				tracking = false; // the column is being scrolled
				return;
			}
			if (LV_ABS(dy) >= LYRICS_DRAG_PX && LV_ABS(dy) > LV_ABS(dx)) {
				tracking = false;
				return;
			}
			if (LV_ABS(dx) < LYRICS_DRAG_PX || LV_ABS(dx) < 2 * LV_ABS(dy)) {
				return;
			}
			if ((opening && dx > 0) || (!opening && dx < 0)) {
				tracking = false; // the other way: not this gesture
				return;
			}
			engaged = true;
			lyrics_swiped = true;
			if (asking) {
				return; // the sleeve stays: there is no page to bring
			}
			lv_anim_delete(lyrics_pane, lyrics_progress_exec);
			lyrics_fade(false);
			if (opening) {
				lyrics_progress_apply(1); // up, so the request below finds it on screen
				device_state_t state;
				device_state_get(&state);
				lyrics_request(&state);
				lyrics_follow(state.progress_current_secs);
			}
		}
		if (asking) {
			return;
		}
		int32_t moved = (int32_t)dx * LYRICS_FULL / cover_box_w;
		lyrics_progress_apply(opening ? -moved : LYRICS_FULL - moved);
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		tracking = false;
		if (!engaged) {
			return;
		}
		engaged = false;
		bool far = LV_ABS(dx) > cover_box_w / LYRICS_COMMIT_FRACTION;
		if (asking) {
			if (far) {
				lyrics_offer_download();
			}
		} else {
			lyrics_commit(opening ? far : !far);
		}
		// Cleared after this gesture's CLICKED has been seen, as for the sheet.
		lv_async_call(lyrics_swiped_clear_cb, NULL);
	}
}

// A tap on a timed line plays from where it is sung.
static void lyrics_tap_cb(lv_event_t *e) {
	(void)e;
	if (lyrics_swiped || !lyrics_cur.synced || !lyrics_view) {
		return;
	}
	lv_indev_t *indev = lv_indev_active();
	if (!indev) {
		return;
	}
	lv_point_t p;
	lv_indev_get_point(indev, &p);
	uint32_t count = lv_obj_get_child_count(lyrics_view);
	for (uint32_t i = 0; i < count && i < (uint32_t)lyrics_cur.count; i++) {
		lv_area_t area;
		lv_obj_get_coords(lv_obj_get_child(lyrics_view, i), &area);
		// The gap between two lines belongs to the one above it.
		if (p.y >= area.y1 && p.y <= area.y2 + LYRICS_LINE_GAP) {
			int32_t ms = lyrics_cur.lines[i].ms;
			if (ms < 0) {
				return;
			}
			device_state_seek((double)ms / 1000.0);
			// Back to following at once: the finger chose this line.
			lyrics_user_until = 0;
			lyrics_centred = false;
			lyrics_follow((double)ms / 1000.0);
			lyrics_tap_ms = ms;
			lyrics_tap_until = lv_tick_get() + LYRICS_TAP_WAIT_MS;
			return;
		}
	}
}

static void lyrics_build(void) {
	lyrics_pane = lv_obj_create(cover_panel);
	lv_obj_remove_style_all(lyrics_pane);
	lv_obj_set_ignore_layout(lyrics_pane, true);
	lv_obj_set_scrollable(lyrics_pane, false);
	// Presses on the page outside the column go to the sleeve under it.
	lv_obj_set_clickable(lyrics_pane, false);
	lv_obj_set_hidden(lyrics_pane, true);

	lyrics_view = lv_obj_create(lyrics_pane);
	lv_obj_remove_style_all(lyrics_view);
	lv_obj_set_ignore_layout(lyrics_view, true);
	lv_obj_set_flex_flow(lyrics_view, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(lyrics_view, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(lyrics_view, LYRICS_LINE_GAP, 0);
	lv_obj_set_style_pad_hor(lyrics_view, 8, 0);
	lv_obj_set_scroll_dir(lyrics_view, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(lyrics_view, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_event_cb(lyrics_view, lyrics_scroll_cb, LV_EVENT_SCROLL_BEGIN, NULL);
	lv_obj_add_event_cb(lyrics_view, lyrics_tap_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(lyrics_view, lyrics_drag_cb, LV_EVENT_PRESSED, (void *)(uintptr_t)false);
	lv_obj_add_event_cb(lyrics_view, lyrics_drag_cb, LV_EVENT_PRESSING, (void *)(uintptr_t)false);
	lv_obj_add_event_cb(lyrics_view, lyrics_drag_cb, LV_EVENT_RELEASED, (void *)(uintptr_t)false);
	lv_obj_add_event_cb(lyrics_view, lyrics_drag_cb, LV_EVENT_PRESS_LOST, (void *)(uintptr_t)false);

	lyrics_note = lv_label_create(lyrics_pane);
	lv_obj_set_ignore_layout(lyrics_note, true);
	lv_label_set_long_mode(lyrics_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_align(lyrics_note, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(lyrics_note, &font_ui_24, 0);
	lv_obj_set_style_text_color(lyrics_note, lv_color_white(), 0);
	lv_obj_set_style_text_opa(lyrics_note, LV_OPA_70, 0);
	lv_obj_set_hidden(lyrics_note, true);

	lyrics_find_btn = lv_btn_create(player_screen);
	lv_obj_set_ignore_layout(lyrics_find_btn, true);
	lv_obj_set_size(lyrics_find_btn, LYRICS_FIND_BTN, LYRICS_FIND_BTN);
	lv_obj_set_style_bg_opa(lyrics_find_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(lyrics_find_btn, 0, 0);
	lv_obj_set_style_border_width(lyrics_find_btn, 0, 0);
	lv_obj_set_ext_click_area(lyrics_find_btn, CORNER_BTN_EXT_CLICK);
	lv_obj_add_event_cb(lyrics_find_btn, lyrics_find_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_set_hidden(lyrics_find_btn, true);
	lyrics_find_icon = lv_image_create(lyrics_find_btn);
	lv_image_set_src(lyrics_find_icon, &icon_lyrics_search);
	lv_obj_add_style(lyrics_find_icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(lyrics_find_icon, LV_OPA_COVER, 0);
	lv_obj_center(lyrics_find_icon);

	// 2.0: the sleeve no longer pulls the page in -- a swipe across it is the
	// previous or next track now (the handoff's gesture matrix), and the words
	// come up from the player's menu ("Lyrics", player_lyrics_toggle()). The
	// swipe on the words that puts them away again is kept: on the column it
	// means nothing else.
}

typedef struct {
	unsigned generation;
	int w, h; // the screen
	cover_image_t src;
	cover_image_t out;
} lyrics_bg_job_t;

static void lyrics_bg_arrived(void *user) {
	lyrics_bg_job_t *job = user;
	if (job->generation == lyrics_bg_generation && job->out.pixels) {
		sx_artview_release_cover(&lyrics_screen_pic);
		lyrics_screen_pic = job->out;
		lyrics_screen_pic.dsc.data = lyrics_screen_pic.pixels; // struct moved, buffer didn't
		if (lyrics_progress > 0) {
			lyrics_look(true);
		}
	} else {
		cover_free(&job->out);
	}
	free(job);
}

static void *lyrics_bg_worker(void *arg) {
	lyrics_bg_job_t *job = arg;
	cover_blur_fill(&job->src, job->w, job->h, &job->out);
	cover_free(&job->src);
	if (!gui_post(lyrics_bg_arrived, job)) {
		cover_free(&job->out);
		free(job);
	}
	return NULL;
}

static void lyrics_bg_forget(void) {
	lyrics_bg_generation++;
	if (lyrics_look_on) {
		lyrics_look(false);
	}
	if (lyrics_background && lv_image_get_src(lyrics_background) == &lyrics_screen_pic.dsc) {
		sx_artview_detach(lyrics_background);
		lv_image_set_src(lyrics_background, NULL);
	}
	sx_artview_release_cover(&lyrics_screen_pic);
}

static void lyrics_bg_prepare(void) {
	if (!current_cover.pixels) {
		return;
	}
	lyrics_bg_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		return;
	}
	size_t size = current_cover.dsc.data_size;
	job->src = current_cover;
	job->src.pixels = malloc(size);
	if (!job->src.pixels) {
		free(job);
		return;
	}
	memcpy(job->src.pixels, current_cover.pixels, size);
	job->src.dsc.data = job->src.pixels;
	job->generation = lyrics_bg_generation;
	job->w = (int)lv_obj_get_width(player_screen);
	job->h = (int)lv_obj_get_height(player_screen);

	pthread_t thread;
	if (pthread_create(&thread, NULL, lyrics_bg_worker, job) == 0) {
		pthread_detach(thread);
	} else {
		cover_free(&job->src);
		free(job);
	}
}

static void align_title_with_star(void) {
	if (song_title_label)
		lv_obj_set_style_translate_y(song_title_label, 0, 0);
}

static void song_info_layout_cb(lv_event_t *e) {
	(void)e;
	align_title_with_star();
}

static void apply_layout(void) {
	if (!song_text_obj)
		return;
	lyrics_layout();
	cover_art_layout();
	menu_style_for_layout();
	np_layout_changed();
	align_title_with_star();
	paint_player_tint();
	bar_over_player_update();
}

static void update_layout(const device_state_t *state) {
	bool lyrics = lyrics_wanted && playing_local_file(state);
	if (lyrics == lyrics_allowed)
		return;
	lyrics_allowed = lyrics;
	bool same_file = strcmp(state->current_file, lyrics_path) == 0;
	if (!lyrics)
		lyrics_now = false;
	else if (same_file && lyrics_loaded)
		lyrics_now = lyrics_cur.count > 0;
	apply_layout();
	lyrics_request(state);
}

// Decode artwork at its final display size.
static void cover_request_size(int *w, int *h) { *w = *h = cover_art_side(); }

// The next track's cover, made and kept in the cache while this one plays, so
// that skipping to it, or reaching it, finds it ready instead of decoding it
// then. Only with the screen on: with it off nobody sees the cover arrive, and
// the decode would be battery spent on a guess.
static void prefetch_next_cover(void) {
	if (!power_screen_is_on() || live_mode) {
		return;
	}
	char next[512];
	if (!playlist_peek_auto(next, sizeof(next)) || strcmp(next, cover_shown_path) == 0) {
		return;
	}
	int w, h;
	cover_request_size(&w, &h);
	coverloader_prefetch_player(next, w, h);
}

// Throws away what is on screen and asks for the artwork of `filepath`. The
// half of refresh_cover() that runs once it is known that the picture really is
// changing.
static void reload_cover(const char *filepath) {
	if (cover_release_cb) {
		cover_release_cb();
	}

	// Detach the old pixels before freeing them, so nothing can draw from a
	// buffer that's already gone. The placeholder covers the gap while the
	// worker decodes.
	sx_artview_detach(cover_img);
	lv_image_set_src(cover_img, NULL);
	lyrics_bg_forget();

	sx_artview_release_cover(&current_cover);
	sx_artview_release_cover(&current_backdrop);
	bar_over_player_update();

	cover_show(false);

	// Backdrop gone: the chrome goes back to following the theme.
	paint_player_chrome();
	album_tone = 0;
	paint_player_tint();

	lv_obj_invalidate(cover_panel);
	lv_obj_invalidate(player_menu);

	if (filepath && filepath[0]) {
		cover_set_backdrop_upright(false);

		int cover_w, cover_h;
		cover_request_size(&cover_w, &cover_h);
		coverloader_request_player(filepath, cover_w, cover_h, backdrop_w, backdrop_h);
		cover_request_outstanding = true;
		// Until it lands, the album's 240 px thumbnail stretched over its place.
		np_thumb240_request();

		// The collector rides on the progress timer too; make sure it is
		// ticking even when nothing is playing (paused player, theme change).
		lv_timer_resume(progress_slider_timer);
		cover_poll_start();
	} else {
		cover_request_outstanding = false;
	}
}

// A track change.
//
// The next track of a record carries the record's cover, and so does the next
// file in a folder that keeps its picture beside the music. Decoding it again
// is a second of this processor's time for a picture that does not change, and
// the cover blinking out to the placeholder and back is the part the user sees.
//
// So nothing is thrown away yet: the loader is asked what the new track's
// artwork is -- which it answers by reading the picture, not by decoding it --
// and only an answer that differs from what is on screen costs anything. When
// it does differ the old cover stays up for the one poll that takes, which
// still reads better than a placeholder in the gap.
static void refresh_cover(const char *filepath) {
	if (!cover_img)
		return;

	snprintf(cover_shown_path, sizeof(cover_shown_path), "%s", filepath ? filepath : "");

	if (cover_shown_id != 0 && current_cover.pixels && filepath && filepath[0]) {
		cover_id_state = COVER_ID_NEW_TRACK;
		coverloader_request_player_id(filepath);
		lv_timer_resume(progress_slider_timer);
		cover_poll_start();
		return;
	}

	cover_id_state = COVER_ID_IDLE;
	sx_artview_refresh();
	cover_incoming_id = 0;
	reload_cover(filepath);
}

// The answer to whichever question was asked, picked up on the progress poll.
static void apply_cover_id_result(void) {
	if (cover_id_state == COVER_ID_IDLE) {
		return;
	}
	uint64_t id = 0;
	bool finished = false;
	coverloader_take_player_id(&id, &finished);
	if (!finished) {
		return;
	}

	if (cover_id_state == COVER_ID_MEASURE) {
		cover_id_state = COVER_ID_IDLE;
		cover_shown_id = id;
		np_mini_from_cover();
		return;
	}

	cover_id_state = COVER_ID_IDLE;

	// The same picture: the cover, the backdrop, the sleeve colour and the
	// chrome all stay exactly as they are. This is the whole point.
	if (id != 0 && id == cover_shown_id) {
		np_mini_from_cover();
		return;
	}

	// Different, and its id is already known: the decode does not have to be
	// asked what it produced.
	cover_incoming_id = id;
	reload_cover(cover_shown_path);
}

// Picks up the worker's finished artwork, if any. Runs on the UI thread from
// the progress timer, so touching widgets here is allowed.
static void apply_cover_result(void) {
	if (!cover_request_outstanding) {
		return;
	}

	cover_image_t new_cover, new_backdrop;
	bool finished = false;
	bool has_cover = coverloader_take_player(&new_cover, &new_backdrop, &finished);

	if (!finished) {
		return; // still decoding; the placeholder stays up
	}
	cover_request_outstanding = false;

	if (has_cover) {
		// What is now on screen, so the next track can be asked whether its
		// artwork is this same picture. Usually known already; when it is not
		// -- the first cover after the player opens, a station's icon -- the
		// loader is asked, because working the id out reads the file and this
		// is the interface thread.
		cover_shown_id = cover_incoming_id;
		if (cover_shown_id == 0 && cover_shown_path[0]) {
			cover_id_state = COVER_ID_MEASURE;
			coverloader_request_player_id(cover_shown_path);
			cover_poll_start();
		}

		current_cover = new_cover;
		current_cover.dsc.data = current_cover.pixels; // struct moved, buffer didn't

		sx_artview_bind(cover_img, &current_cover.dsc, current_cover.display_generation);
		lv_obj_set_size(cover_img, current_cover.dsc.header.w, current_cover.dsc.header.h);
		lv_obj_center(cover_img);

		cover_show(true);

		if (new_backdrop.pixels) {
			current_backdrop = new_backdrop;
			current_backdrop.dsc.data = current_backdrop.pixels;
		}
		// The picture itself is up: the stretched thumbnail that stood in for
		// it goes, and the mini player gets a copy when the library had none.
		np_thumb240_drop();
		np_mini_from_cover();

		// The dark backdrop is up: light chrome, in both themes.
		paint_player_chrome();

		uint32_t tone = 0;
		int luma = 0;
		bool light = false;
		if (current_cover.has_tone) {
			tone = current_cover.tone;
		} else if (!cover_page_tone(&current_cover, COVER_PAGE_LIGHT_FROM, &tone, &luma, &light)) {
			tone = 0;
		}
		album_tone = tone ? cover_tone_stretched(tone) : 0;
		paint_player_tint();

		// The words' background, ready before they are pulled in.
		lyrics_bg_prepare();

		lv_obj_invalidate(cover_panel);
		lv_obj_invalidate(player_menu);
	}
	if (!has_cover) {
		cover_shown_id = 0; // nothing to compare the next track against
		lyrics_bg_forget(); // no picture, nothing to blur
		// Outside the library the mini player shows a copy of the last
		// picture decoded here: a stream after a stream (a Subsonic record,
		// then My Wave) would otherwise keep the previous sleeve under the
		// new title. Nothing decoded now, so this takes the copy away.
		np_mini_from_cover();
	}
	bar_over_player_update();
	if (lyrics_progress > 0) {
		lyrics_look(true);
	}
	sx_artview_refresh();
	cover_incoming_id = 0;

	// No artwork: the placeholder is already showing.

	prefetch_next_cover();
}

// Collects whatever the loader has answered, quickly, for as long as anything
// is outstanding; then the timer stops until the next question.
static void cover_poll_cb(lv_timer_t *timer) {
	apply_cover_id_result();
	apply_cover_result();
	if (!cover_request_outstanding && cover_id_state == COVER_ID_IDLE) {
		lv_timer_pause(timer);
	}
}

static void cover_poll_start(void) {
	if (!cover_poll_timer) {
		cover_poll_timer = lv_timer_create(cover_poll_cb, COVER_POLL_MS, NULL);
	}
	lv_timer_resume(cover_poll_timer);
}

// The artwork the player has already decoded for the loaded track, lent to
// anything else that wants to draw it (the screensaver). NULL while a track
// has no cover or its decode is still in flight -- never a stale pointer:
// refresh_cover() zeroes both before the buffers are freed.
const lv_image_dsc_t *player_cover_image(void) { return current_cover.pixels ? &current_cover.dsc : NULL; }

uint32_t player_cover_generation(void) { return current_cover.display_generation; }
uint32_t player_backdrop_generation(void) { return current_backdrop.display_generation; }

const lv_image_dsc_t *player_backdrop_image(void) { return current_backdrop.pixels ? &current_backdrop.dsc : NULL; }

static void update_repeat_button(void);

// Repaints the bits that are coloured by hand, and rebuilds the blurred
// artwork behind the controls -- it is dimmed towards the page background, so
// it has to be regenerated when that background changes.
static void player_refresh_theme(void) {
	if (!cover_panel)
		return;

	// 2.0: always dark, whatever the theme -- the plaque in the album's tone
	// and a white glyph on it. A new accent can change the palette, so the
	// gradients are looked at again (made again only if their colours moved).
	if (!lyrics_look_on) {
		lv_obj_set_style_bg_color(cover_panel, sx_pal()->np_bg, 0);
	}
	lv_obj_set_style_image_recolor(cover_placeholder_icon, sx_placeholder_ink(sx_pal()->mini_cover_tone), 0);
	np_paint_backgrounds();
	// The backdrop over a loaded cover is always the dark treatment -- same
	// look in both themes; only the no-cover panel follows the theme.
	cover_set_backdrop_light(false);
	lv_obj_set_style_bg_color(progress_slider, chrome_accent(), LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(progress_slider, chrome_accent(), LV_PART_KNOB);
	paint_player_chrome(); // repaints the play disc for the new palette
	paint_player_tint();
	update_repeat_button();
	update_speed_button();

	device_state_t state;
	device_state_get(&state);
	refresh_cover(state.current_file);
	if (sheet_open && power_screen_is_on()) {
		np_chain_refresh(&state);
	}
	if (lyrics_pane) {
		lyrics_look(lyrics_look_on); // the words in the new theme's colours
	}
}

static void update_service_badge(void);

// Refreshes the now-playing info (title + artist) from the current
// device state and resets the per-track length cache. Used both when the user
// picks a file and when playback auto-advances to a new track.
static void refresh_now_playing(void) {
	perfstat_phase("refresh.begin");
	current_total_length = 0; // unknown until the playback thread reports it

	// The library lists carry an accent mark on the playing track (and on the
	// album and artist it belongs to). This is the one place every track
	// change goes through, so it is where they are told.
	medialist_notify_now_playing();
	browser_notify_now_playing();
	trackmenu_notify_now_playing();

	// Wind the progress display back to the start straight away, so the bar
	// never sits at the previous track's position while the new one loads.
	lv_slider_set_value(progress_slider, 0, LV_ANIM_OFF);
	set_progress_label(0, 0);

	device_state_t state;
	device_state_get(&state);

	// The 2.0 palette is drawn from the cover's hue (sx_palette.h): a new
	perfstat_phase("refresh.listeners_state_done");
	// album recolours the interface here, before anything below paints.
	if (state.live) {
		sx_palette_track_changed(NULL, state.metadata.title);
		sx_mini_set_track(state.metadata.title, NULL, state.metadata.artist, true);
	} else {
		sx_palette_track_changed(state.current_file[0] ? state.current_file : NULL, state.metadata.album[0] ? state.metadata.album : NULL);
		// The mini player on every page says the same as this one.
		const char *base = strrchr(state.current_file, '/');
		const char *shown = state.metadata.title[0] ? state.metadata.title : (base ? base + 1 : state.current_file);
		sx_mini_set_track(state.current_file[0] ? shown : NULL, state.current_file[0] ? state.current_file : NULL, state.metadata.artist, false);
	}
	sx_mini_set_playing(state.status == AUDIO_STATUS_PLAYING);
	perfstat_phase("refresh.palette_mini_done");
	// The album the track belongs to, for its thumbnails (the mini player's
	// 56 px, the 240 px that stands in for the artwork), and the page's two
	// gradients in its colour -- made again only when the colour moved.
	np_album_changed(state.current_file, state.live);
	np_paint_backgrounds();

	apply_live_mode(state.live);
	perfstat_phase("refresh.album_background_done");
	apply_connecting();
	// What comes next, and the chain, for the new track.
	upnext_update();
	np_chain_refresh(&state);
	perfstat_phase("refresh.upnext_chain_done");
	// Before anything is drawn: which arrangement this source gets decides
	// where the title goes and what colour everything is.
	update_layout(&state);
	// Decided from the path every time, as for books: a flag raised at start
	// would have to be cleared on every route by which the track can change,
	// and the first one forgotten leaves the player dressed as a podcast over
	// a song.
	apply_audiobook_mode(!state.live && audiobook_is_playing(), !state.live && podcastcache_is_episode(state.current_file));
	chapters_refresh();
	perfstat_phase("refresh.layout_chapters_done");

	if (state.live) {
		// The station on top, what it says is on air underneath -- the same
		// two lines as a track, carrying the two things a radio knows.
		scrolltext_set(song_title_label, state.metadata.title);
		scrolltext_set(song_artist_label, state.metadata.artist);

		update_fav_button();
		update_format_label(&state);

		// The artwork is the station's own icon, downloaded when it started.
		// An empty path simply leaves the radio placeholder up.
		refresh_cover(state.live_cover);
		return;
	}

	const char *file = state.current_file;
	const char *slash = strrchr(file, '/');
	scrolltext_set(song_title_label, state.metadata.title[0] ? state.metadata.title : (slash ? slash + 1 : file));

	// Whose name goes under the title: see metadata_shown_artist().
	const char *artist = metadata_shown_artist(&state.metadata);
	scrolltext_set(song_artist_label, artist);

	update_fav_button();
	update_format_label(&state);
	update_service_badge();
	perfstat_phase("refresh.texts_done");

	lyrics_request(&state);

	refresh_cover(file);
	perfstat_phase("refresh.end");
}

// The parts of the now-playing display a live stream changes while it runs:
// the station name (the stream's own name arrives with the connection), what
// is on air (an ICY title, every few minutes), the bitrate, and the artwork
// once the icon has been downloaded. Deliberately not a full
// refresh_now_playing(): that throws the decoded cover away and asks for it
// again, which for a station whose icon has not changed is work for nothing.
static void refresh_live_texts(const device_state_t *state) {
	sx_mini_set_text(state->metadata.title, state->metadata.artist);
	scrolltext_set(song_title_label, state->metadata.title);
	scrolltext_set(song_artist_label, state->metadata.artist);
	update_fav_button();
	update_format_label(state);

	if (strcmp(cover_shown_path, state->live_cover) != 0) {
		refresh_cover(state->live_cover);
	}
}

// The mirror of a service's favourites has arrived, so the star may need to
// change face. It comes from the service's sync worker, hence the bounce onto
// the UI thread.
static void update_fav_button(void);

static void fav_refresh_async(void *user) {
	(void)user;
	update_fav_button();
}

static void service_favorites_changed(void) { gui_post(fav_refresh_async, NULL); }

// The streaming service the loaded track comes from, NULL when what is playing
// is not a streamed track. This is the question that splits the star and "add
// to playlist" in two: a user's own file is written to the local database, a
// streamed track to the account. The caches live in different directories, so
// a path can belong to at most one service -- the player cannot believe a
// track comes from two by construction.
static const stream_provider_t *current_service(char *path, size_t size) {
	device_state_t state;
	device_state_get(&state);
	const stream_provider_t *service = stream_provider_for_track(state.current_file);
	if (service && path && size) {
		snprintf(path, size, "%s", state.current_file);
	}
	return service;
}

// The podcast the playing episode belongs to, plus what following it needs:
// title, author and cover. All from the sidecar written when the episode was
// downloaded -- no network, which listening to an already cached episode may
// well not have.
static bool podcast_current_feed(long long *id_out, podcast_feed_t *feed_out) {
	device_state_t state;
	device_state_get(&state);
	if (state.live || !state.current_file[0]) {
		return false;
	}
	long long id = 0;
	if (!podcastcache_feed_id(state.current_file, &id) || id <= 0) {
		return false;
	}
	if (id_out) {
		*id_out = id;
	}
	if (feed_out) {
		memset(feed_out, 0, sizeof(*feed_out));
		feed_out->id = id;
		// The podcast name is also the episode's album tag: see
		// podcastcache_write_sidecars(), which writes it to both places on
		// purpose.
		snprintf(feed_out->title, sizeof(feed_out->title), "%.*s", (int)sizeof(feed_out->title) - 1, state.metadata.album);
		podcastcache_tag(state.current_file, "feed_author", feed_out->author, sizeof(feed_out->author));
		podcastcache_tag(state.current_file, "feed_image", feed_out->image, sizeof(feed_out->image));
	}
	return true;
}

static bool current_podcast_episode(void) {
	device_state_t state;
	device_state_get(&state);
	return state.current_file[0] && podcastcache_is_episode(state.current_file);
}

// The playing episode's podcast, for callers outside the player (the control
// centre): the same lookup the player uses for its own star, exposed so both
// stars say the same thing.
bool player_current_podcast_feed(long long *id_out, podcast_feed_t *feed_out) { return podcast_current_feed(id_out, feed_out); }

// The service's mark, top right, when what is playing comes from Qobuz, Tidal
// or another streaming service. Same place and same purpose as the live badge
// for radio: say where the music comes from without taking space from anything
// else.
//
// The question is the star's own -- is the file in a service's cache? -- so
// there is no extra state to keep in step.
static void update_service_badge(void) {
	// The marks share one corner and can never appear together: a track comes
	// from one service only. So which one shows is decided here, rather than
	// leaving each badge to hide itself -- which is how they would eventually
	// have overlapped. One image serves every service: its picture is the
	// service's own, swapped here.
	const stream_provider_t *service = live_mode ? NULL : current_service(NULL, 0);
	bool from_service = service && service->gui.badge;
	bool from_podcast = !live_mode && !service && current_podcast_episode();

	if (service_badge) {
		if (from_service) {
			lv_image_set_src(service_badge, service->gui.badge);
			lv_obj_set_hidden(service_badge, false);
			lv_obj_move_foreground(service_badge);
		} else {
			lv_obj_set_hidden(service_badge, true);
		}
	}
	if (podcast_badge) {
		if (from_podcast) {
			lv_obj_set_hidden(podcast_badge, false);
			lv_obj_move_foreground(podcast_badge);
		} else {
			lv_obj_set_hidden(podcast_badge, true);
		}
	}
}

// Whether what is playing is starred, by whichever list owns its star. Shared
// by the star itself and the side button's "add to favourites", so the two can
// never disagree about what a press would do.
static bool fav_current_starred(void) {
	// A station is starred in the radio favourites, not the music library:
	// there is no file to put in the database, and the list the star feeds is
	// the one on the Radio page.
	radio_station_t station;
	bool starred = false;
	const stream_provider_t *service = NULL;
	char service_path[512];
	long long feed_id = 0;
	if (radio_current_station(&station)) {
		starred = radio_fav_contains(station.uuid);
	} else if (podcast_current_feed(&feed_id, NULL)) {
		// On a podcast the star does not mark a liked track: it follows the
		// podcast, so it turns up in the followed list without searching. The
		// list is on the card, so the answer is immediate and nothing has to
		// be asked of the network.
		starred = podcastsubs_is_followed(feed_id);
	} else if ((service = current_service(service_path, sizeof(service_path))) != NULL) {
		// A streamed track: the star reports what is on the account, not what
		// is in the local database. Until the mirror arrives it is drawn empty
		// and asked for -- one request per session, not one per song.
		starred = service->fav_get(service_path);
		service->fav_refresh(service_favorites_changed);
	} else {
		// device_state, not audio_get_current_file(): the star is drawn from
		// one source and toggled from another, and when the two disagreed the
		// star never filled in -- the tap wrote the right track to the
		// database while the drawing looked up a different one.
		device_state_t state;
		device_state_get(&state);
		starred = state.current_file[0] && library_fav_contains(state.current_file);
	}
	return starred;
}

// Repaints the star from the database: filled yellow when the loaded track
// is a favourite, quiet outline otherwise.
static void update_fav_button(void) {
	if (!fav_btn_icon) {
		return;
	}
	char account_path[512];
	const stream_provider_t *account = current_service(account_path, sizeof(account_path));
	lv_obj_set_hidden(fav_btn_obj, audiobook_mode || (account && account->read_only));
	bool starred = fav_current_starred();

	// The star's colour is decided here and nowhere else. This function runs
	// last on every track change, so anything that tints the star elsewhere is
	// overwritten.
	//
	// Keep the original family's star colors. New families use readable
	// foreground roles: fixed yellow disappears on the bright 2077 page.
	if (starred) {
		lv_image_set_src(fav_btn_icon, &icon_star_filled);
		lv_obj_set_style_image_recolor(fav_btn_icon, theme_get_palette() != 0 ? sx_pal()->star_on : lv_color_make(246, 211, 45), 0);
	} else {
		lv_image_set_src(fav_btn_icon, &icon_star);
		lv_obj_set_style_image_recolor(fav_btn_icon, theme_get_palette() != 0 ? sx_pal()->np_text2 : lv_color_make(150, 150, 150), 0);
	}
	lv_obj_set_style_image_recolor_opa(fav_btn_icon, LV_OPA_COVER, 0);
}

// The outcome of a streamed track's star, from the service's sync worker. Success says
// nothing -- the filled star already did; failure has to be said, because the
// star had filled in optimistically and is about to empty again.
static char fav_error[192];

static void fav_error_async(void *user) {
	(void)user;
	if (fav_error[0]) {
		gui_notify_popup(fav_error);
	}
	update_fav_button();
}

// `user` is the provider that was written to: its fallback message names the
// service, so a failed Tidal star does not send the user looking at Qobuz.
static void service_fav_done(bool ok, const char *error, void *user) {
	const stream_provider_t *service = user;
	snprintf(fav_error, sizeof(fav_error), "%s", ok ? "" : (error && error[0] ? error : tr(service->fav_failed_key)));
	gui_post(fav_error_async, NULL);
}

static void fav_btn_event_cb(lv_event_t *e) {
	(void)e;

	radio_station_t station;
	if (radio_current_station(&station)) {
		if (radio_fav_contains(station.uuid)) {
			radio_fav_remove(station.uuid);
		} else {
			radio_fav_add(&station);
		}
		update_fav_button();
		return;
	}

	// A podcast: the star follows or unfollows it. Written to the card at
	// once, so switching the player off a moment later does not lose it.
	long long feed_id = 0;
	podcast_feed_t feed;
	if (podcast_current_feed(&feed_id, &feed)) {
		if (podcastsubs_is_followed(feed_id)) {
			podcastsubs_unfollow(feed_id);
		} else {
			podcastsubs_follow(&feed);
		}
		update_fav_button();
		return;
	}

	device_state_t state;
	device_state_get(&state);
	if (!state.current_file[0]) {
		return;
	}

	// A streamed track goes into the account's favourites. Saving it in the
	// local database would mean keeping the path of a cache file that will be
	// gone twenty tracks from now, and invisible from any other device.
	const stream_provider_t *service = stream_provider_for_track(state.current_file);
	if (service) {
		bool want = !track_favourite_get(state.current_file);
		track_favourite_set(state.current_file, state.metadata.title, state.metadata.artist, want, service_fav_done, (void *)service);
		update_fav_button(); // the mirror has already flipped: shows at once
		return;
	}

	const char *title = state.metadata.title[0] ? state.metadata.title : strrchr(state.current_file, '/') + 1;
	// No notification popup: the star filling in is the whole feedback.
	track_favourite_set(state.current_file, title, state.metadata.artist, !track_favourite_get(state.current_file), NULL, NULL);
	update_fav_button();
}

// The format line, the way the stock player writes it: "16/44.1 FLAC".
static void update_format_label(const device_state_t *state) {
	// The Bluetooth line under the artist (btline.h) is the same kind of
	// question, asked at the same moments.
	btline_refresh();

	if (!format_label) {
		return;
	}

	if (state->live) {
		// A lossy station is quoted by its bitrate, a lossless one by depth and
		// rate like a track from the card ("16/44.1 FLAC"). The codec is the
		// decoder's, not the directory's claim. Nothing at all until the first
		// frame has been decoded.
		radio_now_t now;
		radio_get_now(&now);
		if (now.bits > 0 && now.sample_rate > 0 && now.codec[0]) {
			lv_label_set_text_fmt(format_label, "%d/%g %s", now.bits, now.sample_rate / 1000.0, now.codec);
		} else if (now.bitrate > 0) {
			lv_label_set_text_fmt(format_label, "%d kbps %s", now.bitrate, now.codec[0] ? now.codec : "MP3");
		} else if (now.codec[0]) {
			lv_label_set_text(format_label, now.codec);
		} else {
			lv_label_set_text(format_label, "");
		}
		return;
	}

	if (state->stream_sample_rate <= 0 || !state->current_file[0]) {
		lv_label_set_text(format_label, "");
		return;
	}

	// Nothing on a podcast. "16/44.1 MP3" is true and useless: an episode is
	// not chosen for its audio quality, there is no better version to buy, and
	// the number takes the line under the title without saying anything worth
	// knowing.
	if (podcastcache_is_episode(state->current_file)) {
		lv_label_set_text(format_label, "");
		return;
	}

	const char *dot = strrchr(state->current_file, '.');
	char kind[8] = "";
	if (dot) {
		size_t k = 0;
		for (const char *c = dot + 1; *c && k < sizeof(kind) - 1; c++) {
			kind[k++] = (char)((*c >= 'a' && *c <= 'z') ? *c - 32 : *c);
		}
		kind[k] = '\0';
	}

	// A DSD track says what it is rather than what carries it: "24/176.4 DSF"
	// is true of a DSD64 file and of a DSD256 one, and tells the listener
	// nothing. How it gets there is not said: DoP is the only way it does.
	int dsd = audio_get_dsd_multiple();
	if (dsd > 0) {
		lv_label_set_text_fmt(format_label, "DSD%d", dsd);
		return;
	}

	// The codec name comes from the decoder, not the extension: an .m4a holds
	// either AAC or ALAC, and those read as two different things under a
	// cover. When the decoder has no name to give -- the libsndfile formats --
	// the extension still serves.
	char codec[16] = "";
	audio_get_stream_codec(codec, sizeof(codec));
	const char *name = codec[0] ? codec : kind;

	// A lossy format has no bit depth of its own: "16/44.1 MP3" describes the
	// PCM leaving the decoder, not the file, and is equally true of a 96 kbps
	// MP3 and a 320 kbps one. What an MP3, an AAC or an Opus is -- exactly as
	// for a radio station -- is its bitrate.
	if (audio_stream_is_lossy()) {
		// The nominal figure first, the one written in the file. It is the
		// only one showable on a Qobuz or Tidal track: the cache file is still
		// growing while it plays, so the bytes-over-seconds average climbed on
		// screen until the download finished and it settled.
		int kbps = audio_get_stream_bitrate_kbps();

		// When the file does not declare one (a variable-bitrate MP3, a silent
		// AAC) the real average stands in -- but only once the download has
		// finished, for the reason just given.
		struct stat st;
		if (kbps <= 0 && !stream_provider_owns(state->current_file) && state->progress_total_secs > 0.5 && stat(state->current_file, &st) == 0 && st.st_size > 0) {
			kbps = (int)((double)st.st_size * 8.0 / state->progress_total_secs / 1000.0 + 0.5);
		}

		if (kbps > 0) {
			lv_label_set_text_fmt(format_label, "%d kbps %s", kbps, name);
			return;
		}
		// Neither one: the format name alone beats quoting a bit depth the
		// file does not have.
		lv_label_set_text(format_label, name);
		return;
	}

	int bits = audio_get_stream_bits();
	lv_label_set_text_fmt(format_label, "%d/%g %s", bits > 0 ? bits : 16, state->stream_sample_rate / 1000.0, name);
}

// The speed setting of what is playing: an episode has its own, apart from the
// books'.
static bool speed_is_podcast(void) { return podcast_mode && !audiobook_mode; }

static int speed_current_permille(void) { return speed_is_podcast() ? podcast_speed_permille() : audiobook_speed_permille(); }

// The gauge wears the accent colour whenever the book or episode is not at
// normal speed, the way the repeat glyph it stands in for marks a mode that is
// on.
static void update_speed_button(void) {
	if (!speed_btn_icon) {
		return;
	}
	// 2.0: as the repeat button it stands in for -- white, and at 45 % while
	// it is at its rest (normal speed).
	bool normal = speed_current_permille() == AUDIOBOOK_SPEED_NORMAL;
	lv_obj_set_style_image_recolor(speed_btn_icon, normal ? (sx_pal()->text) : chrome_accent(), 0);
	lv_obj_set_style_image_recolor_opa(speed_btn_icon, LV_OPA_COVER, 0);
	lv_obj_set_style_image_opa(speed_btn_icon, normal ? NP_REPEAT_OFF_OPA : LV_OPA_COVER, 0);
}

static void speed_pick(void *user) {
	int permille = (int)(intptr_t)user;
	if (speed_is_podcast()) {
		podcast_set_speed_permille(permille);
		permille = podcast_speed_permille();
	} else {
		audiobook_set_speed_permille(permille);
		permille = audiobook_speed_permille();
	}
	// Straight through to the playback thread: it picks the factor up on the
	// next block, so the change is heard within a period rather than at the
	// next track.
	audio_set_speed((double)permille / 1000.0);
	update_speed_button();
}

static void speed_btn_event_cb(lv_event_t *e) {
	static const int CHOICES[] = {500, 750, 1000, 1250, 1500, 1750, 2000};
	static const char *const LABELS[] = {"0.5x", "0.75x", "1.0x", "1.25x", "1.5x", "1.75x", "2.0x"};
	enum { CHOICE_COUNT = sizeof(CHOICES) / sizeof(CHOICES[0]) };

	// Zeroed, not just filled field by field: popover_item_t also has
	// `checked`, and stack garbage in it puts a tick on a random entry -- along
	// with the different row layout a tick brings.
	//
	// The tick belongs here for the same reason it does in the Qobuz quality
	// menu: this is a choice between alternatives, not a list of actions, so
	// the current one has to be visible.
	popover_item_t items[CHOICE_COUNT] = {0};
	int current = speed_current_permille();
	for (int i = 0; i < CHOICE_COUNT; i++) {
		items[i].label = LABELS[i];
		items[i].action = speed_pick;
		items[i].user = (void *)(intptr_t)CHOICES[i];
		items[i].checked = CHOICES[i] == current;
	}
	popover_show(lv_event_get_current_target(e), items, CHOICE_COUNT);
}

// Updates the repeat-mode button's icon and colour to reflect the active mode.
// The icons are the white ARGB bitmaps generated from assets/icons/*.svg;
// recolouring them is what marks the mode as active or not.
static void update_repeat_button(void) {
	// 2.0: white, as everything else on the panel; a mode that is off is the
	// same glyph at 45 % (the handoff's ⟳ / ⟳1 / off). Shuffle, which the
	// button keeps showing when it is on, has its two glyphs of old.
	const lv_image_dsc_t *icon;
	bool on = true;
	switch (playlist_get_mode()) {
	case PLAYBACK_MODE_REPEAT_ONE:
		icon = &icon_repeat_one;
		break;
	case PLAYBACK_MODE_REPEAT_ALL:
		icon = &icon_repeat_all;
		break;
	case PLAYBACK_MODE_SHUFFLE:
		icon = &icon_shuffle;
		break;
	case PLAYBACK_MODE_SHUFFLE_REPEAT:
		icon = &icon_shuffle_repeat;
		break;
	case PLAYBACK_MODE_NORMAL:
	default:
		icon = &icon_repeat_all;
		on = false;
		break;
	}

	lv_image_set_src(repeat_btn_icon, icon);
	lv_obj_set_style_image_recolor(repeat_btn_icon, (false) ? lv_color_white() : sx_pal()->text, 0);
	lv_obj_set_style_image_recolor_opa(repeat_btn_icon, LV_OPA_COVER, 0);
	lv_obj_set_style_image_opa(repeat_btn_icon, on ? LV_OPA_COVER : NP_REPEAT_OFF_OPA, 0);
}

// Called when the current track finished on its own: advances the folder queue
// per the active playback mode. If a next track starts, refresh the UI to it;
// otherwise playback simply stays stopped (end of folder in normal mode).
static void handle_track_finished(void) {
	// What is playing came from DLNA: the queue belongs to the phone, not to
	// the player. Tell it the track finished and wait for the next URL,
	// instead of drawing from a local queue one entry long.
	if (dlna_owns_playback()) {
		dlna_notify_finished();
		return;
	}

	// "Stop at the end of the episode": the queue is left exactly where it is,
	// which is what the player already does when a queue simply runs out. Asked
	// here and not at a boundary inside the file, because for a podcast the end
	// of the episode IS the end of the track -- there is no chapter to watch
	// for, which is what makes this the cheaper half of the audiobook's version.
	char path[512];
	audio_get_current_file(path, sizeof(path));
	if (podcast_stop_at_episode_end() && podcastcache_is_episode(path)) {
		fprintf(stderr, "player: end of the episode; not starting the next\n");
		refresh_now_playing();
		return;
	}

	if (device_state_advance_auto(NULL, 0)) {
		refresh_now_playing();
	} else {
		// The queue ran out: the track it ended on went into the history.
		trackmenu_notify_now_playing();
	}
}

// A track ended on its own: the next one is started now rather than at the
// next poll, while the PCM held for gapless still has its queue to play.
static void track_end_async(void *user) {
	(void)user;
	if (device_state_take_completion()) {
		handle_track_finished();
	}
}

// Playback thread.
static void track_end_hook(void) { gui_post_latest(track_end_async, NULL); }

// Reads the current device state and reconciles the UI against it. This is the
// single point that keeps the play/pause button and the progress bar from
// going stale once a track finishes on its own.
static void update_progress(void) {
	perfstat_phase("update.begin");
	// Auto-advance or loop the folder when the track has played through.
	if (device_state_take_completion()) {
		handle_track_finished();
	}

	// Read the revision before its snapshot so a later change stays pending.
	unsigned serial = radio_now_serial();

	device_state_t state;
	device_state_get(&state);

	double heard_position, heard_length;
	unsigned heard_serial;
	bool heard_ready = audio_get_listening_progress(&heard_position, &heard_length, &heard_serial);
	if (heard_ready && !state.live && state.status == AUDIO_STATUS_PLAYING) {
		playhistory_heard(state.current_file);
		perfstat_phase("update.history_done");
		playbackplace_heard(state.current_file);
	} else
		playhistory_poll();
	audiobook_listen_progress(heard_position, heard_length, heard_ready && !state.live && state.status == AUDIO_STATUS_PLAYING, heard_serial);
	perfstat_phase("update.place_book_done");

	// The live source moves under the player: the station's own name comes
	// with the connection, the on-air title arrives later and keeps changing,
	// the artwork lands when its download finishes -- and the whole thing
	// ends by itself when a station drops for good. None of that goes through
	// a track change, so it is followed here.
	{
		static bool was_live;
		static unsigned last_serial;

		if (state.live != was_live) {
			was_live = state.live;
			last_serial = serial;
			// In or out of live: the whole display changes hands.
			refresh_now_playing();
			device_state_get(&state);
		} else if (state.live && serial != last_serial) {
			last_serial = serial;
			refresh_live_texts(&state);
		}
	}

	// Asked on every poll and not only at a track change, for the routes that
	// are not one: a phone claiming the output over AirPlay does not change the
	// track -- it takes the DAC away under the one that is loaded -- and the
	// answer here has to follow it. Two string compares and a struct copy,
	// twice a second.
	update_layout(&state);
	// Likewise the transport: radio.txt can be reloaded under a station, which
	// decides whether previous and next are there for it.
	apply_live_mode(state.live);
	apply_connecting();

	// The bar itself is set by set_progress_label() below, which knows
	// whether it stands for the file or for the chapter.
	if (state.progress_total_secs > 0) {
		current_total_length = state.progress_total_secs;
	}

	// The truth the smoothing works from, and the moment it was true. Taken
	// here and not in the smoothing itself, so the two can never drift: every
	// poll puts the interpolation back on the playback thread's own figure.
	progress_anchor_secs = state.progress_current_secs;
	progress_anchor_tick = lv_tick_get();
	progress_running = state.status == AUDIO_STATUS_PLAYING && state.progress_total_secs > 0;
	if (smooth_timer) {
		// Sixteen times a second only for a bar somebody sees: the player up
		// and the panel lit. (It used to run whenever music played and return
		// at once inside -- sixteen wake-ups a second for nothing.)
		if (progress_running && sheet_open && power_screen_is_on()) {
			lv_timer_resume(smooth_timer);
		} else {
			lv_timer_pause(smooth_timer);
		}
	}

	// A track the queue ran out on stands at its end, stopped; play starts it
	// from the top (device_state_toggle_play_pause), so that is what the bar
	// says -- the handoff's "the end of the queue is a pause".
	double shown = state.progress_current_secs;
	if (state.status == AUDIO_STATUS_STOPPED && !state.live && current_total_length > 2.0 && shown >= current_total_length - 1.0) {
		shown = 0;
	}
	set_progress_label(shown, current_total_length);
	sx_mini_set_progress(shown, state.live ? 0 : current_total_length);
	if (state.status == AUDIO_STATUS_PLAYING && !state.live) {
		device_state_read_ahead(state.progress_current_secs, state.progress_total_secs);
	}
	lyrics_request(&state);
	lyrics_follow(state.progress_current_secs);
	apply_playback_status(state.status);
	// Asked every poll rather than only on a track change: the answer comes
	// from the audiobook index, which a scan finishing can change under a
	// track that is already playing. Both answers come from the path every
	// time -- a flag raised at start would have to be cleared on every route
	// by which the track can change, and the first one forgotten leaves the
	// player dressed as a podcast over a song.
	apply_audiobook_mode(!state.live && audiobook_is_playing(), !state.live && podcastcache_is_episode(state.current_file));
	update_format_label(&state); // the rate/bits settle shortly after the start
	np_poll(&state);			 // 2.0: "Up next", the chain, sources outside the queue
	perfstat_phase("update.presentation_done");

	// The status LED follows playback promptly from here (the battery poll
	// only comes round once a minute). Only writes when the colour changes.
	// A podcast lights purple instead of the sample-rate colour.
	led_update_playback(state.status == AUDIO_STATUS_PLAYING, state.stream_sample_rate, !state.live && podcastcache_is_episode(state.current_file), !state.live && audio_get_dsd_multiple() > 0);

	// Where the book got to. Not the same thing as "remember track" below and
	// not subject to its switch: that setting is about which track comes back
	// on the next boot, while this is the whole point of a book -- it is
	// remembered per book, it survives listening to something else in between,
	// and turning it off would make the feature meaningless. Throttled inside
	// audiobook_note_position(); forced whenever playback stops or pauses, so
	// a shutdown loses at most a few seconds.
	if (!state.live && audiobook_mode) {
		bool stopped_itself = audiobook_auto_stop(&state);

		static audio_status_t book_last_status = AUDIO_STATUS_STOPPED;
		bool changed = state.status != book_last_status;
		book_last_status = state.status;
		// Not when it has just stopped itself: that path already wrote the
		// position it stopped at, and this snapshot is from before the seek.
		if (!stopped_itself) {
			audiobook_note_position(state.progress_current_secs, state.progress_total_secs, changed || state.status != AUDIO_STATUS_PLAYING);
		}
	}

	// Where a podcast episode got to, kept per episode in the same spirit:
	// throttled inside podcaststate_note(), forced when the playback state
	// changes.
	if (!state.live && podcastcache_is_episode(state.current_file)) {
		static audio_status_t episode_last_status = AUDIO_STATUS_STOPPED;
		bool episode_changed = state.status != episode_last_status;
		episode_last_status = state.status;
		podcaststate_note(state.current_file, state.progress_current_secs, state.progress_total_secs, episode_changed);
	}

	// "Remember track". The decision, and the writing, live in device_state.c:
	// the power menu and the automatic shutdown have to do the same thing on
	// their way out, and neither of them comes through here.
	device_state_remember_note();
	perfstat_phase("update.end");
}

static void play_btn_event_cb(lv_event_t *e) {
	(void)e;

	// A live stream has no position to come back to, so this is a stop, not a
	// pause: the connection goes, the station stays on screen, and pressing
	// play again reconnects to it. Not while it is still connecting.
	if (radio_is_active()) {
		if (radio_is_connecting()) {
			return;
		}
		if (radio_is_playing()) {
			radio_stop();
		} else {
			radio_resume();
		}
		player_refresh_now_playing();
		return;
	}

	audio_status_t new_status = device_state_toggle_play_pause();
	apply_playback_status(new_status); // optimistic UI update; next poll reconciles against the real status
}

// What the on-screen prev/next buttons do. On a book or a podcast they are
// jumps of seconds; on a track they are previous/next, Spotify style -- within
// the first few seconds of a track prev moves to the previous one, later it
// restarts the current one. Public so the control centre can do exactly this:
// the same buttons cannot mean two different things in the two places. (The
// physical side keys stay episode changes on a podcast: see
// player_key_prev/next.)
// Previous and next on a stream. True when the press was the stream's: it
// changed station along the list the station came from, or there was nowhere
// to go, or the station is still connecting (see apply_connecting()), and it
// did nothing. False when no station is loaded and the press is the queue's.
static bool live_step(int step) {
	if (!radio_is_active()) {
		return false;
	}
	if (radio_is_connecting()) {
		return true;
	}
	// A lone station is outside the queue (the handoff's `ext`): either way
	// goes back to the queue, to the track it had got to, from its start.
	if (!radio_can_step()) {
		int at = playlist_current_index();
		if (playlist_count() > 0 && device_state_play_queue_index(at >= 0 ? at : 0)) {
			player_refresh_now_playing();
		}
		return true;
	}
	if (radio_step(step)) {
		refresh_now_playing();
	}
	return true;
}

// A headset may spend several seconds recognising a second long press.
// Remember an explicit restart rather than racing the three-second playhead.
static char previous_restart_file[512];
static uint32_t previous_restart_at;
static unsigned previous_restart_metadata;
static bool previous_restart_pending;

static void previous_track(const device_state_t *state, bool remote) {
	bool repeated = previous_restart_pending && (uint32_t)(lv_tick_get() - previous_restart_at) <= 8000 && previous_restart_metadata == device_state_metadata_revision() && state->progress_current_secs <= 10.0 && strcmp(previous_restart_file, state->current_file) == 0;
	previous_restart_pending = false;
	if (state->progress_current_secs > 3.0 && !repeated) {
		device_state_seek(0);
		snprintf(previous_restart_file, sizeof(previous_restart_file), "%s", state->current_file);
		previous_restart_at = lv_tick_get();
		previous_restart_metadata = device_state_metadata_revision();
		previous_restart_pending = true;
	} else if (remote) {
		dlna_request_prev();
	} else if (device_state_prev(NULL, 0)) {
		refresh_now_playing();
	} else {
		device_state_seek(0);
	}
}

void player_screen_prev(void) {
	if (live_step(-1)) {
		return;
	}

	device_state_t state;
	device_state_get(&state);

	// On a book these two are not "previous chapter" and "next chapter": they
	// are ten seconds back and ten seconds on, which is what somebody who
	// missed a sentence reaches for. Chapters have their own list.
	if (audiobook_mode || podcast_mode) {
		int back = podcast_mode && !audiobook_mode ? podcast_skip_back() : audiobook_skip_back();
		audiobook_seek_by(-(double)back, &state);
		update_progress();
		return;
	}

	// Past the first three seconds, prev restarts the track instead of leaving
	// it: the same rule every player follows, so a double press is what goes
	// back.
	previous_track(&state, false);

	update_progress();
}

void player_screen_next(void) {
	previous_restart_pending = false;
	if (live_step(1)) {
		return;
	}

	if (audiobook_mode || podcast_mode) {
		device_state_t state;
		device_state_get(&state);
		int fwd = podcast_mode && !audiobook_mode ? podcast_skip_forward() : audiobook_skip_forward();
		audiobook_seek_by((double)fwd, &state);
		update_progress();
		return;
	}

	if (device_state_next(NULL, 0)) {
		refresh_now_playing();
	}
	update_progress();
}

static void prev_btn_event_cb(lv_event_t *e) {
	(void)e;
	player_screen_prev();
}

static void next_btn_event_cb(lv_event_t *e) {
	(void)e;
	player_screen_next();
}

// --- hardware transport keys ---
//
// They do exactly what the on-screen buttons do, whichever page is showing:
// the player does not have to be open for the keys on the side of the device
// to work.

void player_key_play_pause(void) { apply_playback_status(device_state_toggle_play_pause()); }

void player_key_next(void) {
	previous_restart_pending = false;
	if (live_step(1)) {
		return;
	}

	// On a track coming from DLNA, "next" is a question for the phone: the
	// queue is its own.
	if (dlna_owns_playback()) {
		dlna_request_next();
		return;
	}

	// The side keys stay previous/next track on a podcast, deliberately. A
	// book is a single file, so "next track" means nothing there and the keys
	// become jumps. A podcast sits in a real queue of episodes: if the side
	// keys also jumped thirty seconds, there would be no way to reach the next
	// episode without opening the queue. This way both exist -- jumps on
	// screen, episode change under the finger.
	if (audiobook_mode) {
		device_state_t state;
		device_state_get(&state);
		audiobook_seek_by((double)audiobook_skip_forward(), &state);
		update_progress();
		return;
	}

	if (device_state_next(NULL, 0)) {
		refresh_now_playing();
	}
	update_progress();
}

void player_key_prev(void) {
	if (live_step(-1)) {
		return;
	}

	device_state_t state;
	device_state_get(&state);

	if (dlna_owns_playback()) {
		// As elsewhere: within the first few seconds go back to the start,
		// later ask for the previous track -- only here the phone is asked.
		previous_track(&state, true);
		return;
	}

	if (audiobook_mode) {
		audiobook_seek_by(-(double)audiobook_skip_back(), &state);
		update_progress();
		return;
	}

	previous_track(&state, false);
	update_progress();
}

// "Add to favourites" from a side button: what the star does, but only ever the
// adding half. A key pressed by feel, in a pocket, must not take a favourite
// away -- pressed twice it says the track is already there. The star decides
// where it goes: the library, the account of the service the track streams
// from (its like), the radio favourites, a podcast followed.
void player_key_favourite(void) {
	device_state_t state;
	device_state_get(&state);
	if (!state.current_file[0] && !state.live) {
		toast_plain("trackmenu_nothing_is_playing");
		return;
	}
	if (audiobook_mode) {
		return; // the star is hidden on a book: a book is not a favourite track
	}
	if (fav_current_starred()) {
		toast_glyph(&icon_star_filled, tr("player_key_favourite_already"));
		return;
	}
	fav_btn_event_cb(NULL);
	toast_glyph(&icon_star_filled, tr("player_key_favourite_added"));
}

void player_key_next_album(void) {
	if (live_mode || dlna_owns_playback() || audiobook_mode) {
		return; // a station, the phone's queue, a book: no next record here
	}
	if (device_state_next_album(NULL, 0)) {
		refresh_now_playing();
	} else {
		toast_plain("player_key_no_next_album");
	}
	update_progress();
}

// The repeat/shuffle button's step, with its name said: the button shows the
// mode on screen, the key in a pocket has nothing to show it on.
void player_key_cycle_mode(void) {
	if (podcast_mode || audiobook_mode) {
		return; // the button is hidden there: these queues carry their own order
	}
	static const char *const NAMES[] = {
		[PLAYBACK_MODE_NORMAL] = "player_mode_normal", [PLAYBACK_MODE_REPEAT_ALL] = "player_mode_repeat_all", [PLAYBACK_MODE_REPEAT_ONE] = "player_mode_repeat_one", [PLAYBACK_MODE_SHUFFLE] = "player_mode_shuffle", [PLAYBACK_MODE_SHUFFLE_REPEAT] = "player_mode_shuffle_repeat",
	};
	static const lv_image_dsc_t *const GLYPHS[] = {
		[PLAYBACK_MODE_NORMAL] = &icon_repeat_off, [PLAYBACK_MODE_REPEAT_ALL] = &icon_repeat_all, [PLAYBACK_MODE_REPEAT_ONE] = &icon_repeat_one, [PLAYBACK_MODE_SHUFFLE] = &icon_shuffle, [PLAYBACK_MODE_SHUFFLE_REPEAT] = &icon_shuffle_repeat,
	};
	playback_mode_t mode = playlist_cycle_mode();
	update_repeat_button();
	if ((size_t)mode < sizeof(NAMES) / sizeof(NAMES[0])) {
		toast_glyph(GLYPHS[mode], tr(NAMES[mode]));
	}
}

// The book's menu. A book with no chapters still lists them, and says so when
// asked: a plainer answer than an entry that comes and goes between books.
static void book_chapters_action(void *user) {
	(void)user;
	if (audiobook_has_chapters) {
		chapters_open();
	} else {
		gui_notify_popup("player_this_audiobook_has_no_chapters_2");
	}
}

static void book_add_bookmark_action(void *user) {
	(void)user;
	audiobookextras_add_bookmark();
}

static void book_bookmarks_action(void *user) {
	(void)user;
	audiobookextras_open_current_bookmarks();
}

static void book_summary_action(void *user) {
	(void)user;
	audiobookextras_open_current_summary();
}

// 2.0: the sleep timer, as the music's menu has it -- the books' own timer
// (sleeptimer.h), which the Audiobooks settings set too.
static void book_sleep_action(void *user) {
	(void)user;
	player_sleep_sheet_open();
}

// The overflow button: the book's menu on a book, the episode queue on a
// podcast, the track menu on anything else.
static void more_btn_event_cb(lv_event_t *e) {
	if (audiobook_mode) {
		static const popover_item_t BOOK_ITEMS[] = {
			{"chapters", book_chapters_action, NULL}, {"audiobook_add_bookmark", book_add_bookmark_action, NULL}, {"bookmarks", book_bookmarks_action, NULL}, {"audiobook_summary", book_summary_action, NULL}, {"np_sleep", book_sleep_action, NULL},
		};
		popover_show(lv_event_get_current_target(e), BOOK_ITEMS, (int)(sizeof(BOOK_ITEMS) / sizeof(BOOK_ITEMS[0])));
		return;
	}
	// A podcast has no chapters: it has episodes, and the episodes are already
	// the queue. The button goes straight there.
	if (podcast_mode) {
		trackmenu_open_queue();
		return;
	}
	trackmenu_open(lv_event_get_current_target(e));
}

// All queue modes are named together; the physical key keeps its cycle.
static void repeat_btn_event_cb(lv_event_t *e) {
	(void)e;
	if (!player_sheet_drag_active())
		player_playback_mode_menu();
}

static void progress_slider_event_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);

	if (code == LV_EVENT_PRESSED) {
		lv_timer_pause(progress_slider_timer); // the poll must not fight the finger
		if (smooth_timer) {
			lv_timer_pause(smooth_timer); // nor the smoothing, which would drag it back
		}
	}

	if (code == LV_EVENT_RELEASED) {
		lv_timer_resume(progress_slider_timer);

		int value = lv_slider_get_value(progress_slider);

		double seconds = view_seconds(value);

		device_state_seek(seconds);

		lv_timer_resume(progress_slider_timer);
	}

	if (code == LV_EVENT_VALUE_CHANGED) {
		// While dragging, the left clock previews where the knob would land.
		int value = lv_slider_get_value(progress_slider);

		double seconds = view_seconds(value);

		formatDoubleSeconds(seconds - view_start, progress_label_text, sizeof(progress_label_text));
		lv_label_set_text(elapsed_label, progress_label_text);
	}
}

static void smooth_timer_cb(lv_timer_t *timer) {
	(void)timer;
	if (!progress_running || view_length <= 0.1) {
		return;
	}
	// Nothing is drawn for a screen nobody is looking at, or for a page that is
	// not the one on show. Drawing a dark panel is work this device cannot
	// spare and has paid for before.
	if (!sheet_open || !power_screen_is_on()) {
		return;
	}

	double seconds = progress_anchor_secs + (double)lv_tick_elaps(progress_anchor_tick) / 1000.0;
	if (seconds > view_start + view_length) {
		seconds = view_start + view_length; // the poll will say what happens next
	}

	int value = view_value(seconds);
	if (value != lv_slider_get_value(progress_slider)) {
		lv_slider_set_value(progress_slider, value, LV_ANIM_OFF);
	}
	lyrics_follow(seconds);
}

static void progress_slider_timer_cb(lv_timer_t *timer) {
	(void)timer;
	apply_cover_id_result(); // "is the new track's picture the one already up?"
	apply_cover_result();	 // artwork the worker has finished since last tick
	update_progress();
}

// The boot-time restore: the remembered track, paused at its position.
void player_restore_track(const char *filepath, double position) {
	device_state_restore_file(filepath, position);
	player_refresh_now_playing();
}

// The same, with the queue that was saved alongside it.
void player_restore_index(library_index_t *ix, int start_index, const char *const *extra, const int *extra_slots, int extra_count, const char *filepath, double position) {
	device_state_restore_index(ix, start_index, extra, extra_slots, extra_count, filepath, position);
	player_refresh_now_playing();
}

void player_restore_list(const char *const *list, int count, int start_index, bool custom, const char *filepath, double position) {
	device_state_restore_list(list, count, start_index, custom, filepath, position);
	player_refresh_now_playing();
}

// Loads and starts a new file, updating the now-playing info. Also rebuilds
// the folder queue inside device_state, so playback carries on to the
// following tracks when this one finishes.
void player_play_file(const char *filepath) {
	device_state_play_file(filepath); // builds folder queue, loads metadata, starts playback

	player_refresh_now_playing();
}

// audio_pause() rather than device_state_toggle_play_pause(): this is a pause,
// not a toggle, and the toggle would start a stopped radio station playing
// again. The button glyph is corrected here rather than left to the next poll
// because half a second of a pause button over a paused track reads as a
// control that did not take.

bool player_cover_accent(void) { return cover_accent; }

void player_cover_accent_set(bool on) {
	if (on == cover_accent) {
		return;
	}
	cover_accent = on;
	config_set_bool("screen", "cover_accent", on);
	config_save();
	paint_transport_tint();
}

void player_output_unplugged(const char *what) {
	if (audio_get_status() != AUDIO_STATUS_PLAYING) {
		return;
	}
	printf("player: %s unplugged; pausing\n", what ? what : "the output");
	audio_pause();
	apply_playback_status(AUDIO_STATUS_PAUSED);
}

// A pause and not a stop, and for the same reason as the one above: the point
// of falling asleep to something is finding it where it was in the morning.
static void sleep_timer_cb(lv_timer_t *timer) {
	(void)timer;
	bool expired = sleeptimer_expired(SLEEPTIMER_MUSIC);
	if (expired)
		spotify_sleep();
	// A station has no pause: its stop, as the button does it.
	if (expired && radio_is_active()) {
		if (radio_is_playing() || radio_is_connecting()) {
			radio_stop();
			apply_playback_status(AUDIO_STATUS_STOPPED);
		}
		return;
	}
	if (!expired || audio_get_status() != AUDIO_STATUS_PLAYING) {
		return;
	}

	// A book being put down has two things to settle that a track does not: the
	// rewind that the next play would otherwise add -- it is for a sentence
	// somebody interrupted, not for one the player stopped on purpose -- and
	// the position, written through rather than left to the ten second throttle.
	if (audiobook_is_playing()) {
		device_state_t state;
		device_state_get(&state);
		audiobook_suppress_rewind_once();
		audio_pause();
		apply_playback_status(AUDIO_STATUS_PAUSED);
		audiobook_note_position(state.progress_current_secs, 0, true);
		return;
	}

	audio_pause();
	apply_playback_status(AUDIO_STATUS_PAUSED);
}

// The artist line again, after "Use track artist" has been switched. Not a
// whole refresh_now_playing(): nothing else on the page has changed, and that
// would decode the cover and ask for the lyrics all over again.
void player_refresh_artist(void) {
	if (!song_artist_label) {
		return;
	}
	device_state_t state;
	device_state_get(&state);
	if (state.live || !state.current_file[0]) {
		return;
	}
	scrolltext_set(song_artist_label, metadata_shown_artist(&state.metadata));
}

// Public face of refresh_now_playing, for callers that started playback
// through device_state themselves (the library lists hand over a whole
// queue rather than a single file).
void player_refresh_now_playing(void) {
	refresh_now_playing();

	// Playback may have been started by something that also set the mode --
	// the favourites shuffle button forces shuffle on its way in -- so the
	// repeat/shuffle glyph is refreshed here too, not only when it is tapped.
	update_repeat_button();

	device_state_t state;
	device_state_get(&state);
	apply_playback_status(state.status);
}

// ---------------------------------------------------------------------------
// the sheet: the player comes up from the bottom and goes back down
//
// 2.0 (principle 7, "down puts the top layer away"): the player opens from
// the bottom -- the mini player's tap or upward swipe, or anything that
// switches to it -- and is put away downwards, by the chevron (pointing down
// now) or by pulling it down: it follows the finger and goes once let go past
// SX_NP_COLLAPSE_PX, otherwise it springs back. Sideways drags on the cover
// are the cover's own (coverflow: previous and next).
// ---------------------------------------------------------------------------

#define SHEET_ANIM_MS SX_ANIM_NAV_MS

static int sheet_height;
static lv_obj_t *sheet_under; // the page the sheet first slid over

// Where the finger went down, and whether the gesture has been taken as a
// pull down.
static lv_point_t drag_start_point;
static bool dragging;

// True once a gesture has moved far enough to count as a drag. Read by the
// tap handlers, so that pulling the player down across a button does not also
// press it.
static bool drag_committed;

static void sheet_anim_y_cb(void *obj, int32_t value) {
	lv_obj_set_y((lv_obj_t *)obj, (int)value);
	// No live blur under the status bar while the artwork slides beneath it:
	// see topbar_hold_backdrop_blur(). Back on the frame the sheet comes to
	// rest open.
	topbar_hold_backdrop_blur(value != 0);
	// The chevron belongs to the sheet while it moves: ride it down, and snap
	// back into the page's corner once the sheet is fully away.
	back_btn_translate_y(value >= sheet_height ? 0 : value);
}

// Moves the sheet to `y` (0 = fully open, sheet_height = fully out of the way).
static void sheet_artwork_opened(lv_anim_t *animation) {
	(void)animation;
	if (sheet_open && lv_obj_get_y(player_screen) == 0 && !screensaver_is_visible())
		sx_artview_opaque_overlay(player_screen);
}

static void sheet_move_to(int y, bool animate) {
	if (y < 0) {
		y = 0;
	}
	if (y > sheet_height) {
		y = sheet_height;
	}

	// Never race a slide that is still running. A slide left to finish after a
	// new one has started parks the sheet off screen while everything else
	// believes it is open -- the page underneath showing, no status bar, and no
	// way back to the player.
	lv_anim_delete(player_screen, sheet_anim_y_cb);

	if (!animate) {
		lv_obj_set_y(player_screen, y);
		back_btn_translate_y(y >= sheet_height ? 0 : y);
		topbar_hold_backdrop_blur(y != 0); // a drag: as in sheet_anim_y_cb
		if (!screensaver_is_visible()) {
			if (y > 0 && y < sheet_height)
				sx_artview_overlay_will_show(player_screen);
			else
				sx_artview_opaque_overlay(y == 0 && sheet_open ? player_screen : NULL);
		}
		return;
	}
	if (lv_obj_get_y(player_screen) == y) {
		sheet_artwork_opened(NULL);
		return;
	}

	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, player_screen);
	lv_anim_set_exec_cb(&a, sheet_anim_y_cb);
	lv_anim_set_values(&a, lv_obj_get_y(player_screen), y);
	lv_anim_set_completed_cb(&a, sheet_artwork_opened);
	lv_anim_set_duration(&a, SHEET_ANIM_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_start(&a);
}

bool player_sheet_is_open(void) { return sheet_open; }

bool player_sheet_drag_active(void) { return drag_committed || sheet_pull_active || sheet_side_active; }

static void bar_over_player_update(void) {
	if (!sheet_open || !bar_always)
		return;
	topbar_set_hidden(false);
	topbar_set_over_artwork(false, false);
	topbar_set_playback(true);
}

bool player_statusbar_always(void) { return bar_always; }

void player_set_statusbar_always(bool on) {
	if (on == bar_always) {
		return;
	}
	bar_always = on;
	back_btn_player_below_bar(on);

	// The marks in the artwork's corner, and everything lined up with the
	// chevron, follow it down or back up.

	lv_obj_t *const marks[] = {live_badge, service_badge, podcast_badge};
	for (size_t i = 0; i < sizeof(marks) / sizeof(marks[0]); i++) {
		if (marks[i]) {
			lv_obj_align(marks[i], LV_ALIGN_TOP_RIGHT, -BADGE_INSET, badge_top());
		}
	}

	lyrics_layout();
	np_layout_changed(); // the "Up next" chip is on the chevron's line

	if (!on) {
		if (sheet_open) {
			topbar_set_playback(false);
			topbar_set_over_artwork(false, false);
			topbar_set_hidden(true);
		}
		return;
	}
	bar_over_player_update();
}

static void sheet_x_cb(void *obj, int32_t x) {
	lv_obj_set_x(obj, x);
	back_btn_translate(x);
}

static void sheet_x_done(lv_anim_t *a) {
	(void)a;
	sheet_side_active = false;
	sheet_x_cb(player_screen, 0);
	if (sheet_side_close)
		player_sheet_close(false);
	else {
		sheet_artwork_opened(NULL);
		if (!bar_always)
			topbar_set_hidden(true);
	}
}

static void player_sheet_open_at(int y, bool animate) {
	// Nothing to show while another mode owns the sound: the sheet is about a
	// track this player is playing, and in Bluetooth receiver mode the track is
	// on somebody else's machine. Refusing to open is the honest answer -- an
	// opened sheet would show the last thing played here, paused, over music
	// that is coming out of the jack right now.
	if (btreceiver_is_active()) {
		return;
	}
	lv_anim_delete(player_screen, sheet_x_cb);
	sheet_side_active = false;
	sheet_x_cb(player_screen, 0);

	if (!sheet_open) {
		// Remember the opener: leaving the player must always land back here,
		// whatever the pages under the sheet did in the meantime.
		sheet_under = lv_screen_active();
	}
	sheet_open = true;
	smooth_timer_sync();
	back_btn_player_mode(true);
	sx_mini_player_shown(true);
	lyrics_screen_hold_update();
	if (smooth_timer && progress_running) {
		lv_timer_resume(smooth_timer);
	}
	// The poll keeps these only while the player is up (np_poll), so they are
	// brought up to date as it comes up.
	{
		device_state_t state;
		device_state_get(&state);
		np_chain_refresh(&state);
		upnext_update();
	}

	// Explicitly the bottom-most thing on the top layer: it covers the page it
	// slides over, while the status bar and the floating back button stay drawn
	// above it, exactly where the page underneath put them. Nothing shared is
	// moved or hidden, so nothing shared slides around during the drag.
	lv_obj_set_hidden(player_screen, false);
	lv_obj_move_background(player_screen);

	// Artwork runs to the top edge here, so the bar steps aside. It is hidden
	// rather than moved, which is why nothing slides during the drag. Unless
	// it is to stay up: then it goes over the artwork.
	if (bar_always) {
		bar_over_player_update();
	} else {
		topbar_set_hidden(true);
	}

	if (!screensaver_is_visible())
		sx_artview_overlay_will_show(player_screen);
	sheet_move_to(y, animate);
}

void player_sheet_open(bool animate) { player_sheet_open_at(0, animate); }

static void sheet_hidden_anim_ready_cb(lv_anim_t *a) {
	(void)a;
	if (!sheet_open) {
		lv_obj_set_hidden(player_screen, true);
		if (!screensaver_is_visible())
			sx_artview_opaque_overlay(NULL);
		sx_artview_refresh();
	}
	// Whatever page the close landed on, the chevron's visibility is settled
	// once more now that the last animation frame has run -- the fix for the
	// chevron flashing on the main menu for a frame on the way back to it.
	back_btn_sync_visibility();
}

void player_sheet_close(bool animate) {
	lv_anim_delete(player_screen, sheet_x_cb);
	sheet_side_active = sheet_pull_active = false;
	sheet_x_cb(player_screen, 0);
	bool was_open = sheet_open;
	sheet_open = false;
	if (!screensaver_is_visible()) {
		if (animate)
			sx_artview_overlay_will_show(player_screen);
		else
			sx_artview_opaque_overlay(NULL);
	}
	chain_set_open(false); // the block is the player's, and does not wait for it
	if (smooth_timer) {
		lv_timer_pause(smooth_timer); // nothing of it shows under the page
	}
	back_btn_player_mode(false);
	sx_mini_player_shown(false);
	lyrics_screen_hold_update();
	topbar_set_playback(false);
	topbar_set_over_artwork(false, false);
	topbar_set_hidden(false);
	// The tab does not travel with the page: coming back to a player with a
	// panel hanging open is not what leaving it looked like.

	// Back where the player was opened from, always: if anything switched the
	// page underneath while the sheet was up, put the opener back.
	if (was_open && sheet_under && lv_screen_active() != sheet_under) {
		switch_screen_no_history(sheet_under);
	}
	// The page underneath stays loaded while the sheet covers it, so nothing
	// tells it to look at its list again.
	medialist_refresh_visible();

	if (!animate) {
		lv_obj_set_y(player_screen, sheet_height);
		back_btn_translate_y(0);
		lv_obj_set_hidden(player_screen, true);
		sx_artview_refresh();
		back_btn_sync_visibility();
		return;
	}

	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, player_screen);
	lv_anim_set_exec_cb(&a, sheet_anim_y_cb);
	lv_anim_set_values(&a, lv_obj_get_y(player_screen), sheet_height);
	lv_anim_set_duration(&a, SHEET_ANIM_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_completed_cb(&a, sheet_hidden_anim_ready_cb);
	lv_anim_start(&a);
}

// Runs from lv_async_call, one tick after the gesture's own events (RELEASED,
// then CLICKED) have all been delivered: the suppression the flag exists for
// has happened by then, and nothing later may still be blocked by it.
bool player_sheet_pull_begin(int visible_height) {
	if (sheet_open || sx_sheet_is_open() || quickpanel_is_open() || coverflow_drag_active() || btreceiver_is_active())
		return false;
	sheet_pull_origin = sheet_height - visible_height;
	// Prepare at the actual pull position, never briefly make it opaque and
	// evict the underlying page's artwork only to recreate it immediately.
	player_sheet_open_at(sheet_pull_origin, false);
	sheet_pull_active = true;
	topbar_set_hidden(false);
	return true;
}

void player_sheet_pull_update(int up) {
	if (sheet_pull_active)
		sheet_move_to(sheet_pull_origin - up, false);
}

void player_sheet_pull_end(bool open) {
	if (!sheet_pull_active)
		return;
	sheet_pull_active = false;
	if (open)
		player_sheet_open(true);
	else
		player_sheet_close(true);
}

bool player_sheet_edge_back_allowed(void) { return sheet_open && !sheet_pull_active && !sheet_side_active && !drag_committed && !chain_is_open() && !sx_sheet_is_open() && !quickpanel_is_open(); }

void player_sheet_edge_back_begin(void) {
	sheet_side_active = true;
	topbar_set_hidden(false);
	if (!screensaver_is_visible())
		sx_artview_overlay_will_show(player_screen);
}

void player_sheet_edge_back_update(int dx) {
	if (sheet_side_active)
		sheet_x_cb(player_screen, LV_CLAMP(0, dx, (int)lv_obj_get_width(player_screen)));
}

void player_sheet_edge_back_end(bool close) {
	if (!sheet_side_active)
		return;
	sheet_side_close = close;
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, player_screen);
	lv_anim_set_exec_cb(&a, sheet_x_cb);
	lv_anim_set_values(&a, lv_obj_get_x(player_screen), close ? lv_obj_get_width(player_screen) : 0);
	lv_anim_set_duration(&a, SX_ANIM_NAV_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_completed_cb(&a, sheet_x_done);
	lv_anim_start(&a);
}

static void drag_committed_clear_cb(void *unused) {
	(void)unused;
	drag_committed = false;
}

// The pull down, on the open player. Only once the movement is clearly
// downwards (SX_DRAG_LOCK_PX, and more down than across) is it taken: a
// sideways drag on the cover belongs to the cover.
static void sheet_drag_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);

	lv_indev_t *indev = lv_indev_active();
	if (!indev) {
		return;
	}

	if (code == LV_EVENT_PRESSED) {
		if (!sheet_open) {
			return;
		}
		lv_indev_get_point(indev, &drag_start_point);
		dragging = true;
		drag_committed = false;
		return;
	}

	if (!dragging) {
		return;
	}

	lv_point_t point;
	lv_indev_get_point(indev, &point);
	int dx = point.x - drag_start_point.x;
	int dy = point.y - drag_start_point.y;

	if (code == LV_EVENT_PRESSING) {
		if (!drag_committed) {
			if (lv_indev_get_scroll_obj(indev) || coverflow_drag_active()) {
				dragging = false;
				return;
			}
			if (LV_ABS(dx) >= SX_DRAG_LOCK_PX && LV_ABS(dx) > dy) {
				dragging = false; // sideways: the cover's own swipe
				return;
			}
			if (dy < SX_DRAG_LOCK_PX || dy <= LV_ABS(dx)) {
				return;
			}
			drag_committed = true;
			// With the chain's block open, down puts the block away and the
			// player stays (principle 7: the top layer goes first).
			if (chain_is_open()) {
				return;
			}
			// The page the player goes back to shows from the top as it is
			// pulled down, status bar and all; let go short of the line, the
			// player hides the bar again as it springs back open.
			topbar_set_hidden(false);
		}

		if (!chain_is_open()) {
			sheet_move_to(dy, false);
		}
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		dragging = false;
		if (drag_committed && chain_is_open()) {
			chain_set_open(false);
		} else if (drag_committed) {
			if (dy > SX_NP_COLLAPSE_PX) {
				player_sheet_close(true);
			} else {
				player_sheet_open(true);
			}
		}

		// The flag must not outlive the gesture. Deferred rather than cleared
		// here, because the CLICKED event of this same gesture still has to
		// see it.
		lv_async_call(drag_committed_clear_cb, NULL);
	}
}

// The control centre, from the player too.
//
// Every other page pulls it down from the status bar, and this page hides the
// status bar, so there is nothing to pull. This strip stands in for it:
// the same gesture, in the same place, on a piece of the artwork that has
// nothing else to do. The bar comes back for as long as the sheet is up (see
// quickpanel.c), so what the finger lands on afterwards is what it expected.
//
// Vertical dominance is required before it engages: a sideways drag across the
// top of the artwork is somebody putting the player away, not somebody
// reaching for the brightness.
static void panel_edge_drag_cb(lv_event_t *e) {
	static lv_point_t start;
	static bool tracking;
	static bool engaged;

	lv_indev_t *indev = lv_indev_active();
	if (!indev) {
		return;
	}

	lv_event_code_t code = lv_event_get_code(e);
	if (code == LV_EVENT_PRESSED) {
		lv_indev_get_point(indev, &start);
		tracking = !quickpanel_is_open();
		engaged = false;
		return;
	}
	if (!tracking) {
		return;
	}

	lv_point_t p;
	lv_indev_get_point(indev, &p);
	int dy = p.y - start.y;
	int dx = p.x - start.x;

	if (code == LV_EVENT_PRESSING) {
		if (!engaged) {
			int adx = dx < 0 ? -dx : dx;
			if (dy < 8 || dy <= adx) {
				return; // still just a press, or a sideways one
			}
			engaged = true;
			quickpanel_drag_begin();
		}
		quickpanel_drag_update(dy);
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		tracking = false;
		if (engaged) {
			engaged = false;
			quickpanel_drag_end();
		}
	}
}

#define NP_HOME_EDGE_H 96
#define NP_HOME_SWIPE_PX 48

// Observe the existing bottom surfaces, including buttons, without covering
// their tap targets. Judge the final sample too: a quick flick need not produce
// an intermediate PRESSING event before RELEASED.
static void home_edge_drag_cb(lv_event_t *e) {
	static lv_point_t start;
	static bool tracking;
	if (lv_event_get_target(e) != lv_event_get_current_target(e))
		return;
	lv_event_code_t code = lv_event_get_code(e);
	if (code == LV_EVENT_PRESS_LOST) {
		tracking = false;
		return;
	}
	lv_indev_t *indev = lv_event_get_param(e);
	if (!indev)
		indev = lv_indev_active();
	if (!indev)
		return;
	if (code == LV_EVENT_PRESSED) {
		lv_indev_get_point(indev, &start);
		tracking = start.y >= sheet_height - NP_HOME_EDGE_H && lv_obj_get_y(player_screen) == 0 && sheet_open && !chain_is_open() && !quickpanel_is_open() && !sx_sheet_is_open();
		return;
	}
	if (!tracking)
		return;
	lv_point_t point;
	lv_indev_get_point(indev, &point);
	int dx = point.x - start.x, up = start.y - point.y;
	if (code == LV_EVENT_RELEASED) {
		tracking = false;
		bool home = up >= NP_HOME_SWIPE_PX && up > LV_ABS(dx) && sheet_open && !chain_is_open() && !quickpanel_is_open() && !sx_sheet_is_open();
		// A drag on a transport button must not also click it, including a
		// cancelled/sideways drag that falls short of Home. A tap stays native.
		if (home || LV_ABS(up) >= SX_DRAG_LOCK_PX || LV_ABS(dx) >= SX_DRAG_LOCK_PX) {
			lv_indev_reset(indev, NULL);
		}
		if (home) {
			sheet_under = main_menu_screen;
			switch_screen(main_menu_screen);
			player_sheet_close(true);
		}
	}
}

static void artist_label_clicked_cb(lv_event_t *e) {
	(void)e;
	if (drag_committed) {
		return; // the end of a swipe, not a tap
	}
	trackmenu_show_artist();
}

void player_sheet_attach_drag(lv_obj_t *obj, bool opening) {
	if (!obj) {
		return;
	}

	lv_obj_set_clickable(obj, true);
	// The leftward drag on a page that pulled the player in before 2.0 is the
	// mini player's job now (tap, or swipe up); a sideways drag on a list row
	// is the row's own (next in the queue / the end of it). The pages that
	// still ask for it get nothing but the clickable surface they had.
	if (opening) {
		return;
	}
	lv_obj_add_event_cb(obj, sheet_drag_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(obj, sheet_drag_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(obj, sheet_drag_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(obj, sheet_drag_cb, LV_EVENT_PRESS_LOST, NULL);
}

// ---------------------------------------------------------------------------
// Graphite playback: inset artwork, flat canvas, title and transport controls.
// Signal-chain details and artwork gestures retain their native behaviour.
// ---------------------------------------------------------------------------

#define NP_PANEL_PAD_H 16
#define NP_PANEL_PAD_TOP 16
#define NP_PANEL_PAD_BOTTOM 16 // above the screen's edge
#define NP_SEEK_BAND_H 72	   // the bar's 10 px track in the middle of it
#define NP_SEEK_MARGIN ((NP_SEEK_BAND_H - PROGRESS_TRACK_HEIGHT) / 2)
#define NP_TIMES_LIFT (-8)
#define NP_TRANSPORT_TOP 6
#define NP_TRANSPORT_GAP 12
#define NP_CHAIN_ROW_H 28
#define NP_CHAIN_DOT 10
#define NP_CHIP_MAX_W 344 // the handoff's 270 is for 19 px type; MiSans 20 bold is wider
#define NP_CHIP_PAD_H 16
#define NP_CHIP_OPA 107 // 42 %
#define NP_CHIP_RIGHT 16
#define NP_BADGE_GAP 10 // between the chip and the source marks under it
#define NP_CHAIN_BLOCK_GAP 12
#define NP_CHAIN_BLOCK_R 20
#define NP_CHAIN_BLOCK_BG 0x141416
#define NP_CHAIN_KEY_W 116 // the handoff's 96 is for a mono 15 px; "НАУШНИКИ" in MiSans 16 needs this
// A finger drawn across the artwork past this is a skip (the handoff's
// prototype: 60 px); short of it, and not taken as the pull down, it is
// nothing.
#define NP_COVER_SWIPE_PX 60

static void menu_style_for_layout(void) {
	if (!player_menu) {
		return;
	}
	lv_obj_set_style_pad_top(player_menu, NP_PANEL_PAD_TOP, 0);
	lv_obj_set_style_pad_bottom(player_menu, SX_NP_PANEL_R + NP_PANEL_PAD_BOTTOM, 0);
	lv_obj_set_style_pad_row(player_menu, 0, 0);
	lv_flex_align_t main = LV_FLEX_ALIGN_START;
	lv_obj_set_flex_align(player_menu, main, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	if (controls_row) {
		lv_obj_set_ignore_layout(controls_row, true);
		lv_obj_align(controls_row, LV_ALIGN_BOTTOM_MID, 0, 0);
	}
}

static lv_obj_t *upnext_lead; // "Up next"

static void np_paint_backgrounds(void) {
	if (!player_screen || !player_menu)
		return;
	const sx_palette_t *pal = sx_pal();
	sx_set_bg_color(player_screen, pal->np_bg);
	sx_set_bg_color(player_menu, pal->np_bg);
	if (cover_panel)
		sx_set_bg_color(cover_panel, pal->np_bg);
	if (upnext_lead)
		lv_obj_set_style_text_color(upnext_lead, pal->np_text2, 0);
	cover_art_layout();
}

// ---------------------------------------------------------------------------
// The thumbnails: 56 px for the mini player, 240 px over the artwork's place
// ---------------------------------------------------------------------------

// The playing track's album, as covertone knows it; 0 when it is not in the
// library (a stream, a station, a file from outside the music).
static int64_t np_album_id;
static uint32_t thumb240_ticket;
static lv_image_dsc_t thumb240_dsc;
static bool thumb240_held;
static int64_t thumb240_album;
static int64_t mini_large_attempted;
static void np_thumb240_request(void);

static uint8_t mini_thumb_buf[COVERTONE_THUMB_BYTES(COVERTONE_THUMB_SMALL)];
static lv_image_dsc_t mini_thumb_dsc;
static uint32_t mini_thumb_generation;
static int64_t mini_thumb_album; // what mini_thumb_buf holds, 0 nothing
// Outside the library: the decoded cover, scaled down, stands in.
static cover_image_t mini_cover_copy;

static bool np_mini_take_thumb240(const lv_image_dsc_t *dsc) {
	if (!dsc || !sx_mini_wants_cover())
		return false;
	if (mini_thumb_album == np_album_id && mini_cover_copy.pixels)
		return true;
	cover_image_t cached = {.dsc = *dsc, .pixels = (uint8_t *)dsc->data}, next = {0};
	if (!cover_scaled_copy(&cached, SX_MINI_COVER, &next))
		return false;
	sx_mini_set_cover(NULL, 0);
	sx_artview_release_cover(&mini_cover_copy);
	mini_cover_copy = next;
	mini_cover_copy.dsc.data = mini_cover_copy.pixels;
	mini_thumb_album = np_album_id;
	sx_mini_set_cover(&mini_cover_copy.dsc, mini_cover_copy.display_generation);
	return true;
}

static void np_mini_thumb_refresh(void) {
	if (!sx_mini_wants_cover()) {
		// Cover-derived colours still need the shared engine, not a hidden
		// mini-thumbnail read or a private scaled copy.
		float h, c, l;
		if (np_album_id && !covertone_for_album(np_album_id, &h, &c, &l))
			covertone_request(np_album_id);
		return;
	}
	if (np_album_id == 0) {
		if (mini_thumb_album) {
			sx_mini_set_cover(NULL, 0);
			mini_thumb_album = 0;
		}
		return;
	}
	if (mini_thumb_album == np_album_id && mini_cover_copy.pixels) {
		return;
	}
	if (mini_thumb_album != np_album_id) {
		// Detach before replacing an image still referenced by LVGL.
		sx_mini_set_cover(NULL, 0);
		sx_artview_release_cover(&mini_cover_copy);
		mini_thumb_album = 0;
	}
	// Reuse the full player's async request and ready pixels. Never read the
	// 115KB cache on the GUI thread or create a separate thumbnail worker.
	if (thumb240_held && thumb240_album == np_album_id && np_mini_take_thumb240(&thumb240_dsc))
		return;
	if (mini_large_attempted != np_album_id) {
		np_thumb240_request();
		if (thumb240_ticket || thumb240_held)
			mini_large_attempted = np_album_id;
	}
	if (mini_thumb_album == np_album_id)
		return;
	if (covertone_thumb(np_album_id, COVERTONE_THUMB_SMALL, &mini_thumb_dsc, mini_thumb_buf, sizeof(mini_thumb_buf))) {
		mini_thumb_album = np_album_id;
		mini_thumb_generation = cover_new_display_generation();
		sx_mini_set_cover(&mini_thumb_dsc, mini_thumb_generation);
	} else {
		covertone_request(np_album_id);
	}
}

// A track outside the library: the mini player gets the picture the player
// decoded, at its size. Once per picture; compact mode requests no private copy.
static void np_mini_from_cover(void) {
	if (!sx_mini_wants_cover() || (np_album_id && mini_thumb_album == np_album_id && mini_cover_copy.pixels)) {
		return;
	}
	if (np_album_id) {
		// A full cover can arrive before the shared thumbnail. Its identity must
		// already belong to this track; never reuse the previous track's pixels.
		device_state_t state;
		device_state_get(&state);
		if (cover_request_outstanding || cover_id_state != COVER_ID_IDLE || strcmp(cover_shown_path, state.current_file))
			return;
	}
	sx_mini_set_cover(NULL, 0);
	sx_artview_release_cover(&mini_cover_copy);
	if (current_cover.pixels && cover_scaled_copy(&current_cover, SX_MINI_COVER, &mini_cover_copy)) {
		mini_cover_copy.dsc.data = mini_cover_copy.pixels;
		mini_thumb_album = np_album_id;
		sx_mini_set_cover(&mini_cover_copy.dsc, mini_cover_copy.display_generation);
	}
}

// UI preference only: release the mini-specific image, preserving the shared
// cover/tone engine and full-player artwork/requests.
void player_mini_mode_changed(void) {
	sx_mini_set_cover(NULL, 0);
	mini_thumb_album = 0;
	mini_large_attempted = 0;
	sx_artview_release_cover(&mini_cover_copy);
	if (!sx_mini_wants_cover() && !cover_request_outstanding)
		np_thumb240_drop();
	if (sx_mini_wants_cover()) {
		np_mini_from_cover();
		np_mini_thumb_refresh();
	}
}

static void np_thumb240_drop(void) {
	if (thumb240_ticket) {
		covertone_thumb_cancel(thumb240_ticket);
		thumb240_ticket = 0;
	}
	if (cover_thumb_img) {
		lv_obj_set_hidden(cover_thumb_img, true);
		sx_artview_detach(cover_thumb_img);
		lv_image_set_src(cover_thumb_img, NULL);
	}
	if (thumb240_held) {
		covertone_thumb_free(&thumb240_dsc);
		thumb240_held = false;
	}
	thumb240_album = 0;
}

static void thumb240_arrived(int64_t album_id, int size, const lv_image_dsc_t *dsc, void *user) {
	(void)size;
	(void)user;
	thumb240_ticket = 0;
	if (!dsc) {
		return;
	}
	lv_image_dsc_t copy = *dsc;
	if (album_id == np_album_id)
		np_mini_take_thumb240(&copy);
	// Only while the picture it stands in for is still on its way, and for the
	// album that is playing.
	if (album_id != np_album_id || !cover_request_outstanding || !cover_thumb_img) {
		covertone_thumb_free(&copy);
		return;
	}
	if (thumb240_held) {
		sx_artview_detach(cover_thumb_img);
		lv_image_set_src(cover_thumb_img, NULL);
		covertone_thumb_free(&thumb240_dsc);
	}
	thumb240_dsc = copy;
	thumb240_held = true;
	thumb240_album = album_id;
	sx_artview_bind(cover_thumb_img, &thumb240_dsc, cover_new_display_generation());
	lv_obj_set_hidden(cover_thumb_img, false);
	sx_artview_refresh();
	if (cover_placeholder_icon) {
		lv_obj_set_hidden(cover_placeholder_icon, true);
	}
}

// Asked when the artwork is about to change (reload_cover).
static void np_thumb240_request(void) {
	if (np_album_id == 0 || (!sx_mini_wants_cover() && (!cover_thumb_img))) {
		np_thumb240_drop();
		return;
	}
	if (thumb240_album == np_album_id && (thumb240_held || thumb240_ticket)) {
		return;
	}
	np_thumb240_drop();
	thumb240_album = np_album_id;
	thumb240_ticket = covertone_thumb_async(np_album_id, COVERTONE_THUMB_LARGE, thumb240_arrived, NULL);
}

// Whether covertone knew the playing album's tone when it came up: until it
// does the palette runs on a hash of the name, and the listener repaints once
// it lands -- once, not at every one of its calls while a scan works through
// the card.
static bool np_album_tone_known;

static bool np_album_tone_now(void) {
	float h, c, l;
	return np_album_id != 0 && covertone_for_album(np_album_id, &h, &c, &l);
}

static void np_album_changed(const char *path, bool live) {
	int64_t id = 0;
	char album[512];
	if (!live && path && path[0] && library_track_album_value(path, album, sizeof(album)) && album[0]) {
		id = covertone_album_id(album);
	}
	if (id != np_album_id) {
		np_thumb240_drop();
		sx_mini_set_cover(NULL, 0);
		mini_thumb_album = 0;
		mini_large_attempted = 0;
		sx_artview_release_cover(&mini_cover_copy);
	}
	np_album_id = id;
	np_album_tone_known = np_album_tone_now();
	np_mini_thumb_refresh();
}

// covertone has new tones or thumbnails (covertonefan.h, at most ~4 a
// second): the playing album's may be among them.
static void np_covertone_changed(void) {
	if (np_album_id == 0) {
		return;
	}
	np_mini_thumb_refresh();
	if (cover_request_outstanding && !thumb240_held && !thumb240_ticket) {
		thumb240_album = 0;
		np_thumb240_request();
	}
	// The hue, when this album's tone has just been worked out: what was a hash
	// of the name until now becomes the sleeve's own colour.
	if (np_album_tone_known || !np_album_tone_now()) {
		return;
	}
	np_album_tone_known = true;
	device_state_t state;
	device_state_get(&state);
	if (!state.live && state.current_file[0]) {
		sx_palette_track_changed(state.current_file, state.metadata.album[0] ? state.metadata.album : NULL);
		np_paint_backgrounds();
	}
}

// ---------------------------------------------------------------------------
// Paused: the glyph over the artwork
// ---------------------------------------------------------------------------

// The tap on the artwork and the play button do the same thing.
static void np_toggle_play(void) {
	// A live stream has no position to come back to, so this is a stop, not a
	// pause: the connection goes, the station stays on screen, and pressing
	// play again reconnects to it. Not while it is still connecting.
	if (radio_is_active()) {
		if (radio_is_connecting()) {
			return;
		}
		if (radio_is_playing()) {
			radio_stop();
		} else {
			radio_resume();
		}
		player_refresh_now_playing();
		return;
	}
	audio_status_t new_status = device_state_toggle_play_pause();
	apply_playback_status(new_status); // optimistic; the next poll reconciles
}

// ---------------------------------------------------------------------------
// The artwork's gestures (the handoff's matrix: "Плеер (обложка)")
//
// A tap pauses or plays, a swipe across of NP_COVER_SWIPE_PX plays the next
// track (to the left) or the previous one (to the right). Books step chapters;
// their buttons still skip seconds, as do podcast swipes. The pull down is
// the sheet's own (sheet_drag_cb). Before 2.0 a leftward swipe
// here pulled the lyrics in: they are in the player's menu now ("Lyrics").
// ---------------------------------------------------------------------------

static bool cover_tap_contains(const lv_point_t *point) {
	lv_obj_t *art = cover_art;
	if (!art || lv_obj_is_hidden(art)) {
		return false;
	}
	lv_area_t bounds;
	lv_obj_get_coords(art, &bounds);
	return lv_area_is_point_on(&bounds, point, lv_obj_get_style_radius(art, LV_PART_MAIN));
}

static void cover_slide_x(void *obj, int32_t x) { lv_obj_set_style_translate_x(obj, x, 0); }

static void cover_slide_finish(int from) {
	if (!cover_art)
		return;
	lv_anim_delete(cover_art, cover_slide_x);
	cover_slide_x(cover_art, from);
	if (!from)
		return; // No movement: do not run a zero-distance settle animation.
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, cover_art);
	lv_anim_set_exec_cb(&a, cover_slide_x);
	lv_anim_set_values(&a, from, 0);
	lv_anim_set_duration(&a, SX_ANIM_NAV_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_start(&a);
}

static void cover_gesture_cb(lv_event_t *e) {
	static lv_point_t start;
	static bool tracking;
	static bool sideways;
	static char started_file[512];

	lv_indev_t *indev = lv_indev_active();
	if (!indev) {
		return;
	}
	lv_event_code_t code = lv_event_get_code(e);
	lv_point_t p;
	lv_indev_get_point(indev, &p);

	if (code == LV_EVENT_PRESSED) {
		device_state_t state;
		device_state_get(&state);
		snprintf(started_file, sizeof(started_file), "%s", state.current_file);
		lv_anim_delete(cover_art, cover_slide_x);
		cover_slide_x(cover_art, 0);
		start = p;
		tracking = true;
		sideways = false;
		return;
	}
	if (!tracking) {
		return;
	}
	int dx = p.x - start.x;
	int dy = p.y - start.y;

	if (code == LV_EVENT_PRESSING) {
		if (lv_indev_get_scroll_obj(indev)) {
			tracking = false;
			return;
		}
		if (!sideways && LV_ABS(dx) >= SX_SWIPE_LOCK_PX && LV_ABS(dx) > LV_ABS(dy)) {
			if (dx > 0 && start.x < SX_EDGE_BACK_W)
				return; // The same edge Back dismisses the player, never a track.
			sideways = true;
		}
		if (sideways && !player_sheet_drag_active())
			cover_slide_x(cover_art, LV_CLAMP(-cover_art_side(), dx, cover_art_side()));
		return;
	}
	if (code == LV_EVENT_PRESS_LOST) {
		tracking = false;
		cover_slide_finish(0);
		return;
	}
	// LV_EVENT_RELEASED
	tracking = false;
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		cover_slide_finish(0);
		return; // the pull down, or the swipe back
	}
	if (sideways) {
		device_state_t before, after;
		device_state_get(&before);
		if (strcmp(before.current_file, started_file)) {
			cover_slide_finish(0);
			return;
		}
		bool chapter_changed = false;
		if (audiobook_mode && LV_ABS(dx) >= NP_COVER_SWIPE_PX) {
			chapter_changed = chapters_step(dx < 0 ? 1 : -1);
		} else if (dx <= -NP_COVER_SWIPE_PX) {
			player_screen_next();
		} else if (dx >= NP_COVER_SWIPE_PX) {
			player_screen_prev();
		}
		device_state_get(&after);
		bool changed = chapter_changed || strcmp(before.current_file, after.current_file);
		cover_slide_finish(changed ? (dx < 0 ? cover_art_side() : -cover_art_side()) : LV_CLAMP(-cover_art_side(), dx, cover_art_side()));
		return;
	}
	if (LV_ABS(dx) < SX_SWIPE_LOCK_PX && LV_ABS(dy) < SX_SWIPE_LOCK_PX) {
		if (chain_is_open()) {
			chain_set_open(false); // the first tap puts the open chain away
			return;
		}
		// The viewport also receives gestures beside the inset sleeve. A miss
		// near the seek bar must not become a pause; both ends stay on artwork.
		if (cover_tap_contains(&start) && cover_tap_contains(&p)) {
			np_toggle_play();
		}
	}
}

// ---------------------------------------------------------------------------
// "Up next · X ›"
// ---------------------------------------------------------------------------

static lv_obj_t *upnext_chip;
static lv_obj_t *upnext_words;
static lv_obj_t *upnext_name; // the track, or "Queue"
static lv_obj_t *upnext_arrow;
static unsigned upnext_revision;
static unsigned upnext_metadata_revision;
static playback_mode_t upnext_mode;
static char upnext_text[256];

// Where the chip, and the source marks under it, sit: on the chevron's line.
static int upnext_y(void) { return back_btn_centre_y() - SX_NP_UPNEXT_H / 2; }

static int badge_y(void) { return upnext_y() + SX_NP_UPNEXT_H + NP_BADGE_GAP; }

static void upnext_clicked_cb(lv_event_t *e) {
	(void)e;
	if (player_sheet_drag_active()) {
		return;
	}
	trackmenu_open_queue();
}

static void np_upnext_build(void) {
	upnext_chip = lv_obj_create(player_screen);
	lv_obj_remove_style_all(upnext_chip);
	lv_obj_set_size(upnext_chip, LV_SIZE_CONTENT, SX_NP_UPNEXT_H);
	lv_obj_set_style_max_width(upnext_chip, NP_CHIP_MAX_W, 0);
	lv_obj_set_style_radius(upnext_chip, SX_CARD_R, 0);
	lv_obj_add_style(upnext_chip, &sx_style_press, LV_STATE_PRESSED);
	lv_obj_set_style_pad_hor(upnext_chip, NP_CHIP_PAD_H, 0);
	lv_obj_set_style_pad_column(upnext_chip, 12, 0);
	lv_obj_set_flex_flow(upnext_chip, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(upnext_chip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(upnext_chip, false);
	lv_obj_set_clickable(upnext_chip, true);
	lv_obj_set_ext_click_area(upnext_chip, 6);
	lv_obj_add_event_cb(upnext_chip, upnext_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_align(upnext_chip, LV_ALIGN_TOP_RIGHT, -NP_CHIP_RIGHT, upnext_y());

	upnext_words = sx_box(upnext_chip);
	lv_obj_set_size(upnext_words, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(upnext_words, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(upnext_words, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
	upnext_lead = lv_label_create(upnext_words);
	lv_label_set_text(upnext_lead, tr("np_up_next"));
	lv_obj_set_style_text_font(upnext_lead, &font_ui_20, 0);
	lv_obj_add_style(upnext_lead, &sx_style_text2, 0);
	lv_obj_set_style_text_opa(upnext_lead, LV_OPA_COVER, 0);

	upnext_name = lv_label_create(upnext_words);
	lv_label_set_long_mode(upnext_name, LV_LABEL_LONG_DOT);
	lv_obj_set_style_text_font(upnext_name, &font_ui_22_bold, 0);
	lv_obj_add_style(upnext_name, &sx_style_text, 0);
	lv_obj_set_style_text_align(upnext_name, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_set_height(upnext_name, lv_font_get_line_height(&font_ui_22_bold));

	upnext_arrow = lv_image_create(upnext_chip);
	lv_image_set_src(upnext_arrow, &npicon_chevron_right_sm);
	lv_obj_add_style(upnext_arrow, &sx_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(upnext_arrow, LV_OPA_COVER, 0);
	lv_obj_set_style_image_opa(upnext_arrow, LV_OPA_COVER, 0);

	lv_label_set_text(upnext_name, tr("trackmenu_queue"));
}

// The name a queue row or the chip gives a track: the library's title, the
// tags for a file the library does not have (a streamed track's sidecar),
// else the file's name without its extension.
static void np_title_for_path(const char *path, char *out, size_t size) {
	out[0] = '\0';
	if (library_track_title(path, out, size) && out[0]) {
		return;
	}
	song_metadata_t meta = {0};
	// Lazy entries can already have service details before any audio exists.
	stream_provider_fill_metadata(path, &meta);
	if (!meta.title[0]) {
		metadata_read(path, &meta);
	}
	if (meta.title[0]) {
		snprintf(out, size, "%s", meta.title);
		return;
	}
	const char *slash = strrchr(path, '/');
	snprintf(out, size, "%.255s", slash ? slash + 1 : path);
	char *dot = strrchr(out, '.');
	if (dot && dot != out) {
		*dot = '\0';
	}
}

// What plays now that is not the queue's: a station, a book, a phone (AirPlay,
// DLNA, the Bluetooth receiver). The handoff's `ext`.
static bool np_outside(void) {
	if (live_mode || audiobook_mode || dlna_owns_playback() || btreceiver_is_active()) {
		return true;
	}
	airplay_state_t air;
	airplay_get_state(&air);
	return air.playing;
}

static void upnext_update(void) {
	upnext_revision = playlist_revision();
	upnext_metadata_revision = device_state_metadata_revision();
	upnext_mode = playlist_get_mode();
	if (!upnext_chip) {
		return;
	}
	char title[256] = "";
	if (!np_outside()) {
		int count = playlist_count();
		int current = playlist_current_index();
		int next = -1;
		if (current >= 0 && current + 1 < count) {
			next = current + 1;
		} else if (current >= 0 && count > 1 && upnext_mode == PLAYBACK_MODE_REPEAT_ALL) {
			next = 0;
		}
		char path[512];
		if (next >= 0 && playlist_path_at(next, path, sizeof(path))) {
			np_title_for_path(path, title, sizeof(title));
		}
	}
	const char *shown = title[0] ? title : tr(audiobook_mode ? "chapters" : "trackmenu_queue");
	if (strcmp(shown, upnext_text) == 0 && lv_obj_is_hidden(upnext_lead) == !title[0]) {
		return;
	}
	snprintf(upnext_text, sizeof(upnext_text), "%s", shown);
	lv_obj_set_hidden(upnext_lead, !title[0]);
	lv_label_set_text(upnext_name, shown);
	// The name takes what the chip's NP_CHIP_MAX_W leaves it.
	lv_obj_update_layout(upnext_lead);
	int32_t room = NP_CHIP_MAX_W - 2 * NP_CHIP_PAD_H - 12 - (int32_t)npicon_chevron_right_sm.header.w;
	lv_obj_set_style_max_width(upnext_lead, room, 0);
	lv_obj_set_width(upnext_name, LV_SIZE_CONTENT);
	lv_obj_set_style_max_width(upnext_name, room > 40 ? room : 40, 0);
}

static void np_layout_changed(void) {
	if (upnext_chip) {
		lv_obj_set_hidden(upnext_chip, false);
		lv_obj_remove_style(upnext_chip, &sx_style_surface, 0);
		lv_obj_align(upnext_chip, LV_ALIGN_TOP_RIGHT, -NP_CHIP_RIGHT, upnext_y());
	}
}

void player_queue_changed(void) {
	upnext_update();
	update_queue_position();
}

// ---------------------------------------------------------------------------
// The signal chain (principle 8): one line on the panel, a block when tapped
// ---------------------------------------------------------------------------

static lv_obj_t *chain_row;
static lv_obj_t *chain_dot;
static lv_obj_t *chain_text;
static lv_obj_t *chain_mark;
static char chain_shown[192];
static int chain_warn_shown = -1;

static lv_obj_t *chain_block;
static lv_obj_t *chain_vals[4];
static bool chain_opened;

static bool chain_is_open(void) { return chain_opened; }

// "44.1" or "96", the rate in kHz as the chain writes it.
static void np_khz(char *out, size_t size, int hz) {
	if (hz % 1000 == 0) {
		snprintf(out, size, "%d", hz / 1000);
	} else {
		snprintf(out, size, "%.1f", hz / 1000.0);
	}
}

// The file, in the chain's words: "FLAC 16/44.1", "MP3 320", "DSD64", "AAC
// 128" for a station.
static void np_file_short(const device_state_t *state, char *out, size_t size, bool compact) {
	out[0] = '\0';
	if (state->live) {
		radio_now_t now;
		radio_get_now(&now);
		const char *codec = now.codec[0] ? now.codec : "";
		if (now.bitrate > 0) {
			snprintf(out, size, "%s%s%d", codec, codec[0] ? " " : "", now.bitrate);
		} else {
			snprintf(out, size, "%s", codec);
		}
		return;
	}
	if (!state->current_file[0] || state->stream_sample_rate <= 0) {
		return;
	}
	int dsd = audio_get_dsd_multiple();
	if (dsd > 0) {
		snprintf(out, size, "DSD%d", dsd);
		return;
	}
	char codec[16] = "";
	audio_get_stream_codec(codec, sizeof(codec));
	if (!codec[0]) {
		const char *dot = strrchr(state->current_file, '.');
		if (dot && !strchr(dot, '/')) {
			size_t k = 0;
			for (const char *c = dot + 1; *c && k < sizeof(codec) - 1; c++) {
				codec[k++] = (char)((*c >= 'a' && *c <= 'z') ? *c - 32 : *c);
			}
			codec[k] = '\0';
		}
	}
	int kbps = audio_get_stream_bitrate_kbps();
	if (audio_stream_is_lossy() || (compact && kbps > 0)) {
		if (kbps > 0) {
			snprintf(out, size, "%s %d", codec, kbps);
		} else {
			snprintf(out, size, "%s", codec);
		}
		return;
	}
	int bits = audio_get_stream_bits();
	char khz[12];
	np_khz(khz, sizeof(khz), state->stream_sample_rate);
	char format[48];
	snprintf(format, sizeof(format), "%s %d/%s", codec, bits > 0 ? bits : 16, khz);
	if (kbps > 0)
		snprintf(out, size, tr("np_chain_kbps"), format, kbps);
	else
		snprintf(out, size, "%s", format);
}

static bool np_output_differs(const device_state_t *state, const audio_output_format_t *output) { return state->stream_sample_rate > 0 && (output->dop || output->rate != (unsigned)state->stream_sample_rate || (!audio_stream_is_lossy() && audio_get_stream_bits() > 0 && output->bits != (unsigned)audio_get_stream_bits())); }

static void np_output_short(const audio_output_format_t *output, bool usb, char *out, size_t size) {
	char khz[12];
	np_khz(khz, sizeof(khz), (int)output->rate);
	snprintf(out, size, "%s %u/%s", output->dop ? "DoP" : usb ? "USB" : "PCM", output->bits, khz);
}

// The line: the file, and where it goes when that is somewhere worth naming --
// the Bluetooth link ("FLAC 16/44.1 → LDAC 990 · 44.1 kHz"), a USB DAC, or a
// rate the output was opened at that is not the file's.
static void np_chain_compose(const device_state_t *state, char *out, size_t size, bool *warn) {
	*warn = false;
	char file[96];
	char link[96] = "";
	bool bt = btline_compose(link, sizeof(link));
	np_file_short(state, file, sizeof(file), bt);
	if (bt) {
		char tmp[96];
		sx_chain_short(tmp, sizeof(tmp), warn); // the warn: a codec other than the one chosen
	} else {
		bool usb = usbaudio_active();
		audio_output_format_t output;
		if (audio_get_output_format(&output) && np_output_differs(state, &output))
			np_output_short(&output, usb, link, sizeof(link));
		else if (usb)
			snprintf(link, sizeof(link), "USB");
	}
	if (file[0] && link[0]) {
		snprintf(out, size, "%s \xE2\x86\x92 %s", file, link);
	} else {
		snprintf(out, size, "%s", file[0] ? file : link);
	}
}

static void chain_block_fill(void);

static void chain_paint_mark(void) {
	if (chain_mark) {
		lv_obj_set_hidden(chain_mark, true);
	}
}

static void np_chain_refresh(const device_state_t *state) {
	if (!chain_row) {
		return;
	}
	char text[sizeof(chain_shown)];
	bool warn = false;
	np_chain_compose(state, text, sizeof(text), &warn);
	// Source/output details arrive after the track selection. Keep their band
	// reserved while enabled so the seek target never moves as the text settles.
	bool show = btline_enabled();
	if (lv_obj_is_hidden(chain_row) == show) {
		lv_obj_set_hidden(chain_row, !show);
	}
	if (strcmp(text, chain_shown) != 0) {
		snprintf(chain_shown, sizeof(chain_shown), "%s", text);
		lv_label_set_text(chain_text, text);
	}
	lv_color_t c = warn ? sx_hex(sx_pal()->dark ? SX_D_WARNING_TEXT : SX_L_WARNING_TEXT) : sx_pal()->np_text2;
	if ((int)warn != chain_warn_shown || !lv_color_eq(c, lv_obj_get_style_bg_color(chain_dot, 0))) {
		chain_warn_shown = warn;
		lv_obj_set_style_bg_color(chain_dot, c, 0);
		lv_obj_set_style_text_color(chain_text, c, 0);
	}
	if (chain_opened) {
		chain_block_fill();
	}
}

void player_chain_open(void) { chain_set_open(true); }

static void np_chain_build(lv_obj_t *parent) {
	chain_row = lv_obj_create(parent);
	lv_obj_remove_style_all(chain_row);
	lv_obj_set_size(chain_row, lv_pct(100), NP_CHAIN_ROW_H);
	lv_obj_set_flex_flow(chain_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(chain_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(chain_row, 8, 0);
	lv_obj_set_scrollable(chain_row, false);
	lv_obj_set_clickable(chain_row, false);

	chain_dot = lv_obj_create(chain_row);
	lv_obj_remove_style_all(chain_dot);
	lv_obj_set_size(chain_dot, NP_CHAIN_DOT, NP_CHAIN_DOT);
	lv_obj_set_style_radius(chain_dot, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(chain_dot, LV_OPA_COVER, 0);
	lv_obj_set_clickable(chain_dot, false);
	lv_obj_set_hidden(chain_dot, true);

	chain_text = lv_label_create(chain_row);
	lv_label_set_text(chain_text, "");
	lv_label_set_long_mode(chain_text, LV_LABEL_LONG_DOT);
	lv_obj_set_style_text_font(chain_text, &font_ui_20, 0);
	lv_obj_set_height(chain_text, lv_font_get_line_height(&font_ui_20));
	lv_obj_set_style_max_width(chain_text, (int32_t)lv_obj_get_style_width(player_screen, 0) - 2 * NP_PANEL_PAD_H - NP_CHAIN_DOT - 16 - (int32_t)npicon_chevron_up_sm.header.w, 0);
	lv_obj_set_clickable(chain_text, false);

	chain_mark = lv_image_create(chain_row);
	lv_obj_set_style_image_recolor(chain_mark, lv_color_white(), 0);
	lv_obj_set_style_image_recolor_opa(chain_mark, LV_OPA_COVER, 0);
	lv_obj_set_style_image_opa(chain_mark, LV_OPA_70, 0);
	lv_obj_set_clickable(chain_mark, false);
	chain_paint_mark();
	lv_obj_set_hidden(chain_row, !btline_enabled());
}

// The block: ФАЙЛ / ПЛЕЕР / ЛИНК / НАУШНИКИ, each a line of what is known.

static void chain_block_clicked_cb(lv_event_t *e) {
	(void)e;
	chain_set_open(false);
}

static void chain_block_build(void) {
	if (chain_block) {
		return;
	}
	int w = sheet_height > 0 ? (int)lv_obj_get_width(player_screen) : 480;
	chain_block = lv_obj_create(player_screen);
	lv_obj_remove_style_all(chain_block);
	lv_obj_set_size(chain_block, w - 2 * NP_CHAIN_BLOCK_GAP, LV_SIZE_CONTENT);
	lv_obj_set_style_radius(chain_block, NP_CHAIN_BLOCK_R, 0);
	lv_obj_add_style(chain_block, &sx_style_surface, 0);
	lv_obj_set_style_bg_opa(chain_block, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_hor(chain_block, 18, 0);
	lv_obj_set_style_pad_ver(chain_block, 8, 0);
	lv_obj_set_flex_flow(chain_block, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scrollable(chain_block, false);
	lv_obj_set_clickable(chain_block, true);
	lv_obj_add_event_cb(chain_block, chain_block_clicked_cb, LV_EVENT_CLICKED, NULL);
	// Pulled down, it goes (sheet_drag_cb puts the block away first).
	player_sheet_attach_drag(chain_block, false);
	lv_obj_align(chain_block, LV_ALIGN_BOTTOM_LEFT, NP_CHAIN_BLOCK_GAP, -(np_panel_h + NP_CHAIN_BLOCK_GAP));

	static const char *const KEYS[4] = {"np_chain_file", "np_chain_player", "np_chain_link", "np_chain_phones"};
	for (int i = 0; i < 4; i++) {
		lv_obj_t *row = lv_obj_create(chain_block);
		lv_obj_remove_style_all(row);
		lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
		lv_obj_set_style_pad_ver(row, 8, 0);
		lv_obj_set_style_pad_column(row, 12, 0);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
		lv_obj_set_scrollable(row, false);
		lv_obj_set_event_bubble(row, true);

		lv_obj_t *key = lv_label_create(row);
		lv_label_set_text(key, tr(KEYS[i]));
		lv_obj_set_width(key, NP_CHAIN_KEY_W);
		lv_obj_set_style_text_font(key, &font_ui_16, 0);
		lv_obj_add_style(key, &sx_style_text2, 0);
		lv_obj_set_style_pad_top(key, 4, 0);
		lv_label_set_long_mode(key, LV_LABEL_LONG_DOT);

		chain_vals[i] = lv_label_create(row);
		lv_label_set_text(chain_vals[i], "");
		lv_label_set_long_mode(chain_vals[i], LV_LABEL_LONG_WRAP);
		lv_obj_set_flex_grow(chain_vals[i], 1);
		lv_obj_set_style_text_font(chain_vals[i], &font_ui_21, 0);
		lv_obj_add_style(chain_vals[i], &sx_style_text, 0);
	}
	lv_obj_set_hidden(chain_block, true);
}

// Joins `part` to `out` with " · ".
static void chain_join(char *out, size_t size, const char *part) {
	if (!part || !part[0]) {
		return;
	}
	size_t used = strlen(out);
	snprintf(out + used, size - used, "%s%s", used ? " \xC2\xB7 " : "", part);
}

static void chain_set_val(int i, const char *text) {
	const char *now = lv_label_get_text(chain_vals[i]);
	if (!now || strcmp(now, text) != 0) {
		lv_label_set_text(chain_vals[i], text);
	}
}

static void chain_block_fill(void) {
	if (!chain_block) {
		return;
	}
	device_state_t state;
	device_state_get(&state);
	char line[192];
	char part[96];

	// ФАЙЛ: what the file is.
	line[0] = '\0';
	if (state.live) {
		radio_now_t now;
		radio_get_now(&now);
		if (now.bitrate > 0) {
			snprintf(part, sizeof(part), tr("np_chain_kbps"), now.codec[0] ? now.codec : "", now.bitrate);
		} else {
			snprintf(part, sizeof(part), "%s", now.codec);
		}
		snprintf(line, sizeof(line), "%s \xC2\xB7 %s", tr("np_chain_stream"), part);
	} else if (state.current_file[0] && state.stream_sample_rate > 0) {
		char codec[16] = "";
		audio_get_stream_codec(codec, sizeof(codec));
		int dsd = audio_get_dsd_multiple();
		char khz[12];
		np_khz(khz, sizeof(khz), state.stream_sample_rate);
		if (dsd > 0) {
			snprintf(line, sizeof(line), "DSD%d", dsd);
		} else if (audio_stream_is_lossy()) {
			int kbps = audio_get_stream_bitrate_kbps();
			if (kbps > 0) {
				snprintf(line, sizeof(line), tr("np_chain_kbps"), codec, kbps);
			} else {
				snprintf(line, sizeof(line), "%s", codec);
			}
		} else {
			int bits = audio_get_stream_bits();
			snprintf(part, sizeof(part), tr("np_chain_bits"), codec, bits > 0 ? bits : 16, khz);
			int kbps = audio_get_stream_bitrate_kbps();
			if (kbps > 0)
				snprintf(line, sizeof(line), tr("np_chain_kbps"), part, kbps);
			else
				snprintf(line, sizeof(line), "%s", part);
		}
	}
	chain_set_val(0, line[0] ? line : "\xE2\x80\x94");

	// ПЛЕЕР: what the player does to it on the way.
	line[0] = '\0';
	int device = audio_get_device_rate();
	if (!state.live && state.stream_sample_rate > 0 && device > 0 && device != state.stream_sample_rate && audio_get_dsd_multiple() <= 0) {
		char from[12], to[12];
		np_khz(from, sizeof(from), state.stream_sample_rate);
		np_khz(to, sizeof(to), device);
		snprintf(part, sizeof(part), tr("np_chain_resample"), from, to);
		chain_join(line, sizeof(line), part);
	} else if (!state.live && state.stream_sample_rate > 0) {
		chain_join(line, sizeof(line), tr("np_chain_bitperfect"));
	}
	const song_metadata_t *meta = device_state_loaded_metadata(NULL);
	if (!state.live && meta && (meta->has_track_gain || meta->has_album_gain) && replaygain_mode() != REPLAYGAIN_OFF) {
		snprintf(part, sizeof(part), "ReplayGain %+.1f dB", replaygain_current_db());
		chain_join(line, sizeof(line), part);
	}
	if (eq_get_enabled()) {
		chain_join(line, sizeof(line), "EQ");
	}
	if (peq_get_enabled()) {
		chain_join(line, sizeof(line), "PEQ");
	}
	if (mseb_get_enabled()) {
		chain_join(line, sizeof(line), "MSEB");
	}
	if ((audiobook_mode || podcast_mode) && speed_current_permille() != AUDIOBOOK_SPEED_NORMAL) {
		snprintf(part, sizeof(part), "%.2g\xC3\x97", speed_current_permille() / 1000.0);
		chain_join(line, sizeof(line), part);
	}
	chain_set_val(1, line[0] ? line : "\xE2\x80\x94");

	// ЛИНК: what carries it out.
	char link[96] = "";
	bool bt = btline_compose(link, sizeof(link));
	if (bt) {
		bool warn = false;
		char tmp[96];
		sx_chain_short(tmp, sizeof(tmp), &warn);
		if (warn) {
			chain_join(link, sizeof(link), tr("np_chain_codec_other"));
		}
		chain_set_val(2, link);
	} else if (usbaudio_active()) {
		char name[64] = "";
		usbaudio_card_name(name, sizeof(name));
		audio_output_format_t output;
		if (audio_get_output_format(&output))
			np_output_short(&output, true, part, sizeof(part));
		else
			snprintf(part, sizeof(part), "USB");
		snprintf(line, sizeof(line), "%s%s%s", part, name[0] ? " \xC2\xB7 " : "", name);
		chain_set_val(2, line);
	} else {
		snprintf(line, sizeof(line), "%s", tr("np_chain_jack"));
		audio_output_format_t output;
		if (audio_get_output_format(&output)) {
			np_output_short(&output, false, part, sizeof(part));
			chain_join(line, sizeof(line), part);
		}
		chain_set_val(2, line);
	}

	// НАУШНИКИ: who listens.
	bt_device_t dev;
	if (bt && bluetooth_connected_device(&dev) && dev.name[0]) {
		int battery = bluetooth_headset_battery(NULL);
		if (battery >= 0) {
			snprintf(line, sizeof(line), "%s \xC2\xB7 %d%%", dev.name, battery);
		} else {
			snprintf(line, sizeof(line), "%s", dev.name);
		}
		chain_set_val(3, line);
	} else if (bt) {
		chain_set_val(3, tr("np_chain_bt_phones"));
	} else {
		chain_set_val(3, "\xE2\x80\x94");
	}
}

static void chain_set_open(bool open) {
	if (open == chain_opened) {
		return;
	}
	if (open) {
		chain_block_build();
	}
	chain_opened = open;
	if (chain_block) {
		if (open) {
			chain_block_fill();
			lv_obj_set_hidden(chain_block, false);
			lv_obj_move_foreground(chain_block);
		} else {
			lv_obj_set_hidden(chain_block, true);
		}
	}
	chain_paint_mark();
}

// ---------------------------------------------------------------------------
// Sources outside the queue: what the mini player says while a phone plays
// ---------------------------------------------------------------------------

static void np_outside_watch(const device_state_t *state) {
	static bool shown;
	static unsigned shown_serial;
	airplay_state_t air;
	airplay_get_state(&air);
	unsigned serial = airplay_serial();
	char title[AIRPLAY_TEXT_MAX + 16] = "";
	if (air.playing) {
		snprintf(title, sizeof(title), "%s", air.title[0] ? air.title : "AirPlay");
	} else if (btreceiver_is_active()) {
		btreceiver_state_t bt;
		btreceiver_get_state(&bt);
		snprintf(title, sizeof(title), "%s", bt.device[0] ? bt.device : "Bluetooth");
		serial = (unsigned)bt.sample_rate ^ (unsigned)strlen(bt.device);
	}
	if (title[0]) {
		if (!shown || serial != shown_serial) {
			shown = true;
			shown_serial = serial;
			sx_mini_set_track(title, NULL, air.playing ? air.artist : NULL, true);
			sx_mini_set_cover(NULL, 0);
			mini_thumb_album = 0;
			mini_large_attempted = 0;
			sx_artview_release_cover(&mini_cover_copy);
			upnext_update();
		}
		return;
	}
	if (shown) {
		// The phone let go: the mini player says what the player has loaded.
		shown = false;
		const char *base = strrchr(state->current_file, '/');
		const char *name = state->metadata.title[0] ? state->metadata.title : (base ? base + 1 : state->current_file);
		sx_mini_set_track(state->current_file[0] ? name : NULL, state->current_file[0] ? state->current_file : NULL, state->metadata.artist, state->live);
		np_mini_thumb_refresh();
		upnext_update();
	}
}

// The poll's share (update_progress(), twice a second playing, every two
// seconds paused): each of these compares before it draws, so a poll where
// nothing moved draws nothing.
static void np_poll(const device_state_t *state) {
	bookresume_note(state);
	static unsigned shown_metadata_revision;
	unsigned metadata_revision = device_state_metadata_revision();
	if (metadata_revision != shown_metadata_revision) {
		shown_metadata_revision = metadata_revision;
		// Details arriving for the same file change text, not transport state.
		// A full track refresh would reset progress and request the cover again.
		if (!state->live && !np_outside() && stream_provider_owns(state->current_file)) {
			const char *base = strrchr(state->current_file, '/');
			const char *title = state->metadata.title[0] ? state->metadata.title : (base ? base + 1 : state->current_file);
			if (song_title_label) {
				scrolltext_set(song_title_label, title);
				scrolltext_set(song_artist_label, metadata_shown_artist(&state->metadata));
			}
			sx_mini_set_text(title, state->metadata.artist);
		}
	}
	if (playlist_revision() != upnext_revision || playlist_get_mode() != upnext_mode || metadata_revision != upnext_metadata_revision) {
		upnext_update();
		update_queue_position();
	}
	np_outside_watch(state);
	trackmenu_queue_poll(); // the queue page, when it is up and its queue moved
	historypage_poll();
	searchall_poll();
	// The chain can change under a track -- a codec renegotiated, headphones
	// switched, the output reopened at another rate -- so it is read again,
	// but only for a line somebody can see.
	if (power_screen_is_on()) {
		if (sheet_open) {
			np_chain_refresh(state);
		}
	}
	queue_undo_tick();
}

bool player_outside_title(char *out, size_t size) {
	if (!out || size == 0) {
		return false;
	}
	out[0] = '\0';
	airplay_state_t air;
	airplay_get_state(&air);
	if (air.playing) {
		snprintf(out, size, "%s", air.title[0] ? air.title : "AirPlay");
		return true;
	}
	if (btreceiver_is_active()) {
		btreceiver_state_t bt;
		btreceiver_get_state(&bt);
		snprintf(out, size, "%s", bt.device[0] ? bt.device : "Bluetooth");
		return true;
	}
	device_state_t state;
	device_state_get(&state);
	bool book = !state.live && audiobook_is_playing();
	if (state.live || book || dlna_owns_playback()) {
		const char *base = strrchr(state.current_file, '/');
		// A book by its name, not by the part or chapter playing.
		const char *name = book && state.metadata.album[0] ? state.metadata.album : state.metadata.title[0] ? state.metadata.title : (base ? base + 1 : state.current_file);
		snprintf(out, size, "%s", name);
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// The player's own choices, for its menu (trackmenu.c)
// ---------------------------------------------------------------------------

static void mode_toast(playback_mode_t mode) {
	static const char *const NAMES[] = {
		[PLAYBACK_MODE_NORMAL] = "player_mode_normal", [PLAYBACK_MODE_REPEAT_ALL] = "player_mode_repeat_all", [PLAYBACK_MODE_REPEAT_ONE] = "player_mode_repeat_one", [PLAYBACK_MODE_SHUFFLE] = "player_mode_shuffle", [PLAYBACK_MODE_SHUFFLE_REPEAT] = "player_mode_shuffle_repeat",
	};
	static const lv_image_dsc_t *const GLYPHS[] = {
		[PLAYBACK_MODE_NORMAL] = &icon_repeat_off, [PLAYBACK_MODE_REPEAT_ALL] = &icon_repeat_all, [PLAYBACK_MODE_REPEAT_ONE] = &icon_repeat_one, [PLAYBACK_MODE_SHUFFLE] = &icon_shuffle, [PLAYBACK_MODE_SHUFFLE_REPEAT] = &icon_shuffle_repeat,
	};
	if ((size_t)mode < sizeof(NAMES) / sizeof(NAMES[0])) {
		toast_glyph(GLYPHS[mode], tr(NAMES[mode]));
	}
}

bool player_shuffle_on(void) {
	playback_mode_t m = playlist_get_mode();
	return m == PLAYBACK_MODE_SHUFFLE || m == PLAYBACK_MODE_SHUFFLE_REPEAT;
}

void player_shuffle_toggle(void) {
	playback_mode_t m = playlist_get_mode();
	playback_mode_t next;
	if (player_shuffle_on()) {
		next = m == PLAYBACK_MODE_SHUFFLE_REPEAT ? PLAYBACK_MODE_REPEAT_ALL : PLAYBACK_MODE_NORMAL;
	} else {
		next = m == PLAYBACK_MODE_REPEAT_ALL ? PLAYBACK_MODE_SHUFFLE_REPEAT : PLAYBACK_MODE_SHUFFLE;
	}
	playlist_set_mode(next);
	update_repeat_button();
	player_queue_changed();
	mode_toast(next);
}

static void playback_mode_pick(void *user) {
	playback_mode_t mode = (playback_mode_t)(intptr_t)user;
	if (podcast_mode || audiobook_mode || live_mode || dlna_owns_playback() || mode == playlist_get_mode())
		return;
	playlist_set_mode(mode);
	update_repeat_button();
	player_queue_changed();
	mode_toast(mode);
}

void player_playback_mode_menu(void) {
	if (podcast_mode || audiobook_mode || live_mode || dlna_owns_playback())
		return;
	static const char *const names[] = {"player_mode_normal", "player_mode_repeat_all", "player_mode_repeat_one", "player_mode_shuffle", "player_mode_shuffle_repeat"};
	static const lv_image_dsc_t *const icons[] = {&npicon_queue, &icon_repeat_all, &icon_repeat_one, &icon_shuffle, &icon_shuffle_repeat};
	sx_sheet_item_t items[5];
	for (int i = 0; i < 5; i++)
		items[i] = (sx_sheet_item_t){tr(names[i]), icons[i], playlist_get_mode() == (playback_mode_t)i, false, playback_mode_pick, (void *)(intptr_t)i};
	library_index_spec_t spec;
	const char *scope = playlist_library_spec(&spec) && spec.kind == LIBRARY_LIST_FAVOURITES ? tr("favourites") : tr("trackmenu_queue");
	sx_sheet_menu(tr("musicsettings_playback_options"), scope, items, 5);
}

bool player_lyrics_available(void) {
	device_state_t state;
	device_state_get(&state);
	return !audiobook_mode && (lyrics_available() || playing_local_file(&state));
}

bool player_lyrics_shown(void) { return lyrics_wanted; }

void player_lyrics_toggle(void) {
	if (lyrics_wanted) {
		lyrics_commit(false);
		return;
	}
	device_state_t state;
	device_state_get(&state);
	lyrics_request(&state);
	if (lyrics_loaded && lyrics_cur.count == 0 && strcmp(lyrics_path, state.current_file) == 0) {
		lyrics_offer_download();
		return;
	}
	// Up now with words for this file, or as soon as they have been read; a
	// file without any leaves the wish standing for the next one that has.
	lyrics_commit(true);
}

// ---------------------------------------------------------------------------
// The sleep timer, from the player's menu
//
// The three timers of the control centre and the Music, Audiobooks and
// Podcast pages (sleeptimer.h), not a fourth: the sheet sets the one that
// belongs to what is playing -- its length, and its switch on -- so the
// control centre's button, the settings' wheel and the moon in the status bar
// all agree with it. "At the end of the track" is the player's "stop after
// this track" (device_state_set_stop_after), which the menu also has.
// ---------------------------------------------------------------------------

static sleeptimer_kind_t sleep_kind_for_menu(void) { return SLEEPTIMER_MUSIC; }

const char *player_sleep_label(void) {
	static char label[96];
	sleeptimer_kind_t kind = sleep_kind_for_menu();
	if (sleeptimer_enabled(kind) && sleeptimer_minutes(kind) > 0) {
		char value[32];
		int minutes = sleeptimer_minutes(kind);
		if (sleeptimer_running(kind)) {
			int left = (sleeptimer_remaining(kind) + 59) / 60;
			snprintf(value, sizeof(value), tr("np_sleep_minutes"), left > 0 ? left : 1);
		} else {
			snprintf(value, sizeof(value), tr("np_sleep_minutes"), minutes);
		}
		snprintf(label, sizeof(label), "%s \xC2\xB7 %s", tr("np_sleep"), value);
	} else if (audiobook_stop_at_chapter_end()) {
		snprintf(label, sizeof(label), "%s \xC2\xB7 %s", tr("np_sleep"), tr("np_sleep_chapter"));
	} else if (device_state_stop_after() == DEVICE_STATE_STOP_AFTER_TRACK) {
		snprintf(label, sizeof(label), "%s \xC2\xB7 %s", tr("np_sleep"), tr("np_sleep_end_short"));
	} else if (device_state_stop_after() == DEVICE_STATE_STOP_AFTER_ALBUM) {
		snprintf(label, sizeof(label), "%s \xC2\xB7 %s", tr("np_sleep"), tr("np_sleep_end_album_short"));
	} else {
		snprintf(label, sizeof(label), "%s", tr("np_sleep"));
	}
	return label;
}

// What the timer was, so the toast's "Undo" can put it back.
typedef struct {
	sleeptimer_kind_t kind;
	bool enabled;
	int minutes;
	device_state_stop_after_t stop_after;
	bool chapter_end;
} sleep_was_t;
static sleep_was_t sleep_was;

static void sleep_undo(void *user) {
	(void)user;
	sleeptimer_set_minutes(sleep_was.kind, sleep_was.minutes);
	sleeptimer_set_enabled(sleep_was.kind, sleep_was.enabled);
	device_state_set_stop_after(sleep_was.stop_after);
	audiobook_set_stop_at_chapter_end(sleep_was.chapter_end);
	config_save();
	topbar_refresh_playback();
	if (sleep_timer) {
		apply_playback_status(audio_get_status());
	}
}

#define SLEEP_PICK_TRACK -1
#define SLEEP_PICK_ALBUM -2
#define SLEEP_PICK_CHAPTER -3

static void sleep_pick(void *user) {
	int choice = (int)(intptr_t)user; // minutes; 0 off; SLEEP_PICK_TRACK / _ALBUM
	sleeptimer_kind_t kind = sleep_kind_for_menu();
	sleep_was.kind = kind;
	sleep_was.enabled = sleeptimer_enabled(kind);
	sleep_was.minutes = sleeptimer_minutes(kind);
	sleep_was.stop_after = device_state_stop_after();
	sleep_was.chapter_end = audiobook_stop_at_chapter_end();
	audiobook_set_stop_at_chapter_end(choice == SLEEP_PICK_CHAPTER);

	static char text[128];
	if (choice > 0) {
		sleeptimer_start(choice);
		device_state_set_stop_after(DEVICE_STATE_STOP_AFTER_NONE); // one way to stop at a time
		char value[32];
		snprintf(value, sizeof(value), tr("np_sleep_minutes"), choice);
		snprintf(text, sizeof(text), tr("np_sleep_set"), value);
	} else if (choice == SLEEP_PICK_CHAPTER) {
		sleeptimer_set_enabled(kind, false);
		device_state_set_stop_after(DEVICE_STATE_STOP_AFTER_NONE);
		snprintf(text, sizeof(text), "%s", tr("np_sleep_chapter"));
	} else if (choice < 0) {
		bool album = choice == SLEEP_PICK_ALBUM;
		sleeptimer_set_enabled(kind, false);
		device_state_set_stop_after(album ? DEVICE_STATE_STOP_AFTER_ALBUM : DEVICE_STATE_STOP_AFTER_TRACK);
		snprintf(text, sizeof(text), "%s", tr(album ? "trackmenu_stop_after_album_set" : "trackmenu_stop_after_track_set"));
	} else {
		sleeptimer_set_enabled(kind, false);
		device_state_set_stop_after(DEVICE_STATE_STOP_AFTER_NONE);
		snprintf(text, sizeof(text), "%s", tr("np_sleep_cleared"));
	}
	// Counting from now when something of that kind is playing, and the moon in
	// the status bar either way.
	apply_playback_status(audio_get_status());
	topbar_refresh_playback();
	config_save();
	sx_toast_undo(text, sleep_undo, NULL);
}

void player_sleep_sheet_open(void) {
	sleeptimer_kind_t kind = sleep_kind_for_menu();
	int now = sleeptimer_enabled(kind) ? sleeptimer_minutes(kind) : 0;
	device_state_stop_after_t stop = device_state_stop_after();
	static char m15[32], m30[32], m60[32];
	snprintf(m15, sizeof(m15), tr("np_sleep_minutes"), 15);
	snprintf(m30, sizeof(m30), tr("np_sleep_minutes"), 30);
	snprintf(m60, sizeof(m60), tr("np_sleep_minutes"), 60);
	sx_sheet_item_t items[7] = {
		{tr("np_sleep_off"), NULL, now == 0 && stop == DEVICE_STATE_STOP_AFTER_NONE && !audiobook_stop_at_chapter_end(), false, sleep_pick, (void *)(intptr_t)0},
		{m15, NULL, now == 15, false, sleep_pick, (void *)(intptr_t)15},
		{m30, NULL, now == 30, false, sleep_pick, (void *)(intptr_t)30},
		{m60, NULL, now == 60, false, sleep_pick, (void *)(intptr_t)60},
	};
	int n = 4;
	// The end of the track (and of the album) only where there is one: not
	// on a station. The album's -- 1.2.5's "stop after this album" -- for
	// music only: a book and a podcast stop at their own ends.
	device_state_t state;
	device_state_get(&state);
	if (!state.live) {
		items[n++] = (sx_sheet_item_t){tr(audiobook_mode ? "np_sleep_chapter" : "np_sleep_end"), NULL, audiobook_mode ? audiobook_stop_at_chapter_end() : stop == DEVICE_STATE_STOP_AFTER_TRACK, false, sleep_pick, (void *)(intptr_t)(audiobook_mode ? SLEEP_PICK_CHAPTER : SLEEP_PICK_TRACK)};
		if (!audiobook_mode && !podcast_mode) {
			items[n++] = (sx_sheet_item_t){tr("np_sleep_end_album"), NULL, stop == DEVICE_STATE_STOP_AFTER_ALBUM, false, sleep_pick, (void *)(intptr_t)SLEEP_PICK_ALBUM};
		}
	}
	// A length set elsewhere (the settings' wheel: 45 minutes) is not one of
	// the four: it is said in the subtitle instead of ticked.
	static char sub[160];
	if (now > 0 && now != 15 && now != 30 && now != 60) {
		char value[32];
		snprintf(value, sizeof(value), tr("np_sleep_minutes"), now);
		snprintf(sub, sizeof(sub), "%s \xC2\xB7 %s", tr("np_sleep_sub"), value);
	} else {
		snprintf(sub, sizeof(sub), "%s", tr("np_sleep_sub"));
	}
	sx_sheet_menu(tr("np_sleep"), sub, items, n);
}

// Builds the player sheet: artwork, controls and the poll that drives them.
void player_init(gui_config_t *cfg) {
	// The artwork decodes on the shared worker; make sure it exists before
	// the first track can be started. Safe to call more than once.
	coverloader_start();
	// The next track's picture is not read ahead once the cover side has no
	// use for it.
	readahead_set_picture_known(cover_picture_known);

	lv_obj_add_style(player_screen, &theme_style_screen, 0);

	// The player is a panel parked just below the bottom edge (see player.h).
	sheet_height = (int)cfg->screen_height;
	lv_obj_set_size(player_screen, cfg->screen_width, cfg->screen_height);
	lv_obj_set_style_pad_all(player_screen, 0, 0);
	lv_obj_set_style_border_width(player_screen, 0, 0);
	lv_obj_set_style_radius(player_screen, 0, 0);
	lv_obj_set_scrollable(player_screen, false);
	lv_obj_set_pos(player_screen, 0, sheet_height);
	lv_obj_set_hidden(player_screen, true);

	// Geometry first, because everything else hangs off it (2.0, the handoff's
	// "Воспроизведение"): the artwork fills the width of the screen and its
	// height down to the panel, H - SX_NP_PANEL_H (488 on the R1, 408 on the R3
	// Pro II), centre-cropped; the panel takes the bottom SX_NP_PANEL_H with
	// its top corners rounded. The panel object reaches SX_NP_PANEL_R past the
	// bottom edge so that its own bottom corners -- an object has one radius
	// for all four -- are off the screen.
	np_panel_h = SX_NP_PANEL_H;
	int cover_size = (int)cfg->screen_width; // the artwork's width
	int cover_height = (int)cfg->screen_height - np_panel_h;
	if (cover_height < 64) {
		cover_height = 64;
	}
	int menu_height = np_panel_h;

	cover_box_w = cover_size;
	cover_box_h = cover_height;
	backdrop_w = (int)cfg->screen_width;
	backdrop_h = menu_height;

	// The page and controls share the flat canvas used by the prototype.
	lv_obj_set_style_bg_color(player_screen, sx_pal()->np_bg, 0);
	lv_obj_set_style_bg_opa(player_screen, LV_OPA_COVER, 0);

	// The panel: title, chain, bar, clocks and transport on the same canvas.
	player_menu = lv_obj_create(player_screen);
	lv_obj_set_size(player_menu, cfg->screen_width, menu_height + SX_NP_PANEL_R);
	lv_obj_set_pos(player_menu, 0, (int)cfg->screen_height - menu_height);
	lv_obj_set_style_bg_color(player_menu, sx_pal()->np_bg, 0);
	lv_obj_set_style_bg_opa(player_menu, LV_OPA_COVER, 0);
	lv_obj_set_flex_flow(player_menu, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_border_width(player_menu, 0, 0);
	lv_obj_set_style_shadow_width(player_menu, 0, 0);
	lv_obj_set_style_radius(player_menu, SX_NP_PANEL_R, 0);
	lv_obj_set_style_pad_hor(player_menu, NP_PANEL_PAD_H, 0);
	menu_style_for_layout();
	lv_obj_set_scrollable(player_menu, false);
	// Pulled down, the panel takes the player with it, as the artwork does.
	player_sheet_attach_drag(player_menu, false);

	// Track info: the text on the left, the star and the format on the right.
	lv_obj_t *song_info = lv_obj_create(player_menu);
	lv_obj_set_size(song_info, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_min_height(song_info, 80, 0);
	lv_obj_set_style_bg_opa(song_info, 0, 0);
	lv_obj_set_style_border_width(song_info, 0, 0);
	lv_obj_set_style_radius(song_info, 0, 0);
	lv_obj_set_style_pad_all(song_info, 0, 0);
	lv_obj_set_style_pad_column(song_info, 0, 0);
	lv_obj_set_flex_flow(song_info, LV_FLEX_FLOW_ROW);
	// Both columns from the top: the title is lined up with the star by hand
	// (align_title_with_star()), and with the columns centred a third line
	// under the artist (btline.h) would push the star down by half a line and
	// the title with it, onto the artist. With the star's column the taller,
	// as it is without that line, the two placements are the same.
	lv_obj_set_flex_align(song_info, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(song_info, false);
	// Every time the row is laid out (a new text size, the star shown or
	// hidden, the row coming back from another arrangement) the title is lined
	// up with the star again.
	lv_obj_add_event_cb(song_info, song_info_layout_cb, LV_EVENT_LAYOUT_CHANGED, NULL);

	lv_obj_t *song_text = lv_obj_create(song_info);
	song_text_obj = song_text;
	lv_obj_set_height(song_text, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(song_text, 1);
	lv_obj_set_style_bg_opa(song_text, 0, 0);
	lv_obj_set_style_border_width(song_text, 0, 0);
	lv_obj_set_style_pad_all(song_text, 0, 0);
	lv_obj_set_flex_flow(song_text, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(song_text, 4, 0);
	lv_obj_set_scrollable(song_text, false);

	// Long titles scroll rather than end in an ellipsis: on a 480 px panel a
	// good half of real album titles do not fit, and the tail is usually the
	// part that tells them apart.
	song_title_label = lv_label_create(song_text);
	lv_label_set_text(song_title_label, tr("player_no_track_loaded"));
	lv_obj_set_width(song_title_label, lv_pct(100));
	lv_obj_add_style(song_title_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(song_title_label, &font_ui_32_bold, 0);
	scrolltext_apply(song_title_label);

	// The artist line -- see refresh_now_playing().
	song_artist_label = lv_label_create(song_text);
	lv_label_set_text(song_artist_label, "");
	lv_obj_set_width(song_artist_label, lv_pct(100));
	lv_obj_add_style(song_artist_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(song_artist_label, &font_ui_22, 0);
	scrolltext_apply(song_artist_label);
	lv_obj_set_clickable(song_artist_label, true);
	lv_obj_set_ext_click_area(song_artist_label, 8);
	lv_obj_add_event_cb(song_artist_label, artist_label_clicked_cb, LV_EVENT_CLICKED, NULL);

	// The Bluetooth line that stood under the artist (btline.h) is the signal
	// chain's line now, under the whole row (np_chain_build): the same words,
	// with the file in front of them, and the same setting showing or hiding
	// it (Bluetooth audio settings > "Show details on Now Playing").

	// Right side: the favourite star over the stream format.
	lv_obj_t *song_side = lv_obj_create(song_info);
	song_side_obj = song_side;
	lv_obj_set_size(song_side, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(song_side, 0, 0);
	lv_obj_set_style_border_width(song_side, 0, 0);
	lv_obj_set_style_pad_all(song_side, 0, 0);
	lv_obj_set_style_pad_gap(song_side, 2, 0);
	lv_obj_set_flex_flow(song_side, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(song_side, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(song_side, false);

	lv_obj_t *fav_btn = lv_btn_create(song_side);
	fav_btn_obj = fav_btn;
	lv_obj_set_size(fav_btn, 88, 80);
	lv_obj_set_style_bg_opa(fav_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(fav_btn, 0, 0);
	lv_obj_set_style_shadow_width(fav_btn, 0, 0);
	lv_obj_set_style_pad_all(fav_btn, 0, 0);
	lv_obj_add_event_cb(fav_btn, fav_btn_event_cb, LV_EVENT_CLICKED, NULL);

	fav_btn_icon = lv_image_create(fav_btn);
	lv_obj_center(fav_btn_icon);
	update_fav_button();

	format_label = lv_label_create(song_side);
	lv_label_set_text(format_label, "");
	lv_obj_add_style(format_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(format_label, &font_ui_18, 0);
	lv_obj_set_hidden(format_label, true);

	np_chain_build(player_menu);

	// Playback progress, drawn as described at PROGRESS_TRACK_HEIGHT.
	progress_slider = lv_slider_create(player_menu);
	lv_obj_set_width(progress_slider, lv_pct(100));
	lv_obj_set_height(progress_slider, PROGRESS_TRACK_HEIGHT);
	lv_slider_set_range(progress_slider, 0, 1000);
	// 2.0: the bar sits in a band of SX_NP_SEEK_BAND_H, the track in its
	// middle; the band above and below it is the slider's own margin, and a
	// press anywhere in it seeks -- the chain line above and the clocks below
	// keep theirs.
	lv_obj_set_style_margin_ver(progress_slider, NP_SEEK_MARGIN, 0);
	lv_obj_set_ext_click_area(progress_slider, NP_SEEK_MARGIN);

	lv_obj_set_style_radius(progress_slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
	lv_obj_set_style_bg_color(progress_slider, lv_color_make(255, 255, 255), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(progress_slider, LV_OPA_30, LV_PART_MAIN);

	lv_obj_set_style_radius(progress_slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(progress_slider, chrome_accent(), LV_PART_INDICATOR);

	lv_obj_set_style_radius(progress_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
	lv_obj_set_style_bg_color(progress_slider, chrome_accent(), LV_PART_KNOB);
	lv_obj_set_style_border_color(progress_slider, lv_color_white(), LV_PART_KNOB);
	lv_obj_set_style_border_opa(progress_slider, LV_OPA_COVER, LV_PART_KNOB);
	lv_obj_set_style_border_width(progress_slider, PROGRESS_KNOB_RING, LV_PART_KNOB);
	lv_obj_set_style_pad_all(progress_slider, PROGRESS_KNOB_GROW, LV_PART_KNOB);
	// No shadow under the knob: 2.0 draws layers with light and a line, never
	// with shadows (handoff, "Слои").

	lv_obj_add_event_cb(progress_slider, progress_slider_event_cb, LV_EVENT_ALL, NULL);

	// Polls playback to keep the slider, the clock and the play/pause icon in
	// sync. It runs from here on, slowing down rather than stopping when
	// nothing is playing (see apply_playback_status).
	smooth_timer = lv_timer_create(smooth_timer_cb, SMOOTH_PERIOD_MS, NULL);
	lv_timer_pause(smooth_timer); // the poll starts it when something is playing

	progress_slider_timer = lv_timer_create(progress_slider_timer_cb, POLL_PERIOD_IDLE_MS, NULL);
	audio_set_completion_hook(track_end_hook);
	// Slowed in standby, never stopped: this timer does more than paint --
	// update_progress() calls device_state_take_completion(), the engine that
	// advances the queue when a track ends on its own. Stopping it when the
	// screen goes off (on the grounds that nobody can see the slider) meant no
	// next track ever started with the screen dark, local files included,
	// until the screen came back on and consumed the completion left waiting.
	// The period is still apply_playback_status's to decide (fast while
	// playing, slow when stopped), in the dark too.
	power_slow_in_standby(progress_slider_timer, POLL_PERIOD_PLAYING_MS);

	// The sleep timer's own tick. Slowed in standby and not stopped, because
	// the screen being dark is the normal case for it -- and slowing it costs
	// nothing in accuracy: the countdown is against the clock, so a late tick
	// stops the music late by however late it was, not by a tick.
	sleep_timer = lv_timer_create(sleep_timer_cb, 1000, NULL);
	lv_timer_pause(sleep_timer); // apply_playback_status starts it when there is a stretch to count
	power_slow_in_standby(sleep_timer, 5000);

	lv_obj_t *below_slider_group = lv_obj_create(player_menu);
	below_slider_obj = below_slider_group;
	lv_obj_set_size(below_slider_group, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(below_slider_group, 0, 0);
	lv_obj_set_style_border_width(below_slider_group, 0, 0);
	lv_obj_set_style_radius(below_slider_group, 0, 0);
	lv_obj_set_style_pad_all(below_slider_group, 0, 0);
	lv_obj_set_scrollable(below_slider_group, false);
	// Tucked up under the band the bar sits in, as the prototype has it.
	lv_obj_set_style_margin_top(below_slider_group, NP_TIMES_LIFT, 0);

	// Elapsed and total time, tucked under the two ends of the bar.
	elapsed_label = lv_label_create(below_slider_group);
	lv_label_set_text(elapsed_label, "0:00");
	lv_obj_add_style(elapsed_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(elapsed_label, &font_ui_20, 0);
	lv_obj_set_align(elapsed_label, LV_ALIGN_LEFT_MID);

	remaining_label = lv_label_create(below_slider_group);
	lv_label_set_text(remaining_label, "0:00");
	lv_obj_add_style(remaining_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(remaining_label, &font_ui_20, 0);
	lv_obj_set_align(remaining_label, LV_ALIGN_RIGHT_MID);

	// Between them, in the middle the two clocks left empty. Dimmer than they
	// are: it is a bearing, not a reading.
	queue_position_label = lv_label_create(below_slider_group);
	lv_label_set_text(queue_position_label, "");
	lv_obj_add_style(queue_position_label, &theme_style_text_dim, 0);
	// 2.0: the same size as the clocks either side of it and only dimmer, as
	// the handoff draws it.
	lv_obj_set_style_text_font(queue_position_label, &font_ui_20, 0);
	lv_obj_set_align(queue_position_label, LV_ALIGN_CENTER);

	// Transport: prev, play/pause, next -- with the repeat toggle parked on
	// the left, out of the way of the transport controls.
	lv_obj_t *player_controls_buttons = lv_obj_create(player_menu);
	controls_row = player_controls_buttons;
	lv_obj_set_size(player_controls_buttons, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_ignore_layout(player_controls_buttons, true);
	lv_obj_align(player_controls_buttons, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_style_bg_opa(player_controls_buttons, 0, 0);
	lv_obj_set_style_border_width(player_controls_buttons, 0, 0);
	lv_obj_set_style_radius(player_controls_buttons, 0, 0);
	lv_obj_set_style_pad_all(player_controls_buttons, 0, 0);
	lv_obj_set_flex_flow(player_controls_buttons, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(player_controls_buttons, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	// 2.0: the five spread across the panel's width, SX_NP_SIDE_BTN at the
	// ends, SX_NP_SKIP either side of the SX_NP_PLAY disc; the three in the
	// middle stay centred and the gap between them is what the width leaves.
	lv_obj_set_style_pad_column(player_controls_buttons, NP_TRANSPORT_GAP, 0);
	lv_obj_set_style_margin_top(player_controls_buttons, NP_TRANSPORT_TOP, 0);

	// The repeat/shuffle button, kept out of the flex row so the transport
	// stays centred on the screen.
	lv_obj_t *repeat_btn = lv_btn_create(player_controls_buttons);
	repeat_btn_obj = repeat_btn;
	lv_obj_set_ignore_layout(repeat_btn, true);
	lv_obj_set_size(repeat_btn, SX_NP_SIDE_BTN, SX_NP_SIDE_BTN);
	lv_obj_set_style_bg_opa(repeat_btn, 0, 0);
	lv_obj_set_style_shadow_width(repeat_btn, 0, 0);
	lv_obj_add_event_cb(repeat_btn, repeat_btn_event_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_align(repeat_btn, LV_ALIGN_LEFT_MID, 0, 0);
	// The glyph stays 56 px; the area that takes the tap does not, out to the
	// height of the row and short of the previous button.
	lv_obj_set_ext_click_area(repeat_btn, CORNER_BTN_EXT_CLICK);

	repeat_btn_icon = lv_image_create(repeat_btn);
	lv_obj_center(repeat_btn_icon);
	update_repeat_button(); // the icon for the mode already in force

	// The playback-speed gauge, in exactly the same corner: on a book the
	// repeat mode is meaningless and the speed is the thing worth reaching,
	// so one hides and the other appears in its place.
	lv_obj_t *speed_btn = lv_btn_create(player_controls_buttons);
	speed_btn_obj = speed_btn;
	lv_obj_set_ignore_layout(speed_btn, true);
	lv_obj_set_size(speed_btn, SX_NP_SIDE_BTN, SX_NP_SIDE_BTN);
	lv_obj_set_style_bg_opa(speed_btn, 0, 0);
	lv_obj_set_style_shadow_width(speed_btn, 0, 0);
	lv_obj_add_event_cb(speed_btn, speed_btn_event_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_align(speed_btn, LV_ALIGN_LEFT_MID, 0, 0);
	lv_obj_set_ext_click_area(speed_btn, CORNER_BTN_EXT_CLICK);
	lv_obj_set_hidden(speed_btn, true);

	speed_btn_icon = lv_image_create(speed_btn);
	lv_image_set_src(speed_btn_icon, &icon_play_speed);
	lv_obj_add_style(speed_btn_icon, &theme_style_icon, 0);
	lv_obj_center(speed_btn_icon);
	update_speed_button();

	// The overflow menu, mirroring the repeat button on the right: queue and
	// details in an iOS-style popover next to the button.
	lv_obj_t *more_btn = lv_btn_create(player_controls_buttons);
	more_btn_obj = more_btn;
	lv_obj_set_ignore_layout(more_btn, true);
	lv_obj_set_size(more_btn, SX_NP_SIDE_BTN, SX_NP_SIDE_BTN);
	lv_obj_set_style_bg_opa(more_btn, 0, 0);
	lv_obj_set_style_shadow_width(more_btn, 0, 0);
	lv_obj_add_event_cb(more_btn, more_btn_event_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_align(more_btn, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_set_ext_click_area(more_btn, CORNER_BTN_EXT_CLICK);

	lv_obj_t *more_icon = lv_image_create(more_btn);
	more_btn_icon = more_icon;
	lv_image_set_src(more_icon, &icon_ellipsis_vertical);
	lv_obj_add_style(more_icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor(more_icon, lv_color_white(), 0);
	lv_obj_set_style_image_recolor_opa(more_icon, LV_OPA_COVER, 0);
	lv_obj_center(more_icon);

	// No button chrome on prev/next: they sit on top of the blurred artwork,
	// and plain glyphs read better there than filled boxes.
	lv_obj_t *prev_btn = lv_btn_create(player_controls_buttons);
	prev_btn_obj = prev_btn;
	lv_obj_set_size(prev_btn, SX_NP_SKIP, SX_NP_SKIP);
	lv_obj_set_style_bg_opa(prev_btn, 0, 0);
	lv_obj_set_style_shadow_width(prev_btn, 0, 0);
	prev_icon = lv_image_create(prev_btn);
	lv_image_set_src(prev_icon, &icon_skip_back);
	lv_obj_add_style(prev_icon, &theme_style_icon, 0);
	lv_obj_center(prev_icon);

	lv_obj_add_event_cb(prev_btn, prev_btn_event_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_set_style_opa(prev_btn, LV_OPA_40, LV_STATE_DISABLED); // see apply_connecting()

	// Play/pause: a white disc, with the glyph carrying the colour.
	play_btn = lv_btn_create(player_controls_buttons);
	lv_obj_set_size(play_btn, SX_NP_PLAY, SX_NP_PLAY);
	lv_obj_set_style_radius(play_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(play_btn, sx_pal()->mini_action, 0); // repainted by set_over_cover
	lv_obj_set_style_bg_opa(play_btn, LV_OPA_COVER, 0);
	lv_obj_set_style_shadow_width(play_btn, 0, 0);
	lv_obj_add_event_cb(play_btn, play_btn_event_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_set_style_opa(play_btn, LV_OPA_40, LV_STATE_DISABLED);

	play_btn_icon = lv_image_create(play_btn);
	lv_obj_center(play_btn_icon);
	apply_playback_status(AUDIO_STATUS_STOPPED); // start on the play glyph

	lv_obj_t *next_btn = lv_btn_create(player_controls_buttons);
	next_btn_obj = next_btn;
	lv_obj_set_size(next_btn, SX_NP_SKIP, SX_NP_SKIP);
	lv_obj_set_style_bg_opa(next_btn, 0, 0);
	lv_obj_set_style_shadow_width(next_btn, 0, 0);
	next_icon = lv_image_create(next_btn);
	lv_image_set_src(next_icon, &icon_skip_forward);
	lv_obj_add_style(next_icon, &theme_style_icon, 0);
	lv_obj_center(next_icon);

	lv_obj_add_event_cb(next_btn, next_btn_event_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_set_style_opa(next_btn, LV_OPA_40, LV_STATE_DISABLED);

	// Album art: a square as wide as the screen, flush against the top edge
	// (this page hides the status bar, see switch_screen). The picture is
	// centre-cropped to fill it rather than letterboxed. Sizes were worked out
	// at the top of this function, before the controls were laid out.
	cover_panel = lv_obj_create(player_screen);
	// Dragging the artwork to the right pushes the player back off screen.
	player_sheet_attach_drag(cover_panel, false);
	lv_obj_set_size(cover_panel, cover_box_w, cover_box_h);
	lv_obj_align(cover_panel, LV_ALIGN_TOP_MID, 0, 0);
	// Until the picture (or its 240 px thumbnail) is up, the plaque in the
	// album's tone, as every 2.0 cover is drawn first.
	lv_obj_set_style_bg_color(cover_panel, sx_pal()->np_bg, 0);
	// A tap pauses, a swipe across skips; the pull down is the sheet's.
	lv_obj_add_event_cb(cover_panel, cover_gesture_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(cover_panel, cover_gesture_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(cover_panel, cover_gesture_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(cover_panel, cover_gesture_cb, LV_EVENT_PRESS_LOST, NULL);
	lv_obj_set_style_border_width(cover_panel, 0, 0);
	lv_obj_set_style_radius(cover_panel, 0, 0);
	lv_obj_set_style_pad_all(cover_panel, 0, 0);
	lv_obj_set_scrollable(cover_panel, false);

	// The live badge, top right over the artwork: the one thing that has to be
	// legible at a glance is that this is not a file being played but a
	// broadcast going past. Red because that is what a live indicator is, and
	// deliberately not the accent colour -- it says something about the
	// stream, not about the theme.
	live_badge = lv_label_create(cover_panel);
	lv_label_set_text(live_badge, tr("player_live"));
	lv_obj_set_style_text_font(live_badge, &font_ui_18, 0);
	lv_obj_set_style_text_color(live_badge, lv_color_white(), 0);
	lv_obj_set_style_bg_color(live_badge, lv_color_make(224, 27, 36), 0); // Adwaita red
	lv_obj_set_style_bg_opa(live_badge, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(live_badge, 6, 0);
	lv_obj_set_style_pad_hor(live_badge, 10, 0);
	lv_obj_set_style_pad_ver(live_badge, 5, 0);
	lv_obj_align(live_badge, LV_ALIGN_TOP_RIGHT, -BADGE_INSET, badge_top());
	lv_obj_set_hidden(live_badge, true);

	// The streaming service's mark (Qobuz, Tidal...), in the same corner and
	// for the same reason: say where what is playing comes from. The picture
	// is the service's own (stream_provider_t.gui.badge), set by
	// update_service_badge(): white-outlined images, legible even over a black
	// cover, and not recoloured (theme_style_icon would turn them into a solid
	// square).
	service_badge = lv_image_create(cover_panel);
	lv_image_set_src(service_badge, &icon_qobuz_badge);
	lv_obj_align(service_badge, LV_ALIGN_TOP_RIGHT, -BADGE_INSET, badge_top());
	lv_obj_set_hidden(service_badge, true);

	// And the podcast one, same corner and same size: the marks replace each
	// other instead of sitting side by side, and update_service_badge()
	// decides which.
	podcast_badge = lv_image_create(cover_panel);
	lv_image_set_src(podcast_badge, &icon_podcast_badge);
	lv_obj_align(podcast_badge, LV_ALIGN_TOP_RIGHT, -BADGE_INSET, badge_top());
	lv_obj_set_hidden(podcast_badge, true);

	// Shown while there is no artwork for the current track.
	cover_art = sx_box(cover_panel);
	lv_obj_set_style_bg_opa(cover_art, LV_OPA_COVER, 0);
	lv_obj_set_style_clip_corner(cover_art, true, 0);
	cover_placeholder_icon = lv_image_create(cover_art);
	lv_image_set_src(cover_placeholder_icon, &icon_music_note);
	lv_obj_add_style(cover_placeholder_icon, &theme_style_text_dim, 0);
	lv_obj_set_style_image_recolor(cover_placeholder_icon, sx_placeholder_ink(sx_pal()->mini_cover_tone), 0);
	lv_obj_set_style_image_recolor_opa(cover_placeholder_icon, LV_OPA_COVER, 0);
	lv_obj_set_style_image_opa(cover_placeholder_icon, LV_OPA_50, 0);
	lv_obj_center(cover_placeholder_icon);

	// The album's 240 px thumbnail stretched over the artwork's place, while
	// the picture itself is being decoded (handoff, cover pipeline step 5).
	cover_thumb_img = lv_image_create(cover_art);
	lv_obj_set_size(cover_thumb_img, cover_box_w, cover_box_h);
	lv_obj_center(cover_thumb_img);
	lv_image_set_inner_align(cover_thumb_img, LV_IMAGE_ALIGN_COVER);
	// Nearest-neighbour: it is up for a fraction of a second, and smoothing a
	// 240 px picture to twice its size every frame of the sheet's slide is
	// work for nothing.
	lv_image_set_antialias(cover_thumb_img, false);
	lv_obj_set_clickable(cover_thumb_img, false);
	lv_obj_set_hidden(cover_thumb_img, true);

	cover_img = lv_image_create(cover_art);
	lv_obj_center(cover_img);
	lv_obj_set_hidden(cover_img, true);

	lyrics_background = lv_image_create(player_screen);
	lv_obj_set_ignore_layout(lyrics_background, true);
	lv_obj_set_size(lyrics_background, cfg->screen_width, cfg->screen_height);
	lv_obj_set_pos(lyrics_background, 0, 0);
	// COVER and not STRETCH: the blurred copy is asked for at the shape of the
	// screen, but between a change of arrangement and the picture that follows
	// it the one on hand is still the old shape, and stretching that is a
	// visibly squashed sleeve.
	lv_image_set_inner_align(lyrics_background, LV_IMAGE_ALIGN_COVER);
	lv_obj_set_clickable(lyrics_background, false);
	lv_obj_set_hidden(lyrics_background, true);
	lv_obj_move_background(lyrics_background);

	lyrics_build();

	// Over the artwork, at the top right: what comes next, and the way to the
	// queue.
	cover_art_layout();
	np_upnext_build();

	// Last, so it is above the artwork: an invisible strip exactly where the
	// status bar would be, owning the pull-down that opens the control centre.
	lv_obj_t *panel_edge = lv_obj_create(player_screen);
	lv_obj_remove_style_all(panel_edge);
	lv_obj_set_size(panel_edge, cfg->screen_width, cfg->top_bar_height);
	lv_obj_set_pos(panel_edge, 0, 0);
	lv_obj_set_scrollable(panel_edge, false);
	lv_obj_set_clickable(panel_edge, true);
	lv_obj_add_event_cb(panel_edge, panel_edge_drag_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(panel_edge, panel_edge_drag_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(panel_edge, panel_edge_drag_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(panel_edge, panel_edge_drag_cb, LV_EVENT_PRESS_LOST, NULL);
	lv_obj_move_foreground(panel_edge);

	lv_obj_t *home_surfaces[] = {player_screen, player_menu, controls_row, play_btn, prev_btn_obj, next_btn_obj, repeat_btn_obj, speed_btn_obj, more_btn_obj};
	for (size_t i = 0; i < sizeof(home_surfaces) / sizeof(home_surfaces[0]); i++) {
		lv_obj_set_press_lock(home_surfaces[i], true);
		lv_obj_add_event_cb(home_surfaces[i], home_edge_drag_cb, LV_EVENT_PRESSED, NULL);
		lv_obj_add_event_cb(home_surfaces[i], home_edge_drag_cb, LV_EVENT_RELEASED, NULL);
		lv_obj_add_event_cb(home_surfaces[i], home_edge_drag_cb, LV_EVENT_PRESS_LOST, NULL);
	}

	// The backdrop over a loaded cover is always the dark treatment -- same
	// look in both themes; only the no-cover panel follows the theme.
	cover_set_backdrop_light(false);

	// The status bar over the player, as it was left.
	bar_height = (int)cfg->top_bar_height;
	player_set_statusbar_always(config_get_int("screen", "statusbar_always", 0) != 0);

	// Legacy layout choices are ignored; their saved value remains untouched.
	cover_accent = config_get_bool("screen", "cover_accent", false);
	apply_layout();

	// The page's gradients in the colour of whatever is loaded (nothing yet:
	// the neutral palette), and the news from covertone of tones and
	// thumbnails that have landed -- one of them may be the playing album's.
	np_paint_backgrounds();
	covertonefan_add(np_covertone_changed);

	theme_register_refresh(player_refresh_theme);
}
