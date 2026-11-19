#define _GNU_SOURCE 1

#include "listenbrainz.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/core/md5.h"
#include "src/system/net/http.h"
#include "src/system/net/wifi.h"

#define LB_URL "https://api.listenbrainz.org/1/submit-listens"
#define LB_VALIDATE_URL "https://api.listenbrainz.org/1/validate-token"
#define LB_QUEUE_MAX 40
#define LB_FIELD_MAX 256
#define LB_BODY_MAX 2048
#define LB_RETRY_MAX 600

typedef struct {
	long long listened_at;
	int duration;
	char account[MD5_HEX_LEN];
	char artist[LB_FIELD_MAX];
	char title[LB_FIELD_MAX];
	char album[LB_FIELD_MAX];
} listen_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake;
static bool started, enabled, token_checking, token_valid, logout_requested;
static bool token_request_pending, refused, now_playing_waiting;
static bool queue_dirty;
static char token[256], new_token[256], user[128], detail[160];
static unsigned serial;
static unsigned validation_generation;
static listen_t queue[LB_QUEUE_MAX];
static listen_t now_playing;
static int queue_count;
static int requests_in_flight;
static char token_path[512], queue_path[512], queue_tmp[520];

static double monotonic_seconds(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static bool online(void) {
	wifi_status_t st;
	wifi_get_status(&st);
	return st.state == WIFI_STATE_CONNECTED && st.ip[0];
}

static void paths_init(void) {
	char dir[400] = "/tmp";
	const char *cfg = config_path();
	if (cfg && cfg[0]) {
		const char *slash = strrchr(cfg, '/');
		if (slash == cfg) snprintf(dir, sizeof(dir), "/");
		else if (slash) snprintf(dir, sizeof(dir), "%.*s", (int)(slash - cfg), cfg);
		else snprintf(dir, sizeof(dir), ".");
	}
	snprintf(token_path, sizeof(token_path), "%s/listenbrainz.token", dir);
	snprintf(queue_path, sizeof(queue_path), "%s/listenbrainz.queue", dir);
	snprintf(queue_tmp, sizeof(queue_tmp), "%s.tmp", queue_path);
}

static void json_string(char *out, size_t cap, const char *s) {
	size_t n = 0;
	if (!cap) return;
	out[n++] = '"';
	for (; s && *s && n + 8 < cap; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = (char)c; }
		else if (c == '\n') { out[n++] = '\\'; out[n++] = 'n'; }
		else if (c == '\r') { out[n++] = '\\'; out[n++] = 'r'; }
		else if (c == '\t') { out[n++] = '\\'; out[n++] = 't'; }
		else if (c >= 0x20) out[n++] = (char)c;
	}
	out[n++] = '"';
	out[n] = '\0';
}

static void field_copy(char *to, size_t cap, const char *from) {
	if (!cap) return;
	snprintf(to, cap, "%s", from ? from : "");
	for (size_t i = 0; to[i]; i++) {
		if (to[i] == '\n' || to[i] == '\r' || to[i] == '\t') to[i] = ' ';
	}
}

// Queue fields are hex encoded so delimiters, newlines and non-ASCII tags are
// preserved in this small, atomically replaced file.
static void hex_write(FILE *f, const char *s) {
	static const char hex[] = "0123456789abcdef";
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		fputc(hex[c >> 4], f); fputc(hex[c & 15], f);
	}
}

