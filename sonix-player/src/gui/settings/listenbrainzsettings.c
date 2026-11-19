#include "listenbrainzsettings.h"

#include <stdio.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"
#include "src/system/listenbrainz/listenbrainz.h"
#include "src/system/net/wifi.h"

static lv_obj_t *page;
static lv_obj_t *enabled_switch, *account_value, *status_value, *queue_value, *note;
static lv_obj_t *token_screen, *token_field;
static keyboard_t *token_keyboard;
static unsigned seen_serial;
static bool waiting_for_token;
static bool enable_after_token;

lv_obj_t *listenbrainzsettings_screen(void) { return page; }

static void refresh(void) {
	listenbrainz_status_t st;
	listenbrainz_get_status(&st);
	if (st.enabled) lv_obj_add_state(enabled_switch, LV_STATE_CHECKED);
	else lv_obj_remove_state(enabled_switch, LV_STATE_CHECKED);
	lv_label_set_text(account_value, st.user[0] ? st.user : tr("listenbrainz_token_required"));
	const char *state = tr("listenbrainz_off");
	if (st.state == LISTENBRAINZ_CHECKING) state = tr("listenbrainz_checking");
	else if (st.state == LISTENBRAINZ_CONNECTED) state = tr("listenbrainz_connected");
	else if (st.state == LISTENBRAINZ_REFUSED) state = st.detail[0] ? st.detail : tr("listenbrainz_refused");
	lv_label_set_text(status_value, state);
	lv_label_set_text_fmt(queue_value, "%d", st.queued);
	lv_label_set_text(note, tr("listenbrainz_token_note"));
	if (waiting_for_token && st.serial != seen_serial) {
		waiting_for_token = false;
		toast_busy_end();
		seen_serial = st.serial;
		if (st.last_token_valid) {
			if (enable_after_token) {
				listenbrainz_set_enabled(true);
				lv_obj_add_state(enabled_switch, LV_STATE_CHECKED);
			}
			enable_after_token = false;
			if (lv_screen_active() == token_screen) back_btn_cb(NULL);
			toast_success("listenbrainz_saved");
		} else {
			enable_after_token = false;
			toast_error(st.detail[0] ? st.detail : tr("listenbrainz_invalid"));
		}
	}
}

static void toggle_cb(lv_event_t *e) {
	(void)e;
	bool on = lv_obj_has_state(enabled_switch, LV_STATE_CHECKED);
	listenbrainz_status_t st;
	listenbrainz_get_status(&st);
	if (on && !st.last_token_valid) {
		lv_obj_remove_state(enabled_switch, LV_STATE_CHECKED);
		enable_after_token = true;
		switch_screen(token_screen);
		return;
	}
	listenbrainz_set_enabled(on);
	refresh();
}

static void do_logout(void *user) {
	(void)user;
	listenbrainz_logout();
	listenbrainz_set_enabled(false);
	toast_plain("listenbrainz_logged_out");
	refresh();
}

static void account_cb(lv_event_t *e) {
	(void)e;
	listenbrainz_status_t st;
	listenbrainz_get_status(&st);
	if (st.last_token_valid) {
		confirm_show("listenbrainz_logout", tr("listenbrainz_logout_note"), "leave", do_logout, NULL);
		return;
	}
	lv_textarea_set_text(token_field, "");
	keyboard_reset(token_keyboard);
	switch_screen(token_screen);
}

static void field_focus_cb(lv_event_t *e) {
	(void)e;
	keyboard_set_field(token_keyboard, token_field);
	lv_obj_add_state(token_field, LV_STATE_FOCUSED);
	keyboard_show_caret(token_field, true);
}

