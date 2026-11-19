#include "streamrowart.h"
#include "src/gui/shell/artview.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/src/misc/cache/instance/lv_image_cache.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/shell/sx_palette.h"
#include "src/gui/shell/sx_row.h"
#include "src/system/core/config.h"
#include "src/system/core/utils.h"
#include "src/system/net/http.h"

#define ROW_SLOTS 48
#define ART_SIDE 56
#define URL_SIZE 768
#define BODY_LIMIT (256 * 1024)
#define FETCH_SECONDS 5

typedef struct {
	lv_obj_t *row, *owner;
	int index;
	unsigned generation;
	bool visible, settled;
	lv_color_t tone;
	float identity_hue;
	bool identity_known;
	char url[URL_SIZE];
	cover_image_t image;
} art_slot_t;

// Slots and LVGL objects belong to the GUI. The worker sees only the copied
// URL and its one result: no object pointers, unbounded queue or disk cache.
// Default-off must not reserve a pool of URL/pixel descriptors either.
// Once enabled, slots stay stable for the row DELETE callbacks' lifetime.
static art_slot_t *slots;
static lv_timer_t *timer;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool worker_started, busy, requested, done;
static char job_url[URL_SIZE];
static unsigned job_generations[ROW_SLOTS];
static cover_image_t result;

static bool enabled(void) { return config_get_bool("streaming", "list_art", false); }

static bool row_visible(lv_obj_t *row) {
	return lv_obj_get_screen(row) == lv_screen_active() && lv_obj_is_visible(row);
}

static void detach(art_slot_t *s) {
	if (s->image.pixels) {
		// LVGL must no longer reference either the descriptor or its pixels.
		sx_row_set_thumb(s->row, NULL, s->tone);
		lv_image_cache_drop(&s->image.dsc);
		sx_artview_release_cover(&s->image);
		if (s->identity_known)
			sx_row_set_content(s->row, s->identity_hue, false, false, 0);
	}
}

static void deleted(lv_event_t *event) {
	art_slot_t *s = lv_event_get_user_data(event);
	// The row's own DELETE handler may already have freed its private state.
	// Deletion prevents further drawing; do not call row setters here.
	lv_image_cache_drop(&s->image.dsc);
	sx_artview_release_cover(&s->image);
	s->row = s->owner = NULL;
	s->url[0] = '\0';
	s->visible = false;
	s->generation++;
}

static void load_image(const char *url, cover_image_t *image) {
	char *body = NULL;
	size_t size = 0;
	memset(image, 0, sizeof(*image));
	if (http_get(url, &body, &size, BODY_LIMIT, FETCH_SECONDS) && size > 0) {
		cover_load_image_memory(body, size, ART_SIDE, ART_SIDE, COVER_FIT_COVER, image);
	}
	free(body);
}

static void *worker(void *unused) {
	(void)unused;
	thread_be_low_priority("streamrowart");
	http_set_thread_timeout_cap(FETCH_SECONDS);
	for (;;) {
		pthread_mutex_lock(&lock);
		while (!requested)
			pthread_cond_wait(&changed, &lock);
		char url[URL_SIZE];
		memcpy(url, job_url, sizeof(url));
		requested = false;
		pthread_mutex_unlock(&lock);
		cover_image_t image;
		load_image(url, &image);
		pthread_mutex_lock(&lock);
		result = image;
		done = true;
		pthread_mutex_unlock(&lock);
	}
	return NULL;
}

static bool copy_image(art_slot_t *s, const cover_image_t *image) {
	uint8_t *pixels = malloc(image->dsc.data_size);
	if (!pixels)
		return false;
	memcpy(pixels, image->pixels, image->dsc.data_size);
	detach(s);
	s->image = *image;
	s->image.pixels = pixels;
	s->image.dsc.data = pixels;
	sx_row_set_thumb(s->row, &s->image.dsc, s->tone);
	if (s->identity_known)
		sx_row_set_content(s->row, s->identity_hue, false, true, s->image.display_generation);
	return true;
}

