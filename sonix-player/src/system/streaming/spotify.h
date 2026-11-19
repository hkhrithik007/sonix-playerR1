#ifndef SONIX_SPOTIFY_H
#define SONIX_SPOTIFY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
	char kind[16], id[23], title[256], artist[256], album[256], album_id[23];
	uint32_t duration_ms;
} spotify_item_t;

// Init registers hooks only. Network/IPC functions are worker-thread only.
void spotify_init(void);
bool spotify_logged_in(void);
char *spotify_request(const char *op, const char *id, const char *query, int offset);
unsigned spotify_browse_begin(void);
char *spotify_browse(const char *op, const char *id, const char *query, int offset, const char *cursor, unsigned ticket);
bool spotify_parse_item(const char *json, spotify_item_t *item);
void spotify_remember(const spotify_item_t *item);
bool spotify_track_path(const char *id, char *out, size_t size);
void spotify_stop(void);
void spotify_resume(void);
void spotify_sleep(void);
const char *spotify_error_key(const char *code);
#endif
