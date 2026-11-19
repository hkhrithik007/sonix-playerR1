#include "chapters.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/sx_row.h"
#include "src/gui/shell/sx_styles.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"
#include "src/system/playback/audiobook.h"
#include "src/system/playback/device_state.h"

lv_obj_t *chapters_screen;

// Books run to dozens of chapters, occasionally a couple of hundred; the list
// is plainly rebuilt each time it opens, like the audiobook index itself. The
// cap is there for a pathological file, not for any real book.
#define MAX_ROWS 400

static lv_obj_t *chapter_list;
static lv_obj_t *empty_label;
static int listed_count;
static unsigned listed_serial;
static unsigned listed_history;
static int listed_playing = -1;

// A folder book lists its files here instead: each file is a chapter.
static bool listing_parts;

static void format_clock(double seconds, char *out, size_t out_size) {
	if (seconds < 0) {
		seconds = 0;
	}
	long total = (long)seconds;
	long hours = total / 3600;
	long minutes = (total % 3600) / 60;
	long secs = total % 60;

	if (hours > 0) {
		snprintf(out, out_size, "%ld:%02ld:%02ld", hours, minutes, secs);
	} else {
		snprintf(out, out_size, "%ld:%02ld", minutes, secs);
	}
}

// Starts part `index` of the folder book from its beginning, with the book's
// parts as the queue again, whatever the queue had become.
static bool play_part(int index) {
	int count = audiobook_part_count();
	if (index < 0 || index >= count) {
		return false;
	}
	char **paths = calloc((size_t)count, sizeof(char *));
	if (!paths) {
		return false;
	}
	int filled = 0;
	for (int i = 0; i < count; i++) {
		char path[512];
		if (!audiobook_part(i, NULL, 0, path, sizeof(path)) || !(paths[filled] = strdup(path))) {
			break;
		}
		filled++;
	}
	bool ok = filled == count;
	if (ok) {
		device_state_play_list_ordered((const char *const *)paths, count, index);
	}
	for (int i = 0; i < filled; i++) {
		free(paths[i]);
	}
	free(paths);
	return ok;
}

static void chapter_row_tap(lv_obj_t *row, void *user) {
	(void)row;
	if (player_sheet_drag_active() || switcher_back_drag_active())
		return;
	int index = (int)(intptr_t)user;
	if (listing_parts) {
		if (!play_part(index))
			return;
	} else {
		double start;
		if (!audiobook_chapter(index, NULL, 0, &start))
			return;
		device_state_seek(start);
		audiobook_note_position(start, 0, true);
	}
	player_refresh_now_playing();
	back_btn_cb(NULL);
}

bool chapters_step(int direction) {
	if (!audiobook_is_playing() || (direction != -1 && direction != 1)) {
		return false;
	}
	if (audiobook_part_count() > 1) {
		if (!play_part(audiobook_part_current() + direction)) {
			return false;
		}
	} else {
		device_state_t state;
		device_state_get(&state);
		int current = audiobook_chapter_at(state.progress_current_secs);
		double start;
		if (current < 0 || !audiobook_chapter(current + direction, NULL, 0, &start)) {
			return false;
		}
		device_state_seek(start);
		audiobook_note_position(start, 0, true);
	}
	player_refresh_now_playing();
	return true;
}

static void row_state(lv_obj_t *row, int i, const char *clock, int playing) {
	bool listened = listing_parts ? audiobook_part_listened(i) : audiobook_chapter_listened(i);
	const char *status = i == playing ? "chapter_current_position" : listened ? "chapter_listened" : "chapter_not_listened";
	sx_row_set_subtitle(row, tr(status), NULL, clock);
	sx_row_set_playing(row, i == playing);
	sx_row_set_trail(row, listened ? SX_ROW_TRAIL_CHECK : SX_ROW_TRAIL_NONE);
}

static lv_obj_t *make_row(int i, const char *title, const char *clock, int playing) {
	const sx_row_handlers_t handlers = {.on_tap = chapter_row_tap};
	lv_obj_t *row = sx_row_create(chapter_list, &handlers, false);
	sx_row_set_user(row, (void *)(intptr_t)i);
	sx_row_set_number(row, i + 1);
	sx_row_set_title(row, title[0] ? title : tr("chapter"));
	row_state(row, i, clock, playing);
	return row;
}

// The row to open the list on: the chapter, or the part, being listened to.
static int playing_row(void) {
	if (listing_parts) {
		return audiobook_part_current();
	}
	device_state_t state;
	device_state_get(&state);
	return audiobook_chapter_at(state.progress_current_secs);
}