static int hex_value(int c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static bool hex_read(char **p, char *out, size_t cap) {
	size_t n = 0;
	while (**p && **p != '\t' && **p != '\n' && **p != '\r') {
		int hi = hex_value((*p)[0]), lo = hex_value((*p)[1]);
		if (hi < 0 || lo < 0 || !(*p)[1] || n + 1 >= cap) return false;
		out[n++] = (char)((hi << 4) | lo);
		*p += 2;
	}
	out[n] = '\0';
	if (**p == '\t') (*p)++;
	return true;
}

static bool queue_save_locked(void) {
	FILE *f = fopen(queue_tmp, "w");
	if (!f) return false;
	for (int i = 0; i < queue_count; i++) {
		listen_t *x = &queue[i];
		fprintf(f, "%lld\t%d\t%s\t", x->listened_at, x->duration, x->account);
		hex_write(f, x->artist); fputc('\t', f);
		hex_write(f, x->title); fputc('\t', f);
		hex_write(f, x->album); fputc('\n', f);
	}
	bool ok = !ferror(f) && fflush(f) == 0 && fsync(fileno(f)) == 0;
	if (fclose(f) != 0) ok = false;
	if (ok && chmod(queue_tmp, 0600) != 0) ok = false;
	if (ok && rename(queue_tmp, queue_path) != 0) ok = false;
	if (!ok) unlink(queue_tmp);
	return ok;
}

static void queue_load(void) {
	FILE *f = fopen(queue_path, "r");
	if (!f) return;
	chmod(queue_path, 0600);
	char line[LB_FIELD_MAX * 6 + 256];
	while (queue_count < LB_QUEUE_MAX && fgets(line, sizeof(line), f)) {
		listen_t x = {0};
		char *p = line, *end;
		x.listened_at = strtoll(p, &end, 10);
		if (end == p || *end != '\t') continue;
		p = end + 1;
		long duration = strtol(p, &end, 10);
		if (end == p || *end != '\t' || duration < 1 || duration > 2073600) continue;
		x.duration = (int)duration; p = end + 1;
		char *tab = strchr(p, '\t');
		if (!tab || (size_t)(tab - p) != 32) continue;
		memcpy(x.account, p, 32); x.account[32] = 0; p = tab + 1;
		if (!hex_read(&p, x.artist, sizeof(x.artist)) || !hex_read(&p, x.title, sizeof(x.title)) ||
			!hex_read(&p, x.album, sizeof(x.album)) || !x.listened_at || !x.artist[0] || !x.title[0]) continue;
		queue[queue_count++] = x;
	}
	fclose(f);
}

static bool token_save(const char *value) {
	char tmp[520];
	snprintf(tmp, sizeof(tmp), "%s.tmp", token_path);
	FILE *f = fopen(tmp, "w");
	if (!f) return false;
	bool ok = fprintf(f, "%s\n", value) > 0 && fflush(f) == 0 && fsync(fileno(f)) == 0;
	if (fclose(f) != 0) ok = false;
	if (ok && (chmod(tmp, 0600) != 0 || rename(tmp, token_path) != 0)) ok = false;
	if (!ok) unlink(tmp);
	return ok;
}

static bool token_shape_ok(const char *value) {
	size_t len = value ? strlen(value) : 0;
	if (len < 10 || len > 200) return false;
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)value[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
			c == '-' || c == '.' || c == '_' || c == '~')) return false;
	}
	return true;
}

static void token_load(void) {
	FILE *f = fopen(token_path, "r");
	if (!f) return;
	if (fgets(token, sizeof(token), f)) {
		token[strcspn(token, "\r\n")] = 0;
		if (!token_shape_ok(token)) token[0] = 0;
	}
	fclose(f);
	chmod(token_path, 0600);
}

static bool validate_token(const char *value, char *user_out, size_t user_cap) {
	char auth[320];
	snprintf(auth, sizeof(auth), "Authorization: Token %s\r\n", value);
	char *body = NULL;
	int status = 0;
	bool ok = http_get_ex(LB_VALIDATE_URL, auth, true, &body, NULL, 8192, 15, &status);
	bool valid = ok && status >= 200 && status < 300 && body &&
		(strstr(body, "\"valid\": true") || strstr(body, "\"valid\":true"));
	if (valid && user_out && user_cap) {
		char *p = strstr(body, "\"user_name\"");
		if (p && (p = strchr(p, ':'))) {
			p = strchr(p, '"');
			if (p) {
				p++;
				size_t n = 0;
				while (*p && *p != '"' && n + 1 < user_cap) {
					if (*p == '\\' && p[1]) p++;
					user_out[n++] = *p++;
				}
				user_out[n] = 0;
			}
		}
	}
	free(body);
	return valid;
}

static bool submit_listen(const listen_t *x, const char *auth_token, bool playing_now, int *status_out) {
	char artist[LB_FIELD_MAX * 2 + 4], title[LB_FIELD_MAX * 2 + 4], album[LB_FIELD_MAX * 2 + 4];
	char body[LB_BODY_MAX];
	json_string(artist, sizeof(artist), x->artist);
	json_string(title, sizeof(title), x->title);
	json_string(album, sizeof(album), x->album);
	int n;
	if (playing_now) {
		n = snprintf(body, sizeof(body),
			"{\"listen_type\":\"playing_now\",\"payload\":[{\"track_metadata\":{" 
			"\"artist_name\":%s,\"track_name\":%s,\"release_name\":%s,\"additional_info\":{" 
			"\"duration_ms\":%d,\"submission_client\":\"Rage-os\",\"submission_client_version\":\"1.0\"}}}]}",
			artist, title, album, x->duration * 1000);
	} else {
		n = snprintf(body, sizeof(body),
			"{\"listen_type\":\"single\",\"payload\":[{\"listened_at\":%lld,\"track_metadata\":{" 
			"\"artist_name\":%s,\"track_name\":%s,\"release_name\":%s,\"additional_info\":{" 
			"\"duration_ms\":%d,\"submission_client\":\"Rage-os\",\"submission_client_version\":\"1.0\"}}}]}",
			x->listened_at, artist, title, album, x->duration * 1000);
	}
	if (n < 0 || (size_t)n >= sizeof(body)) return false;
	char headers[384];
	snprintf(headers, sizeof(headers), "Authorization: Token %s\r\n", auth_token);
	http_req_t req = {.method = "POST", .extra_headers = headers, .body = body,
		.content_type = "application/json", .want_error_body = true, .allow_empty_body = true};
	char *response = NULL;
	int status = 0;
	bool ok = http_request(LB_URL, &req, &response, NULL, 16384, 20, &status);
	free(response);
	if (status_out) *status_out = status;
	return ok && status >= 200 && status < 300;
}

