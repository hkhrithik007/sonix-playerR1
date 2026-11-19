#include "peqautoeq.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "src/gui/audio/peqpage.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/sx_tokens.h"
#include "src/gui/shell/sx_styles.h"
#include "src/system/audio/eq.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/net/http.h"
#include "src/system/device/system.h"
#include "src/system/net/wifi.h"

#define RESULT_MAX 20
#define INDEX_MAX (4u * 1024u * 1024u)
#define PROFILE_MAX (128u * 1024u)

#define INDEX_URL "https://raw.githubusercontent.com/jaakkopasanen/AutoEq/refs/heads/master/results/INDEX.md"
#define RESULTS_URL "https://raw.githubusercontent.com/jaakkopasanen/AutoEq/refs/heads/master/results/"

typedef struct {
	char name[128];
	char path[512];
	char source[128];
	char display[300];
	bool duplicate_name;
} profile_t;

typedef enum {
	JOB_SEARCH,
	JOB_UPDATE_DATABASE,
	JOB_DOWNLOAD_PROFILE,
} job_action_t;

typedef enum { JOB_NEEDS_ADMISSION, JOB_WORKING, JOB_READY } job_phase_t;

typedef struct {
	job_action_t action;
	job_phase_t phase;
	storage_card_token_t card;
	peq_preset_data_t parsed;
	bool published, replaced, cached_profile;
	char query[128];
	char error[192];
	profile_t selected;
	profile_t results[RESULT_MAX];
	int result_count;
	char preset[101];
	char cache[768];
	char *profile_text;
	size_t profile_size;
	uint64_t request_generation;
	uint64_t toast_owner;
} job_t;

static gui_config_t *cfg;
static lv_obj_t *search_screen;
static lv_obj_t *search_field;
static lv_obj_t *search_list;
static lv_obj_t *results_screen;
static lv_obj_t *results_empty;
static keyboard_t *search_keyboard;
static profile_t result_items[RESULT_MAX];
static int result_count;
static void (*reload_cb)(void);
static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static bool job_busy;
static bool job_closing;
static job_t *ready_job;
// GUI-owned page/request state, never read by the worker.
static uint64_t search_generation;
static uint64_t pending_results_generation;
// Matches the existing longest combined profile-applied message, without allocation.
static char pending_notice[512];
static bool pending_notice_error;
static uint64_t pending_notice_generation;

static void job_done(void *user);
static void result_clicked(lv_event_t *event);
static void *worker(void *user);

static bool begin_job(void) {
	pthread_mutex_lock(&job_lock);
	bool ok = !job_busy && !job_closing && !ready_job;
	if (ok) {
		job_busy = true;
	}
	pthread_mutex_unlock(&job_lock);
	return ok;
}



static bool is_busy(void) {
	pthread_mutex_lock(&job_lock);
	bool busy = job_busy;
	pthread_mutex_unlock(&job_lock);
	return busy;
}

static uint32_t hash_text(const char *text) {
	uint32_t hash = 2166136261u;
	for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
		hash = (hash ^ *p) * 16777619u;
	}
	return hash;
}

static bool has_text(const char *text, const char *query) {
	if (!query[0]) {
		return true;
	}
	for (const char *p = text; *p; p++) {
		const char *a = p;
		const char *b = query;
		while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
			a++;
			b++;
		}
		if (!*b) {
			return true;
		}
	}
	return false;
}

static void copy_trimmed(char *out, size_t size, const char *text, size_t length) {
	while (length && isspace((unsigned char)text[length - 1])) {
		length--;
	}
	if (length >= size) {
		length = size - 1;
	}
	memcpy(out, text, length);
	out[length] = '\0';
}

static void profile_source(profile_t *item, const char *meta, const char *path) {
	while (*meta == ' ' || *meta == '\t') {
		meta++;
	}
	if (strncmp(meta, "by ", 3) == 0) {
		meta += 3;
	}
	if (*meta == '[') {
		const char *end = strchr(meta + 1, ']');
		if (end) {
			copy_trimmed(item->source, sizeof(item->source), meta + 1, (size_t)(end - meta - 1));
		}
	} else {
		copy_trimmed(item->source, sizeof(item->source), meta, strlen(meta));
	}

	if (!item->source[0]) {
		const char *last = strrchr(path, '/');
		if (last && last > path) {
			const char *parent_end = last;
			const char *parent_start = parent_end;
			while (parent_start > path && parent_start[-1] != '/') {
				parent_start--;
			}
			copy_trimmed(item->source, sizeof(item->source), parent_start, (size_t)(parent_end - parent_start));
		}
	}
	if (!item->source[0]) {
		snprintf(item->source, sizeof(item->source), "%s", "AutoEq");
	}
}

static void finish_result_names(job_t *job) {
	for (int i = 0; i < job->result_count; i++) {
		int matches = 0;
		for (int j = 0; j < job->result_count; j++) {
			if (strcasecmp(job->results[i].name, job->results[j].name) == 0) {
				matches++;
			}
		}
		job->results[i].duplicate_name = job->results[i].duplicate_name || matches > 1;
		if (job->results[i].duplicate_name) {
			snprintf(job->results[i].display, sizeof(job->results[i].display), "%s [%s]", job->results[i].name, job->results[i].source);
		} else {
			snprintf(job->results[i].display, sizeof(job->results[i].display), "%s", job->results[i].name);
		}
	}
}

