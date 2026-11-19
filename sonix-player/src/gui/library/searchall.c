#include "searchall.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/library/listacts.h"
#include "src/gui/library/medialist.h"
#include "src/gui/library/search.h"
#include "src/gui/library/searchsource.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/main_menu.h"
#include "src/gui/shell/playbackplace.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/sx_card.h"
#include "src/gui/shell/sx_frame.h"
#include "src/gui/shell/sx_palette.h"
#include "src/gui/shell/sx_row.h"
#include "src/gui/shell/sx_styles.h"
#include "src/gui/shell/sx_toast.h"
#include "src/gui/shell/sx_tokens.h"
#include "src/gui/shell/theme.h"
#include "src/gui/streaming/svclist.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/library/library.h"
#include "src/system/net/http.h"
#include "src/system/playback/device_state.h"
#include "src/system/streaming/streamerr.h"

// An overview, not a second full catalogue. More opens the native result list.
// Local results keep compact index handles; only three remote hits per category
// are kept. Each available service has one lazy worker, exiting when idle;
// a slow service never blocks another. Pending work coalesces to the last query.
#define LOCAL_PREVIEW 2
#define REMOTE_SOURCES SEARCH_SOURCE_MAX
typedef search_hit_t remote_hit_t;
typedef search_result_t response_t;
typedef struct {
	unsigned serial, epoch;
	char query[256];
} request_t;
static pthread_mutex_t worker_lock = PTHREAD_MUTEX_INITIALIZER;
static request_t pending[REMOTE_SOURCES];
static unsigned generation;
static unsigned failed_serial[REMOTE_SOURCES];
static bool has_pending[REMOTE_SOURCES], running[REMOTE_SOURCES];

static lv_obj_t *screen, *field, *list_box, *clear_button, *empty_note;
static keyboard_t *keyboard;
static svclist_t *list;
static gui_config_t *config;
static library_index_t *local[3];
static int local_count[3], status[REMOTE_SOURCES]; // 0 unavailable, 1 loading, 2 ready, 3 failed
static bool included[REMOTE_SOURCES];			   // Sources available at submit; logout keeps an explanatory row.
static response_t *remote[REMOTE_SOURCES], *deferred[REMOTE_SOURCES];
static unsigned query_epoch;
static char searched[256], recents[6][128];
static int recent_count;
static lv_obj_t *filter_bar, *filter_pills[SEARCH_KIND_COUNT + 1];
static int type_filter = -1; // -1 all; 0 tracks, 1 albums, 2 artists.
static bool editing;
static home_item_t active_scope = HOME_SEARCH;
static const library_list_t local_kind[] = {LIBRARY_LIST_TRACKS, LIBRARY_LIST_ALBUMS, LIBRARY_LIST_ARTISTS};
static const char *const category[] = {"search_tracks_2", "albums", "artists", "podcasts"};

static bool kind_shown(int kind) { return type_filter < 0 || type_filter == kind; }
static bool source_kind_shown(int source);

static void view_layout(void) {
	if (!config || !filter_bar)
		return;
	bool filters = !editing && searched[0];
	lv_obj_set_hidden(filter_bar, !filters);
	unsigned kinds = active_scope == HOME_SEARCH ? 7u : 0u;
	for (int n = 0; n < search_source_count(); n++) {
		const search_source_t *source = search_source_at(n);
		if ((active_scope == HOME_SEARCH || active_scope == source->home) && source->ready())
			kinds |= source->kinds;
	}
	if (type_filter >= 0 && !(kinds & (1u << type_filter)))
		type_filter = -1;
	for (int i = 0; i <= SEARCH_KIND_COUNT; i++) {
		lv_obj_set_hidden(filter_pills[i], i > 0 && !(kinds & (1u << (i - 1))));
		sx_pill_set_active(filter_pills[i], i - 1 == type_filter);
	}
	if (filters) {
		lv_obj_update_layout(filter_bar);
		lv_obj_scroll_to_view(filter_pills[type_filter + 1], LV_ANIM_OFF);
	}
	int top = settingsrow_content_top(config) + 72 + (filters ? SX_PILL_H + 12 : 0);
	lv_obj_set_y(list_box, top);
	lv_obj_set_height(list_box, config->screen_height - top - (editing ? 316 : 0));
}

