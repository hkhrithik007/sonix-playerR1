#ifndef LISTENBRAINZSETTINGS_H
#define LISTENBRAINZSETTINGS_H

#include "lvgl/lvgl.h"

#include "src/gui/shell/topbar.h"
#include "src/system/core/config.h"

void listenbrainzsettings_init(gui_config_t *cfg);

lv_obj_t *listenbrainzsettings_screen(void);

#endif /* LISTENBRAINZSETTINGS_H */