// INDEX.md is Markdown; balance parentheses so profile paths containing them
// remain intact while parsing the link target.
static void parse_index(const char *text, const char *query, job_t *job) {
	for (const char *line = text; *line;) {
		const char *end = strchr(line, '\n');
		size_t length = end ? (size_t)(end - line) : strlen(line);
		if (length < 2048) {
			char row[2048];
			memcpy(row, line, length);
			row[length] = '\0';
			while (length && isspace((unsigned char)row[length - 1])) {
				row[--length] = '\0';
			}

			char *start = strstr(row, "- [");
			char *name_end = start ? strstr(start + 3, "](") : NULL;
			char *path_end = NULL;
			if (name_end) {
				int depth = 1;
				for (char *p = name_end + 2; *p; p++) {
					if (*p == '(') {
						depth++;
					} else if (*p == ')' && --depth == 0) {
						path_end = p;
						break;
					}
				}
			}

			if (start && name_end && path_end && has_text(start + 3, query)) {
				size_t name_len = (size_t)(name_end - start - 3);
				size_t path_len = (size_t)(path_end - name_end - 2);
				if (name_len && name_len < sizeof(job->results[0].name) && path_len && path_len < sizeof(job->results[0].path)) {
					profile_t item = {0};
					memcpy(item.name, start + 3, name_len);
					memcpy(item.path, name_end + 2, path_len);
					profile_source(&item, path_end + 1, item.path);

					bool duplicate = false;
					for (int i = 0; i < job->result_count; i++) {
						if (!strcasecmp(job->results[i].name, item.name) && !strcmp(job->results[i].path, item.path)) {
							duplicate = true;
							break;
						}
						if (!strcasecmp(job->results[i].name, item.name)) {
							job->results[i].duplicate_name = true;
							item.duplicate_name = true;
						}
					}
					if (!duplicate && job->result_count < RESULT_MAX) {
						job->results[job->result_count++] = item;
					}
				}
			}
		}
		if (!end) {
			break;
		}
		line = end + 1;
	}
	finish_result_names(job);
}

static bool encode_path(const char *input, char *output, size_t capacity) {
	static const char hex[] = "0123456789ABCDEF";
	size_t used = 0;
	for (const unsigned char *p = (const unsigned char *)input; *p; p++) {
		unsigned char c = *p;
		if (c == '/' || isalnum(c) || strchr("-_.~", c)) {
			if (used + 1 >= capacity) {
				return false;
			}
			output[used++] = (char)c;
		} else if (c == '%' && isxdigit(p[1]) && isxdigit(p[2])) {
			if (used + 3 >= capacity) {
				return false;
			}
			output[used++] = '%';
			output[used++] = (char)p[1];
			output[used++] = (char)p[2];
			p += 2;
		} else {
			if (used + 3 >= capacity || c == '?' || c == '#' || c == '\\') {
				return false;
			}
			output[used++] = '%';
			output[used++] = hex[c >> 4];
			output[used++] = hex[c & 15];
		}
	}
	output[used] = '\0';
	return true;
}

static bool make_profile_url(const profile_t *item, char *url, size_t capacity) {
	const char *path = item->path;
	if (!strncmp(path, "./", 2)) {
		path += 2;
	}
	char encoded_path[1600];
	char file[768];
	char name[180];
	if (!encode_path(path, encoded_path, sizeof(encoded_path))) {
		return false;
	}
	snprintf(name, sizeof(name), "%s ParametricEQ.txt", item->name);
	http_url_encode(name, file, sizeof(file));
	int n = snprintf(url, capacity, "%s%s/%s", RESULTS_URL, encoded_path, file);
	return n > 0 && (size_t)n < capacity;
}

static size_t copy_name_part(char *out, size_t capacity, const char *source, size_t limit) {
	size_t used = 0;
	while (*source && used < limit && used + 1 < capacity) {
		unsigned char c = (unsigned char)*source;
		size_t width = 1;
		if ((c & 0xe0) == 0xc0) {
			width = 2;
		} else if ((c & 0xf0) == 0xe0) {
			width = 3;
		} else if ((c & 0xf8) == 0xf0) {
			width = 4;
		}
		if (used + width > limit || used + width >= capacity) {
			break;
		}
		if (width == 1 && strchr("/\\:*?\"<>|", c)) {
			out[used++] = '-';
			source++;
			continue;
		}
		memcpy(out + used, source, width);
		used += width;
		source += width;
	}
	out[used] = '\0';
	return used;
}

static void preset_name(const profile_t *item, char *out, size_t size) {
	if (!item->duplicate_name) {
		char model[101];
		copy_name_part(model, sizeof(model), item->name, 100);
		snprintf(out, size, "%s", model);
		return;
	}
	char model[67];
	char source[25];
	copy_name_part(model, sizeof(model), item->name, 66);
	copy_name_part(source, sizeof(source), item->source, 24);
	if (!source[0]) {
		snprintf(source, sizeof(source), "%s", "AutoEq");
	}
	snprintf(out, size, "%s [%s]", model, source);
}

static void cache_path(const profile_t *item, char *out, size_t size) {
	// Hash the profile name and source path to keep same-name profiles separate.
	uint32_t hash = hash_text(item->name) ^ hash_text(item->path);
	snprintf(out, size, "%08x.txt", hash);
}