static bool pressed(void) {
	for (lv_indev_t *in = lv_indev_get_next(NULL); in; in = lv_indev_get_next(in))
		if (lv_indev_get_state(in) == LV_INDEV_STATE_PRESSED)
			return true;
	return false;
}

static bool current_request(unsigned serial) {
	pthread_mutex_lock(&worker_lock);
	bool current = serial == generation;
	pthread_mutex_unlock(&worker_lock);
	return current;
}

static void cancel_request(void) {
	pthread_mutex_lock(&worker_lock);
	generation++;
	for (int i = 0; i < REMOTE_SOURCES; i++)
		has_pending[i] = false;
	for (int i = 0; i < REMOTE_SOURCES; i++)
		failed_serial[i] = 0;
	pthread_mutex_unlock(&worker_lock);
}

static void clear_results(void) {
	if (list)
		svclist_set_count(list, 0, false);
	for (int i = 0; i < 3; i++) {
		library_index_close(local[i]);
		local[i] = NULL;
		local_count[i] = 0;
	}
	for (int i = 0; i < REMOTE_SOURCES; i++) {
		search_result_free(remote[i]);
		search_result_free(deferred[i]);
		remote[i] = deferred[i] = NULL;
		status[i] = 0;
		included[i] = false;
	}
}

// Resolves one pooled row against a fixed snapshot. A source always ends with
// an ordinary More / Retry row, so an empty or failed source cannot trap users.
static int item_at(int index, int *source, int *kind, int *at) {
	if (index < 0)
		return 0;
	if (editing || !searched[0]) {
		*source = -1;
		*at = index;
		*kind = 0;
		return index < recent_count;
	}
	for (int k = 0; k < 3; k++) {
		if (!kind_shown(k))
			continue;
		int n = local_count[k] < LOCAL_PREVIEW ? local_count[k] : LOCAL_PREVIEW;
		int size = n + (local_count[k] > n);
		if (index < size) {
			*source = 0;
			*kind = k;
			*at = index;
			return index < n ? 1 : 2;
		}
		index -= size;
	}
	for (int s = 0; s < REMOTE_SOURCES; s++) {
		if (!included[s] || !source_kind_shown(s))
			continue;
		for (int i = 0; remote[s] && i < remote[s]->count; i++) {
			if (!kind_shown(remote[s]->hits[i].kind))
				continue;
			if (index-- == 0) {
				*source = s + 1;
				*kind = remote[s]->hits[i].kind;
				*at = i;
				return 1;
			}
		}
		if (index == 0) {
			*source = s + 1;
			*kind = type_filter < 0 ? 0 : type_filter;
			*at = 0;
			return 2;
		}
		index--;
	}
	return 0;
}

static int total_items(void) {
	if (editing || !searched[0])
		return recent_count;
	int n = 0;
	for (int k = 0; k < 3; k++)
		if (kind_shown(k))
			n += local_count[k] < LOCAL_PREVIEW ? local_count[k] : LOCAL_PREVIEW + 1;
	for (int s = 0; s < REMOTE_SOURCES; s++) {
		if (!included[s] || !source_kind_shown(s))
			continue;
		n++;
		if (remote[s])
			for (int i = 0; i < remote[s]->count; i++)
				n += kind_shown(remote[s]->hits[i].kind);
	}
	return n;
}

