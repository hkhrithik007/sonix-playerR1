#include "spotify.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define SPOTIFY_IO_TIMEOUT_MS 3500
#define SPOTIFY_RESPONSE_MAX  32768

static uint64_t request_seed;
static bool daemon_launch_attempted;

static bool connect_socket(int *fd_out) {
	if (!fd_out) {
		return false;
	}

	*fd_out = -1;
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		return false;
	}

	struct timeval timeout;
	timeout.tv_sec = SPOTIFY_IO_TIMEOUT_MS / 1000;
	timeout.tv_usec = (SPOTIFY_IO_TIMEOUT_MS % 1000) * 1000;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, SPOTIFY_SOCKET_PATH, sizeof(addr.sun_path) - 1);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return false;
	}

	*fd_out = fd;
	return true;
}

static bool socket_is_up(void) {
	int fd = -1;
	bool ok = connect_socket(&fd);
	if (ok) {
		close(fd);
	}
	return ok;
}

static bool executable(const char *path) {
	return path && access(path, X_OK) == 0;
}

static bool start_daemon_process(void) {
	const char *path = NULL;
	const char *override = getenv("SONIX_SPOTIFY_DAEMON");
	if (override && override[0] && executable(override)) {
		path = override;
	} else if (executable(SPOTIFY_DAEMON_PATH_DATA)) {
		path = SPOTIFY_DAEMON_PATH_DATA;
	} else if (executable(SPOTIFY_DAEMON_PATH_BIN)) {
		path = SPOTIFY_DAEMON_PATH_BIN;
	}

	if (!path) {
		return false;
	}

	pid_t pid = fork();
	if (pid < 0) {
		return false;
	}
	if (pid == 0) {
		/* Double-fork so the long-lived daemon is re-parented and the Sonix
		 * process never accumulates a zombie if the daemon exits unexpectedly. */
		(void)setsid();
		pid_t daemon_pid = fork();
		if (daemon_pid < 0) {
			_exit(127);
		}
		if (daemon_pid > 0) {
			_exit(0);
		}

		int logfd = open(SPOTIFY_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (logfd >= 0) {
			(void)dup2(logfd, STDOUT_FILENO);
			(void)dup2(logfd, STDERR_FILENO);
			if (logfd > STDERR_FILENO) {
				close(logfd);
			}
		}
		int devnull = open("/dev/null", O_RDONLY);
		if (devnull >= 0) {
			(void)dup2(devnull, STDIN_FILENO);
			if (devnull > STDIN_FILENO) {
				close(devnull);
			}
		}
		execl(path, path, (char *)NULL);
		_exit(127);
	}

	/* Reap the short-lived first child. */
	(void)waitpid(pid, NULL, 0);
	return true;
}

bool spotify_start(void) {
	if (socket_is_up()) {
		return true;
	}
	if (daemon_launch_attempted) {
		return false;
	}
	daemon_launch_attempted = true;
	return start_daemon_process();
}

bool spotify_available(void) {
	return socket_is_up();
}

static bool send_command(const char *command, bool read_reply, char *reply, size_t reply_size, bool multiline) {
	if (!command || !command[0]) {
		return false;
	}

	int fd = -1;
	if (!connect_socket(&fd)) {
		return false;
	}

	size_t command_len = strlen(command);
	if (command[command_len - 1] != '\n') {
		if (command_len + 1 >= 1024) {
			close(fd);
			return false;
		}
	}

	char command_line[1024];
	if (command[command_len - 1] == '\n') {
		if (command_len >= sizeof(command_line)) {
			close(fd);
			return false;
		}
		memcpy(command_line, command, command_len + 1);
	} else {
		int n = snprintf(command_line, sizeof(command_line), "%s\n", command);
		if (n < 0 || (size_t)n >= sizeof(command_line)) {
			close(fd);
			return false;
		}
		command_len = (size_t)n;
	}

	const char *ptr = command_line;
	size_t left = command_len;
	while (left) {
		ssize_t n = send(fd, ptr, left, 0);
		if (n <= 0) {
			close(fd);
			return false;
		}
		ptr += n;
		left -= (size_t)n;
	}

	if (!read_reply) {
		close(fd);
		return true;
	}
	if (!reply || reply_size < 2) {
		close(fd);
		return false;
	}

	size_t used = 0;
	bool got_any = false;
	char line[512];
	size_t line_used = 0;
	int expected_queue_lines = -1;
	bool queue_header_seen = false;
	bool end_seen = false;
	bool overflow = false;
	for (;;) {
		char chunk[1024];
		ssize_t got = recv(fd, chunk, sizeof(chunk), 0);
		if (got == 0) {
			break;
		}
		if (got < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		got_any = true;
		for (ssize_t i = 0; i < got; i++) {
			char ch = chunk[i];
			if (line_used + 1 < sizeof(line)) {
				line[line_used++] = ch;
			}
			if (ch != '\n') {
				continue;
			}
			line[line_used] = '\0';
			if (used + line_used + 1 < reply_size) {
				memcpy(reply + used, line, line_used);
				used += line_used;
				reply[used] = '\0';
			} else {
				overflow = true;
			}

			if (!multiline) {
				end_seen = true;
			} else if (strncmp(line, "END ", 4) == 0) {
				end_seen = true;
			} else if (!queue_header_seen && strncmp(line, "QUEUE_PAGE ", 11) == 0) {
				queue_header_seen = true;
				char source[32];
				unsigned long request_id, current, total, start, count;
				if (sscanf(line, "QUEUE_PAGE %31s %lu %lu %lu %lu %lu", source,
						&request_id, &current, &total, &start, &count) == 6) {
					expected_queue_lines = (int)count;
				}
			} else if (queue_header_seen && expected_queue_lines >= 0) {
				expected_queue_lines--;
				if (expected_queue_lines == 0) {
					end_seen = true;
				}
			}
			line_used = 0;
			if (overflow) {
				end_seen = true;
			}
			if (end_seen) {
				break;
			}
		}
	}

	close(fd);
	if (!got_any || overflow) {
		reply[0] = '\0';
		return false;
	}
	reply[used] = '\0';
	return end_seen;
}

static bool one_line(const char *command, char *reply, size_t reply_size) {
	return send_command(command, true, reply, reply_size, false);
}

static bool fire(const char *command) {
	return send_command(command, false, NULL, 0, false);
}

bool spotify_play(void) { return fire("PLAY"); }
bool spotify_pause(void) { return fire("PAUSE"); }
bool spotify_stop(void) { return fire("STOP"); }
bool spotify_next(void) { return fire("NEXT"); }
bool spotify_previous(void) { return fire("PREVIOUS"); }

bool spotify_load(const char *track_id) {
	char command[128];
	if (!track_id || snprintf(command, sizeof(command), "LOAD %s", track_id) >= (int)sizeof(command)) {
		return false;
	}
	return fire(command);
}

bool spotify_load_liked(uint64_t request_id, const char *track_id) {
	char command[160];
	if (!track_id || snprintf(command, sizeof(command), "LOAD_LIKED %llu %s",
			(unsigned long long)request_id, track_id) >= (int)sizeof(command)) {
		return false;
	}
	return fire(command);
}

bool spotify_load_playlist(uint64_t request_id, const char *playlist_id, const char *track_id) {
	char command[256];
	if (!playlist_id || !track_id || snprintf(command, sizeof(command), "LOAD_PLAYLIST %llu %s %s",
			(unsigned long long)request_id, playlist_id, track_id) >= (int)sizeof(command)) {
		return false;
	}
	return fire(command);
}

bool spotify_load_search(uint64_t request_id, const char *track_id) {
	char command[160];
	if (!track_id || snprintf(command, sizeof(command), "LOAD_SEARCH %llu %s",
			(unsigned long long)request_id, track_id) >= (int)sizeof(command)) {
		return false;
	}
	return fire(command);
}

bool spotify_seek(uint32_t position_ms) {
	char command[64];
	if (snprintf(command, sizeof(command), "SEEK %u", position_ms) >= (int)sizeof(command)) {
		return false;
	}
	return fire(command);
}

bool spotify_set_shuffle(bool enabled) {
	return fire(enabled ? "SET_SHUFFLE ON" : "SET_SHUFFLE OFF");
}

bool spotify_set_repeat(spotify_repeat_t mode) {
	const char *name = NULL;
	switch (mode) {
	case SPOTIFY_REPEAT_OFF: name = "OFF"; break;
	case SPOTIFY_REPEAT_ALL: name = "ALL"; break;
	case SPOTIFY_REPEAT_ONE: name = "ONE"; break;
	default: return false;
	}
	char command[64];
	if (snprintf(command, sizeof(command), "SET_REPEAT %s", name) >= (int)sizeof(command)) {
		return false;
	}
	return fire(command);
}

bool spotify_get_status(spotify_state_t *state) {
	if (!state) return false;
	char reply[128];
	if (!one_line("STATUS", reply, sizeof(reply))) return false;
	if (strstr(reply, "STATUS PLAYING") == reply) {
		*state = SPOTIFY_STATE_PLAYING;
	} else if (strstr(reply, "STATUS PAUSED") == reply) {
		*state = SPOTIFY_STATE_PAUSED;
	} else if (strstr(reply, "STATUS STOPPED") == reply) {
		*state = SPOTIFY_STATE_STOPPED;
	} else {
		*state = SPOTIFY_STATE_UNKNOWN;
		return false;
	}
	return true;
}

bool spotify_get_position(uint32_t *position_ms) {
	if (!position_ms) return false;
	char reply[128];
	if (!one_line("POSITION", reply, sizeof(reply))) return false;
	unsigned long value;
	if (sscanf(reply, "POSITION %lu", &value) != 1) return false;
	*position_ms = (uint32_t)value;
	return true;
}

bool spotify_get_now_playing(spotify_track_t *track) {
	if (!track) return false;
	char reply[768];
	if (!one_line("NOW_PLAYING", reply, sizeof(reply))) return false;

	char *cursor = reply;
	char *space = strchr(cursor, ' ');
	if (!space) return false;
	*space = '\0';
	if (strcmp(cursor, "NOW_PLAYING") != 0) return false;
	cursor = space + 1;

	if (strcmp(cursor, "NONE") == 0) return false;

	char *tab1 = strchr(cursor, '\t');
	if (!tab1) return false;
	*tab1 = '\0';
	char *title = tab1 + 1;

	char *tab2 = strchr(title, '\t');
	if (!tab2) return false;
	*tab2 = '\0';
	char *artist = tab2 + 1;

	char *tab3 = strchr(artist, '\t');
	if (!tab3) return false;
	*tab3 = '\0';
	char *duration_text = tab3 + 1;

	unsigned long duration = 0;
	if (sscanf(duration_text, "%lu", &duration) != 1) return false;

	memset(track, 0, sizeof(*track));
	strncpy(track->id, cursor, sizeof(track->id) - 1);
	strncpy(track->title, title, sizeof(track->title) - 1);
	strncpy(track->artist, artist, sizeof(track->artist) - 1);
	track->duration_ms = (uint32_t)duration;
	return true;
}

bool spotify_get_modes(bool *shuffle, spotify_repeat_t *repeat) {
	char reply[128];
	if (!one_line("PLAYBACK_MODES", reply, sizeof(reply))) return false;
	if (shuffle) *shuffle = strstr(reply, "SHUFFLE ON") != NULL;
	if (repeat) {
		if (strstr(reply, "REPEAT ONE")) *repeat = SPOTIFY_REPEAT_ONE;
		else if (strstr(reply, "REPEAT ALL")) *repeat = SPOTIFY_REPEAT_ALL;
		else *repeat = SPOTIFY_REPEAT_OFF;
	}
	return true;
}

static int parse_results(const char *response, spotify_track_t *tracks, int max_tracks) {
	if (!response || !tracks || max_tracks <= 0) return 0;
	int count = 0;
	const char *line = response;
	while (*line && count < max_tracks) {
		const char *end = strchr(line, '\n');
		if (!end) break;
		size_t len = (size_t)(end - line);
		if (len >= 7 && strncmp(line, "RESULT ", 7) == 0) {
			char buf[512];
			size_t copy = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
			memcpy(buf, line + 7, copy - 7);
			buf[copy - 7] = '\0';
			char *tab1 = strchr(buf, '\t');
			if (tab1) {
				*tab1 = '\0';
				char *tab2 = strchr(tab1 + 1, '\t');
				if (tab2) *tab2 = '\0';
				memset(&tracks[count], 0, sizeof(tracks[count]));
				strncpy(tracks[count].id, buf, sizeof(tracks[count].id) - 1);
				strncpy(tracks[count].title, tab1 + 1, sizeof(tracks[count].title) - 1);
				if (tab2) strncpy(tracks[count].artist, tab2 + 1, sizeof(tracks[count].artist) - 1);
				count++;
			}
		}
		line = end + 1;
	}
	return count;
}

static int request_results(const char *command, spotify_track_t *tracks, int max_tracks) {
	char response[SPOTIFY_RESPONSE_MAX];
	if (!send_command(command, true, response, sizeof(response), true)) return 0;
	return parse_results(response, tracks, max_tracks);
}

int spotify_search(const char *query, spotify_track_t *tracks, int max_tracks) {
	if (!query) return 0;
	char command[768];
	if (snprintf(command, sizeof(command), "SEARCH %s", query) >= (int)sizeof(command)) return 0;
	return request_results(command, tracks, max_tracks);
}

int spotify_get_liked(spotify_track_t *tracks, int max_tracks) {
	return request_results("LIKED", tracks, max_tracks);
}

int spotify_get_playlist_tracks(const char *playlist_id, spotify_track_t *tracks, int max_tracks) {
	if (!playlist_id) return 0;
	char command[256];
	if (snprintf(command, sizeof(command), "PLAYLIST %s", playlist_id) >= (int)sizeof(command)) return 0;
	return request_results(command, tracks, max_tracks);
}

int spotify_get_playlists(spotify_playlist_t *playlists, int max_playlists) {
	if (!playlists || max_playlists <= 0) return 0;
	char response[SPOTIFY_RESPONSE_MAX];
	if (!send_command("PLAYLISTS", true, response, sizeof(response), true)) return 0;

	int count = 0;
	const char *line = response;
	while (*line && count < max_playlists) {
		const char *end = strchr(line, '\n');
		if (!end) break;
		size_t len = (size_t)(end - line);
		if (len > 9 && strncmp(line, "PLAYLIST ", 9) == 0) {
			char buf[600];
			size_t copy = len - 9;
			if (copy >= sizeof(buf)) copy = sizeof(buf) - 1;
			memcpy(buf, line + 9, copy);
			buf[copy] = '\0';
			char *tab1 = strchr(buf, '\t');
			char *tab2 = NULL;
			if (tab1) {
				*tab1 = '\0';
				tab2 = strchr(tab1 + 1, '\t');
				if (tab2) *tab2 = '\0';
			}
			memset(&playlists[count], 0, sizeof(playlists[count]));
			strncpy(playlists[count].id, buf, sizeof(playlists[count].id) - 1);
			if (tab1) strncpy(playlists[count].name, tab1 + 1, sizeof(playlists[count].name) - 1);
			if (tab2) strncpy(playlists[count].owner, tab2 + 1, sizeof(playlists[count].owner) - 1);
			count++;
		}
		line = end + 1;
	}
	return count;
}

uint64_t spotify_next_request_id(void) {
	if (request_seed == 0) {
		struct timeval tv;
		gettimeofday(&tv, NULL);
		request_seed = ((uint64_t)tv.tv_sec << 20) ^ ((uint64_t)tv.tv_usec << 1) ^ (uint64_t)getpid();
	}
	return ++request_seed;
}
