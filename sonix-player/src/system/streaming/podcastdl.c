#define _GNU_SOURCE
#include "podcastdl.h"
#include "podcastpair.h"
#include "src/system/core/sha256.h"
#include <dirent.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include "src/system/core/utils.h"
#include "src/system/device/system.h"
#include "src/system/net/http.h"
#include "src/system/streaming/podcaststate.h"

#define DL_TIMEOUT_SECS 20
#define DL_CHUNK 32768
#define DL_NAME_MAX 150 // bytes of a file or folder name, before the extension

typedef struct {
	unsigned state_epoch;
	podcast_episode_t episode;
	char feed_title[PODCAST_TITLE_MAX];
	char feed_author[PODCAST_AUTHOR_MAX];
	char folder[512];
	char path[768];
	char image[PODCAST_URL_MAX];
	char show[256], file[180], staging[240];
	sha256_ctx sha;
	long long saved_bytes;
	bool recovery;
	bool (*recovery_cancel)(void *user);
	void *recovery_user;
	dev_t root_dev, part_dev;
	ino_t root_ino, part_ino;
	bool part_created;
	bool show_bound;
	dev_t show_dev;
	ino_t show_ino;
	bool tags_owned;
	struct stat tags_stat;
} job_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static podcastdl_state_t state = PODCASTDL_IDLE;
static podcastpair_result_t last_pair_result = PODCASTPAIR_OK;
static long long bytes_done;
static long long bytes_total;
static _Atomic bool cancel_asked;
static unsigned temp_serial;
static http_stream_t *active_stream; // guarded by `lock`; NULL outside the body loop

bool podcastdl_root(char *out, size_t size) {
	if (!out || !size) {
		return false;
	}
	out[0] = '\0';
	// storage_sd_root's string is fixed at startup; availability can change.
	// This is only a path snapshot, never storage admission or an epoch.
	const char *sd = storage_sd_root();
	if (!sd || !sd[0]) {
		return false;
	}
	int n = snprintf(out, size, "%s/%s", sd, PODCASTDL_FOLDER);
	if (n < 0 || (size_t)n >= size) {
		out[0] = '\0';
		return false;
	}
	return true;
}

// The same mapping as podcastcache.c: the decoder picks its path from the
// extension, and a feed that declares nothing is almost always MP3.
static const char *extension_for(const char *mime) {
	if (!mime || !*mime) {
		return "mp3";
	}
	if (strstr(mime, "mp4") || strstr(mime, "m4a") || strstr(mime, "aac")) {
		return "m4a";
	}
	if (strstr(mime, "ogg") || strstr(mime, "opus")) {
		return "opus";
	}
	if (strstr(mime, "wav")) {
		return "wav";
	}
	if (strstr(mime, "flac")) {
		return "flac";
	}
	return "mp3";
}

void podcastdl_safe_name(const char *title, const char *fallback, char *out, size_t size) {
	if (!out || size == 0) {
		return;
	}
	size_t cap = size - 1 < DL_NAME_MAX ? size - 1 : DL_NAME_MAX;
	size_t n = 0;
	for (const unsigned char *p = (const unsigned char *)(title ? title : ""); *p && n < cap; p++) {
		unsigned char c = *p;
		if (c < 0x20 || strchr("/\\*?\"<>|", c)) {
			continue;
		}
		// "Part 1: The start" becomes "Part 1 - The start", "12:30" becomes "12-30".
		if (c == ':') {
			if (p[1] == ' ' && n + 2 <= cap) {
				out[n++] = ' ';
			}
			c = '-';
		}
		out[n++] = (char)c;
	}
	// A cut inside a multi-byte character leaves the lead bytes of it behind.
	if (n == cap) {
		while (n > 0 && ((unsigned char)out[n - 1] & 0xC0) == 0x80) {
			n--;
		}
		if (n > 0 && ((unsigned char)out[n - 1] & 0x80)) {
			n--;
		}
	}
	out[n] = '\0';

	// FAT drops trailing dots and spaces on its own, which would make the name
	// on the card differ from the one asked for.
	while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '.')) {
		out[--n] = '\0';
	}
	size_t lead = 0;
	while (out[lead] == ' ' || out[lead] == '.') {
		lead++;
	}
	if (lead) {
		memmove(out, out + lead, n - lead + 1);
	}
	if (!out[0]) {
		snprintf(out, size, "%s", fallback ? fallback : "Podcast");
	}
}

