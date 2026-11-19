#include "ipodmode.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/corners.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/topbar.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"

#define DESIGN_W 480
#define DESIGN_H 800
#define WHEEL_SIZE 300
#define WHEEL_HUB 104
#define WHEEL_TAP_SLOP 12
#define WHEEL_STEP_RADIANS .105f
#define PI 3.14159265359f
#define VOLUME_SHOW_MS 1300
#define DISPLAY_POLL_MS 200
#define LCD_INK 0x1c1d1b
#define LCD_BG 0xf5f5f3
#define LCD_HEADER_H 34
#define LCD_H 350
#define SLEEVE_SIZE 176
#define SLEEVE_FAR_HEIGHT 148
#define REFLECTION_H 42
#define SLEEVE_IMAGE_H (SLEEVE_SIZE + REFLECTION_H)
#define BAR_W 302
#define BAR_H 22
#define SHELL_BEVEL 50
#define SHELL_CORNER_RADIUS 86
#define SHELL_MAX_EDGE 96
#define SHELL_TILE 16
#define SHELL_INK 0x121212
#define WHEEL_INK 0xe7eef4

static lv_obj_t *screen, *wheel;
static lv_obj_t *cover, *placeholder, *status_icon, *mode_icon, *battery_fill;
static lv_obj_t *position, *title, *artist, *album, *elapsed, *remaining, *bar_fill;
static lv_timer_t *display_timer;
static int scale_w;
static bool volume_shown;
static uint32_t volume_at;
static const lv_image_dsc_t *displayed_art;
static uint16_t sleeve_pixels[SLEEVE_SIZE * SLEEVE_IMAGE_H];
static uint16_t shell_pixels[SHELL_MAX_EDGE * (2 * SHELL_MAX_EDGE + 3 * SHELL_TILE)];
static lv_image_dsc_t shell_images[5];
static const lv_image_dsc_t sleeve_image = {
	.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
			   .w = SLEEVE_SIZE, .h = SLEEVE_IMAGE_H, .stride = SLEEVE_SIZE * 2},
	.data_size = sizeof(sleeve_pixels),
	.data = (const uint8_t *)sleeve_pixels,
};

static struct {
	bool active, moved, have_angle;
	lv_point_t start;
	float angle, travel;
} touch;

static int scale(int value) { return (value * scale_w + DESIGN_W / 2) / DESIGN_W; }

static void place(lv_obj_t *obj, int x, int y, int w, int h) {
	lv_obj_set_pos(obj, scale(x), scale(y));
	lv_obj_set_size(obj, scale(w), scale(h));
}

static lv_obj_t *panel(lv_obj_t *parent, uint32_t color) {
	lv_obj_t *obj = lv_obj_create(parent);
	lv_obj_remove_style_all(obj);
	lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
	lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
	lv_obj_set_clickable(obj, false);
	lv_obj_set_scrollable(obj, false);
	return obj;
}

static void gradient_to(lv_obj_t *obj, lv_grad_dir_t direction, uint32_t color) {
	lv_obj_set_style_bg_grad_dir(obj, direction, 0);
	lv_obj_set_style_bg_grad_color(obj, lv_color_hex(color), 0);
}

static lv_obj_t *lcd_label(lv_obj_t *parent, int x, int y, int w, const lv_font_t *font, lv_text_align_t align) {
	lv_obj_t *label = lv_label_create(parent);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, lv_color_hex(LCD_INK), 0);
	lv_obj_set_style_text_align(label, align, 0);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_pos(label, scale(x), scale(y));
	lv_obj_set_width(label, scale(w));
	lv_obj_set_height(label, lv_font_get_line_height(font));
	return label;
}

static lv_obj_t *glyph(lv_obj_t *parent, const lv_image_dsc_t *src, int size, uint32_t ink) {
	lv_obj_t *image = lv_image_create(parent);
	lv_obj_set_size(image, scale(size), scale(size));
	lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CONTAIN);
	lv_image_set_src(image, src);
	lv_obj_set_style_image_recolor(image, lv_color_hex(ink), 0);
	lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
	return image;
}

