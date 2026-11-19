#define _GNU_SOURCE
#include "spotify.h"
#include "src/gui/shell/gui.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/sx_toast.h"
#include "src/system/core/config.h"
#include "src/system/core/json.h"
#include "src/system/core/lang.h"
#include "src/system/decode/growfile.h"
#include "src/system/device/system.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/playback/sleeptimer.h"
#include "src/system/streaming/cardimport.h"
#include "src/system/streaming/provider.h"
#include "src/system/streaming/streamturn.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t launch_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t reaped = PTHREAD_COND_INITIALIZER;
static pid_t child;
static int owner_write = -1;
static int sockets[4] = {-1, -1, -1, -1};
static unsigned generation;
static unsigned browse_generation;
static int browse_socket = -1;
static bool disabled;
static char directory[384], socket_path[108], cache_root[384];
static spotify_item_t *memo;
static int memo_count;
static unsigned memo_replace;
#define MEMO_MAX 256
#define RESPONSE_MAX (128 * 1024)
#define PREBUFFER (128 * 1024)
static bool cache_worker;
static char pending_path[512], active_path[512];
// Published by the GUI; workers must never inspect the mutable playlist.
static char retained_current[512], retained_next[512];
static long active_bytes;
static bool active_ready;
static bool waiting;
static unsigned waiting_sleep;
static unsigned download_sleep;
static unsigned pending_sleep;
static void prefetch_async(void *user);

static bool valid_id(const char *id) {
	if (!id || strlen(id) != 22)
		return false;
	for (int i = 0; i < 22; i++)
		if (!isalnum((unsigned char)id[i]) || (unsigned char)id[i] > 127)
			return false;
	return true;
}

static void paths_init(void) {
	const char *cfg = config_path_copy((char[512]){0}, 512);
	const char *slash = cfg ? strrchr(cfg, '/') : NULL;
	if (slash)
		snprintf(directory, sizeof(directory), "%.*s/spotify", (int)(slash - cfg), cfg);
	else
		snprintf(directory, sizeof(directory), "./spotify");
	size_t n = strlen(directory);
	if (n + sizeof("/control.sock") <= sizeof(socket_path)) {
		memcpy(socket_path, directory, n);
		memcpy(socket_path + n, "/control.sock", sizeof("/control.sock"));
	} else
		socket_path[0] = 0;
}

bool spotify_logged_in(void) {
	char path[512];
	snprintf(path, sizeof(path), "%s/credentials.json", directory);
	struct stat st;
	return lstat(path, &st) == 0 && S_ISREG(st.st_mode) && !(st.st_mode & 077);
}

static void *reap_child(void *user) {
	pid_t pid = (pid_t)(intptr_t)user;
	while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
	}
	pthread_mutex_lock(&lock);
	if (child == pid) {
		child = 0;
		if (owner_write >= 0)
			close(owner_write);
		owner_write = -1;
	}
	pthread_cond_broadcast(&reaped);
	pthread_mutex_unlock(&lock);
	return NULL;
}

static bool spawn_engine(void) {
	if (!socket_path[0])
		return false;
	struct stat st;
	if (mkdir(directory, 0700) != 0 && errno != EEXIST)
		return false;
	if (lstat(directory, &st) || !S_ISDIR(st.st_mode) || (st.st_mode & 077))
		return false;
	const char *engine = getenv("SONIX_SPOTIFY_ENGINE");
	if (!engine || !*engine)
		engine = "/usr/bin/sonix_spotify";
	char *argv[] = {(char *)engine, "--socket", socket_path, "--credentials-dir", directory, "--parent-fd", "3", NULL};
	int null = open("/dev/null", O_RDWR | O_CLOEXEC);
	if (null < 0)
		return false;
	if (null < 3) {
		int moved = fcntl(null, F_DUPFD_CLOEXEC, 3);
		close(null);
		null = moved;
	}
	if (null < 0)
		return false;
	int owner[2];
	if (pipe2(owner, O_CLOEXEC | O_NONBLOCK)) {
		close(null);
		return false;
	}
	for (int i = 0; i < 2; i++) {
		if (owner[i] < 3) {
			int moved = fcntl(owner[i], F_DUPFD_CLOEXEC, 3);
			close(owner[i]);
			owner[i] = moved;
		}
		if (owner[i] < 0) {
			if (i == 0)
				close(owner[1]);
			else
				close(owner[0]);
			close(null);
			return false;
		}
	}
	long limit = sysconf(_SC_OPEN_MAX);
	if (limit < 0) {
		close(owner[0]);
		close(owner[1]);
		close(null);
		return false;
	}
	pid_t pid = fork();
	if (pid == 0) {
		// Only async-signal-safe operations after fork. Never inherit an open
		// card, audio device or service connection into the transport process.
		for (int fd = 0; fd < 3; fd++)
			if (dup2(null, fd) < 0)
				_exit(126);
		if (dup2(owner[0], 3) < 0 || fcntl(3, F_SETFD, 0) < 0)
			_exit(126);
#ifdef SYS_close_range
		if (syscall(SYS_close_range, 4u, ~0u, 0u) != 0)
#endif
			for (long fd = 4; fd < limit; fd++)
				close((int)fd);
		execve(engine, argv, environ);
		_exit(127);
	}
	close(null);
	close(owner[0]);
	if (pid < 0) {
		close(owner[1]);
		return false;
	}
	pthread_mutex_lock(&lock);
	child = pid;
	owner_write = owner[1];
	pthread_mutex_unlock(&lock);
	pthread_t thread;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 64 * 1024);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	int error = pthread_create(&thread, &attr, reap_child, (void *)(intptr_t)pid);
	pthread_attr_destroy(&attr);
	if (error) {
		kill(pid, SIGTERM);
		reap_child((void *)(intptr_t)pid);
		return false;
	}
	return true;
}

