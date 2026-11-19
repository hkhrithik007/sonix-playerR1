#include "main_menu.h"

#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/library/medialist.h"
#include "src/gui/library/music.h"
#include "src/gui/library/search.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/morepage.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/settings/settings.h"
#include "src/gui/streaming/streaming.h"
#include "src/gui/library/audiobooks.h"
#include "src/gui/wireless/wireless.h"
#include "src/system/audio/audio.h"
#include "src/system/core/lang.h"
#include "src/system/playback/device_state.h"

#define HOME_GAP 10
#define HOME_TILE_HEIGHT 68
#define HOME_NAV_HEIGHT 58
#define HOME_PLAYER_HEIGHT 68
#define HOME_PLAYER_BOTTOM 66
#define HOME_SWIPE_PX 42
#define HOME_PLAYER_REVEAL_MS 150

lv_obj_t *main_menu_screen;

static lv_obj_t *home_scroll;
static lv_obj_t *mini_player;
static lv_obj_t *mini_cover;
static lv_obj_t *mini_title;
static lv_obj_t *mini_artist;
static lv_obj_t *mini_progress;
static lv_obj_t *mini_play_icon;
static lv_obj_t *mini_nav;
static lv_obj_t *home_brand;
static lv_point_t mini_press;
static bool mini_swipe;
static bool mini_seek_active;
static double mini_seek_start;
static double mini_seek_target;
static double mini_seek_total;
static audio_status_t mini_status = AUDIO_STATUS_STOPPED;
static cover_image_t mini_cover_copy;
static char mini_cover_key[1400];
static bool mini_cover_ready;
static bool mini_was_visible;
static lv_obj_t *home_nav_icon;
static lv_obj_t *home_nav_label;
static bool home_visual_cache_valid;
static lv_color_t home_accent_cache;
static lv_color_t home_bg_cache;

static bool colors_equal(lv_color_t a, lv_color_t b) {
	return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

// Carry the player's cover-derived accent onto the home screen as a soft
// header wash and a fine mini-player outline/progress.
static void home_visual_refresh(bool force) {
	lv_color_t accent = player_cover_accent_color();
	lv_color_t background = theme()->screen_bg;
	if (!force && home_visual_cache_valid && colors_equal(accent, home_accent_cache) &&
		colors_equal(background, home_bg_cache)) {
		return;
	}
	home_visual_cache_valid = true;
	home_accent_cache = accent;
	home_bg_cache = background;

	lv_color_t wash = lv_color_mix(accent, background, theme()->dark ? 24 : 12);
	lv_obj_set_style_bg_color(main_menu_screen, wash, 0);
	lv_obj_set_style_bg_grad_color(main_menu_screen, background, 0);
	lv_obj_set_style_bg_grad_dir(main_menu_screen, LV_GRAD_DIR_VER, 0);
	lv_obj_set_style_bg_main_stop(main_menu_screen, 0, 0);
	lv_obj_set_style_bg_grad_stop(main_menu_screen, 190, 0);

	if (mini_player) {
		lv_obj_set_style_border_color(mini_player, accent, 0);
		lv_obj_set_style_bg_color(mini_progress, accent, LV_PART_INDICATOR);
		lv_obj_set_style_image_recolor(mini_play_icon, accent, 0);
		lv_obj_set_style_image_recolor_opa(mini_play_icon, LV_OPA_COVER, 0);
	}
}

static void open_favourites(void) {
	medialist_open(tr("favourites"), LIBRARY_LIST_FAVOURITES, LIBRARY_FILTER_NONE, NULL);
}

static void quick_action_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	void (*action)(void) = (void (*)(void))lv_event_get_user_data(e);
	action();
}

static void nav_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	lv_obj_t *target = lv_event_get_user_data(e);
	if (target != lv_screen_active()) {
		switch_screen(target);
	}
}

static void search_nav_cb(lv_event_t *e) {
	(void)e;
	if (!player_sheet_drag_active() && !switcher_back_drag_active()) {
		search_open();
	}
}

