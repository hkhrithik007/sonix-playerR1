#ifndef HISTORYPAGE_H
#define HISTORYPAGE_H
#include "src/gui/shell/gui.h"
void historypage_init(gui_config_t *cfg);
void historypage_open(void *unused);
// Existing player poll only; no work while the history page is hidden.
void historypage_poll(void);
#endif
