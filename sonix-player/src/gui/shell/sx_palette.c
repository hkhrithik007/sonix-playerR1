#include "sx_palette.h"
#include "artview.h"
#include "src/system/image/albumtone.h"
#include "theme.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lvgl/src/misc/lv_area_private.h"
#include "src/system/playback/device_state.h"

// The hue the interface wears before anything has played, and after the queue
// is gone: a cool blue, the nearest thing in the cover palette to the blue
// accent the player had before 2.0, so a first boot still looks like Sonix.
#define DEFAULT_HUE 255.0f

// The chroma the tokens were drawn for. A cover's own chroma scales them: a
// black-and-white sleeve gives a nearly grey page instead of an invented
// colour, a loud one does not shout louder than the design allows.
#define REFERENCE_CHROMA 0.10f
#define MIN_CHROMA_SCALE 0.20f

static sx_palette_t pal;

// The Appearance choices, as theme.c last handed them over.
static bool cfg_dark = true;
static int cfg_accent = THEME_ACCENT_GRAPHITE;
static lv_color_t cfg_preset;
static bool cfg_tint;
static int cfg_family, cfg_lamp;
static bool cfg_force_color;
static bool cfg_monochrome;
static float play_placeholder_hue;
static lv_color_t family_placeholder(float hue);
static lv_color_t family_placeholder_for(float hue, float context_hue);

// The playing track's tone, and an album page's while it is pushed.
static float play_hue = DEFAULT_HUE;
static float play_chroma = REFERENCE_CHROMA;
static lv_color_t play_placeholder;
static bool play_placeholder_known;
static bool pushed;
static float pushed_hue;
static float pushed_chroma;

// The chroma scale of the hue being drawn, applied by sx_tone().
static float chroma_scale = 1.0f;

static sx_tone_provider_t tone_provider;
static void (*change_hook)(void);

const sx_palette_t *sx_pal(void) { return &pal; }

// ---------------------------------------------------------------------------
// OKLCH
// ---------------------------------------------------------------------------

static float srgb_gamma(float x) {
	if (x <= 0.0031308f) {
		return 12.92f * x;
	}
	return 1.055f * powf(x, 1.0f / 2.4f) - 0.055f;
}

static float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

// OKLCH to gamma-encoded sRGB in 0..1, clipped channel by channel as the
// handoff does: the tokens stay inside the gamut for every hue except the
// palest accents, where a clipped channel is invisible.
static void oklch_to_srgb(float L, float C, float h_deg, float rgb[3]) {
	float h = h_deg * 0.01745329252f;
	float a = C * cosf(h);
	float b = C * sinf(h);

	float l_ = L + 0.3963377774f * a + 0.2158037573f * b;
	float m_ = L - 0.1055613458f * a - 0.0638541728f * b;
	float s_ = L - 0.0894841775f * a - 1.2914855480f * b;

	float l = l_ * l_ * l_;
	float m = m_ * m_ * m_;
	float s = s_ * s_ * s_;

	rgb[0] = clamp01(srgb_gamma(4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s));
	rgb[1] = clamp01(srgb_gamma(-1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s));
	rgb[2] = clamp01(srgb_gamma(-0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s));
}

uint16_t sx_oklch_to_565(float L, float C, float h_deg) {
	float rgb[3];
	oklch_to_srgb(L, C, h_deg, rgb);
	return (uint16_t)(((uint16_t)(rgb[0] * 31.0f + 0.5f) << 11) | ((uint16_t)(rgb[1] * 63.0f + 0.5f) << 5) | (uint16_t)(rgb[2] * 31.0f + 0.5f));
}

// Widened from RGB565 rather than rounded from the float: what is on screen is
// the 565 value, and two colours that look the same on the panel then compare
// equal here too.
lv_color_t sx_oklch(float L, float C, float h_deg) {
	uint16_t c = sx_oklch_to_565(L, C, h_deg);
	uint8_t r5 = (uint8_t)(c >> 11), g6 = (uint8_t)((c >> 5) & 0x3f), b5 = (uint8_t)(c & 0x1f);
	return lv_color_make((uint8_t)((r5 << 3) | (r5 >> 2)), (uint8_t)((g6 << 2) | (g6 >> 4)), (uint8_t)((b5 << 3) | (b5 >> 2)));
}

lv_color_t sx_tone(sx_lc_t lc, float h_deg) { return sx_oklch(lc.L, lc.C * chroma_scale, h_deg); }

lv_color_t sx_palette_cover_accent(void) { return sx_tone(cfg_dark ? SX_T_D_ACCENT : SX_T_L_ACCENT, pal.hue); }

lv_color_t sx_palette_cover_on_accent(void) { return cfg_dark ? sx_tone(SX_T_D_ON_ACC, pal.hue) : lv_color_white(); }

lv_color_t sx_mix(lv_color_t a, lv_color_t b, uint8_t pct) {
	if (pct > 100) {
		pct = 100;
	}
	return lv_color_mix(a, b, (uint8_t)((pct * 255 + 50) / 100));
}

// ---------------------------------------------------------------------------
// The palette
// ---------------------------------------------------------------------------

static float scale_for(float chroma) {
	if (chroma < 0.0f) {
		return 1.0f;
	}
	float s = chroma / REFERENCE_CHROMA;
	if (s < MIN_CHROMA_SCALE) {
		s = MIN_CHROMA_SCALE;
	}
	return s > 1.0f ? 1.0f : s;
}

