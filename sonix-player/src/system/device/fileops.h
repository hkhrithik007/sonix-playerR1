#ifndef SONIX_FILEOPS_H
#define SONIX_FILEOPS_H

#include "src/system/device/system.h"
#include <stdbool.h>
#include <stdint.h>

#define FILEOP_ROOT_MAX 640
#define FILEOP_PATH_MAX 896
typedef enum { FILEOP_COPY, FILEOP_MOVE, FILEOP_DELETE, FILEOP_RENAME, FILEOP_MKDIR } fileop_kind_t;
typedef struct {
	fileop_kind_t kind;
	storage_card_token_t card;
	char root[FILEOP_ROOT_MAX];
	char source[FILEOP_PATH_MAX], target[FILEOP_PATH_MAX];
} fileop_request_t;
typedef void (*fileop_progress_fn)(uint64_t done, uint64_t total, void *user);
typedef bool (*fileop_cancel_fn)(void *user);

// Worker-only. The request belongs to the card admitted by the originating UI;
// never replace its token with a newer one. All traversal/mutations use FDs
// relative to that exact pinned mount. Every exit releases those descriptors.
// Returns an errno value (zero on success). EAGAIN means ownership was busy,
// not proof of a stale card; stop this operation and let an explicit retry decide.
int fileop_run(const fileop_request_t *request, fileop_progress_fn progress, fileop_cancel_fn cancelled, void *user);

#endif
