#define _GNU_SOURCE 1

#include "lastfm.h"

#include "src/gui/shell/gui.h"
#include "src/system/audio/audio.h"
#include "src/system/core/config.h"
#include "src/system/core/md5.h"
#include "src/system/device/system.h"
#include "src/system/net/http.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LASTFM_API_URL "https://ws.audioscrobbler.com/2.0/"
#define LASTFM_QUEUE_MAX_AGE (13LL * 24LL * 60LL * 60LL)
#define LASTFM_POLL_SECONDS 15
#define LASTFM_HTTP_TIMEOUT_SECONDS 15
#define LASTFM_BODY_LIMIT (256U * 1024U)

#define LASTFM_TEXT_MAX 256
#define LASTFM_SESSION_MAX 128
#define LASTFM_API_KEY_MAX 128
#define LASTFM_API_SECRET_MAX 128

#define LASTFM_CONFIG_SECTION "lastfm"
#define CFG_ENABLED "enabled"
#define CFG_API_KEY "api_key"
#define CFG_API_SECRET "api_secret"
#define CFG_SESSION_KEY "session_key"
#define CFG_USERNAME "username"

#define LASTFM_QUEUE_DIR ".plugins"
#define LASTFM_QUEUE_NAME ".lastfm_scrobbler_queue"
#define LASTFM_QUEUE_TMP_NAME ".lastfm_scrobbler_queue.tmp"

typedef struct {
    time_t timestamp;
    double duration;
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
} worker_job_t;

typedef struct {
    worker_job_t kind;
    lastfm_track_t track;
    unsigned generation;
    char username[LASTFM_TEXT_MAX];
    char password[LASTFM_TEXT_MAX];
} job_t;

typedef struct {
    char text[192];
    bool persist_login;
    bool persist_logout;
    char session_key[LASTFM_SESSION_MAX];
    char username[LASTFM_TEXT_MAX];
} ui_result_t;

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

static char login_username[LASTFM_TEXT_MAX];
static char login_password[LASTFM_TEXT_MAX];
static bool login_pending;

static lastfm_track_t current_track;
static bool current_track_valid;
static unsigned current_generation;
static bool scrobbled_this_track;
static bool scrobble_in_flight;
static bool now_playing_sent;

static char queue_path[512];
static char queue_tmp_path[512];

static char last_message[192];
static unsigned long long message_serial;

/* ------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* ------------------------------------------------------------------------- */

static void set_message_locked(const char *message) {
    snprintf(last_message, sizeof(last_message), "%s", message ? message : "");
    message_serial++;
}

static bool send_enabled_locked(void) {
    return enabled && api_key[0] && api_secret[0] && session_key[0];
}

static void copy_track(lastfm_track_t *dst, const song_metadata_t *m, time_t timestamp, double duration) {
    memset(dst, 0, sizeof(*dst));
    dst->timestamp = timestamp;
    dst->duration = duration > 0 ? duration : 0;
    snprintf(dst->artist, sizeof(dst->artist), "%s", m->artist);
    snprintf(dst->title, sizeof(dst->title), "%s", m->title);
    snprintf(dst->album, sizeof(dst->album), "%s", m->album);
}

/* Standard application/x-www-form-urlencoded encoding. */
static size_t form_encode(const char *in, char *out, size_t out_size) {
    static const char HEX[] = "0123456789ABCDEF";
    size_t used = 0;
    if (!in || !out || out_size == 0) {
        return 0;
    }

    for (const unsigned char *p = (const unsigned char *)in; *p; ++p) {
        unsigned char c = *p;
        bool keep = isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~';
        size_t need = keep ? 1 : 3;
        if (used + need + 1 >= out_size) {
            out[0] = '\0';
            return 0;
        }
        if (keep) {
            out[used++] = (char)c;
        } else {
            out[used++] = '%';
            out[used++] = HEX[c >> 4];
            out[used++] = HEX[c & 0x0F];
        }
    }

    out[used] = '\0';
    return used;
}

typedef struct {
    const char *key;
    const char *value;
} param_t;

