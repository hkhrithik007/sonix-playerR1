#include "src/gui/library/listacts.h"
#include "src/gui/library/searchsource.h"
#include "src/gui/streaming/podcastcard.h"
#include "src/gui/streaming/podcastpage.h"
#include "src/gui/streaming/qobuzpage.h"
#include "src/gui/streaming/subsonicpage.h"
#include "src/gui/streaming/tidalpage.h"
#include "src/gui/streaming/yandexpage.h"
#include "src/system/streaming/podcast.h"
#include "src/system/streaming/qobuz.h"
#include "src/system/streaming/subsonic.h"
#include "src/system/streaming/tidal.h"
#include "src/system/streaming/yandex.h"
#include "src/system/streaming/yandexplay.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef SONIX_ZVUK
#include "src/gui/streaming/zvukpage.h"
#include "src/system/streaming/zvuk.h"
#include "src/system/streaming/zvukplay.h"
#endif

// These adapters retain each client's native data and hand actions back to its
// own page/engine. They do not change account, queue or cache ownership.
static bool yandex_ready(void) { return yandex_configured() && yandex_logged_in(); }
static bool tidal_ready(void) { return tidal_configured() && tidal_logged_in(); }
static bool qobuz_ready(void) { return qobuz_configured() && qobuz_logged_in(); }

static void yandex_fetch(const char *query, search_result_t *out, bool (*current)(unsigned)) {
	if (!current(out->serial))
		return;
	yandex_track_t *tracks = calloc(SEARCH_PREVIEW, sizeof(*tracks));
	yandex_album_t *albums = calloc(SEARCH_PREVIEW, sizeof(*albums));
	yandex_artist_t *artists = calloc(SEARCH_PREVIEW, sizeof(*artists));
	int nt = SEARCH_PREVIEW, na = SEARCH_PREVIEW, nr = SEARCH_PREVIEW;
	out->failed = !tracks || !albums || !artists || !yandex_search(query, artists, &nr, albums, &na, tracks, &nt);
	if (!out->failed) {
		for (int i = 0; i < nt && i < SEARCH_PREVIEW; i++)
			search_result_add(out, SEARCH_TRACK, tracks[i].id, tracks[i].title, tracks[i].artist, &tracks[i], sizeof(*tracks));
		for (int i = 0; i < na && i < SEARCH_PREVIEW; i++)
			search_result_add(out, SEARCH_ALBUM, albums[i].id, albums[i].title, albums[i].artist, NULL, 0);
		for (int i = 0; i < nr && i < SEARCH_PREVIEW; i++)
			search_result_add(out, SEARCH_ARTIST, artists[i].id, artists[i].name, "", NULL, 0);
	}
	free(tracks);
	free(albums);
	free(artists);
}
static bool yandex_path(const search_hit_t *hit, char *out, size_t size) {
	char **paths = NULL;
	int n = hit->data ? yandexplay_paths_for(hit->data, 1, &paths) : 0;
	if (n == 1)
		snprintf(out, size, "%s", paths[0]);
	free(paths);
	return n == 1;
}
static void yandex_open(const search_result_t *r, int at, const char *query) {
	const search_hit_t *hit = &r->hits[at];
	if (hit->kind == SEARCH_ALBUM) {
		yandexpage_open_album(hit->id, hit->name);
		return;
	}
	if (hit->kind == SEARCH_ARTIST) {
		yandexpage_open_artist(hit->id, hit->name);
		return;
	}
	yandexlist_t *list = yandexlist_new();
	int start = 0;
	if (!list)
		return;
	for (int i = 0; i < r->count; i++)
		if (r->hits[i].kind == SEARCH_TRACK && r->hits[i].data) {
			if (i < at)
				start++;
			yandexlist_add_track(list, r->hits[i].data);
		}
	char path[512];
	if (yandex_path(hit, path, sizeof(path)) && yandexplay_list(list, start, false))
		yandexpage_note_search_play(query, path);
	yandexlist_free(list);
}
#ifdef SONIX_ZVUK
static void zvuk_fetch(const char *query, search_result_t *out, bool (*current)(unsigned)) {
	for (int k = 0; k < 3 && current(out->serial); k++) {
		size_t size = k == 0 ? sizeof(zvuk_track_t) : k == 1 ? sizeof(zvuk_album_t) : sizeof(zvuk_artist_t);
		void *data = calloc(SEARCH_PREVIEW, size);
		int n = data ? zvuk_search(query, (zvuk_search_kind_t)k, data, SEARCH_PREVIEW) : -1;
		if (n < 0)
			out->failed = true;
		for (int i = 0; i < n && i < SEARCH_PREVIEW; i++) {
			zvuk_track_t *t = data;
			zvuk_album_t *a = data;
			zvuk_artist_t *artist = data;
			search_result_add(out, k, k == 0 ? t[i].id : k == 1 ? a[i].id : artist[i].id, k == 0 ? t[i].title : k == 1 ? a[i].title : artist[i].name, k == 0 ? t[i].artist : k == 1 ? a[i].artist : "", k == 0 ? &t[i] : NULL, k == 0 ? sizeof(*t) : 0);
		}
		free(data);
	}
}
static bool zvuk_path(const search_hit_t *hit, char *out, size_t size) {
	char **paths = NULL;
	int n = hit->data ? zvukplay_paths_for(hit->data, 1, &paths) : 0;
	if (n == 1)
		snprintf(out, size, "%s", paths[0]);
	free(paths);
	return n == 1;
}
static void zvuk_open(const search_result_t *r, int at, const char *query) {
	const search_hit_t *hit = &r->hits[at];
	if (hit->kind == SEARCH_ALBUM) {
		zvukpage_open_album(hit->id, hit->name);
		return;
	}
	if (hit->kind == SEARCH_ARTIST) {
		zvukpage_open_artist(hit->id, hit->name);
		return;
	}
	zvuklist_t *list = zvuklist_new();
	int start = 0;
	if (!list)
		return;
	for (int i = 0; i < r->count; i++)
		if (r->hits[i].kind == SEARCH_TRACK && r->hits[i].data) {
			if (i < at)
				start++;
			zvuklist_add_track(list, r->hits[i].data);
		}
	char path[512];
	if (zvuk_path(hit, path, sizeof(path)) && zvukplay_list(list, start))
		zvukpage_note_search_play(query, path);
	zvuklist_free(list);
}
#endif