// No mount reference is retained during HTTP or while a result waits for GUI.
#ifndef AUTOEQ_INTERNAL_ROOT
#define AUTOEQ_INTERNAL_ROOT "/usr/data"
#endif
#define AUTOEQ_UBIFS_MAGIC 0x24051905
#define AUTOEQ_STAGE_RESERVE (1024u * 1024u)
static uint64_t temp_serial;

static bool job_cancelled(void) {
	pthread_mutex_lock(&job_lock);
	bool closing = job_closing;
	pthread_mutex_unlock(&job_lock);
	return closing;
}

static bool job_stale(job_t *job) {
	if (job_cancelled())
		return true;
	if (storage_card_try_check(&job->card) == 0 || errno == EAGAIN)
		return false;
	snprintf(job->error, sizeof(job->error), "%s", tr("peq_autoeq_need_card"));
	return true;
}

// Refuse a known doomed cold transfer before HTTP or internal staging.
// This is not the final write lease: commit checks the same original token.
static bool write_preflight(job_t *job) {
	if (job_cancelled())
		return false;
	int pin = storage_pin_card_if(&job->card);
	if (pin < 0)
		return false;
	return close(pin) == 0;
}

static int directory_at(int parent, const char *name, bool create) {
	if (create && mkdirat(parent, name, 0700) != 0 && errno != EEXIST)
		return -1;
	return openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
}

static int autoeq_directory(int root, bool profiles, bool create) {
	int local = directory_at(root, ".local", create);
	if (local < 0)
		return -1;
	int autoeq = directory_at(local, "autoeq", create);
	int error = errno;
	if (close(local) != 0 && autoeq >= 0) {
		error = errno;
		close(autoeq);
		autoeq = -1;
	}
	if (autoeq < 0) {
		errno = error;
		return -1;
	}
	if (!profiles)
		return autoeq;
	int result = directory_at(autoeq, "profiles", create);
	error = errno;
	if (close(autoeq) != 0 && result >= 0) {
		error = errno;
		close(result);
		result = -1;
	}
	errno = error;
	return result;
}

static int owned_temp(int directory, char *name, size_t size, bool readable) {
	for (int attempt = 0; attempt < 8; attempt++) {
		if (temp_serial == UINT64_MAX) {
			errno = EOVERFLOW;
			return -1;
		}
		int n = snprintf(name, size, ".autoeq-%ld-%llu.tmp", (long)getpid(), (unsigned long long)++temp_serial);
		if (n < 0 || (size_t)n >= size) {
			errno = ENAMETOOLONG;
			return -1;
		}
		int fd = openat(directory, name, O_CREAT | O_EXCL | (readable ? O_RDWR : O_WRONLY) | O_CLOEXEC | O_NOFOLLOW, 0600);
		if (fd >= 0 || errno != EEXIST)
			return fd;
	}
	errno = EEXIST;
	return -1;
}

static bool write_all(int fd, const void *data, size_t size) {
	const char *bytes = data;
	while (size) {
		ssize_t count = write(fd, bytes, size);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return false;
		bytes += count;
		size -= (size_t)count;
	}
	return true;
}

// Uses only the granted directory. A published rename is never undone on a
// later directory-sync failure; callers record it but do not apply sound.
static bool publish_file(int directory, const char *name, const char *data, int input, size_t size, bool *published) {
	*published = false;
	char temporary[80];
	int fd = owned_temp(directory, temporary, sizeof(temporary), false);
	if (fd < 0)
		return false;
	bool ok = true;
	if (data) {
		ok = write_all(fd, data, size);
	} else {
		ok = lseek(input, 0, SEEK_SET) == 0;
		char buffer[8192];
		size_t remaining = size;
		while (ok && remaining) {
			size_t wanted = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
			ssize_t count = read(input, buffer, wanted);
			if (count < 0 && errno == EINTR)
				continue;
			ok = count > 0 && write_all(fd, buffer, (size_t)count);
			if (ok)
				remaining -= (size_t)count;
		}
	}
	if (ok && fsync(fd) != 0)
		ok = false;
	if (close(fd) != 0)
		ok = false;
	if (ok && job_cancelled())
		ok = false;
	if (ok) {
		ok = renameat(directory, temporary, directory, name) == 0;
		*published = ok;
	}
	if (!*published) {
		if (unlinkat(directory, temporary, 0) != 0)
			fprintf(stderr, "autoeq: temporary cleanup failed\n");
	} else if (fsync(directory) != 0)
		ok = false;
	if (*published && !ok)
		fprintf(stderr, "autoeq: file published; durability not confirmed\n");
	return ok;
}

static char *read_snapshot(int fd, size_t maximum, size_t *size_out) {
	struct stat st;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || (uint64_t)st.st_size > maximum || lseek(fd, 0, SEEK_SET) != 0)
		return NULL;
	size_t size = (size_t)st.st_size;
	char *text = malloc(size + 1);
	if (!text)
		return NULL;
	size_t total = 0;
	while (total < size) {
		ssize_t count = read(fd, text + total, size - total);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			free(text);
			return NULL;
		}
		total += (size_t)count;
	}
	text[size] = '\0';
	if (size_out)
		*size_out = size;
	return text;
}