static void *worker(void *unused) {
	(void)unused;
	unsigned failures = 0;
	for (;;) {
		pthread_mutex_lock(&lock);
		bool check = token_request_pending;
		unsigned check_generation = validation_generation;
		char candidate[256];
		candidate[0] = 0;
		if (check) {
			snprintf(candidate, sizeof(candidate), "%s", new_token);
			token_request_pending = false;
			token_checking = true;
		}
		bool logout = logout_requested;
		logout_requested = false;
		if (logout) {
			validation_generation++;
			char old_hash[MD5_HEX_LEN]; md5_hex(token, strlen(token), old_hash);
			token[0] = user[0] = detail[0] = 0;
			token_valid = enabled = refused = false;
			token_checking = token_request_pending = false;
			now_playing_waiting = false;
			unlink(token_path);
			for (int i = queue_count - 1; i >= 0; i--) {
				if (!strcmp(queue[i].account, old_hash)) queue[i] = queue[--queue_count];
			}
			queue_dirty = !queue_save_locked();
		}
		if (queue_dirty && queue_save_locked()) queue_dirty = false;
		bool active = enabled && token_valid && token[0] && online();
		listen_t item = {0};
		bool have_item = false;
		listen_t now_item = {0};
		bool send_now = false;
		char token_copy[256];
		snprintf(token_copy, sizeof(token_copy), "%s", token);
		char account[MD5_HEX_LEN] = "";
		if (active) {
			md5_hex(token_copy, strlen(token_copy), account);
			if (now_playing_waiting && !strcmp(now_playing.account, account)) {
				now_item = now_playing;
				now_playing_waiting = false;
				send_now = true;
			}
			for (int i = 0; i < queue_count; i++) {
				if (!strcmp(queue[i].account, account)) { item = queue[i]; have_item = true; break; }
			}
		}
		pthread_mutex_unlock(&lock);

		if (check) {
			char checked_user[128] = "";
			bool has_network = online();
			bool valid = false;
			if (has_network) {
				pthread_mutex_lock(&lock); requests_in_flight++; pthread_mutex_unlock(&lock);
				valid = validate_token(candidate, checked_user, sizeof(checked_user));
				pthread_mutex_lock(&lock); requests_in_flight--; pthread_mutex_unlock(&lock);
			}
			pthread_mutex_lock(&lock);
			if (!has_network && check_generation == validation_generation) {
				token_request_pending = true;
				token_checking = true;
			} else if (check_generation == validation_generation) {
				if (valid && token_save(candidate)) {
					snprintf(token, sizeof(token), "%s", candidate);
					snprintf(user, sizeof(user), "%s", checked_user);
					token_valid = true; refused = false; detail[0] = 0;
				} else {
					token_valid = false; refused = true;
					snprintf(detail, sizeof(detail), "ListenBrainz rejected the token");
				}
				token_checking = false;
				serial++;
			}
			bool retry = !has_network && check_generation == validation_generation;
			pthread_mutex_unlock(&lock);
			if (retry) {
				struct timespec retry_at;
				clock_gettime(CLOCK_REALTIME, &retry_at);
				retry_at.tv_sec += 10;
				pthread_mutex_lock(&lock);
				pthread_cond_timedwait(&wake, &lock, &retry_at);
				pthread_mutex_unlock(&lock);
				continue;
			}
		}
		if (have_item) {
			pthread_mutex_lock(&lock); requests_in_flight++; pthread_mutex_unlock(&lock);
			int status = 0;
			bool sent = submit_listen(&item, token_copy, false, &status);
			pthread_mutex_lock(&lock);
			requests_in_flight--;
			if (sent) {
				for (int i = 0; i < queue_count; i++) {
					if (!strcmp(queue[i].account, account) && queue[i].listened_at == item.listened_at &&
						!strcmp(queue[i].title, item.title) && !strcmp(queue[i].artist, item.artist)) {
						queue[i] = queue[--queue_count];
						queue_dirty = true;
						if (queue_save_locked()) queue_dirty = false;
						else if (queue_count < LB_QUEUE_MAX) queue[queue_count++] = item;
						break;
					}
				}
				failures = 0; refused = false; detail[0] = 0;
			} else {
				failures++;
				if (status == 401) {
					token_valid = false; refused = true;
					snprintf(detail, sizeof(detail), "ListenBrainz token expired; set it again");
				} else if (failures >= 3) {
					refused = true; snprintf(detail, sizeof(detail), "ListenBrainz submission failed; queued listens are retained");
				}
			}
			pthread_mutex_unlock(&lock);
		}
		if (send_now) {
			pthread_mutex_lock(&lock); requests_in_flight++; pthread_mutex_unlock(&lock);
			int status = 0;
			submit_listen(&now_item, token_copy, true, &status);
			pthread_mutex_lock(&lock);
			requests_in_flight--;
			if (status == 401) {
				token_valid = false; refused = true;
				snprintf(detail, sizeof(detail), "ListenBrainz token expired; set it again");
			}
			pthread_mutex_unlock(&lock);
		}

		pthread_mutex_lock(&lock);
		if (token_request_pending || logout_requested) {
			pthread_mutex_unlock(&lock);
			continue;
		}
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += (have_item && failures == 0) ? 1 : (failures ? (failures > 5 ? LB_RETRY_MAX : 15 << (failures - 1)) : 10);
		pthread_cond_timedwait(&wake, &lock, &ts);
		pthread_mutex_unlock(&lock);
	}
	return NULL;
}