static void tidal_fetch(const char *query, search_result_t *out, bool (*current)(unsigned)) {
	if (current(out->serial)) {
		tidal_track_t *data = calloc(SEARCH_PREVIEW, sizeof(*data));
		int n = data ? tidal_search_tracks(query, 0, data, SEARCH_PREVIEW) : -1;
		if (n < 0)
			out->failed = true;
		for (int i = 0; i < n && i < SEARCH_PREVIEW; i++) {
			char id[40];
			snprintf(id, sizeof(id), "%ld", data[i].id);
			search_result_add(out, 0, id, data[i].title, data[i].artist, &data[i], sizeof(*data));
		}
		free(data);
	}
	if (current(out->serial)) {
		tidal_album_t *data = calloc(SEARCH_PREVIEW, sizeof(*data));
		int n = data ? tidal_search_albums(query, 0, data, SEARCH_PREVIEW) : -1;
		if (n < 0)
			out->failed = true;
		for (int i = 0; i < n && i < SEARCH_PREVIEW; i++) {
			search_result_add(out, 1, data[i].id_text, data[i].title, data[i].artist, NULL, 0);
		}
		free(data);
	}
	if (current(out->serial)) {
		tidal_artist_t *data = calloc(SEARCH_PREVIEW, sizeof(*data));
		int n = data ? tidal_search_artists(query, 0, data, SEARCH_PREVIEW) : -1;
		if (n < 0)
			out->failed = true;
		for (int i = 0; i < n && i < SEARCH_PREVIEW; i++) {
			char id[40];
			snprintf(id, sizeof(id), "%ld", data[i].id);
			search_result_add(out, 2, id, data[i].name, "", NULL, 0);
		}
		free(data);
	}
}

static void qobuz_fetch(const char *query, search_result_t *out, bool (*current)(unsigned)) {
	if (current(out->serial)) {
		qobuz_track_t *data = calloc(SEARCH_PREVIEW, sizeof(*data));
		int n = data ? qobuz_search_tracks(query, 0, data, SEARCH_PREVIEW) : -1;
		if (n < 0)
			out->failed = true;
		for (int i = 0; i < n && i < SEARCH_PREVIEW; i++) {
			char id[40];
			snprintf(id, sizeof(id), "%ld", data[i].id);
			search_result_add(out, 0, id, data[i].title, data[i].artist, &data[i], sizeof(*data));
		}
		free(data);
	}
	if (current(out->serial)) {
		qobuz_album_t *data = calloc(SEARCH_PREVIEW, sizeof(*data));
		int n = data ? qobuz_search_albums(query, 0, data, SEARCH_PREVIEW) : -1;
		if (n < 0)
			out->failed = true;
		for (int i = 0; i < n && i < SEARCH_PREVIEW; i++) {
			search_result_add(out, 1, data[i].id_text, data[i].title, data[i].artist, NULL, 0);
		}
		free(data);
	}
	if (current(out->serial)) {
		qobuz_artist_t *data = calloc(SEARCH_PREVIEW, sizeof(*data));
		int n = data ? qobuz_search_artists(query, 0, data, SEARCH_PREVIEW) : -1;
		if (n < 0)
			out->failed = true;
		for (int i = 0; i < n && i < SEARCH_PREVIEW; i++) {
			char id[40];
			snprintf(id, sizeof(id), "%ld", data[i].id);
			search_result_add(out, 2, id, data[i].name, "", NULL, 0);
		}
		free(data);
	}
}

