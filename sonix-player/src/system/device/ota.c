#include "ota.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "src/system/core/json.h"
#include "src/system/core/sha256.h"
#include "src/system/core/utils.h"
#include "src/system/device/firmware.h"
#include "src/system/device/sysinfo.h"
#include "src/system/device/system.h"
#include "src/system/net/http.h"
#include "src/system/net/wifi.h"

#define DEFAULT_REPO "Jepl4r/sonix-player"

// The API's own media type and version, as GitHub asks every client to send.
#define API_HEADERS                                                                                                    \
	"Accept: application/vnd.github+json\r\n"                                                                          \
	"X-GitHub-Api-Version: 2022-11-28\r\n"

// Twenty releases with their descriptions stay well under this.
#define LIST_LIMIT (1536 * 1024)
#define LIST_TIMEOUT_SECS 20
#define DOWNLOAD_TIMEOUT_SECS 30

// Room left on the card beyond the file itself.
#define SPACE_MARGIN (8L * 1024 * 1024)

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static ota_state_t state = OTA_IDLE;
static ota_result_t result = OTA_OK;
static long progress_done, progress_total;
static bool cancel_requested;
static http_stream_t *active_stream; // the download's stream, for ota_cancel() to wake

static ota_release_t release;
static bool release_valid;

// Set on the interface thread when a check starts.
static char repo[128];
static char stem[64];

static unsigned char chunk[64 * 1024];

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------

static bool begin(ota_state_t next) {
	pthread_mutex_lock(&lock);
	bool ok = state == OTA_IDLE || state == OTA_FINISHED;
	if (ok) {
		state = next;
		cancel_requested = false;
		progress_done = 0;
		progress_total = 0;
	}
	pthread_mutex_unlock(&lock);
	return ok;
}

static void finish(ota_result_t r) {
	pthread_mutex_lock(&lock);
	result = cancel_requested && r != OTA_OK ? OTA_CANCELLED : r;
	state = OTA_FINISHED;
	active_stream = NULL;
	pthread_mutex_unlock(&lock);
}

static bool cancelled(void) {
	pthread_mutex_lock(&lock);
	bool c = cancel_requested;
	pthread_mutex_unlock(&lock);
	return c;
}

void ota_cancel(void) {
	pthread_mutex_lock(&lock);
	if (state == OTA_CHECKING || state == OTA_DOWNLOADING) {
		cancel_requested = true;
		if (active_stream) {
			http_stream_wake(active_stream);
		}
	}
	pthread_mutex_unlock(&lock);
}

ota_state_t ota_state(void) {
	pthread_mutex_lock(&lock);
	ota_state_t s = state;
	pthread_mutex_unlock(&lock);
	return s;
}

ota_result_t ota_result(void) {
	pthread_mutex_lock(&lock);
	ota_result_t r = result;
	pthread_mutex_unlock(&lock);
	return r;
}

void ota_acknowledge(void) {
	pthread_mutex_lock(&lock);
	if (state == OTA_FINISHED) {
		state = OTA_IDLE;
	}
	pthread_mutex_unlock(&lock);
}

bool ota_busy(void) {
	ota_state_t s = ota_state();
	return s == OTA_CHECKING || s == OTA_DOWNLOADING;
}

void ota_progress(long *done, long *total) {
	pthread_mutex_lock(&lock);
	if (done) {
		*done = progress_done;
	}
	if (total) {
		*total = progress_total;
	}
	pthread_mutex_unlock(&lock);
}

const ota_release_t *ota_release(void) { return &release; }

const char *ota_installed_version(void) { return sysinfo_os_version(); }

// ---------------------------------------------------------------------------
// versions
// ---------------------------------------------------------------------------

#define VERSION_PARTS 4

typedef struct {
	long parts[VERSION_PARTS];
	long ui;
	int stage; // alpha, beta, rc, stable
	long iteration;
} ota_version_t;

enum { VERSION_ALPHA, VERSION_BETA, VERSION_RC, VERSION_STABLE };