static void skip_glyph_cb(lv_event_t *e) {
	bool previous = lv_event_get_user_data(e) != NULL;
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_area_t area;
	lv_obj_get_coords(lv_event_get_target(e), &area);
	int direction = previous ? -1 : 1;
	int origin = previous ? area.x2 : area.x1;
	int middle = (area.y1 + area.y2) / 2;
	lv_color_t ink = lv_color_hex(WHEEL_INK);
	for (int i = 0; i < 2; i++) {
		int base = origin + direction * scale(2 + 11 * i);
		int tip = base + direction * scale(11);
		lv_draw_triangle_dsc_t draw;
		lv_draw_triangle_dsc_init(&draw);
		draw.color = ink;
		draw.opa = LV_OPA_COVER;
		draw.p[0] = (lv_point_precise_t){base, area.y1};
		draw.p[1] = (lv_point_precise_t){base, area.y2};
		draw.p[2] = (lv_point_precise_t){tip, middle};
		lv_draw_triangle(layer, &draw);
	}
	lv_draw_rect_dsc_t stop;
	lv_draw_rect_dsc_init(&stop);
	stop.bg_color = ink;
	stop.bg_opa = LV_OPA_COVER;
	lv_area_t bar = area;
	if (previous) {
		bar.x2 = bar.x1 + scale(3) - 1;
	} else {
		bar.x1 = bar.x2 - scale(3) + 1;
	}
	lv_draw_rect(layer, &stop, &bar);
}

static void set_label(lv_obj_t *label, const char *value) {
	if (strcmp(lv_label_get_text(label), value)) {
		lv_label_set_text(label, value);
	}
}

static void set_clock(lv_obj_t *label, unsigned seconds, bool negative) {
	char time[24];
	const char *sign = negative ? "-" : "";
	if (seconds >= 3600) {
		snprintf(time, sizeof(time), "%s%u:%02u:%02u", sign, seconds / 3600, seconds / 60 % 60, seconds % 60);
	} else {
		snprintf(time, sizeof(time), "%s%u:%02u", sign, seconds / 60, seconds % 60);
	}
	set_label(label, time);
}

void ipod_mode_release_artwork(void) {
	if (displayed_art) {
		displayed_art = NULL;
		lv_obj_set_hidden(cover, true);
		lv_obj_set_hidden(placeholder, false);
	}
}

// Project the sleeve as a plane turned away on the right. The reflection
// shares its vanishing point and starts at the foot of each column.
static bool render_sleeve(const lv_image_dsc_t *art) {
	if (!art || !art->data || art->header.cf != LV_COLOR_FORMAT_RGB565 ||
		!art->header.w || !art->header.h || art->header.stride < art->header.w * 2) {
		return false;
	}
	int width = art->header.w, height = art->header.h;
	int edge = LV_MAX(width, height);
	int w = LV_MAX(1, width * SLEEVE_SIZE / edge);
	int h = LV_MAX(1, height * SLEEVE_SIZE / edge);
	int left = (SLEEVE_SIZE - w) / 2;
	uint16_t paper = lv_color_to_u16(lv_color_hex(LCD_BG));
	const uint16_t *source = (const uint16_t *)art->data;
	int stride = art->header.stride / 2;
	for (size_t i = 0; i < sizeof(sleeve_pixels) / sizeof(*sleeve_pixels); i++) {
		sleeve_pixels[i] = paper;
	}
	int span = SLEEVE_SIZE - 1;
	int full_scale = SLEEVE_SIZE * span;
	for (int x = 0; x < SLEEVE_SIZE; x++) {
		int depth = full_scale - (SLEEVE_SIZE - SLEEVE_FAR_HEIGHT) * x;
		int plane_x = (x * SLEEVE_FAR_HEIGHT * span + depth / 2) / depth;
		if (plane_x < left || plane_x >= left + w) {
			continue;
		}
		int column_h = LV_MAX(1, (h * depth + full_scale / 2) / full_scale);
		int top = (SLEEVE_SIZE - column_h) / 2;
		int sx = (plane_x - left) * width / w;
		for (int y = 0; y < column_h; y++) {
			sleeve_pixels[(top + y) * SLEEVE_SIZE + x] = source[(y * height / column_h) * stride + sx];
		}
		int reflection_top = top + column_h;
		int fade_h = (REFLECTION_H * depth + full_scale / 2) / full_scale;
		for (int y = 0; y < LV_MIN(fade_h, column_h); y++) {
			uint8_t opacity = 84 * (fade_h - 1 - y) / (fade_h - 1);
			int src_y = (column_h - 1 - y) * height / column_h;
			uint16_t color = source[src_y * stride + sx];
			sleeve_pixels[(reflection_top + y) * SLEEVE_SIZE + x] = lv_color_16_16_mix(color, paper, opacity);
		}
	}
	return true;
}