static void add_shortcut(lv_obj_t *grid, gui_config_t *cfg, const char *label, const lv_image_dsc_t *icon,
						 lv_obj_t *target, void (*action)(void)) {
	lv_obj_t *tile = lv_btn_create(grid);
	int width = (cfg->screen_width - 2 * cfg->padding - HOME_GAP) / 2;
	lv_obj_set_size(tile, width, HOME_TILE_HEIGHT);
	lv_obj_add_style(tile, &theme_style_card, 0);
	lv_obj_add_style(tile, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(tile, 12, 0);
	lv_obj_set_style_border_width(tile, 0, 0);
	lv_obj_set_style_shadow_width(tile, 0, 0);
	lv_obj_set_style_pad_hor(tile, 10, 0);
	lv_obj_set_style_pad_column(tile, 10, 0);
	lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_event_bubble(tile, true);

	lv_obj_t *image = lv_image_create(tile);
	lv_image_set_src(image, icon);
	lv_obj_set_size(image, 42, 42);
	lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CENTER);

	lv_obj_t *text = lv_label_create(tile);
	lv_label_set_text(text, tr(label));
	lv_obj_set_width(text, width - 78);
	lv_label_set_long_mode(text, LV_LABEL_LONG_DOT);
	lv_obj_add_style(text, &theme_style_text, 0);
	lv_obj_set_style_text_font(text, &font_ui_20_bold, 0);

	if (target) {
		lv_obj_add_event_cb(tile, switch_screen_cb, LV_EVENT_CLICKED, target);
	} else {
		lv_obj_add_event_cb(tile, quick_action_cb, LV_EVENT_CLICKED, (void *)action);
	}
}

static void mini_play_cb(lv_event_t *e) {
	lv_event_stop_bubbling(e);
	if (!player_sheet_drag_active() && !switcher_back_drag_active()) {
		player_key_play_pause();
	}
}

static void mini_open_cb(lv_event_t *e) {
	(void)e;
	if (!mini_swipe && !player_sheet_drag_active() && !switcher_back_drag_active()) {
		player_sheet_open(true);
	}
	mini_swipe = false;
}

static void mini_gesture_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_indev_t *indev = lv_indev_active();
	if (code == LV_EVENT_LONG_PRESSED) {
		device_state_t state;
		device_state_get(&state);
		if (state.progress_total_secs > 0.0 && state.current_file[0]) {
			mini_seek_active = true;
			mini_swipe = true;
			mini_seek_start = state.progress_current_secs;
			mini_seek_target = mini_seek_start;
			mini_seek_total = state.progress_total_secs;
		}
		return;
	}
	if (!indev) {
		return;
	}
	if (code == LV_EVENT_PRESSED) {
		lv_indev_get_point(indev, &mini_press);
		mini_swipe = false;
		mini_seek_active = false;
		return;
	}
	if (code == LV_EVENT_PRESSING) {
		lv_point_t point;
		lv_indev_get_point(indev, &point);
		int dx = point.x - mini_press.x;
		int dy = point.y - mini_press.y;
		if (mini_seek_active) {
			int width = lv_obj_get_width(mini_player);
			mini_seek_target = mini_seek_start + (width > 0 ? (double)dx * mini_seek_total / width : 0.0);
			if (mini_seek_target < 0.0) mini_seek_target = 0.0;
			if (mini_seek_target > mini_seek_total) mini_seek_target = mini_seek_total;
			int now = (int)mini_seek_target;
			int end = (int)mini_seek_total;
			char preview[48];
			snprintf(preview, sizeof(preview), "%d:%02d / %d:%02d", now / 60, now % 60, end / 60, end % 60);
			lv_label_set_text(mini_artist, preview);
			lv_event_stop_bubbling(e);
			return;
		}
		mini_swipe = LV_ABS(dx) > HOME_SWIPE_PX || dy < -HOME_SWIPE_PX;
		if (mini_swipe) {
			lv_event_stop_bubbling(e);
		}
		return;
	}
	if (code != LV_EVENT_RELEASED) {
		if (code == LV_EVENT_PRESS_LOST) {
			mini_swipe = false;
			mini_seek_active = false;
		}
		return;
	}
	lv_point_t point;
	lv_indev_get_point(indev, &point);
	int dx = point.x - mini_press.x;
	int dy = point.y - mini_press.y;
	if (mini_seek_active) {
		device_state_seek(mini_seek_target);
		mini_seek_active = false;
		mini_swipe = true;
	} else if (dy < -HOME_SWIPE_PX && LV_ABS(dy) > LV_ABS(dx)) {
		mini_swipe = true;
		player_sheet_open(true);
	} else if (LV_ABS(dx) > HOME_SWIPE_PX && LV_ABS(dx) > LV_ABS(dy)) {
		mini_swipe = true;
		dx < 0 ? player_key_next() : player_key_prev();
	}
	lv_event_stop_bubbling(e);
}