static float luminance(lv_color_t c) {
	const uint8_t channel[3] = {c.red, c.green, c.blue};
	const float weight[3] = {0.2126f, 0.7152f, 0.0722f};
	float sum = 0.0f;
	for (int i = 0; i < 3; i++) {
		float v = channel[i] / 255.0f;
		v = v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
		sum += weight[i] * v;
	}
	return sum;
}

static float contrast(lv_color_t a, lv_color_t b) {
	float x = luminance(a), y = luminance(b);
	return (fmaxf(x, y) + 0.05f) / (fminf(x, y) + 0.05f);
}

static lv_color_t readable_accent(lv_color_t accent, lv_color_t surface, lv_color_t text) {
	lv_color_t c = accent;
	for (int pct = 90; pct >= 0 && contrast(c, surface) < 4.5f; pct -= 10) {
		c = sx_mix(accent, text, (uint8_t)pct);
	}
	return c;
}

static lv_color_t placeholder_legacy(float hue) {
	static const uint32_t colours[] = {
		0x984F47, 0x866329, 0x5A7139, 0x36715D, 0x3B6483, 0x4E5F8A, 0x6C5286, 0x884E71,
	};
	int index = ((int)hue % 360 + 360) % 360 / 45;
	return sx_hex(colours[index]);
}

lv_color_t sx_placeholder_tone(float hue) {
	lv_color_t color = cfg_family && !cfg_force_color ? family_placeholder(hue) : placeholder_legacy(hue);
	return cfg_monochrome && !cfg_force_color ? sx_artview_gray_color(color) : color;
}
lv_color_t sx_placeholder_ink(lv_color_t background) {
	if (!cfg_family && !cfg_monochrome)
		return lv_color_white();
	return contrast(lv_color_black(), background) > contrast(lv_color_white(), background) ? lv_color_black() : lv_color_white();
}

lv_color_t sx_placeholder_name(const char *name) { return sx_placeholder_tone(sx_palette_hue_of_name(name)); }

float sx_placeholder_file_hue(const char *path, const char *album) {
	if (album && album[0])
		return sx_palette_hue_of_name(album);
	char folder[512];
	snprintf(folder, sizeof(folder), "%s", path ? path : "");
	char *slash = strrchr(folder, '/');
	if (slash)
		*slash = '\0';
	const char *name = strrchr(folder, '/');
	return sx_palette_hue_of_name(name ? name + 1 : folder);
}
lv_color_t sx_placeholder_file(const char *path, const char *album) { return sx_placeholder_tone(sx_placeholder_file_hue(path, album)); }

// Additional families are opt-in. The original recompute path stays intact.
enum { FAMILY_LCD = 1, FAMILY_CANVAS = 2 };
typedef struct {
	float level[2][5];
	uint32_t fixed[2][5];
	float tint, primary_chroma, hue, chroma, text_chroma, secondary;
	uint32_t recommended[2];
	unsigned flags;
} family_t;
static const family_t families[8] = {
	{.level = {{.95, .99, .89, .20, .43}, {.19, .255, .32, .96, .76}}, .fixed = {{0xefefef, 0xffffff, 0xd6d6d6, 0x212121, 0x525252}, {0x101010, 0x212121, 0x313131, 0xf7f7f7, 0xb8b8b8}}, .tint = .018, .primary_chroma = .105},
	{.level = {{1, .965, .88, .13, .36}, {0, .20, .29, .98, .80}}, .fixed = {{0xffffff, 0xf3f4f6, 0xdce0e5, 0x101319, 0x4a5058}, {0x000000, 0x101216, 0x252a30, 0xf9fbff, 0xb7bdc5}}, .tint = .045, .primary_chroma = .29, .hue = 255, .chroma = .29, .recommended = {0x005beb, 0x008cff}, .flags = FAMILY_CANVAS},
	{.level = {{.81, .85, .73, .19, .35}, {.16, .22, .29, .87, .73}}, .tint = .055, .primary_chroma = .13, .chroma = .10, .text_chroma = .035, .flags = FAMILY_LCD},
	{.level = {{.99, .99, .87, .10, .34}, {.045, .08, .17, .98, .79}}, .fixed = {{0xffffff, 0xffffff, 0xdedede, 0x101010, 0x525252}, {0x000000, 0x080808, 0x212121, 0xffffff, 0xb5b5b5}}, .tint = .012, .primary_chroma = .12},
	{.level = {{.95, .99, .85, .16, .35}, {.10, .16, .24, .91, .77}}, .fixed = {{0xe9f0e7, 0xf6fbef, 0xcad8c6, 0x14331f, 0x3e6046}, {0x080f0a, 0x101a12, 0x1b2b1e, 0x6ce99b, 0x86bb96}}, .tint = .025, .primary_chroma = .22, .hue = 145, .chroma = .21, .text_chroma = .10, .secondary = 195},
	{.level = {{.94, .99, .86, .15, .37}, {.10, .185, .28, .97, .80}}, .tint = .055, .primary_chroma = .27, .hue = 335, .chroma = .25, .secondary = 195},
	{.level = {{.94, .99, .86, .15, .37}, {.10, .185, .28, .97, .80}}, .fixed = {{0xeee736, 0xf8f34d, 0xd4ce2a, 0x101a22, 0x414732}, {0x0b1018, 0x18212b, 0x263744, 0xe8e28c, 0xa2b7ba}}, .tint = .030, .primary_chroma = .24, .hue = 100, .chroma = .19, .secondary = 195, .recommended = {0x14212b, 0xeee736}},
	{.level = {{.94, .99, .86, .15, .37}, {.10, .185, .28, .97, .80}}, .fixed = {{0xeee8e6, 0xfaf6f3, 0xddd0cc, 0x302129, 0x66525a}, {0x110e16, 0x211921, 0x34232b, 0xedc4bd, 0xba9ba0}}, .tint = .025, .primary_chroma = .22, .hue = 25, .chroma = .20, .secondary = 195, .recommended = {0x9f303f, 0xff7772}},
};
static const float lamp_hues[5] = {115, 80, 215, 0, 345};
static const float lamp_chroma[5] = {.065, .085, .055, 0, .055};