static void update_display(lv_timer_t *timer) {
	(void)timer;
	device_state_t state;
	device_state_get(&state);
	const char *track = state.metadata.title;
	if (!track[0] && state.current_file[0]) {
		const char *slash = strrchr(state.current_file, '/');
		track = slash ? slash + 1 : state.current_file;
	}
	set_label(title, track[0] ? track : tr("player_no_track_loaded"));
	set_label(artist, metadata_shown_artist(&state.metadata));
	set_label(album, state.metadata.album);

	const lv_image_dsc_t *art = player_cover_image();
	if (displayed_art != art) {
		displayed_art = art;
		bool drawn = render_sleeve(art);
		lv_obj_set_hidden(placeholder, drawn);
		lv_obj_set_hidden(cover, !drawn);
		lv_obj_invalidate(cover);
	}

	const lv_image_dsc_t *status = state.status == AUDIO_STATUS_PLAYING ? &icon_play : &icon_pause;
	if (lv_image_get_src(status_icon) != status) {
		lv_image_set_src(status_icon, status);
	}
	const lv_image_dsc_t *mode = NULL;
	switch (playlist_get_mode()) {
	case PLAYBACK_MODE_SHUFFLE:
	case PLAYBACK_MODE_SHUFFLE_REPEAT: mode = &icon_shuffle; break;
	case PLAYBACK_MODE_REPEAT_ONE: mode = &icon_repeat_one; break;
	case PLAYBACK_MODE_REPEAT_ALL: mode = &icon_repeat_all; break;
	default: break;
	}
	if (lv_image_get_src(mode_icon) != mode) {
		lv_image_set_src(mode_icon, mode);
	}
	lv_obj_set_hidden(mode_icon, mode == NULL);

	char *end;
	long percent = strtol(state.battery_percent, &end, 10);
	int battery_w = 0;
	if (end != state.battery_percent && percent > 0 && percent <= 100) {
		battery_w = LV_MAX(1, scale(24) * (int)percent / 100);
	}
	lv_obj_set_width(battery_fill, battery_w);
	lv_obj_set_hidden(battery_fill, battery_w == 0);

	double total = fmax(0, state.progress_total_secs);
	double current = total > 0 ? fmin(total, fmax(0, state.progress_current_secs)) : 0;
	set_clock(elapsed, (unsigned)current, false);
	set_clock(remaining, (unsigned)ceil(total - current), true);

	char count_text[32] = "";
	int position_number, queue_count;
	int value = total > 0 ? (int)(current * 1000 / total) : 0;
	volume_shown = volume_shown && lv_tick_elaps(volume_at) < VOLUME_SHOW_MS;
	if (volume_shown) {
		snprintf(count_text, sizeof(count_text), "%s %ld%%", tr("volume"), state.volume);
		value = (int)state.volume * 10;
	} else if (player_queue_position(&position_number, &queue_count)) {
		snprintf(count_text, sizeof(count_text), tr("%d of %d"), position_number, queue_count);
	} else if (state.current_file[0] && !state.live) {
		snprintf(count_text, sizeof(count_text), tr("%d of %d"), 1, 1);
	}
	set_label(position, count_text);
	int fill = scale(BAR_W) * LV_CLAMP(0, value, 1000) / 1000;
	lv_obj_set_width(bar_fill, fill);
	lv_obj_set_hidden(bar_fill, fill == 0);
}

