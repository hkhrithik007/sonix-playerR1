#include "hometiles.h"

#include <stdint.h>
#include <stdio.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/main_menu.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/sx_row.h"
#include "src/gui/shell/sx_sheet.h"
#include "src/gui/shell/sx_styles.h"
#include "src/gui/shell/sx_toast.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"

lv_obj_t *hometiles_screen;

// Five pinned shortcuts, with the same reorder gesture as the Music editor.
// Unpinned shortcuts live in a categorized sheet, not in a long second list.
// More is fixed on Home and is never a movable row here.

#define ROW_HEIGHT 72
#define ROW_GAP 8
#define ROW_PITCH (ROW_HEIGHT + ROW_GAP)
#define ROW_RADIUS 12
#define ROW_PAD 20
#define ICON_BOX 56
// The tiles' own pictures are 112 px: shown at half that.
#define ICON_SCALE (LV_SCALE_NONE / 2)
#define HEADING_H 40

// The grip is only as large as its glyph and a margin round it; the room kept
// for it at the end of the row is wider.
#define GRIP_SIZE 48
#define GRIP_SPACE 64

// How far the finger has to travel before a release counts as a drop.
#define DROP_MIN_TRAVEL 8

// How close to the top or bottom of the page a held row has to be before the
// page moves under it, and how far it moves each time the event comes round.
#define EDGE_PX 70
#define EDGE_STEP 18

typedef struct {
	lv_obj_t *row;
	lv_obj_t *icon;
	lv_obj_t *name;
	lv_obj_t *grip;
} entry_t;

static lv_obj_t *page;
static lv_obj_t *body;
static lv_obj_t *in_use_count;
static lv_obj_t *in_use_heading;
static lv_obj_t *add_row;
static lv_obj_t *drop_line;
static lv_obj_t *fixed_note;
static entry_t entries[HOME_CUSTOM_SLOTS];

// Pinned IDs are rebound after a completed operation, never during a drag.
static home_item_t row_button[HOME_CUSTOM_SLOTS];
static int row_count;

// The row being carried, or -1.
static int drag_row = -1;
static bool drag_moved;
static lv_point_t drag_from;

static void refresh(void);
static void refresh_async(void *unused);

// The heading and count occupy a single row above the pinned shortcuts.
static int in_use_top(void) { return HEADING_H; }

static void refresh(void) {
	row_count = main_menu_count();
	if (row_count > HOME_CUSTOM_SLOTS)
		row_count = HOME_CUSTOM_SLOTS;
	for (int i = 0; i < HOME_CUSTOM_SLOTS; i++) {
		lv_obj_set_hidden(entries[i].row, i >= row_count);
		if (i >= row_count)
			continue;
		row_button[i] = main_menu_at(i);
		sx_row_set_user(entries[i].row, (void *)(intptr_t)row_button[i]);
		lv_obj_set_y(entries[i].row, in_use_top() + i * ROW_PITCH);
		const lv_image_dsc_t *glyph = main_menu_icon(row_button[i]);
		lv_image_set_src(entries[i].icon, glyph);
		lv_image_set_scale(entries[i].icon, 256 * 32 / glyph->header.w);
		lv_label_set_text(entries[i].name, tr(main_menu_tag(row_button[i])));
	}
	lv_label_set_text_fmt(in_use_count, "%d/%d", row_count, HOME_CUSTOM_SLOTS);
	lv_label_set_text(in_use_heading, tr("hometiles_on_main"));
	int add_y = in_use_top() + row_count * ROW_PITCH + 16;
	lv_obj_set_y(add_row, add_y);
	lv_label_set_text(settingsrow_name_label(add_row), tr("hometiles_add"));
	int note_y = add_y + ROW_HEIGHT + 16;
	lv_obj_set_y(fixed_note, note_y);
	lv_label_set_text(fixed_note, tr("hometiles_more_fixed"));
	lv_obj_update_layout(fixed_note);
	lv_obj_set_height(body, note_y + lv_obj_get_height(fixed_note) + 16);
}

