#ifndef LASTFM_H
#define LASTFM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "src/system/library/metadata.h"

/*
 * Native Last.fm client for Sonix Player.
 *
 * The public API is deliberately small: playback code tells us when a track
 * starts, the UI thread calls lastfm_poll() periodically, and the settings
 * page owns configuration. Network I/O never runs on the UI thread.
 */

typedef struct {
	bool enabled;
	bool logged_in;
	bool logging_in;
	bool api_key_configured;
	bool api_secret_configured;
	char username[256];
	char status[192];
	char last_message[192];
	uint64_t message_serial;
} lastfm_snapshot_t;

void lastfm_init(void);

void lastfm_on_track_started(const song_metadata_t *metadata);
void lastfm_poll(void);

void lastfm_set_enabled(bool enabled);
void lastfm_set_api_key(const char *api_key);
void lastfm_set_api_secret(const char *api_secret);

void lastfm_login(const char *username, const char *password);
void lastfm_logout(void);

void lastfm_get_snapshot(lastfm_snapshot_t *out);

#endif /* LASTFM_H */