typedef enum { WHEEL_NONE, WHEEL_MENU, WHEEL_PREV, WHEEL_NEXT, WHEEL_PLAY_PAUSE } wheel_action_t;

static bool in_ring(int x, int y) {
	int distance = x * x + y * y;
	int inner = scale(WHEEL_HUB / 2), outer = scale(WHEEL_SIZE / 2);
	return distance >= inner * inner && distance <= outer * outer;
}

static wheel_action_t wheel_action(int x, int y) {
	if (!in_ring(x, y)) return WHEEL_NONE;
	if (abs(x) > abs(y)) return x < 0 ? WHEEL_PREV : WHEEL_NEXT;
	return y < 0 ? WHEEL_MENU : WHEEL_PLAY_PAUSE;
}

static int wheel_steps(float angle) {
	float delta = angle - touch.angle;
	touch.angle = angle;
	if (delta > PI) delta -= 2 * PI;
	if (delta < -PI) delta += 2 * PI;
	touch.travel += delta;
	int steps = (int)(touch.travel / WHEEL_STEP_RADIANS);
	touch.travel -= steps * WHEEL_STEP_RADIANS;
	return steps;
}

static bool moved_from_start(lv_point_t point) {
	int dx = point.x - touch.start.x, dy = point.y - touch.start.y;
	int slop = scale(WHEEL_TAP_SLOP);
	return dx * dx + dy * dy >= slop * slop;
}

static void wheel_event(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	if (code == LV_EVENT_PRESS_LOST) {
		touch.active = false;
		return;
	}
	lv_indev_t *indev = lv_indev_active();
	if (!indev) return;
	lv_point_t p;
	lv_indev_get_point(indev, &p);
	lv_area_t area;
	lv_obj_get_coords(wheel, &area);
	p.x -= area.x1 + lv_obj_get_width(wheel) / 2;
	p.y -= area.y1 + lv_obj_get_height(wheel) / 2;
	bool ring = in_ring(p.x, p.y);

	if (code == LV_EVENT_PRESSED) {
		touch.active = touch.have_angle = ring;
		touch.moved = false;
		touch.start = p;
		touch.angle = ring ? atan2f(p.y, p.x) : 0;
		touch.travel = 0;
	} else if (touch.active && code == LV_EVENT_PRESSING) {
		if (!ring) {
			// Lifting off the ring and re-entering must never jump the volume.
			touch.have_angle = false;
			touch.moved = true;
			touch.travel = 0;
			return;
		}
		if (!touch.moved && !moved_from_start(p)) return;
		touch.moved = true;
		float angle = atan2f(p.y, p.x);
		int steps = touch.have_angle ? wheel_steps(angle) : 0;
		touch.angle = angle;
		touch.have_angle = true;
		if (steps) {
			device_state_change_volume_percent(steps);
			topbar_refresh_volume(device_state_get_volume_percent());
			volume_at = lv_tick_get();
			volume_shown = true;
			update_display(NULL);
		}
	} else if (touch.active && code == LV_EVENT_RELEASED) {
		touch.active = false;
		wheel_action_t action = wheel_action(p.x, p.y);
		if (touch.moved || moved_from_start(p) || action != wheel_action(touch.start.x, touch.start.y)) return;
		switch (action) {
		case WHEEL_MENU: back_btn_cb(NULL); return;
		case WHEEL_PREV: player_screen_prev(); break;
		case WHEEL_NEXT: player_screen_next(); break;
		case WHEEL_PLAY_PAUSE: player_key_play_pause(); break;
		default: return;
		}
		volume_shown = false;
		lv_timer_ready(display_timer);
	}
}

static void bar_bands(lv_obj_t *bar, uint32_t top, uint32_t highlight, uint32_t waist, uint32_t bottom) {
	lv_obj_t *upper = panel(bar, top);
	lv_obj_set_size(upper, lv_pct(100), scale(10));
	gradient_to(upper, LV_GRAD_DIR_VER, highlight);
	lv_obj_t *lower = panel(bar, waist);
	lv_obj_set_y(lower, scale(10));
	lv_obj_set_size(lower, lv_pct(100), scale(BAR_H) - scale(10));
	gradient_to(lower, LV_GRAD_DIR_VER, bottom);
}