static char *read_cache(job_t *job, bool profile, size_t *size_out) {
	int pin = storage_pin_card_read_if(&job->card);
	if (pin < 0) {
		snprintf(job->error, sizeof(job->error), "%s", tr("peq_autoeq_need_card"));
		return NULL;
	}
	int directory = autoeq_directory(pin, profile, false);
	int fd = directory >= 0 ? openat(directory, profile ? job->cache : "INDEX.md", O_RDONLY | O_CLOEXEC | O_NOFOLLOW) : -1;
	char *text = fd >= 0 ? read_snapshot(fd, profile ? PROFILE_MAX : INDEX_MAX, size_out) : NULL;
	bool ok = true;
	if (fd >= 0 && close(fd) != 0)
		ok = false;
	if (directory >= 0 && close(directory) != 0)
		ok = false;
	if (close(pin) != 0)
		ok = false;
	if (job_stale(job) || !ok) {
		free(text);
		text = NULL;
	}
	return text;
}

static bool stage_filesystem(int fd) {
	struct statfs type;
	if (fstatfs(fd, &type) != 0)
		return false;
#ifdef HOST_BUILD
	uint32_t kind = (uint32_t)type.f_type;
	return kind != 0x01021994u && kind != 0x858458f6u; // TMPFS and RAMFS
#else
	return (unsigned long)type.f_type == AUTOEQ_UBIFS_MAGIC;
#endif
}

#ifdef HOST_BUILD
// XDG cache paths are explicit host input, not a card or /tmp fallback.
// Validate every component before creating any directory.
static int host_cache_root(void) {
	const char *base = getenv("XDG_CACHE_HOME");
	bool home = !base || !base[0];
	if (home)
		base = getenv("HOME");
	if (!base || base[0] != '/' || strnlen(base, 1024) >= 1024) {
		errno = EINVAL;
		return -1;
	}
	for (const char *part = base + 1; *part;) {
		const char *end = strchr(part, '/');
		size_t length = end ? (size_t)(end - part) : strlen(part);
		if (!length || length > 255 || (length == 1 && part[0] == '.') || (length == 2 && part[0] == '.' && part[1] == '.')) {
			errno = EINVAL;
			return -1;
		}
		part += length;
		if (*part)
			part++;
	}
	int parent = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (parent < 0)
		return -1;
	for (const char *part = base + 1; *part;) {
		const char *end = strchr(part, '/');
		size_t length = end ? (size_t)(end - part) : strlen(part);
		char component[256];
		memcpy(component, part, length);
		component[length] = '\0';
		int child = directory_at(parent, component, false);
		if (child < 0 && errno == ENOENT && stage_filesystem(parent))
			child = directory_at(parent, component, true);
		if (close(parent) != 0 && child >= 0) {
			close(child);
			child = -1;
		}
		if (child < 0)
			return -1;
		parent = child;
		part += length;
		if (*part)
			part++;
	}
	if (!stage_filesystem(parent)) {
		close(parent);
		return -1;
	}
	if (home) {
		int child = directory_at(parent, ".cache", true);
		if (close(parent) != 0 && child >= 0) {
			close(child);
			child = -1;
		}
		if (child < 0)
			return -1;
		parent = child;
		if (!stage_filesystem(parent)) {
			close(parent);
			return -1;
		}
	}
	int root = directory_at(parent, "sonix", true);
	if (close(parent) != 0 && root >= 0) {
		close(root);
		root = -1;
	}
	return root;
}
#endif

