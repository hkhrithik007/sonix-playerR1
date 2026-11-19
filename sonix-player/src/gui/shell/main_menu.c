#include "main_menu.h"
#include "bookresume.h"
#include "icons.h"
#include "morepage.h"
#include "playbackplace.h"
#include "settingsrow.h"
#include "src/gui/library/audiobooks.h"
#include "src/gui/library/browser.h"
#include "src/gui/library/filespage.h"
#include "src/gui/library/medialist.h"
#include "src/gui/library/music.h"
#include "src/gui/library/search.h"
#include "src/gui/library/searchall.h"
#include "src/gui/library/searchsource.h"
#include "src/gui/library/shufflecard.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/settings/hometiles.h"
#include "src/gui/settings/settings.h"
#include "src/gui/streaming/streaming.h"
#include "src/gui/streaming/yandexpage.h"
#include "switcher.h"
#include "sx_sheet.h"
#include "sx_styles.h"
#include "toast.h"
#include "touch.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef SONIX_ZVUK
#include "src/gui/streaming/zvukpage.h"
#endif
#include "src/gui/wireless/wireless.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/net/wifi.h"
#include "src/system/streaming/yandex.h"
#ifdef SONIX_ZVUK
#include "src/system/streaming/zvuk.h"
#endif

lv_obj_t *main_menu_screen;
static gui_config_t *page_cfg;
static lv_obj_t *tile_grid;
static home_item_t order[HOME_CUSTOM_SLOTS];
static int order_n;
static bool loaded;
// UI-thread-only preference for this process; no configuration writes.
static int last_search_scope = -1;
static struct {
	home_item_t source;
	lv_obj_t *screen;
	lv_obj_t *field;
	lv_obj_t *title;
} search_inputs[SEARCH_SOURCE_MAX + 2];
static char shared_search_query[256];
static int search_input_count;
static const char *ids[] = {"music", "streaming", "networks", "audiobooks", "settings", "continue_book", "tidal", "qobuz", "radio", "podcasts", "subsonic", "yandex", "zvuk", "music_all", "music_albums", "music_artists", "music_album_artists", "music_genres", "music_years", "music_favourites", "music_most_played", "music_recent_added", "music_recent_played", "music_playlists", "music_browse", "search", "files", "yandex_wave", "yandex_liked", "spotify"};
static const char *tags[] = {"music", "streaming", "wireless", "audiobooks", "settings", "home_resume_book", "tidal", "qobuz", "radio", "podcasts", "subsonic", "yandex", "zvuk", "music_all_tracks", "albums", "artists", "music_album_artists", "music_genres", "music_year", "favourites", "music_most_played", "music_recently_added", "music_recently_played", "playlists", "music_browse", "home_search", "file_explorer", "yandex_my_wave", "yandex_liked", "spotify"};
static const lv_image_dsc_t *icons[] = {&icon_menu_music, &icon_menu_streaming, &icon_menu_wireless, &icon_menu_audiobooks, &icon_menu_settings, &icon_menu_audiobooks};

_Static_assert(sizeof(ids) / sizeof(ids[0]) == HOME_ITEM_COUNT, "Home IDs must match items");
_Static_assert(sizeof(tags) / sizeof(tags[0]) == HOME_ITEM_COUNT, "Home tags must match items");
_Static_assert(HOME_MUSIC_BROWSE - HOME_MUSIC_ALL + 1 == MUSIC_TILE_COUNT, "Music shortcuts must match tiles");

