#include "imageview.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/spinner.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/sx_mini.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"

lv_obj_t *imageview_screen;

static lv_obj_t *title_label;
static lv_obj_t *picture;
static lv_obj_t *busy;
static lv_obj_t *failed; // the file is not a picture this player can decode
static int box_w, box_h;

static cover_image_t shown; // what `picture` draws
static unsigned generation; // a decode for a picture no longer wanted is thrown away
static pthread_mutex_t result_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
	unsigned generation;
	char path[1024];
	int w, h;
	cover_image_t image;
	bool ok;
} decode_job_t;
static decode_job_t *ready;
static decode_job_t *pending;
static bool worker_active;
static lv_timer_t *result_timer;

static void drop_picture(void) {
	lv_image_set_src(picture, NULL);
	cover_free(&shown);
}

static void decoded(void *user) {
	decode_job_t *job = user;
	if (job->generation != generation) {
		cover_free(&job->image);
		free(job);
		return;
	}
	lv_obj_set_hidden(busy, true);
	if (!job->ok) {
		lv_obj_set_hidden(failed, false);
		free(job);
		return;
	}
	drop_picture();
	shown = job->image;
	lv_image_set_src(picture, &shown.dsc);
	lv_obj_set_hidden(picture, false);
	free(job);
}

static void *decode_worker(void *arg) {
	thread_be_background("image viewer");
	decode_job_t *job = arg;
	while (job) {
		job->ok = cover_load_image_file(job->path, job->w, job->h, COVER_FIT_CONTAIN, &job->image);
		pthread_mutex_lock(&result_lock);
		if (job->generation == generation) {
			if (ready) {
				cover_free(&ready->image);
				free(ready);
			}
			ready = job;
		} else {
			cover_free(&job->image);
			free(job);
		}
		job = pending;
		pending = NULL;
		if (!job)
			worker_active = false;
		pthread_mutex_unlock(&result_lock);
	}
	return NULL;
}

static void result_poll(lv_timer_t *timer) {
	pthread_mutex_lock(&result_lock);
	decode_job_t *job = ready;
	ready = NULL;
	pthread_mutex_unlock(&result_lock);
	if (job) {
		bool current = job->generation == generation;
		decoded(job);
		if (current || lv_screen_active() != imageview_screen) {
			lv_timer_pause(timer);
		}
	}
}

void imageview_open(const char *path) {
	gui_page_ensure(imageview_screen);
	if (!path || !result_timer) {
		return;
	}
	pthread_mutex_lock(&result_lock);
	generation++;
	pthread_mutex_unlock(&result_lock);
	drop_picture();
	lv_obj_set_hidden(picture, true);
	lv_obj_set_hidden(failed, true);
	lv_obj_set_hidden(busy, false);

	const char *slash = strrchr(path, '/');
	lv_label_set_text(title_label, slash ? slash + 1 : path);
	switch_screen(imageview_screen);

	decode_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		lv_obj_set_hidden(busy, true);
		lv_obj_set_hidden(failed, false);
		return;
	}
	job->generation = generation;
	job->w = box_w;
	job->h = box_h;
	snprintf(job->path, sizeof(job->path), "%s", path);
	pthread_mutex_lock(&result_lock);
	if (worker_active) {
		free(pending);
		pending = job;
		pthread_mutex_unlock(&result_lock);
		lv_timer_resume(result_timer);
		return;
	}
	worker_active = true;
	pthread_mutex_unlock(&result_lock);
	pthread_t thread;
	if (pthread_create(&thread, NULL, decode_worker, job) != 0) {
		pthread_mutex_lock(&result_lock);
		worker_active = false;
		pthread_mutex_unlock(&result_lock);
		free(job);
		lv_obj_set_hidden(busy, true);
		lv_obj_set_hidden(failed, false);
		return;
	}
	pthread_detach(thread);
	lv_timer_resume(result_timer);
}

// Leaving the page lets the picture go: a photo the size of the screen is the
// better part of a megabyte, and nothing else needs it.
static void unloaded_cb(lv_event_t *e) {
	(void)e;
	pthread_mutex_lock(&result_lock);
	generation++;
	free(pending);
	pending = NULL;
	if (ready) {
		cover_free(&ready->image);
		free(ready);
		ready = NULL;
	}
	pthread_mutex_unlock(&result_lock);
	lv_timer_pause(result_timer);
	drop_picture();
}

static void build_page(gui_config_t *cfg) {
	sx_mini_hide_on_screen(imageview_screen);
	result_timer = lv_timer_create(result_poll, 100, NULL);
	if (result_timer) {
		lv_timer_pause(result_timer);
	}
	lv_obj_add_style(imageview_screen, &theme_style_screen, 0);
	title_label = settingsrow_title(imageview_screen, cfg, "");
	settingsrow_title_corner_slots(title_label, cfg, 0);

	int content_top = settingsrow_content_top(cfg);
	box_w = cfg->screen_width - 2 * cfg->padding;
	box_h = cfg->screen_height - content_top - 2 * cfg->padding;

	lv_obj_t *area = lv_obj_create(imageview_screen);
	lv_obj_remove_style_all(area);
	lv_obj_set_size(area, cfg->screen_width, cfg->screen_height - content_top);
	lv_obj_align(area, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_scrollable(area, false);
	lv_obj_set_event_bubble(area, true);

	picture = lv_image_create(area);
	lv_obj_center(picture);
	lv_obj_set_event_bubble(picture, true);
	lv_obj_set_hidden(picture, true);

	busy = spinner_create(area, &icon_loader_big);
	lv_obj_center(busy);
	lv_obj_set_hidden(busy, true);

	failed = lv_label_create(area);
	lv_label_set_text(failed, tr("files_image_unreadable"));
	lv_label_set_long_mode(failed, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(failed, box_w);
	lv_obj_set_style_text_align(failed, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(failed, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(failed, &font_ui_22, 0);
	lv_obj_center(failed);
	lv_obj_set_hidden(failed, true);

	lv_obj_add_event_cb(imageview_screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(imageview_screen);
}

void imageview_init(gui_config_t *cfg) {
	(void)cfg;
	gui_page_defer(imageview_screen, build_page);
}
