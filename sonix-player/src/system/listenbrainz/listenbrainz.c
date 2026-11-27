#define _GNU_SOURCE 1

#include "listenbrainz.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/device/sysinfo.h"
#include "src/system/device/system.h"
#include "src/system/device/usb.h"
#include "src/system/net/http.h"
#include "src/system/net/wifi.h"

#define API_URL "https://api.listenbrainz.org/1/submit-listens"
#define VALIDATE_URL "https://api.listenbrainz.org/1/validate-token"
#define CLIENT_NAME "Sonix Player"
#define API_TIMEOUT_SECS 15
#define BODY_LIMIT (64 * 1024)

#define BATCH_MAX 50
#define QUEUE_MAX 5000
#define PENDING_MAX 256
#define FIELD_MAX 256

#define CLOCK_VALID_AFTER 1577836800LL

#define RETRY_FIRST_SECS 30.0
#define RETRY_MAX_SECS 600.0
#define WAIT_POLL_SECS 10

#define SESSION_REFUSALS_MAX 3

typedef struct {
	long long when;
	double boot_start;
	int duration;
	char artist[FIELD_MAX];
	char title[FIELD_MAX];
	char album[FIELD_MAX];
} play_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake;
static bool started;

static bool enabled;
static char token[512];
static char user[128];
static bool session_expired;
static int session_refusals;
static bool refused;
static char detail[160];

static bool login_requested;
static bool logout_requested;
static bool logging_in;
static char login_token[512];
static unsigned login_serial;  // increments each login attempt, tells GUI thread that a new login attempt has happened
static listenbrainz_login_result_t login_result;

static play_t pending[PENDING_MAX];
static int pending_count;
static int file_count;

static bool now_playing_waiting;
static play_t now_playing;
static unsigned now_playing_generation;
static unsigned current_generation;
static bool current_playing;

static int requests_in_flight;

static char session_path[512];

static bool card_ready;
static char queue_path[600];
static char queue_tmp_path[610];

static double boot_seconds(void) {
	struct timespec ts;
#ifdef CLOCK_BOOTTIME
	if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0)
#endif
		clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static bool clock_valid(void) { return (long long)time(NULL) >= CLOCK_VALID_AFTER; }

static void wipe(void *p, size_t n) {
	volatile unsigned char *b = p;
	while (n--) {
		*b++ = 0;
	}
}

static void copy_field(char *dst, const char *src) { snprintf(dst, FIELD_MAX, "%s", src ? src : ""); }

static bool counting_locked(void) { return enabled && user[0] && (token[0] || session_expired); }

static bool online(void) {
	wifi_status_t st;
	wifi_get_status(&st);
	return st.state == WIFI_STATE_CONNECTED && st.ip[0];
}

static void build_paths(void) {
	char dir[400] = "/tmp";
	const char *cfg = config_path();
	if (cfg && cfg[0]) {
		const char *slash = strrchr(cfg, '/');
		if (slash == cfg) {
			snprintf(dir, sizeof(dir), "/");
		} else if (slash) {
			snprintf(dir, sizeof(dir), "%.*s", (int)(slash - cfg), cfg);
		} else {
			snprintf(dir, sizeof(dir), ".");
		}
	}
	snprintf(session_path, sizeof(session_path), "%s/listenbrainz.session", dir);
}

static void load_session(void) {
	FILE *f = fopen(session_path, "r");
	if (!f) {
		return;
	}
	char line[512];
	if (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		snprintf(token, sizeof(token), "%.*s", (int)sizeof(token) - 1, line);
	}
	if (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		snprintf(user, sizeof(user), "%.*s", (int)sizeof(user) - 1, line);
	}
	fclose(f);
	wipe(line, sizeof(line));
}

static void save_session(const char *key, const char *name) {
	if (!key[0] && !name[0]) {
		unlink(session_path);
		return;
	}
	char tmp[530];
	snprintf(tmp, sizeof(tmp), "%s.tmp", session_path);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		fprintf(stderr, "listenbrainz: cannot write %s: %s\n", tmp, strerror(errno));
		return;
	}
	chmod(tmp, 0600);
	fprintf(f, "%s\n%s\n", key, name);
	bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp, session_path) != 0) {
		fprintf(stderr, "listenbrainz: cannot write %s\n", session_path);
		unlink(tmp);
	}
}

