#ifndef RECOVERYWIZARD_H
#define RECOVERYWIZARD_H
#include "src/gui/shell/gui.h"
void recoverywizard_init(gui_config_t *cfg);
void recoverywizard_open(void *unused);
bool recoverywizard_busy(void);
#endif
