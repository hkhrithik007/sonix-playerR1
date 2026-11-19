#include "historypage.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "src/gui/library/audiobooks.h"
#include "src/gui/nowplaying/npicons.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/nowplaying/trackmenu.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/sx_frame.h"
#include "src/gui/shell/sx_palette.h"
#include "src/gui/shell/sx_row.h"
#include "src/gui/shell/sx_sheet.h"
#include "src/gui/shell/sx_toast.h"
#include "src/gui/shell/sx_tokens.h"
#include "src/gui/shell/theme.h"
#include "src/gui/streaming/svcui.h"
#include "src/system/core/lang.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/library/library.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playhistory.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/podcaststate.h"
#include "src/system/streaming/provider.h"

#define POOL 16
static lv_obj_t *screen, *viewport, *body, *empty;
static lv_obj_t *rows[POOL];
static library_history_item_t *items;
static int count, pitch;
static int bound[POOL];
static unsigned epoch, revision;
static long long db_revision, current, anchor, menu_id;
static int saved_y;
static bool fresh;

static bool history_entry(long long id, char *path, size_t ps, char *title, size_t ts, char *artist, size_t as) { return epoch == library_database_epoch() && playhistory_entry(id, path, ps, title, ts, artist, as) && epoch == library_database_epoch(); }

static long long row_id(void *user) {
	int at = (int)(intptr_t)user;
	return at >= 0 && at < count ? items[count - 1 - at].id : 0;
}

static void play_id(void *unused) {
	(void)unused;
	char path[512], book[512];
	if (!history_entry(menu_id, path, sizeof(path), NULL, 0, NULL, 0))
		return;
	device_state_t state;
	device_state_get(&state);
	if (!state.live && !strcmp(path, state.current_file) && state.status != AUDIO_STATUS_STOPPED) {
		if (state.status == AUDIO_STATUS_PAUSED)
			player_key_play_pause();
	} else if (audiobookdb_book_for_file(path, book, sizeof(book))) {
		audiobooks_resume_book(book);
	} else {
		bool episode = podcastcache_is_episode(path);
		if (!stream_provider_for_path(path) && !episode && access(path, R_OK)) {
			sx_toast(tr("history_file_unavailable"));
			return;
		}
		const char *paths[] = {path};
		if (episode)
			device_state_play_list_ordered_at(paths, 1, 0, podcaststate_resume_point(path));
		else
			device_state_play_list(paths, 1, 0);
		player_refresh_now_playing();
	}
	player_sheet_open(true);
}

static void queue_id(void *where) {
	char path[512];
	if (history_entry(menu_id, path, sizeof(path), NULL, 0, NULL, 0)) {
		const char *paths[] = {path};
		trackmenu_queue_paths(paths, 1, (playlist_queue_where_t)(intptr_t)where);
	}
}

static void tapped(lv_obj_t *row, void *user) {
	(void)row;
	menu_id = row_id(user);
	play_id(NULL);
}

static void held(lv_obj_t *row, void *user) {
	(void)row;
	menu_id = row_id(user);
	static char title[256], subtitle[384];
	char path[512], artist[256];
	if (!history_entry(menu_id, path, sizeof(path), title, sizeof(title), artist, sizeof(artist)))
		return;
	svcui_track_caption(path, artist, subtitle, sizeof(subtitle));
	sx_sheet_item_t actions[] = {
		{.label = tr("medialist_play"), .icon = &npicon_play, .action = play_id},
		{.label = tr("queue_play_next"), .icon = &npicon_play_next, .action = queue_id, .user = (void *)(intptr_t)PLAYLIST_QUEUE_NEXT},
		{.label = tr("queue_add_to_end"), .icon = &npicon_add_playlist, .action = queue_id, .user = (void *)(intptr_t)PLAYLIST_QUEUE_END},
	};
	sx_sheet_menu(title, subtitle, actions, 3);
}

static void queue_next(lv_obj_t *row, void *user) {
	(void)row;
	menu_id = row_id(user);
	queue_id((void *)(intptr_t)PLAYLIST_QUEUE_NEXT);
}

static void window_update(lv_event_t *e) {
	(void)e;
	int first = lv_obj_get_scroll_y(viewport) / pitch;
	if (first < 0) {
		first = 0;
	}
	for (int i = 0; i < POOL; i++) {
		int at = first + i;
		lv_obj_set_hidden(rows[i], at >= count);
		if (at >= count || bound[i] == at) {
			continue;
		}
		bound[i] = at;
		char path[512] = "", title[256], artist[256] = "";
		library_history_item_t *item = &items[count - 1 - at];
		if (!history_entry(item->id, path, sizeof(path), title, sizeof(title), artist, sizeof(artist))) {
			title[0] = '\0';
		}
		char when[48] = "";
		if (item->id == current) {
			snprintf(when, sizeof(when), "%s", tr("history_now"));
		} else if (item->played_at > 0) {
			time_t stamp = (time_t)item->played_at;
			struct tm tm;
			localtime_r(&stamp, &tm);
			strftime(when, sizeof(when), "%d.%m %H:%M", &tm);
		}
		sx_row_set_user(rows[i], (void *)(intptr_t)at);
		sx_row_set_title(rows[i], title);
		char book[512], detail[320];
		const char *kind = podcastcache_is_episode(path) ? tr("podcasts") : !stream_provider_for_path(path) && audiobookdb_book_for_file(path, book, sizeof(book)) ? tr("audiobooks") : "";
		snprintf(detail, sizeof(detail), "%s%s%s", kind, kind[0] && artist[0] ? " · " : "", artist);
		char subtitle[384];
		svcui_track_caption(path, detail, subtitle, sizeof(subtitle));
		sx_row_set_subtitle(rows[i], subtitle, NULL, when);
		sx_row_set_thumb(rows[i], NULL, sx_placeholder_name(title));
		sx_row_set_content(rows[i], sx_palette_hue_of_name(title), false, false, 0);
		sx_row_set_playing(rows[i], item->id == current);
		lv_obj_set_pos(rows[i], 0, at * pitch);
	}
}

