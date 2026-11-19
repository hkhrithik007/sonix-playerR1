#include "gearboypage.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/gearboy/gearboyplay.h"
#include "src/gui/gearboy/gearboysettings.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"
#include "src/system/gearboy/gbdb.h"

lv_obj_t *gearboypage_screen;

static lv_obj_t *list_box;
static lv_obj_t *status_label;
static lv_obj_t *btn_gb;
static lv_obj_t *btn_gbc;

// Which of the two pills is selected. ROMs live in two folders (Games/GB and
// Games/GBC) for two different machines, so they are shown as two lists rather
// than one list needing a subtitle on every entry.
static bool show_color;

// The list the rows are showing. Rows carry the index, not the pointer, so the
// array can be freed and rebuilt without a surviving row dangling.
static gbrom_t *roms;
static int rom_count;
static storage_card_token_t roms_token;

// GUI owns page visibility/generation. The worker owns no widget pointers.
static uint32_t scan_generation;
static bool scanning;
static bool scan_failed;
static bool page_visible;
static bool rescan_requested;

typedef enum { SCAN_SUCCESS, SCAN_ERROR } scan_status_t;
typedef struct {
	pthread_mutex_t lock;
	bool active; // Includes READY until the GUI takes or cancels it.
	bool ready;
	bool closing;
	uint32_t generation;
	storage_card_token_t token;
	char root[GBROM_PATH_MAX];
	scan_status_t status;
	gbrom_t *list;
	int count;
} scan_terminal_t;
static scan_terminal_t scan_terminal = {.lock = PTHREAD_MUTEX_INITIALIZER};

static void open_rom_cb(lv_event_t *e) {
	// The release of a back swipe lands on whichever row is under the finger:
	// without this, swiping back from here would launch a game.
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}

	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= rom_count || !roms) {
		return;
	}
	// This rejects an already stale list. The delayed emulator open is a
	// separate ownership boundary; this read check is not a filesystem lease.
	if (storage_card_try_check(&roms_token) != 0) {
		if (errno != EAGAIN)
			rescan_requested = true;
		return;
	}
	gearboyplay_open(roms[index].path, roms[index].name);
}

// settingsrow_action() is the canonical row of this interface: same height,
// radius, font and pressed state, and no chevron, because tapping it starts a
// game rather than opening a page.
//
// The name is not a UI label, it comes from the cartridge database. tr() passes
// it through unchanged, since a missing key returns the string it was given.
static void add_row(int index) { settingsrow_action(list_box, roms[index].name, open_rom_cb, (void *)(intptr_t)index); }

static void refresh_view_buttons(void) {
	lv_obj_t *const buttons[] = {btn_gb, btn_gbc};
	const bool colour[] = {false, true};

	for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
		if (!buttons[i]) {
			continue;
		}
		bool on = colour[i] == show_color;
		lv_obj_set_style_bg_color(buttons[i], on ? theme()->accent : theme()->surface_pressed, 0);
		lv_obj_set_style_text_color(lv_obj_get_child(buttons[i], 0), on ? lv_color_white() : theme()->text_primary, 0);
	}
}

static void rebuild_rows(void) {
	lv_obj_clean(list_box);

	int shown = 0;
	for (int i = 0; i < rom_count; i++) {
		if (roms[i].color == show_color) {
			add_row(i);
			shown++;
		}
	}

	if (shown > 0) {
		lv_obj_set_hidden(status_label, true);
		return;
	}

	lv_label_set_text(status_label, tr(scanning ? "gearboy_looking_for_games" : scan_failed ? "gearboy_search_failed" : show_color ? "gearboy_empty_note" : "gearboy_empty_gb_note"));
	lv_obj_set_hidden(status_label, false);
}

static void pick_view_cb(lv_event_t *e) {
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	show_color = (bool)(uintptr_t)lv_event_get_user_data(e);
	refresh_view_buttons();
	rebuild_rows();
	lv_obj_scroll_to_y(list_box, 0, LV_ANIM_OFF);
}

