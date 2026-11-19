#define _GNU_SOURCE
#include "podcastimport.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/system/device/system.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/podcastdl.h"

static bool service_valid(const char *service) { return service && (strcmp(service, "yandex") == 0 || strcmp(service, "zvuk") == 0); }

static bool id_valid(const char *id) {
	if (!id || !id[0] || strlen(id) > 32)
		return false;
	for (const char *p = id; *p; p++) {
		if (*p < '0' || *p > '9')
			return false;
	}
	return true;
}

bool podcastimport_paths(const char *card_root, const char *service, const char *show_id, const char *show_title, const char *episode_id, const char *episode_title, char *folder, size_t folder_size, char *base, size_t base_size) {
	if (folder && folder_size)
		folder[0] = '\0';
	if (base && base_size)
		base[0] = '\0';
	if (!card_root || card_root[0] != '/' || !service_valid(service) || !id_valid(show_id) || !id_valid(episode_id) || !folder || !folder_size || !base || !base_size)
		return false;
	// Keep the relative path below podcaststate's existing 255-byte bound.
	char show[49], title[81];
	podcastdl_safe_name(show_title, "Podcast", show, sizeof(show));
	podcastdl_safe_name(episode_title, "Episode", title, sizeof(title));
	int a = snprintf(folder, folder_size, "%s/" PODCASTDL_FOLDER "/%s [%s-%s]", card_root, show, service, show_id);
	int b = snprintf(base, base_size, "%s [%s]", title, episode_id);
	if (a <= 0 || (size_t)a >= folder_size || b <= 0 || (size_t)b >= base_size || (size_t)a + (size_t)b + 10 >= 512 || (size_t)a - strlen(card_root) + (size_t)b + 10 >= 256) {
		folder[0] = base[0] = '\0';
		return false;
	}
	return true;
}

// The admitted root and a single local-show folder, never a relative path or
// a path through another directory. Naming above never emits dot components.
static bool path_under_root(const char *path, const char *root) {
	char prefix[600];
	int n = snprintf(prefix, sizeof(prefix), "%s/" PODCASTDL_FOLDER "/", root);
	if (!path || n <= 0 || (size_t)n >= sizeof(prefix) || strncmp(path, prefix, (size_t)n) != 0)
		return false;
	const char *show = path + n;
	const char *file = strchr(show, '/');
	if (show[0] == '.' || !file || file == show || !file[1] || strchr(file + 1, '/'))
		return false;
	file++;
	if (file[0] != '.')
		return true;
	// Only completed downloader staging files may be hidden. The final audio
	// is published after its sidecar; the shelf never discovers this name.
	size_t size = strlen(file);
	return size > 6 && file[1] != '.' && strcmp(file + size - 5, ".part") == 0;
}

static void field(FILE *out, const char *key, const char *value) {
	fprintf(out, "%s=", key);
	for (const unsigned char *p = (const unsigned char *)(value ? value : ""); *p; p++)
		fputc(*p == '\r' || *p == '\n' ? ' ' : *p, out);
	fputc('\n', out);
}

// Publication time is only for the existing local shelf's newest-first sort.
// Missing/unknown dates leave the completed download's own mtime intact.
static time_t published_time(const char *text) {
	if (!text || !text[0])
		return 0;
	int y, m, d, h = 12, min = 0, sec = 0, used = 0;
	int count = sscanf(text, "%d-%d-%dT%d:%d:%d%n", &y, &m, &d, &h, &min, &sec, &used);
	if (count != 3 && count != 6)
		return 0;
	if (y < 1970 || y > 2099 || m < 1 || m > 12 || d < 1 || d > 31 || h < 0 || h > 23 || min < 0 || min > 59 || sec < 0 || sec > 59)
		return 0;
	struct tm when = {.tm_year = y - 1900, .tm_mon = m - 1, .tm_mday = d, .tm_hour = h, .tm_min = min, .tm_sec = sec};
	time_t result = timegm(&when);
	if (result <= 0 || when.tm_year != y - 1900 || when.tm_mon != m - 1 || when.tm_mday != d)
		return 0;
	if (count == 6) {
		const char *zone = text + used;
		if (*zone == '.') {
			for (zone++; *zone >= '0' && *zone <= '9'; zone++) {
			}
		}
		int hours, minutes;
		if ((*zone == '+' || *zone == '-') && sscanf(zone + 1, "%d:%d", &hours, &minutes) == 2 && hours <= 23 && minutes <= 59)
			result -= (*zone == '+' ? 1 : -1) * (hours * 3600 + minutes * 60);
	}
	return result;
}

