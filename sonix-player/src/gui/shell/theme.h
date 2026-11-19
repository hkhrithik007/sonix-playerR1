#ifndef THEME_H
#define THEME_H

#include <stdbool.h>

#include "lvgl/lvgl.h"

// The handful of colours the whole UI is built from. Two presets (dark and
// light) live in theme.c; everything on screen pulls from the active one.
typedef struct {
	lv_color_t screen_bg;	   // page background
	lv_color_t surface;		   // list rows, cards
	lv_color_t surface_pressed; // the same, while a finger is on it
	lv_color_t panel;		   // status bar and the player's control block
	lv_color_t cover_bg;	   // behind the artwork, when a track has none
	lv_color_t text_primary;
	lv_color_t text_secondary;
	lv_color_t accent;		   // the colour of active things: the cover's, or a preset
	lv_color_t accent_text;	   // readable foreground, distinct from the action fill
	lv_color_t on_accent;	   // text and glyphs on an accent fill (white, except on a pale accent)
	bool dark;
} theme_palette_t;

// Shared styles. Widgets add these instead of setting colours themselves,
// which is what makes switching themes a one-liner: update the style, LVGL
// repaints everything using it.
extern lv_style_t theme_style_screen;
extern lv_style_t theme_style_panel;
extern lv_style_t theme_style_card;
extern lv_style_t theme_style_card_pressed;
extern lv_style_t theme_style_text;
extern lv_style_t theme_style_text_dim;
extern lv_style_t theme_style_icon;
extern lv_style_t theme_style_switch;
extern lv_style_t theme_style_switch_checked; // image_recolor for monochrome icons

// A plain accent-coloured fill. Anything that has to follow the accent colour
// without the page repainting it adds this instead of setting the colour by
// hand: the sliders' filled part, the pill buttons. A hand-set colour is a
// snapshot -- it keeps whichever accent was live when the page was built, and
// does not follow a later change.
extern lv_style_t theme_style_accent_bg;

// The unfilled part of a slider's track: the secondary text colour at low
// opacity, which reads as a quiet groove on both palettes.
extern lv_style_t theme_style_slider_track;

// Anything that can't be expressed as a style -- an icon recoloured at runtime,
// the blurred artwork behind the controls -- registers a callback here and gets
// called after every switch.
typedef void (*theme_refresh_cb_t)(void);
void theme_register_refresh(theme_refresh_cb_t cb);

// The ring that tells a slider's knob from what is behind it. In the dark
// theme a hairline of black is plenty; in the light one the cards are nearly
// white too, so a white knob needs a real grey outline not to vanish into
// them. Call it when the slider is built and from the page's theme-refresh
// callback -- the colour depends on the theme in force.
void theme_apply_slider_knob(lv_obj_t *slider);

// Builds the shared styles. Call once, before any screen is created.
void theme_init(void);

// Call with the page about to be put on screen. A theme, accent or tint change
// walks only the page the user is looking at; this is what walks a page that
// was hidden when the palette moved, and does nothing when it was not.
void theme_notify_screen_shown(lv_obj_t *screen);

const theme_palette_t *theme(void);
bool theme_is_dark(void);

// Switches between the two presets and repaints the UI.
void theme_toggle(void);

// Graphite (-2) is the default when the setting is absent. Cover (-1) and the
// six saved preset indices (0..5) keep their meanings. Gray is appended at 6.
#define THEME_ACCENT_PALETTE (-3)
#define THEME_PALETTE_COUNT 9
#define THEME_LCD_LAMP_COUNT 5
#define THEME_ACCENT_COUNT 7
#define THEME_ACCENT_GRAY 6
#define THEME_ACCENT_COVER (-1)
#define THEME_ACCENT_GRAPHITE (-2)
lv_color_t theme_accent_preset(int index);
int theme_get_accent(void);
void theme_set_accent(int index);

// Picks a specific theme rather than flipping; no-op when already active.
void theme_set_dark(bool dark);

// "Tint from cover" (Appearance; called Dynamic tint before 2.0, same config
// key): the neutral colours -- backgrounds, cards, the status bar -- take the
// hue of the cover playing (or of the album page shown), at the lightness the
// cover tokens give each of them. Off by default for Graphite; a saved
// preference is preserved.
//
// The hue itself moves without any call here: sx_palette_track_changed() and
// sx_palette_push_hue() recolour the interface in place, running the refresh
// callbacks above as a theme change does.
bool theme_monochrome_covers(void);
bool theme_set_monochrome_covers(bool on);
int theme_get_palette(void);
void theme_set_palette(int family);
int theme_get_lcd_lamp(void);
void theme_set_lcd_lamp(int lamp);
bool theme_force_color_placeholders(void);
void theme_set_force_color_placeholders(bool on);

bool theme_dynamic_tint(void);
void theme_set_dynamic_tint(bool on);

#endif // THEME_H
