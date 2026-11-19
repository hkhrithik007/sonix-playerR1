#include "visualiser.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/topbar.h"
#include "src/system/audio/visualiser_audio.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"

#define FRAME_MS 33
#define CONTROLS_MS 2500
#define CONTROLS_FADE_MS 250
#define STEPS 32
#define STRANDS 7
#define GLOW_RIBBONS 3
#define GLOW_LEVELS 5
#define SPARK_COUNT 72
#define TUNNEL_RINGS 5
#define TUNNEL_STEPS 32
#define SUBPIXEL_BITS 8
#define SUBPIXEL (1 << SUBPIXEL_BITS)
#define SINE_SIZE 256
#define TWO_PI 6.28318530718f
#define TUNING_STEP 25

static lv_obj_t *screen;
static lv_obj_t *image;
static lv_obj_t *close_button;
static lv_obj_t *settings_button;
static lv_obj_t *settings_veil;
static lv_timer_t *timer;
static lv_image_dsc_t picture;
static uint16_t *pixels;
static int width, height;
static uint32_t controls_at;
static uint32_t frame_at;
static float sine_table[SINE_SIZE];
static float reach;
static float phase, drift, twist, hue, beat, burst_angle, bass_floor, previous_low;
static float shape_phase, starting_hue = -1;
static float tunnel_travel;
static float bass, mid, treble;
static float target_bass, target_mid, target_treble, pulse;
static unsigned beat_cooldown, silent_frames;
static uint32_t random_state;

typedef struct {
	float distance, angle, speed, glow, age;
} spark_t;

static spark_t sparks[SPARK_COUNT];
static unsigned next_spark;
static float spark_budget;

typedef struct {
	const char *label;
	const char *key;
	bool enabled;
} visualiser_layer_t;

static visualiser_layer_t sharp_layer = {"Sharp ribbons", "sharp_ribbons", true};
static visualiser_layer_t sharp_antialias = {"Anti-alias sharp ribbons", "sharp_antialias", true};
static visualiser_layer_t soft_layer = {"Soft ribbons", "soft_ribbons", true};
static visualiser_layer_t spark_layer = {"Sparks", "sparks", true};
static visualiser_layer_t tunnel_layer = {"Tunnel", "tunnel", true};

typedef struct {
	const char *label;
	const char *key;
	int minimum;
	int percent;
	lv_obj_t *value;
} visualiser_tuning_t;

static visualiser_tuning_t motion_speed = {"Motion speed", "speed", 50, 100, NULL};
static visualiser_tuning_t music_response = {"Music response", "response", 0, 100, NULL};
static bool settings_dirty;

// A table keeps trigonometry out of the inner drawing loop on the R1. Angles
// are measured in turns so a quarter turn also gives cosine.
static float wave(float turns) {
	float index = turns * SINE_SIZE;
	int base = (int)index;
	if (index < base) {
		base--;
	}
	unsigned slot = (unsigned)base & (SINE_SIZE - 1);
	float fraction = index - base;
	return sine_table[slot] + (sine_table[(slot + 1) & (SINE_SIZE - 1)] - sine_table[slot]) * fraction;
}

static float random_unit(void) {
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return (random_state >> 8) * (1.0f / 16777216.0f);
}

static float next_starting_hue(void) {
	float sample = random_unit();
	if (starting_hue < 0) {
		starting_hue = sample;
	} else {
		// Leave a gap around the last opening colour so a new palette is
		// apparent even when the next random sample would be very close.
		starting_hue += .125f + .75f * sample;
		if (starting_hue >= 1) starting_hue -= 1;
	}
	return starting_hue;
}

static float follow(float current, float target, float elapsed, float rise, float fall) {
	float smoothing = elapsed / (elapsed + (target > current ? rise : fall));
	return current + (target - current) * smoothing;
}

