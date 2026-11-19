#ifndef BOOKDOWNLOAD_H
#define BOOKDOWNLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define BOOKDOWNLOAD_MAX_PARTS 300
#define BOOKDOWNLOAD_PATH_MAX 512

// open_root returns a new FD for the originally admitted card or refuses it.
// The callback must reject changed card epochs, including equal dev/inode reuse.
typedef struct {
	int (*open_root)(void *user);
	bool (*cancelled)(void *user);
	void *user;
} bookdownload_io_t;

// Flat private staging, published in one rename. No network here. The admitted
// API closes its own descriptors after every metadata operation. A caller may
// borrow a stage FD for an active chapter, but must close it before WAIT.
typedef struct {
	char folder[512];
	char final[512];
	char files[BOOKDOWNLOAD_MAX_PARTS][80];
	int count;
	bool owned;
	bool conflicted; // a rejected current-file replacement must not be aborted later
	bookdownload_io_t io;
	char relative_parent[512];
	char stage_name[96], final_name[96];
	dev_t root_dev, parent_dev, stage_dev;
	ino_t root_ino, parent_ino, stage_ino;
	long long audio_sizes[BOOKDOWNLOAD_MAX_PARTS], tag_sizes[BOOKDOWNLOAD_MAX_PARTS];
	dev_t audio_dev[BOOKDOWNLOAD_MAX_PARTS], tags_dev[BOOKDOWNLOAD_MAX_PARTS];
	ino_t audio_ino[BOOKDOWNLOAD_MAX_PARTS], tags_ino[BOOKDOWNLOAD_MAX_PARTS];
} bookdownload_t;

bool bookdownload_path(const char *root, const char *folder, const char *service, const char *id, char *out, size_t size);
// True only for a complete publication whose audio and metadata files exist.
// Returns the scanner's book key: a file for one part, otherwise its folder.
bool bookdownload_ready(const char *directory, char *book_out, size_t size);
// Worker snapshot of a captured card/folder; canonical output only after readmission.
bool bookdownload_ready_admitted(const char *root, const char *directory, const bookdownload_io_t *io, char *book_out, size_t size);
// Compatibility entry: caller owns a stable mount pin for its whole lifetime.
bool bookdownload_begin(bookdownload_t *book, const char *directory);
// Production service workers use the epoch-admitted entry, not the legacy one.
bool bookdownload_begin_admitted(bookdownload_t *book, const char *root, const char *directory, const bookdownload_io_t *io);
int bookdownload_open_stage(bookdownload_t *book);
// Revalidate closed confirmed chapters after WAIT; no network or hashes.
bool bookdownload_validate(bookdownload_t *book);
// Remove only the current not-yet-recorded chapter and its private partials.
bool bookdownload_discard(bookdownload_t *book, const char *filename);
bool bookdownload_record(bookdownload_t *book, const char *filename);
bool bookdownload_publish(bookdownload_t *book, int expected);
void bookdownload_abort(bookdownload_t *book);

#endif
