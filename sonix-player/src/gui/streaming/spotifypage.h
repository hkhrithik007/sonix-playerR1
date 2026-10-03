#ifndef SPOTIFY_PAGE_H
#define SPOTIFY_PAGE_H

#include "src/gui/shell/gui.h"
#include "lvgl/lvgl.h"

extern lv_obj_t *spotify_screen;

void spotify_page_init(gui_config_t *cfg);

#endif /* SPOTIFY_PAGE_H */