static lv_obj_t *make_view_choice(lv_obj_t *parent, const char *text, bool color) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, 56);
	lv_obj_set_style_pad_hor(btn, 26, 0);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0); // Adwaita pill button
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_add_event_cb(btn, pick_view_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)color);

	// "GB" and "GBC" arrive as tags ("gearboy_gb"/"gearboy_gbc") and go through
	// tr(): each language maps them to the right capitalization, so the pills
	// never show up lowercase.
	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, tr(text));
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	lv_obj_center(label);

	return btn;
}

// ---------------------------------------------------------------------------
// the scan
// ---------------------------------------------------------------------------
//
// On its own thread because the first pass reads every ROM end to end to
// compute its CRC32: eight megabytes of cartridge takes well over a second on
// this card, and doing it on the UI thread freezes the page. Later passes are
// answered from the cache and read nothing, but they take the same path.

static void start_scan(void);

// A wake is only a hint. It never owns the list or the terminal status.
static void scan_done_cb(void *unused) {
	(void)unused;
	gearboypage_service_completion();
}

static void *scan_thread(void *arg) {
	uint32_t generation = (uint32_t)(uintptr_t)arg;
	storage_card_token_t token;
	char root[GBROM_PATH_MAX];
	pthread_mutex_lock(&scan_terminal.lock);
	token = scan_terminal.token;
	memcpy(root, scan_terminal.root, sizeof(root));
	pthread_mutex_unlock(&scan_terminal.lock);
	gbrom_t *list = NULL;
	int count = gbdb_scan_admitted(&list, root, &token, NULL, NULL);
	if (count < 0) {
		free(list);
		list = NULL;
	}
	pthread_mutex_lock(&scan_terminal.lock);
	if (scan_terminal.closing) {
		scan_terminal.active = false;
		pthread_mutex_unlock(&scan_terminal.lock);
		free(list);
		return NULL;
	}
	// Single flight remains active through GUI consumption, so READY cannot
	// overwrite another terminal record even when all GUI queues are full.
	scan_terminal.generation = generation;
	scan_terminal.status = count < 0 ? SCAN_ERROR : SCAN_SUCCESS;
	scan_terminal.list = list;
	scan_terminal.count = count < 0 ? 0 : count;
	scan_terminal.ready = true;
	pthread_mutex_unlock(&scan_terminal.lock);
	gui_post(scan_done_cb, NULL);
	return NULL;
}

static void start_scan(void) {
	pthread_mutex_lock(&scan_terminal.lock);
	if (scan_terminal.closing || !page_visible) {
		pthread_mutex_unlock(&scan_terminal.lock);
		return;
	}
	if (scan_terminal.active) {
		rescan_requested = true;
		pthread_mutex_unlock(&scan_terminal.lock);
		scanning = true;
		scan_failed = false;
		lv_label_set_text(status_label, tr("gearboy_looking_for_games"));
		lv_obj_set_hidden(status_label, false);
		return;
	}
	pthread_mutex_unlock(&scan_terminal.lock);
	// Never nest card_lock inside terminal/root locks: detach takes card->root.
	storage_card_token_t token = {0};
	char root[GBROM_PATH_MAX] = "";
	if (!storage_card_try_admit(&token, root, sizeof(root))) {
		if (errno == EAGAIN) {
			rescan_requested = true;
			if (!scanning || scan_failed) {
				scanning = true;
				scan_failed = false;
				lv_label_set_text(status_label, tr("gearboy_looking_for_games"));
				lv_obj_set_hidden(status_label, false);
			}
			return;
		}
		rescan_requested = false;
		scanning = false;
		scan_failed = true;
		free(roms);
		roms = NULL;
		rom_count = 0;
		roms_token = (storage_card_token_t){0};
		rebuild_rows();
		return;
	}
	rescan_requested = false;
	pthread_mutex_lock(&scan_terminal.lock);
	scan_terminal.active = true;
	scan_terminal.generation = scan_generation;
	scan_terminal.token = token;
	memcpy(scan_terminal.root, root, sizeof(root));
	pthread_mutex_unlock(&scan_terminal.lock);
	scanning = true;
	scan_failed = false;
	lv_label_set_text(status_label, tr("gearboy_looking_for_games"));
	lv_obj_set_hidden(status_label, false);

	pthread_t thread;
	if (pthread_create(&thread, NULL, scan_thread, (void *)(uintptr_t)scan_generation) != 0) {
		pthread_mutex_lock(&scan_terminal.lock);
		scan_terminal.active = false;
		pthread_mutex_unlock(&scan_terminal.lock);
		scanning = false;
		scan_failed = true;
		free(roms);
		roms = NULL;
		rom_count = 0;
		rebuild_rows();
		return;
	}
	pthread_detach(thread);
}

