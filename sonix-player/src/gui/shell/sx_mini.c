#include "sx_mini.h"
#include "artview.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/main_menu.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/sx_frame.h"
#include "src/gui/shell/sx_palette.h"
#include "src/gui/shell/sx_styles.h"
#include "src/gui/shell/sx_tokens.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"

static lv_obj_t *slot;		// the 128 px band; takes the gestures
static lv_obj_t *card;		// the 112 px accent card in it
static lv_obj_t *cover;		// the tone plaque...
static lv_obj_t *cover_img; // ...and the thumbnail over it
static lv_obj_t *title;
static lv_obj_t *artist;
static lv_obj_t *text_box;
static lv_obj_t *compact_line;
static char title_text[512], artist_text[256];
static int mini_mode;
static bool layout_compact;
static int frame_width, frame_height;
static lv_indev_t *mini_press_indev;
static void apply_layout(void);
static void cancel_press(void);
static void refresh_theme(void);
static void seek_hide(void);
static lv_obj_t *cover_glyph;
static lv_obj_t *play_disc;
static lv_obj_t *progress_track;
static lv_obj_t *play_area;
static lv_obj_t *play_icon;
static lv_obj_t *progress_fill;
static int progress_w; // the track's width, the fill's 100 %
static double progress_fraction;
static lv_obj_t *seek_panel, *seek_bar, *seek_fill, *seek_knob;
static lv_obj_t *seek_title, *seek_total, *seek_hint, *seek_bubble, *seek_time, *seek_delta;
static player_mini_seek_t seek_range;
static double seek_target;
static bool seek_active, press_moved;
static bool pull_active;

static bool has_track;
static bool live;
static bool playing;
static bool player_open;
static bool shown;

#define HIDDEN_SCREENS_MAX 16
static lv_obj_t *hidden_screens[HIDDEN_SCREENS_MAX];
static int hidden_screen_count;

lv_obj_t *sx_mini_obj(void) { return slot; }

bool sx_mini_visible(void) { return shown; }

bool sx_mini_has_track(void) { return has_track; }

int sx_mini_mode(void) { return mini_mode; }

static bool compact_mode(void) { return mini_mode == SX_MINI_COMPACT && lv_screen_active() != main_menu_screen; }

int sx_mini_slot_height(void) { return compact_mode() ? SX_MINI_COMPACT_SLOT_H : SX_MINI_SLOT_H; }

bool sx_mini_wants_cover(void) { return shown && !compact_mode(); }

bool sx_mini_set_mode(int mode) {
	mode = mode == SX_MINI_COMPACT ? SX_MINI_COMPACT : SX_MINI_NORMAL;
	if (mode == mini_mode)
		return false;
	cancel_press();
	mini_mode = mode;
	if (slot) {
		apply_layout();
		player_mini_mode_changed();
		sx_frame_slot_changed();
	}
	config_set_int("ui", "mini_player", mode);
	config_save();
	return true;
}

static bool screen_hides_mini(lv_obj_t *screen) {
	for (int i = 0; i < hidden_screen_count; i++) {
		if (hidden_screens[i] == screen) {
			return true;
		}
	}
	return false;
}

void sx_mini_hide_on_screen(lv_obj_t *screen) {
	if (!screen || screen_hides_mini(screen) || hidden_screen_count >= HIDDEN_SCREENS_MAX) {
		return;
	}
	hidden_screens[hidden_screen_count++] = screen;
	sx_mini_update_visibility();
}

static lv_obj_t *input_overlay_screen;

void sx_mini_input_overlay(lv_obj_t *screen, bool active) {
	if (active) {
		input_overlay_screen = screen;
	} else if (input_overlay_screen == screen) {
		input_overlay_screen = NULL;
	}
	sx_mini_update_visibility();
}

void sx_mini_update_visibility(void) {
	if (!slot) {
		return;
	}
	lv_obj_t *screen = lv_screen_active();
	bool want = has_track && !player_open && !screen_hides_mini(screen) && !keyboard_shown_on(screen) && screen != input_overlay_screen;
	bool changed = layout_compact != compact_mode();
	if (want == shown && !changed) {
		return;
	}
	if ((!want || changed) && !pull_active)
		cancel_press();
	if (changed) {
		apply_layout();
	}
	shown = want;
	lv_obj_set_hidden(slot, !want);
	player_mini_mode_changed();
	sx_frame_slot_changed();
}