static bool folder_of(const char *base, const char *feed_title, char *out, size_t size) {
	char name[DL_NAME_MAX + 1];
	podcastdl_safe_name(feed_title, "Podcast", name, sizeof(name));
	int n = snprintf(out, size, "%s/%s", base, name);
	if (n < 0 || (size_t)n >= size) {
		out[0] = '\0';
		return false;
	}
	return true;
}

static bool path_in_folder(const podcast_episode_t *episode, const char *folder, char *out, size_t size) {
	char fallback[32];
	snprintf(fallback, sizeof(fallback), "%lld", episode->id);
	char name[DL_NAME_MAX + 1];
	podcastdl_safe_name(episode->title, fallback, name, sizeof(name));
	int n = snprintf(out, size, "%s/%s.%s", folder, name, extension_for(episode->mime));
	if (n < 0 || (size_t)n >= size) {
		out[0] = '\0';
		return false;
	}
	return true;
}

bool podcastdl_path(const podcast_episode_t *episode, const char *feed_title, char *out, size_t size) {
	if (out && size) {
		out[0] = '\0';
	}
	if (!episode || !out || !size) {
		return false;
	}
	char base[512], folder[512];
	return podcastdl_root(base, sizeof(base)) && folder_of(base, feed_title && feed_title[0] ? feed_title : episode->feed_title, folder, sizeof(folder)) && path_in_folder(episode, folder, out, size);
}