bool podcastimport_write_tags(const char *path, const char *service, const char *show_id, const char *show_title, const char *episode_id, const char *episode_title, const char *artist, const char *cover_url, const char *published_at) {
	if (!service_valid(service) || !id_valid(show_id) || !id_valid(episode_id))
		return false;
	char root[512];
	int pin = storage_pin_card(root, sizeof(root));
	if (pin < 0)
		return false;
	if (!path_under_root(path, root)) {
		close(pin);
		return false;
	}
	// Resolve each directory relative to the pinned filesystem. An unmount or
	// a symlink must never redirect a late write into internal storage.
	const char *show = path + strlen(root) + strlen("/" PODCASTDL_FOLDER "/");
	const char *file = strchr(show, '/') + 1;
	char show_name[256];
	size_t show_size = (size_t)(file - show - 1);
	if (show_size >= sizeof(show_name) || strlen(file) > 180) {
		close(pin);
		return false;
	}
	memcpy(show_name, show, show_size);
	show_name[show_size] = '\0';
	int parent = openat(pin, PODCASTDL_FOLDER, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	int dir = parent >= 0 ? openat(parent, show_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW) : -1;
	if (parent >= 0)
		close(parent);
	if (dir < 0) {
		close(pin);
		return false;
	}
	struct stat st;
	int audio = openat(dir, file, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (audio < 0 || fstat(audio, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
		if (audio >= 0)
			close(audio);
		close(dir);
		close(pin);
		return false;
	}
	char tags[200], tmp[240];
	snprintf(tags, sizeof(tags), "%s.tags", file);
	static unsigned temp_serial;
	int fd = -1;
	for (int attempt = 0; attempt < 16 && fd < 0; attempt++) {
		unsigned serial = __atomic_add_fetch(&temp_serial, 1, __ATOMIC_RELAXED);
		snprintf(tmp, sizeof(tmp), ".%s-%ld-%u.tmp", tags, (long)getpid(), serial);
		fd = openat(dir, tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (fd < 0 && errno != EEXIST)
			break;
	}
	FILE *out = fd >= 0 ? fdopen(fd, "w") : NULL;
	bool ok = out != NULL;
	if (out) {
		field(out, "title", episode_title);
		field(out, "artist", artist && artist[0] ? artist : show_title);
		field(out, "album", show_title);
		field(out, "source_service", service);
		field(out, "source_show_id", show_id);
		field(out, "source_episode_id", episode_id);
		field(out, "source_type", "podcast");
		field(out, "cover_url", cover_url);
		field(out, "feed_image", cover_url);
		field(out, "published_at", published_at);
		ok = !ferror(out);
		if (fclose(out) != 0)
			ok = false;
	} else if (fd >= 0) {
		close(fd);
	}
	if (ok)
		ok = renameat(dir, tmp, dir, tags) == 0;
	if (!ok && fd >= 0)
		unlinkat(dir, tmp, 0);
	time_t date = published_time(published_at);
	if (ok && date > 0) {
		struct timespec times[2] = {{.tv_sec = date}, {.tv_sec = date}};
		ok = futimens(audio, times) == 0;
	}
	close(audio);
	close(dir);
	close(pin);
	return ok;
}

bool podcastimport_key(const char *path, char *out, size_t size) {
	if (!out || !size || !podcastdl_owns(path))
		return false;
	const char *name = strrchr(path, '/');
	if (!name || name[1] == '.')
		return false; // Staged imports are never playable episodes.
	char service[24], id[40];
	if (!podcastcache_tag(path, "source_service", service, sizeof(service)) || !service_valid(service) || !podcastcache_tag(path, "source_episode_id", id, sizeof(id)) || !id_valid(id))
		return false;
	int n = snprintf(out, size, "s:%s:%s", service, id);
	return n > 0 && (size_t)n < size;
}