static lv_color_t quantized(lv_color_t c) {
	unsigned r = (c.red * 31U + 127U) / 255U;
	unsigned g = (c.green * 63U + 127U) / 255U;
	unsigned b = (c.blue * 31U + 127U) / 255U;
	return lv_color_make((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2));
}
static lv_color_t family_hex(uint32_t c) { return quantized(sx_hex(c)); }
static lv_color_t family_mix(lv_color_t a, lv_color_t b, unsigned pct) { return quantized(sx_mix(a, b, pct)); }
static float minimum_contrast(lv_color_t ink, const lv_color_t *bg, int n) {
	float c = 100;
	for (int i = 0; i < n; i++)
		c = fminf(c, contrast(ink, bg[i]));
	return c;
}
// Eight bisections bound the work. Compare the final quantized display colors.
static lv_color_t family_readable(lv_color_t candidate, const lv_color_t *bg, int n, float target) {
	candidate = quantized(candidate);
	if (minimum_contrast(candidate, bg, n) >= target)
		return candidate;
	lv_color_t black = lv_color_black(), white = lv_color_white();
	lv_color_t end = minimum_contrast(black, bg, n) > minimum_contrast(white, bg, n) ? black : white;
	int low = 0, high = 100;
	for (int i = 0; i < 8; i++) {
		int middle = (low + high) / 2;
		if (minimum_contrast(family_mix(end, candidate, middle), bg, n) >= target)
			high = middle;
		else
			low = middle + 1;
	}
	return family_mix(end, candidate, high);
}
static lv_color_t family_ink(lv_color_t background) { return contrast(lv_color_black(), background) > contrast(lv_color_white(), background) ? lv_color_black() : lv_color_white(); }
static void family_channels(float hue, float chroma, lv_color_t out[5]) {
	const family_t *f = &families[cfg_family - 1];
	int d = cfg_dark;
	float scale = fminf(1, fmaxf(0, chroma / REFERENCE_CHROMA));
	float base_h = cfg_tint ? hue : f->flags & FAMILY_LCD ? lamp_hues[cfg_lamp] : cfg_family == 6 ? 295 : 0;
	float base_c = cfg_tint ? f->tint * scale : f->flags & FAMILY_LCD ? lamp_chroma[cfg_lamp] : cfg_family == 6 ? .04 : 0;
	bool fixed = f->fixed[d][1] != 0;
	for (int i = 0; i < 5; i++) {
		out[i] = fixed ? family_hex(f->fixed[d][i]) : sx_oklch(f->level[d][i], i < 3 ? base_c : f->text_chroma, i < 3 ? base_h : f->flags & FAMILY_LCD ? base_h : f->hue);
		if (fixed && cfg_tint && i < 3 && (!(f->flags & FAMILY_CANVAS) || i > 0))
			out[i] = family_mix(sx_oklch(f->level[d][i], base_c, base_h), out[i], 32);
	}
	out[3] = family_readable(out[3], out, 3, 4.6f);
	if (f->secondary)
		out[4] = sx_oklch(cfg_dark ? .79f : .36f, .035f, f->secondary);
	out[4] = family_readable(out[4], out, 3, 4.6f);
}
static lv_color_t family_primary(float hue, float chroma) {
	const family_t *f = &families[cfg_family - 1];
	if (cfg_accent == THEME_ACCENT_PALETTE) {
		if (f->recommended[cfg_dark])
			return family_hex(f->recommended[cfg_dark]);
		float h = f->flags & FAMILY_LCD ? lamp_hues[cfg_lamp] : f->hue;
		float c = f->flags & FAMILY_LCD && lamp_chroma[cfg_lamp] == 0 ? 0 : f->chroma;
		return sx_oklch(cfg_dark ? .76f : .46f, c, h);
	}
	if (cfg_accent == THEME_ACCENT_COVER)
		return sx_oklch(cfg_dark ? .76f : .46f, f->primary_chroma * fminf(1, fmaxf(0, chroma / REFERENCE_CHROMA)), hue);
	return quantized(cfg_preset);
}
static void family_recompute(void) {
	float h = pushed ? pushed_hue : play_hue, c = pushed ? pushed_chroma : play_chroma;
	lv_color_t colors[5], playing[5];
	family_channels(h, c, colors);
	family_channels(play_hue, play_chroma, playing);
	pal.dark = cfg_dark;
	pal.tinted = cfg_tint;
	pal.accent_from_cover = cfg_accent == THEME_ACCENT_COVER;
	pal.hue = h;
	pal.play_hue = play_hue;
	pal.bg = colors[0];
	pal.surface = colors[1];
	pal.press = colors[2];
	pal.text = colors[3];
	pal.text2 = colors[4];
	pal.accent = family_primary(h, c);
	pal.on_accent = family_ink(pal.accent);
	pal.active = family_mix(pal.accent, pal.surface, 16);
	lv_color_t text_bgs[] = {pal.bg, pal.surface, pal.press, pal.active};
	pal.text = family_readable(pal.text, text_bgs, 4, 4.6f);
	pal.text2 = family_readable(pal.text2, text_bgs, 4, 4.6f);
	pal.accent_text = family_readable(pal.accent, text_bgs, 4, 4.6f);
	pal.panel = pal.surface;
	pal.panel_press = pal.press;
	pal.panel_text = pal.text;
	pal.panel_text2 = pal.text2;
	if (cfg_family == 7 && !cfg_dark) {
		pal.panel = family_hex(0x1b2730);
		pal.panel_press = family_hex(0x30404b);
		if (cfg_tint) {
			pal.panel = family_mix(sx_oklch(.28, .03, h), pal.panel, 25);
			pal.panel_press = family_mix(sx_oklch(.36, .03, h), pal.panel_press, 25);
		}
		pal.panel_text = family_hex(0xeee8b1);
		pal.panel_text2 = family_hex(0xb7c1c3);
	}
	lv_color_t panel_bgs[] = {pal.panel, pal.panel_press};
	pal.panel_text = family_readable(pal.panel_text, panel_bgs, 2, 4.6f);
	pal.panel_text2 = family_readable(pal.panel_text2, panel_bgs, 2, 4.6f);
	pal.panel_accent = family_readable(pal.accent, panel_bgs, 2, 4.6f);
	pal.panel_on_accent = family_ink(pal.panel_accent);
	pal.panel_active = family_mix(pal.panel_accent, pal.panel, 16);
	lv_color_t panel_states[] = {pal.panel, pal.panel_press, pal.panel_active};
	pal.panel_text = family_readable(pal.panel_text, panel_states, 3, 4.6f);
	pal.panel_text2 = family_readable(pal.panel_text2, panel_states, 3, 4.6f);
	pal.panel_action = family_readable(pal.accent, panel_states, 3, 4.6f);
	pal.swipe_end = family_mix(pal.accent, pal.surface, 25);
	pal.cover_tone = sx_placeholder_tone(h);
	pal.star_on = pal.accent_text;
	pal.hairline = family_mix(pal.text, pal.surface, 25);
	pal.hairline_opa = LV_OPA_COVER;
	pal.np_bg = playing[0];
	pal.np_text2 = playing[4];
	pal.mini_bg = playing[1];
	pal.mini_ink = playing[3];
	pal.mini_text2 = playing[4];
	if (cfg_family == 7 && !cfg_dark) {
		pal.mini_bg = family_hex(0x1b2730);
		if (cfg_tint)
			pal.mini_bg = family_mix(sx_oklch(.28, .03, play_hue), pal.mini_bg, 25);
		pal.mini_ink = family_readable(family_hex(0xeee8b1), &pal.mini_bg, 1, 4.6f);
		pal.mini_text2 = family_readable(sx_oklch(.79, .035, 195), &pal.mini_bg, 1, 4.6f);
	}
	pal.np_accent = family_readable(family_primary(play_hue, play_chroma), playing, 2, 4.6f);
	lv_color_t white = lv_color_white();
	pal.np_ink = family_readable(family_primary(play_hue, play_chroma), &white, 1, 4.6f);
	pal.mini_action = family_primary(play_hue, play_chroma);
	pal.mini_on_action = family_ink(pal.mini_action);
	pal.album_text = pal.text;
	float placeholder_hue = play_placeholder_known ? play_placeholder_hue : DEFAULT_HUE;
	pal.mini_cover_tone = cfg_force_color ? placeholder_legacy(placeholder_hue) : family_placeholder_for(placeholder_hue, play_hue);
	if (cfg_monochrome && !cfg_force_color)
		pal.mini_cover_tone = sx_artview_gray_color(pal.mini_cover_tone);
	for (int i = 0; i < 3; i++)
		pal.album_grad[i] = pal.bg;
	chroma_scale = scale_for(c);
}