static void read_levels(uint32_t now) {
	if (beat_cooldown > 0) {
		beat_cooldown--;
	}

	visualiser_audio_frame_t frame;
	float low = 0, middle = 0, high = 0;
	if (visualiser_audio_take(&frame, now)) {
		silent_frames = 0;
		float a = 1.0f - expf(-TWO_PI * 170.0f / frame.rate);
		float b = 1.0f - expf(-TWO_PI * 2100.0f / frame.rate);
		float slow = 0, fast = 0;
		for (size_t i = 0; i < frame.count; i++) {
			float sample = frame.samples[i] / 32768.0f;
			slow += a * (sample - slow);
			fast += b * (sample - fast);
			low += slow * slow;
			middle += (fast - slow) * (fast - slow);
			high += (sample - fast) * (sample - fast);
		}
		low = fminf(sqrtf(low / frame.count) * 5.6f, 1.0f);
		middle = fminf(sqrtf(middle / frame.count) * 5.6f, 1.0f);
		high = fminf(sqrtf(high / frame.count) * 10.0f, 1.0f);
	} else if (silent_frames < 4) {
		// Audio can arrive in 90 ms writes, with a drawing tick in between.
		// Hold the last levels briefly so a gap does not invent a bass hit.
		silent_frames++;
		return;
	}
	target_bass = low;
	target_mid = middle;
	target_treble = high;
	bass_floor += (low - bass_floor) * .075f;

	// React to the leading edge of a bass hit. A held note feeds a ribbon
	// without repeatedly firing a burst.
	float attack = low - previous_low;
	if (!beat_cooldown && low > .095f && attack > .055f && low > bass_floor * 1.18f) {
		beat = fminf(.30f + low * .52f + attack, 1.0f);
		burst_angle = random_unit();
		beat_cooldown = 6;
	}
	previous_low = low;
}

static void advance_motion(float elapsed) {
	// PCM arrives in batches. Ease toward the latest levels on every drawing
	// tick so a new batch doesn't suddenly bend or stretch the whole picture.
	float response = music_response.percent * .01f;
	bass = follow(bass, fminf(target_bass * response, 1), elapsed, 60, 145);
	mid = follow(mid, fminf(target_mid * response, 1), elapsed, 95, 180);
	treble = follow(treble, fminf(target_treble * response, 1), elapsed, 75, 150);
	pulse = follow(pulse, fminf(beat * response, 1), elapsed, 52, 100);
	beat *= 125.0f / (125.0f + elapsed);

	float step = elapsed / FRAME_MS * (motion_speed.percent * .01f);
	phase += (.0026f + .0045f * mid + .0030f * pulse) * step;
	drift += (.0012f + .0011f * bass) * step;
	twist += (.0019f + .0014f * treble) * step;
	shape_phase += (.0032f + .0013f * mid) * step;
	tunnel_travel += (.017f + .009f * bass + .008f * pulse) * step;
	hue += elapsed / 48000.0f;
	if (phase >= 1) phase -= 1;
	if (drift >= 1) drift -= 1;
	if (twist >= 1) twist -= 1;
	if (shape_phase >= 1) shape_phase -= 1;
	if (hue >= 1) hue -= 1;
	if (tunnel_travel >= 1) tunnel_travel -= 1;
}

static unsigned rgb565(unsigned r, unsigned g, unsigned b) {
	return (r << 11) | (g << 5) | b;
}

static uint16_t filament_color(float tone, float saturation, float level) {
	tone -= floorf(tone);
	lv_color_t rgb = lv_color_hsv_to_rgb((uint16_t)(tone * 360), (uint8_t)(saturation * 100),
										 (uint8_t)(level * 100));
	return lv_color_to_u16(rgb);
}

static void blend_pixel(uint16_t *dst, uint16_t ink) {
	unsigned old = *dst;
	unsigned r = (old >> 11) + (ink >> 11);
	unsigned g = ((old >> 5) & 63) + ((ink >> 5) & 63);
	unsigned b = (old & 31) + (ink & 31);
	*dst = (uint16_t)rgb565(r > 31 ? 31 : r, g > 63 ? 63 : g, b > 31 ? 31 : b);
}

static void point(int x, int y, uint16_t ink) {
	if ((unsigned)x < (unsigned)width && (unsigned)y < (unsigned)height) {
		blend_pixel(&pixels[y * width + x], ink);
	}
}