static bool valid(home_item_t item) { return item >= 0 && item < HOME_ITEM_COUNT; }
static bool contains(home_item_t item) {
	for (int i = 0; i < order_n; i++)
		if (order[i] == item)
			return true;
	return false;
}
static void defaults(void) {
	order_n = HOME_CUSTOM_SLOTS;
	for (int i = 0; i < order_n; i++)
		order[i] = (home_item_t)i;
}
static void load(void) {
	if (loaded)
		return;
	loaded = true;
	const char *saved = config_get_copy("home", "tiles", NULL, (char[CONFIG_VALUE_SIZE]){0}, CONFIG_VALUE_SIZE);
	if (!saved) {
		defaults();
		return;
	}
	order_n = 0;
	while (*saved && order_n < HOME_CUSTOM_SLOTS) {
		const char *end = strchr(saved, ',');
		size_t length = end ? (size_t)(end - saved) : strlen(saved);
		for (int i = 0; i < HOME_ITEM_COUNT; i++) {
			if (strlen(ids[i]) == length && !strncmp(saved, ids[i], length) && !contains((home_item_t)i) && main_menu_available((home_item_t)i))
				order[order_n++] = (home_item_t)i;
		}
		if (!end)
			break;
		saved = end + 1;
	}
}
int main_menu_count(void) {
	load();
	return order_n;
}
home_item_t main_menu_at(int pos) {
	load();
	return pos >= 0 && pos < order_n ? order[pos] : HOME_ITEM_COUNT;
}
bool main_menu_contains(home_item_t item) {
	load();
	return contains(item);
}
bool main_menu_available(home_item_t item) {
	if (item == HOME_YANDEX_WAVE || item == HOME_YANDEX_LIKED)
		return streaming_home_entry("yandex", NULL);
	return valid(item) && (item <= HOME_CONTINUE_BOOK || item >= HOME_MUSIC_ALL || streaming_home_entry(ids[item], NULL));
}
const char *main_menu_tag(home_item_t item) { return valid(item) ? tags[item] : "more"; }
const lv_image_dsc_t *main_menu_icon(home_item_t item) {
	if (item == HOME_YANDEX_WAVE)
		return &icon_menu_tidal;
	if (item == HOME_YANDEX_LIKED)
		return &icon_menu_favourites;
	grid_entry_t entry;
	if (item >= HOME_MUSIC_ALL && item <= HOME_MUSIC_BROWSE && music_home_entry((music_tile_t)(item - HOME_MUSIC_ALL), &entry))
		return entry.icon;
	if (item == HOME_SEARCH)
		return &icon_search;
	if (item == HOME_FILES)
		return &icon_menu_file_explorer;
	if (((item > HOME_CONTINUE_BOOK && item <= HOME_ZVUK) || item == HOME_SPOTIFY) && streaming_home_entry(ids[item], &entry))
		return entry.icon;
	return item >= 0 && item <= HOME_CONTINUE_BOOK ? icons[item] : &icon_menu_more;
}

static void music_description(char *text, size_t size) {
	if (!gui_card_available())
		snprintf(text, size, "%s", tr("home_card_unavailable"));
	else {
		shufflecard_tracks_description(text, size);
		char place[256] = "";
		if (!switcher_browse_description("music", place, sizeof(place)))
			playbackplace_description("music", place, sizeof(place));
		// This names the tile's destination, not the currently playing source.
		if (place[0])
			snprintf(text, size, tr("home_return_to_place"), place);
	}
}