static int param_cmp(const void *a, const void *b) {
    const param_t *pa = (const param_t *)a;
    const param_t *pb = (const param_t *)b;
    return strcmp(pa->key, pb->key);
}

/*
 * Last.fm's signature is:
 *   sort params by key
 *   concatenate key + value without separators
 *   append shared secret
 *   MD5
 */
static bool make_signature(const param_t *params, size_t count, const char *secret, char out[MD5_HEX_LEN]) {
    param_t sorted[16];
    if (count > sizeof(sorted) / sizeof(sorted[0])) {
        return false;
    }

    memcpy(sorted, params, count * sizeof(sorted[0]));
    qsort(sorted, count, sizeof(sorted[0]), param_cmp);

    char concat[4096];
    size_t used = 0;

    for (size_t i = 0; i < count; ++i) {
        size_t kl = strlen(sorted[i].key);
        size_t vl = strlen(sorted[i].value);
        if (used + kl + vl + strlen(secret) + 1 >= sizeof(concat)) {
            return false;
        }
        memcpy(concat + used, sorted[i].key, kl);
        used += kl;
        memcpy(concat + used, sorted[i].value, vl);
        used += vl;
    }

    size_t secret_len = strlen(secret);
    memcpy(concat + used, secret, secret_len);
    used += secret_len;
    concat[used] = '\0';

    md5_hex(concat, used, out);
    return true;
}

static bool build_form(const param_t *params, size_t count, char *out, size_t out_size) {
    size_t used = 0;
    char encoded[1024];

    for (size_t i = 0; i < count; ++i) {
        if (i != 0) {
            if (used + 1 >= out_size) {
                return false;
            }
            out[used++] = '&';
        }

        if (used + strlen(params[i].key) + 1 >= out_size) {
            return false;
        }
        memcpy(out + used, params[i].key, strlen(params[i].key));
        used += strlen(params[i].key);
        out[used++] = '=';

        if (form_encode(params[i].value, encoded, sizeof(encoded)) == 0 && params[i].value[0]) {
            return false;
        }
        size_t el = strlen(encoded);
        if (used + el + 1 >= out_size) {
            return false;
        }
        memcpy(out + used, encoded, el);
        used += el;
    }

    out[used] = '\0';
    return true;
}

static bool api_call(const char *key, const char *secret, const param_t *base_params, size_t base_count,
                      char **body_out, int *status_out) {
    param_t params[16];
    if (!key || !secret || base_count + 2 > sizeof(params) / sizeof(params[0])) {
        return false;
    }

    memcpy(params, base_params, base_count * sizeof(params[0]));
    params[base_count++] = (param_t){"api_key", key};

    char sig[MD5_HEX_LEN];
    if (!make_signature(params, base_count, secret, sig)) {
        return false;
    }
    params[base_count++] = (param_t){"api_sig", sig};

    char form[8192];
    if (!build_form(params, base_count, form, sizeof(form))) {
        return false;
    }

    http_req_t req = {
        .method = "POST",
        .extra_headers = NULL,
        .body = form,
        .body_len = strlen(form),
        .content_type = "application/x-www-form-urlencoded",
        .want_error_body = true,
        .allow_empty_body = false,
    };

    return http_request(LASTFM_API_URL, &req, body_out, NULL, LASTFM_BODY_LIMIT,
                        LASTFM_HTTP_TIMEOUT_SECONDS, status_out);
}

static bool xml_ok(const char *body) {
    return body && strstr(body, "status=\"ok\"") != NULL;
}

static bool xml_error(const char *body, char *out, size_t out_size) {
    if (!body || !out || out_size == 0) {
        return false;
    }

    const char *start = strstr(body, "<error");
    if (!start) {
        return false;
    }
    start = strchr(start, '>');
    if (!start) {
        return false;
    }
    start++;

    const char *end = strstr(start, "</error>");
    if (!end || end <= start) {
        return false;
    }

    size_t len = (size_t)(end - start);
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

static bool xml_key(const char *body, char *out, size_t out_size) {
    if (!body || !out || out_size == 0) {
        return false;
    }
    const char *start = strstr(body, "<key>");
    if (!start) {
        return false;
    }
    start += 5;
    const char *end = strstr(start, "</key>");
    if (!end || end <= start) {
        return false;
    }

    size_t len = (size_t)(end - start);
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, start, len);
    out[len] = '\0';
    return out[0] != '\0';
}

