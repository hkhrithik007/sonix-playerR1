#ifndef SONIX_ARTVIEW_H
#define SONIX_ARTVIEW_H
#include "lvgl/lvgl.h"
#include "src/gui/nowplaying/cover.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define SX_ARTVIEW_BUDGET (3U * 1024U * 1024U)
// Accounted bytes conservatively reserve callback/allocator overhead per view.
typedef struct {
	size_t bytes, peak_bytes;
	unsigned bindings, conversions, failures;
} sx_artview_stats_t;
// GUI thread only. A nonzero generation identifies complete immutable pixels.
// Sources remain borrowed: detach before freeing/recycling their storage.
bool sx_artview_bind(lv_obj_t *image, const lv_image_dsc_t *source, uint32_t generation);
// Same bounded adapter for a real artwork background, not branding or icons.
bool sx_artview_bind_background(lv_obj_t *object, const lv_image_dsc_t *source, uint32_t generation);
// One-shot copy for an existing derived RGB565 face; never call from DRAW.
bool sx_artview_copy_gray565(const void *source, void *target, size_t width, size_t height, size_t source_stride, size_t target_stride);
bool sx_artview_source_is(lv_obj_t *image, const lv_image_dsc_t *source);
void sx_artview_detach(lv_obj_t *image);
void sx_artview_forget_pixels(const void *pixels, uint32_t generation);
// GUI-owned pictures only; worker results still use cover_free directly.
void sx_artview_release_cover(cover_image_t *cover);
bool sx_artview_mode(bool on);
bool sx_artview_refresh(void);
void sx_artview_screen_will_show(lv_obj_t *screen);
// Begin/continue a moving overlay: keep both surfaces ready until commit/cancel.
void sx_artview_overlay_will_show(lv_obj_t *overlay);
void sx_artview_opaque_overlay(lv_obj_t *overlay);
sx_artview_stats_t sx_artview_stats(void);
lv_color_t sx_artview_gray_color(lv_color_t color);
#endif
