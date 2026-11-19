#include "lyricsearch.h"
#include "src/gui/shell/artview.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/sx_row.h"
#include "src/gui/shell/sx_styles.h"
#include "src/gui/shell/sx_tokens.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/sx_card.h"
#include "src/gui/shell/sx_mini.h"
#include "src/gui/shell/sx_palette.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"

lv_obj_t *lyricsearch_screen;

#define APPLY_W 230
#define APPLY_H 64

static lv_obj_t *title_label;
static lv_obj_t *list;		   // the records
static lv_obj_t *preview;	   // the words of one of them
static lv_obj_t *preview_text; // inside `preview`
static lv_obj_t *apply_bar;	   // Apply, under the words

static char file_path[1024];
static lrclib_record_t *records;
static int record_count;
static int previewing = -1; // the record whose words are up, -1 on the list
static unsigned generation;
static pthread_mutex_t save_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct {
	char path[1024];
	char *text;
	unsigned generation;
	bool ok;
} save_job_t;
static save_job_t *save_done;
static bool saving;

static void *save_worker(void *arg) {
	save_job_t *job = arg;
	job->ok = lrclib_save(job->path, job->text);
	free(job->text);
	job->text = NULL;
	pthread_mutex_lock(&save_lock);
	save_done = job;
	pthread_mutex_unlock(&save_lock);
	return NULL;
}

static void save_poll(lv_timer_t *timer) {
	pthread_mutex_lock(&save_lock);
	save_job_t *job = save_done;
	save_done = NULL;
	pthread_mutex_unlock(&save_lock);
	if (!job) {
		return;
	}
	saving = false;
	lv_timer_delete(timer);
	if (!job->ok) {
		toast_error("lyrics_not_saved");
	} else {
		if (job->generation == generation && lv_screen_active() == lyricsearch_screen) {
			back_btn_cb(NULL);
		}
		player_lyrics_reload(job->path);
		toast_success("lyrics_downloaded");
	}
	free(job);
}

static void format_length(int seconds, char *out, size_t size) {
	if (seconds <= 0) {
		out[0] = '\0';
		return;
	}
	snprintf(out, size, "%d:%02d", seconds / 60, seconds % 60);
}

// The words as they are read: timed lines without their "[01:02.34]" stamps,
// and the tag lines of an LRC file ("[ar:...]") left out. Empty lines are kept,
// since they are where the verses break.
static char *readable_text(const char *text) {
	char *out = malloc(strlen(text) + 1);
	if (!out) {
		return NULL;
	}
	char *w = out;
	const char *line = text;
	while (*line) {
		const char *end = strchr(line, '\n');
		size_t len = end ? (size_t)(end - line) : strlen(line);
		const char *p = line;
		const char *stop = line + len;
		bool stamped = false;
		while (p < stop && *p == '[') {
			const char *close = memchr(p, ']', (size_t)(stop - p));
			if (!close) {
				break;
			}
			p = close + 1;
			stamped = true;
		}
		while (p < stop && (*p == ' ' || *p == '\t')) {
			p++;
		}
		size_t rest = (size_t)(stop - p);
		if (rest > 0 && p[rest - 1] == '\r') {
			rest--;
		}
		if (rest > 0 || !stamped) {
			memcpy(w, p, rest);
			w += rest;
			*w++ = '\n';
		}
		if (!end) {
			break;
		}
		line = end + 1;
	}
	while (w > out && w[-1] == '\n') {
		w--;
	}
	*w = '\0';
	return out;
}

static void show_list(void) {
	previewing = -1;
	lv_label_set_text(title_label, tr("lyrics_results"));
	lv_obj_set_hidden(preview, true);
	lv_obj_set_hidden(apply_bar, true);
	lv_obj_set_hidden(list, false);
}

static void show_preview(int index) {
	if (index < 0 || index >= record_count) {
		return;
	}
	char *text = readable_text(records[index].text);
	if (!text) {
		return;
	}
	previewing = index;
	lv_label_set_text(title_label, records[index].track[0] ? records[index].track : tr("lyrics_results"));
	lv_label_set_text(preview_text, text);
	free(text);
	lv_obj_scroll_to_y(preview, 0, LV_ANIM_OFF);
	lv_obj_set_hidden(list, true);
	lv_obj_set_hidden(preview, false);
	lv_obj_set_hidden(apply_bar, false);
}

// The chevron and the swipe-back from the words go back to the list.
static bool back_guard(void) {
	if (previewing < 0) {
		return false;
	}
	show_list();
	return true;
}

static bool back_peek(void) { return previewing >= 0; }

static void result_tap(lv_obj_t *row, void *user) {
	(void)row;
	if (!switcher_back_drag_active())
		show_preview((int)(intptr_t)user);
}

static void apply_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active() || previewing < 0 || saving) {
		return;
	}
	save_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		toast_error("lyrics_not_saved");
		return;
	}
	job->text = strdup(records[previewing].text);
	job->generation = generation;
	snprintf(job->path, sizeof(job->path), "%s", file_path);
	lv_timer_t *timer = job->text ? lv_timer_create(save_poll, 100, NULL) : NULL;
	pthread_t thread;
	if (!timer || pthread_create(&thread, NULL, save_worker, job) != 0) {
		if (timer) {
			lv_timer_delete(timer);
		}
		free(job->text);
		free(job);
		toast_error("lyrics_not_saved");
		return;
	}
	saving = true;
	pthread_detach(thread);
}

