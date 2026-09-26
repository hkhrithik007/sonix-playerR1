#ifndef LASTFM_H
#define LASTFM_H

#include <stdbool.h>

#include "src/system/library/metadata.h"

/*
 * Native Last.fm client for Sonix Player.
 *
 * Network work runs on a dedicated worker thread. Playback only reports a
 * successful new track to this module; it never waits for Last.fm.
 *
 * Credentials and the Last.fm session key live in the normal device config.
 * Failed scrobbles live in a small line-oriented queue on the SD card so the
 * queue survives reboot without becoming a RAM-sized object.
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
    unsigned long long message_serial;
} lastfm_snapshot_t;

void lastfm_init(void);

/* Called after the playback layer has accepted a new track for playing. */
void lastfm_on_track_started(const song_metadata_t *metadata);

void lastfm_set_enabled(bool enabled);
void lastfm_set_api_key(const char *api_key);
void lastfm_set_api_secret(const char *api_secret);

void lastfm_login(const char *username, const char *password);
void lastfm_logout(void);

void lastfm_get_snapshot(lastfm_snapshot_t *out);

#endif /* LASTFM_H */