void gearboypage_service_completion(void) {
	pthread_mutex_lock(&scan_terminal.lock);
	if (!scan_terminal.ready) {
		bool idle = !scan_terminal.active && !scan_terminal.closing;
		pthread_mutex_unlock(&scan_terminal.lock);
		if (idle && page_visible && rescan_requested)
			start_scan();
		return;
	}
	storage_card_token_t token = scan_terminal.token;
	bool relevant = !scan_terminal.closing && page_visible && scan_terminal.generation == scan_generation;
	pthread_mutex_unlock(&scan_terminal.lock);
	bool current = true;
	if (relevant && storage_card_try_check(&token) != 0) {
		if (errno == EAGAIN)
			return; // READY and its payload remain owned until a known result.
		current = false;
	}
	pthread_mutex_lock(&scan_terminal.lock);
	gbrom_t *list = scan_terminal.list;
	int count = scan_terminal.count;
	scan_status_t status = scan_terminal.status;
	uint32_t generation = scan_terminal.generation;
	bool closing = scan_terminal.closing;
	scan_terminal.list = NULL;
	scan_terminal.ready = false;
	scan_terminal.active = false;
	pthread_mutex_unlock(&scan_terminal.lock);
	if (!closing && page_visible && generation == scan_generation && current) {
		free(roms);
		roms = list;
		rom_count = count;
		roms_token = token;
		list = NULL;
		scanning = false;
		scan_failed = status == SCAN_ERROR;
		rebuild_rows();
	}
	free(list);
	if (relevant && !current)
		rescan_requested = true;
	if (!closing && page_visible && rescan_requested) {
		rescan_requested = false;
		start_scan();
	}
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	pthread_mutex_lock(&scan_terminal.lock);
	bool closing = scan_terminal.closing;
	pthread_mutex_unlock(&scan_terminal.lock);
	if (closing)
		return;
	page_visible = true;
	scan_generation++;
	refresh_view_buttons();
	lv_obj_scroll_to_y(list_box, 0, LV_ANIM_OFF);
	// Returning during an old scan defers one fresh pass, never a second worker.
	start_scan();
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	page_visible = false;
	scan_generation++;
	rescan_requested = false;
	scanning = false;
	// Dispose an already published result now; an active worker remains owned
	// and publishes a stale terminal record for the regular GUI service point.
	gearboypage_service_completion();
}

static void screen_deleted_cb(lv_event_t *e) {
	screen_unloaded_cb(e);
	free(roms);
	roms = NULL;
	rom_count = 0;
	roms_token = (storage_card_token_t){0};
	list_box = status_label = btn_gb = btn_gbc = NULL;
	gearboypage_screen = NULL;
}

void gearboypage_shutdown_completions(void) {
	page_visible = false;
	scan_generation++;
	rescan_requested = false;
	scanning = false;
	pthread_mutex_lock(&scan_terminal.lock);
	scan_terminal.closing = true;
	gbrom_t *list = scan_terminal.list;
	scan_terminal.list = NULL;
	if (scan_terminal.ready)
		scan_terminal.active = false;
	scan_terminal.ready = false;
	pthread_mutex_unlock(&scan_terminal.lock);
	free(list);
	// No join in navigation/shutdown hooks. A still-running worker sees closing
	// before publishing; full in-process LVGL teardown needs separate quiescence.
}