static void open_music_place(void) {
	if (switcher_browse_open("music"))
		return;
	// Keep the ordinary Music section one visible Back away from its source.
	switch_screen(music_screen);
	if (!browser_open_playback_place())
		medialist_open_playback_place();
}
static void books_description(char *text, size_t size) {
	if (!gui_card_available())
		snprintf(text, size, "%s", tr("home_card_unavailable"));
	else if (audiobookdb_count() > 0)
		snprintf(text, size, tr("home_books_n"), audiobookdb_count());
}
static int drawn_network_state = -1;
static unsigned drawn_place_revision;
static int network_state(void) {
	wifi_status_t wifi;
	wifi_get_status(&wifi);
	return wifi.state == WIFI_STATE_CONNECTED ? 1 : bluetooth_connected_device(NULL) ? 2 : 0;
}
static void network_description(char *text, size_t size) {
	static const char *const tags[] = {"home_no_connections", "home_wifi_connected", "home_bt_connected"};
	drawn_network_state = network_state();
	snprintf(text, size, "%s", tr(tags[drawn_network_state]));
}
// Called by the existing standby-paused radio poll; no work for hidden grids.
void main_menu_poll_network(void) {
	lv_obj_t *screen = lv_screen_active();
	if (!screen || (screen != main_menu_screen && screen != morepage_screen))
		return;
	int current = network_state();
	unsigned place_revision = playbackplace_revision();
	if (current == drawn_network_state && place_revision == drawn_place_revision)
		return;
	// Update even if this particular grid does not contain the network tile.
	drawn_network_state = current;
	drawn_place_revision = place_revision;
	lv_obj_send_event(screen, LV_EVENT_REFRESH, NULL);
}
static void yandex_description(char *text, size_t size) { snprintf(text, size, "%s", tr("yandex")); }
bool main_menu_get_entry(home_item_t item, grid_entry_t *out) {
	if (!valid(item) || !out)
		return false;
	if (item >= HOME_MUSIC_ALL && item <= HOME_MUSIC_BROWSE)
		return music_home_entry((music_tile_t)(item - HOME_MUSIC_ALL), out);
	if (item == HOME_SEARCH) {
		*out = (grid_entry_t){"home_search", &icon_search, NULL, main_menu_open_search};
		return true;
	}
	if (item == HOME_FILES) {
		*out = (grid_entry_t){"file_explorer", &icon_menu_file_explorer, NULL, filespage_open};
		return true;
	}
	if (item == HOME_YANDEX_WAVE || item == HOME_YANDEX_LIKED) {
		*out = (grid_entry_t){tags[item], main_menu_icon(item), NULL, item == HOME_YANDEX_WAVE ? yandexpage_wave_open : yandexpage_liked_open, yandex_description};
		return true;
	}
	if (item > HOME_CONTINUE_BOOK)
		return streaming_home_entry(ids[item], out);
	*out = (grid_entry_t){tags[item], icons[item], NULL, NULL, NULL};
	switch (item) {
	case HOME_MUSIC:
		out->action = open_music_place;
		out->subtitle = music_description;
		break;
	case HOME_STREAMING:
		out->target = &streaming_screen;
		out->subtitle = streaming_description;
		break;
	case HOME_NETWORKS:
		out->target = &wireless_screen;
		out->subtitle = network_description;
		break;
	case HOME_AUDIOBOOKS:
		out->target = &audiobooks_screen;
		out->subtitle = books_description;
		break;
	case HOME_SETTINGS:
		out->target = &settings_screen;
		break;
	case HOME_CONTINUE_BOOK:
		out->action = bookresume_play;
		out->subtitle = bookresume_description;
		break;
	default:
		return false;
	}
	return true;
}
static void rebuild(void) {
	load();
	if (tile_grid) {
		grid_entry_t entries[6] = {0};
		for (int i = 0; i < order_n; i++)
			main_menu_get_entry(order[i], &entries[i]);
		entries[5] = (grid_entry_t){"more", &icon_menu_more, &morepage_screen};
		gridpage_set_entries(tile_grid, page_cfg, entries, 6, 2, 3, false);
		for (int i = 0; i < order_n; i++)
			lv_obj_add_event_cb(lv_obj_get_child(tile_grid, i), main_menu_hold, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)order[i]);
		lv_obj_add_event_cb(lv_obj_get_child(tile_grid, 5), main_menu_hold, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)HOME_ITEM_COUNT);
	}
	morepage_rebuild();
}
static void save(void) {
	char value[128] = "";
	size_t used = 0;
	for (int i = 0; i < order_n; i++) {
		int n = snprintf(value + used, sizeof(value) - used, "%s%s", i ? "," : "", ids[order[i]]);
		if (n < 0 || (size_t)n >= sizeof(value) - used)
			return;
		used += (size_t)n;
	}
	config_set("home", "tiles", value);
	config_save();
	rebuild();
}
bool main_menu_move(home_item_t item, int pos) {
	load();
	if (!main_menu_available(item))
		return false;
	int old = -1;
	for (int i = 0; i < order_n; i++)
		if (order[i] == item)
			old = i;
	if (old < 0 && order_n == HOME_CUSTOM_SLOTS)
		return false;
	if (old >= 0) {
		memmove(order + old, order + old + 1, (size_t)(order_n - old - 1) * sizeof(*order));
		order_n--;
	}
	if (pos < 0)
		pos = 0;
	if (pos > order_n)
		pos = order_n;
	memmove(order + pos + 1, order + pos, (size_t)(order_n - pos) * sizeof(*order));
	order[pos] = item;
	order_n++;
	save();
	return true;
}
void main_menu_hide(home_item_t item) {
	load();
	for (int i = 0; i < order_n; i++)
		if (order[i] == item) {
			memmove(order + i, order + i + 1, (size_t)(order_n - i - 1) * sizeof(*order));
			order_n--;
			save();
			return;
		}
}
void main_menu_reset(void) {
	loaded = true;
	defaults();
	save();
}
void main_menu_reload(void) {
	loaded = false;
	rebuild();
}
void main_menu_refresh(void) {
	if (tile_grid)
		lv_obj_send_event(main_menu_screen, LV_EVENT_REFRESH, NULL);
	if (morepage_screen)
		lv_obj_send_event(morepage_screen, LV_EVENT_REFRESH, NULL);
}
void main_menu_open_settings(void) {
	gui_init_rest();
	hometiles_open(page_cfg);
}
static void settings_action(void *unused) {
	(void)unused;
	main_menu_open_settings();
}
static void move_action(void *user) {
	home_item_t item = (home_item_t)(intptr_t)user;
	if (main_menu_contains(item))
		main_menu_hide(item);
	else if (!main_menu_move(item, main_menu_count()))
		toast_error("home_full");
}
static void yandex_wave_action(void *unused) {
	(void)unused;
	yandexpage_wave_open();
}
static void yandex_liked_action(void *unused) {
	(void)unused;
	yandexpage_liked_open();
}
void main_menu_hold(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active() || coverflow_drag_active())
		return;
	lv_indev_t *input = lv_event_get_param(e);
	if (!input)
		input = lv_indev_active();
	if (input)
		lv_indev_wait_release(input);
	home_item_t item = (home_item_t)(intptr_t)lv_event_get_user_data(e);
	if (item == HOME_CONTINUE_BOOK) {
		bookresume_menu();
		return;
	}
	sx_sheet_item_t items[4];
	int n = 0;
	if (item == HOME_YANDEX) {
		items[n++] = (sx_sheet_item_t){.label = tr("yandex_my_wave"), .icon = &icon_menu_tidal, .action = yandex_wave_action};
		items[n++] = (sx_sheet_item_t){.label = tr("yandex_liked"), .icon = &icon_menu_favourites, .action = yandex_liked_action};
	}
	if (valid(item))
		items[n++] = (sx_sheet_item_t){.label = tr(main_menu_contains(item) ? (item <= HOME_CONTINUE_BOOK ? "home_to_more" : "hometiles_remove") : "home_to_main"), .icon = &icon_menu_more, .action = move_action, .user = (void *)(intptr_t)item};
	items[n++] = (sx_sheet_item_t){.label = tr("home_settings"), .icon = &icon_menu_settings, .action = settings_action};
	sx_sheet_menu(tr(main_menu_tag(item)), NULL, items, n);
}
void main_menu_init(gui_config_t *cfg) {
	page_cfg = cfg;
	tile_grid = gridpage_build(main_menu_screen, cfg, NULL, 0, 2, 3, false);
	rebuild();
	shufflecard_attach(main_menu_screen, cfg);
}

