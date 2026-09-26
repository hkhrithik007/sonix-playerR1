#include "lastfmsettings.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/lastfm/lastfm.h"

static lv_obj_t *screen;
static lv_obj_t *enabled_switch;
static lv_obj_t *account_value;
static lv_obj_t *api_key_value;
static lv_obj_t *api_secret_value;
static lv_obj_t *status_value;
static lv_obj_t *auth_row;
static lv_obj_t *input_layer;
static lv_obj_t *input_title;
static lv_obj_t *input_field;
static keyboard_t *input_keyboard;
static lv_timer_t *service_timer;

static char pending_username[256];

typedef enum {
	INPUT_USERNAME = 0,
	INPUT_PASSWORD,
	INPUT_API_KEY,
	INPUT_API_SECRET,
} input_mode_t;

static input_mode_t input_mode;
static uint64_t seen_message_serial;

static void refresh_rows(void) {
	if (!screen) return;

	lastfm_snapshot_t snap;
	lastfm_get_snapshot(&snap);

	if (enabled_switch) {
		if (snap.enabled) lv_obj_add_state(enabled_switch, LV_STATE_CHECKED);
		else lv_obj_remove_state(enabled_switch, LV_STATE_CHECKED);
	}
	if (account_value) {
		lv_label_set_text(account_value, snap.logged_in && snap.username[0] ? snap.username : "Not logged in");
	}
	if (api_key_value) {
		lv_label_set_text(api_key_value, snap.api_key_configured ? "Configured" : "Not set");
	}
	if (api_secret_value) {
		lv_label_set_text(api_secret_value, snap.api_secret_configured ? "Configured" : "Not set");
	}
	if (status_value) {
		lv_label_set_text(status_value, snap.status);
	}
	if (auth_row) {
		lv_label_set_text(settingsrow_name_label(auth_row), snap.logged_in ? "Log Out" : "Log In");
	}

	if (snap.message_serial != 0 && snap.message_serial != seen_message_serial && snap.last_message[0]) {
		seen_message_serial = snap.message_serial;
		if (strstr(snap.last_message, "Logged in to Last.fm") ||
			strstr(snap.last_message, "Logged out of Last.fm") ||
			strstr(snap.last_message, "Last.fm error") ||
			strstr(snap.last_message, "Could not start Last.fm")) {
			gui_notify_popup(snap.last_message);
		}
	}
}

static void enabled_cb(lv_event_t *e) {
	bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	lastfm_set_enabled(on);
	refresh_rows();
}

static void input_show(bool visible) {
	if (input_keyboard) keyboard_set_visible(input_keyboard, visible);
	if (input_layer) {
		if (visible) lv_obj_remove_flag(input_layer, LV_OBJ_FLAG_HIDDEN);
		else lv_obj_add_flag(input_layer, LV_OBJ_FLAG_HIDDEN);
	}
}

static void input_prepare(input_mode_t mode, const char *value) {
	input_mode = mode;
	const char *title = "Last.fm";
	const char *placeholder = "Enter value";
	bool password = false;

	switch (mode) {
	case INPUT_USERNAME:
		title = "Last.fm Username";
		placeholder = "Username or email";
		break;
	case INPUT_PASSWORD:
		title = "Last.fm Password";
		placeholder = "Password";
		password = true;
		break;
	case INPUT_API_KEY:
		title = "Last.fm API Key";
		placeholder = "API key";
		break;
	case INPUT_API_SECRET:
		title = "Last.fm API Secret";
		placeholder = "API secret";
		password = true;
		break;
	}

	lv_label_set_text(input_title, title);
	lv_textarea_set_placeholder_text(input_field, placeholder);
	lv_textarea_set_text(input_field, value ? value : "");
	lv_textarea_set_password_mode(input_field, password);
	keyboard_reset(input_keyboard);
	keyboard_set_field(input_keyboard, input_field);
	keyboard_show_caret(input_field, true);
	input_show(true);
}

static void auth_action_cb(lv_event_t *e) {
	(void)e;
	lastfm_snapshot_t snap;
	lastfm_get_snapshot(&snap);
	if (snap.logged_in) {
		lastfm_logout();
		refresh_rows();
		return;
	}
	input_prepare(INPUT_USERNAME, NULL);
}

static void input_accept_cb(lv_event_t *e) {
	(void)e;
	const char *text = lv_textarea_get_text(input_field);
	if (!text || !text[0]) {
		gui_notify_popup("Last.fm: value required");
		return;
	}

	switch (input_mode) {
	case INPUT_USERNAME:
		snprintf(pending_username, sizeof(pending_username), "%s", text);
		lv_textarea_set_text(input_field, "");
		input_prepare(INPUT_PASSWORD, NULL);
		return;
	case INPUT_PASSWORD:
		lastfm_login(pending_username, text);
		memset(pending_username, 0, sizeof(pending_username));
		lv_textarea_set_text(input_field, "");
		input_show(false);
		switch_screen(screen);
		refresh_rows();
		return;
	case INPUT_API_KEY:
		lastfm_set_api_key(text);
		lv_textarea_set_text(input_field, "");
		input_show(false);
		switch_screen(screen);
		refresh_rows();
		return;
	case INPUT_API_SECRET:
		lastfm_set_api_secret(text);
		lv_textarea_set_text(input_field, "");
		input_show(false);
		switch_screen(screen);
		refresh_rows();
		return;
	}
}