static void queue_paths_init(void) {
    queue_path[0] = '\0';
    queue_tmp_path[0] = '\0';

    const char *root = storage_sd_root();
#ifdef HOST_BUILD
    if ((!root || !root[0])) {
        root = getenv("SONIX_SD_ROOT");
    }
#endif
    if (!root || !root[0]) {
        return;
    }

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/%s", root, LASTFM_QUEUE_DIR);
    mkdir(dir, 0755); /* okay if it already exists */

    snprintf(queue_path, sizeof(queue_path), "%s/%s", dir, LASTFM_QUEUE_NAME);
    snprintf(queue_tmp_path, sizeof(queue_tmp_path), "%s/%s", dir, LASTFM_QUEUE_TMP_NAME);
}

static bool queue_line_parse(const char *line, lastfm_track_t *out) {
    if (!line || !out) {
        return false;
    }

    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", line);

    char *fields[5] = {0};
    char *p = tmp;
    for (int i = 0; i < 5; ++i) {
        fields[i] = p;
        char *tab = strchr(p, '\t');
        if (!tab) {
            if (i == 4) {
                break;
            }
            return false;
        }
        *tab = '\0';
        p = tab + 1;
    }

    if (!fields[0] || !fields[1] || !fields[2] || !fields[3] || !fields[4]) {
        return false;
    }

    char decoded[5][LASTFM_TEXT_MAX];
    for (int i = 0; i < 3; ++i) {
        /* Percent-decode the strings. */
        size_t used = 0;
        const char *src = fields[i + 2];
        while (*src && used + 1 < sizeof(decoded[i])) {
            if (src[0] == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
                char h[3] = {src[1], src[2], '\0'};
                decoded[i][used++] = (char)strtol(h, NULL, 16);
                src += 3;
            } else {
                decoded[i][used++] = *src++;
            }
        }
        decoded[i][used] = '\0';
    }

    char *endptr = NULL;
    errno = 0;
    long long ts = strtoll(fields[0], &endptr, 10);
    if (errno != 0 || endptr == fields[0] || ts <= 0) {
        return false;
    }

    errno = 0;
    double duration = strtod(fields[1], &endptr);
    if (errno != 0 || endptr == fields[1] || duration < 0) {
        duration = 0;
    }

    memset(out, 0, sizeof(*out));
    out->timestamp = (time_t)ts;
    out->duration = duration;
    snprintf(out->artist, sizeof(out->artist), "%s", decoded[0]);
    snprintf(out->title, sizeof(out->title), "%s", decoded[1]);
    snprintf(out->album, sizeof(out->album), "%s", decoded[2]);
    return out->timestamp > 0 && out->artist[0] && out->title[0];
}

static bool queue_write_line(FILE *f, const lastfm_track_t *track) {
    if (!f || !track) {
        return false;
    }

    char artist[768], title[768], album[768];
    if (form_encode(track->artist, artist, sizeof(artist)) == 0 && track->artist[0]) return false;
    if (form_encode(track->title, title, sizeof(title)) == 0 && track->title[0]) return false;
    if (form_encode(track->album, album, sizeof(album)) == 0 && track->album[0]) return false;

    return fprintf(f, "%lld\t%.0f\t%s\t%s\t%s\n",
                   (long long)track->timestamp, track->duration,
                   artist, title, album) > 0;
}