static void search_scope_register(lv_obj_t *screen, home_item_t source) {
	for (int i = 0; i < search_input_count; i++) {
		if (search_inputs[i].source == source) {
			search_inputs[i].screen = screen;
			return;
		}
	}
	if (search_input_count < (int)(sizeof(search_inputs) / sizeof(search_inputs[0]))) {
		search_inputs[search_input_count].source = source;
		search_inputs[search_input_count++].screen = screen;
	}
}

static lv_obj_t *search_scope_screen(home_item_t source) {
	for (int i = 0; i < search_input_count; i++)
		if (search_inputs[i].source == source)
			return search_inputs[i].screen;
	return NULL;
}

static bool search_scope_is_input(lv_obj_t *screen) {
	for (int i = 0; i < search_input_count; i++)
		if (search_inputs[i].screen == screen)
			return true;
	return false;
}

static void search_scope_action(void *user) {
	home_item_t source = (home_item_t)(intptr_t)user;
	const search_source_t *delegate = search_source_home(source);
	if (source != HOME_SEARCH && source != HOME_MUSIC && (!delegate || !delegate->ready()))
		return;
	bool replace = search_scope_is_input(lv_screen_active());
	last_search_scope = source;
	if (source == HOME_MUSIC) {
		lv_obj_t *target = search_scope_screen(source);
		if (target && replace)
			switch_screen_no_history(target);
		else
			search_open();
	} else
		searchall_open_source(page_cfg, source, replace);
}
static void search_scope_choose(void) {
	gui_init_rest();
	sx_sheet_item_t items[SEARCH_SOURCE_MAX + 2] = {{.label = tr("search_everywhere"), .icon = &icon_search, .action = search_scope_action, .user = (void *)(intptr_t)HOME_SEARCH}, {.label = tr("home_search_card"), .icon = &icon_menu_music, .action = search_scope_action, .user = (void *)(intptr_t)HOME_MUSIC}};
	int n = 2;
	for (int i = 0; i < search_source_count(); i++) {
		const search_source_t *source = search_source_at(i);
		if (source->ready())
			items[n++] = (sx_sheet_item_t){.label = tr(source->name), .icon = main_menu_icon(source->home), .action = search_scope_action, .user = (void *)(intptr_t)source->home};
	}
	sx_sheet_menu(tr("home_search"), NULL, items, n);
}

void main_menu_open_search(void) {
	gui_init_rest();
	if (last_search_scope == HOME_SEARCH || last_search_scope == HOME_MUSIC || (search_source_home((home_item_t)last_search_scope) && search_source_home((home_item_t)last_search_scope)->ready())) {
		search_scope_action((void *)(intptr_t)last_search_scope);
		return;
	}
	search_scope_action((void *)(intptr_t)HOME_SEARCH);
}