static void rebuild(void) {
	lv_obj_clean(chapter_list);
	empty_label = NULL;

	listing_parts = audiobook_part_count() > 1;
	if (listing_parts) {
		int count = audiobook_part_count();
		if (count > MAX_ROWS) {
			count = MAX_ROWS;
		}
		listed_count = count;
		int playing = playing_row();
		for (int i = 0; i < count; i++) {
			char title[192] = "";
			if (!audiobook_part(i, title, sizeof(title), NULL, 0)) {
				break;
			}
			make_row(i, title, NULL, playing);
		}
		return;
	}

	int count = audiobook_chapter_count();
	if (count > MAX_ROWS) {
		count = MAX_ROWS;
	}
	listed_count = count;

	if (count == 0) {
		empty_label = lv_label_create(chapter_list);
		lv_label_set_text(empty_label, tr("chapters_empty"));
		lv_obj_set_style_text_align(empty_label, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_width(empty_label, lv_pct(100));
		lv_obj_add_style(empty_label, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(empty_label, &font_ui_24, 0);
		lv_obj_set_style_pad_top(empty_label, 80, 0);
		return;
	}

	device_state_t state;
	device_state_get(&state);
	int playing = audiobook_chapter_at(state.progress_current_secs);

	for (int i = 0; i < count; i++) {
		char title[192] = "";
		double start = 0;
		if (!audiobook_chapter(i, title, sizeof(title), &start)) {
			break;
		}

		char clock[24];
		format_clock(start, clock, sizeof(clock));
		make_row(i, title, clock, playing);
	}
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	rebuild();

	// Open on the chapter being listened to rather than at the top: on a book
	// with sixty chapters, the one that matters is the one playing.
	int playing = playing_row();
	listed_playing = playing;
	listed_serial = audiobook_serial();
	listed_history = audiobook_history_serial();
	if (playing > 0 && playing < listed_count) {
		lv_obj_t *row = lv_obj_get_child(chapter_list, playing);
		if (row) {
			lv_obj_scroll_to_view(row, LV_ANIM_OFF);
		}
	}
}

void chapters_refresh(void) {
	if (!chapters_screen || lv_screen_active() != chapters_screen)
		return;
	int playing = playing_row();
	if (listed_serial != audiobook_serial()) {
		rebuild();
		listed_serial = audiobook_serial();
		playing = playing_row();
	} else if (playing != listed_playing || listed_history != audiobook_history_serial()) {
		for (int i = 0; i < listed_count; i++) {
			lv_obj_t *row = lv_obj_get_child(chapter_list, i);
			if (!row)
				break;
			char clock[24];
			double start;
			const char *time = NULL;
			if (!listing_parts && audiobook_chapter(i, NULL, 0, &start)) {
				format_clock(start, clock, sizeof(clock));
				time = clock;
			}
			row_state(row, i, time, playing);
		}
	}
	listed_playing = playing;
	listed_history = audiobook_history_serial();
}

void chapters_open(void) {
	bool return_to_player = player_sheet_is_open();
	player_sheet_close(false);
	switch_screen(chapters_screen);
	switcher_set_player_return(return_to_player ? chapters_screen : NULL);
}

void chapters_init(gui_config_t *cfg) {
	lv_obj_add_style(chapters_screen, &sx_style_page, 0);
	settingsrow_title(chapters_screen, cfg, "chapters");

	int content_top = settingsrow_content_top(cfg);

	chapter_list = lv_obj_create(chapters_screen);
	lv_obj_set_size(chapter_list, lv_pct(100), cfg->screen_height - content_top);
	lv_obj_align(chapter_list, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(chapter_list, 0, 0);
	lv_obj_set_style_border_width(chapter_list, 0, 0);
	lv_obj_set_style_radius(chapter_list, 0, 0);
	lv_obj_set_style_pad_hor(chapter_list, cfg->padding, 0);
	lv_obj_set_style_pad_ver(chapter_list, 0, 0);
	lv_obj_set_style_pad_gap(chapter_list, 8, 0);
	lv_obj_set_scroll_dir(chapter_list, LV_DIR_VER);
	lv_obj_set_flex_flow(chapter_list, LV_FLEX_FLOW_COLUMN);
	// Set here, not in the row builder: a book with no chapters creates no
	// rows, and the page still has to answer the swipe-back.
	lv_obj_set_event_bubble(chapter_list, true);

	// No player-sheet swipe here, for the queue page's reason: it would slide
	// the sheet over the list being read. The swipe-back still works.
	switcher_attach_back_gesture(chapter_list);
	lv_obj_add_event_cb(chapters_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}