static void line(int x, int y, int end_x, int end_y, uint16_t ink) {
	if ((x < 0 && end_x < 0) || (x >= width && end_x >= width) ||
		(y < 0 && end_y < 0) || (y >= height && end_y >= height)) {
		return;
	}
	int dx = abs(end_x - x), dy = -abs(end_y - y);
	int sx = x < end_x ? 1 : -1;
	int sy = y < end_y ? 1 : -1;
	int error = dx + dy;
	while (x != end_x || y != end_y) {
		point(x, y, ink);
		int twice = 2 * error;
		if (twice >= dy) {
			error += dy;
			x += sx;
		}
		if (twice <= dx) {
			error += dx;
			y += sy;
		}
	}
}

typedef struct {
	int x, y; // framebuffer coordinates with 8 fractional bits
} subpixel_point_t;

static void point_coverage(bool steep, int axis, int across, uint16_t ink, unsigned coverage) {
	unsigned color = rgb565(((ink >> 11) * coverage + SUBPIXEL / 2) >> SUBPIXEL_BITS,
							(((ink >> 5) & 63) * coverage + SUBPIXEL / 2) >> SUBPIXEL_BITS,
							((ink & 31) * coverage + SUBPIXEL / 2) >> SUBPIXEL_BITS);
	if (color) {
		point(steep ? across : axis, steep ? axis : across, (uint16_t)color);
	}
}

// Walk the dominant axis and divide the line's light between its two nearest
// pixels. Fractional endpoints keep adjoining segments from drawing a full
// pixel twice. Everything blends into the existing RGB565 trails.
static void line_aa(subpixel_point_t start, subpixel_point_t end, uint16_t ink) {
	bool steep = abs(end.y - start.y) > abs(end.x - start.x);
	int a = steep ? start.y : start.x, b = steep ? end.y : end.x;
	int u = steep ? start.x : start.y, v = steep ? end.x : end.y;
	int limit = steep ? height : width;
	int across_limit = steep ? width : height;
	if (!ink || a == b || (u <= -SUBPIXEL && v <= -SUBPIXEL) ||
		(u >= across_limit * SUBPIXEL && v >= across_limit * SUBPIXEL)) {
		return;
	}
	if (a > b) {
		int tmp = a;
		a = b;
		b = tmp;
		tmp = u;
		u = v;
		v = tmp;
	}
	int first = (a + SUBPIXEL / 2) >> SUBPIXEL_BITS;
	int last = (b + SUBPIXEL / 2) >> SUBPIXEL_BITS;
	if (last < 0 || first >= limit) {
		return;
	}
	if (first < 0) first = 0;
	if (last >= limit) last = limit - 1;

	int slope = (v - u) * SUBPIXEL / (b - a);
	int minor = u * SUBPIXEL + slope * (first * SUBPIXEL - a);
	for (int i = first; i <= last; i++, minor += slope * SUBPIXEL) {
		int center = i * SUBPIXEL;
		int left = center - SUBPIXEL / 2;
		int right = center + SUBPIXEL / 2;
		if (left < a) left = a;
		if (right > b) right = b;
		unsigned weight = (unsigned)(right - left);
		int sample = (minor + slope * ((right + left) / 2 - center)) >> SUBPIXEL_BITS;
		int cross = sample >> SUBPIXEL_BITS;
		unsigned fraction = (unsigned)sample & (SUBPIXEL - 1);
		unsigned upper = (fraction * weight + SUBPIXEL / 2) >> SUBPIXEL_BITS;
		point_coverage(steep, i, cross, ink, weight - upper);
		point_coverage(steep, i, cross + 1, ink, upper);
	}
}

static void fade_trails(void) {
	for (int i = 0; i < width * height; i++) {
		unsigned p = pixels[i];
		pixels[i] = (uint16_t)rgb565((p >> 11) * 15 >> 4, ((p >> 5) & 63) * 15 >> 4, (p & 31) * 15 >> 4);
	}
}

