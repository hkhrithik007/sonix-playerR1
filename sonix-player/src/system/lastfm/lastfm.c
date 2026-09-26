#define _GNU_SOURCE 1

#include "lastfm.h"

#include "src/system/audio/audio.h"
#include "src/system/core/config.h"
#include "src/system/core/md5.h"
#include "src/system/net/http.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LASTFM_API_URL "https://ws.audioscrobbler.com/2.0/"
#define LASTFM_QUEUE_MAX_AGE (13LL * 24LL * 60LL * 60LL)
#define LASTFM_POLL_SECONDS 15
#define LASTFM_HTTP_TIMEOUT_SECONDS 15
#define LASTFM_BODY_LIMIT (256U * 1024U)
#define LASTFM_USER_AGENT "SonixPlayer/1.0 (Last.fm native scrobbler)"

#define LASTFM_TEXT_MAX 256
#define LASTFM_SESSION_MAX 128
#define LASTFM_API_KEY_MAX 128
#define LASTFM_API_SECRET_MAX 128
#define LASTFM_QUEUE_LINE_MAX 4096
#define LASTFM_SIGNATURE_MAX 4096
#define LASTFM_FORM_MAX 8192

#define LASTFM_CONFIG_SECTION "lastfm"

#define CFG_ENABLED "enabled"
#define CFG_API_KEY "api_key"
#define CFG_API_SECRET "api_secret"
#define CFG_SESSION_KEY "session_key"
#define CFG_USERNAME "username"

// ---------------------------------------------------------------------------
// Track state and worker state
// ---------------------------------------------------------------------------

typedef struct {
	time_t timestamp;
	int duration;
	char artist[LASTFM_TEXT_MAX];
	char title[LASTFM_TEXT_MAX];
	char album[LASTFM_TEXT_MAX];
} lastfm_track_t;

typedef enum {
	JOB_NONE = 0,
	JOB_LOGIN,
	JOB_NOW_PLAYING,
	JOB_SCROBBLE,
	JOB_SYNC_QUEUE,
} worker_job_kind_t;

static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t state_cond = PTHREAD_COND_INITIALIZER;
static pthread_t worker_thread;
static bool worker_started;
static bool worker_stop;

static bool enabled;
static bool logging_in;
static char api_key[LASTFM_API_KEY_MAX];
static char api_secret[LASTFM_API_SECRET_MAX];
static char session_key[LASTFM_SESSION_MAX];
static char username[LASTFM_TEXT_MAX];

static lastfm_track_t current_track;
static bool current_track_valid;
static bool scrobbled_this_track;
static bool scrobble_pending;
static bool scrobble_in_flight;
static lastfm_track_t pending_scrobble_track;
static uint64_t pending_scrobble_generation;
static uint64_t track_generation;

static bool now_playing_pending;
static lastfm_track_t now_playing_track;
static uint64_t now_playing_generation;

static bool login_pending;
static char login_username[LASTFM_TEXT_MAX];
static char login_password[LASTFM_TEXT_MAX];

static bool sync_pending;

// UI-thread persistence bridge. The worker never touches config_store.
static bool persist_session;
static bool clear_persisted_session;

static char last_message[192];
static uint64_t message_serial;
static uint64_t last_service_time;

static char queue_path[512];
static char queue_tmp_path[520];

static void set_message_locked(const char *message) {
	if (!message) {
		message = "";
	}
	snprintf(last_message, sizeof(last_message), "%s", message);
	message_serial++;
}


static void build_queue_paths(void) {
	const char *config_file = getenv("SONIX_CONFIG");
	if (!config_file || !config_file[0]) {
#ifndef HOST_BUILD
		config_file = "/usr/data/device_config.ini";
#else
		config_file = "./device_config.ini";
#endif
	}

	char dir[512];
	snprintf(dir, sizeof(dir), "%s", config_file);
	char *slash = strrchr(dir, '/');
	if (!slash) {
		snprintf(queue_path, sizeof(queue_path), "%s", ".lastfm_scrobbler_queue");
	} else {
		*slash = '\0';
		if (!dir[0]) {
			snprintf(dir, sizeof(dir), "/");
		}
		snprintf(queue_path, sizeof(queue_path), "%s/.lastfm_scrobbler_queue", dir);
	}
	snprintf(queue_tmp_path, sizeof(queue_tmp_path), "%s.tmp", queue_path);
}

