#include "gearboysettings.h"

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/gearboy/gearboy.h"
#include "src/system/core/lang.h"

#include <stdint.h>

lv_obj_t *gearboysettings_screen;

// The two pill rows: 5 palettes and 4 shaders. Same shape as the MSEB radius
// and keyboard-type pills, but wrapped over several lines: five names do not
// fit across 480 pixels.
static lv_obj_t *palette_pills[5];
static lv_obj_t *shader_pills[4];
static lv_obj_t *correction_switch;
static lv_obj_t *bootrom_switch;

static const char *const PALETTE_NAMES[5] = {
	"gearboy_palette_original", "gearboy_palette_sharp", "gearboy_palette_bw", "gearboy_palette_autumn", "gearboy_palette_soft",
};
static const char *const SHADER_NAMES[4] = {
	"Pixel perfect", "GBC", "GBC Dot matrix", "GB Dot matrix",
};

// Repaints the pills: accent on the selected one, quiet on the rest.
static void refresh_pills(void) {
	int pal = (int)config_get_int("gearboy", "palette", 0);
	int sh = (int)config_get_int("gearboy", "shader", 0);
	for (int i = 0; i < 5; i++) {
		if (!palette_pills[i]) {
			continue;
		}
		bool on = i == pal;
		settingsrow_pill_active(palette_pills[i], on);
	}
	for (int i = 0; i < 4; i++) {
		if (!shader_pills[i]) {
			continue;
		}
		bool on = i == sh;
		settingsrow_pill_active(shader_pills[i], on);
	}
}

static void palette_pick_cb(lv_event_t *e) {
	config_set_int("gearboy", "palette", (int)(intptr_t)lv_event_get_user_data(e));
	config_save();
	gearboy_refresh_video_settings();
	refresh_pills();
}

static void shader_pick_cb(lv_event_t *e) {
	config_set_int("gearboy", "shader", (int)(intptr_t)lv_event_get_user_data(e));
	config_save();
	gearboy_refresh_video_settings();
	refresh_pills();
}

static void correction_toggle_cb(lv_event_t *e) {
	(void)e;
	config_set_int("gearboy", "gbc_correction", lv_obj_has_state(correction_switch, LV_STATE_CHECKED) ? 1 : 0);
	config_save();
	gearboy_refresh_video_settings();
}

static void bootrom_toggle_cb(lv_event_t *e) {
	(void)e;
	config_set_int("gearboy", "bootrom", lv_obj_has_state(bootrom_switch, LV_STATE_CHECKED) ? 1 : 0);
	config_save();
	// No refresh: the boot ROMs are only consulted when a game starts.
}

static lv_obj_t *make_pill(lv_obj_t *parent, const char *text, lv_event_cb_t cb, int value) { return settingsrow_pill(parent, text, value, cb); }

// A card with a title and a wrapping row of pills inside.
static lv_obj_t *make_pill_card(lv_obj_t *parent, const char *title_tag) {
	lv_obj_t *choices = NULL;
	settingsrow_pills(parent, title_tag, &choices);
	return choices;
}

void gearboysettings_init(gui_config_t *cfg) {
	gearboysettings_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(gearboysettings_screen, cfg, "gearboy_settings");

	// Palette: applies to original Game Boy games only, since GBC titles carry
	// their own colours.
	lv_obj_t *pal_pills = make_pill_card(container, "gearboy_palette");
	for (int i = 0; i < 5; i++) {
		palette_pills[i] = make_pill(pal_pills, PALETTE_NAMES[i], palette_pick_cb, i);
	}

	// Shaders: the real panels' grid, drawn inside the 3x upscale.
	lv_obj_t *sh_pills = make_pill_card(container, "gearboy_shaders");
	for (int i = 0; i < 4; i++) {
		shader_pills[i] = make_pill(sh_pills, SHADER_NAMES[i], shader_pick_cb, i);
	}

	settingsrow_toggle(container, "gearboy_gbc_color_correction", &correction_switch, correction_toggle_cb);
	if (config_get_int("gearboy", "gbc_correction", 1) != 0) {
		lv_obj_add_state(correction_switch, LV_STATE_CHECKED);
	}

	// Boot ROMs: the logo scrolling down at start-up, as on the real machine.
	// The toggle carries its instructions underneath, because without the two
	// files it does nothing and the user has to be told where to put them.
	settingsrow_toggle(container, "gearboy_show_boot_rom", &bootrom_switch, bootrom_toggle_cb);
	if (config_get_int("gearboy", "bootrom", 0) != 0) {
		lv_obj_add_state(bootrom_switch, LV_STATE_CHECKED);
	}
	lv_obj_t *note = lv_label_create(container);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_20, 0);
	lv_label_set_text(note, tr("gearboy_boot_rom_note"));

	refresh_pills();
	theme_register_refresh(refresh_pills);
	switcher_attach_back_gesture(gearboysettings_screen);
}