typedef struct {
	char name[256], path[512], artist[256];
} local_hit_t;
static bool read_local(const char *name, const char *path, const char *artist, void *user) {
	local_hit_t *hit = user;
	snprintf(hit->name, sizeof(hit->name), "%s", name ? name : "");
	snprintf(hit->path, sizeof(hit->path), "%s", path ? path : "");
	snprintf(hit->artist, sizeof(hit->artist), "%s", artist ? artist : "");
	return false;
}

static const char *source_name(int source) {
	const search_source_t *delegate = search_source_at(source - 1);
	return tr(delegate ? delegate->name : "home_search_card");
}
static bool source_ready(int source) {
	const search_source_t *delegate = search_source_at(source);
	return delegate && delegate->ready();
}
static bool source_kind_shown(int source) {
	const search_source_t *delegate = search_source_at(source);
	return delegate && (type_filter < 0 || (delegate->kinds & (1u << type_filter)));
}

static void bind_row(lv_obj_t *row, int index) {
	int source = 0, kind = 0, at = 0;
	int type = item_at(index, &source, &kind, &at);
	if (!type) {
		sx_row_set_title(row, "");
		sx_row_set_swipes(row, false, false);
		return;
	}
	char name[256] = "", subtitle[256] = "";
	if (source == -1)
		snprintf(name, sizeof(name), "%s", recents[at]);
	else if (type == 2) {
		snprintf(name, sizeof(name), "%s", tr(source == 0 ? "search_more_results" : status[source - 1] == 1 ? "loading" : status[source - 1] == 3 ? "search_source_retry" : status[source - 1] == 0 ? "search_source_unavailable" : "search_more_results"));
		snprintf(subtitle, sizeof(subtitle), "%s", source_name(source));
	} else if (!source) {
		local_hit_t hit = {0};
		library_index_window(local[kind], at, 1, read_local, &hit);
		library_album_title(hit.name, name, sizeof(name));
		snprintf(subtitle, sizeof(subtitle), "%s", hit.artist);
	} else {
		remote_hit_t *hit = &remote[source - 1]->hits[at];
		snprintf(name, sizeof(name), "%s", hit->name);
		snprintf(subtitle, sizeof(subtitle), "%s%s%s", tr(category[kind]), hit->artist[0] ? " · " : "", hit->artist);
	}
	sx_row_set_user(row, (void *)(intptr_t)index);
	sx_row_set_title(row, name);
	sx_row_set_subtitle(row, subtitle, NULL, NULL);
	sx_row_set_thumb(row, NULL, sx_placeholder_name(name));
	if (source == -1)
		sx_row_set_icon(row, &icon_radio_recent);
	else if (type == 2)
		sx_row_set_icon(row, &icon_search);
	if (type == 1 && kind == 2)
		sx_row_set_person(row, NULL, name);
	if (source >= 0 && type == 1)
		sx_row_set_content(row, sx_palette_hue_of_name(name), false, false, 0);
	sx_row_set_trail(row, type == 2 || kind != 0 ? SX_ROW_TRAIL_CHEVRON : SX_ROW_TRAIL_NONE);
	sx_row_set_swipes(row, type == 1 && source >= 0 && kind == 0, type == 1 && source >= 0 && kind == 0);
}

static bool heading(int index, char *out, size_t size) {
	int source, kind, at, before, bk, ba;
	if (!item_at(index, &source, &kind, &at))
		return false;
	bool start = index == 0 || !item_at(index - 1, &before, &bk, &ba) || source != before || (source == 0 && kind != bk);
	if (!start)
		return false;
	if (source == -1)
		snprintf(out, size, "%s", tr("search_recent_queries_heading"));
	else
		snprintf(out, size, "%s%s%s", source_name(source), source == 0 ? " · " : "", source == 0 ? tr(category[kind]) : "");
	return true;
}

static void rebuild(void) {
	svclist_anchor_t anchor = {0};
	svclist_capture(list, &anchor);
	svclist_set_count_at(list, total_items(), &anchor);
	if (empty_note)
		lv_obj_set_hidden(empty_note, editing || !searched[0] || total_items() != 0);
}