static void refresh_async(void *unused) {
	(void)unused;
	refresh();
}

// ---------------------------------------------------------------------------
// Carrying a row
// ---------------------------------------------------------------------------

// The insertion gap is counted with the carried row still in its place.
typedef struct {
	int at;
} target_t;

static target_t target_at(lv_point_t point) {
	lv_area_t area;
	lv_obj_get_coords(body, &area);
	int at = (point.y - area.y1 - in_use_top() + ROW_PITCH / 2) / ROW_PITCH;
	if (at < 0)
		at = 0;
	if (at > main_menu_count())
		at = main_menu_count();
	return (target_t){at};
}

// The accent rule in the gap a drop would go into. On the canvas, so it
// scrolls with the rows.
static void drop_line_show(target_t t) {
	int top = in_use_top();
	lv_obj_set_pos(drop_line, 0, top + t.at * ROW_PITCH - ROW_GAP / 2 - 2);
	lv_obj_set_hidden(drop_line, false);
	lv_obj_move_foreground(drop_line);
}

static void carried_look(int index, bool on) {
	lv_obj_t *row = entries[index].row;
	lv_obj_set_style_border_width(row, on ? 2 : 0, 0);
	if (on) {
		lv_obj_set_style_border_color(row, theme()->accent, 0);
		lv_obj_set_style_border_opa(row, LV_OPA_COVER, 0);
	}
}

// The gap is counted with the carried row in place; once it is lifted out,
// every gap below it in its own list is one earlier.
static void drop(int index, target_t t) {
	int at = t.at;
	if (index < at)
		at--;
	if (at != index)
		main_menu_move(row_button[index], at);
}

// The page follows the finger when a held row reaches either end of it.
static void edge_scroll(lv_point_t point) {
	lv_area_t view;
	lv_obj_get_coords(page, &view);
	int step = 0;
	if (point.y < view.y1 + EDGE_PX) {
		step = -EDGE_STEP;
	} else if (point.y > view.y2 - EDGE_PX) {
		step = EDGE_STEP;
	}
	if (step == 0) {
		return;
	}
	int scroll = lv_obj_get_scroll_y(page);
	int limit = scroll + lv_obj_get_scroll_bottom(page);
	int wanted = scroll + step;
	wanted = wanted < 0 ? 0 : (wanted > limit ? limit : wanted);
	if (wanted != scroll) {
		lv_obj_scroll_to_y(page, wanted, LV_ANIM_OFF);
	}
}

static void drag_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *grip = lv_event_get_current_target(e);

	lv_indev_t *indev = lv_indev_active();
	lv_point_t point = {0, 0};
	if (indev) {
		lv_indev_get_point(indev, &point);
	}

	if (code == LV_EVENT_PRESSED) {
		if (!indev || player_sheet_drag_active() || switcher_back_drag_active())
			return;
		drag_row = -1;
		for (int i = 0; i < row_count; i++) {
			if (entries[i].grip == grip) {
				drag_row = i;
			}
		}
		if (drag_row < 0) {
			return;
		}
		drag_moved = false;
		drag_from = point;
		carried_look(drag_row, true);
		return;
	}

	if (drag_row < 0) {
		return;
	}

	if (code == LV_EVENT_PRESSING && indev) {
		edge_scroll(point);
		if (LV_ABS(point.y - drag_from.y) > DROP_MIN_TRAVEL || LV_ABS(point.x - drag_from.x) > DROP_MIN_TRAVEL) {
			drag_moved = true;
		}
		if (drag_moved) {
			drop_line_show(target_at(point));
		}
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		int index = drag_row;
		drag_row = -1;
		lv_obj_set_hidden(drop_line, true);
		carried_look(index, false);

		if (code == LV_EVENT_RELEASED && indev && drag_moved) {
			drop(index, target_at(point));
		}
		// Not here: the repaint rebinds the very row whose release is being
		// handled. It waits for the event to unwind.
		lv_async_call(refresh_async, NULL);
	}
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