void sx_palette_options(int family, int lamp, bool force_color, bool monochrome) {
	cfg_monochrome = monochrome;
	cfg_family = family >= 0 && family <= 8 ? family : 0;
	cfg_lamp = lamp >= 0 && lamp < 5 ? lamp : 0;
	cfg_force_color = force_color;
}

static lv_color_t family_placeholder_for(float hue, float context_hue) {
	int bucket = ((int)hue % 360 + 360) % 360 / 45;
	const family_t *f = &families[cfg_family - 1];
	float h = hue, c = .045f;
	if (cfg_family == 1 || cfg_family == 4)
		c = 0;
	else if (f->flags & FAMILY_LCD) {
		h = cfg_tint ? context_hue : lamp_hues[cfg_lamp];
		c = cfg_tint ? f->tint : lamp_chroma[cfg_lamp];
	} else if (cfg_family == 5)
		h = 145 + bucket * 7;
	else if (cfg_family == 6)
		h = (float[]){285, 310, 195, 335}[bucket % 4];
	else if (cfg_family == 7)
		h = (float[]){100, 195, 240, 75}[bucket % 4];
	else if (cfg_family == 8)
		h = (float[]){20, 345, 195, 280}[bucket % 4];
	return sx_oklch((cfg_dark ? .34f : .84f) + (bucket % 4) * .025f, c, h);
}
static lv_color_t family_placeholder(float hue) { return family_placeholder_for(hue, pal.hue); }