static void subsonic_fetch(const char *query, search_result_t *out, bool (*current)(unsigned)) {
	if (current(out->serial)) {
		subsonic_track_t *data = calloc(SEARCH_PREVIEW, sizeof(*data));
		int n = data ? subsonic_search_tracks(query, 0, data, SEARCH_PREVIEW) : -1;
		if (n < 0)
			out->failed = true;
		for (int i = 0; i < n && i < SEARCH_PREVIEW; i++) {
			search_result_add(out, 0, data[i].id, data[i].title, data[i].artist, &data[i], sizeof(*data));
		}
		free(data);
	}
	if (current(out->serial)) {
		subsonic_album_t *data = calloc(SEARCH_PREVIEW, sizeof(*data));
		int n = data ? subsonic_search_albums(query, 0, data, SEARCH_PREVIEW) : -1;
		if (n < 0)
			out->failed = true;
		for (int i = 0; i < n && i < SEARCH_PREVIEW; i++) {
			search_result_add(out, 1, data[i].id, data[i].title, data[i].artist, NULL, 0);
		}
		free(data);
	}
	if (current(out->serial)) {
		subsonic_artist_t *data = calloc(SEARCH_PREVIEW, sizeof(*data));
		int n = data ? subsonic_search_artists(query, 0, data, SEARCH_PREVIEW) : -1;
		if (n < 0)
			out->failed = true;
		for (int i = 0; i < n && i < SEARCH_PREVIEW; i++) {
			search_result_add(out, 2, data[i].id, data[i].name, "", NULL, 0);
		}
		free(data);
	}
}

static void podcast_fetch(const char *query, search_result_t *out, bool (*current)(unsigned)) {
	if (!current(out->serial))
		return;
	podcast_feed_t *data = calloc(SEARCH_PREVIEW, sizeof(*data));
	int n = data ? podcast_search(query, data, SEARCH_PREVIEW) : -1;
	if (n < 0)
		out->failed = true;
	for (int i = 0; i < n && i < SEARCH_PREVIEW; i++) {
		char id[40];
		snprintf(id, sizeof(id), "%lld", data[i].id);
		search_result_add(out, SEARCH_PODCAST, id, data[i].title, data[i].author, &data[i], sizeof(*data));
	}
	free(data);
}
static void podcast_open(const search_result_t *r, int at, const char *query) {
	(void)query;
	if (r->hits[at].data)
		podcastcard_open_feed(r->hits[at].data);
}

static const search_source_t sources[] = {
	{HOME_YANDEX, "yandex", 7, yandex_ready, yandex_fetch, yandexpage_search_results_kind, yandex_open, yandex_path},
#ifdef SONIX_ZVUK
	{HOME_ZVUK, "zvuk", 7, zvuk_logged_in, zvuk_fetch, zvukpage_search_results_kind, zvuk_open, zvuk_path},
#endif
	{HOME_TIDAL, "tidal", 7, tidal_ready, tidal_fetch, tidalpage_search_results_kind, tidalpage_search_open_hit, tidalpage_search_path},
	{HOME_QOBUZ, "qobuz", 7, qobuz_ready, qobuz_fetch, qobuzpage_search_results_kind, qobuzpage_search_open_hit, qobuzpage_search_path},
	{HOME_SUBSONIC, "subsonic", 7, subsonic_logged_in, subsonic_fetch, subsonicpage_search_results_kind, subsonicpage_search_open_hit, subsonicpage_search_path},
	{HOME_PODCASTS, "podcasts", 1u << SEARCH_PODCAST, podcast_configured, podcast_fetch, podcastpage_search_results_kind, podcast_open, NULL},
};
_Static_assert(sizeof(sources) / sizeof(sources[0]) <= SEARCH_SOURCE_MAX, "search source capacity");
int search_source_count(void) { return sizeof(sources) / sizeof(sources[0]); }
const search_source_t *search_source_at(int i) { return i >= 0 && i < search_source_count() ? &sources[i] : NULL; }
const search_source_t *search_source_home(home_item_t home) {
	for (int i = 0; i < search_source_count(); i++)
		if (sources[i].home == home)
			return &sources[i];
	return NULL;
}