static subpixel_point_t polar_point(float distance, float angle) {
	float radius = distance * reach * SUBPIXEL;
	return (subpixel_point_t){
		.x = width / 2 * SUBPIXEL + (int)(radius * wave(angle + .25f)),
		.y = height / 2 * SUBPIXEL + (int)(radius * wave(angle)),
	};
}

static void draw_tunnel(void) {
	// Equally spaced cross-sections in depth grow faster as they approach
	// the viewer. Both ends fade so recycling a section never flashes.
	for (int i = TUNNEL_RINGS; i > 0; i--) {
		float depth = i - tunnel_travel;
		float distance = .68f / (.50f + depth);
		float fade = fminf(1, fminf(depth * 3.4f, (TUNNEL_RINGS - depth) * 1.6f));
		float turn = phase + .10f * wave(twist) - .085f * depth;
		int offset_x = (int)(reach * SUBPIXEL * .105f * wave(drift + depth * .12f));
		int offset_y = (int)(reach * SUBPIXEL * .076f * wave(drift + depth * .09f + .25f));
		uint16_t ink[3];
		for (int j = 0; j < 3; j++) {
			float level = fade * (.17f + .06f * bass) * (.58f + .21f * j);
			ink[j] = filament_color(hue + .07f + .044f * depth, .64f, level);
		}
		subpixel_point_t previous = {0};
		for (int j = 0; j <= TUNNEL_STEPS; j++) {
			float angle = (float)j / TUNNEL_STEPS;
			float shape = 1 + .17f * wave(2 * angle + twist) + .065f * wave(3 * angle + drift);
			subpixel_point_t p = polar_point(distance * shape, angle + turn);
			p.x += offset_x;
			p.y += offset_y;
			if (j > 0) {
				// Uneven illumination lets pieces of each cross-section
				// surface through the ribbons without dominating them.
				float light = wave(angle + drift + .19f * depth);
				int shade = light > .15f ? 2 : (light < -.5f ? 0 : 1);
				line_aa(previous, p, ink[shade]);
			}
			previous = p;
		}
	}
}

// Stamp a feathered brush directly into the RGB565 trails. This gives the
// broad layer a soft edge without a second framebuffer or a full-screen blur.
static void glow_dab(int x, int y, int radius, const uint16_t ink[GLOW_LEVELS]) {
	if (x + radius < 0 || x - radius >= width || y + radius < 0 || y - radius >= height) {
		return;
	}
	int r2 = radius * radius;
	unsigned scale = GLOW_LEVELS * 1024 / r2;
	int left = x - radius < 0 ? -x : -radius;
	int right = x + radius >= width ? width - 1 - x : radius;
	int top = y - radius < 0 ? -y : -radius;
	int bottom = y + radius >= height ? height - 1 - y : radius;
	for (int dy = top; dy <= bottom; dy++) {
		uint16_t *dst = &pixels[(y + dy) * width + x + left];
		for (int dx = left; dx <= right; dx++, dst++) {
			int d2 = dx * dx + dy * dy;
			if (d2 < r2) {
				uint16_t color = ink[(d2 * scale) >> 10];
				if (color) {
					blend_pixel(dst, color);
				}
			}
		}
	}
}

static void glow_segment(int x, int y, int nx, int ny, int radius, const uint16_t ink[GLOW_LEVELS]) {
	int dx = nx - x, dy = ny - y;
	int length = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
	int stamps = length / (radius * 2 / 3 + 1) + 1;
	for (int i = 0; i < stamps; i++) {
		glow_dab(x + dx * i / stamps, y + dy * i / stamps, radius, ink);
	}
}

static float zigzag(float position, float peak, float rise, float fall) {
	float fraction = position - (int)position;
	return (fraction < peak ? fraction * rise : (1 - fraction) * fall) - 1;
}