static bool play_enabled(void) {
	bool on;
	pthread_mutex_lock(&lock); on = started && enabled && token_valid; pthread_mutex_unlock(&lock);
	return on;
}

void listenbrainz_init(void) {
	if (started) return;
	paths_init(); token_load(); queue_load();
	enabled = config_get_bool("listenbrainz", "enabled", false);
	if (token[0]) {
		// Validate in the worker before enabling submissions, without blocking startup.
		snprintf(new_token, sizeof(new_token), "%s", token);
		token_request_pending = true;
		token_checking = true;
		validation_generation++;
	}
	pthread_cond_init(&wake, NULL);
	pthread_t thread;
	if (pthread_create(&thread, NULL, worker, NULL) != 0) {
		token_checking = token_request_pending = false;
		refused = true;
		snprintf(detail, sizeof(detail), "ListenBrainz worker could not start");
		return;
	}
	pthread_detach(thread); started = true;
}

void listenbrainz_set_enabled(bool on) {
	config_set_bool("listenbrainz", "enabled", on); config_save();
	pthread_mutex_lock(&lock); enabled = on; pthread_cond_signal(&wake); pthread_mutex_unlock(&lock);
}

void listenbrainz_set_token(const char *value) {
	if (!token_shape_ok(value)) return;
	pthread_mutex_lock(&lock);
	snprintf(new_token, sizeof(new_token), "%s", value);
	token_valid = false;
	now_playing_waiting = false;
	validation_generation++;
	token_request_pending = true; token_checking = true; refused = false; detail[0] = 0;
	pthread_cond_signal(&wake); pthread_mutex_unlock(&lock);
}

void listenbrainz_logout(void) {
	pthread_mutex_lock(&lock); logout_requested = true; pthread_cond_signal(&wake); pthread_mutex_unlock(&lock);
}

void listenbrainz_get_status(listenbrainz_status_t *out) {
	if (!out) return;
	memset(out, 0, sizeof(*out));
	pthread_mutex_lock(&lock);
	out->enabled = enabled; out->queued = queue_count; out->serial = serial;
	out->last_token_valid = token_valid;
	snprintf(out->user, sizeof(out->user), "%s", user);
	snprintf(out->detail, sizeof(out->detail), "%s", detail);
	if (token_checking) out->state = LISTENBRAINZ_CHECKING;
	else if (refused) out->state = LISTENBRAINZ_REFUSED;
	else if (!enabled) out->state = LISTENBRAINZ_OFF;
	else out->state = token_valid ? LISTENBRAINZ_CONNECTED : LISTENBRAINZ_TOKEN_REQUIRED;
	pthread_mutex_unlock(&lock);
}

