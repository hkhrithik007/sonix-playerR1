#include "spotifypage.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/sx_card.h"
#include "src/gui/shell/sx_row.h"
#include "src/gui/shell/sx_sheet.h"
#include "src/gui/shell/sx_styles.h"
#include "src/gui/shell/sx_toast.h"
#include "src/gui/shell/sx_tokens.h"
#include "src/gui/shell/theme.h"
#include "src/gui/streaming/streamerror.h"
#include "src/gui/streaming/svclist.h"
#include "src/gui/streaming/svcui.h"
#include "src/system/core/json.h"
#include "src/system/core/lang.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/playback/sleeptimer.h"
#include "src/system/streaming/cardimport.h"
#include "src/system/streaming/provider.h"
#include "src/system/streaming/spotify.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VIEW_MAX 4
#define ITEMS_MAX 256
typedef struct {
	char op[16], id[23], query[512], title[256];
	char cursor[4097];
	spotify_item_t *items;
	int count, offset, scroll;
	bool more, loaded;
	unsigned revision;
} view_t;
typedef struct {
	view_t request;
	int depth;
	unsigned sleep_serial;
	unsigned ticket;
	char *response;
} job_t;
static gui_config_t *cfg;
static lv_obj_t *home, *list_screen, *search_screen, *title, *spinner, *error_panel, *field, *empty;
static svclist_t *list;
static view_t views[VIEW_MAX];
static int depth = -1;
static unsigned revision;
static atomic_bool busy;
static atomic_uint active_request;
static lv_obj_t *navigation, *sign_in_card, *continue_card, *account_note, *search_button;
static void load_page(void);
static void open_view(const char *op, const char *id, const char *query, const char *name, bool nested);

static void paint(void) {
	if (depth < 0)
		return;
	view_t *view = &views[depth];
	lv_label_set_text(title, view->title);
	svclist_set_count(list, view->count + (view->more && view->count < ITEMS_MAX ? 1 : 0), false);
	lv_obj_update_layout(svclist_obj(list));
	lv_obj_scroll_to_y(svclist_obj(list), view->scroll, LV_ANIM_OFF);
	lv_obj_set_hidden(empty, view->count != 0 || !view->loaded);
}

static void done(void *user) {
	job_t *job = user;
	if (job->ticket != active_request)
		goto finish;
	busy = false;
	lv_obj_set_hidden(spinner, true);
	view_t *view = &views[job->depth];
	if (view->revision != job->request.revision || job->sleep_serial != sleeptimer_expiration_serial())
		goto finish;
	json_doc_t doc;
	bool parsed = job->response && json_parse(job->response, &doc);
	bool ok = parsed && json_obj_bool(&doc, json_root(&doc), "ok", false);
	if (ok) {
		int array = json_get(&doc, json_root(&doc), "items");
		int n = json_len(&doc, array);
		if (n > 20 || view->count + n > ITEMS_MAX)
			ok = false;
		spotify_item_t *items = ok ? realloc(view->items, (size_t)(view->count + n + 1) * sizeof(*items)) : NULL;
		if (!items)
			ok = false;
		if (ok) {
			view->items = items;
			for (int i = 0; i < n; i++) {
				int obj = json_at(&doc, array, i);
				if (obj < 0)
					continue;
				json_tok_t *token = &doc.toks[obj];
				int bytes = token->end - token->start;
				char text[4096];
				if (bytes <= 0 || bytes >= (int)sizeof(text))
					continue;
				memcpy(text, doc.text + token->start, (size_t)bytes);
				text[bytes] = 0;
				spotify_item_t item;
				if (!spotify_parse_item(text, &item))
					continue;
				view->items[view->count++] = item;
				spotify_remember(&item);
			}
			view->offset = (int)json_obj_long(&doc, json_root(&doc), "next_offset", view->offset);
			int token = json_get(&doc, json_root(&doc), "cursor");
			view->cursor[0] = 0;
			if (token >= 0 && doc.toks[token].type == JSON_OBJECT) {
				int bytes = doc.toks[token].end - doc.toks[token].start;
				if (bytes > 0 && bytes < (int)sizeof(view->cursor)) {
					memcpy(view->cursor, doc.text + doc.toks[token].start, (size_t)bytes);
					view->cursor[bytes] = 0;
				}
			}
			view->more = json_obj_bool(&doc, json_root(&doc), "more", false);
			view->loaded = true;
		}
	}
	if (depth == job->depth) {
		if (ok) {
			streamerror_hide(error_panel);
			paint();
		} else {
			char code[64] = "";
			if (parsed)
				json_obj_str(&doc, json_root(&doc), "error", code, sizeof(code));
			if (view->count)
				sx_toast(tr(spotify_error_key(code)));
			else
				streamerror_show(error_panel, STREAM_ERR_OTHER, tr(spotify_error_key(code)), load_page, NULL);
		}
	}
	if (parsed)
		json_free(&doc);
finish:
	free(job->response);
	free(job);
}