static bool queue_prune(void) {
    if (!queue_path[0]) {
        return false;
    }

    FILE *in = fopen(queue_path, "r");
    if (!in) {
        return true;
    }
    FILE *out = fopen(queue_tmp_path, "w");
    if (!out) {
        fclose(in);
        return false;
    }

    time_t cutoff = time(NULL) - (time_t)LASTFM_QUEUE_MAX_AGE;
    char line[4096];

    while (fgets(line, sizeof(line), in)) {
        lastfm_track_t track;
        if (queue_line_parse(line, &track) && track.timestamp >= cutoff) {
            fputs(line, out);
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

static bool queue_enqueue(const lastfm_track_t *track) {
    if (!queue_path[0] || !track || !track->timestamp || !track->artist[0] || !track->title[0]) {
        return false;
    }

    queue_prune();

    FILE *f = fopen(queue_path, "a");
    if (!f) {
        return false;
    }
    bool ok = queue_write_line(f, track);
    fflush(f);
    fclose(f);
    return ok;
}

static bool queue_first(lastfm_track_t *out) {
    if (!queue_path[0] || !out) return false;

    FILE *f = fopen(queue_path, "r");
    if (!f) return false;

    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        if (queue_line_parse(line, out)) {
            fclose(f);
            return true;
        }
    }
    fclose(f);
    return false;
}

static bool queue_remove_first(void) {
    if (!queue_path[0]) return false;

    FILE *in = fopen(queue_path, "r");
    if (!in) return true;

    FILE *out = fopen(queue_tmp_path, "w");
    if (!out) {
        fclose(in);
        return false;
    }

    char line[4096];
    bool removed = false;
    while (fgets(line, sizeof(line), in)) {
        if (!removed) {
            lastfm_track_t track;
            if (queue_line_parse(line, &track)) {
                removed = true;
                continue;
            }
        }
        fputs(line, out);
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

/* ------------------------------------------------------------------------- */
/* Worker -> UI                                                              */
/* ------------------------------------------------------------------------- */

static void ui_result_cb(void *user) {
    ui_result_t *result = (ui_result_t *)user;
    if (!result) return;

    if (result->persist_login) {
        config_set(LASTFM_CONFIG_SECTION, CFG_SESSION_KEY, result->session_key);
        config_set(LASTFM_CONFIG_SECTION, CFG_USERNAME, result->username);
        config_save();
        gui_notify_popup(result->text);
    } else if (result->persist_logout) {
        config_set(LASTFM_CONFIG_SECTION, CFG_SESSION_KEY, "");
        config_set(LASTFM_CONFIG_SECTION, CFG_USERNAME, "");
        config_save();
        gui_notify_popup(result->text);
    } else if (result->text[0]) {
        gui_notify_popup(result->text);
    }

    free(result);
}

static void post_ui_message(const char *text, bool persist_login, const char *new_session,
                            const char *new_username, bool persist_logout) {
    ui_result_t *result = calloc(1, sizeof(*result));
    if (!result) return;

    snprintf(result->text, sizeof(result->text), "%s", text ? text : "");
    result->persist_login = persist_login;
    result->persist_logout = persist_logout;
    snprintf(result->session_key, sizeof(result->session_key), "%s", new_session ? new_session : "");
    snprintf(result->username, sizeof(result->username), "%s", new_username ? new_username : "");

    if (!gui_post(ui_result_cb, result)) {
        free(result);
    }
}

/* ------------------------------------------------------------------------- */
/* Network operations                                                        */
/* ------------------------------------------------------------------------- */

static bool do_login(job_t *job) {
    char key[LASTFM_API_KEY_MAX];
    char secret[LASTFM_API_SECRET_MAX];

    pthread_mutex_lock(&state_mutex);
    snprintf(key, sizeof(key), "%s", api_key);
    snprintf(secret, sizeof(secret), "%s", api_secret);
    pthread_mutex_unlock(&state_mutex);

    if (!key[0] || !secret[0]) {
        post_ui_message("Last.fm: API key and API secret are required", false, NULL, NULL, false);
        return false;
    }

    const param_t params[] = {
        {"method", "auth.getMobileSession"},
        {"username", job->username},
        {"password", job->password},
    };

    char *body = NULL;
    int status = 0;
    bool request_ok = api_call(key, secret, params, sizeof(params) / sizeof(params[0]), &body, &status);

    char session[LASTFM_SESSION_MAX] = "";
    char error[160] = "";

    if (request_ok && status == 200 && xml_ok(body) && xml_key(body, session, sizeof(session))) {
        pthread_mutex_lock(&state_mutex);
        snprintf(session_key, sizeof(session_key), "%s", session);
        snprintf(username, sizeof(username), "%s", job->username);
        logging_in = false;
        login_pending = false;
        set_message_locked("Logged in to Last.fm");
        pthread_cond_signal(&state_cond);
        pthread_mutex_unlock(&state_mutex);

        memset((void *)job->password, 0, sizeof(job->password));
        post_ui_message("Logged in to Last.fm", true, session, job->username, false);
        memset(job->password, 0, sizeof(job->password));
        free(body);
        return true;
    }

    if (body && xml_error(body, error, sizeof(error))) {
        char message[192];
        snprintf(message, sizeof(message), "Last.fm login failed: %s", error);
        post_ui_message(message, false, NULL, NULL, false);
    } else if (http_last_error()) {
        char message[192];
        snprintf(message, sizeof(message), "Last.fm login failed: %s", http_last_error());
        post_ui_message(message, false, NULL, NULL, false);
    } else {
        char message[192];
        snprintf(message, sizeof(message), "Last.fm login failed: HTTP %d", status);
        post_ui_message(message, false, NULL, NULL, false);
    }

    pthread_mutex_lock(&state_mutex);
    logging_in = false;
    login_pending = false;
    memset(login_password, 0, sizeof(login_password));
    pthread_mutex_unlock(&state_mutex);
    memset(job->password, 0, sizeof(job->password));
    free(body);
    return false;
}

static bool do_scrobble(const lastfm_track_t *track, const char *session, const char *key, const char *secret) {
    char timestamp[32], duration[32];
    snprintf(timestamp, sizeof(timestamp), "%lld", (long long)track->timestamp);
    snprintf(duration, sizeof(duration), "%.0f", track->duration);

    param_t params[8];
    size_t count = 0;
    params[count++] = (param_t){"method", "track.scrobble"};
    params[count++] = (param_t){"sk", session};
    params[count++] = (param_t){"track", track->title};
    params[count++] = (param_t){"artist", track->artist};
    if (track->album[0]) params[count++] = (param_t){"album", track->album};
    params[count++] = (param_t){"timestamp", timestamp};
    /* duration is optional to Last.fm but supported by the API. */
    if (count < 6) params[count++] = (param_t){"duration", duration};

    char *body = NULL;
    int status = 0;
    bool ok = api_call(key, secret, params, count, &body, &status);
    bool accepted = ok && status == 200 && xml_ok(body);
    free(body);
    return accepted;
}

static bool do_now_playing(const lastfm_track_t *track, const char *session, const char *key, const char *secret) {
    char duration[32];
    snprintf(duration, sizeof(duration), "%.0f", track->duration);

    param_t params[6];
    size_t count = 0;
    params[count++] = (param_t){"method", "track.updateNowPlaying"};
    params[count++] = (param_t){"sk", session};
    params[count++] = (param_t){"track", track->title};
    params[count++] = (param_t){"artist", track->artist};
    if (track->album[0]) params[count++] = (param_t){"album", track->album};
    params[count++] = (param_t){"duration", duration};

    char *body = NULL;
    int status = 0;
    bool ok = api_call(key, secret, params, count, &body, &status);
    bool accepted = ok && status == 200 && xml_ok(body);
    free(body);
    return accepted;
}

static void do_sync_queue(const char *session, const char *key, const char *secret) {
    queue_prune();

    lastfm_track_t item;
    if (!queue_first(&item)) {
        return;
    }

    if (do_scrobble(&item, session, key, secret)) {
        queue_remove_first();
    }
}

static bool current_send_snapshot(lastfm_track_t *track, char *session, size_t session_size,
                                  unsigned *generation_out) {
    pthread_mutex_lock(&state_mutex);
    bool allowed = send_enabled_locked() && current_track_valid;
    if (allowed) {
        *track = current_track;
        snprintf(session, session_size, "%s", session_key);
        if (generation_out) *generation_out = current_generation;
    }
    pthread_mutex_unlock(&state_mutex);
    return allowed;
}

static void *lastfm_worker(void *unused) {
    (void)unused;

    pthread_mutex_lock(&state_mutex);
    while (!worker_stop) {
        while (!worker_stop && !login_pending && !current_track_valid) {
            struct timespec wake;
            clock_gettime(CLOCK_REALTIME, &wake);
            wake.tv_sec += LASTFM_POLL_SECONDS;
            pthread_cond_timedwait(&state_cond, &state_mutex, &wake);
            break;
        }

        if (worker_stop) {
            break;
        }

        bool do_login_job = login_pending;
        job_t job = {0};
        if (do_login_job) {
            job.kind = JOB_LOGIN;
            snprintf(job.username, sizeof(job.username), "%s", login_username);
            snprintf(job.password, sizeof(job.password), "%s", login_password);
            memset(login_password, 0, sizeof(login_password));
            /*
             * Keep login_pending true while the worker operates: the UI will
             * reject another login until the request has completed.
             */
            pthread_mutex_unlock(&state_mutex);
            do_login(&job);
            pthread_mutex_lock(&state_mutex);
            continue;
        }

        pthread_mutex_unlock(&state_mutex);

        /*
         * One poll pass:
         *   1. drain one old offline record
         *   2. update now-playing once for a newly announced track
         *   3. check the current track against Last.fm's 50%/4-minute rule
         */
        char session[LASTFM_SESSION_MAX];
        char key[LASTFM_API_KEY_MAX];
        char secret[LASTFM_API_SECRET_MAX];
        bool allowed;
        pthread_mutex_lock(&state_mutex);
        allowed = send_enabled_locked();
        snprintf(session, sizeof(session), "%s", session_key);
        snprintf(key, sizeof(key), "%s", api_key);
        snprintf(secret, sizeof(secret), "%s", api_secret);
        pthread_mutex_unlock(&state_mutex);

        if (allowed) {
            do_sync_queue(session, key, secret);
        }

        lastfm_track_t track;
        unsigned generation = 0;
        if (current_send_snapshot(&track, session, sizeof(session), &generation)) {
            bool send_now = false;
            bool scrobble = false;

            double position = 0;
            double total = 0;
            audio_get_progress(&position, &total);

            if (total > 0 && track.duration <= 0) {
                track.duration = total;
                pthread_mutex_lock(&state_mutex);
                if (current_generation == generation) {
                    current_track.duration = total;
                }
                pthread_mutex_unlock(&state_mutex);
            }

            pthread_mutex_lock(&state_mutex);
            if (current_generation == generation && !now_playing_sent) {
                now_playing_sent = true;
                send_now = true;
            }
            pthread_mutex_unlock(&state_mutex);

            if (send_now) {
                /* Advisory: failure does not affect scrobbling. */
                do_now_playing(&track, session, key, secret);
            }

            double duration = track.duration;
            if (duration >= 30.0) {
                double threshold = duration / 2.0;
                if (threshold > 240.0) threshold = 240.0;

                pthread_mutex_lock(&state_mutex);
                if (current_generation == generation && !scrobbled_this_track &&
                    !scrobble_in_flight && audio_get_status() == AUDIO_STATUS_PLAYING &&
                    position >= threshold) {
                    scrobble_in_flight = true;
                    scrobble = true;
                }
                pthread_mutex_unlock(&state_mutex);
            }

            if (scrobble) {
                bool accepted = do_scrobble(&track, session, key, secret);
                bool queued = false;

                if (!accepted) {
                    /*
                     * Queueing is disk I/O and must not happen while state_mutex
                     * is held: the UI can change settings while the card is slow.
                     */
                    queued = queue_enqueue(&track);
                }

                pthread_mutex_lock(&state_mutex);
                if (current_generation == generation) {
                    if (accepted || queued) {
                        scrobbled_this_track = true;
                    }
                    scrobble_in_flight = false;
                }
                pthread_mutex_unlock(&state_mutex);
            }
        }

        /* Wait roughly until the next 15-second poll or an explicit wake. */
        pthread_mutex_lock(&state_mutex);
        if (!worker_stop) {
            struct timespec wake;
            clock_gettime(CLOCK_REALTIME, &wake);
            wake.tv_sec += LASTFM_POLL_SECONDS;
            pthread_cond_timedwait(&state_cond, &state_mutex, &wake);
        }
    }
    pthread_mutex_unlock(&state_mutex);
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

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
    queue_paths_init();

    worker_stop = false;
    if (pthread_create(&worker_thread, NULL, lastfm_worker, NULL) != 0) {
        set_message_locked("Last.fm worker could not start");
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

    /*
     * Only record a new Last.fm play for a genuine new track. The device
     * playback layer calls this from its track-change path; pauses/resumes and
     * output restarts do not call it.
     */
    pthread_mutex_lock(&state_mutex);
    if (!worker_started) {
        pthread_mutex_unlock(&state_mutex);
        return;
    }

    double duration = 0;
    double current = 0;
    audio_get_progress(&current, &duration);

    copy_track(&current_track, metadata, time(NULL), duration);
    current_track_valid = true;
    current_generation++;
    scrobbled_this_track = false;
    scrobble_in_flight = false;
    now_playing_sent = false;
    set_message_locked("Track started");

    pthread_cond_signal(&state_cond);
    pthread_mutex_unlock(&state_mutex);
}

void lastfm_set_enabled(bool value) {
    pthread_mutex_lock(&state_mutex);
    enabled = value;
    config_set_bool(LASTFM_CONFIG_SECTION, CFG_ENABLED, enabled);
    config_save();
    if (enabled && session_key[0]) {
        pthread_cond_signal(&state_cond);
    }
    pthread_mutex_unlock(&state_mutex);
}

static void set_credential_locked(const char *key, char *dst, size_t dst_size, const char *value) {
    snprintf(dst, dst_size, "%s", value ? value : "");

    /*
     * The credentials are part of the session identity. Changing either one
     * invalidates the current session and forces a fresh login.
     */
    session_key[0] = '\0';
    username[0] = '\0';
    config_set(LASTFM_CONFIG_SECTION, key, dst);
    config_set(LASTFM_CONFIG_SECTION, CFG_SESSION_KEY, "");
    config_set(LASTFM_CONFIG_SECTION, CFG_USERNAME, "");
    config_save();
    set_message_locked(dst[0] ? "Last.fm credentials updated; log in again"
                               : "Last.fm credential cleared");
}

void lastfm_set_api_key(const char *value) {
    pthread_mutex_lock(&state_mutex);
    set_credential_locked(CFG_API_KEY, api_key, sizeof(api_key), value);
    pthread_cond_signal(&state_cond);
    pthread_mutex_unlock(&state_mutex);
}

void lastfm_set_api_secret(const char *value) {
    pthread_mutex_lock(&state_mutex);
    set_credential_locked(CFG_API_SECRET, api_secret, sizeof(api_secret), value);
    pthread_cond_signal(&state_cond);
    pthread_mutex_unlock(&state_mutex);
}

void lastfm_login(const char *user, const char *password) {
    pthread_mutex_lock(&state_mutex);
    if (!api_key[0] || !api_secret[0]) {
        set_message_locked("Last.fm: API key and API secret are required");
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
    config_set(LASTFM_CONFIG_SECTION, CFG_SESSION_KEY, "");
    config_set(LASTFM_CONFIG_SECTION, CFG_USERNAME, "");
    config_save();
    set_message_locked("Logged out of Last.fm");
    pthread_mutex_unlock(&state_mutex);
}

void lastfm_get_snapshot(lastfm_snapshot_t *out) {
    if (!out) return;

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
        snprintf(out->status, sizeof(out->status), "API key and API secret required");
    } else if (logging_in) {
        snprintf(out->status, sizeof(out->status), "Logging in...");
    } else if (!session_key[0]) {
        snprintf(out->status, sizeof(out->status), "Not logged in");
    } else {
        snprintf(out->status, sizeof(out->status), "Connected as %s", username[0] ? username : "Last.fm user");
    }

    pthread_mutex_unlock(&state_mutex);
}