static int connect_engine(unsigned stamp) {
	pthread_mutex_lock(&launch_lock);
	pthread_mutex_lock(&lock);
	bool allowed = !disabled && stamp == generation;
	bool running = child > 0;
	pthread_mutex_unlock(&lock);
	if (!allowed || (!running && !spawn_engine())) {
		pthread_mutex_unlock(&launch_lock);
		return -1;
	}
	int fd = -1;
	int restarts = 0;
	for (int attempt = 0; attempt < 100; attempt++) {
		fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (fd < 0)
			break;
		struct sockaddr_un addr = {.sun_family = AF_UNIX};
		snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);
		if (!connect(fd, (struct sockaddr *)&addr, sizeof(addr)))
			break;
		close(fd);
		fd = -1;
		pthread_mutex_lock(&lock);
		allowed = !disabled && stamp == generation && child > 0;
		pthread_mutex_unlock(&lock);
		if (!allowed) {
			pthread_mutex_lock(&lock);
			bool restart = !disabled && stamp == generation && child == 0;
			pthread_mutex_unlock(&lock);
			if (!restart || restarts++ != 0 || !spawn_engine())
				break;
		}
		usleep(10000);
	}
	pthread_mutex_unlock(&launch_lock);
	if (fd < 0)
		return -1;
	struct timeval timeout = {.tv_sec = 22};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
	pthread_mutex_lock(&lock);
	int slot = 0;
	while (slot < 4 && sockets[slot] >= 0)
		slot++;
	if (disabled || stamp != generation || slot == 4) {
		close(fd);
		fd = -1;
	} else
		sockets[slot] = fd;
	pthread_mutex_unlock(&lock);
	return fd;
}

static void disconnect_engine(int fd) {
	if (fd < 0)
		return;
	pthread_mutex_lock(&lock);
	if (browse_socket == fd)
		browse_socket = -1;
	for (int i = 0; i < 4; i++)
		if (sockets[i] == fd)
			sockets[i] = -1;
	close(fd);
	pthread_mutex_unlock(&lock);
}

static void abandon(void) {
	pthread_mutex_lock(&lock);
	generation++;
	pending_path[0] = 0;
	for (int i = 0; i < 4; i++)
		if (sockets[i] >= 0)
			shutdown(sockets[i], SHUT_RDWR);
	pthread_mutex_unlock(&lock);
}

void spotify_sleep(void) {
	pthread_mutex_lock(&lock);
	disabled = true;
	waiting = false;
	generation++;
	pending_path[0] = 0;
	if (owner_write >= 0) {
		close(owner_write);
		owner_write = -1;
	}
	for (int i = 0; i < 4; i++)
		if (sockets[i] >= 0)
			shutdown(sockets[i], SHUT_RDWR);
	if (child > 0)
		kill(child, SIGTERM);
	pthread_mutex_unlock(&lock);
}

void spotify_stop(void) {
	spotify_sleep();
	pthread_mutex_lock(&launch_lock);
	pthread_mutex_lock(&lock);
	if (child > 0) {
		kill(child, SIGTERM);
		struct timespec deadline;
		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_sec += 2;
		while (child > 0 && pthread_cond_timedwait(&reaped, &lock, &deadline) == 0) {
		}
		if (child > 0)
			kill(child, SIGKILL);
	}
	pthread_mutex_unlock(&lock);
	pthread_mutex_unlock(&launch_lock);
}