static bool version_number(const char **text, long *out) {
	if (!isdigit((unsigned char)**text)) {
		return false;
	}
	char *end;
	errno = 0;
	long number = strtol(*text, &end, 10);
	if (errno == ERANGE || number > INT_MAX) {
		return false;
	}
	*text = end;
	*out = number;
	return true;
}

// Keep the UI generation and prerelease counter: alpha25 is not alpha24,
// and an old unbranded 1.3.0 is not newer than 1.3.0-ui3-alpha25.
static bool parse_version(const char *text, ota_version_t *out) {
	memset(out, 0, sizeof(*out));
	out->stage = VERSION_STABLE;
	const char *p = text;
	if (*p == 'v' || *p == 'V') {
		p++;
	}
	for (int i = 0; i < VERSION_PARTS; i++) {
		if (!version_number(&p, &out->parts[i])) {
			return false;
		}
		if (*p != '.') {
			break;
		}
		p++;
		if (i == VERSION_PARTS - 1) {
			return false;
		}
	}
	if (strncmp(p, "-ui", 3) == 0) {
		p += 3;
		if (!version_number(&p, &out->ui)) {
			return false;
		}
	}
	if (!*p || strcmp(p, "-r1") == 0 || strcmp(p, "-stable") == 0) {
		return true;
	}
	static const char *const stages[] = {"-alpha", "-beta", "-rc"};
	for (int stage = VERSION_ALPHA; stage < VERSION_STABLE; stage++) {
		size_t length = strlen(stages[stage]);
		if (strncmp(p, stages[stage], length) == 0) {
			p += length;
			if (*p == '.') {
				p++;
			}
			out->stage = stage;
			return version_number(&p, &out->iteration) && !*p;
		}
	}
	return false;
}

static int version_cmp(const ota_version_t *a, const ota_version_t *b) {
	for (int i = 0; i < VERSION_PARTS; i++) {
		if (a->parts[i] != b->parts[i]) {
			return a->parts[i] < b->parts[i] ? -1 : 1;
		}
	}
	if (a->ui != b->ui) {
		return a->ui < b->ui ? -1 : 1;
	}
	if (a->stage != b->stage) {
		return a->stage < b->stage ? -1 : 1;
	}
	if (a->iteration != b->iteration) {
		return a->iteration < b->iteration ? -1 : 1;
	}
	return 0;
}

// A release tag may be an editorial name such as "r2". An explicit version
// in its body is authoritative; dates and marketing titles are not versions.
// Only a whole line, or one whole HTML comment line, is accepted. Reject even
// identical duplicate markers rather than guessing which one the author meant.
static bool version_from_body(char *body, const char *tag, ota_version_t *out) {
	bool found = false;
	for (char *line = body; line && *line;) {
		char *next = strchr(line, '\n');
		if (next) {
			*next++ = '\0';
		}
		while (isspace((unsigned char)*line)) {
			line++;
		}
		bool hidden = strncmp(line, "<!--", 4) == 0;
		if (hidden) {
			line += 4;
			while (*line == ' ' || *line == '\t') {
				line++;
			}
		}
		if (strncmp(line, "Sonix-Version:", 14) == 0) {
			if (found) {
				return false;
			}
			found = true;
			line += 14;
			while (*line == ' ' || *line == '\t') {
				line++;
			}
			char *end = line + strlen(line);
			while (end > line && isspace((unsigned char)end[-1])) {
				*--end = '\0';
			}
			if (hidden) {
				if (end - line < 3 || strcmp(end - 3, "-->") != 0) {
					return false;
				}
				end -= 3;
				while (end > line && isspace((unsigned char)end[-1])) {
					end--;
				}
				*end = '\0';
			}
			if (!parse_version(line, out)) {
				return false;
			}
		}
		line = next;
	}
	if (found) {
		return true;
	}
	return parse_version(tag, out);
}