#define QUEUE_HEADER "#listenbrainz\t"

static bool card_writable(void) { return storage_sd_root() && storage_sd_device() && storage_card_attached() && storage_sd_writable() && !usb_storage_active(); }

static void queue_escape(FILE *f, const char *s) {
	for (; *s; s++) {
		if (*s == '%' || *s == '\t' || *s == '\r' || *s == '\n') {
			fprintf(f, "%%%02X", (unsigned char)*s);
		} else {
			fputc(*s, f);
		}
	}
}

static int hex_digit(char c) {
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

static void queue_unescape(const char *from, const char *to, char *out) {
	size_t n = 0;
	while (from < to && n + 1 < FIELD_MAX) {
		int hi, lo;
		if (*from == '%' && to - from >= 3 && (hi = hex_digit(from[1])) >= 0 && (lo = hex_digit(from[2])) >= 0) {
			out[n++] = (char)(hi * 16 + lo);
			from += 3;
		} else {
			out[n++] = *from++;
		}
	}
	out[n] = '\0';
}

static bool queue_parse(const char *line, play_t *out) {
	const char *field[5];
	const char *end[5];
	const char *p = line;
	for (int i = 0; i < 5; i++) {
		field[i] = p;
		const char *stop = i < 4 ? strchr(p, '\t') : p + strcspn(p, "\r\n");
		if (!stop)
			return false;
		end[i] = stop;
		p = stop + 1;
	}

	memset(out, 0, sizeof(*out));
	char *num_end = NULL;
	out->when = strtoll(field[0], &num_end, 10);
	if (num_end != end[0] || out->when < CLOCK_VALID_AFTER)
		return false;
	out->duration = (int)strtol(field[1], &num_end, 10);
	if (num_end != end[1] || out->duration < 0)
		out->duration = 0;
	queue_unescape(field[2], end[2], out->artist);
	queue_unescape(field[3], end[3], out->title);
	queue_unescape(field[4], end[4], out->album);
	return out->artist[0] && out->title[0];
}

static bool queue_header_is(const char *line, const char *name) {
	size_t len = strlen(QUEUE_HEADER);
	if (strncmp(line, QUEUE_HEADER, len) != 0)
		return false;
	const char *who = line + len;
	size_t who_len = strcspn(who, "\r\n");
	return who_len == strlen(name) && strncasecmp(who, name, who_len) == 0;
}

static bool queue_attach(const char *name) {
	if (!card_writable())
		return false;
	char dir[520];
	snprintf(dir, sizeof(dir), "%s/.local", storage_sd_root());
	mkdir(dir, 0755);
	snprintf(queue_path, sizeof(queue_path), "%s/listenbrainz.queue", dir);
	snprintf(queue_tmp_path, sizeof(queue_tmp_path), "%s.tmp", queue_path);

	int plays = 0;
	FILE *f = fopen(queue_path, "r");
	if (f) {
		char *line = NULL;
		size_t cap = 0;
		bool mine = getline(&line, &cap, f) >= 0 && queue_header_is(line, name);
		while (mine && getline(&line, &cap, f) >= 0)
			plays++;
		free(line);
		fclose(f);
		if (!mine) {
			printf("listenbrainz: %s is not %s's; discarding it\n", queue_path, name);
			unlink(queue_path);
			plays = 0;
		}
	}
	pthread_mutex_lock(&lock);
	file_count = plays;
	pthread_mutex_unlock(&lock);
	return true;
}

static bool queue_append(const play_t *plays, int count, const char *name) {
	FILE *f = fopen(queue_path, "a");
	if (!f) {
		fprintf(stderr, "listenbrainz: cannot open %s: %s\n", queue_path, strerror(errno));
		return false;
	}
	if (ftell(f) == 0)
		fprintf(f, QUEUE_HEADER "%s\n", name);
	for (int i = 0; i < count; i++) {
		fprintf(f, "%lld\t%d\t", plays[i].when, plays[i].duration);
		queue_escape(f, plays[i].artist);
		fputc('\t', f);
		queue_escape(f, plays[i].title);
		fputc('\t', f);
		queue_escape(f, plays[i].album);
		fputc('\n', f);
	}
	bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
	ok = fclose(f) == 0 && ok;
	return ok;
}

static int queue_read_head(play_t *out, int max, int *lines_used) {
	*lines_used = 0;
	FILE *f = fopen(queue_path, "r");
	if (!f)
		return 0;
	char *line = NULL;
	size_t cap = 0;
	int count = 0;
	if (getline(&line, &cap, f) >= 0) {
		while (count < max && getline(&line, &cap, f) >= 0) {
			(*lines_used)++;
			if (queue_parse(line, &out[count]))
				count++;
		}
	}
	free(line);
	fclose(f);
	return count;
}

static void queue_drop_head(int lines) {
	FILE *in = fopen(queue_path, "r");
	if (!in)
		return;
	FILE *out = fopen(queue_tmp_path, "w");
	if (!out) {
		fclose(in);
		return;
	}
	char *line = NULL;
	size_t cap = 0;
	int n = -1;
	bool any = false;
	while (getline(&line, &cap, in) >= 0) {
		if (n < 0)
			fputs(line, out);
		else if (n >= lines) {
			fputs(line, out);
			any = true;
		}
		n++;
	}
	free(line);
	fclose(in);
	bool ok = fflush(out) == 0 && fsync(fileno(out)) == 0;
	ok = fclose(out) == 0 && ok;
	if (!ok || !any) {
		unlink(queue_tmp_path);
		if (ok)
			unlink(queue_path);
		return;
	}
	if (rename(queue_tmp_path, queue_path) != 0)
		unlink(queue_tmp_path);
}

typedef struct {
	char *buf;
	size_t len;
	size_t cap;
	bool failed;
} text_t;

static void text_add(text_t *t, const char *s, size_t n) {
	if (t->failed)
		return;
	if (t->len + n + 1 > t->cap) {
		size_t cap = t->cap ? t->cap : 1024;
		while (cap < t->len + n + 1)
			cap *= 2;
		char *grown = realloc(t->buf, cap);
		if (!grown) {
			t->failed = true;
			return;
		}
		t->buf = grown;
		t->cap = cap;
	}
	memcpy(t->buf + t->len, s, n);
	t->len += n;
	t->buf[t->len] = '\0';
}

static void text_str(text_t *t, const char *s) { text_add(t, s, strlen(s)); }

static void text_json(text_t *t, const char *s) {
	text_str(t, "\"");
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (*p == '"' || *p == '\\') {
			char esc[3] = {'\\', (char)*p, 0};
			text_str(t, esc);
		} else if (*p == '\b')
			text_str(t, "\\b");
		else if (*p == '\f')
			text_str(t, "\\f");
		else if (*p == '\n')
			text_str(t, "\\n");
		else if (*p == '\r')
			text_str(t, "\\r");
		else if (*p == '\t')
			text_str(t, "\\t");
		else if (*p < 0x20) {
			char esc[7];
			snprintf(esc, sizeof(esc), "\\u%04x", *p);
			text_str(t, esc);
		} else {
			char c[2] = {(char)*p, 0};
			text_str(t, c);
		}
	}
	text_str(t, "\"");
}