static void filter_clicked(lv_event_t *e) {
	int selected = (int)(intptr_t)lv_event_get_user_data(e) - 1;
	if (selected == type_filter || selected < -1 || selected >= SEARCH_KIND_COUNT || switcher_back_drag_active() || player_sheet_drag_active())
		return;
	type_filter = selected;
	view_layout();
	lv_obj_scroll_to_view(filter_pills[selected + 1], LV_ANIM_OFF);
	svclist_set_count(list, total_items(), false);
	if (empty_note)
		lv_obj_set_hidden(empty_note, total_items() != 0);
}

static void accept_response(void *user) {
	response_t *r = user;
	if (!current_request(r->serial) || r->epoch != query_epoch || r->epoch != library_database_epoch() || !included[r->source]) {
		search_result_free(r);
		return;
	}
	int s = r->source;
	// Never replace the item under a finger. A later existing UI poll delivers
	// it after release; there is no network or rendering timer for this page.
	if (pressed() || player_sheet_is_open()) {
		search_result_free(deferred[s]);
		deferred[s] = r;
		return;
	}
	if (!source_ready(s)) {
		search_result_free(r);
		search_result_free(remote[s]);
		remote[s] = NULL;
		status[s] = 0;
		rebuild();
		return;
	}
	if (r->failed && !r->count && remote[s] && remote[s]->count) {
		search_result_free(r);
		remote[s]->failed = true;
		status[s] = 3;
		rebuild();
		return;
	}
	search_result_free(remote[s]);
	remote[s] = r;
	status[s] = r->failed ? 3 : 2;
	rebuild();
}

void searchall_poll(void) {
	if (!screen || lv_screen_active() != screen || pressed() || player_sheet_is_open())
		return;
	if (query_epoch != library_database_epoch()) {
		cancel_request();
		clear_results();
		searched[0] = 0;
		editing = true;
		view_layout();
		query_epoch = library_database_epoch();
		recent_count = search_recent_queries(recents, 6);
		rebuild();
		return;
	}
	bool failed = false;
	pthread_mutex_lock(&worker_lock);
	for (int s = 0; s < REMOTE_SOURCES; s++)
		if (failed_serial[s] && failed_serial[s] == generation) {
			failed_serial[s] = 0;
			status[s] = 3;
			failed = true;
		}
	pthread_mutex_unlock(&worker_lock);
	for (int s = 0; s < REMOTE_SOURCES; s++) {
		if (status[s] && !source_ready(s)) {
			search_result_free(remote[s]);
			remote[s] = NULL;
			search_result_free(deferred[s]);
			deferred[s] = NULL;
			status[s] = 0;
			failed = true;
		}
	}
	if (failed)
		rebuild();
	for (int s = 0; s < REMOTE_SOURCES; s++)
		if (deferred[s]) {
			response_t *r = deferred[s];
			deferred[s] = NULL;
			accept_response(r);
		}
}

static void report_delivery_failure(const request_t *job, int source) {
	pthread_mutex_lock(&worker_lock);
	if (job->serial == generation)
		failed_serial[source] = job->serial;
	pthread_mutex_unlock(&worker_lock);
}

static void fetch_source(const request_t *job, int source) {
	response_t *r = calloc(1, sizeof(*r));
	if (!r) {
		report_delivery_failure(job, source);
		return;
	}
	r->serial = job->serial;
	r->epoch = job->epoch;
	r->source = source;
	const search_source_t *delegate = search_source_at(source);
	if (delegate && current_request(job->serial))
		delegate->fetch(job->query, r, current_request);
	if (!gui_post(accept_response, r)) {
		search_result_free(r);
		report_delivery_failure(job, source);
	}
}