// Native sidecars identify an RSS episode. Markerless files without that
// field remain ordinary user files; malformed/mismatched native IDs do not
// make another same-title episode look downloaded or authorize its overwrite.
static bool native_id_at(int dir, const char *file, long long wanted) {
	char tags[256];
	int n = snprintf(tags, sizeof(tags), "%s.tags", file);
	if (n < 0 || (size_t)n >= sizeof(tags))
		return false;
	int fd = openat(dir, tags, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return errno == ENOENT;
	struct stat st;
	bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0 && st.st_size <= 16384;
	FILE *f = ok ? fdopen(fd, "r") : NULL;
	if (!f) {
		close(fd);
		return false;
	}
	char line[1024];
	bool beginning = true, match = false;
	unsigned ids = 0;
	size_t read_bytes = 0;
	for (;;) {
		memset(line, 0xff, sizeof(line));
		if (!fgets(line, sizeof(line), f))
			break;
		size_t length = strlen(line);
		if (memchr(line + length + 1, 0, sizeof(line) - length - 1)) {
			ok = false;
			break;
		}
		read_bytes += length;
		if (read_bytes > 16384) {
			ok = false;
			break;
		}
		if (beginning && !strncmp(line, "episode_id=", 11)) {
			ids++;
			char *value = line + 11, *end;
			errno = 0;
			long long id = strtoll(value, &end, 10);
			match = ids == 1 && *value >= '0' && *value <= '9' && errno != ERANGE && id > 0 && id == wanted && (!*end || !strcmp(end, "\n") || !strcmp(end, "\r\n"));
		}
		beginning = strchr(line, '\n') != NULL;
	}
	ok = ok && !ferror(f);
	if (fclose(f) != 0)
		ok = false;
	return ok && (!ids || match);
}

bool podcastdl_exists(const podcast_episode_t *episode, const char *feed_title) {
	char path[768];
	struct stat st;
	if (!podcastdl_path(episode, feed_title, path, sizeof(path)) || stat(path, &st) != 0 || st.st_size <= 0 || !podcastpair_ready(path))
		return false;
	const char *file = strrchr(path, '/') + 1;
	char folder[768];
	size_t n = (size_t)(file - path - 1);
	memcpy(folder, path, n);
	folder[n] = 0;
	int dir = open(folder, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (dir < 0)
		return false;
	bool ready = native_id_at(dir, file, episode->id);
	close(dir);
	return ready;
}

static void set_state(podcastdl_state_t s) {
	pthread_mutex_lock(&lock);
	state = s;
	pthread_mutex_unlock(&lock);
}

// No descriptor survives a network wait. A short pin prevents ordinary USB
// unmount during local I/O; relative lookup also survives a forced/lazy detach
// without resolving a replacement card through the old absolute pathname.
static int job_dir(const job_t *job, bool create, bool cleanup) {
	if ((!cleanup && cancel_asked) || job->state_epoch != podcaststate_card_epoch()) {
		errno = cancel_asked ? ECANCELED : ESTALE;
		return -1;
	}
	char root[512];
	int pin = storage_pin_card(root, sizeof(root));
	struct stat st;
	if (pin < 0)
		return -1;
	if (job->state_epoch != podcaststate_card_epoch() || fstat(pin, &st) != 0 || st.st_dev != job->root_dev || st.st_ino != job->root_ino || (!cleanup && cancel_asked)) {
		close(pin);
		errno = ESTALE;
		return -1;
	}
	if (create && mkdirat(pin, PODCASTDL_FOLDER, 0755) != 0 && errno != EEXIST) {
		close(pin);
		return -1;
	}
	int parent = openat(pin, PODCASTDL_FOLDER, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	close(pin);
	if (parent < 0)
		return -1;
	if (create && mkdirat(parent, job->show, 0755) != 0 && errno != EEXIST) {
		close(parent);
		return -1;
	}
	int dir = job->show[0] ? openat(parent, job->show, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : fcntl(parent, F_DUPFD_CLOEXEC, 0);
	close(parent);
	if (dir >= 0 && job->show_bound) {
		struct stat st;
		if (fstat(dir, &st) != 0 || st.st_dev != job->show_dev || st.st_ino != job->show_ino) {
			close(dir);
			dir = -1;
		}
	}
	return dir;
}

static bool same_part(const job_t *job, int fd) {
	struct stat st;
	return fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_dev == job->part_dev && st.st_ino == job->part_ino;
}

static bool create_part(job_t *job) {
	int dir = job_dir(job, true, false);
	if (dir < 0)
		return false;
	struct stat show;
	if (fstat(dir, &show) != 0) {
		close(dir);
		return false;
	}
	job->show_dev = show.st_dev;
	job->show_ino = show.st_ino;
	job->show_bound = true;
	int fd = -1;
	for (int attempt = 0; attempt < 16; attempt++) {
		unsigned serial = __atomic_add_fetch(&temp_serial, 1, __ATOMIC_RELAXED);
		snprintf(job->staging, sizeof(job->staging), ".%s-%ld-%u.part", job->file, (long)getpid(), serial);
		fd = openat(dir, job->staging, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
		if (fd >= 0 || errno != EEXIST)
			break;
	}
	struct stat st;
	bool ok = fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
	if (ok) {
		job->part_dev = st.st_dev;
		job->part_ino = st.st_ino;
		job->part_created = true;
	}
	if (fd >= 0) {
		if (!ok)
			unlinkat(dir, job->staging, 0);
		close(fd);
	}
	close(dir);
	return ok;
}

static bool append_part(job_t *job, const char *buf, size_t size, long long have) {
	int dir = job_dir(job, false, false);
	if (dir < 0)
		return false;
	int fd = openat(dir, job->staging, O_WRONLY | O_APPEND | O_NOFOLLOW | O_CLOEXEC);
	struct stat st;
	bool ok = same_part(job, fd) && fstat(fd, &st) == 0 && st.st_size == have;
	close(dir);
	while (ok && size) {
		ssize_t n = write(fd, buf, size);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			ok = false;
			break;
		}
		sha256_update(&job->sha, buf, (size_t)n);
		job->saved_bytes += n;
		buf += n;
		size -= (size_t)n;
	}
	if (fd >= 0 && close(fd) != 0)
		ok = false;
	return ok;
}

static void cleanup_part(const job_t *job) {
	if (!job->part_created)
		return;
	int dir = job_dir(job, false, true);
	if (dir < 0)
		return;
	int fd = openat(dir, job->staging, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (same_part(job, fd))
		unlinkat(dir, job->staging, 0);
	if (fd >= 0)
		close(fd);
	close(dir);
}

static bool download_body(job_t *job) {
	sha256_init(&job->sha);
	http_stream_t *st = calloc(1, sizeof(*st));
	if (!st)
		return false;
	if (!http_stream_open(st, job->episode.enclosure, DL_TIMEOUT_SECS)) {
		free(st);
		return false;
	}
	pthread_mutex_lock(&lock);
	bytes_total = st->content_length > 0 ? st->content_length : 0;
	active_stream = st;
	if (cancel_asked)
		http_stream_wake(st);
	pthread_mutex_unlock(&lock);

	char *buf = malloc(DL_CHUNK);
	bool ok = buf && create_part(job);
	long long done = 0;
	while (ok && !cancel_asked) {
		int n = http_stream_read(st, buf, DL_CHUNK);
		if (n < 0) {
			ok = false;
			break;
		}
		if (!n)
			break;
		ok = append_part(job, buf, (size_t)n, done);
		if (ok) {
			done += n;
			pthread_mutex_lock(&lock);
			bytes_done = done;
			pthread_mutex_unlock(&lock);
		}
	}
	free(buf);
	ok = ok && !cancel_asked && done > 0 && (st->content_length <= 0 || done == st->content_length);
	pthread_mutex_lock(&lock);
	active_stream = NULL;
	pthread_mutex_unlock(&lock);
	http_stream_close(st);
	free(st);
	return ok;
}

// Artwork is optional. Fetch without a card pin, then admit a fresh local
// transaction; cancellation/detach cannot turn its late response into a write.
static void fetch_cover(const job_t *job) {
	if (!job->image[0] || cancel_asked)
		return;
	int dir = job_dir(job, false, false);
	if (dir < 0)
		return;
	struct stat st;
	bool exists = fstatat(dir, "cover.jpg", &st, AT_SYMLINK_NOFOLLOW) == 0 || fstatat(dir, "cover.png", &st, AT_SYMLINK_NOFOLLOW) == 0;
	close(dir);
	if (exists)
		return;
	char *body = NULL;
	size_t len = 0;
	if (!http_get(job->image, &body, &len, 8 * 1024 * 1024, DL_TIMEOUT_SECS) || !body || len < 8) {
		free(body);
		return;
	}
	const unsigned char *b = (const unsigned char *)body;
	const char *dest = b[0] == 0xFF && b[1] == 0xD8 ? "cover.jpg" : b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G' ? "cover.png" : NULL;
	dir = dest ? job_dir(job, false, false) : -1;
	if (dir >= 0) {
		char tmp[80];
		snprintf(tmp, sizeof(tmp), ".cover-%ld-%u.tmp", (long)getpid(), __atomic_add_fetch(&temp_serial, 1, __ATOMIC_RELAXED));
		int fd = openat(dir, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
		if (fd >= 0) {
			FILE *f = fdopen(fd, "wb");
			bool ok = f && fwrite(body, 1, len, f) == len && fflush(f) == 0 && fsync(fileno(f)) == 0;
			if (f ? fclose(f) != 0 : close(fd) != 0)
				ok = false;
			if (!ok || cancel_asked || job->state_epoch != podcaststate_card_epoch() || renameat(dir, tmp, dir, dest) != 0)
				unlinkat(dir, tmp, 0);
			else
				fsync(dir);
		}
		close(dir);
	}
	free(body);
}

// The sidecar podcastcache.c writes for an episode it streams, with the id of
// the episode added: here the file name is its title, not its id.
static void tag_field(FILE *f, const char *key, const char *value) {
	fprintf(f, "%s=", key);
	for (const unsigned char *p = (const unsigned char *)value; *p; p++)
		fputc(*p < 32 ? ' ' : *p, f);
	fputc('\n', f);
}

static bool prepare_tags(job_t *job, int dir, char *tmp, size_t size) {
	char tags[200];
	snprintf(tags, sizeof(tags), "%s.tags", job->file);
	snprintf(tmp, size, ".%s-%ld-%u.tmp", tags, (long)getpid(), __atomic_add_fetch(&temp_serial, 1, __ATOMIC_RELAXED));
	int fd = openat(dir, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
	if (!f) {
		if (fd >= 0) {
			close(fd);
			unlinkat(dir, tmp, 0);
		}
		tmp[0] = 0;
		return false;
	}
	const podcast_episode_t *e = &job->episode;
	tag_field(f, "title", e->title);
	tag_field(f, "artist", job->feed_title);
	tag_field(f, "album", job->feed_title);
	fprintf(f, "feed_id=%lld\n", e->feed_id);
	tag_field(f, "feed_author", job->feed_author);
	tag_field(f, "feed_image", job->image);
	tag_field(f, "cover_url", e->image);
	fprintf(f, "episode_id=%lld\n", e->id);
	bool ok = !ferror(f) && fflush(f) == 0 && fsync(fileno(f)) == 0;
	if (ok) {
		ok = fstat(fileno(f), &job->tags_stat) == 0;
		job->tags_owned = ok;
	}
	if (fclose(f) != 0)
		ok = false;
	if (!ok)
		unlinkat(dir, tmp, 0);
	return ok;
}

static int pair_open_dir(void *user) {
	job_t *job = user;
	int dir = job_dir(job, false, true);
	if (dir >= 0 && !job->show_bound) {
		struct stat st;
		if (fstat(dir, &st) != 0) {
			close(dir);
			return -1;
		}
		job->show_dev = st.st_dev;
		job->show_ino = st.st_ino;
		job->show_bound = true;
	}
	return dir;
}
static bool pair_cancelled(void *user) {
	job_t *job = user;
	return job->recovery ? job->recovery_cancel && job->recovery_cancel(job->recovery_user) : cancel_asked;
}

static void *download_main(void *arg) {
	job_t *job = arg;
	thread_be_low_priority("podcast save"); // the card on screen is waiting for it

	podcastpair_result_t paired = PODCASTPAIR_FAILED;
	bool new_record_possible = false;
	bool allowed = !cancel_asked;
	if (allowed) {
		int existing = pair_open_dir(job);
		if (existing >= 0) {
			bool pending = podcastpair_pending_fd(existing);
			close(existing);
			if (pending) {
				podcastpair_io_t io = {.open_dir = pair_open_dir, .cancelled = pair_cancelled, .user = job};
				paired = podcastpair_recover(&io);
				allowed = paired == PODCASTPAIR_OK;
			}
			if (allowed) {
				int check = pair_open_dir(job);
				if (check < 0 || !native_id_at(check, job->file, job->episode.id)) {
					allowed = false;
					paired = PODCASTPAIR_CONFLICT;
				}
				if (check >= 0)
					close(check);
			}
		} else if (errno != ENOENT)
			allowed = false;
	}
	bool ok = allowed && !cancel_asked && download_body(job);
	int dir = ok ? job_dir(job, false, false) : -1;
	int fd = dir >= 0 ? openat(dir, job->staging, O_RDONLY | O_NOFOLLOW | O_CLOEXEC) : -1;
	ok = ok && same_part(job, fd);
	if (ok && job->episode.published > 0) {
		struct timespec times[2] = {{.tv_sec = job->episode.published}, {.tv_sec = job->episode.published}};
		ok = futimens(fd, times) == 0;
	}
	char tags_tmp[240] = "";
	if (ok && !native_id_at(dir, job->file, job->episode.id)) {
		ok = false;
		paired = PODCASTPAIR_CONFLICT;
	}
	if (ok)
		ok = fsync(fd) == 0 && prepare_tags(job, dir, tags_tmp, sizeof(tags_tmp));
	if (fd >= 0)
		close(fd);
	if (dir >= 0)
		close(dir);
	if (ok) {
		podcastpair_audio_t audio = {.dev = job->part_dev, .ino = job->part_ino, .size = job->saved_bytes};
		sha256_final_hex(&job->sha, audio.sha256);
		podcastpair_io_t io = {.open_dir = pair_open_dir, .cancelled = pair_cancelled, .user = job};
		new_record_possible = true;
		paired = podcastpair_publish(&io, job->staging, tags_tmp, job->file, &audio);
		ok = paired == PODCASTPAIR_OK;
	}
	bool recovery = paired == PODCASTPAIR_RECOVERY || paired == PODCASTPAIR_CONFLICT || paired == PODCASTPAIR_BUSY;
	if (!ok && (!recovery || !new_record_possible)) {
		cleanup_part(job);
		// The tag temporary was created O_EXCL by this worker; pair rollback may
		// already have removed it. Never remove any final/backup by absolute path.
		if (tags_tmp[0] && job->tags_owned) {
			dir = job_dir(job, false, true);
			if (dir >= 0) {
				int tags = openat(dir, tags_tmp, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
				struct stat st;
				if (tags >= 0 && fstat(tags, &st) == 0 && S_ISREG(st.st_mode) && st.st_dev == job->tags_stat.st_dev && st.st_ino == job->tags_stat.st_ino && st.st_size == job->tags_stat.st_size && st.st_mtim.tv_sec == job->tags_stat.st_mtim.tv_sec && st.st_mtim.tv_nsec == job->tags_stat.st_mtim.tv_nsec)
					unlinkat(dir, tags_tmp, 0);
				if (tags >= 0)
					close(tags);
				close(dir);
			}
		}
	} else if (ok) {
		podcaststate_downloaded(job->episode.id, job->path, job->state_epoch);
		fetch_cover(job);
	}
	podcastpair_release(job);

	printf("podcastdl: %s %s\n", job->path, ok ? "complete" : cancel_asked ? "cancelled" : "failed");
	pthread_mutex_lock(&lock);
	last_pair_result = paired;
	state = ok ? PODCASTDL_DONE : recovery ? PODCASTDL_RECOVERY_REQUIRED : cancel_asked ? PODCASTDL_CANCELLED : PODCASTDL_FAILED;
	pthread_mutex_unlock(&lock);
	free(job);
	return NULL;
}

bool podcastdl_owns(const char *path) {
	char base[512];
	if (!path || !podcastdl_root(base, sizeof(base))) {
		return false;
	}
	size_t n = strlen(base);
	return strncasecmp(path, base, n) == 0 && path[n] == '/';
}

bool podcastdl_start(const podcast_episode_t *episode, const char *feed_title, const char *feed_author, const char *feed_image) {
	if (!episode || !episode->enclosure[0]) {
		return false;
	}
	pthread_mutex_lock(&lock);
	bool busy = state == PODCASTDL_RUNNING;
	if (!busy) {
		state = PODCASTDL_RUNNING;
		last_pair_result = PODCASTPAIR_OK;
		bytes_done = bytes_total = 0;
		cancel_asked = false;
	}
	pthread_mutex_unlock(&lock);
	if (busy) {
		return false;
	}

	job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		set_state(PODCASTDL_FAILED);
		return false;
	}
	job->state_epoch = podcaststate_card_epoch();
	job->episode = *episode;
	const char *title = feed_title && feed_title[0] ? feed_title : episode->feed_title;
	snprintf(job->feed_title, sizeof(job->feed_title), "%s", title);
	snprintf(job->feed_author, sizeof(job->feed_author), "%s", feed_author ? feed_author : "");
	char root[512], base[512];
	int pin = storage_pin_card(root, sizeof(root));
	struct stat st;
	bool admitted = pin >= 0 && fstat(pin, &st) == 0 && job->state_epoch == podcaststate_card_epoch();
	if (pin >= 0)
		close(pin);
	int n = admitted ? snprintf(base, sizeof(base), "%s/" PODCASTDL_FOLDER, root) : -1;
	if (n < 0 || (size_t)n >= sizeof(base) || !folder_of(base, title, job->folder, sizeof(job->folder)) || !path_in_folder(episode, job->folder, job->path, sizeof(job->path))) {
		free(job);
		set_state(PODCASTDL_FAILED);
		return false;
	}
	job->root_dev = st.st_dev;
	job->root_ino = st.st_ino;
	podcastdl_safe_name(title, "Podcast", job->show, sizeof(job->show));
	snprintf(job->file, sizeof(job->file), "%s", strrchr(job->path, '/') + 1);
	snprintf(job->image, sizeof(job->image), "%s", feed_image && feed_image[0] ? feed_image : episode->image);

	if (!podcastpair_reserve(job)) {
		free(job);
		set_state(PODCASTDL_IDLE);
		return false;
	}
	pthread_t thread;
	pthread_attr_t attr;
	if (pthread_attr_init(&attr) != 0) {
		podcastpair_release(job);
		free(job);
		set_state(PODCASTDL_FAILED);
		return false;
	}
	bool valid_attr = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0 && pthread_attr_setstacksize(&attr, 256 * 1024) == 0;
	bool started = valid_attr && pthread_create(&thread, &attr, download_main, job) == 0;
	pthread_attr_destroy(&attr);
	if (!started) {
		podcastpair_release(job);
		free(job);
		set_state(PODCASTDL_FAILED);
		return false;
	}
	return true;
}

void podcastdl_cancel(void) {
	cancel_asked = true;
	// A read blocked on a stalled socket returns only at the receive timeout
	// unless the socket is shut under it.
	pthread_mutex_lock(&lock);
	if (active_stream) {
		http_stream_wake(active_stream);
	}
	pthread_mutex_unlock(&lock);
}

podcastdl_state_t podcastdl_state(long long *done, long long *total) {
	pthread_mutex_lock(&lock);
	podcastdl_state_t s = state;
	if (done) {
		*done = bytes_done;
	}
	if (total) {
		*total = bytes_total;
	}
	pthread_mutex_unlock(&lock);
	return s;
}

void podcastdl_acknowledge(void) {
	pthread_mutex_lock(&lock);
	if (state != PODCASTDL_RUNNING) {
		state = PODCASTDL_IDLE;
	}
	pthread_mutex_unlock(&lock);
}

bool podcastdl_busy(void) { return podcastdl_state(NULL, NULL) == PODCASTDL_RUNNING; }

// A local recovery job reserves only RAM. Directory enumeration and each hash
// chunk close their descriptors before admitting the next filesystem operation.
podcastpair_result_t podcastdl_recover_card(unsigned epoch, bool (*stop)(void *), void *user) {
	job_t job = {0};
	job.recovery = true;
	job.recovery_cancel = stop;
	job.recovery_user = user;
	job.state_epoch = epoch;
	if (!podcastpair_reserve(&job))
		return PODCASTPAIR_BUSY;
	podcastpair_result_t result = PODCASTPAIR_OK;
	char root[512];
	int pin = -1;
	long cursor = 0;
	if (epoch != podcaststate_card_epoch() || (pin = storage_pin_card(root, sizeof(root))) < 0) {
		result = PODCASTPAIR_RECOVERY;
		goto done;
	}
	struct stat st;
	if (fstat(pin, &st) != 0 || epoch != podcaststate_card_epoch()) {
		result = PODCASTPAIR_RECOVERY;
		close(pin);
		goto done;
	}
	job.root_dev = st.st_dev;
	job.root_ino = st.st_ino;
	close(pin);
	// A moved/copied owned record can also accompany loose files at Podcast/.
	// Unknown root records are reported instead of silently hiding those files.
	int loose = pair_open_dir(&job);
	if (loose >= 0) {
		bool pending = podcastpair_pending_fd(loose);
		close(loose);
		if (pending) {
			podcastpair_io_t io = {.open_dir = pair_open_dir, .cancelled = pair_cancelled, .user = &job};
			result = podcastpair_recover(&io);
		}
	} else if (errno != ENOENT)
		result = PODCASTPAIR_RECOVERY;
	job.show_bound = false;
	for (;;) {
		if (stop && stop(user)) {
			result = PODCASTPAIR_RECOVERY;
			break;
		}
		if (epoch != podcaststate_card_epoch() || (pin = storage_pin_card(root, sizeof(root))) < 0) {
			result = PODCASTPAIR_RECOVERY;
			break;
		}
		if (fstat(pin, &st) != 0 || st.st_dev != job.root_dev || st.st_ino != job.root_ino || epoch != podcaststate_card_epoch()) {
			close(pin);
			result = PODCASTPAIR_RECOVERY;
			break;
		}
		int parent = openat(pin, PODCASTDL_FOLDER, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		close(pin);
		if (parent < 0) {
			if (errno != ENOENT)
				result = PODCASTPAIR_RECOVERY;
			break;
		}
		DIR *stream = fdopendir(parent);
		if (!stream) {
			close(parent);
			result = PODCASTPAIR_RECOVERY;
			break;
		}
		// Linux directory cookies are filesystem offsets. Reopen at the next cookie
		// rather than retain the public mount descriptor while hashing a show.
		if (cursor)
			seekdir(stream, cursor);
		errno = 0;
		struct dirent *entry = readdir(stream);
		if (!entry) {
			if (errno && result < PODCASTPAIR_RECOVERY)
				result = PODCASTPAIR_RECOVERY;
			closedir(stream);
			break;
		}
		long next = telldir(stream);
		bool show = entry->d_name[0] != '.' && strlen(entry->d_name) < sizeof(job.show) && fstatat(dirfd(stream), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode);
		if (show) {
			job.show_bound = false;
			snprintf(job.show, sizeof(job.show), "%s", entry->d_name);
		}
		closedir(stream);
		if (next < 0 || next == cursor) {
			result = PODCASTPAIR_RECOVERY;
			break;
		}
		cursor = next;
		if (show) {
			podcastpair_io_t io = {.open_dir = pair_open_dir, .cancelled = pair_cancelled, .user = &job};
			podcastpair_result_t one = podcastpair_recover(&io);
			if (one > result)
				result = one;
		}
	}
done:
	podcastpair_release(&job);
	return result;
}

bool podcastdl_recovery_busy(void) { return podcastpair_busy() && !podcastdl_busy(); }

podcastpair_result_t podcastdl_pair_result(void) {
	pthread_mutex_lock(&lock);
	podcastpair_result_t result = last_pair_result;
	pthread_mutex_unlock(&lock);
	return result;
}
