#include "bookdownload.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define OWNER ".sonix-download"
#define MANIFEST ".sonix-book"
#define FORMAT "SONIX-BOOK-1\n"

static bool identifier(const char *s) {
	if (!s || !s[0] || strlen(s) > 40)
		return false;
	for (; *s; s++)
		if (!isalnum((unsigned char)*s) && *s != '-')
			return false;
	return true;
}

bool bookdownload_path(const char *root, const char *folder, const char *service, const char *id, char *out, size_t size) {
	if (!root || !root[0] || !folder || !folder[0] || !identifier(service) || !identifier(id))
		return false;
	for (const char *p = folder; *p;) {
		size_t n = strcspn(p, "/");
		if (!n || *p == '.' || memchr(p, '\\', n))
			return false;
		p += n;
		if (*p && !*++p)
			return false;
	}
	int n = snprintf(out, size, "%s/%s/%s-%s", root, folder, service, id);
	// Leave room for staging prefix and chapter filename plus tags suffix.
	return n > 0 && (size_t)n < size && n + 100 < BOOKDOWNLOAD_PATH_MAX;
}

static bool filename_ok(const char *name) {
	if (!name || !name[0] || strlen(name) >= 80 || name[0] == '.')
		return false;
	for (const char *p = name; *p; p++)
		if (!isalnum((unsigned char)*p) && *p != '-' && *p != '.')
			return false;
	return true;
}