static bool release_version(const json_doc_t *doc, int rel, ota_version_t *out) {
	char tag[sizeof(release.tag)];
	int token = json_get(doc, rel, "tag_name");
	if (token < 0 || doc->toks[token].end - doc->toks[token].start >= (int)sizeof(tag) || !json_str(doc, token, tag, sizeof(tag)) || !tag[0]) {
		return false;
	}
	int notes = json_get(doc, rel, "body");
	if (notes < 0 || doc->toks[notes].type == JSON_NULL) {
		return parse_version(tag, out);
	}
	int raw_length = doc->toks[notes].end - doc->toks[notes].start;
	if (doc->toks[notes].type != JSON_STRING || raw_length < 0 || raw_length > LIST_LIMIT) {
		return false;
	}
	// The UI preview can be truncated, but version markers and conflicts may
	// be at the end of a long changelog. JSON unescaping never grows the raw
	// token, and the same response budget bounds this one temporary copy.
	char *body = malloc((size_t)raw_length + 1);
	if (!body) {
		return false;
	}
	bool valid = json_str(doc, notes, body, (size_t)raw_length + 1) && version_from_body(body, tag, out);
	free(body);
	return valid;
}

// ---------------------------------------------------------------------------
// the release list
// ---------------------------------------------------------------------------

// Drops a UTF-8 sequence cut in half at the end of `s`.
static void trim_partial_utf8(char *s) {
	size_t len = strlen(s);
	size_t i = len;
	while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) {
		i--;
	}
	if (i == 0) {
		return;
	}
	unsigned char lead = (unsigned char)s[i - 1];
	size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
	if (len - (i - 1) < need) {
		s[i - 1] = '\0';
	}
}

// The asset named "<stem>.upt" in release `rel`, -1 when there is none.
static int find_asset(const json_doc_t *doc, int rel) {
	char wanted[80];
	snprintf(wanted, sizeof(wanted), "%s.upt", stem);

	int assets = json_get(doc, rel, "assets");
	int count = json_len(doc, assets);
	for (int i = 0; i < count; i++) {
		int a = json_at(doc, assets, i);
		char name[96], asset_state[24];
		json_obj_str(doc, a, "name", name, sizeof(name));
		json_obj_str(doc, a, "state", asset_state, sizeof(asset_state));
		if (strcasecmp(name, wanted) == 0 && strcmp(asset_state, "uploaded") == 0) {
			return a;
		}
	}
	return -1;
}

static ota_result_t pick_release(const json_doc_t *doc) {
	int root = json_root(doc);
	if (root < 0 || doc->toks[root].type != JSON_ARRAY) {
		return OTA_CHECK_FAILED;
	}

	ota_version_t installed;
	if (!parse_version(ota_installed_version(), &installed)) {
		return OTA_CHECK_FAILED;
	}

	ota_version_t best = installed;
	int best_rel = -1, best_asset = -1;
	bool unversioned = false;

	int count = json_len(doc, root);
	for (int i = 0; i < count; i++) {
		int rel = json_at(doc, root, i);
		int prerelease = json_get(doc, rel, "prerelease");
		if (prerelease < 0 || doc->toks[prerelease].type != JSON_BOOL || json_obj_bool(doc, rel, "draft", true) || (json_obj_bool(doc, rel, "prerelease", true) && installed.stage == VERSION_STABLE)) {
			continue;
		}
		int asset = find_asset(doc, rel);
		if (asset < 0) {
			continue;
		}
		ota_version_t version;
		if (!release_version(doc, rel, &version)) {
			unversioned = true;
			continue;
		}
		if ((version.stage != VERSION_STABLE && installed.stage == VERSION_STABLE) || version_cmp(&version, &best) <= 0) {
			continue;
		}
		best = version;
		best_rel = rel;
		best_asset = asset;
	}

	if (best_rel < 0) {
		return unversioned ? OTA_CHECK_FAILED : OTA_UP_TO_DATE;
	}

	memset(&release, 0, sizeof(release));
	json_obj_str(doc, best_rel, "tag_name", release.tag, sizeof(release.tag));
	if (!json_obj_str(doc, best_rel, "name", release.name, sizeof(release.name)) || !release.name[0]) {
		snprintf(release.name, sizeof(release.name), "%s", release.tag);
	}
	json_obj_str(doc, best_rel, "body", release.notes, sizeof(release.notes));
	trim_partial_utf8(release.notes);
	release.size = json_obj_long(doc, best_asset, "size", 0);
	json_obj_str(doc, best_asset, "browser_download_url", release.url, sizeof(release.url));

	// "sha256:<64 hex>" on assets uploaded since mid-2025.
	char digest[80];
	if (json_obj_str(doc, best_asset, "digest", digest, sizeof(digest)) && strncmp(digest, "sha256:", 7) == 0) {
		if (strlen(digest + 7) != 64) {
			return OTA_CHECK_FAILED;
		}
		for (const char *p = digest + 7; *p; p++) {
			if (!isxdigit((unsigned char)*p)) {
				return OTA_CHECK_FAILED;
			}
		}
		memcpy(release.sha256, digest + 7, sizeof(release.sha256));
	}

	if (release.size <= 0 || strncmp(release.url, "https://", 8) != 0) {
		return OTA_CHECK_FAILED;
	}
	release_valid = true;
	printf("ota: %s (%s) available, %ld bytes, sha256 %s\n", release.tag, release.name, release.size,
		   release.sha256[0] ? release.sha256 : "not published");
	return OTA_OK;
}

