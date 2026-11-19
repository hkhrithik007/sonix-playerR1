#ifndef MAIN_MENU_H
#define MAIN_MENU_H

#include "gridpage.h"
#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

extern lv_obj_t *main_menu_screen;

void main_menu_init(gui_config_t *cfg);

// Stable IDs are stored as names, never as translated text or enum numbers.
typedef enum { HOME_MUSIC, HOME_STREAMING, HOME_NETWORKS, HOME_AUDIOBOOKS, HOME_SETTINGS, HOME_CONTINUE_BOOK, HOME_TIDAL, HOME_QOBUZ, HOME_RADIO, HOME_PODCASTS, HOME_SUBSONIC, HOME_YANDEX, HOME_ZVUK, HOME_MUSIC_ALL, HOME_MUSIC_ALBUMS, HOME_MUSIC_ARTISTS, HOME_MUSIC_ALBUM_ARTISTS, HOME_MUSIC_GENRES, HOME_MUSIC_YEARS, HOME_MUSIC_FAVOURITES, HOME_MUSIC_MOST_PLAYED, HOME_MUSIC_RECENT_ADDED, HOME_MUSIC_RECENT_PLAYED, HOME_MUSIC_PLAYLISTS, HOME_MUSIC_BROWSE, HOME_SEARCH, HOME_FILES, HOME_YANDEX_WAVE, HOME_YANDEX_LIKED, HOME_SPOTIFY, HOME_ITEM_COUNT } home_item_t;
#define HOME_CUSTOM_SLOTS 5
int main_menu_count(void);
home_item_t main_menu_at(int position);
bool main_menu_contains(home_item_t item);
bool main_menu_available(home_item_t item);
const char *main_menu_tag(home_item_t item);
const lv_image_dsc_t *main_menu_icon(home_item_t item);
bool main_menu_move(home_item_t item, int position);
void main_menu_hide(home_item_t item);
void main_menu_reset(void);
void main_menu_reload(void);
void main_menu_refresh(void);
// Visible Home/More network subtitle, from the existing GUI radio poll.
void main_menu_poll_network(void);
void main_menu_open_settings(void);
void main_menu_open_search(void);
// Shared query/source controls. UI thread only.
void main_menu_search_header(lv_obj_t *screen, gui_config_t *cfg, home_item_t source);
void main_menu_search_scope_set(lv_obj_t *screen, home_item_t source);
void main_menu_search_bind(lv_obj_t *screen, lv_obj_t *field);
const char *main_menu_search_query(void);
bool main_menu_get_entry(home_item_t item, grid_entry_t *out);
void main_menu_hold(lv_event_t *e);

#endif
