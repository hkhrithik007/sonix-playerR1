#ifndef PLAYCOUNT_H
#define PLAYCOUNT_H

#include <stdbool.h>

// ---------------------------------------------------------------------------
// Following what is played: the one place that decides when a play starts and
// when it has been listened to long enough to count.
//
// Once a second a timer looks at the player. A play counts by ListenBrainz's rule
// (playcount_play_counts): a track of at least 30 seconds, played for half its
// length or for four minutes. Only time actually playing counts -- not a pause,
// not the device asleep -- and a track started again, by repeat-one or by hand,
// is a new play. Stations, audiobooks and podcasts are never counted.
//
// What it works out goes to the player's own play counts (library_note_play,
// for the Most played list, kept only for tracks the library has indexed) and,
// when enabled, to ListenBrainz (listenbrainz_note_play), which decides what
// to send and when.
// ---------------------------------------------------------------------------

// The play under way, as handed to the scrobblers. Strings are "" while not
// known; the tags may come in a few seconds after the file starts.
typedef struct {
	unsigned generation;  // moves at the start of each play, and when one ends
	bool playing;		  // playing now, rather than paused or stopped
	bool skip;			  // a station, an audiobook or a podcast: never counted
	bool counts;		  // listened to long enough (playcount_play_counts)
	double boot_start;	  // CLOCK_BOOTTIME seconds at the start of the play
	long long real_start; // UTC seconds at the start, 0 if the clock was not set
	int duration;		  // seconds, 0 while unknown
	const char *artist;
	const char *title;
	const char *album;
} playcount_play_t;

// Starts the once-a-second timer. GUI thread, after lv_init() and the audio.
void playcount_init(void);

// ListenBrainz's rule for a play that counts: a track of at least 30 seconds,
// listened to for half its length or for four minutes, whichever comes first.
// `listened` is time actually played, in seconds.
bool playcount_play_counts(double length, double listened);

#endif // PLAYCOUNT_H