static void *worker(void *user) {
	job_t *job = user;
	job->response = spotify_browse(job->request.op, job->request.id, job->request.query, job->request.offset, job->request.cursor, job->ticket);
	if (!gui_post(done, job)) {
		if (job->ticket == active_request)
			busy = false;
		free(job->response);
		free(job);
	}
	return NULL;
}

static void load_page(void) {
	if (depth < 0 || busy)
		return;
	job_t *job = calloc(1, sizeof(*job));
	if (!job)
		return;
	job->depth = depth;
	job->request = views[depth];
	job->request.items = NULL;
	job->sleep_serial = sleeptimer_expiration_serial();
	active_request = job->ticket = spotify_browse_begin();
	spotify_resume();
	views[depth].scroll = lv_obj_get_scroll_y(svclist_obj(list));
	busy = true;
	lv_obj_set_hidden(spinner, false);
	streamerror_hide(error_panel);
	pthread_t thread;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, 256 * 1024);
	int result = pthread_create(&thread, &attr, worker, job);
	pthread_attr_destroy(&attr);
	if (result) {
		busy = false;
		lv_obj_set_hidden(spinner, true);
		free(job);
		sx_toast(tr("spotify_unavailable"));
	}
}

static bool has_parent(void) { return depth > 0; }
static bool back(void) {
	if (depth <= 0)
		return false;
	views[depth].scroll = lv_obj_get_scroll_y(svclist_obj(list));
	views[depth].revision = ++revision;
	if (busy) {
		active_request = spotify_browse_begin();
		busy = false;
		lv_obj_set_hidden(spinner, true);
	}
	depth--;
	streamerror_hide(error_panel);
	paint();
	return true;
}

static void play_row(int index, bool enqueue, playlist_queue_where_t where) {
	view_t *view = &views[depth];
	char (*paths)[512] = calloc((size_t)view->count, 512);
	const char **pointers = calloc((size_t)view->count, sizeof(*pointers));
	if (!paths || !pointers) {
		free(paths);
		free(pointers);
		return;
	}
	int n = 0, selected = -1;
	for (int i = 0; i < view->count; i++) {
		if (strcmp(view->items[i].kind, "track") || (enqueue && i != index))
			continue;
		if (!spotify_track_path(view->items[i].id, paths[n], sizeof(paths[n])))
			continue;
		spotify_remember(&view->items[i]);
		pointers[n] = paths[n];
		if (i == index)
			selected = n;
		n++;
	}
	if (!n || selected < 0)
		sx_toast(tr("spotify_need_card"));
	else if (enqueue)
		svcui_queue(pointers, n, where, view->items[index].title);
	else {
		device_state_play_list(pointers, n, selected);
		player_sheet_open(true);
	}
	free(paths);
	free(pointers);
}

static void tap(lv_obj_t *row, void *user) {
	(void)row;
	int index = (int)(intptr_t)user;
	view_t *view = &views[depth];
	if (index == view->count) {
		load_page();
		return;
	}
	if (index < 0 || index >= view->count)
		return;
	spotify_item_t item = view->items[index];
	if (!strcmp(item.kind, "track"))
		play_row(index, false, PLAYLIST_QUEUE_NEXT);
	else
		open_view(item.kind, item.id, NULL, item.title, true);
}
static void next(lv_obj_t *row, void *user) {
	(void)row;
	play_row((int)(intptr_t)user, true, PLAYLIST_QUEUE_NEXT);
}
static void queue_end(void *user) { play_row((int)(intptr_t)user, true, PLAYLIST_QUEUE_END); }
static void queue_next(void *user) { play_row((int)(intptr_t)user, true, PLAYLIST_QUEUE_NEXT); }
static void sheet(lv_obj_t *row, void *user) {
	(void)row;
	int index = (int)(intptr_t)user;
	if (index < 0 || index >= views[depth].count || strcmp(views[depth].items[index].kind, "track"))
		return;
	sx_sheet_item_t items[] = {{.label = tr("queue_play_next"), .icon = &icon_list_start, .action = queue_next, .user = user}, {.label = tr("queue_add_to_end"), .icon = &icon_list_plus_small, .action = queue_end, .user = user}};
	sx_sheet_menu(views[depth].items[index].title, tr("spotify_read_only"), items, 2);
}