void spotify_resume(void) {
	pthread_mutex_lock(&lock);
	disabled = false;
	pthread_mutex_unlock(&lock);
}

static void quote(const char *text, char *out, size_t size) {
	size_t n = 0;
	for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p && n + 7 < size; p++) {
		if (*p == '"' || *p == '\\') {
			out[n++] = '\\';
			out[n++] = *p;
		} else if (*p < 32) {
			n += (size_t)snprintf(out + n, size - n, "\\u%04x", *p);
		} else
			out[n++] = *p;
	}
	out[n] = 0;
}

static bool send_request(int fd, const char *op, const char *id, const char *query, int offset, const char *cursor) {
	char escaped[3072], request[8192];
	quote(query, escaped, sizeof(escaped));
	if (id && *id && !valid_id(id))
		return false;
	int n = snprintf(request, sizeof(request), "{\"op\":\"%s\",\"id\":\"%s\",\"query\":\"%s\",\"offset\":%d,\"cursor\":%s}\n", op, id ? id : "", escaped, offset, cursor && *cursor ? cursor : "null");
	if (n < 0 || n >= (int)sizeof(request))
		return false;
	int sent = 0;
	while (sent < n) {
		ssize_t part = send(fd, request + sent, (size_t)(n - sent), MSG_NOSIGNAL);
		if (part < 0 && errno == EINTR)
			continue;
		if (part <= 0)
			return false;
		sent += (int)part;
	}
	return true;
}

static char *read_header(int fd) {
	char *text = malloc(RESPONSE_MAX + 1);
	if (!text)
		return NULL;
	size_t have = 0;
	// Preserve stream bytes following the JSON line in the socket buffer.
	while (have < RESPONSE_MAX) {
		char peek[2048];
		ssize_t got = recv(fd, peek, sizeof(peek), MSG_PEEK);
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break;
		char *end = memchr(peek, '\n', (size_t)got);
		size_t take = end ? (size_t)(end - peek) + 1 : (size_t)got;
		if (have + take > RESPONSE_MAX)
			break;
		ssize_t used = recv(fd, text + have, take, 0);
		if (used <= 0)
			break;
		have += (size_t)used;
		if (text[have - 1] == '\n') {
			text[have] = 0;
			return text;
		}
	}
	free(text);
	return NULL;
}

char *spotify_request(const char *op, const char *id, const char *query, int offset) { return spotify_browse(op, id, query, offset, NULL, 0); }

unsigned spotify_browse_begin(void) {
	pthread_mutex_lock(&lock);
	if (++browse_generation == 0)
		browse_generation++;
	if (browse_socket >= 0)
		shutdown(browse_socket, SHUT_RDWR);
	unsigned ticket = browse_generation;
	pthread_mutex_unlock(&lock);
	return ticket;
}

char *spotify_browse(const char *op, const char *id, const char *query, int offset, const char *cursor, unsigned ticket) {
	pthread_mutex_lock(&lock);
	unsigned stamp = generation;
	bool current = !ticket || ticket == browse_generation;
	pthread_mutex_unlock(&lock);
	if (!current)
		return NULL;
	int fd = connect_engine(stamp);
	pthread_mutex_lock(&lock);
	current = !ticket || ticket == browse_generation;
	if (fd >= 0 && current && ticket)
		browse_socket = fd;
	pthread_mutex_unlock(&lock);
	if (!current) {
		disconnect_engine(fd);
		return NULL;
	}
	char *response = fd >= 0 && send_request(fd, op, id, query, offset, cursor) ? read_header(fd) : NULL;
	disconnect_engine(fd);
	return response;
}

const char *spotify_error_key(const char *code) {
	if (code && (!strcmp(code, "auth_required") || !strcmp(code, "private_credentials_required")))
		return "spotify_sign_in";
	if (code && !strcmp(code, "premium_required"))
		return "spotify_premium";
	return "spotify_unavailable";
}

static bool item_from_doc(const json_doc_t *doc, int obj, spotify_item_t *item) {
	memset(item, 0, sizeof(*item));
	json_obj_str(doc, obj, "id", item->id, sizeof(item->id));
	json_obj_str(doc, obj, "kind", item->kind, sizeof(item->kind));
	json_obj_str(doc, obj, "title", item->title, sizeof(item->title));
	json_obj_str(doc, obj, "artist", item->artist, sizeof(item->artist));
	json_obj_str(doc, obj, "album", item->album, sizeof(item->album));
	json_obj_str(doc, obj, "album_id", item->album_id, sizeof(item->album_id));
	item->duration_ms = (uint32_t)json_obj_long(doc, obj, "duration_ms", 0);
	return valid_id(item->id) && item->title[0];
}