static void draw_ribbons(bool soft) {
	// Each layer has independently drifting bends and unevenly spaced lanes.
	// Paths can reverse their curl while the whole field keeps moving outwards.
	float bend[STEPS + 1], flare[STEPS + 1], ripple[STEPS + 1];
	for (int i = 0; i <= STEPS; i++) {
		float t = (float)i / STEPS;
		float curl = (.31f + .48f * wave(drift) + .19f * mid) * t;
		bend[i] = curl + (.16f + .17f * mid) * t * wave(t * 1.23f + twist);
		flare[i] = wave(t * .5f);
		ripple[i] = t * wave(t * 2.45f - twist * 2) * (.011f + treble * .024f);
	}

	int count = soft ? GLOW_RIBBONS : STRANDS;
	for (int strand = 0; strand < count; strand++) {
		float lane = ((float)strand + (soft ? .58f : 0)) / count;
		float base = lane + phase + .16f * wave(lane * 2 + drift);
		float fold = wave(lane * 4 + twist);
		float lean = wave(lane * 3 - drift);
		// Each ribbon eases into a different zigzag. The corners travel
		// outward with the ripple, and the uneven teeth drift in shape.
		float contour = fmaxf(0, wave(shape_phase + .56f * lane + (soft ? .12f : 0)));
		float zigzag_mix = contour * contour;
		float teeth = 2.65f + .75f * lean;
		float tooth_phase = 3 + lane - 2 * twist;
		float peak = .48f + .18f * fold;
		float rise = 2 / peak, fall = 2 / (1 - peak);
		float zigzag_depth = (soft ? .039f : .052f) + .010f * treble;
		float reach_scale = 1.04f + .17f * lean + .10f * pulse;
		float tone = hue + .27f * lane + (soft ? 0 : .07f);
		uint16_t halo[GLOW_LEVELS];
		uint16_t core = 0;
		if (soft) {
			float glow = .45f + .20f * bass + .22f * pulse;
			for (int level = 0; level < GLOW_LEVELS; level++) {
				float opacity = 1 - (float)level / GLOW_LEVELS;
				halo[level] = filament_color(tone, .85f, glow * opacity * opacity * .42f);
			}
		} else {
			core = filament_color(tone, .72f, .30f + .24f * mid + .17f * pulse);
		}

		// Stagger the starts so the crossing paths never draw a fixed centre
		// emblem. Wider strokes open up farther from the centre.
		float start = (soft ? .065f : .013f) + (soft ? .15f : .095f) * (.5f + .5f * fold);
		subpixel_point_t previous = polar_point(start, base);
		for (int i = 1; i <= STEPS; i++) {
			float t = (float)i / STEPS;
			float distance = start + t * (reach_scale - start);
			float detail = ripple[i];
			if (zigzag_mix > 0) {
				float angular = t * zigzag_depth * zigzag(t * teeth + tooth_phase, peak, rise, fall);
				detail += zigzag_mix * (angular - detail);
			}
			float angle = base + bend[i] * (.72f + .52f * lean) +
						  fold * .17f * t * flare[i] + detail;
			subpixel_point_t p = polar_point(distance, angle);
			if (soft) {
				float spread = .5f + .5f * wave(lane * 2 + drift + t * .87f);
				int radius = 2 + (int)(flare[i] * (4 + 9 * spread + 6 * bass + 3 * pulse));
				glow_segment(previous.x >> SUBPIXEL_BITS, previous.y >> SUBPIXEL_BITS,
							 p.x >> SUBPIXEL_BITS, p.y >> SUBPIXEL_BITS, radius, halo);
			} else if (sharp_antialias.enabled) {
				line_aa(previous, p, core);
			} else {
				line(previous.x >> SUBPIXEL_BITS, previous.y >> SUBPIXEL_BITS,
					 p.x >> SUBPIXEL_BITS, p.y >> SUBPIXEL_BITS, core);
			}
			previous = p;
		}
	}
}