static void add_row(int index) {
	const lrclib_record_t *record = &records[index];
	const sx_row_handlers_t handlers = {.on_tap = result_tap};
	lv_obj_t *row = sx_row_create(list, &handlers, false);
	lv_obj_set_width(row, lv_pct(100));
	sx_row_set_user(row, (void *)(intptr_t)index);
	sx_row_set_title(row, record->track[0] ? record->track : "-");
	char who[512], length[16];
	snprintf(who, sizeof(who), "%s%s%s", record->artist, record->album[0] ? " - " : "", record->album);
	format_length(record->duration, length, sizeof(length));
	sx_row_set_subtitle(row, who, tr(record->synced ? "lyrics_synced" : "lyrics_plain"), length);
	sx_row_set_glyph(row, &icon_lyrics_search, sx_placeholder_name(record->track));
	sx_row_set_content(row, sx_palette_hue_of_name(record->track), false, false, 0);
}

void lyricsearch_open(const char *path, lrclib_record_t *records_in, int count) {
	gui_page_ensure(lyricsearch_screen);
	lrclib_free(records, record_count);
	records = records_in;
	record_count = count;
	snprintf(file_path, sizeof(file_path), "%s", path);

	lv_obj_clean(list);
	for (int i = 0; i < record_count; i++) {
		add_row(i);
	}
	lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
	show_list();

	player_sheet_close(false);
	switch_screen(lyricsearch_screen);
	switcher_set_player_return(lyricsearch_screen);
}

// Left, whichever way: the records go, they are a few hundred kilobytes at most
// but nothing needs them any more.
static void unloaded_cb(lv_event_t *e) {
	(void)e;
	generation++;
	show_list();
	lv_obj_clean(list);
	lrclib_free(records, record_count);
	records = NULL;
	record_count = 0;
}

static void build_page(gui_config_t *cfg) {
	sx_mini_hide_on_screen(lyricsearch_screen);
	lv_obj_add_style(lyricsearch_screen, &theme_style_screen, 0);
	title_label = settingsrow_title(lyricsearch_screen, cfg, "lyrics_results");
	settingsrow_title_corner_slots(title_label, cfg, 0);

	int content_top = settingsrow_content_top(cfg);
	int height = cfg->screen_height - content_top;

	list = lv_obj_create(lyricsearch_screen);
	lv_obj_set_size(list, lv_pct(100), height);
	lv_obj_align(list, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(list, 0, 0);
	lv_obj_set_style_border_width(list, 0, 0);
	lv_obj_set_style_radius(list, 0, 0);
	lv_obj_set_style_pad_hor(list, cfg->padding, 0);
	lv_obj_set_style_pad_top(list, 0, 0);
	lv_obj_set_style_pad_bottom(list, cfg->padding, 0);
	lv_obj_set_style_pad_gap(list, SX_ROW_GAP, 0);
	lv_obj_set_scroll_dir(list, LV_DIR_VER);
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_event_bubble(list, true);
	switcher_attach_back_gesture(list);

	// The words, above a bar holding Apply.
	int bar_h = APPLY_H + 2 * cfg->padding;
	preview = lv_obj_create(lyricsearch_screen);
	lv_obj_set_size(preview, lv_pct(100), height - bar_h);
	lv_obj_align(preview, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(preview, 0, 0);
	lv_obj_set_style_border_width(preview, 0, 0);
	lv_obj_set_style_radius(preview, 0, 0);
	lv_obj_set_style_pad_hor(preview, cfg->padding, 0);
	lv_obj_set_style_pad_ver(preview, 0, 0);
	lv_obj_set_scroll_dir(preview, LV_DIR_VER);
	lv_obj_set_event_bubble(preview, true);
	switcher_attach_back_gesture(preview);

	preview_text = lv_label_create(preview);
	lv_label_set_long_mode(preview_text, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(preview_text, lv_pct(100));
	lv_obj_set_style_text_align(preview_text, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_line_space(preview_text, 8, 0);
	lv_obj_add_style(preview_text, &theme_style_text, 0);
	lv_obj_set_style_text_font(preview_text, &font_ui_24, 0);
	lv_label_set_text(preview_text, "");

	apply_bar = lv_obj_create(lyricsearch_screen);
	lv_obj_remove_style_all(apply_bar);
	lv_obj_set_size(apply_bar, lv_pct(100), bar_h);
	lv_obj_align(apply_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_scrollable(apply_bar, false);

	lv_obj_t *apply = lv_btn_create(apply_bar);
	lv_obj_set_size(apply, APPLY_W, APPLY_H);
	lv_obj_center(apply);
	lv_obj_add_style(apply, &theme_style_card, 0);
	lv_obj_add_style(apply, &theme_style_accent_bg, 0);
	lv_obj_add_style(apply, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(apply, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_border_width(apply, 0, 0);
	lv_obj_set_style_shadow_width(apply, 0, 0);
	lv_obj_add_event_cb(apply, apply_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *apply_label = lv_label_create(apply);
	lv_label_set_text(apply_label, tr("lyrics_apply"));
	lv_obj_set_style_text_font(apply_label, &font_ui_24, 0);
	lv_obj_add_style(apply_label, &sx_style_accent, 0);
	lv_obj_set_style_bg_opa(apply_label, LV_OPA_TRANSP, 0);
	lv_obj_center(apply_label);

	show_list();

	switcher_set_back_guard(lyricsearch_screen, back_guard);
	switcher_set_back_guard_peek(lyricsearch_screen, back_peek);
	lv_obj_add_event_cb(lyricsearch_screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
}

void lyricsearch_init(gui_config_t *cfg) {
	(void)cfg;
	gui_page_defer(lyricsearch_screen, build_page);
}