static void *worker(void *user) {
	int source = (int)(intptr_t)user;
	char name[16];
	const search_source_t *delegate = search_source_at(source);
	snprintf(name, sizeof(name), "search-%s", delegate ? delegate->name : "source");
	thread_be_low_priority(name);
	http_set_thread_timeout_cap(STREAM_UI_TIMEOUT_SECS);
	http_set_thread_body_cap(256 * 1024);
	for (;;) {
		pthread_mutex_lock(&worker_lock);
		if (!has_pending[source]) {
			running[source] = false;
			pthread_mutex_unlock(&worker_lock);
			return NULL;
		}
		request_t job = pending[source];
		has_pending[source] = false;
		pthread_mutex_unlock(&worker_lock);
		if (current_request(job.serial))
			fetch_source(&job, source);
	}
}

// Called with worker_lock held. One running request and one replaceable pending
// request per service; a retry never restarts another service or the card query.
static void queue_source(int source, const request_t *job) {
	pending[source] = *job;
	has_pending[source] = true;
	failed_serial[source] = 0;
	if (!has_pending[source] || running[source])
		return;
	pthread_t thread;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, 256 * 1024);
	running[source] = pthread_create(&thread, &attr, worker, (void *)(intptr_t)source) == 0;
	pthread_attr_destroy(&attr);
	if (!running[source]) {
		has_pending[source] = false;
		status[source] = 3;
	}
}

static void retry_source(int source) {
	if (source < 0 || source >= REMOTE_SOURCES || !searched[0] || editing)
		return;
	if (!source_ready(source)) {
		search_result_free(remote[source]);
		remote[source] = NULL;
		search_result_free(deferred[source]);
		deferred[source] = NULL;
		status[source] = 0;
		rebuild();
		return;
	}
	request_t job = {.epoch = query_epoch};
	snprintf(job.query, sizeof(job.query), "%s", searched);
	status[source] = 1;
	pthread_mutex_lock(&worker_lock);
	job.serial = generation;
	queue_source(source, &job);
	pthread_mutex_unlock(&worker_lock);
	rebuild();
}

static void submit(void) {
	char query[256];
	search_query_copy(query, sizeof(query), lv_textarea_get_text(field));
	cancel_request();
	clear_results();
	snprintf(searched, sizeof(searched), "%s", query);
	query_epoch = library_database_epoch();
	if (!searched[0]) {
		editing = true;
		view_layout();
		recent_count = search_recent_queries(recents, 6);
		rebuild();
		return;
	}
	search_note_query(searched);
	editing = false;
	for (int k = 0; active_scope == HOME_SEARCH && k < 3; k++) {
		local[k] = library_index_open(local_kind[k], LIBRARY_FILTER_SEARCH, searched, LIBRARY_ORDER_DEFAULT, false);
		local_count[k] = library_index_count(local[k]);
	}
	request_t job = {.epoch = query_epoch};
	snprintf(job.query, sizeof(job.query), "%s", searched);
	for (int i = 0; i < search_source_count(); i++) {
		const search_source_t *delegate = search_source_at(i);
		included[i] = (active_scope == HOME_SEARCH || active_scope == delegate->home) && delegate->ready();
		status[i] = included[i] ? 1 : 0;
	}
	pthread_mutex_lock(&worker_lock);
	job.serial = generation;
	for (int i = 0; i < REMOTE_SOURCES; i++)
		if (included[i])
			queue_source(i, &job);
	pthread_mutex_unlock(&worker_lock);
	keyboard_set_visible(keyboard, false);
	view_layout();
	svclist_set_count(list, total_items(), false);
	if (empty_note)
		lv_obj_set_hidden(empty_note, total_items() != 0);
}

static bool remote_path(int source, int at, char *out, size_t size) {
	response_t *r = remote[source - 1];
	const search_source_t *delegate = search_source_at(source - 1);
	return r && at >= 0 && at < r->count && r->hits[at].kind == SEARCH_TRACK && delegate && delegate->path && delegate->path(&r->hits[at], out, size);
}