static void copy_track(lastfm_track_t *dst, const song_metadata_t *metadata, time_t timestamp, int duration) {
	memset(dst, 0, sizeof(*dst));
	dst->timestamp = timestamp;
	dst->duration = duration > 0 ? duration : 0;
	if (!metadata) {
		return;
	}
	snprintf(dst->artist, sizeof(dst->artist), "%s", metadata->artist);
	snprintf(dst->title, sizeof(dst->title), "%s", metadata->title);
	snprintf(dst->album, sizeof(dst->album), "%s", metadata->album);
}

// ---------------------------------------------------------------------------
// Last.fm request signing and form encoding
// ---------------------------------------------------------------------------

typedef struct {
	const char *key;
	const char *value;
} sign_param_t;

static int sign_param_cmp(const void *a, const void *b) {
	const sign_param_t *pa = a;
	const sign_param_t *pb = b;
	return strcmp(pa->key, pb->key);
}

static bool append_form_pair(char *out, size_t out_size, size_t *used, const char *key, const char *value,
						 bool *first) {
	if (!out || !used || !key || !value) {
		return false;
	}
	char enc_key[1024];
	char enc_value[2048];
	http_url_encode(key, enc_key, sizeof(enc_key));
	http_url_encode(value, enc_value, sizeof(enc_value));

	int n = snprintf(out + *used, out_size - *used, "%s%s=%s", *first ? "" : "&", enc_key, enc_value);
	if (n < 0 || (size_t)n >= out_size - *used) {
		return false;
	}
	*used += (size_t)n;
	*first = false;
	return true;
}

static bool make_signature(const sign_param_t *params, size_t count, const char *secret, char out[MD5_HEX_LEN]) {
	if (!params || !secret || !out || count == 0 || count > 32) {
		return false;
	}

	sign_param_t sorted[32];
	memcpy(sorted, params, count * sizeof(sorted[0]));
	qsort(sorted, count, sizeof(sorted[0]), sign_param_cmp);

	char concat[LASTFM_SIGNATURE_MAX];
	size_t used = 0;
	for (size_t i = 0; i < count; i++) {
		int n = snprintf(concat + used, sizeof(concat) - used, "%s%s", sorted[i].key, sorted[i].value);
		if (n < 0 || (size_t)n >= sizeof(concat) - used) {
			return false;
		}
		used += (size_t)n;
	}
	int n = snprintf(concat + used, sizeof(concat) - used, "%s", secret);
	if (n < 0 || (size_t)n >= sizeof(concat) - used) {
		return false;
	}
	used += (size_t)n;

	md5_hex(concat, used, out);
	return true;
}

static bool build_form(char *out, size_t out_size, const sign_param_t *params, size_t count, const char *signature) {
	size_t used = 0;
	bool first = true;
	for (size_t i = 0; i < count; i++) {
		if (!append_form_pair(out, out_size, &used, params[i].key, params[i].value, &first)) {
			return false;
		}
	}
	return append_form_pair(out, out_size, &used, "api_sig", signature, &first);
}

static bool api_call_locked(const sign_param_t *method_params, size_t method_param_count, bool authenticated,
						char **body_out, int *status_out) {
	if (!body_out) {
		return false;
	}
	*body_out = NULL;
	if (status_out) {
		*status_out = 0;
	}

	char api_key_copy[LASTFM_API_KEY_MAX];
	char api_secret_copy[LASTFM_API_SECRET_MAX];
	char session_copy[LASTFM_SESSION_MAX];
	pthread_mutex_lock(&state_mutex);
	snprintf(api_key_copy, sizeof(api_key_copy), "%s", api_key);
	snprintf(api_secret_copy, sizeof(api_secret_copy), "%s", api_secret);
	snprintf(session_copy, sizeof(session_copy), "%s", session_key);
	pthread_mutex_unlock(&state_mutex);

	if (!api_key_copy[0] || !api_secret_copy[0]) {
		return false;
	}
	if (authenticated && !session_copy[0]) {
		return false;
	}

	sign_param_t params[16];
	size_t count = 0;
	for (size_t i = 0; i < method_param_count; i++) {
		params[count++] = method_params[i];
	}
	params[count++] = (sign_param_t){"api_key", api_key_copy};
	if (authenticated) {
		params[count++] = (sign_param_t){"sk", session_copy};
	}

	char signature[MD5_HEX_LEN];
	if (!make_signature(params, count, api_secret_copy, signature)) {
		return false;
	}

	char form[LASTFM_FORM_MAX];
	if (!build_form(form, sizeof(form), params, count, signature)) {
		return false;
	}

	http_req_t req = {
		.method = "POST",
		.extra_headers = "User-Agent: " LASTFM_USER_AGENT "\r\nAccept: application/xml\r\n",
		.body = form,
		.body_len = 0,
		.content_type = "application/x-www-form-urlencoded",
		.want_error_body = true,
		.allow_empty_body = false,
	};

	return http_request(LASTFM_API_URL, &req, body_out, NULL, LASTFM_BODY_LIMIT, LASTFM_HTTP_TIMEOUT_SECONDS, status_out);
}