typedef enum {
	RESULT_OK,
	RESULT_RETRY,
	RESULT_SESSION,
	RESULT_BAD_REQUEST,
	RESULT_REFUSED,
} result_t;

// minimal JSON string field extractor: finds "key":"value" or "key": "value"
static bool json_string(const char *json, const char *key, char *out, size_t size) {
	char needle[64];
	snprintf(needle, sizeof(needle), "\"%s\"", key);
	const char *p = strstr(json, needle);
	if (!p)
		return false;
	p += strlen(needle);
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':') {
		if (*p == ':') {
			p++;
			break;
		}
		p++;
	}
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
		p++;
	if (*p != '"')
		return false;
	p++;
	size_t n = 0;
	while (*p && *p != '"' && n + 1 < size) {
		if (*p == '\\' && p[1]) {
			p++;
			if (*p == 'n')
				out[n++] = '\n';
			else if (*p == 'r')
				out[n++] = '\r';
			else if (*p == 't')
				out[n++] = '\t';
			else
				out[n++] = *p;
		} else {
			out[n++] = *p;
		}
		p++;
	}
	out[n] = '\0';
	return true;
}

static bool json_bool_true(const char *json, const char *key) {
	char needle[64];
	snprintf(needle, sizeof(needle), "\"%s\"", key);
	const char *p = strstr(json, needle);
	if (!p)
		return false;
	p += strlen(needle);
	while (*p && *p != ':')
		p++;
	if (*p == ':')
		p++;
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
		p++;
	return strncmp(p, "true", 4) == 0;
}