static int position_of(home_item_t item) {
	for (int i = 0; i < main_menu_count(); i++)
		if (main_menu_at(i) == item)
			return i;
	return -1;
}

static void move_action(void *user) {
	intptr_t encoded = (intptr_t)user;
	home_item_t item = (home_item_t)(encoded / 4);
	int action = (int)(encoded % 4);
	int at = position_of(item);
	if (action == 0) {
		main_menu_hide(item);
	} else {
		int to = action == 1 ? at - 1 : action == 2 ? at + 1 : main_menu_count();
		if (to >= 0 && !main_menu_move(item, to))
			toast_error("hometiles_full");
	}
	refresh();
}

static void row_menu_cb(lv_obj_t *row, void *user) {
	(void)row;
	if (drag_row >= 0 || player_sheet_drag_active() || switcher_back_drag_active())
		return;
	home_item_t item = (home_item_t)(intptr_t)user;
	int at = position_of(item);
	if (at < 0)
		return;
	sx_sheet_item_t items[3];
	int n = 0;
	if (at > 0)
		items[n++] = (sx_sheet_item_t){.label = tr("hometiles_earlier"), .icon = &icon_chevron_up, .action = move_action, .user = (void *)(intptr_t)(item * 4 + 1)};
	if (at >= 0 && at < main_menu_count() - 1)
		items[n++] = (sx_sheet_item_t){.label = tr("hometiles_later"), .icon = &icon_chevron_down, .action = move_action, .user = (void *)(intptr_t)(item * 4 + 2)};
	items[n++] = (sx_sheet_item_t){.label = tr(at >= 0 ? (item <= HOME_CONTINUE_BOOK ? "hometiles_to_more" : "hometiles_remove") : "hometiles_to_main"), .icon = &icon_menu_more, .action = move_action, .user = (void *)(intptr_t)(item * 4 + (at >= 0 ? 0 : 3))};
	sx_sheet_menu(tr(main_menu_tag(item)), NULL, items, n);
}

// Only the latest toast owns an undo. Insert this ID into the current order;
// do not restore a whole snapshot over changes made after the swipe.
static struct {
	home_item_t item;
	int position;
	bool valid;
} removed;

static void undo_remove(void *unused) {
	(void)unused;
	if (!removed.valid)
		return;
	removed.valid = false;
	if (main_menu_contains(removed.item))
		return;
	if (!main_menu_move(removed.item, removed.position))
		toast_error("hometiles_full");
	lv_async_call(refresh_async, NULL);
}

static void remove_swipe(lv_obj_t *row, void *user) {
	(void)row;
	if (drag_row >= 0 || player_sheet_drag_active() || switcher_back_drag_active())
		return;
	home_item_t item = (home_item_t)(intptr_t)user;
	int at = position_of(item);
	if (at < 0)
		return;
	removed.item = item;
	removed.position = at;
	removed.valid = true;
	main_menu_hide(item);
	char text[256];
	snprintf(text, sizeof(text), "%s: %s", tr("hometiles_remove"), tr(main_menu_tag(item)));
	sx_toast_undo(text, undo_remove, NULL);
	lv_async_call(refresh_async, NULL);
}

static bool in_category(home_item_t item, int category) {
	if (category == 0)
		return item <= HOME_CONTINUE_BOOK;
	if (category == 1)
		return item >= HOME_MUSIC_ALL && item <= HOME_FILES;
	return (item >= HOME_TIDAL && item <= HOME_ZVUK) || item == HOME_YANDEX_WAVE || item == HOME_YANDEX_LIKED || item == HOME_SPOTIFY;
}

static void add_item(void *user) {
	home_item_t item = (home_item_t)(intptr_t)user;
	if (!main_menu_available(item) || main_menu_contains(item))
		return;
	if (!main_menu_move(item, main_menu_count()))
		toast_error("hometiles_full");
	refresh();
}

