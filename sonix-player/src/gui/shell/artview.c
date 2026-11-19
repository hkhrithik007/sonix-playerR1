// GUI-owned views of already decoded artwork. Source pixels are never changed.
#include "artview.h"
#include "lvgl/src/misc/cache/instance/lv_image_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint16_t gray_linear5[32] = {
	0, 164, 352, 625, 992, 1461, 2040, 2734, 3550, 4492, 5565, 6775, 8127, 9623, 11269, 13069, 15026, 17143, 19426, 21877, 24499, 27295, 30270, 33426, 36766, 40292, 44009, 47918, 52022, 56325, 60828, 65535,
};
static const uint16_t gray_linear6[64] = {
	0, 81, 161, 244, 345, 466, 609, 776, 965, 1180, 1420, 1687, 1980, 2301, 2651, 3030, 3439, 3879, 4349, 4851, 5386, 5953, 6554, 7189, 7858, 8562, 9302, 10078, 10890, 11739, 12626, 13551, 14513, 15515, 16556, 17636, 18757, 19918, 21120, 22363, 23648, 24974, 26344, 27756, 29211, 30710, 32253, 33840, 35471, 37148, 38870, 40638, 42452, 44312, 46219, 48173, 50174, 52223, 54320, 56465, 58659, 60901, 63193, 65535,
};
static const uint8_t gray_encoded[1024] = {
	0,	 3,	  6,   10,	13,	 15,  18,  20,	22,	 23,  25,  27,	28,	 30,  31,  32,	34,	 35,  36,  37,	38,	 39,  40,  41,	42,	 43,  44,  45,	46,	 47,  48,  49,	49,	 50,  51,  52,	53,	 53,  54,  55,	56,	 56,  57,  58,	58,	 59,  60,  61,	61,	 62,  62,  63,	64,	 64,  65,  66,	66,	 67,  67,  68,	68,	 69,  70,  70,	71,	 71,  72,  72,	73,	 73,  74,  74,	75,	 76,  76,  77,	77,	 78,  78,  79,	79,	 79,  80,  80,	81,	 81,  82,  82,	83,	 83,  84,  84,	85,	 85,  85,  86,	86,	 87,  87,  88,	88,	 88,  89,  89,	90,	 90,  91,  91,	91,	 92,  92,  93,	93,	 93,  94,  94,	95,	 95,  95,  96,	96,	 97,  97,  97,	98,	 98,  98,  99,	99,	 99,  100, 100, 101, 101, 101, 102, 102, 102, 103, 103, 103, 104, 104, 104, 105, 105, 106, 106, 106, 107, 107, 107, 108, 108, 108, 109, 109, 109, 110, 110, 110, 110, 111, 111, 111, 112, 112, 112, 113, 113, 113,
	114, 114, 114, 115, 115, 115, 115, 116, 116, 116, 117, 117, 117, 118, 118, 118, 118, 119, 119, 119, 120, 120, 120, 121, 121, 121, 121, 122, 122, 122, 123, 123, 123, 123, 124, 124, 124, 125, 125, 125, 125, 126, 126, 126, 126, 127, 127, 127, 128, 128, 128, 128, 129, 129, 129, 129, 130, 130, 130, 130, 131, 131, 131, 131, 132, 132, 132, 133, 133, 133, 133, 134, 134, 134, 134, 135, 135, 135, 135, 136, 136, 136, 136, 137, 137, 137, 137, 138, 138, 138, 138, 138, 139, 139, 139, 139, 140, 140, 140, 140, 141, 141, 141, 141, 142, 142, 142, 142, 143, 143, 143, 143, 143, 144, 144, 144, 144, 145, 145, 145, 145, 146, 146, 146, 146, 146, 147, 147, 147, 147, 148, 148, 148, 148, 148, 149, 149, 149, 149, 150, 150, 150, 150, 150, 151, 151, 151, 151, 152, 152, 152, 152, 152, 153, 153, 153, 153, 153, 154, 154, 154, 154, 155, 155, 155, 155, 155, 156, 156, 156, 156,
	156, 157, 157, 157, 157, 157, 158, 158, 158, 158, 158, 159, 159, 159, 159, 159, 160, 160, 160, 160, 160, 161, 161, 161, 161, 161, 162, 162, 162, 162, 162, 163, 163, 163, 163, 163, 164, 164, 164, 164, 164, 165, 165, 165, 165, 165, 166, 166, 166, 166, 166, 167, 167, 167, 167, 167, 168, 168, 168, 168, 168, 168, 169, 169, 169, 169, 169, 170, 170, 170, 170, 170, 171, 171, 171, 171, 171, 171, 172, 172, 172, 172, 172, 173, 173, 173, 173, 173, 173, 174, 174, 174, 174, 174, 175, 175, 175, 175, 175, 175, 176, 176, 176, 176, 176, 177, 177, 177, 177, 177, 177, 178, 178, 178, 178, 178, 178, 179, 179, 179, 179, 179, 179, 180, 180, 180, 180, 180, 181, 181, 181, 181, 181, 181, 182, 182, 182, 182, 182, 182, 183, 183, 183, 183, 183, 183, 184, 184, 184, 184, 184, 184, 185, 185, 185, 185, 185, 185, 186, 186, 186, 186, 186, 186, 187, 187, 187, 187, 187, 187, 188,
	188, 188, 188, 188, 188, 189, 189, 189, 189, 189, 189, 190, 190, 190, 190, 190, 190, 191, 191, 191, 191, 191, 191, 191, 192, 192, 192, 192, 192, 192, 193, 193, 193, 193, 193, 193, 194, 194, 194, 194, 194, 194, 194, 195, 195, 195, 195, 195, 195, 196, 196, 196, 196, 196, 196, 197, 197, 197, 197, 197, 197, 197, 198, 198, 198, 198, 198, 198, 199, 199, 199, 199, 199, 199, 199, 200, 200, 200, 200, 200, 200, 200, 201, 201, 201, 201, 201, 201, 202, 202, 202, 202, 202, 202, 202, 203, 203, 203, 203, 203, 203, 203, 204, 204, 204, 204, 204, 204, 204, 205, 205, 205, 205, 205, 205, 206, 206, 206, 206, 206, 206, 206, 207, 207, 207, 207, 207, 207, 207, 208, 208, 208, 208, 208, 208, 208, 209, 209, 209, 209, 209, 209, 209, 210, 210, 210, 210, 210, 210, 210, 211, 211, 211, 211, 211, 211, 211, 212, 212, 212, 212, 212, 212, 212, 212, 213, 213, 213, 213, 213, 213,
	213, 214, 214, 214, 214, 214, 214, 214, 215, 215, 215, 215, 215, 215, 215, 216, 216, 216, 216, 216, 216, 216, 216, 217, 217, 217, 217, 217, 217, 217, 218, 218, 218, 218, 218, 218, 218, 219, 219, 219, 219, 219, 219, 219, 219, 220, 220, 220, 220, 220, 220, 220, 221, 221, 221, 221, 221, 221, 221, 221, 222, 222, 222, 222, 222, 222, 222, 222, 223, 223, 223, 223, 223, 223, 223, 224, 224, 224, 224, 224, 224, 224, 224, 225, 225, 225, 225, 225, 225, 225, 225, 226, 226, 226, 226, 226, 226, 226, 227, 227, 227, 227, 227, 227, 227, 227, 228, 228, 228, 228, 228, 228, 228, 228, 229, 229, 229, 229, 229, 229, 229, 229, 230, 230, 230, 230, 230, 230, 230, 230, 231, 231, 231, 231, 231, 231, 231, 231, 232, 232, 232, 232, 232, 232, 232, 232, 233, 233, 233, 233, 233, 233, 233, 233, 234, 234, 234, 234, 234, 234, 234, 234, 235, 235, 235, 235, 235, 235, 235, 235, 236,
	236, 236, 236, 236, 236, 236, 236, 236, 237, 237, 237, 237, 237, 237, 237, 237, 238, 238, 238, 238, 238, 238, 238, 238, 239, 239, 239, 239, 239, 239, 239, 239, 239, 240, 240, 240, 240, 240, 240, 240, 240, 241, 241, 241, 241, 241, 241, 241, 241, 241, 242, 242, 242, 242, 242, 242, 242, 242, 243, 243, 243, 243, 243, 243, 243, 243, 243, 244, 244, 244, 244, 244, 244, 244, 244, 245, 245, 245, 245, 245, 245, 245, 245, 245, 246, 246, 246, 246, 246, 246, 246, 246, 246, 247, 247, 247, 247, 247, 247, 247, 247, 248, 248, 248, 248, 248, 248, 248, 248, 248, 249, 249, 249, 249, 249, 249, 249, 249, 249, 250, 250, 250, 250, 250, 250, 250, 250, 250, 251, 251, 251, 251, 251, 251, 251, 251, 251, 252, 252, 252, 252, 252, 252, 252, 252, 252, 253, 253, 253, 253, 253, 253, 253, 253, 253, 254, 254, 254, 254, 254, 254, 254, 254, 254, 255, 255, 255, 255, 255,
};