void sx_mini_player_shown(bool open) {
	player_open = open;
	sx_mini_update_visibility();
}

// ---------------------------------------------------------------------------
// What it shows
// ---------------------------------------------------------------------------

static bool remember_text(char *stored, size_t size, const char *value) {
	const char *source = value ? value : "";
	size_t count = strlen(source);
	if (count >= size) {
		count = size - 1;
		while (count && ((unsigned char)source[count] & 0xc0) == 0x80)
			count--;
	}
	if (strlen(stored) == count && memcmp(stored, source, count) == 0)
		return false;
	memcpy(stored, source, count);
	stored[count] = 0;
	return true;
}

static void compact_text(void) {
	if (!compact_line || !compact_mode())
		return;
	const char *name = title_text;
	const char *by = artist_text;
	// The source cache is canonical: LVGL DOT mode may alter display buffers.
	char line[sizeof(title_text) + sizeof(artist_text) + 5];
	snprintf(line, sizeof(line), "%s%s%s", name, name[0] && by[0] ? " \xC2\xB7 " : "", by);
	sx_set_text(compact_line, line);
}

void sx_mini_set_text(const char *track_title, const char *track_artist) {
	if (!slot) {
		return;
	}
	bool title_changed = remember_text(title_text, sizeof(title_text), track_title);
	bool artist_changed = remember_text(artist_text, sizeof(artist_text), track_artist);
	if (title_changed)
		lv_label_set_text(title, title_text);
	if (artist_changed)
		lv_label_set_text(artist, artist_text);
	lv_obj_set_hidden(artist, compact_mode() || !artist_text[0]);
	if (title_changed || artist_changed)
		compact_text();
}

void sx_mini_set_track(const char *track_title, const char *track_path, const char *track_artist, bool is_live) {
	if (!slot) {
		return;
	}
	if (seek_active)
		cancel_press();
	has_track = (track_path && track_path[0]) || is_live || (track_title && track_title[0]);
	live = is_live;
	sx_mini_set_text(track_title, track_artist);
	// A stream and a file can have the same playing state but different controls.
	lv_image_set_src(play_icon, playing ? (live ? &icon_stop : &icon_pause_small) : &icon_play_small);
	sx_mini_set_progress(0, 0);
	sx_mini_update_visibility();
	sx_frame_slot_changed(); // the idle cards follow whether anything is loaded
}

void sx_mini_set_playing(bool now_playing) {
	if (!play_icon || now_playing == playing) {
		return;
	}
	playing = now_playing;
	// A stream has no pause to come back from; the player shows stop there,
	// and so does this.
	lv_image_set_src(play_icon, playing ? (live ? &icon_stop : &icon_pause_small) : &icon_play_small);
}

void sx_mini_set_progress(double position, double total) {
	if (!progress_fill) {
		return;
	}
	if (seek_active && !player_mini_seek_valid(&seek_range))
		cancel_press();
	int w;
	if (live) {
		progress_fraction = 1;
		w = progress_w;
	} else if (!(total > 0) || !(position >= 0)) {
		progress_fraction = 0;
		w = 0;
	} else {
		double f = position / total;
		f = f < 0 ? 0 : (f > 1 ? 1 : f);
		progress_fraction = f;
		w = (int)(f * progress_w + 0.5);
	}
	// lv_obj_set_width() is a no-op for the same width: the line is redrawn
	// only when it grows by a pixel.
	lv_obj_set_width(progress_fill, w);
	lv_obj_set_hidden(progress_fill, w == 0);
}

void sx_mini_set_cover(const lv_image_dsc_t *thumbnail, uint32_t generation) {
	if (!cover_img || (thumbnail && !sx_mini_wants_cover())) {
		return;
	}
	if (thumbnail) {
		sx_artview_bind(cover_img, thumbnail, generation);
		if (thumbnail->header.w > 0) {
			lv_image_set_scale(cover_img, SX_MINI_COVER * LV_SCALE_NONE / thumbnail->header.w);
		}
		lv_obj_set_hidden(cover_img, false);
	} else {
		lv_obj_set_hidden(cover_img, true);
		sx_artview_detach(cover_img);
		lv_image_set_src(cover_img, NULL);
	}
	lv_obj_set_hidden(cover_glyph, thumbnail != NULL);
	sx_artview_refresh();
}

