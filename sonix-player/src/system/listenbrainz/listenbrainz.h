#ifndef LISTENBRAINZ_H
#define LISTENBRAINZ_H

#include <stdbool.h>

typedef enum {
	LISTENBRAINZ_OFF,
	LISTENBRAINZ_TOKEN_REQUIRED,
	LISTENBRAINZ_CHECKING,
	LISTENBRAINZ_CONNECTED,
	LISTENBRAINZ_REFUSED,
} listenbrainz_state_t;

typedef struct {
	listenbrainz_state_t state;
	bool enabled;
	char user[128];
	char detail[160];
	int queued;
	unsigned serial;
	bool last_token_valid;
} listenbrainz_status_t;

void listenbrainz_init(void);
void listenbrainz_set_enabled(bool enabled);
void listenbrainz_set_token(const char *token);
void listenbrainz_logout(void);
void listenbrainz_get_status(listenbrainz_status_t *out);

typedef struct {
	bool playing;
	const char *file;
	double position;
	double duration;
	const char *artist;
	const char *title;
	const char *album;
	bool skip;
} listenbrainz_playback_t;

void listenbrainz_note_playback(const listenbrainz_playback_t *now);
bool listenbrainz_network_wanted(void);

#endif