bool spotify_parse_item(const char *text, spotify_item_t *item) {
	json_doc_t doc;
	if (!text || !json_parse(text, &doc))
		return false;
	bool ok = item_from_doc(&doc, json_root(&doc), item);
	json_free(&doc);
	return ok;
}

void spotify_remember(const spotify_item_t *item) {
	if (!valid_id(item->id) || strcmp(item->kind, "track"))
		return;
	pthread_mutex_lock(&lock);
	if (!memo)
		memo = calloc(MEMO_MAX, sizeof(*memo));
	if (memo) {
		int at = 0;
		while (at < memo_count && strcmp(memo[at].id, item->id))
			at++;
		if (at == MEMO_MAX)
			at = memo_replace++ % MEMO_MAX;
		if (at < MEMO_MAX) {
			memo[at] = *item;
			if (at == memo_count)
				memo_count++;
		}
	}
	pthread_mutex_unlock(&lock);
}

bool spotify_track_path(const char *id, char *out, size_t size) {
	pthread_mutex_lock(&lock);
	bool ok = cache_root[0] && valid_id(id);
	if (ok)
		ok = snprintf(out, size, "%s/%s/audio.ogg", cache_root, id) < (int)size;
	pthread_mutex_unlock(&lock);
	return ok;
}

static bool path_id(const char *path, char id[23]) {
	pthread_mutex_lock(&lock);
	size_t n = strlen(cache_root);
	bool owned = n && path && !strncmp(path, cache_root, n) && path[n] == '/';
	if (owned) {
		const char *tail = path + n + 1;
		owned = strlen(tail) == 32 && !strcmp(tail + 22, "/audio.ogg");
		if (owned) {
			memcpy(id, tail, 22);
			id[22] = 0;
			owned = valid_id(id);
		}
	}
	pthread_mutex_unlock(&lock);
	return owned;
}

static bool owns(const char *path) {
	char id[23];
	return path_id(path, id);
}
static const char *cache_dir(void) { return cache_root; }
static bool peek(const char *path, song_metadata_t *out) {
	char id[23];
	if (!path_id(path, id))
		return false;
	bool found = false;
	pthread_mutex_lock(&lock);
	for (int i = 0; memo && i < memo_count; i++)
		if (!strcmp(id, memo[i].id)) {
			memset(out, 0, sizeof(*out));
			snprintf(out->title, sizeof(out->title), "%s", memo[i].title);
			snprintf(out->artist, sizeof(out->artist), "%s", memo[i].artist);
			snprintf(out->album, sizeof(out->album), "%s", memo[i].album);
			out->has_tags = true;
			found = true;
			break;
		}
	if (!found) {
		memset(out, 0, sizeof(*out));
		snprintf(out->title, sizeof(out->title), "Spotify");
		out->has_tags = true;
	}
	pthread_mutex_unlock(&lock);
	return true;
}
static bool album_id(const char *path, char *out, size_t size) {
	char id[23];
	if (!path_id(path, id))
		return false;
	bool found = false;
	pthread_mutex_lock(&lock);
	for (int i = 0; memo && i < memo_count; i++)
		if (!strcmp(id, memo[i].id)) {
			snprintf(out, size, "%s", memo[i].album_id);
			found = out[0] != 0;
			break;
		}
	pthread_mutex_unlock(&lock);
	return found;
}
static bool downloading(void) {
	pthread_mutex_lock(&lock);
	bool busy = cache_worker;
	pthread_mutex_unlock(&lock);
	return busy;
}
static bool network_wanted(void) {
	pthread_mutex_lock(&lock);
	bool wanted = child > 0 || cache_worker;
	pthread_mutex_unlock(&lock);
	return wanted;
}
static bool unstarred(const char *path) {
	(void)path;
	return false;
}
static void no_refresh(void (*done)(void)) { (void)done; }
static void no_toggle(const char *path, bool want, stream_provider_done_cb done, void *user) {
	(void)path;
	(void)want;
	if (done)
		done(false, tr("spotify_read_only"), user);
}