static result_t http_json(const char *url, const char *tok, const char *method, const char *payload, char *why, size_t why_size, char **body_out) {
	why[0] = '\0';
	char auth[600];
	snprintf(auth, sizeof(auth), "Authorization: Token %s\r\n", tok);

	http_req_t req = {
		.method = method,
		.extra_headers = auth,
		.body = payload,
		.body_len = payload ? strlen(payload) : 0,
		.content_type = payload ? "application/json" : NULL,
		.want_error_body = true,
		.allow_empty_body = true,
	};

	pthread_mutex_lock(&lock);
	requests_in_flight++;
	pthread_mutex_unlock(&lock);

	char *body = NULL;
	size_t body_len = 0;
	int status = 0;
	http_request(url, &req, &body, &body_len, BODY_LIMIT, API_TIMEOUT_SECS, &status);

	pthread_mutex_lock(&lock);
	requests_in_flight--;
	pthread_mutex_unlock(&lock);

	result_t result;
	if (!body) {
		const char *err = http_last_error();
		snprintf(why, why_size, "%s", (err && err[0]) ? err : "no reply");
		return RESULT_RETRY;
	}
	if (status >= 200 && status < 300) {
		result = RESULT_OK;
	} else if (status == 401) {
		json_string(body, "error", why, why_size);
		if (!why[0])
			snprintf(why, why_size, "invalid token");
		result = RESULT_SESSION;
	} else if (status == 400) {
		json_string(body, "error", why, why_size);
		if (!why[0])
			snprintf(why, why_size, "bad request");
		result = RESULT_BAD_REQUEST;
	} else if (status == 429 || status >= 500) {
		json_string(body, "error", why, why_size);
		if (!why[0])
			snprintf(why, why_size, "HTTP %d", status);
		result = RESULT_RETRY;
	} else {
		json_string(body, "error", why, why_size);
		if (!why[0])
			snprintf(why, why_size, "HTTP %d", status);
		result = RESULT_REFUSED;
	}

	if (result == RESULT_OK && body_out) {
		*body_out = body;
	} else {
		free(body);
	}
	return result;
}

static void do_login(char *tok) {
	char why[160];
	char *body = NULL;
	result_t req_result = http_json(VALIDATE_URL, tok, "GET", NULL, why, sizeof(why), &body);

	char name_out[128] = "";
	listenbrainz_login_result_t outcome;
	if (req_result == RESULT_OK) {
		if (!json_bool_true(body, "valid") || !json_string(body, "user_name", name_out, sizeof(name_out))) {
			// handle 200 response that says token is invalid
			snprintf(why, sizeof(why), "invalid token");
			outcome = LISTENBRAINZ_LOGIN_WRONG_CREDENTIALS;
		} else {
			// handle 200 response that says token is valid
			outcome = LISTENBRAINZ_LOGIN_OK;
		}
	} else if (req_result == RESULT_RETRY) {
		outcome = LISTENBRAINZ_LOGIN_NO_REPLY;
	} else if (req_result == RESULT_SESSION) {
		outcome = LISTENBRAINZ_LOGIN_WRONG_CREDENTIALS;
	} else {
		outcome = LISTENBRAINZ_LOGIN_REFUSED;
	}

	// save new session, if login was successful
	if (outcome == LISTENBRAINZ_LOGIN_OK) {
		save_session(tok, name_out);
		printf("listenbrainz: signed in as %s\n", name_out);
	} else {
		fprintf(stderr, "listenbrainz: token validation failed: %s\n", why);
	}

	free(body);  // clean up `body` pointer

	pthread_mutex_lock(&lock);
	logging_in = false;
	login_result = outcome;  // update global login_result variable

	login_serial++;
	if (outcome == LISTENBRAINZ_LOGIN_OK) {
		// update global state variables
		snprintf(token, sizeof(token), "%s", tok);
		snprintf(user, sizeof(user), "%s", name_out);
		session_expired = false;
		session_refusals = 0;
		refused = false;
		detail[0] = '\0';
	} else {
		snprintf(detail, sizeof(detail), "%s", why);
	}

	pthread_mutex_unlock(&lock);
	wipe(tok, strlen(tok));
}