static ota_result_t do_check(void) {
	wifi_status_t wifi;
	wifi_get_status(&wifi);
	if (wifi.state != WIFI_STATE_CONNECTED) {
		return OTA_NO_NETWORK;
	}
	if (!stem[0]) {
		return OTA_NO_MODEL;
	}

	char url[256];
	snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases?per_page=20", repo);
	printf("ota: checking %s, installed '%s', file %s.upt\n", url, ota_installed_version(), stem);

	char *body = NULL;
	size_t body_len = 0;
	int status = 0;
	if (!http_get_ex(url, API_HEADERS, false, &body, &body_len, LIST_LIMIT, LIST_TIMEOUT_SECS, &status)) {
		const char *why = http_last_error();
		printf("ota: release list not fetched (status %d%s%s)\n", status, why ? ", " : "", why ? why : "");
		return OTA_CHECK_FAILED;
	}
	if (cancelled()) {
		free(body);
		return OTA_CANCELLED;
	}

	json_doc_t doc;
	ota_result_t r = OTA_CHECK_FAILED;
	if (json_parse(body, &doc)) {
		r = pick_release(&doc);
		json_free(&doc);
	} else {
		printf("ota: release list is not valid JSON (%zu bytes)\n", body_len);
	}
	free(body);
	return r;
}

static void *check_thread(void *unused) {
	(void)unused;
	thread_be_background("ota check");
	finish(do_check());
	return NULL;
}

bool ota_check_start(void) {
	if (!begin(OTA_CHECKING)) {
		return false;
	}
	release_valid = false;

	const char *r = sysinfo_ota_repo();
	snprintf(repo, sizeof(repo), "%s", r[0] ? r : DEFAULT_REPO);
	const char *s = firmware_update_stem((char[128]){0}, 128);
	snprintf(stem, sizeof(stem), "%s", s ? s : "");

	pthread_t thread;
	if (pthread_create(&thread, NULL, check_thread, NULL) != 0) {
		finish(OTA_CHECK_FAILED);
		return true;
	}
	pthread_detach(thread);
	return true;
}

// ---------------------------------------------------------------------------
// the download
// ---------------------------------------------------------------------------

// Removes every "<stem>.upt" on the card, whatever its case: on exFAT two
// spellings can sit side by side, and the recovery kernel would take either.
static void remove_old_files(const char *root) {
	char wanted[80];
	snprintf(wanted, sizeof(wanted), "%s.upt", stem);

	DIR *dir = opendir(root);
	if (!dir) {
		return;
	}
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (strcasecmp(de->d_name, wanted) == 0) {
			char path[600];
			snprintf(path, sizeof(path), "%s/%s", root, de->d_name);
			unlink(path);
		}
	}
	closedir(dir);
}