// Cache/prepare implementation follows below; browsing never replaces a queue.
static bool prepare(const char *path);
static cardimport_result_t import_credentials(const char lines[][CARDIMPORT_LINE_MAX], int count, char *message, size_t size);
static void wipe(void) {
	spotify_stop();
	const char *names[] = {"credentials.json", "credentials.previous", "credentials.tmp"};
	for (int i = 0; i < 3; i++) {
		char path[512];
		snprintf(path, sizeof(path), "%s/%s", directory, names[i]);
		unlink(path);
	}
	pthread_mutex_lock(&lock);
	free(memo);
	memo = NULL;
	memo_count = 0;
	pthread_mutex_unlock(&lock);
}
static void set_root(const char *root) {
	spotify_stop();
	pthread_mutex_lock(&lock);
	disabled = !root || !*root;
	if (!disabled)
		snprintf(cache_root, sizeof(cache_root), "%s/.local/spotify-cache", root);
	else
		cache_root[0] = 0;
	pthread_mutex_unlock(&lock);
}

void spotify_init(void) {
	paths_init();
	// An interrupted import keeps the last validated account, never its candidate.
	char previous[512], current[512];
	struct stat st;
	snprintf(previous, sizeof(previous), "%s/credentials.previous", directory);
	snprintf(current, sizeof(current), "%s/credentials.json", directory);
	if (!lstat(previous, &st) && S_ISREG(st.st_mode) && !(st.st_mode & 077))
		rename(previous, current);
	static stream_provider_t provider = {.id = "spotify", .read_only = true, .set_root = set_root, .cache_dir = cache_dir, .owns_path = owns, .is_track = owns, .downloading = downloading, .network_wanted = network_wanted, .clear_on_exit = spotify_stop, .wipe = wipe, .peek_metadata = peek, .album_id = album_id, .album_failed_key = "spotify_unavailable", .fav_get = unstarred, .fav_refresh = no_refresh, .fav_toggle = no_toggle, .fav_failed_key = "spotify_read_only"};
	stream_provider_register(&provider);
	streamturn_register_abandon(abandon);
	device_state_add_prepare_cb(prepare);
	cardimport_register("spotify", "spotify-credentials.json", import_credentials);
}

static bool completed(const char *path) {
	char marker[540];
	snprintf(marker, sizeof(marker), "%s.complete", path);
	FILE *file = fopen(marker, "r");
	long expected = 0;
	if (file) {
		if (fscanf(file, "%ld", &expected) != 1)
			expected = 0;
		fclose(file);
	}
	struct stat st;
	return expected > 0 && !stat(path, &st) && st.st_size == expected;
}

static void ready_async(void *user) {
	char *path = user;
	device_state_note_metadata_available(path);
	char current[512];
	pthread_mutex_lock(&lock);
	bool wanted = waiting && waiting_sleep == sleeptimer_expiration_serial();
	pthread_mutex_unlock(&lock);
	int index = playlist_current_index();
	if (wanted && index >= 0 && playlist_path_at(index, current, sizeof(current)) && !strcmp(path, current)) {
		pthread_mutex_lock(&lock);
		waiting = false;
		pthread_mutex_unlock(&lock);
		if (device_state_play_queue_index(index))
			player_refresh_now_playing();
	}
	free(path);
}

static void post_ready(const char *path) {
	char *copy = strdup(path);
	if (copy && !gui_post(ready_async, copy))
		free(copy);
}

static void post_prefetch(const char *path) {
	char *copy = strdup(path);
	if (copy && !gui_post(prefetch_async, copy))
		free(copy);
}

static void failure_async(void *user) {
	char *path = user;
	char current[512];
	int index = playlist_current_index();
	pthread_mutex_lock(&lock);
	bool wanted = waiting && waiting_sleep == sleeptimer_expiration_serial();
	if (wanted)
		waiting = false;
	pthread_mutex_unlock(&lock);
	if (wanted && index >= 0 && playlist_path_at(index, current, sizeof(current)) && !strcmp(current, path))
		sx_toast(tr("spotify_unavailable"));
	free(path);
}

static bool stamp_valid(unsigned stamp) {
	pthread_mutex_lock(&lock);
	bool ok = !disabled && stamp == generation && download_sleep == sleeptimer_expiration_serial();
	pthread_mutex_unlock(&lock);
	return ok;
}