static void chrome_edges(void) {
	pal.edge_frame = cfg_family == 4 || cfg_family == 5;
	pal.surface_edge = pal.hairline;
	pal.surface_edge_opa = pal.hairline_opa;
	pal.playback_edge = pal.mini_ink;
	lv_opa_t playback_opa = cfg_family ? LV_OPA_20 : pal.hairline_opa;
	if (pal.edge_frame) {
		pal.surface_edge = sx_mix(pal.panel_text2, pal.panel, 60);
		pal.playback_edge = sx_mix(pal.mini_text2, pal.mini_bg, 60);
		pal.surface_edge_opa = playback_opa = LV_OPA_COVER;
	} else if (cfg_family && cfg_family != 3) {
		// Keep a quiet edge only where adjacent fills are hard to distinguish.
		// This is a layer-separation choice, not a text contrast requirement.
		if (contrast(pal.surface, pal.bg) >= 1.25f)
			pal.surface_edge_opa = LV_OPA_TRANSP;
		if (contrast(pal.mini_bg, pal.bg) >= 1.25f)
			playback_opa = LV_OPA_TRANSP;
	}
	pal.mini_edge_opa = pal.shade_edge_opa = pal.floating_edge_opa = playback_opa;
	if (cfg_family == 3) {
		// LCD mini/floating pills already separate by fill. A drawer can lie
		// over same-colored cards, so its outer boundary remains distinct.
		pal.mini_edge_opa = pal.floating_edge_opa = LV_OPA_TRANSP;
	} else if (!cfg_family) {
		// The large Graphite shade needs a softer edge than the small mini.
		pal.shade_edge_opa = (playback_opa + 1) / 2;
	}
}

static void recompute(void) {
	if (cfg_family) {
		family_recompute();
		chrome_edges();
		return;
	}
	float hue = pushed ? pushed_hue : play_hue;
	float page_scale = scale_for(pushed ? pushed_chroma : play_chroma);
	float play_scale = scale_for(play_chroma);

	pal.dark = cfg_dark;
	pal.tinted = cfg_tint;
	pal.accent_from_cover = cfg_accent == THEME_ACCENT_COVER;
	pal.hue = hue;
	pal.play_hue = play_hue;

	chroma_scale = page_scale;

	// The neutrals: the cover's hue at the token's lightness, or Adwaita's
	// greys with the tint off.
	if (cfg_tint) {
		if (cfg_dark) {
			pal.bg = sx_tone(SX_T_D_BG, hue);
			pal.surface = sx_tone(SX_T_D_SURFACE, hue);
			pal.press = sx_tone(SX_T_D_PRESS, hue);
			pal.text = sx_hex(SX_TEXT_ON_DARK);
			pal.text2 = sx_tone(SX_T_D_TEXT2, hue);
		} else {
			pal.bg = sx_tone(SX_T_L_BG, hue);
			pal.surface = sx_hex(SX_L_SURFACE);
			pal.press = sx_tone(SX_T_L_PRESS, hue);
			pal.text = sx_hex(SX_TEXT_ON_LIGHT);
			pal.text2 = sx_tone(SX_T_L_TEXT2, hue);
		}
	} else if (cfg_dark) {
		pal.bg = sx_hex(SX_D_BG);
		pal.surface = sx_hex(SX_D_SURFACE);
		pal.press = sx_hex(SX_D_PRESS);
		pal.text = sx_hex(SX_D_TEXT);
		pal.text2 = sx_hex(SX_D_TEXT2);
	} else {
		pal.bg = sx_hex(SX_L_BG);
		pal.surface = sx_hex(SX_L_SURFACE);
		pal.press = sx_hex(SX_L_PRESS);
		pal.text = sx_hex(SX_L_TEXT);
		pal.text2 = sx_hex(SX_L_TEXT2);
	}

	// The accent comes from the cover whether or not the neutrals do: the
	// tint is about the backgrounds, the accent choice about the highlights.
	if (pal.accent_from_cover) {
		pal.accent = sx_tone(cfg_dark ? SX_T_D_ACCENT : SX_T_L_ACCENT, hue);
		pal.on_accent = cfg_dark ? sx_tone(SX_T_D_ON_ACC, hue) : lv_color_white();
	} else {
		pal.accent = cfg_preset;
		lv_color_t ink = sx_hex(SX_L_TEXT);
		pal.on_accent = contrast(ink, pal.accent) > contrast(lv_color_white(), pal.accent) ? ink : lv_color_white();
	}
	pal.accent_text = cfg_accent == THEME_ACCENT_GRAPHITE ? sx_hex(cfg_dark ? SX_D_ACCENT_TEXT : SX_L_ACCENT) : readable_accent(pal.accent, pal.surface, pal.text);

	pal.active = sx_mix(pal.accent, pal.surface, 16);
	pal.swipe_end = sx_tone(cfg_dark ? SX_T_D_SWIPE_L : SX_T_L_SWIPE_L, hue);
	pal.cover_tone = sx_placeholder_tone(hue);
	pal.star_on = sx_hex(cfg_dark ? SX_STAR_ON_DARK : SX_STAR_ON_LIGHT);
	pal.hairline = cfg_dark ? lv_color_white() : lv_color_black();
	pal.hairline_opa = cfg_dark ? SX_HAIRLINE_DARK_OPA : SX_HAIRLINE_LIGHT_OPA;

	pal.album_grad[0] = sx_tone(SX_T_AL_GRAD_0, hue);
	pal.album_grad[1] = sx_tone(SX_T_AL_GRAD_1, hue);
	pal.album_grad[2] = sx_tone(SX_T_AL_GRAD_2, hue);

	// Playback surfaces stay tied to the playing track while an album page
	// borrows a different hue. Reuse the existing fields in all consumers.
	chroma_scale = play_scale;
	pal.np_bg = cfg_tint ? sx_tone(cfg_dark ? SX_T_D_BG : SX_T_L_BG, play_hue) : pal.bg;
	pal.np_text2 = cfg_tint ? sx_tone(cfg_dark ? SX_T_D_TEXT2 : SX_T_L_TEXT2, play_hue) : pal.text2;
	pal.mini_bg = pal.accent_from_cover ? sx_tone(cfg_dark ? SX_T_D_ACCENT : SX_T_L_ACCENT, play_hue) : pal.accent;
	pal.mini_ink = pal.accent_from_cover && cfg_dark ? sx_tone(SX_T_D_ON_ACC, play_hue) : pal.on_accent;
	lv_color_t play_surface = cfg_tint && cfg_dark ? sx_tone(SX_T_D_SURFACE, play_hue) : pal.surface;
	pal.np_accent = cfg_accent == THEME_ACCENT_GRAPHITE ? pal.accent_text : readable_accent(pal.mini_bg, play_surface, pal.text);
	pal.np_ink = pal.mini_bg;
	pal.mini_cover_tone = play_placeholder_known ? (cfg_monochrome && !cfg_force_color ? sx_artview_gray_color(play_placeholder) : play_placeholder) : sx_placeholder_tone(DEFAULT_HUE);
	if (!cfg_tint) {
		pal.album_grad[0] = pal.album_grad[1] = pal.album_grad[2] = pal.bg;
	}

	pal.panel = pal.surface;
	pal.panel_press = pal.press;
	pal.panel_text = pal.text;
	pal.panel_text2 = pal.text2;
	pal.panel_accent = pal.accent;
	pal.panel_on_accent = pal.on_accent;
	pal.panel_active = pal.active;
	pal.panel_action = pal.accent_text;
	pal.album_text = pal.tinted ? lv_color_white() : pal.text;
	pal.mini_text2 = sx_mix(pal.mini_ink, pal.mini_bg, 85);
	// Keep the chosen primary vivid; playback chrome shares a quiet surface.
	// These roles stay bound to playback, even while browsing another album.
	pal.mini_action = pal.mini_bg;
	pal.mini_on_action = pal.mini_ink;
	pal.mini_bg = cfg_tint && cfg_dark ? sx_tone(SX_T_D_SURFACE, play_hue) : pal.surface;
	pal.mini_ink = pal.text;
	pal.mini_text2 = pal.np_text2;
	// sx_tone() as called from outside works at the page's scale.
	chroma_scale = page_scale;
	chrome_edges();
}