static bool part_sizes_at(int dir, const char *name, long long *audio, long long *tags) {
	char sidecar[96];
	struct stat a, t;
	snprintf(sidecar, sizeof(sidecar), "%s.tags", name);
	if (!filename_ok(name) || fstatat(dir, name, &a, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(a.st_mode) || a.st_size <= 0 || fstatat(dir, sidecar, &t, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(t.st_mode) || t.st_size <= 0)
		return false;
	*audio = a.st_size;
	*tags = t.st_size;
	return true;
}

// Both public lookups share the original SONIX-BOOK-1 manifest reader.
static bool ready_files(int dir, char first[80], int *count, const bookdownload_io_t *io) {
	int fd = openat(dir, MANIFEST, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return false;
	struct stat st;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
		close(fd);
		return false;
	}
	FILE *f = fdopen(fd, "r");
	if (!f) {
		close(fd);
		return false;
	}
	char line[160];
	first[0] = 0;
	*count = 0;
	bool ok = fgets(line, sizeof(line), f) && strcmp(line, FORMAT) == 0;
	if (ok)
		ok = fgets(line, sizeof(line), f) && sscanf(line, "%d", count) == 1 && *count > 0 && *count <= BOOKDOWNLOAD_MAX_PARTS;
	for (int i = 0; ok && i < *count; i++) {
		if (io && io->cancelled && i % 8 == 0 && io->cancelled(io->user)) {
			errno = ECANCELED;
			ok = false;
			break;
		}
		char name[80], extra;
		long long audio, tags, actual_audio, actual_tags;
		ok = fgets(line, sizeof(line), f) && sscanf(line, "%79s %lld %lld %c", name, &audio, &tags, &extra) == 3 && part_sizes_at(dir, name, &actual_audio, &actual_tags) && audio == actual_audio && tags == actual_tags;
		if (ok && i == 0)
			snprintf(first, 80, "%s", name);
	}
	ok = ok && fgetc(f) == EOF && !ferror(f);
	int error = errno;
	if (fclose(f) != 0) {
		ok = false;
		error = errno;
	}
	if (!ok)
		errno = error;
	return ok;
}

static bool book_key(const char *directory, const char *first, int count, char *out, size_t size) {
	if (!out || !size)
		return true;
	int n = count == 1 ? snprintf(out, size, "%s/%s", directory, first) : snprintf(out, size, "%s", directory);
	return n > 0 && (size_t)n < size;
}

bool bookdownload_ready(const char *directory, char *book_out, size_t size) {
	if (!directory)
		return false;
	int dir = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (dir < 0)
		return false;
	char first[80];
	int count;
	bool ok = ready_files(dir, first, &count, NULL);
	close(dir);
	return ok && book_key(directory, first, count, book_out, size);
}

static int open_relative_directory(int root, const char *relative) {
	char path[512];
	if (!relative[0] || strlen(relative) >= sizeof(path)) {
		errno = EINVAL;
		return -1;
	}
	for (const char *p = relative; *p;) {
		size_t n = strcspn(p, "/");
		if (!n || *p == '.' || memchr(p, '\\', n)) {
			errno = EINVAL;
			return -1;
		}
		p += n;
		if (*p && !*++p) {
			errno = EINVAL;
			return -1;
		}
	}
	int fd = fcntl(root, F_DUPFD_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	snprintf(path, sizeof(path), "%s", relative);
	char *save = NULL;
	for (char *part = strtok_r(path, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
		int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		close(fd);
		fd = next;
		if (fd < 0)
			return -1;
	}
	return fd;
}

bool bookdownload_ready_admitted(const char *root, const char *directory, const bookdownload_io_t *io, char *book_out, size_t size) {
	if (book_out && size)
		book_out[0] = 0;
	if (!root || !root[0] || !directory || !io || !io->open_root)
		return false;
	size_t n = strlen(root);
	if (strncmp(directory, root, n) || directory[n] != '/')
		return false;
	if (io->cancelled && io->cancelled(io->user)) {
		errno = ECANCELED;
		return false;
	}
	int rootfd = io->open_root(io->user);
	if (rootfd < 0)
		return false;
	struct stat card, folder;
	bool ok = fstat(rootfd, &card) == 0 && S_ISDIR(card.st_mode);
	int dir = ok ? open_relative_directory(rootfd, directory + n + 1) : -1;
	close(rootfd);
	if (dir < 0)
		return false;
	char first[80];
	int count = 0;
	ok = fstat(dir, &folder) == 0 && S_ISDIR(folder.st_mode) && ready_files(dir, first, &count, io);
	if (ok) {
		rootfd = io->open_root(io->user);
		int check = -1;
		struct stat current;
		ok = rootfd >= 0 && fstat(rootfd, &current) == 0 && S_ISDIR(current.st_mode) && current.st_dev == card.st_dev && current.st_ino == card.st_ino;
		if (ok) {
			check = open_relative_directory(rootfd, directory + n + 1);
			ok = check >= 0 && fstat(check, &current) == 0 && S_ISDIR(current.st_mode) && current.st_dev == folder.st_dev && current.st_ino == folder.st_ino;
		}
		if (!ok && rootfd >= 0)
			errno = ESTALE;
		if (check >= 0)
			close(check);
		if (rootfd >= 0)
			close(rootfd);
		if (ok && io->cancelled && io->cancelled(io->user)) {
			ok = false;
			errno = ECANCELED;
		}
	}
	int error = errno;
	close(dir);
	if (!ok)
		errno = error;
	if (!ok || !book_key(directory, first, count, book_out, size)) {
		if (book_out && size)
			book_out[0] = 0;
		return false;
	}
	return true;
}

static bool sync_directory(int dir) {
	for (int attempt = 0; attempt < 8; attempt++) {
		if (fsync(dir) == 0)
			return true;
		if (errno == EINVAL || errno == ENOTSUP)
			return true;
		if (errno != EINTR)
			return false;
	}
	return false;
}

static bool cancelled(bookdownload_t *book) {
	if (book && book->io.cancelled && book->io.cancelled(book->io.user)) {
		errno = ECANCELED;
		return true;
	}
	return false;
}

static int legacy_root(void *user) {
	(void)user;
	return open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}

static bool folder_stat(int fd, dev_t dev, ino_t ino) {
	struct stat st;
	return fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) && st.st_dev == dev && st.st_ino == ino;
}

static int parent_open(bookdownload_t *book, bool initial) {
	int fd = book->io.open_root(book->io.user);
	if (fd < 0)
		return -1;
	if (!folder_stat(fd, book->root_dev, book->root_ino)) {
		close(fd);
		errno = ESTALE;
		return -1;
	}
	char path[sizeof(book->relative_parent)];
	snprintf(path, sizeof(path), "%s", book->relative_parent);
	char *save = NULL;
	for (char *part = strtok_r(path, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
		if (!part[0] || !strcmp(part, ".") || !strcmp(part, "..")) {
			close(fd);
			errno = EINVAL;
			return -1;
		}
		if (initial && mkdirat(fd, part, 0755) != 0 && errno != EEXIST) {
			close(fd);
			return -1;
		}
		int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		close(fd);
		fd = next;
		if (fd < 0)
			return -1;
	}
	struct stat st;
	if (fstat(fd, &st) != 0 || (!initial && (st.st_dev != book->parent_dev || st.st_ino != book->parent_ino))) {
		close(fd);
		errno = ESTALE;
		return -1;
	}
	if (initial) {
		book->parent_dev = st.st_dev;
		book->parent_ino = st.st_ino;
	}
	return fd;
}

static bool owned_stage_fd(int dir) {
	int fd = openat(dir, OWNER, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return false;
	struct stat st;
	char line[sizeof(FORMAT)];
	bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == (off_t)strlen(FORMAT) && read(fd, line, strlen(FORMAT)) == (ssize_t)strlen(FORMAT) && !memcmp(line, FORMAT, strlen(FORMAT));
	if (close(fd) != 0)
		ok = false;
	return ok;
}

int bookdownload_open_stage(bookdownload_t *book) {
	if (!book || !book->owned || book->conflicted) {
		errno = EINVAL;
		return -1;
	}
	int parent = parent_open(book, false);
	if (parent < 0)
		return -1;
	int fd = openat(parent, book->stage_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	close(parent);
	if (fd >= 0 && (!folder_stat(fd, book->stage_dev, book->stage_ino) || !owned_stage_fd(fd))) {
		close(fd);
		errno = ESTALE;
		return -1;
	}
	return fd;
}

static bool part_stat_at(int dir, const char *name, struct stat *audio, struct stat *tags) {
	char sidecar[96];
	snprintf(sidecar, sizeof(sidecar), "%s.tags", name);
	return filename_ok(name) && fstatat(dir, name, audio, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(audio->st_mode) && audio->st_size > 0 && fstatat(dir, sidecar, tags, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(tags->st_mode) && tags->st_size > 0;
}

bool bookdownload_validate(bookdownload_t *book) {
	if (cancelled(book))
		return false;
	int dir = bookdownload_open_stage(book);
	if (dir < 0)
		return false;
	bool ok = true;
	for (int i = 0; ok && i < book->count; i++) {
		if (i && i % 8 == 0) {
			close(dir);
			if (cancelled(book))
				return false;
			dir = bookdownload_open_stage(book);
			if (dir < 0)
				return false;
		}
		struct stat a, t;
		ok = part_stat_at(dir, book->files[i], &a, &t) && a.st_size == book->audio_sizes[i] && t.st_size == book->tag_sizes[i] && a.st_dev == book->audio_dev[i] && t.st_dev == book->tags_dev[i] && a.st_ino == book->audio_ino[i] && t.st_ino == book->tags_ino[i];
	}
	close(dir);
	return ok;
}

static bool flat_owned_name(const char *name) {
	if (!strcmp(name, OWNER) || !strcmp(name, MANIFEST) || !strcmp(name, "cover.jpg") || !strcmp(name, "cover.png"))
		return true;
	const char *p = name[0] == '.' ? name + 1 : name;
	if (strlen(p) < 6 || p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9' || p[2] < '0' || p[2] > '9' || p[3] < '0' || p[3] > '9' || p[4] != '-')
		return false;
	for (; *p; p++)
		if (!isalnum((unsigned char)*p) && *p != '-' && *p != '.')
			return false;
	return true;
}

void bookdownload_abort(bookdownload_t *book) {
	int dir = bookdownload_open_stage(book);
	if (dir < 0)
		return;
	DIR *entries = fdopendir(dir);
	if (!entries) {
		close(dir);
		return;
	}
	struct dirent *e;
	bool ok = true;
	unsigned seen = 0;
	// Validate the flat ownership boundary before deleting anything.
	while ((e = readdir(entries))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		struct stat st;
		if (++seen > BOOKDOWNLOAD_MAX_PARTS * 4 + 4 || !flat_owned_name(e->d_name) || fstatat(dir, e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode)) {
			ok = false;
			break;
		}
	}
	if (ok) {
		rewinddir(entries);
		while ((e = readdir(entries))) {
			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strcmp(e->d_name, OWNER))
				continue;
			if (unlinkat(dir, e->d_name, 0) != 0) {
				ok = false;
				break;
			}
		}
	}
	if (ok)
		ok = unlinkat(dir, OWNER, 0) == 0;
	closedir(entries);
	if (ok) {
		int parent = parent_open(book, false);
		if (parent >= 0) {
			struct stat current;
			ok = fstatat(parent, book->stage_name, &current, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(current.st_mode) && current.st_dev == book->stage_dev && current.st_ino == book->stage_ino && unlinkat(parent, book->stage_name, AT_REMOVEDIR) == 0;
			close(parent);
		} else
			ok = false;
	}
	if (ok)
		book->owned = false;
}

bool bookdownload_begin_admitted(bookdownload_t *book, const char *root, const char *directory, const bookdownload_io_t *io) {
	if (!book || !root || !directory || !io || !io->open_root)
		return false;
	size_t n = strlen(root);
	if (strncmp(directory, root, n) || directory[n] != '/')
		return false;
	memset(book, 0, sizeof(*book));
	book->io = *io;
	const char *relative = directory + n + 1, *slash = strrchr(relative, '/'), *name = slash ? slash + 1 : relative;
	if (!filename_ok(name) || strlen(name) + 7 >= sizeof(book->stage_name) || strlen(directory) >= sizeof(book->final) || strlen(directory) + 7 >= sizeof(book->folder))
		return false;
	snprintf(book->final, sizeof(book->final), "%s", directory);
	snprintf(book->final_name, sizeof(book->final_name), "%s", name);
	snprintf(book->stage_name, sizeof(book->stage_name), ".%s.part", name);
	if (slash)
		snprintf(book->relative_parent, sizeof(book->relative_parent), "%.*s", (int)(slash - relative), relative);
	for (const char *p = book->relative_parent; *p;) {
		size_t len = strcspn(p, "/");
		if (!len || *p == '.' || memchr(p, '\\', len))
			return false;
		p += len;
		if (*p && !*++p)
			return false;
	}
	int fd = io->open_root(io->user);
	struct stat st;
	if (fd < 0)
		return false;
	bool ok = fstat(fd, &st) == 0 && S_ISDIR(st.st_mode);
	if (ok) {
		book->root_dev = st.st_dev;
		book->root_ino = st.st_ino;
	}
	close(fd);
	if (!ok)
		return false;
	int parent = parent_open(book, true);
	if (parent < 0)
		return false;
	if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) {
		close(parent);
		return false;
	}
	int stage = openat(parent, book->stage_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (stage >= 0) {
		ok = fstat(stage, &st) == 0 && owned_stage_fd(stage);
		if (ok) {
			book->stage_dev = st.st_dev;
			book->stage_ino = st.st_ino;
			book->owned = true;
		}
		close(stage);
		close(parent);
		if (!ok)
			return false;
		bookdownload_abort(book);
		if (book->owned)
			return false;
		parent = parent_open(book, false);
		if (parent < 0)
			return false;
	} else if (errno != ENOENT) {
		close(parent);
		return false;
	}
	if (mkdirat(parent, book->stage_name, 0755) != 0) {
		close(parent);
		return false;
	}
	stage = openat(parent, book->stage_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (stage < 0) {
		close(parent);
		return false;
	}
	ok = fstat(stage, &st) == 0;
	if (ok) {
		book->stage_dev = st.st_dev;
		book->stage_ino = st.st_ino;
	}
	fd = ok ? openat(stage, OWNER, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600) : -1;
	struct stat owner;
	bool own_marker = fd >= 0 && fstat(fd, &owner) == 0;
	ok = own_marker && write(fd, FORMAT, strlen(FORMAT)) == (ssize_t)strlen(FORMAT) && fsync(fd) == 0;
	int failure = errno;
	if (fd >= 0 && close(fd) != 0) {
		ok = false;
		failure = errno;
	}
	if (!ok) {
		struct stat current;
		if (own_marker && fstatat(stage, OWNER, &current, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(current.st_mode) && current.st_dev == owner.st_dev && current.st_ino == owner.st_ino)
			unlinkat(stage, OWNER, 0);
		if (fstatat(parent, book->stage_name, &current, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(current.st_mode) && current.st_dev == book->stage_dev && current.st_ino == book->stage_ino)
			unlinkat(parent, book->stage_name, AT_REMOVEDIR);
	}
	close(stage);
	close(parent);
	if (!ok) {
		errno = failure;
		return false;
	}
	snprintf(book->folder, sizeof(book->folder), "%.*s/%s", (int)(strrchr(directory, '/') - directory), directory, book->stage_name);
	book->owned = true;
	return true;
}

bool bookdownload_begin(bookdownload_t *book, const char *directory) {
	bookdownload_io_t io = {.open_root = legacy_root};
	return directory && directory[0] == '/' && bookdownload_begin_admitted(book, "", directory, &io);
}

bool bookdownload_record(bookdownload_t *book, const char *filename) {
	if (cancelled(book) || !book || !book->owned || book->count >= BOOKDOWNLOAD_MAX_PARTS || !filename_ok(filename))
		return false;
	for (int i = 0; i < book->count; i++)
		if (!strcmp(book->files[i], filename))
			return false;
	int dir = bookdownload_open_stage(book);
	if (dir < 0)
		return false;
	struct stat a = {0}, t = {0};
	bool ok = part_stat_at(dir, filename, &a, &t);
	// Confirm delayed output errors once per closed chapter, not per body chunk.
	char tags[96];
	snprintf(tags, sizeof(tags), "%s.tags", filename);
	const char *names[2] = {filename, tags};
	struct stat expected[2] = {a, t};
	for (int n = 0; ok && n < 2; n++) {
		int fd = openat(dir, names[n], O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
		struct stat current;
		ok = fd >= 0 && fstat(fd, &current) == 0 && S_ISREG(current.st_mode) && current.st_dev == expected[n].st_dev && current.st_ino == expected[n].st_ino && current.st_size == expected[n].st_size && fsync(fd) == 0;
		int error = errno;
		if (fd >= 0 && close(fd) != 0) {
			ok = false;
			error = errno;
		}
		if (!ok)
			errno = error;
	}
	int error = errno;
	close(dir);
	if (!ok)
		errno = error;
	if (!ok)
		return false;
	int i = book->count++;
	snprintf(book->files[i], sizeof(book->files[i]), "%s", filename);
	book->audio_sizes[i] = a.st_size;
	book->tag_sizes[i] = t.st_size;
	book->audio_dev[i] = a.st_dev;
	book->tags_dev[i] = t.st_dev;
	book->audio_ino[i] = a.st_ino;
	book->tags_ino[i] = t.st_ino;
	return true;
}

bool bookdownload_discard(bookdownload_t *book, const char *filename) {
	if (!book || !book->owned || !filename_ok(filename))
		return false;
	for (int i = 0; i < book->count; i++)
		if (!strcmp(book->files[i], filename)) {
			errno = EEXIST;
			return false;
		}
	char names[7][96];
	snprintf(names[0], 96, "%s", filename);
	snprintf(names[1], 96, "%s.tags", filename);
	snprintf(names[2], 96, ".%s.part", filename);
	snprintf(names[3], 96, ".%s.part.tags", filename);
	snprintf(names[4], 96, "%s.part", filename);
	snprintf(names[5], 96, "%s.tags.part", filename);
	snprintf(names[6], 96, "%s.tags.tmp", filename);
	struct stat expected[7];
	bool present[7] = {0};
	int dir = bookdownload_open_stage(book);
	if (dir < 0)
		return false;
	bool ok = true;
	for (int i = 0; i < 7; i++) {
		if (fstatat(dir, names[i], &expected[i], AT_SYMLINK_NOFOLLOW) != 0) {
			if (errno != ENOENT)
				ok = false;
		} else {
			present[i] = true;
			if (!S_ISREG(expected[i].st_mode))
				ok = false;
		}
	}
	int error = errno;
	close(dir);
	if (!ok) {
		errno = error;
		return false;
	}
	// No descriptor survives capture/readmission. Never unlink a replaced file.
	for (int i = 0; i < 7; i++)
		if (present[i]) {
			dir = bookdownload_open_stage(book);
			if (dir < 0)
				return false;
			struct stat current, *old = &expected[i];
			ok = fstatat(dir, names[i], &current, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(current.st_mode) && current.st_dev == old->st_dev && current.st_ino == old->st_ino && current.st_size == old->st_size && current.st_mtim.tv_sec == old->st_mtim.tv_sec && current.st_mtim.tv_nsec == old->st_mtim.tv_nsec && current.st_ctim.tv_sec == old->st_ctim.tv_sec && current.st_ctim.tv_nsec == old->st_ctim.tv_nsec;
			if (!ok) {
				book->conflicted = true;
				errno = ESTALE;
			} else
				ok = unlinkat(dir, names[i], 0) == 0;
			error = errno;
			close(dir);
			if (!ok) {
				errno = error;
				return false;
			}
		}
	return true;
}

bool bookdownload_publish(bookdownload_t *book, int expected) {
	if (!book || !book->owned || expected <= 0 || book->count != expected || !bookdownload_validate(book))
		return false;
	int dir = bookdownload_open_stage(book);
	if (dir < 0)
		return false;
	int fd = openat(dir, MANIFEST, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
	FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
	bool ok = f && fprintf(f, FORMAT "%d\n", book->count) > 0;
	for (int i = 0; ok && i < book->count; i++)
		ok = fprintf(f, "%s\t%lld\t%lld\n", book->files[i], book->audio_sizes[i], book->tag_sizes[i]) > 0;
	if (f) {
		if (fflush(f) != 0 || fsync(fileno(f)) != 0)
			ok = false;
		if (fclose(f) != 0)
			ok = false;
	} else if (fd >= 0)
		close(fd);
	if (ok)
		ok = sync_directory(dir);
	int error = errno;
	close(dir);
	if (!ok) {
		errno = error;
		return false;
	}
	int parent = parent_open(book, false);
	if (parent < 0)
		return false;
	struct stat st;
	ok = fstatat(parent, book->final_name, &st, AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT && fstatat(parent, book->stage_name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode) && st.st_dev == book->stage_dev && st.st_ino == book->stage_ino && !cancelled(book) && renameat(parent, book->stage_name, parent, book->final_name) == 0;
	if (ok) {
		// Rename published the complete folder. A later sync failure must not
		// let terminal cleanup delete that publication as if it were staging.
		book->owned = false;
		ok = sync_directory(parent);
	}
	error = errno;
	close(parent);
	if (!ok)
		errno = error;
	return ok;
}