static bool response_ok(const char *body) {
	return body && strstr(body, "<lfm status=\"ok\">") != NULL;
}

static void format_api_error(const char *body, int status, char *out, size_t out_size) {
	if (!out || out_size == 0) {
		return;
	}
	out[0] = '\0';
	if (body) {
		const char *start = strstr(body, "<error");
		if (start) {
			const char *code_start = strstr(start, "code=\"");
			const char *text_start = strchr(start, '>');
			const char *text_end = text_start ? strstr(text_start + 1, "</error>") : NULL;
			char code[24] = {0};
			char message[144] = {0};
			if (code_start) {
				code_start += 6;
				const char *q = strchr(code_start, '\"');
				if (q) {
					size_t n = (size_t)(q - code_start);
					if (n >= sizeof(code)) n = sizeof(code) - 1;
					memcpy(code, code_start, n);
					code[n] = '\0';
				}
			}
			if (text_start && text_end && text_end > text_start + 1) {
				size_t n = (size_t)(text_end - (text_start + 1));
				if (n >= sizeof(message)) n = sizeof(message) - 1;
				memcpy(message, text_start + 1, n);
				message[n] = '\0';
			}
			if (code[0] && message[0]) {
				snprintf(out, out_size, "Last.fm error %s: %s", code, message);
				return;
			}
			if (message[0]) {
				snprintf(out, out_size, "Last.fm: %s", message);
				return;
			}
		}
	}
	const char *net_error = http_last_error();
	if (net_error && net_error[0]) {
		snprintf(out, out_size, "Last.fm: %s", net_error);
		return;
	}
	if (status) {
		snprintf(out, out_size, "Last.fm: HTTP %d", status);
		return;
	}
	snprintf(out, out_size, "Last.fm request failed");
}

// ---------------------------------------------------------------------------
// Persistent queue. It is streamed line-by-line and never loaded wholesale.
// ---------------------------------------------------------------------------

static char *queue_decode_field(const char *encoded, char *out, size_t out_size) {
	size_t w = 0;
	for (size_t i = 0; encoded && encoded[i] && w + 1 < out_size; i++) {
		if (encoded[i] == '%' && encoded[i + 1] && encoded[i + 2]) {
			char hex[3] = {encoded[i + 1], encoded[i + 2], '\0'};
			char *end = NULL;
			long v = strtol(hex, &end, 16);
			if (end && *end == '\0') {
				out[w++] = (char)v;
				i += 2;
				continue;
			}
		}
		out[w++] = encoded[i];
	}
	out[w] = '\0';
	return out;
}

static bool parse_queue_line(const char *line, lastfm_track_t *out) {
	if (!line || !out) {
		return false;
	}
	char copy[LASTFM_QUEUE_LINE_MAX];
	snprintf(copy, sizeof(copy), "%s", line);
	char *fields[5] = {0};
	char *save = NULL;
	char *p = strtok_r(copy, "\t", &save);
	int count = 0;
	while (p && count < 5) {
		fields[count++] = p;
		p = strtok_r(NULL, "\t", &save);
	}
	if (count != 5 || !fields[0] || !fields[1]) {
		return false;
	}

	char *end = NULL;
	long long ts = strtoll(fields[0], &end, 10);
	if (!end || *end || ts <= 0) {
		return false;
	}
	long duration = strtol(fields[1], &end, 10);
	if (!end || *end || duration < 0) {
		duration = 0;
	}

	memset(out, 0, sizeof(*out));
	out->timestamp = (time_t)ts;
	out->duration = (int)duration;
	queue_decode_field(fields[2], out->artist, sizeof(out->artist));
	queue_decode_field(fields[3], out->title, sizeof(out->title));
	queue_decode_field(fields[4], out->album, sizeof(out->album));
	return out->artist[0] && out->title[0];
}