static void draw_sparks(float step) {
	spark_budget += (4 * treble + 5 * pulse) * step;
	int count = (int)spark_budget;
	spark_budget -= count;
	for (int i = 0; i < count; i++) {
		float angle = random_unit();
		if (pulse > .16f && (i & 1)) {
			angle = burst_angle + (angle - .5f) * .30f;
		}
		sparks[next_spark] = (spark_t){
			.distance = .022f + random_unit() * .13f,
			.angle = angle,
			.speed = .012f + random_unit() * .023f + .020f * pulse,
			.glow = .46f + random_unit() * .24f + .28f * treble,
		};
		next_spark = (next_spark + 1) % SPARK_COUNT;
	}
	for (unsigned i = 0; i < SPARK_COUNT; i++) {
		spark_t *spark = &sparks[i];
		if (spark->glow < .035f || spark->distance >= 1.13f) {
			continue;
		}
		subpixel_point_t start = polar_point(spark->distance, spark->angle);
		spark->distance += spark->speed * step;
		spark->angle += (.007f + .018f * mid * wave(spark->angle * 2 + phase)) * step;
		spark->age += step;
		subpixel_point_t end = polar_point(spark->distance, spark->angle);
		float intensity = spark->glow * fminf(spark->age * .33f, 1.0f);
		uint16_t ink = filament_color(hue + .18f + .15f * spark->angle, .60f, intensity);
		line(start.x >> SUBPIXEL_BITS, start.y >> SUBPIXEL_BITS,
			 end.x >> SUBPIXEL_BITS, end.y >> SUBPIXEL_BITS, ink);
		spark->glow *= 1.0f / (1.0f + .075f * step);
	}
}

static void set_controls_opacity(lv_opa_t opacity) {
	lv_obj_set_style_opa(close_button, opacity, 0);
	lv_obj_set_style_opa(settings_button, opacity, 0);
	lv_obj_set_hidden(close_button, opacity == LV_OPA_TRANSP);
	lv_obj_set_hidden(settings_button, opacity == LV_OPA_TRANSP);
}

static void reveal_controls(void) {
	controls_at = lv_tick_get();
	set_controls_opacity(LV_OPA_COVER);
}

static void reveal_cb(lv_event_t *e) {
	(void)e;
	reveal_controls();
}

static void close_cb(lv_event_t *e) {
	(void)e;
	back_btn_cb(NULL);
}

static void hide_settings(void) {
	lv_obj_set_hidden(settings_veil, true);
	if (settings_dirty && config_save()) {
		settings_dirty = false;
	}
}

static void settings_cb(lv_event_t *e) {
	if (lv_event_get_target(e) == settings_veil || !lv_obj_is_hidden(settings_veil)) {
		hide_settings();
	} else {
		lv_obj_set_hidden(settings_veil, false);
	}
	reveal_controls();
}

static void layer_changed(lv_event_t *e) {
	visualiser_layer_t *layer = lv_event_get_user_data(e);
	layer->enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	if (layer == &spark_layer && !layer->enabled) {
		memset(sparks, 0, sizeof(sparks));
		spark_budget = 0;
	}
	config_set_bool("visualiser", layer->key, layer->enabled);
	settings_dirty = true;
}

static void tuning_changed(lv_event_t *e) {
	visualiser_tuning_t *tuning = lv_event_get_user_data(e);
	tuning->percent = tuning->minimum + lv_slider_get_value(lv_event_get_target(e)) * TUNING_STEP;
	lv_label_set_text_fmt(tuning->value, "%d%%", tuning->percent);
	config_set_int("visualiser", tuning->key, tuning->percent);
	settings_dirty = true;
}

static void add_layer_toggle(lv_obj_t *panel, visualiser_layer_t *layer) {
	lv_obj_t *toggle;
	lv_obj_t *card = settingsrow_toggle(panel, layer->label, &toggle, NULL);
	lv_obj_set_height(card, 76);
	layer->enabled = config_get_bool("visualiser", layer->key, true);
	if (layer->enabled) {
		lv_obj_add_state(toggle, LV_STATE_CHECKED);
	}
	lv_obj_add_event_cb(toggle, layer_changed, LV_EVENT_VALUE_CHANGED, layer);
}

