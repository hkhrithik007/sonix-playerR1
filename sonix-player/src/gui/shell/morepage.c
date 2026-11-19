#include "morepage.h"

#include "lvgl/lvgl.h"

#include "src/gui/audio/dacpage.h"
#include "src/gui/audio/metronomepage.h"
#include "src/gui/ebook/ebookpage.h"
#include "src/gui/flappybird/flappybird.h"
#include "src/gui/gearboy/gearboypage.h"
#include "src/gui/library/filespage.h"
#include "src/gui/shell/gridpage.h"
#include "main_menu.h"
#include <stdint.h>
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/sx_styles.h"

lv_obj_t *morepage_screen;

static gui_config_t *page_cfg;
static lv_obj_t *tile_grid;

void morepage_rebuild(void) {
	if (!tile_grid) return;
	grid_entry_t entries[HOME_ITEM_COUNT + 6];
	int count = 0;
	for (int i = 0; i <= HOME_CONTINUE_BOOK; i++)
		if (!main_menu_contains((home_item_t)i)) main_menu_get_entry((home_item_t)i, &entries[count++]);
	const grid_entry_t tools[] = {
		{"dac", &icon_menu_dac, &dacpage_screen, NULL},
		{"metronome", &icon_menu_metronome, &metronomepage_screen, NULL},
#if GB_CORE
		// The Game Boy emulator. Present only when the binary was built with
		// GB=1 (see the Makefile).
		{"gearboy", &icon_menu_gearboy, &gearboypage_screen, NULL},
#endif
		// An action rather than a screen: the shelf reads the Ebook folder again
		// every time it is opened, so a book copied onto the card turns up
		// without a restart.
		{"books", &icon_menu_books, NULL, ebookpage_open},
		// An action for the same reason the shelf is one: the listing is read
		// when the page is entered, so a card that changed under the player
		// shows what is on it now.
		{"file_explorer", &icon_menu_file_explorer, NULL, filespage_open},
		// An action: the artwork is read from the resource tree on the way in.
		{"flappy_bird", &icon_menu_flappy_bird, NULL, flappybird_open},
	};

	int sections = count;
	for (unsigned i = 0; i < sizeof(tools) / sizeof(tools[0]); i++) {
		if (tools[i].action == filespage_open && main_menu_contains(HOME_FILES)) continue;
		entries[count++] = tools[i];
	}
	gridpage_set_entries(tile_grid, page_cfg, entries, count, 2, 3, true);
	int row = 0;
	for (int i = 0; i <= HOME_CONTINUE_BOOK && row < sections; i++) {
		if (main_menu_contains((home_item_t)i)) continue;
		lv_obj_add_event_cb(lv_obj_get_child(tile_grid, row++), main_menu_hold, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
	}
}

static void search_cb(lv_event_t *e) { (void)e; main_menu_open_search(); }

void morepage_init(gui_config_t *cfg) {
	page_cfg = cfg;
	tile_grid = gridpage_build(morepage_screen, cfg, NULL, 0, 2, 3, true);
	morepage_rebuild();
	settingsrow_title_corner_slots(settingsrow_title(morepage_screen, cfg, "more"), cfg, 1);
	lv_obj_t *search = lv_button_create(morepage_screen);
	lv_obj_remove_style_all(search);
	lv_obj_set_size(search, 56, 56);
	lv_obj_align(search, LV_ALIGN_TOP_RIGHT, -cfg->padding, cfg->padding + cfg->top_bar_height);
	lv_obj_add_style(search, &sx_style_press, LV_STATE_PRESSED);
	lv_obj_add_event_cb(search, search_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *icon = lv_image_create(search);
	lv_image_set_src(icon, &icon_search);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_center(icon);
}