static void bind(lv_obj_t *row, int index) {
	view_t *view = &views[depth];
	if (index == view->count) {
		sx_row_set_title(row, tr("svc_browse_next"));
		sx_row_set_subtitle(row, "", NULL, NULL);
		sx_row_set_glyph(row, &icon_chevron_right, svcui_tone("Spotify"));
		sx_row_set_trail(row, SX_ROW_TRAIL_CHEVRON);
		sx_row_set_swipes(row, false, false);
		sx_row_set_playing(row, false);
		return;
	}
	spotify_item_t *item = &view->items[index];
	bool track = !strcmp(item->kind, "track");
	char duration[24] = "";
	if (item->duration_ms)
		snprintf(duration, sizeof(duration), "%u:%02u", item->duration_ms / 60000, item->duration_ms / 1000 % 60);
	sx_row_set_title(row, item->title);
	sx_row_set_subtitle(row, item->artist, NULL, duration);
	sx_row_set_letter(row, "", svcui_tone(item->album_id[0] ? item->album_id : item->id), false);
	sx_row_set_trail(row, track ? SX_ROW_TRAIL_NONE : SX_ROW_TRAIL_CHEVRON);
	sx_row_set_swipes(row, track, false);
	char path[512], current[512];
	int playing = playlist_current_index();
	bool selected = track && spotify_track_path(item->id, path, sizeof(path)) && playlist_path_at(playing, current, sizeof(current)) && !strcmp(path, current);
	sx_row_set_playing(row, selected);
}

static void open_view(const char *op, const char *id, const char *query, const char *name, bool nested) {
	if (busy) {
		active_request = spotify_browse_begin();
		busy = false;
	}
	if (depth >= 0 && lv_screen_active() == list_screen)
		views[depth].scroll = lv_obj_get_scroll_y(svclist_obj(list));
	if (!nested && depth >= 0 && !strcmp(views[0].op, op) && !strcmp(views[0].id, id ? id : "") && !strcmp(views[0].query, query ? query : "")) {
		depth = 0;
		paint();
		switch_screen(list_screen);
		return;
	}
	if (nested && depth + 1 >= VIEW_MAX)
		return;
	int at = nested ? depth + 1 : 0;
	for (int i = at; i < VIEW_MAX; i++) {
		free(views[i].items);
		memset(&views[i], 0, sizeof(views[i]));
	}
	depth = at;
	view_t *view = &views[depth];
	view->revision = ++revision;
	snprintf(view->op, sizeof(view->op), "%s", op);
	snprintf(view->id, sizeof(view->id), "%s", id ? id : "");
	snprintf(view->query, sizeof(view->query), "%s", query ? query : "");
	snprintf(view->title, sizeof(view->title), "%s", name);
	streamerror_hide(error_panel);
	paint();
	if (lv_screen_active() != list_screen)
		switch_screen(list_screen);
	load_page();
}

static void open_liked(lv_event_t *e) {
	(void)e;
	open_view("liked", NULL, NULL, tr("spotify_liked"), false);
}
static void open_playlists(lv_event_t *e) {
	(void)e;
	open_view("playlists", NULL, NULL, tr("spotify_playlists"), false);
}
static void resume(lv_event_t *e) {
	(void)e;
	if (depth >= 0) {
		paint();
		switch_screen(list_screen);
	}
}
static void accept_search(lv_event_t *e) {
	(void)e;
	const char *text = lv_textarea_get_text(field);
	if (*text)
		open_view("search", NULL, text, text, false);
}
static void open_search(lv_event_t *e) {
	(void)e;
	switch_screen(search_screen);
}
static void refresh(lv_event_t *e) {
	(void)e;
	load_page();
}
static void screen_loaded(lv_event_t *e) {
	(void)e;
	if (list)
		svclist_rebind(list);
}
static void screen_left(lv_event_t *e) {
	(void)e;
	if (depth >= 0)
		views[depth].scroll = lv_obj_get_scroll_y(svclist_obj(list));
}
static void check_import(lv_event_t *e) {
	(void)e;
	cardimport_check("spotify");
	sx_toast(tr("spotify_sign_in"));
}
static void home_loaded(lv_event_t *e) {
	(void)e;
	bool signed_in = spotify_logged_in();
	lv_obj_set_hidden(navigation, !signed_in);
	lv_obj_set_hidden(sign_in_card, signed_in);
	lv_obj_set_hidden(search_button, !signed_in);
	lv_obj_set_hidden(account_note, !signed_in);
	lv_obj_set_hidden(continue_card, depth < 0);
}

