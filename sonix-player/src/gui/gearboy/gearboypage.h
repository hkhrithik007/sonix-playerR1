#ifndef GUI_GEARBOYPAGE_H
#define GUI_GEARBOYPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The game list: whatever is in Games/GB and Games/GBC on the card, titled
// from the database (see src/system/gbdb.h) rather than from the filename.

extern lv_obj_t *gearboypage_screen;

void gearboypage_init(gui_config_t *cfg);

// GUI thread: drain at most one owned terminal result on an existing main-loop
// pass. Correctness does not depend on GUI message admission or a timer.
void gearboypage_service_completion(void);

// GUI thread, before process exit: refuse admission and drop ready payloads.
// Does not join a disk worker or permit live-process LVGL teardown by itself.
void gearboypage_shutdown_completions(void);

#endif /* GUI_GEARBOYPAGE_H */