static uint16_t artwork_gray_pixel(uint16_t pixel) {
	uint32_t light = (13933U * gray_linear5[pixel >> 11] + 46871U * gray_linear6[(pixel >> 5) & 63] + 4732U * gray_linear5[pixel & 31] + 32768U) >> 16;
	unsigned gray = gray_encoded[(light * 1023U + 32767U) / 65535U];
	unsigned r = (gray * 31U + 127U) / 255U, g = (gray * 63U + 127U) / 255U;
	return (uint16_t)((r << 11) | (g << 5) | r);
}
static bool artwork_gray565(const uint8_t *source, uint8_t *target, size_t width, size_t height, size_t source_stride, size_t target_stride) {
	if (!source || !target || !width || !height || width > SIZE_MAX / 2)
		return false;
	size_t bytes = width * 2;
	if (source_stride < bytes || target_stride < bytes || height - 1 > (SIZE_MAX - bytes) / source_stride || height - 1 > (SIZE_MAX - bytes) / target_stride)
		return false;
	for (size_t y = 0; y < height; y++)
		for (size_t x = 0; x < width; x++) {
			uint16_t pixel;
			memcpy(&pixel, source + y * source_stride + x * 2, 2);
			pixel = artwork_gray_pixel(pixel);
			memcpy(target + y * target_stride + x * 2, &pixel, 2);
		}
	return true;
}

