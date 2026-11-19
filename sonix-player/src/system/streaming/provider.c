#include "provider.h"
#include "src/system/library/library.h"

#include <stdio.h>
#include <string.h>

// Filled at start-up, read-only afterwards (see provider.h): no lock needed,
// which matters because the power loop asks it several times a second.
static stream_provider_t *providers[STREAM_PROVIDER_MAX];
static int provider_count;

void stream_provider_register(stream_provider_t *provider) {
	if (!provider || !provider->id) {
		return;
	}
	for (int i = 0; i < provider_count; i++) {
		if (providers[i] == provider) {
			return; // init called twice: harmless
		}
	}
	if (provider_count >= STREAM_PROVIDER_MAX) {
		fprintf(stderr, "provider: no room for %s (raise STREAM_PROVIDER_MAX)\n", provider->id);
		return;
	}
	providers[provider_count++] = provider;
}

void stream_provider_attach_gui(const char *id, const stream_provider_gui_t *gui) {
	if (!id || !gui) {
		return;
	}
	for (int i = 0; i < provider_count; i++) {
		if (strcmp(providers[i]->id, id) == 0) {
			providers[i]->gui = *gui;
			return;
		}
	}
}

int stream_provider_count(void) { return provider_count; }

const stream_provider_t *stream_provider_at(int index) { return index >= 0 && index < provider_count ? providers[index] : NULL; }

const stream_provider_t *stream_provider_for_path(const char *path) {
	if (!path || !path[0]) {
		return NULL;
	}
	for (int i = 0; i < provider_count; i++) {
		if (providers[i]->owns_path && providers[i]->owns_path(path)) {
			return providers[i];
		}
	}
	return NULL;
}

const stream_provider_t *stream_provider_for_track(const char *path) {
	const stream_provider_t *p = stream_provider_for_path(path);
	return p && p->is_track && p->is_track(path) ? p : NULL;
}

bool stream_provider_owns(const char *path) { return stream_provider_for_path(path) != NULL; }

bool track_favourite_get(const char *path) {
	const stream_provider_t *p = stream_provider_for_path(path);
	if (p)
		return !p->read_only && p->is_track && p->is_track(path) && p->fav_get && p->fav_get(path);
	return path && path[0] && library_fav_contains(path);
}

bool track_favourite_set(const char *path, const char *title, const char *artist, bool want, stream_provider_done_cb done, void *user) {
	if (!path || !path[0])
		return false;
	const stream_provider_t *p = stream_provider_for_path(path);
	if (p) {
		if (p->read_only || !p->is_track || !p->is_track(path) || !p->fav_toggle)
			return false;
		p->fav_toggle(path, want, done, user);
		return true;
	}
	if (library_fav_contains(path) != want)
		library_fav_toggle(path, title ? title : "", artist ? artist : "");
	return library_fav_contains(path) == want;
}

bool stream_provider_fill_metadata(const char *path, song_metadata_t *out) {
	const stream_provider_t *p = stream_provider_for_track(path);
	song_metadata_t extra = {0};
	if (!out || !p || !p->peek_metadata || !p->peek_metadata(path, &extra)) {
		return false;
	}
	bool changed = false;
#define FILL_DISPLAY(field)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            \
	if (!out->field[0] && extra.field[0]) {                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            \
		snprintf(out->field, sizeof(out->field), "%s", extra.field);                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   \
		changed = true;                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                \
	}
	FILL_DISPLAY(title)
	FILL_DISPLAY(artist)
	FILL_DISPLAY(album)
#undef FILL_DISPLAY
	if (changed) {
		out->has_tags = true;
	}
	return changed;
}

bool stream_provider_playback_downloading(const stream_provider_t *provider) {
	if (!provider)
		return false;
	bool (*busy)(void) = provider->playback_downloading ? provider->playback_downloading : provider->downloading;
	return busy && busy();
}

bool stream_provider_any_downloading(void) {
	for (int i = 0; i < provider_count; i++) {
		if (providers[i]->downloading && providers[i]->downloading()) {
			return true;
		}
	}
	return false;
}

bool stream_provider_any_network_wanted(void) {
	for (int i = 0; i < provider_count; i++) {
		if ((providers[i]->downloading && providers[i]->downloading()) || (providers[i]->network_wanted && providers[i]->network_wanted())) {
			return true;
		}
	}
	return false;
}

void stream_provider_set_root_all(const char *sd_root) {
	for (int i = 0; i < provider_count; i++) {
		if (providers[i]->set_root) {
			providers[i]->set_root(sd_root);
		}
	}
}

void stream_provider_clear_on_exit_all(void) {
	for (int i = 0; i < provider_count; i++) {
		if (providers[i]->clear_on_exit) {
			providers[i]->clear_on_exit();
		}
	}
}

void stream_provider_wipe_all(void) {
	for (int i = 0; i < provider_count; i++) {
		void (*wipe)(void) = providers[i]->wipe ? providers[i]->wipe : providers[i]->clear_on_exit;
		if (wipe) {
			wipe();
		}
	}
}