static void do_logout(void) {
	save_session("", "");
	if (card_ready)
		unlink(queue_path);
	card_ready = false;
	pthread_mutex_lock(&lock);
	token[0] = '\0';
	user[0] = '\0';
	session_expired = false;
	refused = false;
	detail[0] = '\0';
	file_count = 0;
	pending_count = 0;
	now_playing_waiting = false;
	pthread_mutex_unlock(&lock);
	printf("listenbrainz: signed out\n");
}

static void session_lost(const char *why) {
	fprintf(stderr, "listenbrainz: token refused (%s); add it again\n", why);
	pthread_mutex_lock(&lock);
	token[0] = '\0';
	session_expired = true;
	session_refusals = 0;
	char name[128];
	snprintf(name, sizeof(name), "%s", user);
	pthread_mutex_unlock(&lock);
	save_session("", name);
}

static bool session_refused(const char *why) {
	pthread_mutex_lock(&lock);
	int count = ++session_refusals;
	pthread_mutex_unlock(&lock);
	if (count >= SESSION_REFUSALS_MAX) {
		session_lost(why);
		return true;
	}
	fprintf(stderr, "listenbrainz: token refused (%s), %d of %d; keeping it for now\n", why, count, SESSION_REFUSALS_MAX);
	return false;
}

static void session_accepted(void) {
	pthread_mutex_lock(&lock);
	session_refusals = 0;
	pthread_mutex_unlock(&lock);
}

static void flush_pending(const char *name) {
	bool valid = clock_valid();
	long long now_real = (long long)time(NULL);
	double now_boot = boot_seconds();

	pthread_mutex_lock(&lock);
	for (int i = 0; i < pending_count; i++) {
		if (!pending[i].when && valid)
			pending[i].when = now_real - (long long)(now_boot - pending[i].boot_start);
	}
	pthread_mutex_unlock(&lock);
	if (!card_ready)
		return;

	play_t *ready = malloc(sizeof(play_t) * PENDING_MAX);
	if (!ready)
		return;
	pthread_mutex_lock(&lock);
	int ready_count = 0;
	for (int i = 0; i < pending_count; i++)
		if (pending[i].when)
			ready[ready_count++] = pending[i];
	int room = QUEUE_MAX - file_count;
	pthread_mutex_unlock(&lock);
	if (!ready_count) {
		free(ready);
		return;
	}

	if (room < 0)
		room = 0;
	int kept = ready_count > room ? room : ready_count;
	if (kept < ready_count)
		fprintf(stderr, "listenbrainz: the queue is full; %d plays not kept\n", ready_count - kept);
	bool written = kept == 0 || queue_append(ready, kept, name);
	free(ready);
	if (!written)
		return;

	pthread_mutex_lock(&lock);
	file_count += kept;
	int keep = 0;
	for (int i = 0; i < pending_count; i++)
		if (!pending[i].when)
			pending[keep++] = pending[i];
	pending_count = keep;
	pthread_mutex_unlock(&lock);
}

// One listen. `listened_at` is left out for "playing_now", which must not
// carry it.
static void build_listen_json(text_t *t, const play_t *track, bool listened_at) {
	text_str(t, "{");
	if (listened_at) {
		char ts[32];
		snprintf(ts, sizeof(ts), "%lld", track->when);
		text_str(t, "\"listened_at\":");
		text_str(t, ts);
		text_str(t, ",");
	}
	text_str(t, "\"track_metadata\":{\"artist_name\":");
	text_json(t, track->artist);
	text_str(t, ",\"track_name\":");
	text_json(t, track->title);
	if (track->album[0]) {
		text_str(t, ",\"release_name\":");
		text_json(t, track->album);
	}
	text_str(t, ",\"additional_info\":{\"media_player\":\"" CLIENT_NAME "\",\"submission_client\":\"" CLIENT_NAME "\"");
	const char *version = sysinfo_os_version();
	if (version && version[0]) {
		text_str(t, ",\"submission_client_version\":");
		text_json(t, version);
	}
	if (track->duration > 0) {
		char num[32];
		snprintf(num, sizeof(num), "%d", track->duration);
		text_str(t, ",\"duration\":");
		text_str(t, num);
	}
	text_str(t, "}}}");
}