static const char *search_scope_tag(home_item_t source) {
	const search_source_t *delegate = search_source_home(source);
	return source == HOME_SEARCH ? "search_everywhere" : delegate ? delegate->name : "home_search_card";
}
void main_menu_search_scope_set(lv_obj_t *screen, home_item_t source) {
	for (int i = 0; i < search_input_count; i++)
		if (search_inputs[i].screen == screen) {
			search_inputs[i].source = source;
			lv_obj_t *title = search_inputs[i].title;
			if (title) {
				lv_obj_set_user_data(title, (void *)(intptr_t)source);
				lv_label_set_text(title, tr(search_scope_tag(source)));
				lv_obj_send_event(title, LV_EVENT_SIZE_CHANGED, NULL);
			}
			last_search_scope = source;
		}
}

static void search_scope_header_click(lv_event_t *e) {
	(void)e;
	if (!switcher_back_drag_active())
		search_scope_choose();
}

static void search_scope_chevron_place(lv_event_t *e) {
	lv_obj_t *title = lv_event_get_target(e);
	lv_obj_t *arrow = lv_event_get_user_data(e);
	lv_point_t text_size;
	lv_text_get_size(&text_size, lv_label_get_text(title), lv_obj_get_style_text_font(title, 0), lv_obj_get_style_text_letter_space(title, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	int max_x = lv_obj_get_width(title) - (int)icon_chevron_down.header.w;
	int x = text_size.x + 8;
	if (x > max_x)
		x = max_x;
	lv_obj_set_pos(arrow, x > 0 ? x : 0, (lv_obj_get_height(title) - (int)icon_chevron_down.header.h) / 2);
}

static void search_scope_loaded(lv_event_t *e) {
	lv_obj_t *title = lv_event_get_user_data(e);
	home_item_t source = (home_item_t)(intptr_t)lv_obj_get_user_data(title);
	last_search_scope = source;
	for (int i = 0; i < search_input_count; i++) {
		if (search_inputs[i].source == source && search_inputs[i].field && strcmp(lv_textarea_get_text(search_inputs[i].field), shared_search_query))
			lv_textarea_set_text(search_inputs[i].field, shared_search_query);
	}
	lv_label_set_text(title, tr(search_scope_tag(source)));
	lv_obj_send_event(title, LV_EVENT_SIZE_CHANGED, NULL);
}

void main_menu_search_header(lv_obj_t *screen, gui_config_t *cfg, home_item_t source) {
	search_scope_register(screen, source);
	lv_obj_t *title = settingsrow_title(screen, cfg, search_scope_tag(source));
	for (int i = 0; i < search_input_count; i++)
		if (search_inputs[i].screen == screen)
			search_inputs[i].title = title;
	lv_obj_set_user_data(title, (void *)(intptr_t)source);
	lv_obj_set_style_pad_right(title, 36, 0);
	lv_obj_set_clickable(title, true);
	// The full heading is a normal fingertip-sized control, not a hold action.
	touch_pad(title, 0, 0, 20, 20);
	lv_obj_add_event_cb(title, search_scope_header_click, LV_EVENT_SHORT_CLICKED, NULL);
	lv_obj_t *arrow = lv_image_create(title);
	lv_image_set_src(arrow, &icon_chevron_down);
	lv_obj_add_style(arrow, &sx_style_icon2, 0);
	lv_obj_add_event_cb(title, search_scope_chevron_place, LV_EVENT_SIZE_CHANGED, arrow);
	lv_obj_add_event_cb(title, search_scope_chevron_place, LV_EVENT_STYLE_CHANGED, arrow);
	lv_obj_add_event_cb(screen, search_scope_loaded, LV_EVENT_SCREEN_LOADED, title);
}

static void shared_query_changed(lv_event_t *e) {
	lv_obj_t *field = lv_event_get_target(e);
	const char *text = lv_textarea_get_text(field);
	size_t n = strlen(text);
	if (n >= sizeof(shared_search_query)) {
		n = sizeof(shared_search_query) - 1;
		while (n && ((unsigned char)text[n] & 0xc0) == 0x80)
			n--;
	}
	memcpy(shared_search_query, text, n);
	shared_search_query[n] = 0;
}

void main_menu_search_bind(lv_obj_t *screen, lv_obj_t *field) {
	for (int i = 0; i < search_input_count; i++) {
		if (search_inputs[i].screen == screen) {
			search_inputs[i].field = field;
			lv_obj_add_event_cb(field, shared_query_changed, LV_EVENT_VALUE_CHANGED, NULL);
			return;
		}
	}
}

const char *main_menu_search_query(void) { return shared_search_query; }