static void mini_reveal_opa_cb(void *obj, int32_t value) {
	lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void mini_reveal_y_cb(void *obj, int32_t value) {
	lv_obj_set_style_translate_y((lv_obj_t *)obj, value, 0);
}

static void mini_reveal(void) {
	lv_anim_delete(mini_player, mini_reveal_opa_cb);
	lv_anim_delete(mini_player, mini_reveal_y_cb);
	lv_obj_set_hidden(mini_player, false);
	lv_obj_set_style_opa(mini_player, LV_OPA_TRANSP, 0);
	lv_obj_set_style_translate_y(mini_player, 8, 0);

	lv_anim_t fade;
	lv_anim_init(&fade);
	lv_anim_set_var(&fade, mini_player);
	lv_anim_set_exec_cb(&fade, mini_reveal_opa_cb);
	lv_anim_set_values(&fade, LV_OPA_TRANSP, LV_OPA_COVER);
	lv_anim_set_duration(&fade, HOME_PLAYER_REVEAL_MS);
	lv_anim_set_path_cb(&fade, lv_anim_path_ease_out);
	lv_anim_start(&fade);

	lv_anim_t rise;
	lv_anim_init(&rise);
	lv_anim_set_var(&rise, mini_player);
	lv_anim_set_exec_cb(&rise, mini_reveal_y_cb);
	lv_anim_set_values(&rise, 8, 0);
	lv_anim_set_duration(&rise, HOME_PLAYER_REVEAL_MS);
	lv_anim_set_path_cb(&rise, lv_anim_path_ease_out);
	lv_anim_start(&rise);
}

static void mini_player_update(lv_timer_t *timer) {
	(void)timer;
	device_state_t state;
	device_state_get(&state);
	bool loaded = state.current_file[0] || state.live;
	bool visible = loaded && !player_sheet_is_open();
	if (visible && !mini_was_visible) {
		mini_reveal();
	} else if (!visible) {
		lv_anim_delete(mini_player, mini_reveal_opa_cb);
		lv_anim_delete(mini_player, mini_reveal_y_cb);
		lv_obj_set_style_opa(mini_player, LV_OPA_COVER, 0);
		lv_obj_set_style_translate_y(mini_player, 0, 0);
		lv_obj_set_hidden(mini_player, true);
	}
	mini_was_visible = visible;
	if (!loaded) {
		home_visual_refresh(false);
		return;
	}

	const char *title = state.metadata.title;
	if (!title[0] && state.current_file[0]) {
		const char *base = strrchr(state.current_file, '/');
		title = base ? base + 1 : state.current_file;
	}
	lv_label_set_text(mini_title, title[0] ? title : tr("quickpanel_no_track"));
	lv_label_set_text(mini_artist, state.metadata.artist[0] ? state.metadata.artist : tr("music"));

	char cover_key[sizeof(mini_cover_key)];
	snprintf(cover_key, sizeof(cover_key), "%s\n%s\n%s\n%s", state.current_file, state.live_cover,
			 state.metadata.title, state.metadata.artist);
	if (strcmp(cover_key, mini_cover_key) != 0) {
		lv_image_set_src(mini_cover, &icon_music_note);
		cover_free(&mini_cover_copy);
		snprintf(mini_cover_key, sizeof(mini_cover_key), "%s", cover_key);
		mini_cover_ready = false;
	}
	if (!mini_cover_ready) {
		uint64_t cover_id;
		if (player_cover_copy(50, &mini_cover_copy, &cover_id)) {
			(void)cover_id;
			lv_image_set_src(mini_cover, &mini_cover_copy.dsc);
			lv_obj_set_size(mini_cover, 50, 50);
			lv_image_set_inner_align(mini_cover, LV_IMAGE_ALIGN_STRETCH);
			mini_cover_ready = true;
		}
	}
	if (state.status != mini_status) {
		mini_status = state.status;
		lv_image_set_src(mini_play_icon, mini_status == AUDIO_STATUS_PLAYING ? &icon_pause : &icon_play);
	}
	int percent = state.progress_total_secs > 0.0
				  ? (int)(100.0 * state.progress_current_secs / state.progress_total_secs)
				  : 0;
	if (percent < 0) percent = 0;
	if (percent > 100) percent = 100;
	lv_bar_set_value(mini_progress, percent, LV_ANIM_OFF);
	home_visual_refresh(false);
}

static void home_theme_refresh(void) {
	if (mini_player) {
		lv_obj_set_style_bg_color(mini_player, theme()->surface, 0);
		lv_obj_set_style_bg_color(mini_nav, theme()->screen_bg, 0);
		lv_obj_set_style_bg_color(mini_progress, theme()->text_secondary, LV_PART_MAIN);
		home_visual_refresh(true);
	}
	if (home_brand) {
		lv_obj_set_style_text_color(home_brand, theme()->accent, 0);
	}
	if (home_nav_icon) {
		lv_obj_set_style_image_recolor(home_nav_icon, theme()->accent, 0);
		lv_obj_set_style_image_recolor_opa(home_nav_icon, LV_OPA_COVER, 0);
		lv_obj_set_style_text_color(home_nav_label, theme()->text_primary, 0);
	}
}

static lv_obj_t *add_nav_item(lv_obj_t *bar, const char *label, const lv_image_dsc_t *icon, lv_obj_t *target) {
	lv_obj_t *button = lv_btn_create(bar);
	lv_obj_set_size(button, LV_PCT(33), HOME_NAV_HEIGHT);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_bg_opa(button, LV_OPA_10, LV_STATE_PRESSED);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 2, 0);
	lv_obj_set_flex_flow(button, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(button, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_event_cb(button, target ? nav_cb : search_nav_cb, LV_EVENT_CLICKED, target);
	lv_obj_t *image = lv_image_create(button);
	lv_image_set_src(image, icon);
	lv_obj_add_style(image, &theme_style_icon, 0);
	lv_obj_t *text = lv_label_create(button);
	lv_label_set_text(text, tr(label));
	lv_obj_add_style(text, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(text, &font_ui_14, 0);
	if (target == main_menu_screen) {
		home_nav_icon = image;
		home_nav_label = text;
	}
	return button;
}

static void build_mini_player(gui_config_t *cfg) {
	mini_player = lv_obj_create(main_menu_screen);
	lv_obj_set_size(mini_player, cfg->screen_width - 16, HOME_PLAYER_HEIGHT);
	lv_obj_align(mini_player, LV_ALIGN_BOTTOM_MID, 0, -HOME_PLAYER_BOTTOM);
	lv_obj_set_style_bg_color(mini_player, theme()->surface, 0);
	lv_obj_set_style_bg_opa(mini_player, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(mini_player, 14, 0);
	lv_obj_set_style_border_width(mini_player, 1, 0);
	lv_obj_set_style_border_opa(mini_player, LV_OPA_20, 0);
	lv_obj_set_style_pad_all(mini_player, 8, 0);
	lv_obj_set_style_pad_column(mini_player, 10, 0);
	lv_obj_set_scrollable(mini_player, false);
	lv_obj_set_clickable(mini_player, true);
	lv_obj_set_flex_flow(mini_player, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(mini_player, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_event_cb(mini_player, mini_gesture_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(mini_player, mini_gesture_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(mini_player, mini_gesture_cb, LV_EVENT_LONG_PRESSED, NULL);
	lv_obj_add_event_cb(mini_player, mini_gesture_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(mini_player, mini_gesture_cb, LV_EVENT_PRESS_LOST, NULL);
	lv_obj_add_event_cb(mini_player, mini_open_cb, LV_EVENT_CLICKED, NULL);

	mini_cover = lv_image_create(mini_player);
	lv_obj_set_size(mini_cover, 50, 50);
	lv_image_set_src(mini_cover, &icon_music_note);
	lv_obj_set_event_bubble(mini_cover, true);

	lv_obj_t *labels = lv_obj_create(mini_player);
	lv_obj_remove_style_all(labels);
	// Let the flex row assign the space between the cover and play button.
	// A content-sized width can otherwise grow to the full song title and push
	// the play control off the card on long track names.
	lv_obj_set_width(labels, 0);
	lv_obj_set_height(labels, LV_PCT(100));
	lv_obj_set_flex_grow(labels, 1);
	lv_obj_set_flex_flow(labels, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(labels, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_row(labels, 2, 0);
	lv_obj_set_scrollable(labels, false);
	lv_obj_set_clickable(labels, false);
	lv_obj_set_event_bubble(labels, true);
	mini_title = lv_label_create(labels);
	lv_obj_set_width(mini_title, LV_PCT(100));
	// LV_LABEL_LONG_DOT wraps before truncating. Pin both labels to one line so
	// a long title cannot shove the artist out of the mini-player.
	lv_obj_set_height(mini_title, lv_font_get_line_height(&font_ui_20_bold));
	lv_obj_set_event_bubble(mini_title, true);
	lv_label_set_long_mode(mini_title, LV_LABEL_LONG_DOT);
	lv_obj_add_style(mini_title, &theme_style_text, 0);
	lv_obj_set_style_text_font(mini_title, &font_ui_20_bold, 0);
	mini_artist = lv_label_create(labels);
	lv_obj_set_width(mini_artist, LV_PCT(100));
	lv_obj_set_height(mini_artist, lv_font_get_line_height(&font_ui_14));
	lv_obj_set_event_bubble(mini_artist, true);
	lv_label_set_long_mode(mini_artist, LV_LABEL_LONG_DOT);
	lv_obj_add_style(mini_artist, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(mini_artist, &font_ui_14, 0);

	lv_obj_t *play = lv_btn_create(mini_player);
	lv_obj_set_size(play, 42, 48);
	lv_obj_set_style_bg_opa(play, LV_OPA_TRANSP, 0);
	lv_obj_set_style_bg_color(play, theme()->accent, LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(play, LV_OPA_20, LV_STATE_PRESSED);
	lv_obj_set_style_radius(play, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_border_width(play, 0, 0);
	lv_obj_set_style_shadow_width(play, 0, 0);
	lv_obj_set_style_pad_all(play, 0, 0);
	lv_obj_set_event_bubble(play, false);
	lv_obj_add_event_cb(play, mini_play_cb, LV_EVENT_CLICKED, NULL);
	mini_play_icon = lv_image_create(play);
	lv_image_set_src(mini_play_icon, &icon_play);
	lv_obj_add_style(mini_play_icon, &theme_style_icon, 0);
	lv_obj_center(mini_play_icon);

	mini_progress = lv_bar_create(mini_player);
	lv_obj_set_ignore_layout(mini_progress, true);
	lv_obj_set_size(mini_progress, LV_PCT(100), 3);
	lv_bar_set_range(mini_progress, 0, 100);
	lv_obj_set_style_bg_color(mini_progress, theme()->text_secondary, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(mini_progress, LV_OPA_30, LV_PART_MAIN);
	lv_obj_set_style_bg_color(mini_progress, theme()->accent, LV_PART_INDICATOR);
	lv_obj_set_style_radius(mini_progress, LV_RADIUS_CIRCLE, LV_PART_MAIN);
	lv_obj_set_style_radius(mini_progress, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
	lv_obj_set_clickable(mini_progress, false);
	lv_obj_set_event_bubble(mini_progress, true);
	lv_obj_align(mini_progress, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_move_foreground(mini_progress);

	mini_nav = lv_obj_create(main_menu_screen);
	lv_obj_set_size(mini_nav, cfg->screen_width, HOME_NAV_HEIGHT);
	lv_obj_align(mini_nav, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_style_bg_color(mini_nav, theme()->screen_bg, 0);
	lv_obj_set_style_bg_opa(mini_nav, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(mini_nav, 0, 0);
	lv_obj_set_style_pad_all(mini_nav, 0, 0);
	lv_obj_set_flex_flow(mini_nav, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(mini_nav, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(mini_nav, false);
	add_nav_item(mini_nav, "home", &icon_nav_home, main_menu_screen);
	add_nav_item(mini_nav, "search_tab", &icon_search, NULL);
	add_nav_item(mini_nav, "library_tab", &icon_nav_library, music_screen);

	theme_register_refresh(home_theme_refresh);
	home_theme_refresh();
	lv_timer_create(mini_player_update, 500, NULL);
	mini_player_update(NULL);
}

void main_menu_init(gui_config_t *cfg) {
	lv_obj_add_style(main_menu_screen, &theme_style_screen, 0);
	int top = cfg->top_bar_height + 4;
	int height = cfg->screen_height - top - HOME_NAV_HEIGHT;
	home_scroll = lv_obj_create(main_menu_screen);
	lv_obj_set_size(home_scroll, cfg->screen_width, height);
	lv_obj_align(home_scroll, LV_ALIGN_TOP_LEFT, 0, top);
	lv_obj_set_style_bg_opa(home_scroll, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(home_scroll, 0, 0);
	lv_obj_set_style_pad_hor(home_scroll, cfg->padding, 0);
	lv_obj_set_style_pad_ver(home_scroll, 16, 0);
	lv_obj_set_style_pad_bottom(home_scroll, HOME_PLAYER_HEIGHT + HOME_PLAYER_BOTTOM + 8, 0);
	lv_obj_set_style_pad_row(home_scroll, 14, 0);
	lv_obj_set_scroll_dir(home_scroll, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(home_scroll, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_flex_flow(home_scroll, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(home_scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	home_brand = lv_label_create(home_scroll);
	lv_label_set_text(home_brand, "RAGE OS");
	lv_obj_add_style(home_brand, &theme_style_text_dim, 0);
	lv_obj_set_style_text_color(home_brand, theme()->accent, 0);
	lv_obj_set_style_text_font(home_brand, &font_ui_20_bold, 0);
	lv_obj_t *greeting = lv_label_create(home_scroll);
	lv_label_set_text(greeting, tr("home_greeting_evening"));
	lv_obj_add_style(greeting, &theme_style_text, 0);
	lv_obj_set_style_text_font(greeting, &font_ui_32, 0);

	lv_obj_t *shortcuts = lv_obj_create(home_scroll);
	lv_obj_remove_style_all(shortcuts);
	lv_obj_set_width(shortcuts, LV_PCT(100));
	lv_obj_set_height(shortcuts, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_row(shortcuts, HOME_GAP, 0);
	lv_obj_set_style_pad_column(shortcuts, HOME_GAP, 0);
	lv_obj_set_flex_flow(shortcuts, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(shortcuts, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	add_shortcut(shortcuts, cfg, "favourites", &icon_menu_favourites, NULL, open_favourites);
	add_shortcut(shortcuts, cfg, "music", &icon_menu_music, music_screen, NULL);
	add_shortcut(shortcuts, cfg, "streaming", &icon_menu_streaming, streaming_screen, NULL);
	add_shortcut(shortcuts, cfg, "audiobooks", &icon_menu_audiobooks, audiobooks_screen, NULL);
	add_shortcut(shortcuts, cfg, "wireless", &icon_menu_wireless, wireless_screen, NULL);
	add_shortcut(shortcuts, cfg, "more", &icon_menu_more, morepage_screen, NULL);

	lv_obj_t *settings = lv_btn_create(home_scroll);
	lv_obj_set_size(settings, LV_PCT(100), 52);
	lv_obj_add_style(settings, &theme_style_card, 0);
	lv_obj_add_style(settings, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(settings, 12, 0);
	lv_obj_set_style_border_width(settings, 0, 0);
	lv_obj_set_style_shadow_width(settings, 0, 0);
	lv_obj_add_event_cb(settings, switch_screen_cb, LV_EVENT_CLICKED, settings_screen);
	lv_obj_t *settings_icon = lv_image_create(settings);
	lv_image_set_src(settings_icon, &icon_menu_settings);
	lv_obj_align(settings_icon, LV_ALIGN_LEFT_MID, 12, 0);
	lv_obj_t *settings_text = lv_label_create(settings);
	lv_label_set_text(settings_text, tr("settings"));
	lv_obj_add_style(settings_text, &theme_style_text, 0);
	lv_obj_set_style_text_font(settings_text, &font_ui_20_bold, 0);
	lv_obj_align(settings_text, LV_ALIGN_LEFT_MID, 48, 0);

	player_sheet_attach_drag(home_scroll, true);
	switcher_attach_back_gesture(home_scroll);
	build_mini_player(cfg);
}