static void unloaded(lv_event_t *e) {
	(void)e;
	saved_y = lv_obj_get_scroll_y(viewport);
	int at = saved_y / pitch;
	anchor = at >= 0 && at < count ? items[count - 1 - at].id : 0;
	free(items);
	items = NULL;
	count = 0;
}

static void reload(bool top) {
	if (items && !top) {
		saved_y = lv_obj_get_scroll_y(viewport);
		int at = saved_y / pitch;
		anchor = at >= 0 && at < count ? items[count - 1 - at].id : 0;
	}
	free(items);
	items = NULL;
	epoch = library_database_epoch();
	count = playhistory_items(&items);
	revision = playhistory_revision();
	db_revision = library_history_revision();
	current = playhistory_current_id();
	pitch = sx_row_height(true) + SX_ROW_GAP;
	int y = top ? 0 : saved_y;
	for (int at = 0; !top && at < count; at++) {
		if (items[count - 1 - at].id == anchor) {
			y = at * pitch + saved_y % pitch;
			break;
		}
	}
	for (int i = 0; i < POOL; i++) {
		bound[i] = -1;
		sx_row_set_compact(rows[i], true);
	}
	lv_obj_set_height(body, count ? count * pitch : 1);
	lv_obj_set_hidden(empty, count != 0);
	lv_obj_update_layout(viewport);
	lv_obj_scroll_to_y(viewport, y, LV_ANIM_OFF);
	window_update(NULL);
}

static void loaded(lv_event_t *e) {
	(void)e;
	reload(fresh);
	fresh = false;
	if (playhistory_unsaved())
		sx_toast(tr("history_unsaved"));
}

void historypage_poll(void) {
	if (!screen || lv_screen_active() != screen || !viewport || player_sheet_is_open())
		return;
	// Retain the snapshot until the finger and its sheet let go of the row.
	for (lv_indev_t *in = lv_indev_get_next(NULL); in; in = lv_indev_get_next(in))
		if (lv_indev_get_state(in) == LV_INDEV_STATE_PRESSED)
			return;
	if (sx_sheet_is_open() || switcher_nav_anim_running())
		return;
	if (epoch != library_database_epoch() || revision != playhistory_revision() || db_revision != library_history_revision() || current != playhistory_current_id())
		reload(false);
}

void historypage_open(void *unused) {
	(void)unused;
	gui_page_ensure(screen);
	fresh = true;
	if (lv_screen_active() == screen) {
		reload(true);
		fresh = false;
	} else
		switch_screen(screen);
}

static void build_page(gui_config_t *cfg) {
	lv_obj_add_style(screen, &theme_style_screen, 0);
	settingsrow_title(screen, cfg, "queue_history");
	viewport = lv_obj_create(screen);
	lv_obj_remove_style_all(viewport);
	lv_obj_set_size(viewport, cfg->screen_width - SX_BODY_PAD_L - SX_BODY_PAD_R, cfg->screen_height - settingsrow_content_top(cfg));
	lv_obj_set_pos(viewport, SX_BODY_PAD_L, settingsrow_content_top(cfg));
	lv_obj_set_scroll_dir(viewport, LV_DIR_VER);
	sx_frame_reserve_bottom(viewport);
	body = lv_obj_create(viewport);
	lv_obj_remove_style_all(body);
	lv_obj_set_width(body, lv_pct(100));
	lv_obj_set_scrollable(body, false);
	const sx_row_handlers_t handlers = {.on_tap = tapped, .on_long_press = held, .on_swipe_next = queue_next};
	for (int i = 0; i < POOL; i++) {
		rows[i] = sx_row_create(body, &handlers, true);
		lv_obj_set_width(rows[i], lv_pct(100));
		lv_obj_set_hidden(rows[i], true);
	}
	empty = lv_label_create(viewport);
	lv_label_set_text(empty, tr("queue_history_empty"));
	lv_obj_add_style(empty, &theme_style_text_dim, 0);
	lv_obj_center(empty);
	lv_obj_add_event_cb(viewport, window_update, LV_EVENT_SCROLL, NULL);
	lv_obj_add_event_cb(screen, unloaded, LV_EVENT_SCREEN_UNLOADED, NULL);
	lv_obj_add_event_cb(screen, loaded, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(screen);
}

void historypage_init(gui_config_t *cfg) {
	(void)cfg;
	screen = lv_obj_create(NULL);
	gui_page_defer(screen, build_page);
}