static bool queue_write_field(FILE *f, const char *value) {
	char enc[2048];
	http_url_encode(value ? value : "", enc, sizeof(enc));
	return fputs(enc, f) >= 0;
}

static bool queue_write_item(FILE *f, const lastfm_track_t *item) {
	if (!f || !item || !item->artist[0] || !item->title[0] || item->timestamp <= 0) {
		return false;
	}
	if (fprintf(f, "%lld\t%d\t", (long long)item->timestamp, item->duration) < 0) {
		return false;
	}
	if (!queue_write_field(f, item->artist) || fputc('\t', f) == EOF ||
		!queue_write_field(f, item->title) || fputc('\t', f) == EOF ||
		!queue_write_field(f, item->album) || fputc('\n', f) == EOF) {
		return false;
	}
	return true;
}

static bool prune_queue(void) {
	FILE *in = fopen(queue_path, "r");
	if (!in) {
		return true;
	}
	FILE *out = fopen(queue_tmp_path, "w");
	if (!out) {
		fclose(in);
		return false;
	}

	long long cutoff = (long long)time(NULL) - LASTFM_QUEUE_MAX_AGE;
	char line[LASTFM_QUEUE_LINE_MAX];
	while (fgets(line, sizeof(line), in)) {
		lastfm_track_t item;
		if (parse_queue_line(line, &item) && (long long)item.timestamp >= cutoff) {
			if (!queue_write_item(out, &item)) {
				fclose(in);
				fclose(out);
				remove(queue_tmp_path);
				return false;
			}
		}
	}
	fclose(in);
	if (fclose(out) != 0) {
		remove(queue_tmp_path);
		return false;
	}
	if (rename(queue_tmp_path, queue_path) != 0) {
		remove(queue_tmp_path);
		return false;
	}
	return true;
}

static bool enqueue_scrobble(const lastfm_track_t *item) {
	if (!item || !item->artist[0] || !item->title[0] || item->timestamp <= 0) {
		return false;
	}
	prune_queue();
	FILE *f = fopen(queue_path, "a");
	if (!f) {
		return false;
	}
	bool ok = queue_write_item(f, item);
	fclose(f);
	return ok;
}

static bool read_first_queued(lastfm_track_t *item) {
	if (!item) {
		return false;
	}
	FILE *f = fopen(queue_path, "r");
	if (!f) {
		return false;
	}
	char line[LASTFM_QUEUE_LINE_MAX];
	while (fgets(line, sizeof(line), f)) {
		if (parse_queue_line(line, item)) {
			fclose(f);
			return true;
		}
	}
	fclose(f);
	return false;
}