static void add_tuning_slider(lv_obj_t *panel, visualiser_tuning_t *tuning) {
	lv_obj_t *slider;
	int steps = (150 - tuning->minimum) / TUNING_STEP + 1;
	settingsrow_slider(panel, tuning->label, steps, &tuning->value, &slider, NULL);
	long saved = config_get_int("visualiser", tuning->key, 100);
	int index = 0;
	if (saved >= 150) {
		index = steps - 1;
	} else if (saved > tuning->minimum) {
		index = (int)(saved - tuning->minimum) / TUNING_STEP;
	}
	tuning->percent = tuning->minimum + index * TUNING_STEP;
	lv_slider_set_value(slider, index, LV_ANIM_OFF);
	lv_label_set_text_fmt(tuning->value, "%d%%", tuning->percent);
	lv_obj_add_event_cb(slider, tuning_changed, LV_EVENT_VALUE_CHANGED, tuning);
}

static void build_settings(void) {
	settings_veil = lv_obj_create(screen);
	lv_obj_remove_style_all(settings_veil);
	lv_obj_set_size(settings_veil, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_color(settings_veil, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(settings_veil, LV_OPA_30, 0);
	lv_obj_set_scrollable(settings_veil, false);
	lv_obj_add_event_cb(settings_veil, settings_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *panel = lv_obj_create(settings_veil);
	lv_obj_add_style(panel, &theme_style_panel, 0);
	lv_obj_set_size(panel, lv_pct(90), lv_pct(76));
	lv_obj_set_style_max_width(panel, 440, 0);
	lv_obj_center(panel);
	lv_obj_set_style_radius(panel, 16, 0);
	lv_obj_set_style_border_width(panel, 0, 0);
	lv_obj_set_style_pad_all(panel, 12, 0);
	lv_obj_set_style_pad_row(panel, 6, 0);
	lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scroll_dir(panel, LV_DIR_VER);

	lv_obj_t *title = lv_label_create(panel);
	lv_label_set_text(title, tr("settings"));
	lv_obj_add_style(title, &theme_style_text, 0);
	lv_obj_set_style_text_font(title, &font_ui_28, 0);
	add_layer_toggle(panel, &sharp_layer);
	add_layer_toggle(panel, &sharp_antialias);
	add_layer_toggle(panel, &soft_layer);
	add_layer_toggle(panel, &spark_layer);
	add_layer_toggle(panel, &tunnel_layer);
	add_tuning_slider(panel, &motion_speed);
	add_tuning_slider(panel, &music_response);
	lv_obj_set_hidden(settings_veil, true);
}

static void frame_cb(lv_timer_t *t) {
	if (!pixels || lv_screen_active() != screen) {
		lv_timer_pause(t);
		return;
	}
	uint32_t now = visualiser_audio_now_ms();
	uint32_t elapsed = now - frame_at;
	frame_at = now;
	// After a stall or screen wake, avoid advancing the whole field in one jump.
	if (!elapsed || elapsed > FRAME_MS * 2) {
		elapsed = FRAME_MS;
	}
	read_levels(now);
	advance_motion(elapsed);
	fade_trails();
	if (tunnel_layer.enabled) draw_tunnel();
	if (soft_layer.enabled) draw_ribbons(true);
	if (sharp_layer.enabled) draw_ribbons(false);
	if (spark_layer.enabled) draw_sparks((float)elapsed / FRAME_MS * (motion_speed.percent * .01f));
	lv_obj_invalidate(image);

	uint32_t controls_elapsed = lv_tick_elaps(controls_at);
	if (!lv_obj_is_hidden(settings_veil)) {
		return;
	}
	if (controls_elapsed >= CONTROLS_MS + CONTROLS_FADE_MS) {
		if (!lv_obj_is_hidden(close_button)) {
			set_controls_opacity(LV_OPA_TRANSP);
		}
	} else if (controls_elapsed >= CONTROLS_MS) {
		set_controls_opacity((lv_opa_t)((CONTROLS_MS + CONTROLS_FADE_MS - controls_elapsed) *
									  LV_OPA_COVER / CONTROLS_FADE_MS));
	}
}

static void screen_loaded(lv_event_t *e) {
	(void)e;
	power_hold_screen_for_view(true);
	topbar_set_hidden(true);
	reveal_controls();
	visualiser_audio_frame_t old;
	uint32_t now = visualiser_audio_now_ms();
	frame_at = now - FRAME_MS;
	visualiser_audio_take(&old, now);
	visualiser_audio_set_active(true);
	lv_timer_resume(timer);
	lv_timer_ready(timer);
}

static void screen_unloaded(lv_event_t *e) {
	(void)e;
	hide_settings();
	power_hold_screen_for_view(false);
	visualiser_audio_set_active(false);
	lv_timer_pause(timer);
	lv_image_set_src(image, NULL);
	free(pixels);
	pixels = NULL;
	back_btn_hide_on_screen(screen, false);
}

static lv_obj_t *build_button(lv_align_t align, int padding, const lv_image_dsc_t *icon, lv_event_cb_t cb) {
	lv_obj_t *button = lv_button_create(screen);
	lv_obj_set_size(button, 56, 56);
	lv_obj_align(button, align, align == LV_ALIGN_TOP_LEFT ? padding : -padding, padding);
	lv_obj_set_style_bg_color(button, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(button, LV_OPA_70, 0);
	lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *glyph = lv_image_create(button);
	lv_image_set_src(glyph, icon);
	lv_obj_set_style_image_recolor(glyph, lv_color_white(), 0);
	lv_obj_set_style_image_recolor_opa(glyph, LV_OPA_COVER, 0);
	lv_obj_center(glyph);
	return button;
}

static void build(gui_config_t *cfg) {
	screen = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
	lv_obj_set_scrollable(screen, false);
	lv_obj_set_clickable(screen, true);
	lv_obj_add_event_cb(screen, reveal_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(screen, screen_loaded, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(screen, screen_unloaded, LV_EVENT_SCREEN_UNLOADED, NULL);

	image = lv_image_create(screen);
	lv_obj_set_size(image, cfg->screen_width, cfg->screen_height);
	lv_image_set_inner_align(image, LV_IMAGE_ALIGN_STRETCH);

	build_settings();
	close_button = build_button(LV_ALIGN_TOP_LEFT, cfg->padding, &icon_close, close_cb);
	settings_button = build_button(LV_ALIGN_TOP_RIGHT, cfg->padding, &icon_music_settings, settings_cb);

	timer = lv_timer_create(frame_cb, FRAME_MS, NULL);
	power_pause_in_standby(timer);
	lv_timer_pause(timer);
	for (int i = 0; i < SINE_SIZE; i++) {
		sine_table[i] = sinf(i * (TWO_PI / SINE_SIZE));
	}
}

void visualiser_open(gui_config_t *cfg) {
	if (!screen) {
		build(cfg);
	}
	if (lv_screen_active() == screen) {
		return;
	}

	// Keep the software framebuffer small on the R1. LVGL scales the finished
	// pixels to the panel in a single opaque image.
	width = cfg->screen_width / 2;
	height = cfg->screen_height / 2;
	pixels = calloc((size_t)width * height, sizeof(*pixels));
	if (!pixels) {
		return;
	}
	picture = (lv_image_dsc_t){
		.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
				   .w = width, .h = height, .stride = width * 2},
		.data_size = (uint32_t)(width * height * 2),
		.data = (const uint8_t *)pixels,
	};
	lv_image_set_src(image, &picture);
	reach = .54f * sqrtf((float)width * width + (float)height * height);
	if (!random_state) {
		random_state = (0xcf624a18u ^ visualiser_audio_now_ms() ^ (uint32_t)time(NULL)) | 1u;
	}
	phase = random_unit();
	drift = random_unit();
	twist = random_unit();
	shape_phase = random_unit();
	hue = next_starting_hue();
	tunnel_travel = 0;
	bass = mid = treble = target_bass = target_mid = target_treble = 0;
	beat = pulse = bass_floor = previous_low = spark_budget = 0;
	beat_cooldown = silent_frames = next_spark = 0;
	memset(sparks, 0, sizeof(sparks));
	player_sheet_close(false);
	back_btn_hide_on_screen(screen, true);
	switch_screen(screen);
	switcher_set_player_return(screen);
}