static void tapped(lv_obj_t *row, void *user) {
	(void)row;
	int source = 0, kind = 0, at = 0;
	int type = item_at((int)(intptr_t)user, &source, &kind, &at);
	if (!type)
		return;
	if (source == -1) {
		lv_textarea_set_text(field, recents[at]);
		submit();
		return;
	}
	if (!type || query_epoch != library_database_epoch())
		return;
	if (source > 0 && type == 1 && !source_ready(source - 1)) {
		sx_toast(tr("search_source_unavailable"));
		return;
	}
	if (type == 2) {
		if (!source)
			search_open_query_kind(searched, type_filter);
		else if (status[source - 1] == 3)
			retry_source(source - 1);
		else if (source_ready(source - 1)) {
			const search_source_t *delegate = search_source_at(source - 1);
			delegate->more(searched, type_filter);
		}
		return;
	}
	if (!source) {
		local_hit_t hit = {0};
		library_index_window(local[kind], at, 1, read_local, &hit);
		if (!kind) {
			if (device_state_play_index(library_index_clone(local[0]), at)) {
				playbackplace_t place = {.kind = LIBRARY_LIST_TRACKS, .filter = LIBRARY_FILTER_SEARCH};
				snprintf(place.title, sizeof(place.title), "%s", searched);
				snprintf(place.a, sizeof(place.a), "%s", searched);
				playbackplace_arm("music", &place, hit.path);
				listacts_played();
			}
		} else if (kind == 1)
			medialist_open(hit.name, LIBRARY_LIST_TRACKS, LIBRARY_FILTER_ALBUM, hit.name);
		else
			medialist_open_artist(hit.name);
		return;
	}
	const search_source_t *delegate = search_source_at(source - 1);
	if (delegate && remote[source - 1])
		delegate->open(remote[source - 1], at, searched);
}

static void row_action(lv_obj_t *row, void *user, int action) {
	(void)row;
	int source, kind, at;
	if (item_at((int)(intptr_t)user, &source, &kind, &at) != 1 || source < 0 || kind || query_epoch != library_database_epoch())
		return;
	if (source > 0 && !source_ready(source - 1)) {
		sx_toast(tr("search_source_unavailable"));
		return;
	}
	char path[512], title[256], artist[256];
	if (!source) {
		local_hit_t hit = {0};
		library_index_window(local[0], at, 1, read_local, &hit);
		snprintf(path, sizeof(path), "%s", hit.path);
		snprintf(title, sizeof(title), "%s", hit.name);
		snprintf(artist, sizeof(artist), "%s", hit.artist);
	} else {
		if (!remote_path(source, at, path, sizeof(path)))
			return;
		remote_hit_t *hit = &remote[source - 1]->hits[at];
		snprintf(title, sizeof(title), "%s", hit->name);
		snprintf(artist, sizeof(artist), "%s", hit->artist);
	}
	if (action == 0) {
		const char *paths[] = {path};
		listacts_queue(paths, 1, PLAYLIST_QUEUE_NEXT, title);
	} else if (action == 1)
		listacts_star(path, title, artist);
	else
		listacts_track_sheet(path, title, artist, source_name(source), NULL, NULL, NULL, 0);
}
static void next(lv_obj_t *r, void *u) { row_action(r, u, 0); }
static void star(lv_obj_t *r, void *u) { row_action(r, u, 1); }
static char forget_query[128];
static void forget_recent(void *unused) {
	(void)unused;
	search_forget_query(forget_query);
	recent_count = search_recent_queries(recents, 6);
	rebuild();
}
static void hold(lv_obj_t *r, void *u) {
	int index = (int)(intptr_t)u;
	if ((editing || !searched[0]) && index >= 0 && index < recent_count) {
		memcpy(forget_query, recents[index], sizeof(forget_query));
		forget_query[sizeof(forget_query) - 1] = 0;
		sx_sheet_item_t action = {.label = tr("delete"), .icon = &icon_clear, .action = forget_recent};
		sx_sheet_menu(forget_query, NULL, &action, 1);
		return;
	}
	row_action(r, u, 2);
}

