#include "spotifypage.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/nowplaying/player.h"
#include "src/system/streaming/spotify.h"

lv_obj_t *spotify_screen;

static lv_obj_t *status_label;
static lv_obj_t *track_label;
static lv_obj_t *artist_label;
static lv_obj_t *position_label;
static lv_obj_t *play_btn_label;
static lv_timer_t *poll_timer;


static lv_obj_t *make_action_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb) {
	lv_obj_t *button = lv_btn_create(parent);
	lv_obj_set_width(button, lv_pct(100));
	lv_obj_set_height(button, 64);
	lv_obj_add_style(button, &theme_style_card, 0);
	lv_obj_add_style(button, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_t *label = lv_label_create(button);
	lv_label_set_text(label, text);
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_obj_center(label);
	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);
	return button;
}

static void refresh_now_playing(void) {
	spotify_state_t state = SPOTIFY_STATE_UNKNOWN;
	spotify_track_t track;
	uint32_t position = 0;
	bool available = spotify_get_status(&state);

	if (!available) {
		lv_label_set_text(status_label, "Spotify daemon: offline");
		lv_label_set_text(track_label, "No Spotify session");
		lv_label_set_text(artist_label, "");
		lv_label_set_text(position_label, "");
		lv_label_set_text(play_btn_label, "Play");
		return;
	}

	const char *state_text = "Unknown";
	if (state == SPOTIFY_STATE_PLAYING) state_text = "Playing";
	else if (state == SPOTIFY_STATE_PAUSED) state_text = "Paused";
	else if (state == SPOTIFY_STATE_STOPPED) state_text = "Stopped";
	lv_label_set_text_fmt(status_label, "Spotify daemon: %s", state_text);

	if (spotify_get_now_playing(&track)) {
		lv_label_set_text(track_label, track.title[0] ? track.title : "Unknown track");
		lv_label_set_text(artist_label, track.artist[0] ? track.artist : "Unknown artist");
		if (spotify_get_position(&position)) {
			unsigned cur_s = position / 1000;
			unsigned dur_s = track.duration_ms / 1000;
			lv_label_set_text_fmt(position_label, "%u:%02u / %u:%02u",
				cur_s / 60, cur_s % 60, dur_s / 60, dur_s % 60);
		} else {
			lv_label_set_text(position_label, "");
		}
	} else {
		lv_label_set_text(track_label, "Nothing playing");
		lv_label_set_text(artist_label, "");
		lv_label_set_text(position_label, "");
	}

	lv_label_set_text(play_btn_label, state == SPOTIFY_STATE_PLAYING ? "Pause" : "Play");
}

static void poll_timer_cb(lv_timer_t *timer) {
	(void)timer;
	if (spotify_start()) {
		/* The daemon can need a moment after fork before the Unix socket exists. */
	}
	refresh_now_playing();
}

static void command_failed(const char *text) {
	gui_notify_popup(text);
}

static void play_pause_cb(lv_event_t *e) {
	(void)e;
	spotify_state_t state;
	if (!spotify_get_status(&state)) {
		command_failed("Spotify is not connected");
		return;
	}
	bool ok = state == SPOTIFY_STATE_PLAYING ? spotify_pause() : spotify_play();
	if (!ok) command_failed("Spotify command failed");
	refresh_now_playing();
}

static void next_cb(lv_event_t *e) {
	(void)e;
	if (!spotify_next()) command_failed("Spotify next failed");
}

static void previous_cb(lv_event_t *e) {
	(void)e;
	if (!spotify_previous()) command_failed("Spotify previous failed");
}

static void stop_cb(lv_event_t *e) {
	(void)e;
	if (!spotify_stop()) command_failed("Spotify stop failed");
}

static void open_not_implemented_cb(lv_event_t *e) {
	(void)e;
	gui_notify_popup("Spotify browsing is the next integration stage");
}

void spotify_page_init(gui_config_t *cfg) {
	spotify_screen = lv_obj_create(NULL);
	lv_obj_add_style(spotify_screen, &theme_style_screen, 0);

	lv_obj_t *content = settingsrow_page(spotify_screen, cfg, "spotify");
	(void)content;

	status_label = lv_label_create(spotify_screen);
	lv_obj_set_width(status_label, cfg->screen_width - 2 * cfg->padding);
	lv_obj_align(status_label, LV_ALIGN_TOP_LEFT, cfg->padding, settingsrow_content_top(cfg) + 8);
	lv_obj_add_style(status_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(status_label, &font_ui_18, 0);

	track_label = lv_label_create(spotify_screen);
	lv_obj_set_width(track_label, cfg->screen_width - 2 * cfg->padding);
	lv_label_set_long_mode(track_label, LV_LABEL_LONG_DOTS);
	lv_obj_align_to(track_label, status_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 18);
	lv_obj_add_style(track_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(track_label, &font_ui_24_bold, 0);

	artist_label = lv_label_create(spotify_screen);
	lv_obj_set_width(artist_label, cfg->screen_width - 2 * cfg->padding);
	lv_label_set_long_mode(artist_label, LV_LABEL_LONG_DOTS);
	lv_obj_align_to(artist_label, track_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 4);
	lv_obj_add_style(artist_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(artist_label, &font_ui_20, 0);

	position_label = lv_label_create(spotify_screen);
	lv_obj_align_to(position_label, artist_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 10);
	lv_obj_add_style(position_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(position_label, &font_ui_18, 0);

	lv_obj_t *controls = lv_obj_create(spotify_screen);
	lv_obj_set_width(controls, cfg->screen_width - 2 * cfg->padding);
	lv_obj_set_height(controls, LV_SIZE_CONTENT);
	lv_obj_align(controls, LV_ALIGN_BOTTOM_LEFT, cfg->padding, -cfg->padding);
	lv_obj_add_style(controls, &theme_style_screen, 0);
	lv_obj_set_style_pad_all(controls, 0, 0);
	lv_obj_set_style_pad_row(controls, 8, 0);
	lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_COLUMN);
	lv_obj_remove_flag(controls, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *row = lv_obj_create(controls);
	lv_obj_set_size(row, lv_pct(100), 64);
	lv_obj_set_style_bg_opa(row, 0, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *prev = make_action_button(row, "Previous", previous_cb);
	lv_obj_set_width(prev, 145);
	lv_obj_t *play = make_action_button(row, "Play", play_pause_cb);
	lv_obj_set_width(play, 145);
	play_btn_label = lv_obj_get_child(play, 0);
	lv_obj_t *next = make_action_button(row, "Next", next_cb);
	lv_obj_set_width(next, 145);

	make_action_button(controls, "Stop", stop_cb);
	make_action_button(controls, "Liked Songs", open_not_implemented_cb);
	make_action_button(controls, "Playlists", open_not_implemented_cb);
	make_action_button(controls, "Search", open_not_implemented_cb);

	switcher_attach_back_gesture(spotify_screen);
	player_sheet_attach_drag(spotify_screen, true);

	refresh_now_playing();
	poll_timer = lv_timer_create(poll_timer_cb, 1000, NULL);
	if (poll_timer) {
		lv_timer_ready(poll_timer);
	}
}
