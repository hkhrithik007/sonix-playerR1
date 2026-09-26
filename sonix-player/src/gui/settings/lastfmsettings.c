#include "lastfmsettings.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/lastfm/lastfm.h"

static lv_obj_t *lastfm_page;
static lv_obj_t *enabled_switch;
static lv_obj_t *api_key_value;
static lv_obj_t *api_secret_value;
static lv_obj_t *account_value;
static lv_obj_t *status_value;
static lv_obj_t *auth_row;

static lv_obj_t *input_screen;
static lv_obj_t *input_field;
static lv_obj_t *input_title;
static keyboard_t *input_keyboard;

typedef enum {
    INPUT_API_KEY,
    INPUT_API_SECRET,
    INPUT_USERNAME,
    INPUT_PASSWORD,
} input_mode_t;

static input_mode_t input_mode;
static char pending_username[256];

static void refresh_page(void) {
    if (!lastfm_page) {
        return;
    }

    lastfm_snapshot_t snap;
    lastfm_get_snapshot(&snap);

    if (enabled_switch) {
        if (snap.enabled) {
            lv_obj_add_state(enabled_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(enabled_switch, LV_STATE_CHECKED);
        }
    }

    if (api_key_value) {
        /*
         * Show the actual API key in the settings row. API keys are not treated
         * as passwords here, so the user can verify exactly what was entered.
         */
        char value[128];
        snprintf(value, sizeof(value), "%s", snap.api_key_configured
                                                  ? config_get("lastfm", "api_key", "")
                                                  : "Not set");
        lv_label_set_text(api_key_value, value);
    }

    if (api_secret_value) {
        /*
         * Deliberately visible, matching the API key. The user explicitly needs
         * to be able to inspect/correct the secret rather than only seeing
         * "Configured".
         */
        char value[128];
        snprintf(value, sizeof(value), "%s", snap.api_secret_configured
                                                  ? config_get("lastfm", "api_secret", "")
                                                  : "Not set");
        lv_label_set_text(api_secret_value, value);
    }

    if (account_value) {
        lv_label_set_text(account_value,
                          snap.logged_in && snap.username[0] ? snap.username : "Not logged in");
    }

    if (status_value) {
        lv_label_set_text(status_value, snap.status);
    }

    if (auth_row) {
        lv_label_set_text(settingsrow_name_label(auth_row),
                          snap.logging_in ? "Logging In..." : (snap.logged_in ? "Log Out" : "Log In"));
    }
}

static void lastfm_poll_timer_cb(lv_timer_t *timer) {
    (void)timer;
    refresh_page();
}

static void enabled_cb(lv_event_t *e) {
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    lastfm_set_enabled(on);
    refresh_page();
}

static void input_prepare(input_mode_t mode, const char *value) {
    input_mode = mode;

    const char *title = "Last.fm";
    const char *placeholder = "Enter value";
    bool password = false;

    switch (mode) {
    case INPUT_API_KEY:
        title = "Last.fm API Key";
        placeholder = "Paste or type API key";
        break;
    case INPUT_API_SECRET:
        /*
         * API SECRET IS INTENTIONALLY VISIBLE.
         * This is not a password field.
         */
        title = "Last.fm API Secret";
        placeholder = "Paste or type API secret";
        break;
    case INPUT_USERNAME:
        title = "Last.fm Username";
        placeholder = "Username";
        break;
    case INPUT_PASSWORD:
        title = "Last.fm Password";
        placeholder = "Password";
        password = true;
        break;
    }

    lv_label_set_text(input_title, title);
    lv_textarea_set_placeholder_text(input_field, placeholder);
    lv_textarea_set_text(input_field, value ? value : "");
    lv_textarea_set_password_mode(input_field, password);
    if (password) {
        keyboard_style_password(input_field, 0);
    } else {
        keyboard_refresh_password(input_field);
    }

    keyboard_reset(input_keyboard);
    keyboard_set_field(input_keyboard, input_field);
    keyboard_show_caret(input_field, true);
    keyboard_set_visible(input_keyboard, true);
    lv_obj_add_state(input_field, LV_STATE_FOCUSED);
}

static void input_accept_cb(lv_event_t *e) {
    (void)e;

    const char *text = lv_textarea_get_text(input_field);
    if (!text || !text[0]) {
        gui_notify_popup("Last.fm: a value is required");
        return;
    }

    switch (input_mode) {
    case INPUT_API_KEY:
        lastfm_set_api_key(text);
        switch_screen(lastfm_page);
        refresh_page();
        return;

    case INPUT_API_SECRET:
        lastfm_set_api_secret(text);
        switch_screen(lastfm_page);
        refresh_page();
        return;

    case INPUT_USERNAME:
        snprintf(pending_username, sizeof(pending_username), "%s", text);
        input_prepare(INPUT_PASSWORD, NULL);
        return;

    case INPUT_PASSWORD:
        lastfm_login(pending_username, text);
        memset(pending_username, 0, sizeof(pending_username));
        lv_textarea_set_text(input_field, "");
        switch_screen(lastfm_page);
        refresh_page();
        return;
    }
}

static void api_key_cb(lv_event_t *e) {
    (void)e;
    const char *value = config_get("lastfm", "api_key", "");
    input_prepare(INPUT_API_KEY, value);
    switch_screen(input_screen);
}

static void api_secret_cb(lv_event_t *e) {
    (void)e;
    const char *value = config_get("lastfm", "api_secret", "");
    input_prepare(INPUT_API_SECRET, value);
    switch_screen(input_screen);
}

static void auth_cb(lv_event_t *e) {
    (void)e;

    lastfm_snapshot_t snap;
    lastfm_get_snapshot(&snap);

    if (snap.logging_in) {
        return;
    }

    if (snap.logged_in) {
        lastfm_logout();
        refresh_page();
        return;
    }

    if (!snap.api_key_configured || !snap.api_secret_configured) {
        gui_notify_popup("Enter the Last.fm API key and API secret first");
        return;
    }

    input_prepare(INPUT_USERNAME, snap.username);
    switch_screen(input_screen);
}

static void build_input_screen(gui_config_t *cfg) {
    input_screen = lv_obj_create(NULL);
    lv_obj_add_style(input_screen, &theme_style_screen, 0);

    input_title = settingsrow_title(input_screen, cfg, "Last.fm");

    input_field = lv_textarea_create(input_screen);
    lv_textarea_set_one_line(input_field, true);
    lv_obj_set_size(input_field, cfg->screen_width - 2 * cfg->padding, 62);
    lv_obj_set_scrollbar_mode(input_field, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(input_field, LV_ALIGN_TOP_LEFT, cfg->padding, settingsrow_content_top(cfg));
    lv_obj_add_style(input_field, &theme_style_card, 0);
    lv_obj_set_style_radius(input_field, 12, 0);
    lv_obj_set_style_border_width(input_field, 0, 0);
    lv_obj_set_style_shadow_width(input_field, 0, 0);
    lv_obj_set_style_pad_all(input_field, 14, 0);
    lv_obj_set_style_text_font(input_field, &font_ui_24, 0);
    keyboard_style_caret(input_field);

    input_keyboard = keyboard_create(input_screen, cfg->screen_width, 316, input_field,
                                      NULL, "OK", input_accept_cb, NULL);

    switcher_attach_back_gesture(input_screen);

    /*
     * The key/secret/password page starts keyboard-visible. The actual values
     * are filled each time it is opened, so changes made in Settings are not
     * left stale here.
     */
    keyboard_set_visible(input_keyboard, true);
}

static void build_lastfm_page(gui_config_t *cfg) {
    lastfm_page = lv_obj_create(NULL);
    lv_obj_t *container = settingsrow_page(lastfm_page, cfg, "Last.fm");
    settingsrow_title_corner_slots(settingsrow_page_title(lastfm_page), cfg, 0);

    settingsrow_toggle(container, "Enabled", &enabled_switch, enabled_cb);

    settingsrow_add(container, "API Key", &api_key_value, api_key_cb, NULL);
    settingsrow_add(container, "API Secret", &api_secret_value, api_secret_cb, NULL);

    auth_row = settingsrow_add(container, "Log In", &account_value, auth_cb, NULL);
    status_value = NULL;
    settingsrow_add(container, "Status", &status_value, NULL, NULL);

    refresh_page();
    switcher_attach_back_gesture(lastfm_page);

    /*
     * Last.fm authentication completes on the network worker, so refresh this
     * page periodically while it is visible. This timer is cheap and touches
     * only LVGL from the LVGL thread.
     */
    lv_timer_create(lastfm_poll_timer_cb, 500, NULL);
}

void lastfmsettings_init(gui_config_t *cfg) {
    build_input_screen(cfg);
    build_lastfm_page(cfg);
}

lv_obj_t *lastfmsettings_screen(void) {
    return lastfm_page;
}
