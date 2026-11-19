#ifndef PODCASTDL_H
#define PODCASTDL_H

#include <stdbool.h>
#include <stddef.h>

#include "src/system/streaming/podcast.h"
#include "src/system/streaming/podcastpair.h"

// Podcast episodes kept on the card for good, as ordinary files:
//
//   <card>/Podcast/<podcast title>/<episode title>.<ext>
//
// with the podcast's artwork beside them as cover.jpg (or cover.png), each
// file's modification time set to the episode's publication date, and a
// "<file>.tags" sidecar in podcastcache.h's format, so the player treats the
// file as the episode it is. Unlike podcastcache.h nothing here is ever pruned
// or emptied.
//
// One download at a time, on a thread of its own. The file is written under a
// hidden ".<name>-<pid>-<serial>.part" and renamed when complete, so a cut download never
// shows up as an episode.

#define PODCASTDL_FOLDER "Podcast"

typedef enum {
	PODCASTDL_IDLE,
	PODCASTDL_RUNNING,
	PODCASTDL_DONE,
	PODCASTDL_FAILED,
	PODCASTDL_CANCELLED,
	PODCASTDL_RECOVERY_REQUIRED,
} podcastdl_state_t;

// Copy <card>/Podcast into caller-owned storage. False and an empty output on
// no card or insufficient space (when out/size allow it). No storage pin or
// card epoch is acquired: callers still need admission for file mutations.
bool podcastdl_root(char *out, size_t size);

// Where `episode` of the podcast titled `feed_title` is saved. False when there
// is no card or output space is insufficient; output is cleared on failure.
// Says nothing about whether the file exists or storage admission.
bool podcastdl_path(const podcast_episode_t *episode, const char *feed_title, char *out, size_t size);

// Whether that file is already on the card.
bool podcastdl_exists(const podcast_episode_t *episode, const char *feed_title);

// Starts downloading `episode` into the folder of `feed_title`. `feed_image` is
// the podcast's artwork, fetched once per folder; `feed_author` goes into the
// sidecar for the player's follow star. Either NULL or empty for none. False,
// starting nothing, when a download is already running, there is no admitted
// card, the path is invalid or the worker cannot start. Directory/network/write
// failures after start are reported by podcastdl_state().
bool podcastdl_start(const podcast_episode_t *episode, const char *feed_title, const char *feed_author,
					 const char *feed_image);

// True when `path` is inside the Podcast folder.
bool podcastdl_owns(const char *path);

// Stops the running download and removes its own staging file when the same
// card is still admitted. A staging file left on a detached card is never
// cleaned through the replacement card pathname. Returns at once; the
// state becomes PODCASTDL_CANCELLED when the thread has wound up.
void podcastdl_cancel(void);

// The state, with the bytes written and the size the server announced (0 when
// it did not say). Either pointer may be NULL.
podcastdl_state_t podcastdl_state(long long *done, long long *total);

// Back to PODCASTDL_IDLE after a finished, failed or cancelled download has
// been reported.
void podcastdl_acknowledge(void);

// True while a download holds the network.
bool podcastdl_busy(void);

// Turns a title into a file or folder name the card accepts: the characters
// FAT forbids go, leading and trailing dots and spaces go, the length is capped
// on a UTF-8 boundary. `fallback` when nothing is left.
void podcastdl_safe_name(const char *title, const char *fallback, char *out, size_t size);

// Local worker only, with the GUI request's captured card epoch. No network.
podcastpair_result_t podcastdl_recover_card(unsigned epoch, bool (*stop)(void *), void *user);
bool podcastdl_recovery_busy(void);
// The last terminal download's bounded reason, for recovery/conflict feedback.
podcastpair_result_t podcastdl_pair_result(void);

#endif /* PODCASTDL_H */