typedef struct gray_variant {
	lv_image_dsc_t descriptor;
	const void *original_pixels;
	uint32_t generation;
	size_t source_stride, charge;
	unsigned references;
	uint8_t pixels[];
} gray_variant_t;

typedef struct artwork_view {
	struct artwork_view *next;
	lv_obj_t *image;
	const lv_image_dsc_t *source;
	lv_image_dsc_t original;
	uint32_t generation, failed_epoch;
	gray_variant_t *variant;
	bool background;
} artwork_view_t;
static artwork_view_t *views;
static bool monochrome;
static unsigned epoch = 1, policy_epoch = 1, warned_epoch;
static lv_obj_t *incoming_screen, *opaque_overlay, *preparing_overlay;
static sx_artview_stats_t stats;
#define VIEW_CHARGE (sizeof(artwork_view_t) + 256U)

static artwork_view_t *view_of(lv_obj_t *image) {
	for (artwork_view_t *v = views; v; v = v->next)
		if (v->image == image)
			return v;
	return NULL;
}
static void set_source(artwork_view_t *v, const lv_image_dsc_t *source) {
	if (v->background)
		lv_obj_set_style_bg_image_src(v->image, source, 0);
	else
		lv_image_set_src(v->image, source);
}
static const void *get_source(artwork_view_t *v) { return v->background ? lv_obj_get_style_bg_image_src(v->image, 0) : lv_image_get_src(v->image); }
static bool below(lv_obj_t *image, lv_obj_t *ancestor) {
	for (lv_obj_t *at = image; at; at = lv_obj_get_parent(at))
		if (at == ancestor)
			return true;
	return false;
}
static bool wanted(const artwork_view_t *v) {
	if (!monochrome || !v->source)
		return false;
	if (opaque_overlay && !below(v->image, opaque_overlay))
		return false;
	lv_obj_t *screen = lv_obj_get_screen(v->image);
	bool preparing = preparing_overlay && below(v->image, preparing_overlay);
	bool coming = (incoming_screen && screen == incoming_screen) || preparing;
	if (!coming && !lv_obj_is_visible(v->image))
		return false;
	for (lv_obj_t *at = v->image; at; at = lv_obj_get_parent(at))
		if (lv_obj_is_hidden(at))
			return false;
	if (preparing)
		return true;
	lv_area_t box;
	lv_obj_get_coords(v->image, &box);
	for (lv_obj_t *at = lv_obj_get_parent(v->image); at; at = lv_obj_get_parent(at)) {
		if (lv_obj_is_overflow_visible(at))
			continue;
		lv_area_t clip;
		lv_obj_get_coords(at, &clip);
		box.x1 = LV_MAX(box.x1, clip.x1);
		box.x2 = LV_MIN(box.x2, clip.x2);
		box.y1 = LV_MAX(box.y1, clip.y1);
		box.y2 = LV_MIN(box.y2, clip.y2);
		if (box.x1 > box.x2 || box.y1 > box.y2)
			return false;
	}
	if (box.x2 < 0 || box.y2 < 0 || box.x1 >= lv_display_get_horizontal_resolution(lv_display_get_default()) || box.y1 >= lv_display_get_vertical_resolution(lv_display_get_default()))
		return false;
	return true;
}
static void peak(void) {
	if (stats.bytes > stats.peak_bytes)
		stats.peak_bytes = stats.bytes;
}
static void failure(artwork_view_t *v, const char *reason) {
	if (v)
		v->failed_epoch = epoch;
	stats.failures++;
	if (warned_epoch != policy_epoch) {
		warned_epoch = policy_epoch;
		fprintf(stderr, "artview: %s; %s\n", reason, v ? "keeping original artwork" : "artwork temporarily hidden");
	}
}
static void release_gray(artwork_view_t *v, bool restore) {
	gray_variant_t *variant = v->variant;
	if (!variant)
		return;
	if (restore)
		set_source(v, v->source ? &v->original : NULL);
	v->variant = NULL;
	if (--variant->references == 0) {
		lv_image_cache_drop(&variant->descriptor);
		stats.bytes -= variant->charge;
		free(variant);
	}
}
static void release_hidden(void) {
	for (artwork_view_t *v = views; v; v = v->next)
		if (!wanted(v))
			release_gray(v, true);
}
static bool variant_matches(const gray_variant_t *variant, const artwork_view_t *v) {
	const lv_image_dsc_t *source = &v->original;
	size_t stride = source->header.stride ? source->header.stride : source->header.w * 2U;
	return variant && variant->original_pixels == source->data && variant->generation == v->generation && variant->descriptor.header.cf == source->header.cf && variant->descriptor.header.w == source->header.w && variant->descriptor.header.h == source->header.h && variant->source_stride == stride;
}
static bool convert(artwork_view_t *v) {
	if (!wanted(v))
		return true;
	if (variant_matches(v->variant, v)) {
		if (get_source(v) != &v->variant->descriptor)
			set_source(v, &v->variant->descriptor);
		return true;
	}
	if (v->failed_epoch == epoch)
		return false;
	release_gray(v, true);
	const lv_image_dsc_t *source = &v->original;
	size_t w = source->header.w, h = source->header.h;
	size_t stride = source->header.stride ? source->header.stride : w * 2;
	if (!v->generation || source->header.cf != LV_COLOR_FORMAT_RGB565 || source->header.flags & LV_IMAGE_FLAGS_COMPRESSED || !source->data || !w || !h || w > SIZE_MAX / 2 || h > SIZE_MAX / (w * 2) || stride < w * 2 || h - 1 > (SIZE_MAX - w * 2) / stride || (h - 1) * stride + w * 2 > source->data_size) {
		failure(v, "unsupported artwork descriptor");
		return false;
	}
	// Identical content displayed by the player, Studio or saver shares one allocation.
	for (artwork_view_t *other = views; other; other = other->next) {
		if (other != v && variant_matches(other->variant, v)) {
			v->variant = other->variant;
			v->variant->references++;
			set_source(v, &v->variant->descriptor);
			return true;
		}
	}
	size_t bytes = w * h * 2;
	if (bytes > SIZE_MAX - sizeof(gray_variant_t) - 32U) {
		failure(v, "invalid artwork size");
		return false;
	}
	size_t charge = sizeof(gray_variant_t) + bytes + 32U;
	if (charge > SX_ARTVIEW_BUDGET - stats.bytes)
		release_hidden();
	if (charge > SX_ARTVIEW_BUDGET - stats.bytes) {
		failure(v, "view budget exhausted");
		return false;
	}
	gray_variant_t *variant = malloc(sizeof(*variant) + bytes);
	if (!variant) {
		failure(v, "view allocation failed");
		return false;
	}
	if (!artwork_gray565(source->data, variant->pixels, w, h, stride, w * 2)) {
		free(variant);
		failure(v, "invalid artwork dimensions");
		return false;
	}
	variant->descriptor = *source;
	variant->descriptor.data = variant->pixels;
	variant->descriptor.data_size = (uint32_t)bytes;
	variant->descriptor.header.stride = (uint32_t)w * 2;
	variant->descriptor.header.flags &= ~(LV_IMAGE_FLAGS_ALLOCATED | LV_IMAGE_FLAGS_MODIFIABLE | LV_IMAGE_FLAGS_COMPRESSED);
	variant->original_pixels = source->data;
	variant->generation = v->generation;
	variant->source_stride = stride;
	variant->references = 1;
	variant->charge = charge;
	v->variant = variant;
	stats.bytes += charge;
	peak();
	stats.conversions++;
	set_source(v, &variant->descriptor);
	return true;
}
static void deleted(lv_event_t *event) {
	artwork_view_t *v = lv_event_get_user_data(event), **at = &views;
	while (*at && *at != v)
		at = &(*at)->next;
	if (*at)
		*at = v->next;
	release_gray(v, false);
	lv_image_cache_drop(&v->original);
	stats.bytes -= VIEW_CHARGE;
	stats.bindings--;
	free(v);
}
static void screen_event(lv_event_t *event) {
	lv_obj_t *screen = lv_event_get_target(event);
	if (lv_event_get_code(event) == LV_EVENT_DELETE) {
		if (incoming_screen == screen)
			incoming_screen = NULL;
		return;
	}
	if (lv_event_get_code(event) == LV_EVENT_SCREEN_LOADED && incoming_screen == screen)
		incoming_screen = NULL;
	epoch++;
	sx_artview_refresh();
}
static void watch_screen(lv_obj_t *image) {
	lv_obj_t *screen = lv_obj_get_screen(image);
	lv_obj_remove_event_cb(screen, screen_event);
	lv_obj_add_event_cb(screen, screen_event, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(screen, screen_event, LV_EVENT_SCREEN_UNLOADED, NULL);
	lv_obj_add_event_cb(screen, screen_event, LV_EVENT_DELETE, NULL);
}
static bool bind_view(lv_obj_t *image, const lv_image_dsc_t *source, uint32_t generation, bool background) {
	if (!image)
		return false;
	artwork_view_t *v = view_of(image);
	if (!source) {
		sx_artview_detach(image);
		return true;
	}
	if (!v) {
		if (VIEW_CHARGE > SX_ARTVIEW_BUDGET - stats.bytes)
			release_hidden();
		if (stats.bindings >= 512 || VIEW_CHARGE > SX_ARTVIEW_BUDGET - stats.bytes) {
			if (background)
				lv_obj_set_style_bg_image_src(image, NULL, 0);
			else
				lv_image_set_src(image, NULL);
			failure(NULL, "binding budget exhausted");
			return false;
		}
		v = calloc(1, sizeof(*v));
		if (!v) {
			if (background)
				lv_obj_set_style_bg_image_src(image, NULL, 0);
			else
				lv_image_set_src(image, NULL);
			failure(NULL, "binding allocation failed");
			return false;
		}
		v->image = image;
		v->background = background;
		v->next = views;
		views = v;
		stats.bytes += VIEW_CHARGE;
		stats.bindings++;
		peak();
		lv_obj_add_event_cb(image, deleted, LV_EVENT_DELETE, v);
		watch_screen(image);
	}
	bool same = v->source && v->generation == generation && v->original.header.w == source->header.w && v->original.header.h == source->header.h && v->original.header.cf == source->header.cf;
	if (!same) {
		v->failed_epoch = 0;
	}
	lv_image_cache_drop(&v->original);
	v->source = source;
	v->original = *source;
	v->generation = generation;
	if (!same || !v->variant)
		set_source(v, &v->original);
	if (monochrome && !same)
		lv_obj_update_layout(image);
	if (!monochrome || !wanted(v)) {
		release_gray(v, true);
		set_source(v, &v->original);
		return true;
	}
	return convert(v);
}
bool sx_artview_bind(lv_obj_t *image, const lv_image_dsc_t *source, uint32_t generation) { return bind_view(image, source, generation, false); }
bool sx_artview_bind_background(lv_obj_t *object, const lv_image_dsc_t *source, uint32_t generation) { return bind_view(object, source, generation, true); }
bool sx_artview_copy_gray565(const void *source, void *target, size_t width, size_t height, size_t source_stride, size_t target_stride) { return artwork_gray565(source, target, width, height, source_stride, target_stride); }
bool sx_artview_source_is(lv_obj_t *image, const lv_image_dsc_t *source) {
	artwork_view_t *v = view_of(image);
	return v && v->source == source;
}
void sx_artview_detach(lv_obj_t *image) {
	if (!image)
		return;
	artwork_view_t *v = view_of(image);
	if (!v) {
		if (lv_obj_check_type(image, &lv_image_class))
			lv_image_set_src(image, NULL);
		else
			lv_obj_set_style_bg_image_src(image, NULL, 0);
		return;
	}
	if (!v->source && !v->variant)
		return;
	set_source(v, NULL);
	release_gray(v, false);
	lv_image_cache_drop(&v->original);
	v->source = NULL;
	v->generation = 0;
	v->failed_epoch = 0;
	memset(&v->original, 0, sizeof(v->original));
}
void sx_artview_forget_pixels(const void *pixels, uint32_t generation) {
	if (!pixels)
		return;
	for (artwork_view_t *v = views; v; v = v->next)
		if (v->source && v->original.data == pixels && (!generation || v->generation == generation))
			sx_artview_detach(v->image);
}
void sx_artview_release_cover(cover_image_t *cover) {
	if (!cover)
		return;
	sx_artview_forget_pixels(cover->pixels, cover->display_generation);
	cover_free(cover);
}
bool sx_artview_refresh(void) {
	bool ok = true;
	for (artwork_view_t *v = views; v; v = v->next)
		if (!convert(v))
			ok = false;
	release_hidden();
	return ok;
}
bool sx_artview_mode(bool on) {
	if (monochrome != on) {
		monochrome = on;
		epoch++;
		policy_epoch++;
	}
	return sx_artview_refresh();
}
void sx_artview_screen_will_show(lv_obj_t *screen) {
	incoming_screen = screen;
	epoch++;
	sx_artview_refresh();
}
static void overlay_deleted(lv_event_t *event) {
	lv_obj_t *object = lv_event_get_target(event);
	if (opaque_overlay == object)
		opaque_overlay = NULL;
	if (preparing_overlay == object)
		preparing_overlay = NULL;
}
static void overlay_watch(lv_obj_t *object) {
	if (!object)
		return;
	lv_obj_remove_event_cb(object, overlay_deleted);
	lv_obj_add_event_cb(object, overlay_deleted, LV_EVENT_DELETE, NULL);
}
void sx_artview_overlay_will_show(lv_obj_t *overlay) {
	// A partially visible overlay needs both surfaces. Keep its prepared
	// artwork even while it is still below the viewport; do not convert/free
	// it again at every drag coordinate.
	if (preparing_overlay == overlay && !opaque_overlay)
		return;
	if (preparing_overlay && preparing_overlay != opaque_overlay)
		lv_obj_remove_event_cb(preparing_overlay, overlay_deleted);
	if (opaque_overlay)
		lv_obj_remove_event_cb(opaque_overlay, overlay_deleted);
	opaque_overlay = NULL;
	preparing_overlay = overlay;
	overlay_watch(overlay);
	epoch++;
	sx_artview_refresh();
}
void sx_artview_opaque_overlay(lv_obj_t *overlay) {
	if (opaque_overlay == overlay && !preparing_overlay)
		return;
	if (preparing_overlay && preparing_overlay != opaque_overlay)
		lv_obj_remove_event_cb(preparing_overlay, overlay_deleted);
	preparing_overlay = NULL;
	if (opaque_overlay && opaque_overlay != overlay)
		lv_obj_remove_event_cb(opaque_overlay, overlay_deleted);
	opaque_overlay = overlay;
	overlay_watch(overlay);
	epoch++;
	sx_artview_refresh();
}
sx_artview_stats_t sx_artview_stats(void) { return stats; }
lv_color_t sx_artview_gray_color(lv_color_t color) {
	uint16_t pixel = (uint16_t)(((color.red >> 3) << 11) | ((color.green >> 2) << 5) | (color.blue >> 3));
	pixel = artwork_gray_pixel(pixel);
	unsigned r = pixel >> 11, g = (pixel >> 5) & 63, b = pixel & 31;
	return lv_color_make((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2));
}
