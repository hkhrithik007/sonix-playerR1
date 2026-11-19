#include "rowart.h"
#include "src/gui/shell/artview.h"
#include "src/gui/shell/sx_row.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/shell/sx_palette.h"
#include "src/gui/shell/sx_tokens.h"
#include "src/system/library/covertone.h"
#include "src/system/library/library.h"

void rowart_bind_id(rowart_t *art, int64_t album_id, const char *title) {
	if (art->album_id == album_id && album_id != 0 && (art->tone_known || art->has_picture)) {
		return;
	}
	bool same = art->album_id == album_id;
	art->album_id = album_id;
	if (!same) {
		art->has_picture = false;
		art->asked = false;
		art->settled = false;
		art->revision = 0;
	}
	art->identity_hue = sx_palette_hue_of_name(title);
	art->tone = sx_placeholder_name(title);
	art->tone_known = true;
	if (album_id == 0) {
		art->settled = true;
	}
}

void rowart_bind(rowart_t *art, const char *album_value) {
	char title[256] = "";
	if (album_value && album_value[0]) {
		library_album_title(album_value, title, sizeof(title));
	}
	rowart_bind_id(art, album_value && album_value[0] ? covertone_album_id(album_value) : 0, title);
}

lv_color_t rowart_tone(const rowart_t *art) { return art->tone_known ? sx_placeholder_tone(art->identity_hue) : art->tone; }
void rowart_apply(rowart_t *art, lv_obj_t *row) {
	const lv_image_dsc_t *image = rowart_picture(art);
	sx_row_set_thumb(row, image, rowart_tone(art));
	sx_row_set_content(row, art->identity_hue, false, image != NULL, art->display_generation);
}

const lv_image_dsc_t *rowart_picture(const rowart_t *art) { return art->has_picture ? &art->dsc : NULL; }

bool rowart_wanted(const rowart_t *art) {
	if (art->album_id == 0 || art->has_picture) {
		return false;
	}
	return !art->settled || art->revision != covertone_revision();
}

bool rowart_load(rowart_t *art) {
	if (!rowart_wanted(art)) {
		return false;
	}
	art->revision = covertone_revision();
	bool changed = false;
	if (!art->buf) {
		art->buf = malloc(COVERTONE_THUMB_BYTES(COVERTONE_THUMB_SMALL));
		if (!art->buf) {
			art->settled = true;
			return changed;
		}
	}
	if (covertone_thumb(art->album_id, COVERTONE_THUMB_SMALL, &art->dsc, art->buf,
						COVERTONE_THUMB_BYTES(COVERTONE_THUMB_SMALL))) {
		art->display_generation = cover_new_display_generation();
		art->has_picture = true;
		art->settled = true;
		return true;
	}
	// Not made yet, or no picture at all: asked for once, and tried again
	// when covertone has news (the revision moves).
	if (!art->asked) {
		covertone_request(art->album_id);
		art->asked = true;
	}
	art->settled = true;
	return changed;
}

void rowart_free(rowart_t *art) {
	sx_artview_forget_pixels(art->buf, art->display_generation);
	free(art->buf);
	memset(art, 0, sizeof(*art));
}

// ---------------------------------------------------------------------------
// The track -> album cache
// ---------------------------------------------------------------------------

#define TRACK_CACHE 64

static struct {
	uint64_t key;
	unsigned age;
	bool found;
	char album[256];
} track_cache[TRACK_CACHE];
static unsigned track_clock;
static unsigned track_cache_revision;

static uint64_t path_key(const char *path) {
	uint64_t h = 1469598103934665603ULL; // FNV-1a
	for (const unsigned char *s = (const unsigned char *)path; *s; s++) {
		h ^= *s;
		h *= 1099511628211ULL;
	}
	return h ? h : 1;
}

bool rowart_album_of_track(const char *path, char *out, size_t size) {
	if (size > 0) {
		out[0] = '\0';
	}
	if (!path || !path[0]) {
		return false;
	}
	// A rescan can file a track under another album: what was remembered
	// before it is forgotten.
	unsigned rev = library_revision(LIBRARY_LIST_TRACKS);
	if (rev != track_cache_revision) {
		memset(track_cache, 0, sizeof(track_cache));
		track_cache_revision = rev;
	}
	uint64_t key = path_key(path);
	int oldest = 0;
	for (int i = 0; i < TRACK_CACHE; i++) {
		if (track_cache[i].key == key) {
			track_cache[i].age = ++track_clock;
			snprintf(out, size, "%s", track_cache[i].album);
			return track_cache[i].found;
		}
		if (track_cache[i].age < track_cache[oldest].age) {
			oldest = i;
		}
	}
	char album[256] = "";
	bool found = library_track_album_value(path, album, sizeof(album));
	track_cache[oldest].key = key;
	track_cache[oldest].age = ++track_clock;
	track_cache[oldest].found = found;
	snprintf(track_cache[oldest].album, sizeof(track_cache[oldest].album), "%s", found ? album : "");
	snprintf(out, size, "%s", found ? album : "");
	return found;
}

bool rowart_list_busy(lv_obj_t *list) {
	if (!list) {
		return false;
	}
	for (lv_indev_t *indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev)) {
		if (lv_indev_get_scroll_obj(indev) != NULL) {
			return true;
		}
	}
	return lv_obj_is_scrolling(list);
}