static void tick(lv_timer_t *unused) {
	(void)unused;
	bool allow = enabled();
	// Hidden/recycled rows never receive late images, and retain no pixels.
	for (int i = 0; i < ROW_SLOTS; i++) {
		art_slot_t *s = &slots[i];
		if (s->row && (!allow || !s->visible || !row_visible(s->row))) {
			detach(s);
			s->visible = false;
		}
	}
	pthread_mutex_lock(&lock);
	bool ready = done;
	cover_image_t image = {0};
	if (ready) {
		image = result;
		memset(&result, 0, sizeof(result));
		done = false;
		busy = false;
	}
	bool working = busy;
	pthread_mutex_unlock(&lock);
	if (ready) {
		for (int i = 0; i < ROW_SLOTS; i++) {
			art_slot_t *s = &slots[i];
			if (s->row && s->visible && s->generation == job_generations[i] && strcmp(s->url, job_url) == 0) {
				s->settled = true;
				if (image.pixels)
					copy_image(s, &image);
			}
		}
		cover_free(&image);
	}
	if (working)
		return;

	for (int i = 0; i < ROW_SLOTS; i++) {
		art_slot_t *s = &slots[i];
		if (!s->row || !s->visible || s->settled || !s->url[0])
			continue;
		// A second row of the same album needs no second HTTP/decode.
		for (int j = 0; j < ROW_SLOTS; j++) {
			if (j != i && slots[j].image.pixels && strcmp(s->url, slots[j].url) == 0) {
				s->settled = true;
				copy_image(s, &slots[j].image);
				break;
			}
		}
		if (s->settled)
			continue;
		if (!worker_started) {
			pthread_t thread;
			pthread_attr_t attr;
			pthread_attr_init(&attr);
			pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
			pthread_attr_setstacksize(&attr, 256 * 1024);
			worker_started = pthread_create(&thread, &attr, worker, NULL) == 0;
			pthread_attr_destroy(&attr);
			if (!worker_started) {
				s->settled = true;
				continue;
			}
		}
		pthread_mutex_lock(&lock);
		snprintf(job_url, sizeof(job_url), "%s", s->url);
		for (int j = 0; j < ROW_SLOTS; j++)
			job_generations[j] = slots[j].visible ? slots[j].generation : 0;
		busy = requested = true;
		pthread_cond_signal(&changed);
		pthread_mutex_unlock(&lock);
		return;
	}
	lv_timer_pause(timer);
}

static void wake(void) {
	if (!timer)
		timer = lv_timer_create(tick, 100, NULL);
	else
		lv_timer_resume(timer);
}

void streamrowart_forget(lv_obj_t *row) {
	if (!slots)
		return;
	for (int i = 0; i < ROW_SLOTS; i++) {
		art_slot_t *s = &slots[i];
		if (s->row == row) {
			detach(s);
			s->generation++;
			s->url[0] = '\0';
			s->settled = s->visible = false;
			return;
		}
	}
}

void streamrowart_bind(lv_obj_t *owner, lv_obj_t *row, int index, const char *url, lv_color_t tone) {
	if (!enabled()) {
		streamrowart_forget(row);
		sx_row_set_thumb(row, NULL, tone);
		return;
	}
	if (!slots)
		slots = calloc(ROW_SLOTS, sizeof(*slots));
	art_slot_t *s = NULL;
	for (int i = 0; slots && i < ROW_SLOTS; i++) {
		if (slots[i].row == row) {
			s = &slots[i];
			break;
		}
		if (!s && !slots[i].row)
			s = &slots[i];
	}
	if (!s) {
		sx_row_set_thumb(row, NULL, tone);
		return;
	}
	if (!s->row) {
		s->row = row;
		lv_obj_add_event_cb(row, deleted, LV_EVENT_DELETE, s);
	}
	url = url ? url : "";
	// Never fetch a truncated URL.
	if (strlen(url) >= sizeof(s->url))
		url = "";
	if (s->owner != owner || s->index != index || strcmp(s->url, url) != 0) {
		detach(s);
		s->generation++;
		s->settled = s->visible = false;
		snprintf(s->url, sizeof(s->url), "%s", url);
	}
	s->owner = owner;
	s->index = index;
	s->tone = tone;
	sx_row_set_thumb(row, s->image.pixels ? &s->image.dsc : NULL, tone);
}

void streamrowart_visible(lv_obj_t *owner, int first, int last) {
	if (!enabled()) {
		streamrowart_reset(owner);
		return;
	}
	if (!slots)
		return;
	for (int i = 0; i < ROW_SLOTS; i++) {
		art_slot_t *s = &slots[i];
		if (s->row && s->owner == owner) {
			bool visible = s->index >= first && s->index <= last && row_visible(s->row);
			if (visible && !s->visible && !s->image.pixels)
				s->settled = false;
			s->visible = visible;
		}
	}
	wake();
}

void streamrowart_reset(lv_obj_t *owner) {
	if (!slots)
		return;
	for (int i = 0; i < ROW_SLOTS; i++) {
		if (slots[i].row && slots[i].owner == owner)
			streamrowart_forget(slots[i].row);
	}
	if (timer) {
		// Another service may just have become visible during this transition.
		// One pass settles its requests or pauses the timer again.
		lv_timer_resume(timer);
	}
}

void streamrowart_content(lv_obj_t *owner, lv_obj_t *row, int index, const char *url, float hue) {
	streamrowart_bind(owner, row, index, url, sx_placeholder_tone(hue));
	art_slot_t *slot = NULL;
	for (int i = 0; slots && i < ROW_SLOTS; i++)
		if (slots[i].row == row) {
			slot = &slots[i];
			break;
		}
	if (slot) {
		slot->identity_hue = hue;
		slot->identity_known = true;
	}
	bool photo = slot && slot->image.pixels;
	sx_row_set_content(row, hue, false, photo, photo ? slot->image.display_generation : 0);
}
