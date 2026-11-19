#ifndef STREAMING_H
#define STREAMING_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"
#include "src/gui/shell/gridpage.h"

// The Streaming section: the same tiled page as Music and Wireless, with a
// tile for each service the user has added -- Tidal, Qobuz, internet radio,
// podcasts, a Subsonic server, Yandex Music. The "+" in the corner adds one,
// a long press on a tile takes it off; the list is [streaming] tiles.
extern lv_obj_t *streaming_screen;

void streaming_init(gui_config_t *cfg);

bool streaming_home_entry(const char *id, grid_entry_t *entry);

void streaming_description(char *text, size_t size);

#endif /* STREAMING_H */
