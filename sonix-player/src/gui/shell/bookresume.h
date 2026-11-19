#ifndef BOOKRESUME_H
#define BOOKRESUME_H
#include <stddef.h>
#include "src/system/playback/device_state.h"
// UI thread only. Only confirmed playback selects a book, not browsing/import.
void bookresume_note(const device_state_t *state);
void bookresume_description(char *text, size_t size);
void bookresume_play(void);
void bookresume_menu(void);
#endif
