#ifndef PLAYCOUNT_H
#define PLAYCOUNT_H

#include <stdbool.h>

// ---------------------------------------------------------------------------
// How many times each track has been played, for the Most played list.
//
// A play counts when a track is at least 30 seconds and has been played for
// half its length or four minutes. Only time actually playing counts -- not a
// pause, not the device asleep -- and a track started again, by repeat-one or
// by hand, is a new play. It is written to the library (library_note_play),
// which keeps it only for tracks it has indexed.
// ---------------------------------------------------------------------------

typedef struct {
	bool playing;
	const char *file; // the loaded file, "" for none
	double position;  // seconds
	double duration;  // seconds, 0 when unknown
	bool skip;		  // a station, an audiobook or a podcast: never counted
} playcount_playback_t;

// GUI thread, about once a second, with what is playing now.
void playcount_note_playback(const playcount_playback_t *now);

#endif // PLAYCOUNT_H