static void token_accept_cb(lv_event_t *e) {
	(void)e;
	const char *value = lv_textarea_get_text(token_field);
	bool valid = value && strlen(value) >= 10 && strlen(value) <= 200;
	for (const unsigned char *p = (const unsigned char *)value; valid && *p; p++) {
		valid = ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
			*p == '-' || *p == '.' || *p == '_' || *p == '~');
	}
	if (!valid) {
		toast_error("listenbrainz_invalid");
		return;
	}
	wifi_status_t wifi;
	wifi_get_status(&wifi);
	if (wifi.state != WIFI_STATE_CONNECTED || !wifi.ip[0]) {
		toast_error("enable_wi_fi_first");
		return;
	}
	listenbrainz_set_token(value);
	lv_textarea_set_text(token_field, "");
	waiting_for_token = true;
	listenbrainz_status_t st;
	listenbrainz_get_status(&st);
	seen_serial = st.serial;
	toast_busy("listenbrainz_checking");
}

static void build_page(gui_config_t *cfg) {
	page = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(page, cfg, "listenbrainz");
	settingsrow_toggle(container, "listenbrainz_enabled", &enabled_switch, toggle_cb);
	settingsrow_add(container, "listenbrainz_account", &account_value, account_cb, NULL);
	settingsrow_add(container, "listenbrainz_status", &status_value, NULL, NULL);
	settingsrow_add(container, "listenbrainz_queued", &queue_value, NULL, NULL);
	lv_obj_t *values[] = {account_value, status_value, queue_value};
	for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
		lv_label_set_long_mode(values[i], LV_LABEL_LONG_DOT);
		lv_obj_set_style_max_width(values[i], lv_pct(60), 0);
		lv_obj_set_height(values[i], lv_font_get_line_height(&font_ui_24));
	}
	note = lv_label_create(container);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_22, 0);
	switcher_attach_back_gesture(page);

	token_screen = lv_obj_create(NULL);
	lv_obj_add_style(token_screen, &theme_style_screen, 0);
	settingsrow_title(token_screen, cfg, "listenbrainz_enter_token");
	int top = settingsrow_content_top(cfg);
	token_field = lv_textarea_create(token_screen);
	lv_textarea_set_one_line(token_field, true);
	lv_textarea_set_max_length(token_field, 200);
	lv_textarea_set_placeholder_text(token_field, tr("listenbrainz_token"));
	lv_obj_set_size(token_field, cfg->screen_width - 2 * cfg->padding, 62);
	lv_obj_set_scrollbar_mode(token_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(token_field, LV_ALIGN_TOP_LEFT, cfg->padding, top);
	lv_obj_add_style(token_field, &theme_style_card, 0);
	lv_obj_set_style_radius(token_field, 12, 0);
	lv_obj_set_style_border_width(token_field, 0, 0);
	lv_obj_set_style_shadow_width(token_field, 0, 0);
	lv_obj_set_style_pad_all(token_field, 14, 0);
	lv_obj_set_style_text_font(token_field, &font_ui_24, 0);
	lv_textarea_set_password_mode(token_field, true);
	keyboard_style_password(token_field, 0);
	keyboard_style_caret(token_field);
	lv_obj_add_event_cb(token_field, field_focus_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *tip = lv_label_create(token_screen);
	lv_label_set_long_mode(tip, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(tip, cfg->screen_width - 2 * cfg->padding);
	lv_obj_align(tip, LV_ALIGN_TOP_LEFT, cfg->padding, top + 78);
	lv_obj_add_style(tip, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(tip, &font_ui_18, 0);
	lv_label_set_text(tip, tr("listenbrainz_token_note"));
	token_keyboard = keyboard_create(token_screen, cfg->screen_width, 316, token_field, NULL, "ok", token_accept_cb, NULL);
	switcher_attach_back_gesture(token_screen);
}

static void tick_cb(lv_timer_t *timer) {
	(void)timer;
	listenbrainz_status_t st;
	listenbrainz_get_status(&st);
	if (lv_screen_active() == page || waiting_for_token) refresh();
}

void listenbrainzsettings_init(gui_config_t *cfg) {
	build_page(cfg);
	listenbrainz_status_t st;
	listenbrainz_get_status(&st);
	seen_serial = st.serial;
	refresh();
	lv_timer_create(tick_cb, 1000, NULL);
}
