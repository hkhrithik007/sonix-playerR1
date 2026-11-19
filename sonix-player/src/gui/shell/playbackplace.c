#include "playbackplace.h"
#include "src/system/core/lang.h"
#include "src/system/library/library.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/provider.h"
#include <stdio.h>
#include <string.h>

// Seven bounded card-scoped entries. No worker, timer or idle filesystem work.
static const char *const sources[] = {"music", "yandex", "zvuk", "qobuz", "tidal", "subsonic", "podcast"};
static playbackplace_t places[7], pending;
static unsigned epoch, pending_revision;
static unsigned loaded, dirty;
static int pending_source = -1;
static int active_source = -1;
static unsigned active_queue;
static unsigned inspected_queue;
static bool inspected;
static unsigned place_revision;

static int source_index(const char *source) {
	for (int i = 0; source && i < 7; i++)
		if (!strcmp(source, sources[i]))
			return i;
	return -1;
}

static void sync_card(void) {
	unsigned now = library_database_epoch();
	if (now == epoch)
		return;
	epoch = now;
	loaded = dirty = 0;
	pending_source = -1;
	active_source = -1;
	inspected = false;
	memset(places, 0, sizeof(places));
	place_revision++;
}

static bool valid(const playbackplace_t *p) { return p->version == 1 && memchr(p->title, 0, sizeof(p->title)) && memchr(p->a, 0, sizeof(p->a)) && memchr(p->b, 0, sizeof(p->b)) && memchr(p->path, 0, sizeof(p->path)); }

void playbackplace_arm(const char *source, const playbackplace_t *place, const char *expected) {
	sync_card();
	int index = source_index(source);
	pending_source = -1;
	if (!epoch || index < 0 || !place || !expected || !expected[0] || strlen(expected) >= sizeof(pending.path))
		return;
	pending = *place;
	pending.version = 1;
	snprintf(pending.path, sizeof(pending.path), "%s", expected);
	if (!valid(&pending))
		return;
	pending_revision = device_state_metadata_revision();
	pending_source = index;
}

void playbackplace_heard(const char *path) {
	sync_card();
	if (pending_source >= 0 && path) {
		bool same = !strcmp(path, pending.path);
		const stream_provider_t *provider = stream_provider_for_path(path);
		if (!same && provider && !strcmp(provider->id, sources[pending_source]) && provider->owns_path(pending.path)) {
			// Cache extensions can change after the requested quality is resolved.
			const char *a = strrchr(path, '.'), *b = strrchr(pending.path, '.');
			same = a && b && a - path == b - pending.path && !strncmp(path, pending.path, (size_t)(a - path));
		}
		if (same) {
			place_revision++;
			snprintf(pending.path, sizeof(pending.path), "%s", path);
			places[pending_source] = pending;
			loaded |= 1u << pending_source;
			dirty |= 1u << pending_source;
			active_source = pending_source;
			active_queue = playlist_source_revision();
			pending_source = -1;
		} else if (device_state_metadata_revision() != pending_revision) {
			pending_source = -1;
		}
	}
	if (active_source >= 0 && active_queue != playlist_source_revision())
		active_source = -1;
	if (active_source < 0 && pending_source < 0 && path && path[0] && (!inspected || inspected_queue != playlist_source_revision())) {
		// A restored queue or automatic consecutive album has no new page tap.
		// Inspect its existing descriptor once, never scan its tracks on a poll.
		inspected = true;
		inspected_queue = playlist_source_revision();
		const stream_provider_t *provider = stream_provider_for_path(path);
		int source = provider ? source_index(provider->id) : 0;
		playbackplace_t saved;
		if (source >= 0 && playbackplace_get(sources[source], &saved) && !strcmp(saved.path, path)) {
			active_source = source;
			active_queue = inspected_queue;
		} else if (!provider) {
			library_index_spec_t spec;
			if (playlist_library_spec(&spec) && spec.valid) {
				playbackplace_t place = {.version = 1, .kind = spec.kind, .filter = spec.filter, .order = spec.order, .desc = spec.desc};
				snprintf(place.a, sizeof(place.a), "%s", spec.value);
				if (spec.value[0] && spec.filter == LIBRARY_FILTER_ALBUM)
					library_album_title(spec.value, place.title, sizeof(place.title));
				else if (spec.value[0])
					snprintf(place.title, sizeof(place.title), "%s", spec.value);
				else
					snprintf(place.title, sizeof(place.title), "%s", tr(spec.kind == LIBRARY_LIST_FAVOURITES ? "favourites" : spec.kind == LIBRARY_LIST_MOST_PLAYED ? "music_most_played" : "music_all_tracks"));
				snprintf(place.path, sizeof(place.path), "%s", path);
				places[0] = place;
				place_revision++;
				loaded |= 1u;
				dirty |= 1u;
				active_source = 0;
				active_queue = inspected_queue;
			}
		}
	}
	if (active_source >= 0 && path && strcmp(places[active_source].path, path)) {
		const stream_provider_t *provider = stream_provider_for_path(path);
		bool belongs = provider ? !strcmp(provider->id, sources[active_source]) : active_source == 0;
		if (belongs && strlen(path) < sizeof(places[active_source].path)) {
			snprintf(places[active_source].path, sizeof(places[active_source].path), "%s", path);
			dirty |= 1u << active_source;
		}
	}
	// One ordinary event flushes one dirty route. Never join another writer's
	// transaction; persistent failures wait for a new confirmed route instead.
	for (int i = 0; i < 7; i++) {
		if (!(dirty & (1u << i)))
			continue;
		int saved = library_place_store_at(epoch, sources[i], &places[i], sizeof(places[i]));
		if (saved != -1)
			dirty &= ~(1u << i);
		break;
	}
}

bool playbackplace_get(const char *source, playbackplace_t *out) {
	sync_card();
	int i = source_index(source);
	if (!epoch || i < 0 || !out)
		return false;
	if (!(loaded & (1u << i))) {
		loaded |= 1u << i;
		if (!library_place_read_at(epoch, source, &places[i], sizeof(places[i])) || !valid(&places[i]))
			memset(&places[i], 0, sizeof(places[i]));
	}
	if (!valid(&places[i]))
		return false;
	*out = places[i];
	return true;
}

void playbackplace_description(const char *source, char *out, size_t size) {
	playbackplace_t p;
	if (out && size && playbackplace_get(source, &p))
		snprintf(out, size, "%s", p.title);
}

unsigned playbackplace_revision(void) {
	sync_card();
	return place_revision;
}