void sx_palette_configure(bool dark, int accent, lv_color_t preset, bool tint) {
	cfg_dark = dark;
	cfg_accent = accent;
	cfg_preset = preset;
	cfg_tint = tint;
	recompute();
}

void sx_palette_set_change_hook(void (*hook)(void)) { change_hook = hook; }

// Recomputes and tells theme.c, but only when what is drawn would change: a
// hue a fraction of a degree away is the same 565 colours, and every repaint
// costs a frame.
static void hue_moved(void) {
	sx_palette_t before = pal;
	recompute();
	if (memcmp(&before, &pal, sizeof(pal)) == 0) {
		return;
	}
	if (change_hook) {
		change_hook();
	}
}

// ---------------------------------------------------------------------------
// The hue of a track
// ---------------------------------------------------------------------------

// FNV-1a: stable across builds and platforms, so an album keeps its colour
// from one firmware to the next and on the device as in the emulator.
float sx_palette_hue_of_name(const char *name) { return (float)albumtone_hash_hue(name); }

// The default: no cover tone known, so the album's title is hashed -- the
// album tag of the loaded track when this is it, the folder's name otherwise
// (the folder is the album on a card laid out the usual way).
static bool default_tone(const char *track_path, float *hue, float *chroma) {
	if (!track_path || !track_path[0]) {
		return false;
	}
	const char *loaded = NULL;
	const song_metadata_t *m = device_state_loaded_metadata(&loaded);
	if (m && loaded && strcmp(loaded, track_path) == 0 && m->album[0]) {
		*hue = sx_palette_hue_of_name(m->album);
		*chroma = REFERENCE_CHROMA;
		return true;
	}

	const char *slash = strrchr(track_path, '/');
	if (!slash || slash == track_path) {
		*hue = sx_palette_hue_of_name(track_path);
	} else {
		char folder[256];
		size_t len = (size_t)(slash - track_path);
		const char *start = track_path;
		const char *prev = NULL;
		for (const char *p = track_path; p < slash; p++) {
			if (*p == '/') {
				prev = p;
			}
		}
		if (prev) {
			start = prev + 1;
			len = (size_t)(slash - start);
		}
		if (len >= sizeof(folder)) {
			len = sizeof(folder) - 1;
		}
		memcpy(folder, start, len);
		folder[len] = '\0';
		*hue = sx_palette_hue_of_name(folder);
	}
	*chroma = REFERENCE_CHROMA;
	return true;
}

void sx_palette_set_tone_provider(sx_tone_provider_t provider) { tone_provider = provider; }

void sx_palette_track_changed(const char *track_path, const char *name) {
	play_placeholder_hue = sx_placeholder_file_hue(track_path, name);
	play_placeholder = placeholder_legacy(play_placeholder_hue);
	play_placeholder_known = true;
	float hue = DEFAULT_HUE;
	float chroma = REFERENCE_CHROMA;
	bool known = false;

	if (track_path && track_path[0] && tone_provider) {
		known = tone_provider(track_path, &hue, &chroma);
	}
	if (!known && name && name[0]) {
		hue = sx_palette_hue_of_name(name);
		chroma = REFERENCE_CHROMA;
		known = true;
	}
	if (!known) {
		known = default_tone(track_path, &hue, &chroma);
	}
	if (!known) {
		hue = DEFAULT_HUE;
		chroma = REFERENCE_CHROMA;
	}

	if (hue < 0.0f || hue >= 360.0f) {
		hue = fmodf(fmodf(hue, 360.0f) + 360.0f, 360.0f);
	}
	play_hue = hue;
	play_chroma = chroma;
	hue_moved();
}