static void send_now_playing(const play_t *track, const char *tok) {
	text_t body = {0};
	text_str(&body, "{\"listen_type\":\"playing_now\",\"payload\":[");
	build_listen_json(&body, track, false);
	text_str(&body, "]}");
	char why[160];
	if (body.failed) {
		free(body.buf);
		fprintf(stderr, "listenbrainz: now playing not sent: out of memory\n");
		return;
	}
	result_t result = http_json(API_URL, tok, "POST", body.buf, why, sizeof(why), NULL);
	free(body.buf);
	if (result == RESULT_OK) {
		session_accepted();
		return;
	}
	fprintf(stderr, "listenbrainz: now playing not sent: %s\n", why);
	// A revoked token shows up here as well as on the queue: count it the same.
	if (result == RESULT_SESSION)
		session_refused(why);
}

static result_t send_batch(int max, const char *tok, char *why, size_t why_size, int *lines) {
	*lines = 0;
	why[0] = '\0';
	play_t *plays = malloc(sizeof(play_t) * (size_t)max);
	if (!plays) {
		snprintf(why, why_size, "out of memory");
		return RESULT_RETRY;
	}

	int count = queue_read_head(plays, max, lines);
	result_t result = RESULT_OK;
	if (count > 0) {
		text_t body = {0};
		text_str(&body, "{\"listen_type\":\"import\",\"payload\":[");
		for (int i = 0; i < count; i++) {
			if (i)
				text_str(&body, ",");
			build_listen_json(&body, &plays[i], true);
		}
		text_str(&body, "]}");
		if (body.failed) {
			snprintf(why, why_size, "out of memory");
			result = RESULT_RETRY;
		} else {
			result = http_json(API_URL, tok, "POST", body.buf, why, why_size, NULL);
			if (result == RESULT_OK)
				printf("listenbrainz: %d listens sent\n", count);
		}
		free(body.buf);
	}

	free(plays);
	return result;
}

typedef struct {
	int batch;
	// After a batch is refused as a bad request, its lines are sent one at a
	// time to find the bad one; this many are left before full batches again.
	int single_left;
	double backoff;
	double retry_at;
	bool was_online;
} sender_t;

static void send_queue(sender_t *s, const char *tok) {
	char why[160];
	int lines = 0;
	result_t result = send_batch(s->batch, tok, why, sizeof(why), &lines);
	bool drop = false;

	switch (result) {
	case RESULT_OK:
		drop = true;
		session_accepted();
		s->backoff = RETRY_FIRST_SECS;
		pthread_mutex_lock(&lock);
		refused = false;
		detail[0] = '\0';
		pthread_mutex_unlock(&lock);
		break;
	case RESULT_BAD_REQUEST:
		if (s->batch > 1) {
			s->batch = 1;
			s->single_left = lines;
		} else {
			fprintf(stderr, "listenbrainz: a queued listen was refused (%s); dropping it\n", why);
			drop = true;
		}
		break;
	case RESULT_SESSION:
		if (!session_refused(why)) {
			s->retry_at = boot_seconds() + s->backoff;
			s->backoff = s->backoff * 2 > RETRY_MAX_SECS ? RETRY_MAX_SECS : s->backoff * 2;
		}
		break;
	case RESULT_REFUSED:
		fprintf(stderr, "listenbrainz: listens refused: %s\n", why);
		pthread_mutex_lock(&lock);
		refused = true;
		snprintf(detail, sizeof(detail), "%s", why);
		pthread_mutex_unlock(&lock);
		s->retry_at = boot_seconds() + RETRY_MAX_SECS;
		break;
	case RESULT_RETRY:
		fprintf(stderr, "listenbrainz: listens not sent (%s); again in %.0f s\n", why, s->backoff);
		s->retry_at = boot_seconds() + s->backoff;
		s->backoff = s->backoff * 2 > RETRY_MAX_SECS ? RETRY_MAX_SECS : s->backoff * 2;
		break;
	}

	pthread_mutex_lock(&lock);
	if (lines == 0)
		file_count = 0;
	else if (drop)
		file_count = file_count > lines ? file_count - lines : 0;
	if (drop && s->batch == 1 && lines > 0) {
		s->single_left -= lines;
		if (s->single_left <= 0)
			s->batch = BATCH_MAX;
	}
	if (file_count == 0) {
		s->batch = BATCH_MAX;
		s->single_left = 0;
	}
	pthread_mutex_unlock(&lock);
	if (drop && lines > 0)
		queue_drop_head(lines);
}