// ---------------------------------------------------------------------------
// Gestures
// ---------------------------------------------------------------------------

static lv_point_t press_at;
static bool press_tracking;

static void cancel_press(void) {
	if (pull_active) {
		pull_active = false;
		player_sheet_pull_end(false);
	}
	lv_indev_t *input = mini_press_indev;
	press_tracking = false;
	mini_press_indev = NULL;
	seek_hide();
	if (input && lv_indev_get_state(input) == LV_INDEV_STATE_PRESSED)
		lv_indev_wait_release(input);
}

static void seek_hide(void) {
	if (!seek_active)
		return;
	seek_active = false;
	lv_obj_set_hidden(seek_panel, true);
	lv_obj_set_hidden(seek_bubble, true);
	lv_obj_set_hidden(card, false);
	lv_obj_set_hidden(play_area, false);
}

static void seek_paint(int finger_x) {
	double fraction = (seek_target - seek_range.start) / seek_range.length;
	int width = frame_width - 2 * SX_MINI_MARGIN_SIDE - 36;
	int pixels = (int)(fraction * width + 0.5);
	lv_obj_set_width(seek_fill, pixels);
	lv_obj_set_x(seek_knob, 18 + pixels - 9);
	char text[40], stamp[24];
	formatDoubleSeconds(seek_target - seek_range.start, stamp, sizeof(stamp));
	sx_set_text(seek_time, stamp);
	double before = seek_range.position - seek_range.start;
	if (before < 0)
		before = 0;
	// Match the two displayed whole-second positions, including chapter time.
	double delta = (double)(uint32_t)(seek_target - seek_range.start) - (double)(uint32_t)before;
	formatDoubleSeconds(delta < 0 ? -delta : delta, stamp, sizeof(stamp));
	snprintf(text, sizeof(text), "%c%s", delta < 0 ? '-' : '+', stamp);
	sx_set_text(seek_delta, text);
	int x = finger_x - 68;
	if (x < SX_MINI_MARGIN_SIDE)
		x = SX_MINI_MARGIN_SIDE;
	if (x > frame_width - SX_MINI_MARGIN_SIDE - 136)
		x = frame_width - SX_MINI_MARGIN_SIDE - 136;
	lv_obj_set_x(seek_bubble, x);
}