static int child_directory(int parent, const char *name) {
	if (mkdirat(parent, name, 0700) && errno != EEXIST)
		return -1;
	return openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

static void trim_cache(int cache, const char *keep) {
	DIR *dir = fdopendir(dup(cache));
	if (!dir)
		return;
	struct dirent *entry;
	while ((entry = readdir(dir))) {
		if (!valid_id(entry->d_name) || !strcmp(entry->d_name, keep))
			continue;
		char path[512];
		if (!spotify_track_path(entry->d_name, path, sizeof(path)))
			continue;
		pthread_mutex_lock(&lock);
		if (!strcmp(path, retained_current) || !strcmp(path, retained_next) || growfile_is_growing(path)) {
			pthread_mutex_unlock(&lock);
			continue;
		}
		int track = openat(cache, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (track < 0) {
			pthread_mutex_unlock(&lock);
			continue;
		}
		unlinkat(track, "audio.ogg", 0);
		unlinkat(track, "audio.ogg.complete", 0);
		unlinkat(track, "audio.ogg.tags", 0);
		close(track);
		unlinkat(cache, entry->d_name, AT_REMOVEDIR);
		pthread_mutex_unlock(&lock);
	}
	closedir(dir);
}

// GUI thread only. Publish before checking a cached file or starting playback.
static void retain_queue_paths(const char *current) {
	char next[512] = "";
	int index = playlist_current_index();
	playlist_path_at(index + 1, next, sizeof(next));
	pthread_mutex_lock(&lock);
	snprintf(retained_current, sizeof(retained_current), "%s", current ? current : "");
	snprintf(retained_next, sizeof(retained_next), "%s", next);
	pthread_mutex_unlock(&lock);
}

static bool store_gain_header(int folder, const json_doc_t *doc) {
	const char *fields[] = {"track_gain_db", "track_peak", "album_gain_db", "album_peak"};
	const char *tags[] = {"REPLAYGAIN_TRACK_GAIN", "REPLAYGAIN_TRACK_PEAK", "REPLAYGAIN_ALBUM_GAIN", "REPLAYGAIN_ALBUM_PEAK"};
	int fd = openat(folder, "audio.ogg.tags", O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return false;
	bool ok = true;
	for (int i = 0; i < 4; i++) {
		double value = json_double(doc, json_get(doc, json_root(doc), fields[i]), NAN);
		if (isfinite(value) && (!(i & 1) || value > 0))
			ok = dprintf(fd, "%s=%.9g\n", tags[i], value) > 0 && ok;
	}
	if (fsync(fd))
		ok = false;
	close(fd);
	return ok;
}

static bool fetch(const char *path, unsigned stamp) {
	char id[23], root[384];
	if (!path_id(path, id))
		return false;
	char *details = spotify_request("track", id, NULL, 0);
	spotify_item_t info = {0};
	uint32_t duration = 0;
	if (details) {
		json_doc_t doc;
		if (json_parse(details, &doc)) {
			spotify_item_t item;
			if (item_from_doc(&doc, json_get(&doc, json_root(&doc), "item"), &item)) {
				spotify_remember(&item);
				info = item;
				duration = item.duration_ms;
			}
			json_free(&doc);
		}
		free(details);
	}
	if (!stamp_valid(stamp))
		return false;
	int pin = storage_pin_card(root, sizeof(root));
	if (pin < 0)
		return false;
	int local = child_directory(pin, ".local");
	int cache = local >= 0 ? child_directory(local, "spotify-cache") : -1;
	if (local >= 0)
		close(local);
	int folder = cache >= 0 ? child_directory(cache, id) : -1;
	if (folder < 0) {
		if (cache >= 0)
			close(cache);
		close(pin);
		return false;
	}
	trim_cache(cache, id);
	// Reuse the common sidecar reader, including after a queue is restored.
	// Only a track actually played writes tags; browsing never writes the card.
	if (info.title[0]) {
		int tags = openat(folder, "audio.ogg.tags", O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
		if (tags >= 0) {
			dprintf(tags, "title=%s\nartist=%s\nalbum=%s\n", info.title, info.artist, info.album);
			fsync(tags);
			close(tags);
		}
	}
	unlinkat(folder, "audio.ogg.complete", 0);
	int output = openat(folder, "audio.ogg", O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
	long have = 0, total = 0;
	bool announced = false, notified = false, ok = false;
	char buffer[16384];
	for (int retry = 0; output >= 0 && retry < 3 && stamp_valid(stamp); retry++) {
		int fd = connect_engine(stamp);
		char *header = fd >= 0 && send_request(fd, "stream", id, NULL, (int)have, NULL) ? read_header(fd) : NULL;
		json_doc_t doc;
		bool parsed = header && json_parse(header, &doc);
		long expected = parsed ? json_obj_long(&doc, json_root(&doc), "total", 0) : 0;
		long offset = parsed ? json_obj_long(&doc, json_root(&doc), "offset", -1) : -1;
		bool accepted = parsed && json_obj_bool(&doc, json_root(&doc), "ok", false) && offset == have && expected > 0 && expected <= 64 * 1024 * 1024L && (!total || total == expected);
		struct statvfs space;
		if (accepted && (fstatvfs(pin, &space) || (unsigned long long)space.f_bavail * space.f_frsize < (unsigned long long)(expected - have) + 1024 * 1024))
			accepted = false;
		// Commit loudness before announcing bytes: ready_async reloads metadata
		// before audio_play, so the first PCM block uses Sonix's normal gain.
		if (accepted && have == 0 && !store_gain_header(folder, &doc))
			accepted = false;
		if (parsed)
			json_free(&doc);
		free(header);
		if (!accepted) {
			disconnect_engine(fd);
			break;
		}
		total = expected;
		if (!announced) {
			growfile_announce(path, total);
			growfile_set_duration(path, duration);
			announced = true;
		}
		int quiet = 0;
		while (have < total && stamp_valid(stamp)) {
			struct pollfd pending = {.fd = fd, .events = POLLIN};
			int result = poll(&pending, 1, 250);
			if (result < 0 && errno == EINTR)
				continue;
			if (result < 0)
				break;
			if (!result) {
				if (++quiet >= 40)
					break;
				continue;
			}
			quiet = 0;
			ssize_t got = recv(fd, buffer, sizeof(buffer), 0);
			if (got < 0 && errno == EINTR)
				continue;
			if (got <= 0 || have + got > total)
				break;
			ssize_t written = 0;
			while (written < got) {
				ssize_t n = write(output, buffer + written, (size_t)(got - written));
				if (n < 0 && errno == EINTR)
					continue;
				if (n <= 0)
					break;
				written += n;
			}
			if (written != got)
				break;
			have += got;
			growfile_progress(path, have);
			pthread_mutex_lock(&lock);
			active_bytes = have;
			active_ready = have >= PREBUFFER || have == total;
			pthread_mutex_unlock(&lock);
			if (!notified && (have >= PREBUFFER || have == total)) {
				post_ready(path);
				notified = true;
			}
		}
		disconnect_engine(fd);
		if (have == total) {
			ok = stamp_valid(stamp);
			break;
		}
	}
	if (output >= 0) {
		if (ok && fsync(output))
			ok = false;
		close(output);
	}
	if (ok) {
		int marker = openat(folder, "audio.ogg.complete", O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
		if (marker >= 0) {
			dprintf(marker, "%ld\n", total);
			fsync(marker);
			close(marker);
		}
	}
	if (announced)
		growfile_finish(path, ok);
	close(folder);
	close(cache);
	close(pin);
	if (ok && !notified)
		post_ready(path);
	return ok;
}

static void *cache_main(void *user) {
	(void)user;
	for (;;) {
		char path[512];
		pthread_mutex_lock(&lock);
		snprintf(path, sizeof(path), "%s", pending_path);
		pending_path[0] = 0;
		unsigned stamp = generation;
		download_sleep = pending_sleep;
		if (!path[0] || disabled) {
			cache_worker = false;
			active_path[0] = 0;
			pthread_cond_broadcast(&reaped);
			pthread_mutex_unlock(&lock);
			return NULL;
		}
		snprintf(active_path, sizeof(active_path), "%s", path);
		active_bytes = 0;
		active_ready = false;
		pthread_mutex_unlock(&lock);
		streamturn_acquire();
		bool ok = stamp_valid(stamp) && (completed(path) || fetch(path, stamp));
		streamturn_release();
		if (ok)
			post_prefetch(path);
		if (!ok && stamp_valid(stamp)) {
			char *copy = strdup(path);
			if (copy && !gui_post(failure_async, copy))
				free(copy);
		}
	}
}

static void enqueue_locked(const char *path) {
	snprintf(pending_path, sizeof(pending_path), "%s", path);
	pending_sleep = sleeptimer_expiration_serial();
	if (!cache_worker && !disabled) {
		pthread_t thread;
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		pthread_attr_setstacksize(&attr, 256 * 1024);
		cache_worker = pthread_create(&thread, &attr, cache_main, NULL) == 0;
		pthread_attr_destroy(&attr);
	}
}

static void prefetch_async(void *user) {
	char *finished = user;
	char current[512], next[512];
	int index = playlist_current_index();
	if (playlist_path_at(index, current, sizeof(current)))
		retain_queue_paths(current);
	bool valid = index >= 0 && playlist_path_at(index, current, sizeof(current)) && !strcmp(current, finished) && playlist_path_at(index + 1, next, sizeof(next)) && owns(next) && !completed(next);
	if (valid) {
		pthread_mutex_lock(&lock);
		if (!waiting && !disabled && strcmp(active_path, next))
			enqueue_locked(next);
		pthread_mutex_unlock(&lock);
	}
	free(finished);
}

static bool prepare(const char *path) {
	retain_queue_paths(path);
	if (!owns(path)) {
		pthread_mutex_lock(&lock);
		waiting = false;
		bool busy = cache_worker;
		pthread_mutex_unlock(&lock);
		if (busy)
			abandon();
		return true;
	}
	if (completed(path)) {
		spotify_resume();
		post_prefetch(path);
		return true;
	}
	spotify_resume();
	pthread_mutex_lock(&lock);
	bool ready = !strcmp(active_path, path) && active_ready && growfile_is_growing(path);
	if (ready) {
		pthread_mutex_unlock(&lock);
		return true;
	}
	bool same = cache_worker && !strcmp(active_path, path);
	pthread_mutex_unlock(&lock);
	device_state_stop();
	if (!same)
		streamturn_abandon_all();
	pthread_mutex_lock(&lock);
	waiting = true;
	waiting_sleep = sleeptimer_expiration_serial();
	if (!same)
		enqueue_locked(path);
	pthread_mutex_unlock(&lock);
	return false;
}

static cardimport_result_t import_credentials(const char lines[][CARDIMPORT_LINE_MAX], int count, char *message, size_t size) {
	char serialized[CARDIMPORT_MAX_BYTES + 1] = "";
	size_t used = 0;
	for (int i = 0; i < count; i++) {
		size_t n = strlen(lines[i]);
		if (used + n >= sizeof(serialized))
			return CARDIMPORT_INVALID;
		memcpy(serialized + used, lines[i], n);
		used += n;
		serialized[used] = 0;
	}
	json_doc_t doc;
	if (!json_parse(serialized, &doc))
		return CARDIMPORT_INVALID;
	int root = json_root(&doc);
	bool valid = json_get(&doc, root, "username") >= 0 && json_get(&doc, root, "auth_data") >= 0 && json_get(&doc, root, "auth_type") >= 0;
	json_free(&doc);
	if (!valid)
		return CARDIMPORT_INVALID;
	device_state_t state;
	device_state_get(&state);
	if ((owns(state.current_file) && state.status == AUDIO_STATUS_PLAYING) || downloading()) {
		snprintf(message, size, "%s", tr("spotify_import_stop"));
		return CARDIMPORT_OFFLINE;
	}
	spotify_stop();
	if (mkdir(directory, 0700) && errno != EEXIST)
		return CARDIMPORT_OFFLINE;
	char path[512], backup[512], temporary[512];
	snprintf(path, sizeof(path), "%s/credentials.json", directory);
	snprintf(backup, sizeof(backup), "%s/credentials.previous", directory);
	snprintf(temporary, sizeof(temporary), "%s/credentials.tmp", directory);
	struct stat st;
	if (lstat(directory, &st) || !S_ISDIR(st.st_mode) || (st.st_mode & 077))
		return CARDIMPORT_INVALID;
	bool had_previous = access(path, F_OK) == 0;
	if (access(backup, F_OK) == 0)
		return CARDIMPORT_OFFLINE;
	int fd = open(temporary, O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0)
		return CARDIMPORT_OFFLINE;
	size_t bytes = strlen(serialized);
	bool saved = write(fd, serialized, bytes) == (ssize_t)bytes && fsync(fd) == 0;
	close(fd);
	if (!saved || (had_previous && rename(path, backup))) {
		unlink(temporary);
		return CARDIMPORT_OFFLINE;
	}
	if (rename(temporary, path)) {
		if (had_previous)
			rename(backup, path);
		unlink(temporary);
		return CARDIMPORT_OFFLINE;
	}
	spotify_resume();
	char *response = spotify_request("authenticate", NULL, NULL, 0);
	bool ok = response && json_parse(response, &doc);
	char code[64] = "";
	if (ok) {
		ok = json_obj_bool(&doc, json_root(&doc), "ok", false);
		json_obj_str(&doc, json_root(&doc), "error", code, sizeof(code));
		json_free(&doc);
	}
	free(response);
	if (ok) {
		if (had_previous)
			unlink(backup);
		return CARDIMPORT_OK;
	}
	spotify_stop();
	unlink(path);
	if (had_previous)
		rename(backup, path);
	spotify_resume();
	snprintf(message, size, "%s", tr(spotify_error_key(code)));
	return !strcmp(code, "premium_required") ? CARDIMPORT_REFUSED : CARDIMPORT_OFFLINE;
}
