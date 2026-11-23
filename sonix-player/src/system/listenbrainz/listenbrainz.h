#ifndef LISTENBRAINZ_H
#define LISTENBRAINZ_H

#include <stdbool.h>

#include "src/system/library/playcount.h"

// Scrobbling to ListenBrainz.
//
// Mirrors the Last.fm integration: playcount.c follows what is played and
// hands the play under way to listenbrainz_note_play() once a second while
// this is active; it decides when a play counts. This module only announces
// each play and queues it as a listen once it counts.
//
// A worker thread does the network and the files. "Now playing" and scrobbles
// are submitted to the ListenBrainz submit-listens API when there is Wi-Fi.
// Scrobbles are queued in .local on the card and sent in batches whenever the
// Wi-Fi is up. Nothing here turns the Wi-Fi on.
//
// Authentication uses a ListenBrainz user token (from listenbrainz.org
// settings), kept beside device_config.ini; it is never shown back to the UI
// once stored, only the associated user name is.

typedef enum {
	LISTENBRAINZ_STATE_UNAVAILABLE,
	LISTENBRAINZ_STATE_OFF,
	LISTENBRAINZ_STATE_SIGNED_OUT,
	LISTENBRAINZ_STATE_SIGNING_IN,
	LISTENBRAINZ_STATE_CONNECTED,
	LISTENBRAINZ_STATE_SESSION_EXPIRED, // token refused: add a valid token again
	LISTENBRAINZ_STATE_REFUSED,			// request refused; `detail` says why
} listenbrainz_state_t;

typedef enum {
	LISTENBRAINZ_LOGIN_OK,
	LISTENBRAINZ_LOGIN_WRONG_CREDENTIALS,
	LISTENBRAINZ_LOGIN_NO_REPLY,
	LISTENBRAINZ_LOGIN_REFUSED, // `detail` says why
} listenbrainz_login_result_t;

typedef struct {
	listenbrainz_state_t state;
	bool enabled;
	char user[128];
	char detail[160];
	int queued;

	unsigned login_serial;
	listenbrainz_login_result_t login_result;
} listenbrainz_status_t;

void listenbrainz_init(void);

bool listenbrainz_available(void);

void listenbrainz_set_enabled(bool on);

bool listenbrainz_active(void);

// Validates `token` (needs Wi-Fi), stores it and the user name on success.
// Returns at once; the worker does the work. The token is wiped once used.
// Leading and trailing whitespace is ignored. False, and nothing happens, when
// the module is not running or the token is empty or too long; the result of a
// request that was taken comes later, through login_serial/login_result.
bool listenbrainz_login(const char *token);
void listenbrainz_logout(void);

void listenbrainz_get_status(listenbrainz_status_t *out);

// From playcount.c, GUI thread, about once a second while
// listenbrainz_active().
void listenbrainz_note_play(const playcount_play_t *now);

// True while a request is on the wire.
bool listenbrainz_network_wanted(void);

#endif /* LISTENBRAINZ_H */