static ota_result_t do_download(void) {
	const char *root = storage_sd_root();
	if (!root || !*root) {
		return OTA_NO_CARD;
	}

	struct statvfs vfs;
	if (statvfs(root, &vfs) == 0) {
		unsigned long long unit = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
		unsigned long long avail = (unsigned long long)vfs.f_bavail * unit;
		if (avail < (unsigned long long)release.size + SPACE_MARGIN) {
			return OTA_NO_SPACE;
		}
	}

	char final_path[600], part_path[620];
	snprintf(final_path, sizeof(final_path), "%s/%s.upt", root, stem);
	snprintf(part_path, sizeof(part_path), "%s.part", final_path);

	FILE *f = fopen(part_path, "wb");
	if (!f) {
		printf("ota: cannot create %s\n", part_path);
		return OTA_DOWNLOAD_FAILED;
	}

	http_stream_t stream;
	memset(&stream, 0, sizeof(stream));
	stream.fd = -1;
	printf("ota: downloading %s\n", release.url);
	if (!http_stream_open(&stream, release.url, DOWNLOAD_TIMEOUT_SECS)) {
		const char *why = http_last_error();
		printf("ota: download not started%s%s\n", why ? ": " : "", why ? why : "");
		fclose(f);
		unlink(part_path);
		return cancelled() ? OTA_CANCELLED : OTA_DOWNLOAD_FAILED;
	}

	pthread_mutex_lock(&lock);
	active_stream = &stream;
	bool stop = cancel_requested;
	pthread_mutex_unlock(&lock);

	ota_result_t r = OTA_OK;
	if (stream.content_length > 0 && stream.content_length != release.size) {
		printf("ota: the server sends %ld bytes, the release says %ld\n", stream.content_length, release.size);
		r = OTA_CORRUPT;
	}

	sha256_ctx sha;
	sha256_init(&sha);
	long done = 0;
	while (r == OTA_OK && !stop && done < release.size) {
		long want = release.size - done;
		int n = http_stream_read(&stream, chunk, want < (long)sizeof(chunk) ? (int)want : (int)sizeof(chunk));
		if (n <= 0) {
			printf("ota: the connection ended at %ld of %ld bytes\n", done, release.size);
			r = OTA_DOWNLOAD_FAILED;
			break;
		}
		if (fwrite(chunk, 1, (size_t)n, f) != (size_t)n) {
			printf("ota: write to the card failed at %ld bytes\n", done);
			r = OTA_DOWNLOAD_FAILED;
			break;
		}
		sha256_update(&sha, chunk, (size_t)n);
		done += n;

		pthread_mutex_lock(&lock);
		progress_done = done;
		stop = cancel_requested;
		pthread_mutex_unlock(&lock);
	}

	pthread_mutex_lock(&lock);
	active_stream = NULL;
	pthread_mutex_unlock(&lock);
	http_stream_close(&stream);

	if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
		r = r == OTA_OK ? OTA_DOWNLOAD_FAILED : r;
	}
	if (fclose(f) != 0 && r == OTA_OK) {
		r = OTA_DOWNLOAD_FAILED;
	}

	if (stop) {
		r = OTA_CANCELLED;
	}
	if (r == OTA_OK && release.sha256[0]) {
		char hex[SHA256_HEX_LEN];
		sha256_final_hex(&sha, hex);
		if (strcasecmp(hex, release.sha256) != 0) {
			printf("ota: sha256 %s, the release says %s\n", hex, release.sha256);
			r = OTA_CORRUPT;
		}
	}
	if (r != OTA_OK) {
		unlink(part_path);
		return r;
	}

	remove_old_files(root);
	if (rename(part_path, final_path) != 0) {
		printf("ota: cannot rename %s\n", part_path);
		unlink(part_path);
		return OTA_DOWNLOAD_FAILED;
	}
	sync();
	printf("ota: %s ready (%ld bytes%s)\n", final_path, done, release.sha256[0] ? ", sha256 matches" : "");
	return OTA_OK;
}

static void *download_thread(void *unused) {
	(void)unused;
	thread_be_background("ota download");
	finish(do_download());
	return NULL;
}

bool ota_download_start(void) {
	if (!release_valid || !begin(OTA_DOWNLOADING)) {
		return false;
	}

	pthread_mutex_lock(&lock);
	progress_total = release.size;
	pthread_mutex_unlock(&lock);

	pthread_t thread;
	if (pthread_create(&thread, NULL, download_thread, NULL) != 0) {
		finish(OTA_DOWNLOAD_FAILED);
		return true;
	}
	pthread_detach(thread);
	return true;
}