// The top-right gear, like the one on the Music page: opens Gearboy settings.
static void open_settings_cb(lv_event_t *e) {
	(void)e;
	switch_screen(gearboysettings_screen);
}

void gearboypage_init(gui_config_t *cfg) {
	lv_obj_add_style(gearboypage_screen, &theme_style_screen, 0);
	lv_obj_t *page_title = settingsrow_title(gearboypage_screen, cfg, "gearboy");
	settingsrow_title_corner_slots(page_title, cfg, 1);

	{
		lv_obj_t *button = lv_btn_create(gearboypage_screen);
		lv_obj_set_size(button, 56, 56);
		lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(button, 0, 0);
		lv_obj_set_style_shadow_width(button, 0, 0);
		lv_obj_set_style_pad_all(button, 0, 0);
		lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -cfg->padding, cfg->padding + cfg->top_bar_height);
		lv_obj_add_event_cb(button, open_settings_cb, LV_EVENT_CLICKED, NULL);

		lv_obj_t *icon = lv_image_create(button);
		lv_image_set_src(icon, &icon_music_settings);
		lv_obj_add_style(icon, &theme_style_icon, 0);
		lv_obj_center(icon);
	}

	int content_top = settingsrow_content_top(cfg);

	// The pills stay put while only the list below them scrolls. Same
	// construction as the Audiobooks page: a fixed column holding the chooser
	// row and, under it, the list.
	lv_obj_t *container = lv_obj_create(gearboypage_screen);
	lv_obj_set_size(container, lv_pct(100), cfg->screen_height - content_top);
	lv_obj_align(container, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(container, 0, 0);
	lv_obj_set_style_border_width(container, 0, 0);
	lv_obj_set_style_radius(container, 0, 0);
	lv_obj_set_style_pad_hor(container, cfg->padding, 0);
	lv_obj_set_style_pad_ver(container, 0, 0);
	lv_obj_set_style_pad_gap(container, 12, 0);
	lv_obj_set_scrollable(container, false);
	lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	switcher_attach_back_gesture(container);

	lv_obj_t *chooser = lv_obj_create(container);
	lv_obj_set_size(chooser, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(chooser, 0, 0);
	lv_obj_set_style_border_width(chooser, 0, 0);
	lv_obj_set_style_pad_all(chooser, 0, 0);
	lv_obj_set_style_pad_gap(chooser, 10, 0);
	lv_obj_set_scrollable(chooser, false);
	lv_obj_set_flex_flow(chooser, LV_FLEX_FLOW_ROW);

	btn_gb = make_view_choice(chooser, "gearboy_gb", false);
	btn_gbc = make_view_choice(chooser, "gearboy_gbc", true);

	list_box = lv_obj_create(container);
	lv_obj_set_width(list_box, lv_pct(100));
	lv_obj_set_flex_grow(list_box, 1);
	lv_obj_set_style_bg_opa(list_box, 0, 0);
	lv_obj_set_style_border_width(list_box, 0, 0);
	lv_obj_set_style_radius(list_box, 0, 0);
	lv_obj_set_style_pad_all(list_box, 0, 0);
	lv_obj_set_style_pad_bottom(list_box, cfg->padding, 0);
	lv_obj_set_style_pad_gap(list_box, 10, 0);
	lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(list_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scroll_dir(list_box, LV_DIR_VER);
	switcher_attach_back_gesture(list_box);

	status_label = lv_label_create(gearboypage_screen);
	lv_label_set_text(status_label, "");
	lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(status_label, lv_pct(80));
	lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(status_label, &font_ui_20, 0);
	lv_obj_add_style(status_label, &theme_style_text_dim, 0);
	lv_obj_center(status_label);

	refresh_view_buttons();
	theme_register_refresh(refresh_view_buttons);

	lv_obj_add_event_cb(gearboypage_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(gearboypage_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	lv_obj_add_event_cb(gearboypage_screen, screen_deleted_cb, LV_EVENT_DELETE, NULL);
	switcher_attach_back_gesture(gearboypage_screen);
}
