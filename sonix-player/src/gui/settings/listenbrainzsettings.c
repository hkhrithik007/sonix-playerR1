#include "listenbrainzsettings.h"

#include <stdio.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"
#include "src/system/listenbrainz/listenbrainz.h"
#include "src/system/net/wifi.h"

#define TICK_MS 1000

static lv_obj_t *page_screen;
static lv_obj_t *enabled_switch;
static lv_obj_t *account_value;
static lv_obj_t *status_value;
static lv_obj_t *queued_value;
static lv_obj_t *note_label;

static lv_obj_t *login_screen;
static lv_obj_t *token_field;
static keyboard_t *login_keyboard;

static unsigned seen_login_serial;
static bool waiting_for_login;

lv_obj_t *listenbrainzsettings_screen(void) { return page_screen; }

static void refresh(void) {
	listenbrainz_status_t st;
	listenbrainz_get_status(&st);

	if (st.enabled)
		lv_obj_add_state(enabled_switch, LV_STATE_CHECKED);
	else
		lv_obj_remove_state(enabled_switch, LV_STATE_CHECKED);

	lv_label_set_text(account_value, st.user[0] ? st.user : tr("listenbrainz_token_required"));

	const char *status = "";
	switch (st.state) {
	case LISTENBRAINZ_STATE_UNAVAILABLE:
		status = tr("listenbrainz_refused");
		break;
	case LISTENBRAINZ_STATE_OFF:
		status = tr("listenbrainz_off");
		break;
	case LISTENBRAINZ_STATE_SIGNED_OUT:
		status = tr("listenbrainz_token_required");
		break;
	case LISTENBRAINZ_STATE_SIGNING_IN:
		status = tr("listenbrainz_checking");
		break;
	case LISTENBRAINZ_STATE_CONNECTED:
		status = tr("connected");
		break;
	case LISTENBRAINZ_STATE_SESSION_EXPIRED:
		status = tr("listenbrainz_invalid");
		break;
	case LISTENBRAINZ_STATE_REFUSED:
		status = st.detail;
		break;
	}
	lv_label_set_text(status_value, status);
	lv_label_set_text(note_label, tr("listenbrainz_token_note"));
	lv_label_set_text_fmt(queued_value, "%d", st.queued);
}

static void open_login(void) {
	lv_textarea_set_text(token_field, "");
	keyboard_reset(login_keyboard);
	keyboard_set_field(login_keyboard, token_field);
	lv_obj_add_state(token_field, LV_STATE_FOCUSED);
	keyboard_show_caret(token_field, true);
	switch_screen(login_screen);
}

static void enabled_cb(lv_event_t *e) {
	(void)e;
	bool on = lv_obj_has_state(enabled_switch, LV_STATE_CHECKED);
	listenbrainz_set_enabled(on);
	listenbrainz_status_t st;
	listenbrainz_get_status(&st);
	if (on && !st.user[0] && st.state != LISTENBRAINZ_STATE_SIGNING_IN)
		open_login();
	refresh();
}

static void do_logout(void *user) {
	(void)user;
	listenbrainz_logout();
	toast_success("signed_out");
	refresh();
}

static void account_cb(lv_event_t *e) {
	(void)e;
	listenbrainz_status_t st;
	listenbrainz_get_status(&st);
	if (st.state == LISTENBRAINZ_STATE_SIGNING_IN)
		return;
	if (st.user[0]) {
		char message[320];
		// Signing out throws away the listens still waiting to be sent: say so.
		if (st.queued > 0)
			snprintf(message, sizeof(message), "%s", tr("listenbrainz_logout_note"));
		else
			snprintf(message, sizeof(message), tr("sign_out_confirm_note"), "ListenBrainz");
		confirm_show("listenbrainz_logout", message, "leave", do_logout, NULL);
		return;
	}
	open_login();
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	refresh();
}

static void build_page(gui_config_t *cfg) {
	page_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(page_screen, cfg, "listenbrainz");

	settingsrow_toggle(container, "listenbrainz_enabled", &enabled_switch, enabled_cb);
	settingsrow_add(container, "listenbrainz_account", &account_value, account_cb, NULL);
	settingsrow_add(container, "listenbrainz_status", &status_value, NULL, NULL);
	settingsrow_add(container, "listenbrainz_queued", &queued_value, NULL, NULL);

	lv_obj_t *values[] = {account_value, status_value};
	for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
		lv_label_set_long_mode(values[i], LV_LABEL_LONG_DOT);
		lv_obj_set_style_max_width(values[i], lv_pct(60), 0);
		lv_obj_set_height(values[i], lv_font_get_line_height(&font_ui_24));
	}

	note_label = lv_label_create(container);
	lv_label_set_long_mode(note_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note_label, lv_pct(100));
	lv_obj_add_style(note_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note_label, &font_ui_22, 0);
	lv_label_set_text(note_label, tr("listenbrainz_token_note"));

	lv_obj_add_event_cb(page_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(page_screen);
}