static void *worker_main(void *arg) {
	(void)arg;
	sender_t sender = {.batch = BATCH_MAX, .backoff = RETRY_FIRST_SECS};

	for (;;) {
		pthread_mutex_lock(&lock);
		bool out = logout_requested;
		bool in = login_requested;
		char tok[512] = "";
		if (in) {
			snprintf(tok, sizeof(tok), "%s", login_token);
			wipe(login_token, sizeof(login_token));
		}
		logout_requested = false;
		login_requested = false;
		pthread_mutex_unlock(&lock);

		if (out)
			do_logout();
		if (in) {
			do_login(tok);
			wipe(tok, sizeof(tok));
			card_ready = false;
			sender.retry_at = 0;
			sender.backoff = RETRY_FIRST_SECS;
		}

		pthread_mutex_lock(&lock);
		bool active = enabled && token[0];
		bool counting = counting_locked();
		char tk[512];
		char account[128];
		snprintf(tk, sizeof(tk), "%s", token);
		snprintf(account, sizeof(account), "%s", user);
		pthread_mutex_unlock(&lock);

		bool up = false;
		if (counting) {
			if (!card_writable()) {
				if (card_ready) {
					card_ready = false;
					pthread_mutex_lock(&lock);
					file_count = 0;
					pthread_mutex_unlock(&lock);
				}
			} else if (!card_ready) {
				card_ready = queue_attach(account);
			}
			flush_pending(account);
		}
		if (active) {
			up = online();
			if (up && !sender.was_online) {
				sender.retry_at = 0;
				sender.backoff = RETRY_FIRST_SECS;
			}
			sender.was_online = up;

			if (up && boot_seconds() >= sender.retry_at) {
				pthread_mutex_lock(&lock);
				bool announce = false;
				play_t track;
				if (now_playing_waiting && (now_playing_generation != current_generation || !current_playing)) {
					now_playing_waiting = false;
				} else if (now_playing_waiting) {
					now_playing_waiting = false;
					announce = true;
					track = now_playing;
				}
				bool queued = file_count > 0;
				pthread_mutex_unlock(&lock);

				if (announce)
					send_now_playing(&track, tk);
				if (queued)
					send_queue(&sender, tk);
			}
		}

		pthread_mutex_lock(&lock);
		active = enabled && token[0];
		counting = counting_locked();
		bool more_now = active && up && file_count > 0 && boot_seconds() >= sender.retry_at;
		bool waiting = (counting && (!card_ready || pending_count > 0)) || (active && (file_count > 0 || now_playing_waiting));
		if (!logout_requested && !login_requested && !more_now) {
			if (waiting) {
				struct timespec ts;
				clock_gettime(CLOCK_MONOTONIC, &ts);
				ts.tv_sec += WAIT_POLL_SECS;
				pthread_cond_timedwait(&wake, &lock, &ts);
			} else {
				pthread_cond_wait(&wake, &lock);
			}
		}
		pthread_mutex_unlock(&lock);
	}
	return NULL;
}

void listenbrainz_init(void) {
	if (started)
		return;
	build_paths();

	pthread_condattr_t attr;
	pthread_condattr_init(&attr);
	pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
	pthread_cond_init(&wake, &attr);
	pthread_condattr_destroy(&attr);

	pthread_mutex_lock(&lock);
	enabled = config_get_bool("listenbrainz", "enabled", false);
	load_session();
	session_expired = !token[0] && user[0];
	pthread_mutex_unlock(&lock);

	pthread_t thread;
	if (pthread_create(&thread, NULL, worker_main, NULL) != 0) {
		fprintf(stderr, "listenbrainz: cannot start the worker\n");
		return;
	}
	pthread_detach(thread);
	started = true;
	printf("listenbrainz: %s, %s\n", enabled ? "on" : "off", token[0] ? "signed in" : "signed out");
}

bool listenbrainz_available(void) { return started; }

void listenbrainz_set_enabled(bool on) {
	config_set_bool("listenbrainz", "enabled", on);
	config_save();
	pthread_mutex_lock(&lock);
	enabled = on;
	pthread_cond_signal(&wake);
	pthread_mutex_unlock(&lock);
}