static void category_action(void *user) {
	int category = (int)(intptr_t)user;
	sx_sheet_item_t items[HOME_ITEM_COUNT];
	int n = 0;
	for (int i = 0; i < HOME_ITEM_COUNT; i++) {
		home_item_t item = (home_item_t)i;
		if (!main_menu_available(item) || main_menu_contains(item) || !in_category(item, category))
			continue;
		items[n++] = (sx_sheet_item_t){.label = tr(main_menu_tag(item)), .icon = main_menu_icon(item), .action = add_item, .user = (void *)(intptr_t)item};
	}
	if (!n) {
		toast_error("hometiles_none");
		return;
	}
	const char *title = category == 0 ? "hometiles_basic" : category == 1 ? "music" : "streaming";
	sx_sheet_menu(tr(title), NULL, items, n);
}

static void add_cb(lv_event_t *e) {
	(void)e;
	if (main_menu_count() >= HOME_CUSTOM_SLOTS) {
		toast_error("hometiles_full");
		return;
	}
	sx_sheet_item_t items[] = {
		{.label = tr("hometiles_basic"), .icon = &icon_menu_more, .action = category_action, .user = (void *)(intptr_t)0},
		{.label = tr("music"), .icon = &icon_menu_music, .action = category_action, .user = (void *)(intptr_t)1},
		{.label = tr("streaming"), .icon = &icon_menu_streaming, .action = category_action, .user = (void *)(intptr_t)2},
	};
	sx_sheet_menu(tr("hometiles_add"), NULL, items, 3);
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	if (drag_row >= 0) {
		carried_look(drag_row, false);
		drag_row = -1;
	}
	lv_obj_set_hidden(drop_line, true);
	refresh();
}