static void build_lcd(void) {
	lv_obj_t *lcd = panel(screen, LCD_BG);
	lv_obj_set_size(lcd, lv_pct(100), scale(LCD_H));
	lv_obj_t *header = panel(lcd, 0xf2f4f5);
	lv_obj_set_size(header, lv_pct(100), scale(LCD_HEADER_H));
	gradient_to(header, LV_GRAD_DIR_VER, 0xa8afb1);
	lv_obj_t *rule = panel(header, 0x81827b);
	lv_obj_set_size(rule, lv_pct(100), 1);
	lv_obj_align(rule, LV_ALIGN_BOTTOM_MID, 0, 0);

	lv_obj_t *heading = lcd_label(header, 14, 4, 350, &font_ui_20_bold, LV_TEXT_ALIGN_LEFT);
	lv_label_set_text(heading, tr("musicsettings_now_playing"));
	status_icon = glyph(header, &icon_pause, 16, 0x217c9d);
	lv_obj_align(status_icon, LV_ALIGN_TOP_RIGHT, -scale(45), scale(9));
	lv_obj_t *battery = panel(header, 0x9da596);
	lv_obj_set_size(battery, scale(28), scale(15));
	lv_obj_align(battery, LV_ALIGN_TOP_RIGHT, -scale(14), scale(9));
	lv_obj_set_style_border_width(battery, 1, 0);
	lv_obj_set_style_border_opa(battery, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(battery, lv_color_hex(0x65735e), 0);
	lv_obj_t *terminal = panel(header, 0x65735e);
	lv_obj_set_size(terminal, scale(3), scale(7));
	lv_obj_align(terminal, LV_ALIGN_TOP_RIGHT, -scale(11), scale(13));
	battery_fill = panel(battery, 0x6ca450);
	place(battery_fill, 1, 1, 24, 11);

	lv_obj_t *content = panel(lcd, LCD_BG);
	lv_obj_set_size(content, scale_w, scale(LCD_H - LCD_HEADER_H));
	lv_obj_align(content, LV_ALIGN_TOP_MID, 0, scale(LCD_HEADER_H));
	mode_icon = glyph(content, NULL, 21, LCD_INK);
	lv_obj_align(mode_icon, LV_ALIGN_TOP_RIGHT, -scale(18), scale(8));
	lv_obj_set_hidden(mode_icon, true);

	placeholder = panel(content, 0xe0e1df);
	place(placeholder, 24, 26, SLEEVE_SIZE, SLEEVE_SIZE);
	lv_obj_center(glyph(placeholder, &icon_music_note, 50, 0x777c75));
	cover = lv_image_create(content);
	place(cover, 24, 26, SLEEVE_SIZE, SLEEVE_IMAGE_H);
	lv_image_set_inner_align(cover, LV_IMAGE_ALIGN_STRETCH);
	lv_image_set_src(cover, &sleeve_image);
	lv_obj_set_hidden(cover, true);

	title = lcd_label(content, 224, 66, 236, &font_ui_24_bold, LV_TEXT_ALIGN_LEFT);
	artist = lcd_label(content, 224, 101, 236, &font_ui_20, LV_TEXT_ALIGN_LEFT);
	album = lcd_label(content, 224, 128, 236, &font_ui_20, LV_TEXT_ALIGN_LEFT);
	position = lcd_label(content, 224, 179, 236, &font_ui_20_bold, LV_TEXT_ALIGN_LEFT);

	lv_obj_t *track = panel(content, 0xe6e8e9);
	place(track, 88, 274, BAR_W, BAR_H);
	bar_bands(track, 0xc1c7ca, 0xf2f3f4, 0xe2e5e7, 0xd4d8da);
	bar_fill = panel(track, 0x75b3e9);
	lv_obj_set_size(bar_fill, 0, lv_pct(100));
	bar_bands(bar_fill, 0x9bc1f0, 0x5087ce, 0x3d95d3, 0x62d4f6);
	lv_obj_set_hidden(bar_fill, true);
	elapsed = lcd_label(content, 13, 274, 70, &font_ui_18, LV_TEXT_ALIGN_LEFT);
	remaining = lcd_label(content, 395, 274, 70, &font_ui_18, LV_TEXT_ALIGN_RIGHT);
}

static uint16_t shell_shade(uint16_t ink, int distance, int bevel, int x, int y) {
	// The shell's dark colours occupy only a few levels of RGB565. Ordered
	// dithering makes the longer fade gradual between those levels.
	static const uint8_t threshold[4][4] = {
		{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5},
	};
	unsigned t = 256 * LV_MIN(distance, bevel - 1) / (bevel - 1);
	unsigned weight = t * t * (768 - 2 * t) >> 16;
	unsigned bias = 16 * threshold[y % 4][x % 4] + 8;
	unsigned r = ((ink >> 11) * weight + bias) >> 8;
	unsigned g = (((ink >> 5) & 63) * weight + bias) >> 8;
	unsigned b = ((ink & 31) * weight + bias) >> 8;
	return (uint16_t)((r << 11) | (g << 5) | b);
}

static void build_lower_shell(gui_config_t *cfg) {
	lv_obj_t *shell = panel(screen, SHELL_INK);
	int top = scale(LCD_H);
	int width = (int)cfg->screen_width, height = (int)cfg->screen_height - top;
	lv_obj_set_y(shell, top);
	lv_obj_set_size(shell, width, height);
	int limit = LV_MIN(SHELL_MAX_EDGE, LV_MIN(width / 2, height));
	int bevel = LV_MIN(scale(SHELL_BEVEL), limit);
	if (bevel < 2) {
		return;
	}
	int radius = LV_MIN(scale(SHELL_CORNER_RADIUS), limit);
	// Tile the straight edges. The bottom corners trace a round outline that
	// meets those edges tangentially; only the shading rounds the silhouette.
	const struct { int x, y, w, h, tex_w, tex_h; } regions[] = {
		{0, 0, bevel, height - radius, bevel, SHELL_TILE},
		{width - bevel, 0, bevel, height - radius, bevel, SHELL_TILE},
		{radius, height - bevel, width - 2 * radius, bevel, SHELL_TILE, bevel},
		{0, height - radius, radius, radius, radius, radius},
		{width - radius, height - radius, radius, radius, radius, radius},
	};
	uint16_t *pixels = shell_pixels;
	uint16_t ink = lv_color_to_u16(lv_color_hex(SHELL_INK));
	for (size_t i = 0; i < sizeof(regions) / sizeof(*regions); i++) {
		int ox = regions[i].x, oy = regions[i].y;
		int w = LV_MIN(regions[i].tex_w, regions[i].w);
		int h = LV_MIN(regions[i].tex_h, regions[i].h);
		if (!w || !h) {
			continue;
		}
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				int px = ox + x, py = oy + y;
				int side = LV_MIN(px, width - 1 - px);
				int bottom = height - 1 - py;
				int distance = LV_MIN(side, bottom);
				if (side < radius && bottom < radius) {
					int dx = radius - 1 - side, dy = radius - 1 - bottom;
					distance = LV_MAX(0, radius - 1 - (int)(sqrtf((float)(dx * dx + dy * dy)) + .5f));
				}
				pixels[y * w + x] = shell_shade(ink, distance, bevel, px, py);
			}
		}
		shell_images[i] = (lv_image_dsc_t){
			.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .w = w, .h = h, .stride = w * 2},
			.data_size = (uint32_t)(w * h * 2),
			.data = (const uint8_t *)pixels,
		};
		lv_obj_t *edge = lv_image_create(shell);
		lv_obj_set_pos(edge, ox, oy);
		lv_obj_set_size(edge, regions[i].w, regions[i].h);
		lv_image_set_inner_align(edge, LV_IMAGE_ALIGN_TILE);
		lv_image_set_src(edge, &shell_images[i]);
		pixels += w * h;
	}
}

