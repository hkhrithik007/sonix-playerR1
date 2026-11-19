#include "sx_card.h"

#include <stdlib.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/sx_palette.h"
#include "src/gui/shell/sx_styles.h"
#include "src/gui/shell/sx_tokens.h"
#include "src/gui/shell/theme.h"

#define DIM_OPA 115 // 45 %

// The parts of a card built here, kept on the card (its user data).
typedef struct {
	lv_obj_t *head;
	lv_obj_t *text;
	lv_obj_t *title;
	lv_obj_t *subtitle;
	lv_obj_t *value;
	lv_obj_t *chevron;
	lv_obj_t *sw;
	lv_event_cb_t click_cb;
	void *click_user;
} card_parts_t;

static void parts_free_cb(lv_event_t *e) { free(lv_event_get_user_data(e)); }

static card_parts_t *parts_of(lv_obj_t *card) { return card ? lv_obj_get_user_data(card) : NULL; }

static void frame_draw_cb(lv_event_t *e) {
	int family = theme_get_palette();
	if (family != 4 && family != 5)
		return;
	lv_obj_t *obj = lv_event_get_current_target_obj(e);
	lv_obj_t *parent = lv_obj_get_parent(obj);
	if (parent && lv_obj_get_user_flag(parent, 3))
		return; // The navigation group owns the outer frame.
	const sx_palette_t *p = sx_pal();
	lv_draw_rect_dsc_t dsc;
	lv_draw_rect_dsc_init(&dsc);
	dsc.bg_opa = LV_OPA_TRANSP;
	dsc.border_width = 1;
	dsc.border_color = p->surface_edge;
	dsc.border_opa = LV_OPA_MIX2(p->surface_edge_opa, lv_obj_get_style_opa_recursive(obj, LV_PART_MAIN));
	dsc.radius = lv_obj_get_style_radius(obj, LV_PART_MAIN);
	lv_area_t area;
	lv_obj_get_coords(obj, &area);
	lv_draw_rect(lv_event_get_layer(e), &dsc, &area);
}

void sx_card_frame(lv_obj_t *obj) {
	if (!obj)
		return;
	lv_obj_remove_event_cb(obj, frame_draw_cb);
	lv_obj_add_event_cb(obj, frame_draw_cb, LV_EVENT_DRAW_POST, NULL);
}