void sx_palette_track_cleared(void) {
	play_placeholder_known = false;
	play_hue = DEFAULT_HUE;
	play_chroma = REFERENCE_CHROMA;
	hue_moved();
}

void sx_palette_push_hue(float hue_deg, float chroma) {
	pushed = true;
	pushed_hue = fmodf(fmodf(hue_deg, 360.0f) + 360.0f, 360.0f);
	pushed_chroma = chroma < 0.0f ? REFERENCE_CHROMA : chroma;
	hue_moved();
}

void sx_palette_pop_hue(void) {
	if (!pushed) {
		return;
	}
	pushed = false;
	hue_moved();
}

// ---------------------------------------------------------------------------
// The dithered gradient
// ---------------------------------------------------------------------------

// The 4 x 4 Bayer matrix, as thresholds in sixteenths.
static const uint8_t BAYER4[4][4] = {
	{0, 8, 2, 10},
	{12, 4, 14, 6},
	{3, 11, 1, 9},
	{15, 7, 13, 5},
};

// One channel, from 0..255 to `levels - 1` steps, the fraction pushed over the
// next step where the matrix says so.
static inline uint16_t dither_channel(float v, int levels_minus_one, uint8_t threshold) {
	float scaled = v * (float)levels_minus_one / 255.0f;
	int base = (int)scaled;
	float frac = scaled - (float)base;
	if (frac * 16.0f > (float)threshold + 0.5f && base < levels_minus_one) {
		base++;
	}
	return (uint16_t)base;
}