static void seek_build(void) {
	if (seek_panel)
		return;
	int w = frame_width, h = frame_height;
	// Temporary seek surfaces do not change the page's reserved bottom space.
	seek_panel = sx_box(lv_layer_top());
	lv_obj_add_style(seek_panel, &sx_style_mini, 0);
	lv_obj_set_size(seek_panel, w - 2 * SX_MINI_MARGIN_SIDE, SX_MINI_H);
	lv_obj_set_pos(seek_panel, SX_MINI_MARGIN_SIDE, h - SX_MINI_SLOT_H + SX_MINI_MARGIN_TOP);
	lv_obj_set_style_radius(seek_panel, SX_MINI_R, 0);
	sx_chrome_edge(seek_panel, SX_CHROME_MINI);
	seek_title = sx_label(seek_panel, &font_ui_24_bold, NULL);
	lv_obj_set_pos(seek_title, 18, 14);
	lv_obj_set_width(seek_title, w - 2 * SX_MINI_MARGIN_SIDE - 176);
	seek_total = sx_label(seek_panel, &font_ui_22, NULL);
	lv_obj_set_width(seek_total, 120);
	lv_obj_set_style_text_align(seek_total, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_set_pos(seek_total, w - 2 * SX_MINI_MARGIN_SIDE - 138, 16);
	seek_hint = sx_label(seek_panel, &font_ui_18, NULL);
	lv_obj_set_pos(seek_hint, 18, 50);
	lv_obj_set_width(seek_hint, w - 2 * SX_MINI_MARGIN_SIDE - 36);
	seek_bar = sx_box(seek_panel);
	lv_obj_set_size(seek_bar, w - 2 * SX_MINI_MARGIN_SIDE - 36, 6);
	lv_obj_set_pos(seek_bar, 18, SX_MINI_H - 23);
	lv_obj_set_style_bg_opa(seek_bar, LV_OPA_20, 0);
	lv_obj_set_style_radius(seek_bar, 3, 0);
	seek_fill = sx_box(seek_bar);
	lv_obj_set_size(seek_fill, 0, 6);
	lv_obj_set_style_bg_opa(seek_fill, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(seek_fill, 3, 0);
	seek_knob = sx_box(seek_panel);
	lv_obj_set_size(seek_knob, 18, 18);
	lv_obj_set_style_bg_opa(seek_knob, LV_OPA_COVER, 0);
	lv_obj_set_y(seek_knob, SX_MINI_H - 29);
	lv_obj_set_style_radius(seek_knob, LV_RADIUS_CIRCLE, 0);
	seek_bubble = sx_box(lv_layer_top());
	lv_obj_add_style(seek_bubble, &sx_style_mini, 0);
	lv_obj_set_size(seek_bubble, 136, 72);
	lv_obj_set_y(seek_bubble, h - SX_MINI_SLOT_H - 76);
	lv_obj_set_style_radius(seek_bubble, 12, 0);
	sx_chrome_edge(seek_bubble, SX_CHROME_FLOATING);
	seek_time = sx_label(seek_bubble, &font_ui_24_bold, NULL);
	lv_obj_set_width(seek_time, 124);
	lv_obj_set_style_text_align(seek_time, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_pos(seek_time, 6, 8);
	seek_delta = sx_label(seek_bubble, &font_ui_18, NULL);
	lv_obj_set_width(seek_delta, 124);
	lv_obj_set_style_text_align(seek_delta, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_pos(seek_delta, 6, 41);
	lv_obj_set_hidden(seek_panel, true);
	lv_obj_set_hidden(seek_bubble, true);
}

static void seek_show(void) {
	if (!player_mini_seek_begin(&seek_range))
		return;
	seek_build();
	refresh_theme();
	seek_active = true;
	seek_target = seek_range.position;
	if (seek_target < seek_range.start)
		seek_target = seek_range.start;
	if (seek_target >= seek_range.start + seek_range.length)
		seek_target = seek_range.start + seek_range.length - 0.001;
	char total[24];
	formatDoubleSeconds(seek_range.length, total, sizeof(total));
	sx_set_text(seek_total, total);
	sx_set_text(seek_title, tr("mini_seek_title"));
	sx_set_text(seek_hint, tr("mini_seek_release"));
	lv_obj_set_hidden(card, true);
	lv_obj_set_hidden(play_area, true);
	lv_obj_set_hidden(seek_panel, false);
	lv_obj_set_hidden(seek_bubble, false);
	lv_obj_move_foreground(seek_panel);
	lv_obj_move_foreground(seek_bubble);
	seek_paint(press_at.x);
}

static void open_player(void) {
	if (!player_sheet_is_open()) {
		switch_screen(player_screen);
	}
}

static void slot_event_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_indev_t *indev = lv_indev_active();
	if (!indev) {
		return;
	}
	lv_point_t p;
	lv_indev_get_point(indev, &p);

	if (code == LV_EVENT_PRESSED) {
		press_at = p;
		press_tracking = true;
		mini_press_indev = indev;
		press_moved = false;
		return;
	}
	int dx = p.x - press_at.x;
	int dy = p.y - press_at.y;
	if (code == LV_EVENT_PRESSING && press_tracking) {
		if (seek_active) {
			if (!player_mini_seek_valid(&seek_range) || p.y < frame_height - SX_MINI_SLOT_H - 24) {
				cancel_press();
				return;
			}
			int width = frame_width - 2 * SX_MINI_MARGIN_SIDE - 36;
			seek_target = seek_range.position + (double)dx * seek_range.length / width;
			if (seek_target < seek_range.start)
				seek_target = seek_range.start;
			if (seek_target >= seek_range.start + seek_range.length)
				seek_target = seek_range.start + seek_range.length - 0.001;
			seek_paint(p.x);
		} else if (LV_ABS(dx) >= SX_SWIPE_LOCK_PX || LV_ABS(dy) >= SX_SWIPE_LOCK_PX) {
			press_moved = true;
			if (!pull_active && dy < -SX_DRAG_LOCK_PX && -dy > LV_ABS(dx)) {
				pull_active = true; // Preserve this contact when the mini gives way.
				if (!player_sheet_pull_begin(sx_mini_slot_height()))
					pull_active = false;
			}
		}
		if (pull_active)
			player_sheet_pull_update(-dy);
		return;
	}
	if (code == LV_EVENT_LONG_PRESSED && press_tracking && !press_moved && !live) {
		seek_show();
		return;
	}
	if (code == LV_EVENT_PRESS_LOST) {
		if (pull_active) {
			pull_active = false;
			player_sheet_pull_end(false);
		}
		if (seek_active) {
			cancel_press();
			return;
		}
		mini_press_indev = NULL;
		press_tracking = false;
		return;
	}
	if (code != LV_EVENT_RELEASED || !press_tracking) {
		return;
	}
	press_tracking = false;
	mini_press_indev = NULL;
	if (pull_active) {
		pull_active = false;
		player_sheet_pull_end(dy < -SX_MINI_SWIPE_UP_PX && -dy > LV_ABS(dx));
		return;
	}
	if (seek_active) {
		double target = seek_target;
		seek_hide();
		if (player_mini_seek_commit(&seek_range, target))
			sx_mini_set_progress(target, seek_range.total);
		return;
	}
	if (dy < -SX_MINI_SWIPE_UP_PX && -dy > LV_ABS(dx)) {
		open_player();
	} else if (dx < -SX_MINI_SWIPE_PX) {
		player_screen_next();
	} else if (dx > SX_MINI_SWIPE_PX) {
		player_screen_prev();
	} else if (LV_ABS(dx) < SX_SWIPE_LOCK_PX && LV_ABS(dy) < SX_SWIPE_LOCK_PX) {
		open_player();
	}
}

static void play_clicked_cb(lv_event_t *e) {
	(void)e;
	if (shown && has_track)
		player_key_play_pause();
}

static void play_press_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	if (code == LV_EVENT_PRESSED)
		mini_press_indev = lv_indev_active();
	else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
		mini_press_indev = NULL;
}

// ---------------------------------------------------------------------------
// Building it
// ---------------------------------------------------------------------------

static void refresh_theme(void) {
	const sx_palette_t *p = sx_pal();
	sx_set_recolor(cover_glyph, sx_placeholder_ink(p->mini_cover_tone));
	lv_obj_set_style_text_color(title, p->mini_ink, 0);
	lv_obj_set_style_text_color(artist, p->mini_text2, 0);
	lv_obj_set_style_text_color(compact_line, p->mini_ink, 0);
	lv_obj_set_style_bg_color(play_disc, p->mini_action, 0);
	lv_obj_set_style_image_recolor(play_icon, p->mini_on_action, 0);
	lv_obj_set_style_border_color(cover, sx_mix(p->mini_ink, p->mini_bg, 65), 0);
	lv_obj_set_style_bg_color(progress_track, p->mini_ink, 0);
	lv_obj_set_style_bg_color(progress_fill, p->mini_action, 0);
	if (seek_panel) {
		lv_obj_t *labels[] = {seek_title, seek_total, seek_hint, seek_time, seek_delta};
		for (unsigned i = 0; i < sizeof(labels) / sizeof(labels[0]); i++)
			lv_obj_set_style_text_color(labels[i], p->mini_ink, 0);
		lv_obj_set_style_bg_color(seek_bar, p->mini_ink, 0);
		lv_obj_set_style_bg_color(seek_fill, p->mini_action, 0);
		lv_obj_set_style_bg_color(seek_knob, p->mini_action, 0);
	}
}

static void apply_layout(void) {
	bool compact = compact_mode();
	layout_compact = compact;
	int slot_h = sx_mini_slot_height();
	int card_h = compact ? SX_MINI_COMPACT_H : SX_MINI_H;
	lv_obj_set_size(slot, frame_width, slot_h);
	lv_obj_set_y(slot, frame_height - slot_h);
	lv_obj_set_height(card, card_h);
	progress_w = compact ? frame_width - 2 * SX_MINI_MARGIN_SIDE - SX_MINI_PROGRESS_INSET - 76 : frame_width - 2 * SX_MINI_MARGIN_SIDE - 2 * SX_MINI_PROGRESS_INSET;
	lv_obj_set_width(progress_track, progress_w);
	lv_obj_align(progress_track, LV_ALIGN_BOTTOM_LEFT, SX_MINI_PROGRESS_INSET - 12, -SX_MINI_PROGRESS_BOTTOM);
	lv_obj_set_width(progress_fill, (int)(progress_fraction * progress_w + 0.5));
	lv_obj_set_hidden(progress_fill, progress_fraction == 0);
	lv_obj_set_height(text_box, card_h);
	lv_obj_set_flex_grow(text_box, compact ? 0 : 1);
	lv_obj_set_width(text_box, compact ? frame_width - 2 * SX_MINI_MARGIN_SIDE - 24 - SX_MINI_COMPACT_TARGET : 0);
	lv_obj_set_hidden(cover, compact);
	lv_obj_set_hidden(title, compact);
	lv_obj_set_hidden(artist, compact || !artist_text[0]);
	lv_obj_set_hidden(compact_line, !compact);
	if (compact) {
		// The full 64 px pause target stays inside the reserved 72 px band.
		lv_obj_set_parent(play_area, slot);
		lv_obj_set_ignore_layout(play_area, true);
		lv_obj_set_size(play_area, SX_MINI_COMPACT_TARGET, SX_MINI_COMPACT_TARGET);
		lv_obj_set_pos(play_area, frame_width - 2 * SX_MINI_MARGIN_SIDE - SX_MINI_COMPACT_TARGET, (SX_MINI_COMPACT_SLOT_H - SX_MINI_COMPACT_TARGET) / 2 - SX_MINI_MARGIN_TOP);
	} else {
		// DOT mode may alter a label's display buffer; restore canonical text.
		lv_label_set_text(title, title_text);
		lv_label_set_text(artist, artist_text);
		lv_obj_set_parent(play_area, card);
		lv_obj_set_ignore_layout(play_area, false);
		lv_obj_set_pos(play_area, 0, 0);
		lv_obj_set_size(play_area, SX_MINI_PLAY_TARGET, SX_MINI_H);
		lv_obj_move_to_index(progress_track, -1);
	}
	int disc_h = compact ? SX_MINI_COMPACT_PLAY : SX_MINI_PLAY;
	lv_obj_set_size(play_disc, disc_h, disc_h);
	lv_obj_center(play_disc);
	lv_obj_center(play_icon);
	compact_text();
}

void sx_mini_init(gui_config_t *cfg) {
	if (slot) {
		return;
	}
	int w = (int)cfg->screen_width;
	int h = (int)cfg->screen_height;
	frame_width = w;
	frame_height = h;
	mini_mode = config_get_int("ui", "mini_player", SX_MINI_NORMAL) == SX_MINI_COMPACT ? SX_MINI_COMPACT : SX_MINI_NORMAL;

	slot = sx_box(lv_layer_top());
	lv_obj_set_size(slot, w, SX_MINI_SLOT_H);
	lv_obj_set_pos(slot, 0, h - SX_MINI_SLOT_H);
	// The margins around the card are the page's colour: a list scrolled
	// under the slot does not show through beside the card.
	lv_obj_add_style(slot, &sx_style_page, 0);
	lv_obj_set_style_pad_top(slot, SX_MINI_MARGIN_TOP, 0);
	lv_obj_set_style_pad_hor(slot, SX_MINI_MARGIN_SIDE, 0);
	lv_obj_set_style_pad_bottom(slot, SX_MINI_MARGIN_BOTTOM, 0);
	// The slot takes the presses: the margins around the card are part of the
	// target, and nothing under the slot is reachable while it is up anyway.
	lv_obj_set_clickable(slot, true);
	lv_obj_set_press_lock(slot, true);
	lv_obj_set_event_bubble(slot, false);
	lv_obj_add_event_cb(slot, slot_event_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(slot, slot_event_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(slot, slot_event_cb, LV_EVENT_LONG_PRESSED, NULL);
	lv_obj_add_event_cb(slot, slot_event_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(slot, slot_event_cb, LV_EVENT_PRESS_LOST, NULL);
	lv_obj_set_hidden(slot, true);

	card = sx_box(slot);
	lv_obj_set_size(card, lv_pct(100), SX_MINI_H);
	lv_obj_add_style(card, &sx_style_mini, 0);
	lv_obj_set_style_radius(card, SX_MINI_R, 0);
	lv_obj_set_style_clip_corner(card, false, 0);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(card, 12, 0);

	sx_chrome_edge(card, SX_CHROME_MINI);

	cover = sx_box(card);
	lv_obj_set_size(cover, SX_MINI_COVER, SX_MINI_COVER);
	lv_obj_add_style(cover, &sx_style_mini_tone, 0);
	lv_obj_set_style_radius(cover, SX_MINI_COVER_R, 0);
	lv_obj_set_style_clip_corner(cover, true, 0);
	lv_obj_set_style_border_width(cover, 1, 0);
	cover_glyph = lv_image_create(cover);
	lv_image_set_src(cover_glyph, &icon_music2);
	sx_set_recolor(cover_glyph, lv_color_white());
	lv_obj_center(cover_glyph);
	cover_img = lv_image_create(cover);
	lv_obj_center(cover_img);
	lv_obj_set_hidden(cover_img, true);

	lv_obj_t *text = sx_box(card);
	text_box = text;
	lv_obj_set_height(text, SX_MINI_H);
	lv_obj_set_flex_grow(text, 1);
	lv_obj_set_style_pad_hor(text, 12, 0);
	lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(text, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_row(text, 6, 0);

	title = sx_label(text, &font_ui_24_bold, NULL);
	lv_obj_set_width(title, lv_pct(100));
	lv_obj_set_style_text_color(title, lv_color_white(), 0);

	artist = sx_label(text, &font_ui_22, NULL);
	lv_obj_set_width(artist, lv_pct(100));

	compact_line = sx_label(text, &font_ui_22, NULL);
	lv_obj_set_width(compact_line, lv_pct(100));
	lv_obj_set_hidden(compact_line, true);

	// The disc sits in its own target; the standard geometry stays unchanged.
	play_area = lv_obj_create(card);
	lv_obj_remove_style_all(play_area);
	lv_obj_set_size(play_area, SX_MINI_PLAY_TARGET, SX_MINI_H);
	lv_obj_set_scrollable(play_area, false);
	lv_obj_set_event_bubble(play_area, false);
	lv_obj_add_event_cb(play_area, play_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(play_area, play_press_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(play_area, play_press_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(play_area, play_press_cb, LV_EVENT_PRESS_LOST, NULL);

	lv_obj_t *disc = sx_box(play_area);
	play_disc = disc;
	lv_obj_set_size(disc, SX_MINI_PLAY, SX_MINI_PLAY);
	lv_obj_center(disc);
	lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(disc, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
	lv_obj_set_event_bubble(disc, false);

	play_icon = lv_image_create(disc);
	lv_image_set_src(play_icon, &icon_play_small);
	lv_obj_set_style_image_recolor_opa(play_icon, LV_OPA_COVER, 0);
	lv_obj_center(play_icon);

	// The progress line: 3 px along the card's bottom edge, 18 px in from
	// each side, 4 px up.
	progress_w = w - 2 * SX_MINI_MARGIN_SIDE - 2 * SX_MINI_PROGRESS_INSET;
	lv_obj_t *track = sx_box(card);
	progress_track = track;
	lv_obj_set_ignore_layout(track, true);
	lv_obj_set_size(track, progress_w, SX_MINI_PROGRESS_H);
	lv_obj_align(track, LV_ALIGN_BOTTOM_MID, 0, -SX_MINI_PROGRESS_BOTTOM);
	lv_obj_set_style_radius(track, 2, 0);
	lv_obj_set_style_bg_color(track, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(track, LV_OPA_20, 0);

	progress_fill = sx_box(track);
	lv_obj_set_size(progress_fill, 0, SX_MINI_PROGRESS_H);
	lv_obj_add_style(progress_fill, &sx_style_np_accent, 0);
	lv_obj_set_style_radius(progress_fill, 2, 0);
	lv_obj_set_hidden(progress_fill, true);

	apply_layout();
	refresh_theme();
	theme_register_refresh(refresh_theme);
}