lv_obj_t *sx_card_nav_group(lv_obj_t *parent) {
	lv_obj_t *group = sx_box(parent);
	lv_obj_set_size(group, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(group, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(group, 0, 0);
	lv_obj_add_style(group, &sx_style_panel, 0);
	sx_card_frame(group);
	lv_obj_set_style_radius(group, SX_CARD_R, 0);
	lv_obj_set_style_clip_corner(group, true, 0);
	lv_obj_set_user_flag(group, 3, true);
	lv_obj_set_event_bubble(group, true);
	return group;
}

// The same for a part of the head row: asked for one, the head is in use.
static card_parts_t *head_parts(lv_obj_t *card) {
	card_parts_t *p = parts_of(card);
	if (p && p->head) {
		lv_obj_set_hidden(p->head, false);
	}
	return p;
}

// ---------------------------------------------------------------------------
// The looks
// ---------------------------------------------------------------------------

void sx_card_style(lv_obj_t *obj) {
	if (!obj) {
		return;
	}
	lv_obj_add_style(obj, &sx_style_panel, 0);
	lv_obj_add_style(obj, &sx_style_panel_press, LV_STATE_PRESSED);
	lv_obj_set_style_radius(obj, SX_CARD_R, 0);
	lv_obj_set_style_border_width(obj, 0, 0);
	lv_obj_set_style_shadow_width(obj, 0, 0);
	lv_obj_set_style_outline_width(obj, 0, 0);
	sx_card_frame(obj);
	lv_obj_set_style_pad_hor(obj, SX_CARD_PAD_H, 0);
	lv_obj_set_style_pad_ver(obj, SX_CARD_PAD_V, 0);
}

void sx_pill_style(lv_obj_t *pill) {
	if (!pill) {
		return;
	}
	lv_obj_set_height(pill, SX_PILL_H);
	lv_obj_set_style_pad_hor(pill, SX_PILL_PAD_H, 0);
	lv_obj_set_style_pad_ver(pill, 0, 0);
	lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_border_width(pill, 0, 0);
	lv_obj_set_style_shadow_width(pill, 0, 0);
	lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
	lv_obj_add_style(pill, &sx_style_panel_press, 0);
	lv_obj_add_style(pill, &sx_style_panel_text, 0);
	lv_obj_add_style(pill, &sx_style_panel, LV_STATE_PRESSED);
	lv_obj_set_style_transform_width(pill, 0, LV_STATE_PRESSED);
	lv_obj_set_style_transform_height(pill, 0, LV_STATE_PRESSED);
	// Half the gap each way: a tap between two pills goes to the nearer one.
	lv_obj_set_ext_click_area(pill, SX_PILL_GAP / 2);
}

void sx_pill_set_active(lv_obj_t *pill, bool active) {
	if (!pill) {
		return;
	}
	if (lv_obj_get_user_flag(lv_obj_get_parent(pill), 2)) {
		lv_obj_t *label = lv_obj_get_child(pill, 0);
		lv_obj_remove_style(pill, &sx_style_panel_accent, 0);
		lv_obj_remove_style(pill, &sx_style_panel_accent_press, LV_STATE_PRESSED);
		lv_obj_remove_style(label, &sx_style_panel_text, 0);
		lv_obj_remove_style(label, &sx_style_panel_text2, 0);
		if (active)
			lv_obj_add_state(pill, LV_STATE_CHECKED);
		else {
			lv_obj_remove_state(pill, LV_STATE_CHECKED);
			lv_obj_add_style(label, &sx_style_panel_text2, 0);
		}
		lv_obj_set_style_text_font(label, active ? &font_ui_22_bold : &font_ui_22, 0);
		return;
	}
	// The accent's style goes on top of the neutral one, so it wins while
	// there; both follow the palette by themselves.
	lv_obj_remove_style(pill, &sx_style_panel_accent, 0);
	lv_obj_remove_style(pill, &sx_style_panel_accent_press, LV_STATE_PRESSED);
	if (active) {
		lv_obj_add_style(pill, &sx_style_panel_accent, 0);
		lv_obj_add_style(pill, &sx_style_panel_accent_press, LV_STATE_PRESSED);
	}
	// A label coloured by hand (the pages' own pills did that before 2.0)
	// would hide the style's text colour.
	lv_obj_t *label = lv_obj_get_child(pill, 0);
	if (label) {
		lv_obj_remove_local_style_prop(label, LV_STYLE_TEXT_COLOR, 0);
	}
}

static void choice_style_changed(lv_event_t *e) { sx_choice_reflow(lv_event_get_target(e)); }

void sx_choice_reflow(lv_obj_t *row) {
	static bool busy;
	if (busy)
		return;
	// Compact service/search bars keep their own height and scrolling contract.
	if (lv_obj_get_style_height(row, 0) != LV_SIZE_CONTENT || lv_obj_get_style_width(row, 0) != lv_pct(100)) {
		return;
	}
	busy = true;
	uint32_t count = lv_obj_get_child_count(row);
	if (!lv_obj_get_user_flag(row, 2)) {
		lv_obj_add_event_cb(row, choice_style_changed, LV_EVENT_STYLE_CHANGED, NULL);
	}
	lv_obj_set_user_flag(row, 2, true);
	lv_obj_add_style(row, &sx_style_panel_press, 0);
	lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(row, SX_CARD_R, 0);
	lv_obj_set_style_pad_all(row, 4, 0);
	lv_obj_set_style_pad_hor(row, 4, 0);
	lv_obj_set_style_pad_gap(row, 4, 0);
	bool single_row = count <= (fonts_large_text() ? 2U : 4U);
	if (single_row && count > 2) {
		int width = lv_obj_get_content_width(row);
		if (width <= 0)
			width = 400;
		int label_width = (width - 4 * ((int)count - 1)) / (int)count - 16;
		for (uint32_t i = 0; i < count && single_row; i++) {
			lv_obj_t *label = lv_obj_get_child(lv_obj_get_child(row, (int)i), 0);
			const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
			lv_point_t size;
			lv_text_get_size(&size, lv_label_get_text(label), font, 0, 0, LV_MAX(1, label_width), LV_TEXT_FLAG_NONE);
			if (size.y > lv_font_get_line_height(font))
				single_row = false;
		}
	}
	lv_obj_set_flex_flow(row, single_row ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_ROW_WRAP);
	for (uint32_t i = 0; i < count; i++) {
		lv_obj_t *button = lv_obj_get_child(row, (int)i);
		lv_obj_set_size(button, single_row ? 0 : lv_pct(count % 2 && i == count - 1 ? 100 : 48), 80);
		lv_obj_set_flex_grow(button, single_row ? 1 : 0);
		lv_obj_set_style_pad_hor(button, 8, 0);
		lv_obj_set_style_radius(button, 12, 0);
		lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
		lv_obj_add_style(button, &sx_style_panel_accent, LV_STATE_CHECKED);
		lv_obj_add_style(button, &sx_style_panel_accent_press, LV_STATE_CHECKED | LV_STATE_PRESSED);
		lv_obj_set_style_bg_opa(button, LV_OPA_40, LV_STATE_PRESSED);
		lv_obj_set_ext_click_area(button, 0);
		lv_obj_t *label = lv_obj_get_child(button, 0);
		lv_obj_set_width(label, lv_pct(100));
		lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
		lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_translate_y(label, 0, 0);
	}
	busy = false;
}

void sx_switch_style(lv_obj_t *sw) {
	if (!sw) {
		return;
	}
	lv_obj_set_size(sw, SX_SWITCH_W, SX_SWITCH_H);
	lv_obj_add_style(sw, &sx_style_panel_press, LV_PART_MAIN);
	lv_obj_add_style(sw, &sx_style_panel_accent, LV_PART_INDICATOR | LV_STATE_CHECKED);
	lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
	lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
	lv_obj_set_style_shadow_width(sw, 0, LV_PART_KNOB);
	// The knob is the track's height less the padding: 36 - 2 x 4 = 28.
	lv_obj_set_style_pad_all(sw, -(SX_SWITCH_H - SX_SWITCH_KNOB) / 2, LV_PART_KNOB);
}

// ---------------------------------------------------------------------------
// A card
// ---------------------------------------------------------------------------

lv_obj_t *sx_card_create(lv_obj_t *parent) {
	lv_obj_t *card = lv_obj_create(parent);
	lv_obj_remove_style_all(card);
	sx_card_style(card);
	lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_clickable(card, false);
	lv_obj_set_event_bubble(card, true);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(card, 10, 0);

	card_parts_t *p = calloc(1, sizeof(*p));
	if (p) {
		lv_obj_set_user_data(card, p);
		lv_obj_add_event_cb(card, parts_free_cb, LV_EVENT_DELETE, p);

		p->head = sx_box(card);
		lv_obj_set_size(p->head, lv_pct(100), LV_SIZE_CONTENT);
		lv_obj_set_style_min_height(p->head, SX_CARD_MIN_H, 0);
		lv_obj_set_flex_flow(p->head, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(p->head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_column(p->head, 10, 0);

		// The text column carries the head's 60 px itself and centres its
		// lines in them: a row track is only as tall as its tallest child, so
		// a minimum on the head alone would leave the text at its top.
		p->text = sx_box(p->head);
		// Allocate the remaining row width before wrapping the title.
		lv_obj_set_width(p->text, 0);
		lv_obj_set_height(p->text, LV_SIZE_CONTENT);
		lv_obj_set_style_min_height(p->text, SX_CARD_MIN_H, 0);
		lv_obj_set_flex_grow(p->text, 1);
		lv_obj_set_flex_flow(p->text, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(p->text, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
		lv_obj_set_style_pad_row(p->text, 4, 0);
		// Until something is put in it: a card of pills alone has no head.
		lv_obj_set_hidden(p->head, true);
	}
	return card;
}

static void card_clicked_cb(lv_event_t *e) {
	lv_obj_t *card = lv_event_get_current_target(e);
	card_parts_t *p = parts_of(card);
	if (!p || !p->click_cb) {
		return;
	}
	// A swipe or a scroll that ended on the card is not a tap on it.
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	p->click_cb(e);
}

void sx_card_on_click(lv_obj_t *card, lv_event_cb_t cb, void *user) {
	card_parts_t *p = parts_of(card);
	if (!p) {
		return;
	}
	p->click_cb = cb;
	p->click_user = user;
	lv_obj_set_clickable(card, true);
	lv_obj_remove_event_cb(card, card_clicked_cb);
	lv_obj_add_event_cb(card, card_clicked_cb, LV_EVENT_CLICKED, user);
}

lv_obj_t *sx_card_title(lv_obj_t *card, const char *text) {
	card_parts_t *p = head_parts(card);
	if (!p) {
		return NULL;
	}
	if (!p->title) {
		p->title = lv_label_create(p->text);
		lv_obj_set_width(p->title, lv_pct(100));
		lv_label_set_long_mode(p->title, LV_LABEL_LONG_WRAP);
		lv_obj_set_style_text_font(p->title, &font_ui_24_bold, 0);
		lv_obj_add_style(p->title, &sx_style_panel_text, 0);
		lv_obj_move_to_index(p->title, 0);
	}
	sx_set_text(p->title, text ? text : "");
	return p->title;
}

lv_obj_t *sx_card_subtitle(lv_obj_t *card, const char *text, bool mono) {
	card_parts_t *p = head_parts(card);
	if (!p) {
		return NULL;
	}
	bool created = !p->subtitle;
	if (created) {
		p->subtitle = lv_label_create(p->sw ? card : p->text);
		lv_obj_set_width(p->subtitle, lv_pct(100));
		lv_label_set_long_mode(p->subtitle, LV_LABEL_LONG_WRAP);
	}
	const lv_font_t *font = mono ? &font_ui_17 : &font_ui_22;
	// The two component modes keep their shared, theme-bound colour style.
	if (created || lv_obj_get_style_text_font(p->subtitle, 0) != font) {
		lv_obj_remove_style(p->subtitle, &sx_style_panel_text2, 0);
		lv_obj_remove_style(p->subtitle, &sx_style_panel_action, 0);
		lv_obj_add_style(p->subtitle, mono ? &sx_style_panel_action : &sx_style_panel_text2, 0);
		lv_obj_set_style_text_font(p->subtitle, font, 0);
	}
	sx_set_text(p->subtitle, text ? text : "");
	lv_obj_set_hidden(p->subtitle, !text || !text[0]);
	return p->subtitle;
}

lv_obj_t *sx_card_value(lv_obj_t *card, const char *text) {
	card_parts_t *p = head_parts(card);
	if (!p) {
		return NULL;
	}
	if (!p->value) {
		p->value = lv_label_create(p->head);
		lv_obj_set_style_max_width(p->value, lv_pct(45), 0);
		lv_label_set_long_mode(p->value, LV_LABEL_LONG_DOT);
		lv_obj_set_style_text_font(p->value, &font_ui_22, 0);
		lv_obj_add_style(p->value, &sx_style_panel_text2, 0);
		// Before the chevron or the switch, if either is there already.
		lv_obj_move_to_index(p->value, 1);
	}
	sx_set_text(p->value, text ? text : "");
	return p->value;
}

void sx_card_nav_item(lv_obj_t *card) {
	lv_obj_t *parent = lv_obj_get_parent(card);
	if (!lv_obj_get_user_flag(parent, 3))
		return;
	lv_obj_set_style_radius(card, 0, 0);
	int index = lv_obj_get_index(card);
	if (index > 0) {
		lv_obj_t *previous = lv_obj_get_child(parent, index - 1);
		lv_obj_t *line = sx_box(previous);
		lv_obj_set_ignore_layout(line, true);
		lv_obj_set_size(line, lv_pct(100), 1);
		lv_obj_add_style(line, &sx_style_hairline, 0);
		lv_obj_align(line, LV_ALIGN_BOTTOM_MID, 0, SX_CARD_PAD_V);
	}
}

lv_obj_t *sx_card_chevron(lv_obj_t *card) {
	card_parts_t *p = head_parts(card);
	if (!p) {
		return NULL;
	}
	if (!p->chevron) {
		p->chevron = lv_image_create(p->head);
		lv_image_set_src(p->chevron, &icon_chevron_right);
		lv_obj_add_style(p->chevron, &sx_style_panel_icon, 0);
		lv_obj_set_style_image_opa(p->chevron, LV_OPA_60, 0);
		sx_card_nav_item(card);
	}
	return p->chevron;
}

// A tap, and only a tap, flips the switch (see settingsrow_switch).
static void switch_tap_cb(lv_event_t *e) {
	lv_obj_t *sw = lv_event_get_user_data(e);
	if (!sw || lv_obj_has_state(sw, LV_STATE_DISABLED)) {
		return;
	}
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	if (lv_obj_has_state(sw, LV_STATE_CHECKED)) {
		lv_obj_remove_state(sw, LV_STATE_CHECKED);
	} else {
		lv_obj_add_state(sw, LV_STATE_CHECKED);
	}
	lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, NULL);
}

lv_obj_t *sx_card_switch(lv_obj_t *card, lv_event_cb_t cb) {
	card_parts_t *p = head_parts(card);
	if (!p) {
		return NULL;
	}
	if (!p->sw) {
		if (p->subtitle) {
			lv_obj_set_parent(p->subtitle, card);
			lv_obj_move_to_index(p->subtitle, 1);
		}
		p->sw = lv_switch_create(p->head);
		sx_switch_style(p->sw);
		lv_obj_set_checkable(p->sw, false);
		lv_obj_set_ext_click_area(p->sw, 24);
		lv_obj_add_event_cb(p->sw, switch_tap_cb, LV_EVENT_CLICKED, p->sw);
		if (cb) {
			lv_obj_add_event_cb(p->sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
		}
	}
	return p->sw;
}

static lv_obj_t *wrap_row(lv_obj_t *card, int gap) {
	lv_obj_t *row = sx_box(card);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_gap(row, gap, 0);
	return row;
}

lv_obj_t *sx_card_pills(lv_obj_t *card) { return wrap_row(card, SX_PILL_GAP); }

lv_obj_t *sx_card_swatches(lv_obj_t *card) { return wrap_row(card, 12); }

lv_obj_t *sx_card_bar(lv_obj_t *card, int percent) {
	lv_obj_t *track = sx_box(card);
	lv_obj_set_size(track, lv_pct(100), SX_BAR_H);
	lv_obj_add_style(track, &sx_style_panel_press, 0);
	lv_obj_set_style_radius(track, SX_BAR_H / 2, 0);
	lv_obj_t *fill = sx_box(track);
	lv_obj_set_height(fill, SX_BAR_H);
	lv_obj_add_style(fill, &sx_style_panel_accent, 0);
	lv_obj_set_style_radius(fill, SX_BAR_H / 2, 0);
	sx_card_bar_set(track, percent);
	return track;
}

void sx_card_bar_set(lv_obj_t *bar, int percent) {
	lv_obj_t *fill = bar ? lv_obj_get_child(bar, 0) : NULL;
	if (!fill) {
		return;
	}
	percent = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
	lv_obj_set_width(fill, lv_pct(percent));
	lv_obj_set_hidden(fill, percent == 0);
}

lv_obj_t *sx_card_action(lv_obj_t *card, const char *text, lv_event_cb_t cb, void *user) {
	lv_obj_t *row = sx_box(card);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_t *btn = lv_btn_create(row);
	sx_pill_style(btn);
	lv_obj_set_size(btn, lv_pct(100), SX_SHEET_BTN_H);
	lv_obj_set_style_radius(btn, SX_CARD_R, 0);
	lv_obj_set_style_pad_hor(btn, 16, 0);
	lv_obj_set_ext_click_area(btn, 0);
	if (cb) {
		lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);
	}
	lv_obj_t *label = lv_label_create(btn);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_style_max_height(label, 2 * lv_font_get_line_height(&font_ui_22), 0);
	lv_label_set_text(label, text ? text : "");
	lv_obj_center(label);
	return btn;
}

void sx_card_set_active(lv_obj_t *card, bool active) {
	if (!card) {
		return;
	}
	lv_obj_remove_style(card, &sx_style_panel_active, 0);
	if (active) {
		lv_obj_add_style(card, &sx_style_panel_active, 0);
	}
}

void sx_card_set_dim(lv_obj_t *card, bool dim) {
	if (card) {
		lv_obj_set_style_opa(card, dim ? DIM_OPA : LV_OPA_COVER, 0);
	}
}

lv_obj_t *sx_pill(lv_obj_t *row, const char *text, intptr_t value, lv_event_cb_t cb) {
	lv_obj_t *btn = lv_btn_create(row);
	sx_pill_style(btn);
	lv_obj_set_width(btn, LV_SIZE_CONTENT);
	if (cb) {
		lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)value);
	}
	lv_obj_t *label = lv_label_create(btn);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_label_set_text(label, text ? text : "");
	lv_obj_center(label);
	sx_choice_reflow(row);
	return btn;
}

lv_obj_t *sx_swatch(lv_obj_t *row, lv_color_t colour, intptr_t value, lv_event_cb_t cb) {
	lv_obj_t *btn = lv_btn_create(row);
	lv_obj_set_size(btn, SX_SWATCH, SX_SWATCH);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_pad_all(btn, 0, 0);
	lv_obj_set_style_bg_color(btn, colour, 0);
	lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	if (cb) {
		lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)value);
	}
	return btn;
}

// The ring's colour is the text colour at the moment of the call: a page that
// keeps a swatch across a theme change marks it again from its refresh.
void sx_swatch_set_active(lv_obj_t *swatch, bool active) {
	if (!swatch) {
		return;
	}
	lv_obj_set_style_border_width(swatch, active ? 3 : 0, 0);
	lv_obj_set_style_border_color(swatch, theme()->text_primary, 0);
	lv_obj_set_style_border_opa(swatch, LV_OPA_COVER, 0);
}
