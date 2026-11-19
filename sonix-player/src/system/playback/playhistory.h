#ifndef PLAYHISTORY_H
#define PLAYHISTORY_H

#include "src/system/library/library.h"
#include <stdbool.h>

// The history of what the queue has played, as the queue page shows it.
//
// A real first PCM block confirms a listening event, independently of play
// counts. Seek/pause/resume keep the event; leaving and starting again creates
// another. Deferred writes are bounded and tied to the original database epoch.
// Safe from workers too. One lazy writer sleeps when idle; no periodic timer.

// A track has been loaded. `title` and `artist` are what it is to be shown by;
// `kept_out` says it is not to go into the history. With `waiting`, it only
// counts once it starts playing (playhistory_resumed): the track a reboot put
// back, paused, was not played again just by coming back.
void playhistory_started(const char *path, const char *title, const char *artist, bool kept_out, bool waiting);

// The track loaded has started playing after a pause.
void playhistory_resumed(void);
// Confirmed first PCM, from the existing player poll; no new poll or timer.
void playhistory_heard(const char *path);
long long playhistory_current_id(void);
unsigned playhistory_revision(void);
void playhistory_poll(void);
// Wait for accepted saves on explicit history reads and normal storage/power
// exits. Playback callbacks use poll/heard and never wait for this barrier.
void playhistory_flush(void);
// Main-thread process exit only; flushes and joins the lazy writer.
void playhistory_shutdown(void);
bool playhistory_unsaved(void);
// The bounded persistent history plus not-yet-saved events, oldest first.
int playhistory_items(library_history_item_t **out);
bool playhistory_entry(long long id, char *path, size_t path_size, char *name, size_t name_size, char *artist, size_t artist_size);
// Lazy service metadata updates the pending entry without starting a new listen.
void playhistory_metadata(const char *path, const char *title, const char *artist);

// Leaves the current event; also confirms a short real listen between polls.
void playhistory_left(void);

#endif // PLAYHISTORY_H