static void input_cancel_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(input_field, "");
	memset(pending_username, 0, sizeof(pending_username));
	input_show(false);
	switch_screen(screen);
}

static void field_clicked_cb(lv_event_t *e) {
	keyboard_set_field(input_keyboard, lv_event_get_target(e));
	keyboard_show_caret(input_field, true);
}

static void service_tick_cb(lv_timer_t *timer) {
	(void)timer;
	lastfm_poll();
	refresh_rows();
}

static void api_key_open_cb(lv_event_t *e) {
	(void)e;
	lastfm_snapshot_t snap;
	lastfm_get_snapshot(&snap);
	(void)snap;
	input_prepare(INPUT_API_KEY, NULL);
}

static void api_secret_open_cb(lv_event_t *e) {
	(void)e;
	input_prepare(INPUT_API_SECRET, NULL);
}

static void build_input_layer(gui_config_t *cfg) {
	input_layer = lv_obj_create(screen);
	lv_obj_set_size(input_layer, cfg->screen_width, cfg->screen_height);
	lv_obj_align(input_layer, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_add_style(input_layer, &theme_style_screen, 0);
	lv_obj_set_style_bg_opa(input_layer, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(input_layer, 0, 0);
	lv_obj_set_style_radius(input_layer, 0, 0);
	lv_obj_set_style_pad_all(input_layer, 0, 0);
	lv_obj_remove_flag(input_layer, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(input_layer, LV_OBJ_FLAG_HIDDEN);

	input_title = lv_label_create(input_layer);
	lv_label_set_text(input_title, "Last.fm");
	lv_obj_add_style(input_title, &theme_style_text, 0);
	lv_obj_set_style_text_font(input_title, &font_ui_24, 0);
	lv_obj_align(input_title, LV_ALIGN_TOP_LEFT, cfg->padding + 56 + 14, cfg->padding + cfg->top_bar_height + 10);

	lv_obj_t *cancel = lv_btn_create(input_layer);
	lv_obj_set_size(cancel, 56, 56);
	lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, -cfg->padding, cfg->padding + cfg->top_bar_height);
	lv_obj_set_style_bg_opa(cancel, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(cancel, 0, 0);
	lv_obj_set_style_shadow_width(cancel, 0, 0);
	lv_obj_set_style_pad_all(cancel, 0, 0);
	lv_obj_add_event_cb(cancel, input_cancel_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *cancel_label = lv_label_create(cancel);
	lv_label_set_text(cancel_label, "×");
	lv_obj_set_style_text_font(cancel_label, &font_ui_32, 0);
	lv_obj_center(cancel_label);

	input_field = lv_textarea_create(input_layer);
	lv_textarea_set_one_line(input_field, true);
	lv_textarea_set_max_length(input_field, 255);
	lv_textarea_set_placeholder_text(input_field, "Enter value");
	lv_obj_set_size(input_field, cfg->screen_width - 2 * cfg->padding, 62);
	lv_obj_set_scrollbar_mode(input_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(input_field, LV_ALIGN_TOP_LEFT, cfg->padding, cfg->padding + cfg->top_bar_height + 60);
	lv_obj_add_style(input_field, &theme_style_card, 0);
	lv_obj_set_style_radius(input_field, 12, 0);
	lv_obj_set_style_border_width(input_field, 0, 0);
	lv_obj_set_style_shadow_width(input_field, 0, 0);
	lv_obj_set_style_pad_all(input_field, 14, 0);
	lv_obj_set_style_text_font(input_field, &font_ui_24, 0);
	keyboard_style_caret(input_field);
	lv_obj_add_event_cb(input_field, field_clicked_cb, LV_EVENT_CLICKED, NULL);

	input_keyboard = keyboard_create(input_layer, cfg->screen_width, 316, input_field, NULL, "OK", input_accept_cb, NULL);
}

void lastfmsettings_init(gui_config_t *cfg) {
	screen = lv_obj_create(NULL);
	lv_obj_add_style(screen, &theme_style_screen, 0);
	lv_obj_t *container = settingsrow_page(screen, cfg, "Last.fm");
	settingsrow_title_corner_slots(settingsrow_page_title(screen), cfg, 0);

	settingsrow_toggle(container, "Enabled", &enabled_switch, enabled_cb);
	settingsrow_add(container, "Account", &account_value, NULL, NULL);
	auth_row = settingsrow_action(container, "Log In", auth_action_cb, NULL);
	settingsrow_add(container, "API Key", &api_key_value, api_key_open_cb, NULL);
	settingsrow_add(container, "API Secret", &api_secret_value, api_secret_open_cb, NULL);
	settingsrow_add(container, "Connection", &status_value, NULL, NULL);

	lv_obj_t *note = lv_label_create(container);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_22, 0);
	lv_label_set_text(note,
		"Scrobbles tracks after 50% played or 4 minutes, whichever comes first.\n"
		"Offline scrobbles are kept on disk for 13 days and retried automatically.\n"
		"Your Last.fm password is used only for login and is never saved.");

	build_input_layer(cfg);
	service_timer = lv_timer_create(service_tick_cb, 1000, NULL);
	(void)service_timer;

	lastfm_snapshot_t snap;
	lastfm_get_snapshot(&snap);
	if (snap.enabled) lv_obj_add_state(enabled_switch, LV_STATE_CHECKED);
	seen_message_serial = snap.message_serial;
	refresh_rows();

	switcher_attach_back_gesture(screen);
}

lv_obj_t *lastfmsettings_screen(void) { return screen; }
