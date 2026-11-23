#include "easteregg.h"

#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/shell/icons.h"

// Taps before the press that opens it.
#define EASTEREGG_TAPS 5

// The heart. Grown from nothing to its own size, so the icon is generated at
// the size it is seen at: LVGL draws it 1:1 at the end of the animation and
// never above it.
#define HEART_GROW_MS 700
#define HEART_HOLD_MS 250
#define HEART_FADE_MS 500
#define HEART_COLOR lv_color_make(228, 55, 70)

static int taps;

static lv_obj_t *heart;

// ---------------------------------------------------------------------------
// The heart
// ---------------------------------------------------------------------------

static void heart_scale_cb(void *obj, int32_t value) { lv_image_set_scale((lv_obj_t *)obj, (uint32_t)value); }

static void heart_opa_cb(void *obj, int32_t value) {
	lv_obj_set_style_image_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void heart_faded_cb(lv_anim_t *a) {
	(void)a;
	if (heart) {
		lv_obj_delete(heart);
		heart = NULL;
	}
}

static void heart_grown_cb(lv_anim_t *a) {
	if (!heart) {
		return;
	}

	lv_anim_t fade;
	lv_anim_init(&fade);
	lv_anim_set_var(&fade, a->var);
	lv_anim_set_exec_cb(&fade, heart_opa_cb);
	lv_anim_set_values(&fade, LV_OPA_COVER, LV_OPA_TRANSP);
	lv_anim_set_delay(&fade, HEART_HOLD_MS);
	lv_anim_set_duration(&fade, HEART_FADE_MS);
	lv_anim_set_completed_cb(&fade, heart_faded_cb);
	lv_anim_start(&fade);
}

static void heart_pop(void) {
	// A second one on top of the first: the old heart goes, animations and all
	// (deleting an object cancels the animations running on it).
	if (heart) {
		lv_obj_delete(heart);
		heart = NULL;
	}

	heart = lv_image_create(lv_layer_top());
	lv_image_set_src(heart, &icon_heart);
	lv_obj_set_style_image_recolor(heart, HEART_COLOR, 0);
	lv_obj_set_style_image_recolor_opa(heart, LV_OPA_COVER, 0);
	lv_obj_center(heart);

	// From a single pixel rather than from zero: a scale of zero is a zero-wide
	// draw area, and nothing downstream has to be asked to divide by it.
	lv_image_set_scale(heart, 1);

	// bounce reaches full size at about two fifths of the duration, overshoots
	// back by a twentieth, and settles at 1:1 -- which is the growing-with-a-
	// bounce this wants, and never larger than the icon really is.
	lv_anim_t grow;
	lv_anim_init(&grow);
	lv_anim_set_var(&grow, heart);
	lv_anim_set_exec_cb(&grow, heart_scale_cb);
	lv_anim_set_values(&grow, 1, LV_SCALE_NONE);
	lv_anim_set_duration(&grow, HEART_GROW_MS);
	lv_anim_set_path_cb(&grow, lv_anim_path_bounce);
	lv_anim_set_completed_cb(&grow, heart_grown_cb);
	lv_anim_start(&grow);
}

// ---------------------------------------------------------------------------
// The gesture
// ---------------------------------------------------------------------------

static void row_clicked_cb(lv_event_t *e) {
	(void)e;

	taps++;
	// show easter egg if taps is high enough
	if (taps >= EASTEREGG_TAPS) {
		easteregg_reset();  // reset taps
		heart_pop();
	}
}

void easteregg_attach(lv_obj_t *row) {
	if (!row) {
		return;
	}

	lv_obj_set_clickable(row, true);
	lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, NULL);
}

void easteregg_reset(void) {
	taps = 0;
}
