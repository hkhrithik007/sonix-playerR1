#ifndef SEARCH_H
#define SEARCH_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The search page: a text field with the on-screen keyboard, the hits grouped
// under Tracks / Albums / Artists with filter pills, the recent searches while
// there is no query. Opened from the magnifier button on the Music page and
// the artists' lists.
extern lv_obj_t *search_screen;

void search_init(gui_config_t *cfg);
void search_open(void);
void search_open_query(const char *query);
// -1 all, 0 tracks, 1 albums, 2 artists; other values fall back to all.
void search_open_query_kind(const char *query, int kind);
int search_recent_queries(char out[][128], int max);
void search_note_query(const char *query);
void search_forget_query(const char *query);
void search_query_copy(char *out, size_t size, const char *text);

// The playing track changed: its row is marked (medialist_notify_now_playing
// passes it on). UI thread.
void search_notify_now_playing(void);

#endif // SEARCH_H