static bool remove_first_queued(void) {
	FILE *in = fopen(queue_path, "r");
	if (!in) {
		return true;
	}
	FILE *out = fopen(queue_tmp_path, "w");
	if (!out) {
		fclose(in);
		return false;
	}

	bool removed = false;
	char line[LASTFM_QUEUE_LINE_MAX];
	while (fgets(line, sizeof(line), in)) {
		lastfm_track_t item;
		if (!removed && parse_queue_line(line, &item)) {
			removed = true;
			continue;
		}
		if (fputs(line, out) == EOF) {
			fclose(in);
			fclose(out);
			remove(queue_tmp_path);
			return false;
		}
	}
	fclose(in);
	if (fclose(out) != 0) {
		remove(queue_tmp_path);
		return false;
	}
	if (rename(queue_tmp_path, queue_path) != 0) {
		remove(queue_tmp_path);
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Worker operations
// ---------------------------------------------------------------------------

static bool do_login_request(const char *user, const char *password, char *error, size_t error_size) {
	if (!user || !*user || !password || !*password) {
		snprintf(error, error_size, "Username and password are required");
		return false;
	}

	sign_param_t params[3] = {
		{"method", "auth.getMobileSession"},
		{"password", password},
		{"username", user},
	};
	char *body = NULL;
	int status = 0;
	if (!api_call_locked(params, 3, false, &body, &status) || !response_ok(body)) {
		format_api_error(body, status, error, error_size);
		free(body);
		return false;
	}

	const char *key_start = strstr(body, "<key>");
	const char *key_end = key_start ? strstr(key_start + 5, "</key>") : NULL;
	if (!key_start || !key_end || key_end <= key_start + 5) {
		snprintf(error, error_size, "Last.fm login returned no session key");
		free(body);
		return false;
	}

	char key[LASTFM_SESSION_MAX];
	size_t key_len = (size_t)(key_end - (key_start + 5));
	if (key_len >= sizeof(key)) {
		key_len = sizeof(key) - 1;
	}
	memcpy(key, key_start + 5, key_len);
	key[key_len] = '\0';

	char normalized_user[LASTFM_TEXT_MAX];
	const char *name_start = strstr(body, "<name>");
	const char *name_end = name_start ? strstr(name_start + 6, "</name>") : NULL;
	if (name_start && name_end && name_end > name_start + 6) {
		size_t name_len = (size_t)(name_end - (name_start + 6));
		if (name_len >= sizeof(normalized_user)) name_len = sizeof(normalized_user) - 1;
		memcpy(normalized_user, name_start + 6, name_len);
		normalized_user[name_len] = '\0';
	} else {
		snprintf(normalized_user, sizeof(normalized_user), "%s", user);
	}

	pthread_mutex_lock(&state_mutex);
	snprintf(session_key, sizeof(session_key), "%s", key);
	snprintf(username, sizeof(username), "%s", normalized_user);
	logging_in = false;
	persist_session = true;
	clear_persisted_session = false;
	char message[192];
	snprintf(message, sizeof(message), "Logged in to Last.fm as %s", username);
	snprintf(last_message, sizeof(last_message), "%s", message);
	message_serial++;
	pthread_mutex_unlock(&state_mutex);

	free(body);
	return true;
}

static bool do_now_playing(const lastfm_track_t *track) {
	if (!track || !track->artist[0] || !track->title[0]) {
		return false;
	}

	char duration[32];
	snprintf(duration, sizeof(duration), "%d", track->duration);
	char album_value[LASTFM_TEXT_MAX];
	snprintf(album_value, sizeof(album_value), "%s", track->album);

	sign_param_t params[5];
	size_t count = 0;
	params[count++] = (sign_param_t){"artist", track->artist};
	params[count++] = (sign_param_t){"duration", duration};
	if (album_value[0]) {
		params[count++] = (sign_param_t){"album", album_value};
	}
	params[count++] = (sign_param_t){"method", "track.updateNowPlaying"};
	params[count++] = (sign_param_t){"track", track->title};

	char *body = NULL;
	int status = 0;
	bool ok = api_call_locked(params, count, true, &body, &status) && response_ok(body);
	free(body);
	(void)status;
	return ok;
}

static bool do_scrobble(const lastfm_track_t *track) {
	if (!track || !track->artist[0] || !track->title[0] || track->timestamp <= 0) {
		return false;
	}

	char duration[32];
	char timestamp[32];
	snprintf(duration, sizeof(duration), "%d", track->duration);
	snprintf(timestamp, sizeof(timestamp), "%lld", (long long)track->timestamp);

	sign_param_t params[8];
	size_t count = 0;
	params[count++] = (sign_param_t){"artist", track->artist};
	if (track->album[0]) {
		params[count++] = (sign_param_t){"album", track->album};
	}
	params[count++] = (sign_param_t){"duration", duration};
	params[count++] = (sign_param_t){"method", "track.scrobble"};
	params[count++] = (sign_param_t){"timestamp", timestamp};
	params[count++] = (sign_param_t){"track", track->title};

	char *body = NULL;
	int status = 0;
	bool ok = api_call_locked(params, count, true, &body, &status) && response_ok(body);
	free(body);
	(void)status;
	return ok;
}

static bool lastfm_is_send_enabled(void) {
	bool allowed;
	pthread_mutex_lock(&state_mutex);
	allowed = enabled && session_key[0];
	pthread_mutex_unlock(&state_mutex);
	return allowed;
}

static bool do_sync_queue(void) {
	lastfm_track_t item;
	if (!lastfm_is_send_enabled()) {
		return true;
	}
	prune_queue();
	if (!read_first_queued(&item)) {
		return true;
	}
	if (!do_scrobble(&item)) {
		return false;
	}
	return remove_first_queued();
}

static void worker_mark_scrobble_complete(uint64_t generation) {
	pthread_mutex_lock(&state_mutex);
	if (generation == track_generation) {
		scrobble_pending = false;
		scrobbled_this_track = true;
	} 
	pthread_mutex_unlock(&state_mutex);
}

static void worker_mark_scrobble_queued(uint64_t generation) {
	pthread_mutex_lock(&state_mutex);
	if (generation == track_generation) {
		scrobble_pending = false;
		scrobbled_this_track = true;
	}
	pthread_mutex_unlock(&state_mutex);
}

static void *lastfm_worker(void *unused) {
	(void)unused;
	for (;;) {
		worker_job_kind_t job = JOB_NONE;
		lastfm_track_t track;
		uint64_t generation = 0;
		char login_user[LASTFM_TEXT_MAX];
		char login_pass[LASTFM_TEXT_MAX];

		memset(&track, 0, sizeof(track));
		memset(login_user, 0, sizeof(login_user));
		memset(login_pass, 0, sizeof(login_pass));

		pthread_mutex_lock(&state_mutex);
		while (!worker_stop && !login_pending && !now_playing_pending && !sync_pending && !scrobble_pending) {
			pthread_cond_wait(&state_cond, &state_mutex);
		}
		if (worker_stop) {
			pthread_mutex_unlock(&state_mutex);
			break;
		}

		if (login_pending) {
			job = JOB_LOGIN;
			snprintf(login_user, sizeof(login_user), "%s", login_username);
			snprintf(login_pass, sizeof(login_pass), "%s", login_password);
			memset(login_password, 0, sizeof(login_password));
			login_pending = false;
		} else if (now_playing_pending) {
			job = JOB_NOW_PLAYING;
			track = now_playing_track;
			generation = now_playing_generation;
			now_playing_pending = false;
		} else if (sync_pending) {
			job = JOB_SYNC_QUEUE;
			sync_pending = false;
		} else if (scrobble_pending) {
			job = JOB_SCROBBLE;
			track = pending_scrobble_track;
			generation = pending_scrobble_generation;
			scrobble_pending = false;
			scrobble_in_flight = true;
		}
		pthread_mutex_unlock(&state_mutex);

		switch (job) {
		case JOB_LOGIN: {
			char error[192];
			bool ok = do_login_request(login_user, login_pass, error, sizeof(error));
			memset(login_pass, 0, sizeof(login_pass));
			if (!ok) {
				pthread_mutex_lock(&state_mutex);
				logging_in = false;
				set_message_locked(error);
				pthread_mutex_unlock(&state_mutex);
			}
			break;
		}
		case JOB_NOW_PLAYING:
			// Advisory. The source plugin deliberately ignores failures here.
			if (lastfm_is_send_enabled()) {
				do_now_playing(&track);
			}
			break;
		case JOB_SCROBBLE: {
			bool allowed = lastfm_is_send_enabled();
			bool ok = allowed && do_scrobble(&track);
			if (ok) {
				// The direct request is complete only after Last.fm accepted it.
				worker_mark_scrobble_complete(generation);
			} else if (allowed) {
				// Only cache a failed attempt while the service is still enabled.
				// If the user disabled Last.fm while the request was in flight, leave
				// the play eligible for a later retry rather than silently queueing it.
				bool queued = enqueue_scrobble(&track);
				if (queued) {
					// Identical policy to the plugin: once a failed direct request has
					// been safely written to disk, do not attempt the same play again.
					worker_mark_scrobble_queued(generation);
				}
			}
			pthread_mutex_lock(&state_mutex);
			scrobble_in_flight = false;
			pthread_mutex_unlock(&state_mutex);
			break;
		}
		case JOB_SYNC_QUEUE:
			// Queue sync failures are silent. The next 15-second poll retries the
			// exact original record with its original timestamp.
			do_sync_queue();
			break;
		default:
			break;
		}
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void lastfm_init(void) {
	pthread_mutex_lock(&state_mutex);
	if (worker_started) {
		pthread_mutex_unlock(&state_mutex);
		return;
	}

	enabled = config_get_bool(LASTFM_CONFIG_SECTION, CFG_ENABLED, false);
	snprintf(api_key, sizeof(api_key), "%s", config_get(LASTFM_CONFIG_SECTION, CFG_API_KEY, ""));
	snprintf(api_secret, sizeof(api_secret), "%s", config_get(LASTFM_CONFIG_SECTION, CFG_API_SECRET, ""));
	snprintf(session_key, sizeof(session_key), "%s", config_get(LASTFM_CONFIG_SECTION, CFG_SESSION_KEY, ""));
	snprintf(username, sizeof(username), "%s", config_get(LASTFM_CONFIG_SECTION, CFG_USERNAME, ""));
	build_queue_paths();

	worker_stop = false;
	if (pthread_create(&worker_thread, NULL, lastfm_worker, NULL) != 0) {
		set_message_locked("Could not start Last.fm worker");
		pthread_mutex_unlock(&state_mutex);
		return;
	}
	worker_started = true;
	pthread_mutex_unlock(&state_mutex);
}

void lastfm_on_track_started(const song_metadata_t *metadata) {
	if (!metadata || !metadata->artist[0] || !metadata->title[0]) {
		return;
	}

	pthread_mutex_lock(&state_mutex);
	if (!worker_started) {
		pthread_mutex_unlock(&state_mutex);
		return;
	}

	copy_track(&current_track, metadata, time(NULL), 0);
	current_track_valid = true;
	scrobbled_this_track = false;
	scrobble_pending = false;
	track_generation++;

	if (enabled && session_key[0]) {
		double ignored_position = 0.0, total = 0.0;
		audio_get_progress(&ignored_position, &total);
		if (total > 0.0) {
			current_track.duration = (int)(total + 0.5);
		}
		now_playing_track = current_track;
		now_playing_generation = track_generation;
		now_playing_pending = true;
		sync_pending = true;
		pthread_cond_signal(&state_cond);
	}
	pthread_mutex_unlock(&state_mutex);
}

void lastfm_poll(void) {
	// Called by the LVGL/UI timer. This is the only place that persists worker
	// results into config, keeping config.c strictly on the UI/main thread.
	bool do_persist = false;
	bool do_clear = false;
	uint64_t now = (uint64_t)time(NULL);

	pthread_mutex_lock(&state_mutex);
	if (persist_session) {
		persist_session = false;
		do_persist = true;
	}
	if (clear_persisted_session) {
		clear_persisted_session = false;
		do_clear = true;
	}

	if (enabled && session_key[0]) {
		if (last_service_time == 0 || now - last_service_time >= LASTFM_POLL_SECONDS) {
			last_service_time = now;
			sync_pending = true;
		}

		if (current_track_valid && !scrobbled_this_track && !scrobble_pending && !scrobble_in_flight) {
			double position = 0.0;
			double total = 0.0;
			audio_status_t status = audio_get_status();
			audio_get_progress(&position, &total);
			if (total > 0.0) {
				current_track.duration = (int)(total + 0.5);
			}
			int duration = current_track.duration;
			if (status == AUDIO_STATUS_PLAYING && duration >= 30) {
				double threshold = duration / 2.0;
				if (threshold > 240.0) threshold = 240.0;
				if (position >= threshold) {
					pending_scrobble_track = current_track;
					pending_scrobble_generation = track_generation;
					scrobble_pending = true;
				}
			}
		}
	}
	pthread_mutex_unlock(&state_mutex);

	if (do_persist) {
		char session_copy[LASTFM_SESSION_MAX];
		char username_copy[LASTFM_TEXT_MAX];
		pthread_mutex_lock(&state_mutex);
		snprintf(session_copy, sizeof(session_copy), "%s", session_key);
		snprintf(username_copy, sizeof(username_copy), "%s", username);
		pthread_mutex_unlock(&state_mutex);
		config_set(LASTFM_CONFIG_SECTION, CFG_SESSION_KEY, session_copy);
		config_set(LASTFM_CONFIG_SECTION, CFG_USERNAME, username_copy);
		config_save();
	}
	if (do_clear) {
		config_set(LASTFM_CONFIG_SECTION, CFG_SESSION_KEY, "");
		config_set(LASTFM_CONFIG_SECTION, CFG_USERNAME, "");
		config_save();
	}
	pthread_mutex_lock(&state_mutex);
	bool wake = login_pending || now_playing_pending || sync_pending || scrobble_pending;
	if (wake) {
		pthread_cond_signal(&state_cond);
	}
	pthread_mutex_unlock(&state_mutex);
}

void lastfm_set_enabled(bool value) {
	pthread_mutex_lock(&state_mutex);
	enabled = value;
	config_set_bool(LASTFM_CONFIG_SECTION, CFG_ENABLED, enabled);
	config_save();
	if (enabled && session_key[0]) {
		sync_pending = true;
		pthread_cond_signal(&state_cond);
	}
	pthread_mutex_unlock(&state_mutex);
}

static void set_credential(const char *key_name, char *dst, size_t dst_size, const char *value) {
	pthread_mutex_lock(&state_mutex);
	snprintf(dst, dst_size, "%s", value ? value : "");
	// The API credentials are part of the session identity. Changing either one
	// invalidates the old session for this application, so force a fresh login.
	session_key[0] = '\0';
	username[0] = '\0';
	logging_in = false;
	config_set(LASTFM_CONFIG_SECTION, key_name, dst);
	config_set(LASTFM_CONFIG_SECTION, CFG_SESSION_KEY, "");
	config_set(LASTFM_CONFIG_SECTION, CFG_USERNAME, "");
	config_save();
	set_message_locked(dst[0] ? "Last.fm credentials updated; please log in again" : "Last.fm credential cleared");
	pthread_mutex_unlock(&state_mutex);
}

void lastfm_set_api_key(const char *value) { set_credential(CFG_API_KEY, api_key, sizeof(api_key), value); }

void lastfm_set_api_secret(const char *value) {
	set_credential(CFG_API_SECRET, api_secret, sizeof(api_secret), value);
}

void lastfm_login(const char *user, const char *password) {
	pthread_mutex_lock(&state_mutex);
	if (!api_key[0] || !api_secret[0]) {
		set_message_locked("Set the Last.fm API key and API secret first");
		pthread_mutex_unlock(&state_mutex);
		return;
	}
	if (logging_in || login_pending) {
		pthread_mutex_unlock(&state_mutex);
		return;
	}
	snprintf(login_username, sizeof(login_username), "%s", user ? user : "");
	snprintf(login_password, sizeof(login_password), "%s", password ? password : "");
	logging_in = true;
	clear_persisted_session = false;
	login_pending = true;
	set_message_locked("Logging in to Last.fm...");
	pthread_cond_signal(&state_cond);
	pthread_mutex_unlock(&state_mutex);
}

void lastfm_logout(void) {
	pthread_mutex_lock(&state_mutex);
	session_key[0] = '\0';
	username[0] = '\0';
	logging_in = false;
	login_pending = false;
	memset(login_password, 0, sizeof(login_password));
	clear_persisted_session = true;
	set_message_locked("Logged out of Last.fm");
	pthread_mutex_unlock(&state_mutex);

	// Persist on the UI/main thread on the next lastfm_poll().
}

void lastfm_get_snapshot(lastfm_snapshot_t *out) {
	if (!out) {
		return;
	}
	memset(out, 0, sizeof(*out));

	pthread_mutex_lock(&state_mutex);
	out->enabled = enabled;
	out->logged_in = session_key[0] != '\0';
	out->logging_in = logging_in;
	out->api_key_configured = api_key[0] != '\0';
	out->api_secret_configured = api_secret[0] != '\0';
	snprintf(out->username, sizeof(out->username), "%s", username);
	snprintf(out->last_message, sizeof(out->last_message), "%s", last_message);
	out->message_serial = message_serial;

	if (!enabled) {
		snprintf(out->status, sizeof(out->status), "Disabled");
	} else if (!api_key[0] || !api_secret[0]) {
		snprintf(out->status, sizeof(out->status), "API key and secret required");
	} else if (logging_in) {
		snprintf(out->status, sizeof(out->status), "Logging in...");
	} else if (!session_key[0]) {
		snprintf(out->status, sizeof(out->status), "Not logged in");
	} else if (username[0]) {
		snprintf(out->status, sizeof(out->status), "Connected as %s", username);
	} else {
		snprintf(out->status, sizeof(out->status), "Connected");
	}
	pthread_mutex_unlock(&state_mutex);
}