static int internal_stage_root(void) {
#ifdef HOST_BUILD
	return host_cache_root();
#else
	return open(AUTOEQ_INTERNAL_ROOT, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
#endif
}

static int internal_stage(void) {
	int root = internal_stage_root();
	if (root < 0)
		return -1;
	struct statvfs space;
	bool ok = stage_filesystem(root) && fstatvfs(root, &space) == 0 && !(space.f_flag & ST_RDONLY);
	if (ok)
		ok = space.f_frsize && space.f_bavail >= ((uint64_t)INDEX_MAX + AUTOEQ_STAGE_RESERVE) / space.f_frsize + (((uint64_t)INDEX_MAX + AUTOEQ_STAGE_RESERVE) % space.f_frsize != 0);
	int directory = ok ? directory_at(root,
#ifdef HOST_BUILD
		"autoeq",
#else
		"sonix-autoeq",
#endif
		true) : -1;
	if (directory >= 0)
		ok = stage_filesystem(directory);
	else
		ok = false;
	char name[80];
	int fd = ok ? owned_temp(directory, name, sizeof(name), true) : -1;
	if (fd >= 0) {
		struct stat st;
		ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
		if (unlinkat(directory, name, 0) != 0) {
			ok = false;
			// One best-effort cleanup of this owned, still-empty inode only.
			if (unlinkat(directory, name, 0) != 0)
				fprintf(stderr, "autoeq: internal stage cleanup failed\n");
		}
		if (fstat(fd, &st) != 0 || st.st_nlink != 0)
			ok = false;
	} else
		ok = false;
	if (directory >= 0 && close(directory) != 0)
		ok = false;
	if (close(root) != 0)
		ok = false;
	if (!ok && fd >= 0) {
		close(fd);
		fd = -1;
	}
	return fd;
}

static int download_index(job_t *job, size_t *size_out) {
	if (!peqautoeq_network_ready()) {
		snprintf(job->error, sizeof(job->error), "%s", tr("peq_autoeq_only_works_over_wi_fi"));
		return -1;
	}
	if (!write_preflight(job)) {
		snprintf(job->error, sizeof(job->error), "%s", tr("peq_autoeq_error_database_save"));
		return -1;
	}
	int fd = internal_stage();
	if (fd < 0) {
		snprintf(job->error, sizeof(job->error), "%s", tr("peq_autoeq_error_database_save"));
		return -1;
	}
	http_stream_t stream;
	bool opened = http_stream_open(&stream, INDEX_URL, 35);
	bool ok = opened && stream.content_length <= (long)INDEX_MAX;
	size_t total = 0;
	char buffer[8192];
	while (ok && !job_stale(job)) {
		int count = http_stream_read(&stream, buffer, (int)sizeof(buffer));
		if (count <= 0) {
			ok = count == 0;
			break;
		}
		ok = (size_t)count <= INDEX_MAX - total && write_all(fd, buffer, (size_t)count);
		if (ok)
			total += (size_t)count;
	}
	if (opened) {
		if (stream.content_length > 0 && total != (size_t)stream.content_length)
			ok = false;
		http_stream_close(&stream);
	}
	ok = ok && total && !job_stale(job);
	if (ok && fsync(fd) != 0)
		ok = false;
	if (!ok) {
		close(fd);
		if (!job->error[0])
			snprintf(job->error, sizeof(job->error), "%s", tr("peq_autoeq_error_index_download"));
		return -1;
	}
	*size_out = total;
	return fd;
}

static bool commit_index(job_t *job, int input, size_t size) {
	if (job_cancelled())
		return false;
	int pin = storage_pin_card_if(&job->card);
	if (pin < 0)
		return false;
	int directory = autoeq_directory(pin, false, true);
	bool published = false;
	bool ok = directory >= 0 && publish_file(directory, "INDEX.md", NULL, input, size, &published);
	job->published = published;
	if (directory >= 0 && close(directory) != 0)
		ok = false;
	if (close(pin) != 0)
		ok = false;
	return ok;
}

static bool load_search_results(job_t *job) {
	char *index = read_cache(job, false, NULL);
	if (!index && !job->error[0] && !job_cancelled()) {
		size_t size = 0;
		int fd = download_index(job, &size);
		if (fd >= 0) {
			if (commit_index(job, fd, size))
				index = read_snapshot(fd, INDEX_MAX, NULL);
			if (close(fd) != 0) {
				free(index);
				index = NULL;
			}
		}
	}
	if (!index) {
		if (!job->error[0])
			snprintf(job->error, sizeof(job->error), "%s", tr("peq_autoeq_error_database_save"));
		return false;
	}
	bool ok = !job_stale(job);
	if (ok)
		parse_index(index, job->query, job);
	free(index);
	return ok;
}

static bool clear_profile_cache(job_t *job) {
	for (;;) {
		if (job_cancelled())
			return false;
		int pin = storage_pin_card_if(&job->card);
		if (pin < 0)
			return false;
		int fd = autoeq_directory(pin, true, false);
		bool missing = fd < 0 && errno == ENOENT;
		DIR *dir = fd >= 0 ? fdopendir(fd) : NULL;
		if (fd >= 0 && !dir)
			close(fd);
		bool ok = dir != NULL || missing;
		int removed = 0;
		while (dir && ok && removed < 16) {
			errno = 0;
			struct dirent *entry = readdir(dir);
			if (!entry) {
				ok = errno == 0;
				break;
			}
			if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
				continue;
			ok = unlinkat(dirfd(dir), entry->d_name, 0) == 0;
			if (ok)
				removed++;
		}
		if (dir && closedir(dir) != 0)
			ok = false;
		if (close(pin) != 0)
			ok = false;
		if (!ok || removed < 16)
			return ok;
	}
}

static bool update_index(job_t *job) {
	size_t size = 0;
	int fd = download_index(job, &size);
	if (fd < 0)
		return false;
	bool ok = commit_index(job, fd, size);
	if (close(fd) != 0)
		ok = false;
	if (!ok) {
		if (!job->error[0])
			snprintf(job->error, sizeof(job->error), "%s", tr("peq_autoeq_error_database_save"));
		return false;
	}
	if (!clear_profile_cache(job)) {
		snprintf(job->error, sizeof(job->error), "%s", tr("peq_autoeq_error_clear_profile_cache"));
		return false;
	}
	return true;
}

static bool commit_profile(job_t *job) {
	if (job_cancelled())
		return false;
	int pin = storage_pin_card_if(&job->card);
	if (pin < 0)
		return false;
	int cache = job->cached_profile ? -1 : autoeq_directory(pin, true, true);
	if (cache >= 0) {
		bool cached = false;
		(void)publish_file(cache, job->cache, job->profile_text, -1, job->profile_size, &cached);
		close(cache);
	}
	if (job_cancelled()) {
		close(pin);
		return false;
	}
	int directory = directory_at(pin, "PEQ", true);
	char name[sizeof(job->preset) + 5];
	int n = snprintf(name, sizeof(name), "%s.txt", job->preset);
	struct stat st;
	bool ok = directory >= 0 && n > 0 && (size_t)n < sizeof(name);
	if (ok) {
		int result = fstatat(directory, name, &st, AT_SYMLINK_NOFOLLOW);
		job->replaced = result == 0;
		ok = result == 0 || errno == ENOENT;
	}
	if (ok)
		ok = publish_file(directory, name, job->profile_text, -1, job->profile_size, &job->published);
	if (directory >= 0 && close(directory) != 0)
		ok = false;
	if (close(pin) != 0)
		ok = false;
	return ok;
}

static bool download_profile(job_t *job) {
	job->profile_text = read_cache(job, true, &job->profile_size);
	job->cached_profile = job->profile_text != NULL;
	if (!job->profile_text && !job->error[0] && !job_cancelled()) {
		char url[3072];
		if (!peqautoeq_network_ready())
			snprintf(job->error, sizeof(job->error), "%s", tr("peq_autoeq_only_works_over_wi_fi"));
		else if (!make_profile_url(&job->selected, url, sizeof(url)))
			snprintf(job->error, sizeof(job->error), "%s", tr("peq_autoeq_error_profile_url"));
		else if (!write_preflight(job))
			snprintf(job->error, sizeof(job->error), "%s", tr("peq_autoeq_error_save_preset"));
		else if (!http_get(url, &job->profile_text, &job->profile_size, PROFILE_MAX, 30)) {
			const char *error = http_last_error();
			snprintf(job->error, sizeof(job->error), "%s", error && error[0] ? error : tr("peq_autoeq_error_profile_download"));
		}
	}
	if (!job->profile_text || job->error[0])
		return false;
	if (job_stale(job))
		return false;
	errno = 0;
	if (!peq_preset_parse_text(job->profile_text, job->profile_size, &job->parsed)) {
		snprintf(job->error, sizeof(job->error), "%s", tr(errno == ENOMEM ? "out_of_memory" : "peq_autoeq_error_unsupported_profile"));
		return false;
	}
	if (!commit_profile(job)) {
		snprintf(job->error, sizeof(job->error), "%s", tr("peq_autoeq_error_save_preset"));
		return false;
	}
	return true;
}


static void free_job(job_t *job) {
	if (!job)
		return;
	free(job->profile_text);
	free(job);
}

static void report_status(const char *text, bool error, bool announce, uint64_t request_generation) {
	if (job_closing)
		return;
	if (announce && !toast_busy_showing()) {
		pending_notice[0] = '\0';
		if (error)
			toast_error(text);
		else
			toast_success(text);
	} else {
		snprintf(pending_notice, sizeof(pending_notice), "%s", text);
		pending_notice_error = error;
		pending_notice_generation = request_generation;
	}
}

static void *worker(void *user) {
	job_t *job = user;
	thread_be_background("autoeq");

	if (!job_cancelled()) {
		if (job->action == JOB_SEARCH)
			load_search_results(job);
		else if (job->action == JOB_UPDATE_DATABASE)
			update_index(job);
		else
			download_profile(job);
	}

	pthread_mutex_lock(&job_lock);
	if (job_closing) {
		job_busy = false;
		pthread_mutex_unlock(&job_lock);
		free_job(job);
		return NULL;
	}
	job->phase = JOB_READY;
	ready_job = job;
	pthread_mutex_unlock(&job_lock);
	// The notification owns no payload. Direct GUI service also works if full.
	(void)gui_post(job_done, NULL);
	return NULL;
}

// GUI-only, with no filesystem access. The first successful observation binds
// this not-yet-admitted request; an existing token is never refreshed.
static int admit_and_start(job_t *job) {
	char root[512];
	if (!storage_card_try_admit(&job->card, root, sizeof(root))) {
		if (errno == EAGAIN)
			return 0;
		snprintf(job->error, sizeof(job->error), "%s", tr("peq_autoeq_need_card"));
	} else {
		pthread_mutex_lock(&job_lock);
		ready_job = NULL;
		job->phase = JOB_WORKING;
		pthread_mutex_unlock(&job_lock);
		pthread_t thread;
		if (pthread_create(&thread, NULL, worker, job) == 0) {
			pthread_detach(thread);
			return 1;
		}
		snprintf(job->error, sizeof(job->error), "%s", tr("peq_autoeq_error_start"));
	}
	pthread_mutex_lock(&job_lock);
	if (ready_job == job)
		ready_job = NULL;
	job_busy = false;
	pthread_mutex_unlock(&job_lock);
	bool owned = toast_busy_end_owned(job->toast_owner);
	report_status(job->error, true, owned, job->request_generation);
	free_job(job);
	return -1;
}

static bool start_job(job_t *job, const char *busy_text) {
	if (!job)
		return false;
	if (!begin_job()) {
		free_job(job);
		return false;
	}
	pending_notice[0] = '\0';
	pending_results_generation = 0;
	if (job->action == JOB_SEARCH)
		job->request_generation = ++search_generation;
	job->toast_owner = toast_busy_owned(busy_text);
	job->phase = JOB_NEEDS_ADMISSION;
	pthread_mutex_lock(&job_lock);
	ready_job = job;
	pthread_mutex_unlock(&job_lock);
	return admit_and_start(job) >= 0;
}

static bool save_and_apply(job_t *job, bool announce) {
	if (!job->published || !peq_preset_apply_parsed(job->preset, &job->parsed, true)) {
		report_status(tr("peq_autoeq_error_save_preset"), true, announce, 0);
		return false;
	}
	bool replaced = job->replaced;
	int ignored = job->parsed.ignored;
	if (reload_cb) {
		reload_cb();
	}
	if (job_closing)
		return true;
	if (replaced && ignored) {
		char message[512];
		snprintf(message, sizeof(message), "%s %s", tr("peq_autoeq_unsupported_filters"), tr("peq_autoeq_profile_applied_replaced"));
		report_status(message, false, announce, 0);
	} else if (replaced) {
		report_status(tr("peq_autoeq_profile_applied_replaced"), false, announce, 0);
	} else if (ignored) {
		report_status(tr("peq_autoeq_unsupported_filters"), false, announce, 0);
	} else {
		report_status(tr("peq_autoeq_profile_applied"), false, announce, 0);
	}
	lv_obj_t *active = lv_screen_active();
	if (announce && !toast_busy_showing() && (active == search_screen || active == results_screen))
		switch_screen_return_to(peqpage_screen());
	return true;
}

static void rebuild_results(job_t *job, bool navigate) {
	result_count = job->result_count;
	memcpy(result_items, job->results, sizeof(result_items));
	lv_obj_clean(search_list);
	for (int i = 0; i < result_count; i++) {
		lv_obj_t *row = settingsrow_add(search_list, result_items[i].display, NULL, result_clicked, (void *)(intptr_t)i);
		settingsrow_name_lines(row, 2);
	}
	if (result_count) {
		lv_obj_set_hidden(results_empty, true);
	} else {
		lv_obj_set_hidden(results_empty, false);
	}
	if (navigate)
		switch_screen(results_screen);
}

static void service_deferred(void) {
	if (job_closing)
		return;
	if (pending_notice_generation && pending_notice_generation != search_generation)
		pending_notice[0] = '\0';
	if (pending_results_generation != search_generation)
		pending_results_generation = 0;
	if (!pending_notice[0] && !pending_results_generation)
		return;
	lv_obj_t *active = lv_screen_active();
	if (!active || (active != search_screen && active != results_screen) || toast_busy_showing())
		return;
	if (pending_notice[0]) {
		if (pending_notice_error)
			toast_error(pending_notice);
		else
			toast_success(pending_notice);
		pending_notice[0] = '\0';
	}
	if (pending_results_generation && active == search_screen) {
		pending_results_generation = 0;
		switch_screen(results_screen);
	}
}

void peqautoeq_service_completion(void) {
	pthread_mutex_lock(&job_lock);
	if (job_closing) {
		pthread_mutex_unlock(&job_lock);
		return;
	}
	job_t *job = ready_job;
	pthread_mutex_unlock(&job_lock);
	if (!job) {
		service_deferred();
		return;
	}
	bool stale_search = job->action == JOB_SEARCH && (job->request_generation != search_generation || !search_screen);
	if (!stale_search && job->phase == JOB_NEEDS_ADMISSION) {
		(void)admit_and_start(job);
		return;
	}
	if (!stale_search && !job->error[0]) {
		if (storage_card_try_check(&job->card) != 0) {
			if (errno == EAGAIN)
				return;
			snprintf(job->error, sizeof(job->error), "%s", tr("peq_autoeq_need_card"));
		}
	}
	if (!stale_search && job->action == JOB_SEARCH && lv_screen_active() != search_screen)
		stale_search = true;
	pthread_mutex_lock(&job_lock);
	ready_job = NULL;
	job_busy = false;
	pthread_mutex_unlock(&job_lock);
	bool owned = toast_busy_end_owned(job->toast_owner);
	if (stale_search) {
		// Disposal is always allowed, including when storage is busy or gone.
	} else if (job->error[0]) {
		report_status(job->error, true, owned, job->request_generation);
	} else if (job->action == JOB_SEARCH) {
		bool navigate = owned && !toast_busy_showing();
		rebuild_results(job, navigate);
		if (!navigate)
			pending_results_generation = job->request_generation;
	} else if (job->action == JOB_UPDATE_DATABASE) {
		report_status(tr("peq_autoeq_database_updated"), false, owned, 0);
	} else {
		save_and_apply(job, owned);
	}
	free_job(job);
	service_deferred();
}

static void job_done(void *user) {
	(void)user;
	peqautoeq_service_completion();
}

void peqautoeq_shutdown_completions(void) {
	pthread_mutex_lock(&job_lock);
	job_closing = true;
	job_t *job = ready_job;
	ready_job = NULL;
	if (job)
		job_busy = false;
	pthread_mutex_unlock(&job_lock);
	free_job(job);
	search_generation++;
	pending_results_generation = 0;
	pending_notice[0] = '\0';
}

static void search_left(lv_event_t *event) {
	search_generation++;
	pending_results_generation = 0;
	if (pending_notice_generation)
		pending_notice[0] = '\0';
	if (lv_event_get_code(event) == LV_EVENT_DELETE)
		search_screen = NULL;
}

static void result_clicked(lv_event_t *event) {
	if (switcher_back_drag_active() || is_busy()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(event);
	if (index < 0 || index >= result_count) {
		return;
	}

	job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		toast_error(tr("out_of_memory"));
		return;
	}
	job->action = JOB_DOWNLOAD_PROFILE;
	job->selected = result_items[index];
	preset_name(&job->selected, job->preset, sizeof(job->preset));
	cache_path(&job->selected, job->cache, sizeof(job->cache));
	start_job(job, tr("peq_autoeq_downloading_profile"));
}

static void search_accept(lv_event_t *event) {
	(void)event;
	char query[128];
	snprintf(query, sizeof(query), "%s", lv_textarea_get_text(search_field));
	char *start = query;
	while (isspace((unsigned char)*start)) {
		start++;
	}
	size_t length = strlen(start);
	while (length && isspace((unsigned char)start[length - 1])) {
		start[--length] = '\0';
	}
	if (!*start) {
		toast_error(tr("peq_autoeq_enter_name"));
		return;
	}

	job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		toast_error(tr("out_of_memory"));
		return;
	}
	job->action = JOB_SEARCH;
	snprintf(job->query, sizeof(job->query), "%s", start);
	start_job(job, tr("peq_autoeq_downloading_database"));
}

