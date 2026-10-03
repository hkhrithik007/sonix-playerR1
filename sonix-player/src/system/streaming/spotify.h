#ifndef SONIX_SPOTIFY_H
#define SONIX_SPOTIFY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
	SPOTIFY_STATE_UNKNOWN = 0,
	SPOTIFY_STATE_STOPPED,
	SPOTIFY_STATE_PLAYING,
	SPOTIFY_STATE_PAUSED,
} spotify_state_t;

typedef enum {
	SPOTIFY_REPEAT_OFF = 0,
	SPOTIFY_REPEAT_ALL,
	SPOTIFY_REPEAT_ONE,
} spotify_repeat_t;

typedef struct {
	char id[32];
	char title[192];
	char artist[192];
	uint32_t duration_ms;
} spotify_track_t;

typedef struct {
	char id[96];
	char name[192];
	char owner[192];
} spotify_playlist_t;

typedef struct {
	spotify_state_t state;
	bool available;
	spotify_track_t track;
	uint32_t position_ms;
} spotify_nowplaying_t;

/*
 * The working R1 SpotUI daemon intentionally keeps its public socket path.
 * Keeping this unchanged avoids modifying the proven librespot/daemon build in
 * the first integration milestone.
 */
#define SPOTIFY_SOCKET_PATH "/tmp/spotui.sock"

/* Device locations. The first path matches the documented working deployment;
 * the /usr/bin fallback is useful when the daemon is packaged by Sonix. */
#define SPOTIFY_DAEMON_PATH_DATA "/usr/data/spotui_daemon"
#define SPOTIFY_DAEMON_PATH_BIN  "/usr/bin/spotui_daemon"
#define SPOTIFY_LOG_PATH         "/tmp/sonix-spotify.log"

/* Start the daemon on demand. Returns true once the daemon process has been
 * launched or the socket was already available; it does not mean Spotify is
 * authenticated or connected yet. */
bool spotify_start(void);

/* Fast availability probe. This is deliberately just a local Unix-socket
 * connect and never touches the network. */
bool spotify_available(void);

/* Fire-and-forget playback controls. These mirror the daemon's existing
 * protocol and intentionally do not wait for the textual acknowledgement. */
bool spotify_play(void);
bool spotify_pause(void);
bool spotify_stop(void);
bool spotify_next(void);
bool spotify_previous(void);
bool spotify_load(const char *track_id);
bool spotify_load_liked(uint64_t request_id, const char *track_id);
bool spotify_load_playlist(uint64_t request_id, const char *playlist_id, const char *track_id);
bool spotify_load_search(uint64_t request_id, const char *track_id);
bool spotify_seek(uint32_t position_ms);
bool spotify_set_shuffle(bool enabled);
bool spotify_set_repeat(spotify_repeat_t mode);

/* One-line state queries. */
bool spotify_get_status(spotify_state_t *state);
bool spotify_get_position(uint32_t *position_ms);
bool spotify_get_now_playing(spotify_track_t *track);
bool spotify_get_modes(bool *shuffle, spotify_repeat_t *repeat);

/* Browse/search. The daemon caps these at 50 items, so keeping the Sonix-side
 * buffers bounded to the same number avoids unbounded allocations on the R1. */
#define SPOTIFY_MAX_RESULTS 50
int spotify_search(const char *query, spotify_track_t *tracks, int max_tracks);
int spotify_get_liked(spotify_track_t *tracks, int max_tracks);
int spotify_get_playlists(spotify_playlist_t *playlists, int max_playlists);
int spotify_get_playlist_tracks(const char *playlist_id, spotify_track_t *tracks, int max_tracks);

/* Monotonic-ish request IDs for latest-tap-wins queue commands. */
uint64_t spotify_next_request_id(void);

#endif /* SONIX_SPOTIFY_H */
