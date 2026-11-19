#ifndef PODCASTPAIR_H
#define PODCASTPAIR_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "src/system/core/sha256.h"

// Every call opens the same admitted folder, or refuses a stale/missing card.
// No descriptor returned by open_dir survives a network wait or hash chunk.
typedef struct {
	int (*open_dir)(void *user);
	bool (*cancelled)(void *user);
	void *user;
} podcastpair_io_t;

typedef enum {
	PODCASTPAIR_OK,
	PODCASTPAIR_FAILED,	  // no publication; the previous files were preserved/restored
	PODCASTPAIR_BUSY,	  // another live RSS job/recovery owns the RAM slot
	PODCASTPAIR_RECOVERY, // retain the record/backups; do not claim ready
	PODCASTPAIR_CONFLICT, // unexpected files/record; no guessed cleanup
} podcastpair_result_t;

typedef struct {
	dev_t dev;
	ino_t ino;
	long long size;
	char sha256[SHA256_HEX_LEN];
} podcastpair_audio_t;

// Nonblocking RAM-only reservation for a live RSS job; release before token free.
bool podcastpair_reserve(void *token);
void podcastpair_release(void *token);
bool podcastpair_busy(void);

// Worker only. Audio digest covers bytes already written to this exact inode.
// Staging/tags are unique owned regular files in the admitted folder.
podcastpair_result_t podcastpair_publish(const podcastpair_io_t *io, const char *audio_stage, const char *tags_stage, const char *final_audio, const podcastpair_audio_t *audio);

// Worker only; one fixed record per admitted RSS show folder. BUSY never waits/spins.
podcastpair_result_t podcastpair_recover(const podcastpair_io_t *io);

// Read-only logical readiness: no audio hash. Markerless/manual files pass.
// A surviving record blocks its affected file, including COMMITTED. Unknown
// records or any update temp conservatively block this folder only.
// Other existing files are distinguished by the filesystem's actual inode,
// including FAT case aliases.
// Worker verification/cleanup is required; no GUI audio hash/cache.
// Two metadata probes for worker preflight. Do not cache across a concurrent
// collection without a consistency fence. GUI collectors use ready_at with
// their existing directory FD, never hashing or opening a folder per item.
bool podcastpair_pending_fd(int directory_fd);
bool podcastpair_ready_at(int directory_fd, const char *basename);
bool podcastpair_ready(const char *audio_path);

#endif