bool peqautoeq_network_ready(void) {
#ifdef HOST_BUILD
	return true;
#else
	wifi_status_t status;
	wifi_get_status(&status);
	return status.state == WIFI_STATE_CONNECTED && status.ip[0] != '\0';
#endif
}

bool peqautoeq_network_needed(void) {
	if (peqautoeq_network_ready()) {
		return false;
	}
	gui_notify_popup("peq_autoeq_only_works_over_wi_fi");
	return true;
}

static void update_database(lv_event_t *event) {
	(void)event;
	if (is_busy() || peqautoeq_network_needed()) {
		return;
	}
	job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		toast_error(tr("out_of_memory"));
		return;
	}
	job->action = JOB_UPDATE_DATABASE;
	start_job(job, tr("peq_autoeq_downloading_database"));
}

static void build_pages(gui_config_t *config) {
	lv_obj_t *search_container = settingsrow_page(search_screen, config, "peq_autoeq_search");
	lv_obj_set_height(search_container, config->screen_height - settingsrow_content_top(config) - 316);

	search_field = lv_textarea_create(search_container);
	lv_textarea_set_one_line(search_field, true);
	lv_textarea_set_max_length(search_field, 120);
	lv_textarea_set_placeholder_text(search_field, tr("peq_autoeq_headphones"));
	lv_obj_set_size(search_field, lv_pct(100), 62);
	lv_obj_add_style(search_field, &theme_style_card, 0);
	// The 2.0 input: a pill on the surface.
	lv_obj_add_style(search_field, &sx_style_surface, 0);
	lv_obj_set_style_radius(search_field, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_border_width(search_field, 0, 0);
	lv_obj_set_style_pad_all(search_field, 14, 0);
	lv_obj_set_style_pad_hor(search_field, 22, 0);
	lv_obj_set_style_text_font(search_field, &font_ui_24, 0);
	keyboard_style_caret(search_field);

	// The database download sits in the title's corner.
	settingsrow_title_corner_slots(settingsrow_page_title(search_screen), config, 1);
	lv_obj_t *update_btn = lv_btn_create(search_screen);
	lv_obj_set_size(update_btn, 56, 56);
	lv_obj_set_style_bg_opa(update_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(update_btn, 0, 0);
	lv_obj_set_style_shadow_width(update_btn, 0, 0);
	lv_obj_set_style_pad_all(update_btn, 0, 0);
	lv_obj_align(update_btn, LV_ALIGN_TOP_RIGHT, -config->padding, config->padding + config->top_bar_height);
	lv_obj_add_event_cb(update_btn, update_database, LV_EVENT_CLICKED, NULL);
	lv_obj_t *update_icon = lv_image_create(update_btn);
	lv_image_set_src(update_icon, &icon_autoeq_update);
	lv_obj_add_style(update_icon, &theme_style_icon, 0);
	lv_obj_center(update_icon);

	search_keyboard = keyboard_create(search_screen, config->screen_width, 316, search_field, &icon_search, NULL, search_accept, NULL);

	search_list = settingsrow_page(results_screen, config, "peq_autoeq_results");
	results_empty = lv_label_create(results_screen);
	lv_label_set_text(results_empty, tr("peq_autoeq_no_results"));
	lv_obj_add_style(results_empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(results_empty, &font_ui_24, 0);
	lv_obj_set_width(results_empty, config->screen_width - 2 * config->padding);
	lv_obj_set_style_text_align(results_empty, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(results_empty, LV_ALIGN_TOP_MID, 0, settingsrow_content_top(config) + 100);
	lv_obj_set_hidden(results_empty, true);
}

// The two screens exist from boot, their contents from the first search (see
// gui_page_defer): AutoEq is a once-in-a-while errand and the search page alone
// carries a whole keyboard. Every way in goes through peqautoeq_open(), which
// asks for the build first; the results page is only ever switched to from a
// search started there, so it is built by then too.
void peqautoeq_init(gui_config_t *config) {
	cfg = config;
	search_screen = lv_obj_create(NULL);
	results_screen = lv_obj_create(NULL);
	gui_page_defer(search_screen, build_pages);
	lv_obj_add_event_cb(search_screen, search_left, LV_EVENT_SCREEN_UNLOAD_START, NULL);
	lv_obj_add_event_cb(search_screen, search_left, LV_EVENT_DELETE, NULL);
}

lv_obj_t *peqautoeq_screen(void) { return search_screen; }

void peqautoeq_set_reload_cb(void (*cb)(void)) { reload_cb = cb; }

void peqautoeq_open(void) {
	if (!search_screen || job_closing)
		return;
	search_generation++;
	pending_results_generation = 0;
	if (pending_notice_generation)
		pending_notice[0] = '\0';
	gui_page_ensure(search_screen); // the field below is filled before the switch
	lv_textarea_set_text(search_field, "");
	keyboard_reset(search_keyboard);
	switch_screen(search_screen);
	lv_obj_add_state(search_field, LV_STATE_FOCUSED);
	lv_obj_send_event(search_field, LV_EVENT_FOCUSED, NULL);
}