static void build_wheel(void) {
	wheel = panel(screen, 0x242424);
	lv_obj_set_size(wheel, scale(WHEEL_SIZE), scale(WHEEL_SIZE));
	lv_obj_align(wheel, LV_ALIGN_BOTTOM_MID, 0, -scale(84));
	lv_obj_set_style_radius(wheel, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_clickable(wheel, true);
	lv_obj_set_press_lock(wheel, true);
	lv_obj_add_event_cb(wheel, wheel_event, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(wheel, wheel_event, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(wheel, wheel_event, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(wheel, wheel_event, LV_EVENT_PRESS_LOST, NULL);

	lv_obj_t *hub = panel(wheel, 0x111111);
	lv_obj_set_size(hub, scale(WHEEL_HUB), scale(WHEEL_HUB));
	lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
	lv_obj_center(hub);
	lv_obj_t *menu = lv_label_create(wheel);
	lv_label_set_text(menu, "MENU");
	lv_obj_set_style_text_color(menu, lv_color_hex(WHEEL_INK), 0);
	lv_obj_set_style_text_font(menu, &font_ui_20_bold, 0);
	lv_obj_set_style_text_letter_space(menu, 1, 0);
	lv_obj_align(menu, LV_ALIGN_TOP_MID, 0, scale(13));

	lv_obj_t *prev = panel(wheel, 0);
	lv_obj_t *next = panel(wheel, 0);
	lv_obj_set_style_bg_opa(prev, LV_OPA_TRANSP, 0);
	lv_obj_set_style_bg_opa(next, LV_OPA_TRANSP, 0);
	lv_obj_set_size(prev, scale(29), scale(16));
	lv_obj_set_size(next, scale(29), scale(16));
	lv_obj_align(prev, LV_ALIGN_LEFT_MID, scale(18), 0);
	lv_obj_align(next, LV_ALIGN_RIGHT_MID, -scale(18), 0);
	lv_obj_add_event_cb(prev, skip_glyph_cb, LV_EVENT_DRAW_MAIN, prev);
	lv_obj_add_event_cb(next, skip_glyph_cb, LV_EVENT_DRAW_MAIN, NULL);
	lv_obj_t *play = glyph(wheel, &icon_play, 21, WHEEL_INK);
	lv_obj_t *pause = glyph(wheel, &icon_pause, 21, WHEEL_INK);
	lv_obj_align(play, LV_ALIGN_BOTTOM_MID, -scale(11), -scale(19));
	lv_obj_align(pause, LV_ALIGN_BOTTOM_MID, scale(11), -scale(19));
}

static void loaded(lv_event_t *e) {
	(void)e;
	topbar_set_hidden(true);
	update_display(NULL);
	lv_timer_resume(display_timer);
}

static void unloaded(lv_event_t *e) {
	(void)e;
	touch.active = false;
	volume_shown = false;
	lv_timer_pause(display_timer);
	ipod_mode_release_artwork();
	back_btn_hide_on_screen(screen, false);
}

void ipod_mode_open(gui_config_t *cfg) {
	if (!screen) {
		scale_w = LV_MIN((int)cfg->screen_width, (int)cfg->screen_height * DESIGN_W / DESIGN_H);
		screen = lv_obj_create(NULL);
		lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
		lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
		lv_obj_set_scrollable(screen, false);
		build_lcd();
		build_lower_shell(cfg);
		build_wheel();
		display_timer = lv_timer_create(update_display, DISPLAY_POLL_MS, NULL);
		power_pause_in_standby(display_timer);
		lv_timer_pause(display_timer);
		lv_obj_add_event_cb(screen, loaded, LV_EVENT_SCREEN_LOADED, NULL);
		lv_obj_add_event_cb(screen, unloaded, LV_EVENT_SCREEN_UNLOADED, NULL);
	}
	player_sheet_close(false);
	back_btn_hide_on_screen(screen, true);
	switch_screen(screen);
	switcher_set_player_return(screen);
}