static void build(void) {
	if (home)
		return;
	home = lv_obj_create(NULL);
	lv_obj_add_style(home, &theme_style_screen, 0);
	settingsrow_title(home, cfg, "spotify");
	search_button = svcui_header_button(home, cfg, 0, &icon_search, open_search, NULL);
	lv_obj_t *column = svcui_column(home, cfg);
	lv_obj_t *nav = sx_card_nav_group(column);
	navigation = nav;
	svcui_nav_card(nav, tr("spotify_liked"), NULL, open_liked, NULL);
	svcui_nav_card(nav, tr("spotify_playlists"), NULL, open_playlists, NULL);
	continue_card = svcui_nav_card(nav, tr("spotify_continue"), NULL, resume, NULL);
	lv_obj_t *card = sx_card_create(column);
	sign_in_card = card;
	sx_card_title(card, "Spotify Premium");
	sx_card_subtitle(card, tr("spotify_sign_in"), false);
	sx_card_action(card, tr("spotify_check_import"), check_import, NULL);
	account_note = svcui_note(column, tr("spotify_read_only"));
	lv_obj_add_event_cb(home, home_loaded, LV_EVENT_SCREEN_LOADED, NULL);
	home_loaded(NULL);
	switcher_attach_back_gesture(home);
	player_sheet_attach_drag(column, true);
	list_screen = lv_obj_create(NULL);
	lv_obj_add_style(list_screen, &theme_style_screen, 0);
	title = settingsrow_title(list_screen, cfg, "spotify");
	svcui_header_button(list_screen, cfg, 0, &icon_refresh, refresh, NULL);
	sx_row_handlers_t handlers = {.on_tap = tap, .on_long_press = sheet, .on_swipe_next = next};
	svclist_cbs_t cbs = {.bind = bind};
	list = svclist_create(list_screen, &handlers, &cbs, 12, false);
	lv_obj_t *obj = svclist_obj(list);
	int top = settingsrow_content_top(cfg);
	lv_obj_set_pos(obj, 0, top);
	lv_obj_set_size(obj, cfg->screen_width, cfg->screen_height - top);
	spinner = svcui_spinner(list_screen);
	error_panel = streamerror_create(list_screen);
	lv_obj_set_width(error_panel, cfg->screen_width - 2 * SX_BODY_PAD_L);
	lv_obj_align(error_panel, LV_ALIGN_TOP_MID, 0, top + 8);
	streamerror_set_service(error_panel, "Spotify");
	empty = svcui_note(list_screen, tr("spotify_empty"));
	lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, top + 32);
	lv_obj_set_hidden(empty, true);
	switcher_attach_back_gesture(list_screen);
	switcher_set_back_guard(list_screen, back);
	switcher_set_back_guard_peek(list_screen, has_parent);
	lv_obj_add_event_cb(list_screen, screen_loaded, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(list_screen, screen_left, LV_EVENT_SCREEN_UNLOADED, NULL);
	player_sheet_attach_drag(obj, true);
	search_screen = lv_obj_create(NULL);
	lv_obj_add_style(search_screen, &theme_style_screen, 0);
	settingsrow_title(search_screen, cfg, "search");
	field = settingsrow_name_field(search_screen, cfg, 160);
	keyboard_create(search_screen, cfg->screen_width, 340, field, &icon_search, NULL, accept_search, NULL);
	switcher_attach_back_gesture(search_screen);
}

static void *open_album(const char *id, const char *name) {
	build();
	open_view("album", id, NULL, name, false);
	return list_screen;
}
void spotifypage_init(gui_config_t *config) {
	cfg = config;
	const stream_provider_gui_t gui = {.open_album = open_album};
	stream_provider_attach_gui("spotify", &gui);
}
void spotifypage_open(void) {
	build();
	cardimport_check("spotify");
	switch_screen(home);
}