bool listenbrainz_network_wanted(void) {
	bool wanted;
	pthread_mutex_lock(&lock);
	wanted = requests_in_flight > 0;
	if (!wanted && enabled && token_valid && token[0]) {
		char account[MD5_HEX_LEN];
		md5_hex(token, strlen(token), account);
		wanted = now_playing_waiting && !strcmp(now_playing.account, account);
		for (int i = 0; !wanted && i < queue_count; i++) {
			wanted = !strcmp(queue[i].account, account);
		}
	}
	pthread_mutex_unlock(&lock);
	return wanted;
}

static struct {
	char file[512];
	char artist[LB_FIELD_MAX], title[LB_FIELD_MAX], album[LB_FIELD_MAX];
	int duration;
	double listened, last_tick;
	double last_position;
	long long started_at;
	bool was_playing, skip, submitted, now_sent;
} play;

static void qualify(void) {
	if (play.submitted || play.skip || !play.artist[0] || !play.title[0] || play.duration < 30) return;
	double enough = play.duration / 2.0;
	if (enough > 240) enough = 240;
	if (play.listened < enough || play.started_at <= 0) return;
	listen_t x = {.listened_at = play.started_at, .duration = play.duration};
	field_copy(x.artist, sizeof(x.artist), play.artist);
	field_copy(x.title, sizeof(x.title), play.title);
	field_copy(x.album, sizeof(x.album), play.album);
	pthread_mutex_lock(&lock);
	if (enabled && token_valid && queue_count < LB_QUEUE_MAX) {
		md5_hex(token, strlen(token), x.account);
		queue[queue_count++] = x;
		queue_dirty = true;
		pthread_cond_signal(&wake);
		play.submitted = true;
	} else if (enabled && token_valid) {
		play.submitted = true;
		refused = true;
		snprintf(detail, sizeof(detail), "ListenBrainz queue is full; new listens are skipped");
	}
	pthread_mutex_unlock(&lock);
}

static void announce_now_playing(void) {
	if (play.now_sent || play.skip || !play.artist[0] || !play.title[0] || play.duration < 30) return;
	pthread_mutex_lock(&lock);
	if (enabled && token_valid && token[0]) {
		now_playing = (listen_t){.duration = play.duration};
		field_copy(now_playing.artist, sizeof(now_playing.artist), play.artist);
		field_copy(now_playing.title, sizeof(now_playing.title), play.title);
		field_copy(now_playing.album, sizeof(now_playing.album), play.album);
		md5_hex(token, strlen(token), now_playing.account);
		now_playing_waiting = true;
		play.now_sent = true;
		pthread_cond_signal(&wake);
	}
	pthread_mutex_unlock(&lock);
}

void listenbrainz_note_playback(const listenbrainz_playback_t *now) {
	if (!now || !play_enabled()) { play.file[0] = 0; return; }
	double t = monotonic_seconds();
	const char *file = now->file ? now->file : "";
	bool same = play.file[0] && strcmp(play.file, file) == 0;
	bool again = same && play.last_position > now->position + 10 && now->position < 5;
	if (now->playing && file[0] && (!same || again)) {
		if (play.file[0]) {
			if (play.was_playing) {
				double elapsed = t - play.last_tick;
				if (elapsed > 0 && elapsed < 5) play.listened += elapsed;
			}
			qualify();
		}
		bool skip = now->skip;
		memset(&play, 0, sizeof(play));
		snprintf(play.file, sizeof(play.file), "%s", file);
		play.started_at = time(NULL);
		field_copy(play.artist, sizeof(play.artist), now->artist);
		field_copy(play.title, sizeof(play.title), now->title);
		field_copy(play.album, sizeof(play.album), now->album);
		play.duration = (int)(now->duration + .5);
		play.skip = skip;
	} else if (!same) {
		play.file[0] = 0; return;
	} else {
		if (!play.title[0] && now->title) field_copy(play.title, sizeof(play.title), now->title);
		if (!play.artist[0] && now->artist) field_copy(play.artist, sizeof(play.artist), now->artist);
		if (!play.album[0] && now->album) field_copy(play.album, sizeof(play.album), now->album);
		if (now->duration > 0) play.duration = (int)(now->duration + .5);
		play.skip = now->skip;
	}
	announce_now_playing();
	if (now->playing && play.was_playing) {
		double elapsed = t - play.last_tick;
		if (elapsed > 0 && elapsed < 5) play.listened += elapsed;
	}
	play.was_playing = now->playing;
	play.last_tick = t;
	play.last_position = now->position;
	qualify();
}