bool listenbrainz_active(void) {
	if (!started)
		return false;
	pthread_mutex_lock(&lock);
	bool counting = counting_locked();
	pthread_mutex_unlock(&lock);
	return counting;
}

bool listenbrainz_login(const char *tok) {
	if (!started || !tok)
		return false;
	// Typed by hand on the device: a stray space must not make it "invalid".
	while (isspace((unsigned char)*tok))
		tok++;
	size_t len = strlen(tok);
	while (len && isspace((unsigned char)tok[len - 1]))
		len--;
	if (!len || len >= sizeof(login_token))
		return false;
	pthread_mutex_lock(&lock);
	if (!logging_in) {
		snprintf(login_token, sizeof(login_token), "%.*s", (int)len, tok);
		logging_in = true;
		login_requested = true;
		pthread_cond_signal(&wake);
	}
	pthread_mutex_unlock(&lock);
	return true;
}

void listenbrainz_logout(void) {
	if (!started)
		return;
	pthread_mutex_lock(&lock);
	logout_requested = true;
	token[0] = '\0';
	session_expired = false;
	pthread_cond_signal(&wake);
	pthread_mutex_unlock(&lock);
}

void listenbrainz_get_status(listenbrainz_status_t *out) {
	memset(out, 0, sizeof(*out));
	pthread_mutex_lock(&lock);
	out->enabled = enabled;
	snprintf(out->user, sizeof(out->user), "%s", token[0] ? user : "");
	snprintf(out->detail, sizeof(out->detail), "%s", detail);
	out->queued = file_count + pending_count;
	out->login_serial = login_serial;
	out->login_result = login_result;
	if (!started)
		out->state = LISTENBRAINZ_STATE_UNAVAILABLE;
	else if (logging_in)
		out->state = LISTENBRAINZ_STATE_SIGNING_IN;
	else if (!enabled)
		out->state = LISTENBRAINZ_STATE_OFF;
	else if (session_expired)
		out->state = LISTENBRAINZ_STATE_SESSION_EXPIRED;
	else if (!token[0])
		out->state = LISTENBRAINZ_STATE_SIGNED_OUT;
	else if (refused)
		out->state = LISTENBRAINZ_STATE_REFUSED;
	else
		out->state = LISTENBRAINZ_STATE_CONNECTED;
	pthread_mutex_unlock(&lock);
}

bool listenbrainz_network_wanted(void) {
	pthread_mutex_lock(&lock);
	bool wanted = requests_in_flight > 0;
	pthread_mutex_unlock(&lock);
	return wanted;
}

// ---------------------------------------------------------------------------
// plays, as playcount.c follows them -- GUI thread only
// ---------------------------------------------------------------------------

static struct {
	unsigned generation;
	bool scrobbled;
	play_t track;
} play;

static void take_tags(const playcount_play_t *now) {
	copy_field(play.track.artist, now->artist);
	copy_field(play.track.title, now->title);
	copy_field(play.track.album, now->album);
}

void listenbrainz_note_play(const playcount_play_t *now) {
	bool announce = false;
	if (now->generation != play.generation) {
		play.generation = now->generation;
		play.scrobbled = false;
		memset(&play.track, 0, sizeof(play.track));
		take_tags(now);
		announce = true;
	} else if (!play.track.title[0] && now->title[0]) {
		// The tags came in after the file started.
		take_tags(now);
		announce = true;
	}
	play.track.when = now->real_start;
	play.track.boot_start = now->boot_start;
	if (now->duration > 0) {
		play.track.duration = now->duration;
	}
	bool named = !now->skip && play.track.artist[0] && play.track.title[0];
	bool scrobble = named && now->counts && !play.scrobbled;
	if (scrobble) {
		play.scrobbled = true;
	}

	pthread_mutex_lock(&lock);
	current_generation = now->generation;
	current_playing = now->playing;
	if (announce && token[0] && named) {
		now_playing = play.track;
		now_playing_generation = now->generation;
		now_playing_waiting = true;
		pthread_cond_signal(&wake);
	}
	if (scrobble && pending_count < PENDING_MAX) {
		pending[pending_count++] = play.track;
		pthread_cond_signal(&wake);
	}
	pthread_mutex_unlock(&lock);
}