lv_draw_buf_t *sx_gradient_create(int32_t w, int32_t h, const sx_grad_stop_t *stops, int count) {
	if (w <= 0 || h <= 0 || !stops || count <= 0) {
		return NULL;
	}
	lv_draw_buf_t *buf = lv_draw_buf_create((uint32_t)w, (uint32_t)h, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
	if (!buf) {
		return NULL;
	}

	int stop = 0;
	for (int32_t y = 0; y < h; y++) {
		// The two stops around this row.
		while (stop + 1 < count && stops[stop + 1].y <= y) {
			stop++;
		}
		float rgb[3];
		const sx_grad_stop_t *a = &stops[stop];
		if (y <= stops[0].y || stop + 1 >= count) {
			const sx_grad_stop_t *only = y <= stops[0].y ? &stops[0] : a;
			rgb[0] = only->color.red;
			rgb[1] = only->color.green;
			rgb[2] = only->color.blue;
		} else {
			const sx_grad_stop_t *b = &stops[stop + 1];
			float t = (float)(y - a->y) / (float)(b->y - a->y);
			rgb[0] = a->color.red + (b->color.red - a->color.red) * t;
			rgb[1] = a->color.green + (b->color.green - a->color.green) * t;
			rgb[2] = a->color.blue + (b->color.blue - a->color.blue) * t;
		}

		uint16_t *row = (uint16_t *)(buf->data + (size_t)y * buf->header.stride);
		for (int32_t x = 0; x < w; x++) {
			uint8_t th = BAYER4[y & 3][x & 3];
			uint16_t r = dither_channel(rgb[0], 31, th);
			uint16_t g = dither_channel(rgb[1], 63, th);
			uint16_t b = dither_channel(rgb[2], 31, th);
			row[x] = (uint16_t)((r << 11) | (g << 5) | b);
		}
	}
	return buf;
}

void sx_gradient_destroy(lv_draw_buf_t *strip) {
	if (strip) {
		lv_draw_buf_destroy(strip);
	}
}

// One tiled draw of the strip over `part` of the object, the tiles laid from
// the object's top left whatever part is drawn (image_area), so the bands
// meet without a seam. Both the area and the layer's clip are narrowed to the
// part: LVGL walks every line of every tile of the area it is given before it
// clips (the masked path), and blends whole tiles up to the clip and no
// further (the plain one), so either alone would draw too much -- the second
// would paint the strip square over the corners.
static void gradient_draw_part(lv_layer_t *layer, const lv_draw_buf_t *strip, const lv_area_t *obj_area, const lv_area_t *part, int32_t radius, lv_opa_t opa) {
	lv_area_t clipped;
	if (!lv_area_intersect(&clipped, part, &layer->_clip_area)) {
		return;
	}
	lv_draw_image_dsc_t dsc;
	lv_draw_image_dsc_init(&dsc);
	dsc.src = strip;
	dsc.tile = 1;
	dsc.opa = opa;
	dsc.image_area = *obj_area;
	dsc.clip_radius = radius;
	lv_area_t saved_clip = layer->_clip_area; // a task takes the clip it is made with
	layer->_clip_area = clipped;
	lv_draw_image(layer, &dsc, &clipped);
	layer->_clip_area = saved_clip;
}

static void chrome_edge_draw(lv_event_t *e) {
	sx_chrome_edge_t edge = (sx_chrome_edge_t)(intptr_t)lv_event_get_user_data(e);
	lv_obj_t *obj = lv_event_get_current_target_obj(e);
	lv_color_t color = edge == SX_CHROME_SHEET ? pal.surface_edge : pal.playback_edge;
	lv_opa_t opa = edge == SX_CHROME_SHEET ? pal.surface_edge_opa : edge == SX_CHROME_MINI ? pal.mini_edge_opa : edge == SX_CHROME_SHADE ? pal.shade_edge_opa : pal.floating_edge_opa;
	if (edge == SX_CHROME_TOAST) {
		if (!pal.edge_frame)
			return; // The ordinary contrasting notice already has its own boundary.
		color = sx_mix(lv_obj_get_style_text_color(obj, 0), lv_obj_get_style_bg_color(obj, 0), 60);
		opa = LV_OPA_COVER;
	}
	opa = LV_OPA_MIX2(opa, lv_obj_get_style_opa_recursive(obj, LV_PART_MAIN));
	if (opa <= LV_OPA_MIN)
		return;
	lv_area_t area;
	lv_obj_get_coords(obj, &area);
	int32_t r = lv_obj_get_style_radius(obj, LV_PART_MAIN);
	r = LV_MIN(r, LV_MIN(lv_area_get_width(&area), lv_area_get_height(&area)) / 2);
	lv_area_t band = area;
	if (!pal.edge_frame && edge != SX_CHROME_FLOATING) {
		if (edge == SX_CHROME_SHADE)
			band.y1 = area.y2 - r;
		else
			band.y2 = area.y1 + r;
	}
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_area_t clipped;
	if (!lv_area_intersect(&clipped, &band, &layer->_clip_area))
		return;
	lv_draw_rect_dsc_t dsc;
	lv_draw_rect_dsc_init(&dsc);
	dsc.bg_opa = LV_OPA_TRANSP;
	dsc.border_width = 1;
	dsc.border_color = color;
	dsc.border_opa = opa;
	dsc.radius = r;
	// A top-only LVGL border does not trace the complete quarter circles.
	// Reuse the rounded-border renderer, clipped to the actual edge band.
	lv_area_t saved_clip = layer->_clip_area;
	layer->_clip_area = clipped;
	lv_draw_rect(layer, &dsc, &area);
	layer->_clip_area = saved_clip;
}

void sx_chrome_edge(lv_obj_t *obj, sx_chrome_edge_t edge) {
	if (!obj)
		return;
	lv_obj_remove_event_cb(obj, chrome_edge_draw);
	lv_obj_add_event_cb(obj, chrome_edge_draw, LV_EVENT_DRAW_POST, (void *)(intptr_t)edge);
}

static void gradient_paint_cb(lv_event_t *e) {
	lv_draw_buf_t *const *slot = lv_event_get_user_data(e);
	const lv_draw_buf_t *strip = slot ? *slot : NULL;
	if (!strip) {
		return;
	}
	lv_obj_t *obj = lv_event_get_current_target_obj(e);
	lv_opa_t opa = lv_obj_get_style_bg_image_opa(obj, LV_PART_MAIN);
	if (opa <= LV_OPA_MIN || lv_obj_get_style_bg_opa(obj, LV_PART_MAIN) <= LV_OPA_MIN) {
		return;
	}
	// As LVGL's own background would be: faded with the object and its parents.
	lv_opa_t whole = lv_obj_get_style_opa_recursive(obj, LV_PART_MAIN);
	if (whole < LV_OPA_MAX) {
		opa = LV_OPA_MIX2(opa, whole);
	}
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_area_t area;
	lv_obj_get_coords(obj, &area);

	// The bands that hold the corners go through the radius mask, the middle
	// as plain copies.
	int32_t h = lv_area_get_height(&area);
	int32_t w = lv_area_get_width(&area);
	int32_t r = lv_obj_get_style_radius(obj, LV_PART_MAIN);
	if (r > w / 2) {
		r = w / 2;
	}
	if (r > h / 2) {
		r = h / 2;
	}
	if (r <= 0) {
		gradient_draw_part(layer, strip, &area, &area, 0, opa);
		return;
	}
	lv_area_t top = area, mid = area, bottom = area;
	top.y2 = area.y1 + r - 1;
	mid.y1 = top.y2 + 1;
	mid.y2 = area.y2 - r;
	bottom.y1 = mid.y2 + 1;
	gradient_draw_part(layer, strip, &area, &top, r, opa);
	if (mid.y1 <= mid.y2) {
		gradient_draw_part(layer, strip, &area, &mid, 0, opa);
	}
	gradient_draw_part(layer, strip, &area, &bottom, r, opa);
}

void sx_gradient_paint(lv_obj_t *obj, lv_draw_buf_t *const *slot) {
	if (obj) {
		lv_obj_add_event_cb(obj, gradient_paint_cb, LV_EVENT_DRAW_MAIN, (void *)slot);
	}
}

lv_color_t sx_palette_recommended_accent(void) {
	if (!cfg_family)
		return sx_hex(cfg_dark ? SX_D_ACCENT : SX_L_ACCENT);
	const family_t *f = &families[cfg_family - 1];
	if (f->recommended[cfg_dark])
		return family_hex(f->recommended[cfg_dark]);
	float h = f->flags & FAMILY_LCD ? lamp_hues[cfg_lamp] : f->hue;
	float c = f->flags & FAMILY_LCD && lamp_chroma[cfg_lamp] == 0 ? 0 : f->chroma;
	return sx_oklch(cfg_dark ? .76f : .46f, c, h);
}

lv_color_t sx_placeholder_category(float hue) {
	if (cfg_family || cfg_force_color)
		return sx_placeholder_tone(hue);
	lv_color_t color = sx_hex(SX_CATEGORY_BG);
	return cfg_monochrome ? sx_artview_gray_color(color) : color;
}
