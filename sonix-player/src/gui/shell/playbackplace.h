#ifndef PLAYBACKPLACE_H
#define PLAYBACKPLACE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// GUI thread. A route holds identifiers and the last confirmed track, never
// credentials, responses, artwork or a copy of the queue. Versioned on disk.
typedef struct {
	uint32_t version;
	int32_t kind, filter, order, desc, index;
	char title[256], a[512], b[256], path[512];
} playbackplace_t;

void playbackplace_arm(const char *source, const playbackplace_t *place, const char *expected);
void playbackplace_heard(const char *path);
bool playbackplace_get(const char *source, playbackplace_t *out);
void playbackplace_description(const char *source, char *out, size_t size);
unsigned playbackplace_revision(void);
#endif
