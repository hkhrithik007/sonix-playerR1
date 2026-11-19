#ifndef SEARCHALL_H
#define SEARCHALL_H
#include "src/gui/shell/gui.h"
#include "src/gui/shell/main_menu.h"
void searchall_open(gui_config_t *cfg, bool replace_source);
void searchall_poll(void);
void searchall_open_source(gui_config_t *cfg, home_item_t scope, bool replace_source);
#endif