static void accepted(lv_event_t *e) {
	(void)e;
	submit();
}
static void edit(lv_event_t *e) {
	(void)e;
	editing = true;
	recent_count = search_recent_queries(recents, 6);
	keyboard_set_visible(keyboard, true);
	view_layout();
	svclist_set_count(list, total_items(), false);
	if (empty_note)
		lv_obj_set_hidden(empty_note, true);
}
static void changed(lv_event_t *e) {
	(void)e;
	if (clear_button)
		lv_obj_set_hidden(clear_button, !lv_textarea_get_text(field)[0]);
	if (!searched[0] || !strcmp(lv_textarea_get_text(field), searched))
		return;
	cancel_request();
	clear_results();
	searched[0] = 0;
	recent_count = search_recent_queries(recents, 6);
	editing = true;
	view_layout();
	rebuild();
}
static void loaded(lv_event_t *e) {
	(void)e;
	if (clear_button)
		lv_obj_set_hidden(clear_button, !lv_textarea_get_text(field)[0]);
	if (query_epoch != library_database_epoch()) {
		cancel_request();
		clear_results();
		searched[0] = 0;
	}
	if (!searched[0]) {
		recent_count = search_recent_queries(recents, 6);
		rebuild();
		edit(NULL);
	}
}
static void unloaded(lv_event_t *e) {
	(void)e;
	cancel_request();
	for (int s = 0; s < REMOTE_SOURCES; s++) {
		search_result_free(deferred[s]);
		deferred[s] = NULL;
		if (status[s] == 1)
			status[s] = 3;
	}
}

static void clear_clicked(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(field, "");
	edit(NULL);
}