static lv_obj_t *make_heading(lv_obj_t *parent, const char *tag, int y) {
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text(label, tr(tag));
	lv_obj_add_style(label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_obj_set_pos(label, 4, y + 10);
	return label;
}

static void make_row(entry_t *entry, int width) {
	const sx_row_handlers_t handlers = {.on_tap = row_menu_cb, .on_swipe_end = remove_swipe, .end_is_remove = true, .remove_label_key = "hometiles_remove"};
	lv_obj_t *row;
	entry->row = sx_row_create_custom(body, &handlers, ROW_HEIGHT, &row);
	lv_obj_set_size(entry->row, width, ROW_HEIGHT);
	lv_obj_set_x(entry->row, 0);
	lv_obj_set_style_radius(entry->row, ROW_RADIUS, 0);
	lv_obj_set_layout(row, LV_LAYOUT_NONE);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_set_style_radius(row, ROW_RADIUS, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, ROW_PAD, 0);
	lv_obj_set_style_pad_ver(row, 0, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_clickable(row, true);

	lv_obj_t *box = lv_obj_create(row);
	lv_obj_remove_style_all(box);
	lv_obj_set_size(box, ICON_BOX, ROW_HEIGHT);
	lv_obj_align(box, LV_ALIGN_LEFT_MID, 0, 0);
	lv_obj_set_clickable(box, false);

	// Match the accent glyphs on the Home tiles.
	entry->icon = lv_image_create(box);
	lv_image_set_scale(entry->icon, ICON_SCALE);
	lv_obj_add_style(entry->icon, &sx_style_icon_accent, 0);
	lv_obj_set_clickable(entry->icon, false);
	lv_obj_center(entry->icon);

	entry->name = lv_label_create(row);
	lv_obj_add_style(entry->name, &theme_style_text, 0);
	lv_obj_set_style_text_font(entry->name, &font_ui_24, 0);
	lv_label_set_long_mode(entry->name, LV_LABEL_LONG_DOT);
	lv_obj_set_clickable(entry->name, false);
	lv_obj_set_width(entry->name, width - ROW_PAD - ICON_BOX - 16 - GRIP_SPACE);
	lv_obj_align(entry->name, LV_ALIGN_LEFT_MID, ICON_BOX + 16, 0);

	// The handle, and the only part of the row that can be taken hold of. Not
	// a button: a tap on it does nothing. A drag on it must not find the page
	// and scroll it, hence no scroll chain.
	entry->grip = lv_obj_create(row);
	lv_obj_set_size(entry->grip, GRIP_SIZE, GRIP_SIZE);
	// Centred in its room, which runs into the row's right padding.
	lv_obj_align(entry->grip, LV_ALIGN_RIGHT_MID, ROW_PAD - (GRIP_SPACE - GRIP_SIZE) / 2, 0);
	lv_obj_set_style_bg_opa(entry->grip, 0, 0);
	lv_obj_set_style_border_width(entry->grip, 0, 0);
	lv_obj_set_style_pad_all(entry->grip, 0, 0);
	lv_obj_set_scrollable(entry->grip, false);
	lv_obj_set_scroll_chain_ver(entry->grip, false);
	lv_obj_set_scroll_chain_hor(entry->grip, false);
	lv_obj_set_clickable(entry->grip, true);

	lv_obj_t *grip_icon = lv_image_create(entry->grip);
	lv_image_set_src(grip_icon, &icon_grip);
	lv_obj_add_style(grip_icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(grip_icon, LV_OPA_COVER, 0);
	lv_obj_center(grip_icon);

	lv_obj_add_event_cb(entry->grip, drag_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(entry->grip, drag_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(entry->grip, drag_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(entry->grip, drag_cb, LV_EVENT_PRESS_LOST, NULL);
}

static void build_page(gui_config_t *cfg) {
	hometiles_screen = lv_obj_create(NULL);
	lv_obj_add_style(hometiles_screen, &theme_style_screen, 0);

	page = settingsrow_page(hometiles_screen, cfg, "hometiles_title");
	// No buttons in the corner, so the heading may run to the edge: the name
	// of the page is long in every language.
	settingsrow_title_corner_slots(settingsrow_page_title(hometiles_screen), cfg, 0);

	int width = cfg->screen_width - 2 * cfg->padding;

	body = lv_obj_create(page);
	lv_obj_set_width(body, width);
	lv_obj_set_height(body, ROW_PITCH);
	lv_obj_set_style_bg_opa(body, 0, 0);
	lv_obj_set_style_border_width(body, 0, 0);
	lv_obj_set_style_pad_all(body, 0, 0);
	lv_obj_set_scrollable(body, false);

	in_use_heading = make_heading(body, "hometiles_on_main", 0);

	// How many of the tiles are on the page, level with the first heading.
	in_use_count = lv_label_create(body);
	lv_obj_add_style(in_use_count, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(in_use_count, &font_ui_22, 0);
	lv_obj_align(in_use_count, LV_ALIGN_TOP_RIGHT, -4, 10);

	for (int i = 0; i < HOME_CUSTOM_SLOTS; i++) {
		make_row(&entries[i], width);
	}

	drop_line = lv_obj_create(body);
	lv_obj_set_size(drop_line, width, 4);
	lv_obj_set_scrollable(drop_line, false);
	lv_obj_set_clickable(drop_line, false);
	lv_obj_set_style_border_width(drop_line, 0, 0);
	lv_obj_set_style_shadow_width(drop_line, 0, 0);
	lv_obj_set_style_pad_all(drop_line, 0, 0);
	lv_obj_add_style(drop_line, &theme_style_accent_bg, 0);
	lv_obj_set_style_radius(drop_line, 2, 0);
	lv_obj_set_hidden(drop_line, true);

	add_row = settingsrow_add(body, "hometiles_add", NULL, add_cb, NULL);
	lv_obj_set_size(add_row, width, ROW_HEIGHT);
	lv_obj_set_x(add_row, 0);
	fixed_note = lv_label_create(body);
	lv_obj_set_width(fixed_note, width - 8);
	lv_obj_set_x(fixed_note, 4);
	lv_label_set_long_mode(fixed_note, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(fixed_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(fixed_note, &font_ui_22, 0);

	lv_obj_add_event_cb(hometiles_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(hometiles_screen);

	refresh();
}

void hometiles_open(gui_config_t *cfg) {
	if (!cfg)
		return;
	if (!hometiles_screen)
		build_page(cfg);
	refresh();
	switch_screen(hometiles_screen);
}