static void field_focus_cb(lv_event_t *e) {
	(void)e;
	keyboard_set_field(login_keyboard, token_field);
	lv_obj_add_state(token_field, LV_STATE_FOCUSED);
	keyboard_show_caret(token_field, true);
}

static void login_accept_cb(lv_event_t *e) {
	(void)e;
	const char *tok = lv_textarea_get_text(token_field);
	if (!tok || !tok[strspn(tok, " \t\r\n")]) {
		toast_error("listenbrainz_token_required");
		return;
	}
	if (!listenbrainz_available()) {
		toast_error("stream_err_no_response");
		return;
	}
	wifi_status_t wifi;
	wifi_get_status(&wifi);
	if (wifi.state != WIFI_STATE_CONNECTED || !wifi.ip[0]) {
		toast_error("enable_wi_fi_first");
		return;
	}

	// Only wait for a result the worker has actually taken on, or the busy
	// toast would never end.
	bool taken = listenbrainz_login(tok);
	lv_textarea_set_text(token_field, "");
	if (!taken) {
		toast_error("listenbrainz_invalid");
		return;
	}
	waiting_for_login = true;
	toast_busy("listenbrainz_checking");
}

static lv_obj_t *make_field(lv_obj_t *parent, gui_config_t *cfg, const char *placeholder, int y) {
	lv_obj_t *field = lv_textarea_create(parent);
	lv_textarea_set_one_line(field, true);
	lv_textarea_set_placeholder_text(field, tr(placeholder));
	lv_obj_set_size(field, cfg->screen_width - 2 * cfg->padding, 62);
	lv_obj_set_scrollbar_mode(field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(field, LV_ALIGN_TOP_LEFT, cfg->padding, y);
	lv_obj_add_style(field, &theme_style_card, 0);
	lv_obj_set_style_radius(field, 12, 0);
	lv_obj_set_style_border_width(field, 0, 0);
	lv_obj_set_style_shadow_width(field, 0, 0);
	lv_obj_set_style_pad_all(field, 14, 0);
	lv_obj_set_style_text_font(field, &font_ui_24, 0);
	keyboard_style_caret(field);
	lv_obj_add_event_cb(field, field_focus_cb, LV_EVENT_CLICKED, NULL);
	return field;
}

static void build_login(gui_config_t *cfg) {
	login_screen = lv_obj_create(NULL);
	lv_obj_add_style(login_screen, &theme_style_screen, 0);

	settingsrow_title(login_screen, cfg, "listenbrainz_enter_token");
	int top = settingsrow_content_top(cfg);

	token_field = make_field(login_screen, cfg, "listenbrainz_token", top);

	lv_obj_t *note = lv_label_create(login_screen);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, cfg->screen_width - 2 * cfg->padding);
	lv_obj_align(note, LV_ALIGN_TOP_LEFT, cfg->padding, top + 78);
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_18, 0);
	lv_label_set_text(note, tr("listenbrainz_token_note"));

	login_keyboard = keyboard_create(login_screen, cfg->screen_width, 316, token_field, NULL, "ok", login_accept_cb, NULL);
	switcher_attach_back_gesture(login_screen);
}

static void login_finished(const listenbrainz_status_t *st) {
	if (!waiting_for_login)
		return;
	waiting_for_login = false;
	toast_busy_end();

	switch (st->login_result) {
	case LISTENBRAINZ_LOGIN_OK:
		if (lv_screen_active() == login_screen)
			back_btn_cb(NULL);
		toast_success("signed_in");
		break;
	case LISTENBRAINZ_LOGIN_WRONG_CREDENTIALS:
		toast_error("listenbrainz_invalid");
		break;
	case LISTENBRAINZ_LOGIN_NO_REPLY:
		toast_error("stream_err_no_response");
		break;
	case LISTENBRAINZ_LOGIN_REFUSED:
		toast_error(st->detail);
		break;
	}
}

static void tick_cb(lv_timer_t *t) {
	(void)t;
	listenbrainz_status_t st;
	listenbrainz_get_status(&st);
	if (st.login_serial != seen_login_serial) {
		seen_login_serial = st.login_serial;
		login_finished(&st);
	}
	if (lv_screen_active() == page_screen)
		refresh();
}

void listenbrainzsettings_init(gui_config_t *cfg) {
	build_page(cfg);
	build_login(cfg);

	listenbrainz_status_t st;
	listenbrainz_get_status(&st);
	seen_login_serial = st.login_serial;
	refresh();

	lv_timer_create(tick_cb, TICK_MS, NULL);
}