void searchall_open_source(gui_config_t *cfg, home_item_t scope, bool replace_source) {
	if (active_scope != scope) {
		cancel_request();
		clear_results();
		searched[0] = 0;
		editing = true;
		type_filter = -1;
		active_scope = scope;
	}
	if (!screen) {
		config = cfg;
		screen = lv_obj_create(NULL);
		lv_obj_add_style(screen, &theme_style_screen, 0);
		main_menu_search_header(screen, cfg, HOME_SEARCH);
		field = lv_textarea_create(screen);
		lv_textarea_set_one_line(field, true);
		lv_textarea_set_max_length(field, 255);
		lv_textarea_set_placeholder_text(field, tr("artist_album_or_track"));
		lv_obj_set_size(field, cfg->screen_width - SX_BODY_PAD_L - SX_BODY_PAD_R, 60);
		lv_obj_set_pos(field, SX_BODY_PAD_L, settingsrow_content_top(cfg));
		lv_obj_add_style(field, &sx_style_surface, 0);
		lv_obj_add_style(field, &sx_style_text, 0);
		lv_obj_add_style(field, &sx_style_search_placeholder, LV_PART_TEXTAREA_PLACEHOLDER);
		lv_obj_set_style_text_font(field, &font_ui_24, 0);
		lv_obj_set_style_radius(field, SX_CARD_R, 0);
		lv_obj_set_style_border_width(field, 0, 0);
		lv_obj_set_style_shadow_width(field, 0, 0);
		lv_obj_set_style_pad_hor(field, SX_CARD_PAD_H, 0);
		lv_obj_set_style_pad_right(field, 64, 0);
		keyboard_style_caret(field);
		main_menu_search_bind(screen, field);
		clear_button = lv_button_create(screen);
		lv_obj_remove_style_all(clear_button);
		lv_obj_set_size(clear_button, 56, 60);
		lv_obj_set_pos(clear_button, cfg->screen_width - SX_BODY_PAD_R - 56, settingsrow_content_top(cfg));
		lv_obj_add_style(clear_button, &sx_style_press, LV_STATE_PRESSED);
		lv_obj_t *clear_icon = lv_image_create(clear_button);
		lv_image_set_src(clear_icon, &icon_clear);
		lv_obj_add_style(clear_icon, &sx_style_icon2, 0);
		lv_obj_center(clear_icon);
		lv_obj_set_hidden(clear_button, true);
		lv_obj_add_event_cb(clear_button, clear_clicked, LV_EVENT_CLICKED, NULL);
		list_box = sx_box(screen);
		lv_obj_set_pos(list_box, SX_BODY_PAD_L, settingsrow_content_top(cfg) + 72);
		lv_obj_set_width(list_box, cfg->screen_width - SX_BODY_PAD_L - SX_BODY_PAD_R);
		lv_obj_set_height(list_box, cfg->screen_height - settingsrow_content_top(cfg) - 72 - 316);
		const sx_row_handlers_t handlers = {.on_tap = tapped, .on_long_press = hold, .on_swipe_next = next, .on_star = star};
		const svclist_cbs_t cbs = {.bind = bind_row, .heading = heading};
		list = svclist_create(list_box, &handlers, &cbs, 12, false);
		empty_note = sx_label(list_box, &font_ui_22, &sx_style_text2);
		lv_label_set_text(empty_note, tr("lists_nothing_found"));
		lv_obj_set_size(empty_note, lv_pct(100), LV_SIZE_CONTENT);
		lv_obj_set_y(empty_note, 16);
		lv_obj_set_hidden(empty_note, true);
		filter_bar = sx_box(screen);
		lv_obj_set_size(filter_bar, cfg->screen_width, SX_PILL_H);
		lv_obj_set_pos(filter_bar, 0, settingsrow_content_top(cfg) + 72);
		lv_obj_set_style_pad_hor(filter_bar, SX_BODY_PAD_L, 0);
		lv_obj_set_style_pad_column(filter_bar, SX_PILL_GAP, 0);
		lv_obj_set_flex_flow(filter_bar, LV_FLEX_FLOW_ROW);
		lv_obj_set_scrollable(filter_bar, true);
		lv_obj_set_scroll_dir(filter_bar, LV_DIR_HOR);
		lv_obj_set_scrollbar_mode(filter_bar, LV_SCROLLBAR_MODE_OFF);
		lv_obj_set_scroll_chain_hor(filter_bar, false);
		filter_pills[0] = sx_pill(filter_bar, tr("lists_filter_all"), 0, filter_clicked);
		for (int i = 0; i < SEARCH_KIND_COUNT; i++)
			filter_pills[i + 1] = sx_pill(filter_bar, tr(category[i]), i + 1, filter_clicked);
		lv_obj_set_hidden(filter_bar, true);
		keyboard = keyboard_create(screen, cfg->screen_width, 316, field, &icon_search, NULL, accepted, NULL);
		lv_obj_add_event_cb(field, edit, LV_EVENT_CLICKED, NULL);
		lv_obj_add_event_cb(field, changed, LV_EVENT_VALUE_CHANGED, NULL);
		lv_obj_add_event_cb(screen, loaded, LV_EVENT_SCREEN_LOADED, NULL);
		lv_obj_add_event_cb(screen, unloaded, LV_EVENT_SCREEN_UNLOADED, NULL);
		switcher_attach_back_gesture(screen);
	}
	main_menu_search_scope_set(screen, scope);
	lv_textarea_set_placeholder_text(field, tr(scope == HOME_PODCASTS ? "podcast_name" : "artist_album_or_track"));
	if (lv_screen_active() == screen)
		loaded(NULL);
	else if (replace_source)
		switch_screen_no_history(screen);
	else
		switch_screen(screen);
}

void searchall_open(gui_config_t *cfg, bool replace_source) { searchall_open_source(cfg, HOME_SEARCH, replace_source); }
