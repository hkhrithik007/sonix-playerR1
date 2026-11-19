#define _GNU_SOURCE
#include "fileops.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define COPY_CHUNK (64 * 1024)
#define MAX_DEPTH 128
typedef struct {
	const fileop_request_t *request;
	fileop_progress_fn progress;
	fileop_cancel_fn cancelled;
	void *user;
	int root;
	dev_t device;
	uint64_t done, total;
} operation_t;

static bool current(operation_t *op) {
	// In particular, do not wait for card_lock while USB waits for our FD.
	// A short supervisor lookup may temporarily own it too. Allow that lookup
	// to finish, but release our pin promptly when a handoff keeps it busy.
	struct timespec begin = {0};
	for (;;) {
		if (op->cancelled && op->cancelled(op->user)) {
			errno = ECANCELED;
			return false;
		}
		if (storage_card_try_check(&op->request->card) == 0)
			return true;
		if (errno != EAGAIN)
			return false;
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (!begin.tv_sec && !begin.tv_nsec)
			begin = now;
		if ((int64_t)(now.tv_sec - begin.tv_sec) * 1000000000 + now.tv_nsec - begin.tv_nsec >= 50000000) {
			errno = EAGAIN;
			return false;
		}
		usleep(2000);
	}
}

static bool same_file(const struct stat *a, const struct stat *b) { return a->st_dev == b->st_dev && a->st_ino == b->st_ino && (a->st_mode & S_IFMT) == (b->st_mode & S_IFMT); }

static bool stat_at(operation_t *op, int parent, const char *name, struct stat *st) {
	if (!current(op) || fstatat(parent, name, st, AT_SYMLINK_NOFOLLOW))
		return false;
	if (st->st_dev != op->device) {
		errno = EXDEV;
		return false;
	}
	return true;
}

static int open_directory(operation_t *op, int parent, const char *name) {
	if (!current(op))
		return -1;
	int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	struct stat st;
	if (fd >= 0 && (fstat(fd, &st) || st.st_dev != op->device)) {
		close(fd);
		errno = EXDEV;
		return -1;
	}
	return fd;
}

static bool relative_path(const char *root, const char *full, char *out, size_t size) {
	size_t n = strlen(root);
	while (n > 1 && root[n - 1] == '/')
		n--;
	const char *relative = NULL;
	if (n == 1 && root[0] == '/' && full[0] == '/')
		relative = full + 1;
	else if (n && !strncmp(root, full, n) && full[n] == '/')
		relative = full + n + 1;
	if (!relative || !relative[0] || strlen(relative) >= size) {
		errno = EINVAL;
		return false;
	}
	const char *part = relative;
	while (*part) {
		size_t length = strcspn(part, "/");
		if (!length || length > NAME_MAX || (length == 1 && part[0] == '.') || (length == 2 && !strncmp(part, "..", 2))) {
			errno = EINVAL;
			return false;
		}
		part += length;
		if (*part == '/' && !*++part) {
			errno = EINVAL;
			return false;
		}
	}
	memcpy(out, relative, strlen(relative) + 1);
	return true;
}

static int parent_open(operation_t *op, const char *full, char name[NAME_MAX + 1]) {
	char relative[FILEOP_PATH_MAX];
	if (!relative_path(op->request->root, full, relative, sizeof(relative)))
		return -1;
	int fd = fcntl(op->root, F_DUPFD_CLOEXEC, 3);
	if (fd < 0)
		return -1;
	char *part = relative, *slash;
	while ((slash = strchr(part, '/'))) {
		*slash = '\0';
		int next = open_directory(op, fd, part);
		int error = errno;
		close(fd);
		if (next < 0) {
			errno = error;
			return -1;
		}
		fd = next;
		part = slash + 1;
	}
	memcpy(name, part, strlen(part) + 1);
	return fd;
}

static bool ordinary_name(const char *name) { return strcmp(name, ".") && strcmp(name, ".."); }

static DIR *directory_stream(int fd) {
	int copy = fcntl(fd, F_DUPFD_CLOEXEC, 3);
	DIR *dir = copy < 0 ? NULL : fdopendir(copy);
	if (!dir && copy >= 0)
		close(copy);
	return dir;
}

static bool count_tree(operation_t *op, int parent, const char *name, unsigned depth) {
	if (depth > MAX_DEPTH) {
		errno = ELOOP;
		return false;
	}
	struct stat st;
	if (!stat_at(op, parent, name, &st))
		return false;
	if (!S_ISDIR(st.st_mode)) {
		if (!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) {
			errno = ENOTSUP;
			return false;
		}
		if (st.st_size < 0 || UINT64_MAX - op->total < (uint64_t)st.st_size) {
			errno = EOVERFLOW;
			return false;
		}
		op->total += (uint64_t)st.st_size;
		return true;
	}
	int fd = open_directory(op, parent, name);
	DIR *dir = fd < 0 ? NULL : directory_stream(fd);
	if (!dir) {
		if (fd >= 0)
			close(fd);
		return false;
	}
	bool ok = true;
	for (;;) {
		errno = 0;
		struct dirent *entry = readdir(dir);
		if (!entry) {
			ok = errno == 0;
			break;
		}
		if (ordinary_name(entry->d_name) && !count_tree(op, fd, entry->d_name, depth + 1)) {
			ok = false;
			break;
		}
	}
	int error = errno;
	closedir(dir);
	close(fd);
	errno = error;
	return ok;
}

static bool remove_tree(operation_t *op, int parent, const char *name, unsigned depth) {
	if (depth > MAX_DEPTH) {
		errno = ELOOP;
		return false;
	}
	struct stat original;
	if (!stat_at(op, parent, name, &original))
		return false;
	if (!S_ISDIR(original.st_mode))
		return current(op) && unlinkat(parent, name, 0) == 0;
	int fd = open_directory(op, parent, name);
	struct stat opened;
	bool ok = fd >= 0 && !fstat(fd, &opened) && same_file(&original, &opened);
	DIR *dir = ok ? directory_stream(fd) : NULL;
	if (!dir) {
		if (fd >= 0)
			close(fd);
		return false;
	}
	for (;;) {
		errno = 0;
		struct dirent *entry = readdir(dir);
		if (!entry) {
			ok = errno == 0;
			break;
		}
		if (ordinary_name(entry->d_name) && !remove_tree(op, fd, entry->d_name, depth + 1)) {
			ok = false;
			break;
		}
	}
	int error = errno;
	closedir(dir);
	close(fd);
	if (!ok) {
		errno = error;
		return false;
	}
	struct stat now;
	return stat_at(op, parent, name, &now) && same_file(&original, &now) && unlinkat(parent, name, AT_REMOVEDIR) == 0;
}

static bool copy_file(operation_t *op, int from, const char *source, int to, const char *target, const struct stat *original) {
	int in = openat(from, source, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
	struct stat opened, owned;
	if (in < 0 || fstat(in, &opened) || !same_file(original, &opened) || !S_ISREG(opened.st_mode)) {
		if (in >= 0)
			close(in);
		return false;
	}
	int out = current(op) ? openat(to, target, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666) : -1;
	if (out < 0) {
		int error = errno;
		close(in);
		errno = error;
		return false;
	}
	bool own = !fstat(out, &owned);
	char *buffer = own ? malloc(COPY_CHUNK) : NULL;
	bool ok = buffer != NULL;
	if (own && !buffer)
		errno = ENOMEM;
	while (ok) {
		if (!current(op)) {
			ok = false;
			break;
		}
		ssize_t got = read(in, buffer, COPY_CHUNK);
		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0) {
			ok = false;
			break;
		}
		if (!got)
			break;
		ssize_t sent = 0;
		while (sent < got) {
			if (!current(op)) {
				ok = false;
				break;
			}
			ssize_t wrote = write(out, buffer + sent, (size_t)(got - sent));
			if (wrote < 0 && errno == EINTR)
				continue;
			if (wrote <= 0) {
				ok = false;
				if (!wrote)
					errno = EIO;
				break;
			}
			sent += wrote;
		}
		if (sent != got) {
			ok = false;
			break;
		}
		op->done += (uint64_t)got;
		if (op->progress)
			op->progress(op->done, op->total, op->user);
	}
	struct stat end;
	if (ok && (fstat(in, &end) || !same_file(&opened, &end) || opened.st_size != end.st_size || opened.st_mtime != end.st_mtime)) {
		errno = ESTALE;
		ok = false;
	}
	int error = errno;
	free(buffer);
	close(in);
	if (close(out) && ok) {
		error = errno;
		ok = false;
	}
	// A revoked operation may clean its own partial file through the old pin;
	// it never resolves the absolute mount path or deletes a replacement file.
	if (!ok && own && !fstatat(to, target, &end, AT_SYMLINK_NOFOLLOW) && same_file(&owned, &end))
		unlinkat(to, target, 0);
	errno = error;
	return ok;
}

static bool copy_tree(operation_t *op, int from, const char *source, int to, const char *target, unsigned depth) {
	if (depth > MAX_DEPTH) {
		errno = ELOOP;
		return false;
	}
	struct stat st;
	if (!stat_at(op, from, source, &st))
		return false;
	if (S_ISREG(st.st_mode))
		return copy_file(op, from, source, to, target, &st);
	if (S_ISLNK(st.st_mode)) {
		char link[FILEOP_PATH_MAX];
		ssize_t len = readlinkat(from, source, link, sizeof(link) - 1);
		if (len < 0 || (size_t)len == sizeof(link) - 1) {
			errno = ENAMETOOLONG;
			return false;
		}
		link[len] = '\0';
		bool ok = current(op) && symlinkat(link, to, target) == 0;
		if (ok) {
			op->done += (uint64_t)len;
			if (op->progress)
				op->progress(op->done, op->total, op->user);
		}
		return ok;
	}
	if (!S_ISDIR(st.st_mode)) {
		errno = ENOTSUP;
		return false;
	}
	int src = open_directory(op, from, source);
	struct stat opened;
	bool ok = src >= 0 && !fstat(src, &opened) && same_file(&st, &opened) && current(op) && mkdirat(to, target, 0777) == 0;
	int dst = ok ? open_directory(op, to, target) : -1;
	DIR *dir = dst >= 0 ? directory_stream(src) : NULL;
	if (!dir) {
		int error = errno;
		if (src >= 0)
			close(src);
		if (dst >= 0)
			close(dst);
		errno = error;
		return false;
	}
	for (;;) {
		errno = 0;
		struct dirent *entry = readdir(dir);
		if (!entry) {
			ok = errno == 0;
			break;
		}
		if (ordinary_name(entry->d_name) && !copy_tree(op, src, entry->d_name, dst, entry->d_name, depth + 1)) {
			ok = false;
			break;
		}
	}
	int error = errno;
	closedir(dir);
	close(src);
	close(dst);
	errno = error;
	return ok;
}

int fileop_run(const fileop_request_t *request, fileop_progress_fn progress, fileop_cancel_fn cancelled, void *user) {
	if (!request || !request->card.epoch || request->kind < FILEOP_COPY || request->kind > FILEOP_MKDIR || request->root[0] != '/' || !memchr(request->root, 0, sizeof(request->root)) || !memchr(request->source, 0, sizeof(request->source)) || !memchr(request->target, 0, sizeof(request->target)))
		return EINVAL;
	operation_t op = {.request = request, .progress = progress, .cancelled = cancelled, .user = user, .root = -1};
	op.root = storage_pin_card_if(&request->card);
	if (op.root < 0)
		return errno ? errno : EIO;
	struct stat root;
	if (fstat(op.root, &root)) {
		int error = errno;
		close(op.root);
		return error;
	}
	op.device = root.st_dev;
	char source[NAME_MAX + 1], target[NAME_MAX + 1];
	int from = -1, to = -1;
	bool ok = current(&op);
	if (ok && request->kind != FILEOP_MKDIR) {
		from = parent_open(&op, request->source, source);
		ok = from >= 0;
	}
	if (ok && request->kind != FILEOP_DELETE) {
		to = parent_open(&op, request->target, target);
		ok = to >= 0;
	}
	if (ok && (request->kind == FILEOP_MOVE || request->kind == FILEOP_COPY || request->kind == FILEOP_RENAME)) {
		size_t n = strlen(request->source);
		if (!strncmp(request->source, request->target, n) && (!request->target[n] || request->target[n] == '/')) {
			errno = EINVAL;
			ok = false;
		}
	}
	if (ok && (request->kind == FILEOP_MOVE || request->kind == FILEOP_RENAME)) {
		struct stat st;
		// R1's VFAT does not support RENAME_NOREPLACE (verified natively).
		// Keep rename within the original pinned parents; the GUI admits only
		// one file-manager mutation at a time. Recheck the name on the worker.
		if (!fstatat(to, target, &st, AT_SYMLINK_NOFOLLOW)) {
			errno = EEXIST;
			ok = false;
		} else
			ok = errno == ENOENT && stat_at(&op, from, source, &st) && current(&op) && renameat(from, source, to, target) == 0;
	} else if (ok && request->kind == FILEOP_MKDIR)
		ok = current(&op) && mkdirat(to, target, 0777) == 0;
	else if (ok && request->kind == FILEOP_DELETE)
		ok = remove_tree(&op, from, source, 0);
	else if (ok) {
		ok = count_tree(&op, from, source, 0);
		if (ok && progress)
			progress(0, op.total, user);
		if (ok)
			ok = copy_tree(&op, from, source, to, target, 0);
	}
	int error = ok ? 0 : (errno ? errno : EIO);
	// One final filesystem flush, not one fsync per thumbnail in a tree.
	if (ok && (!current(&op) || syncfs(op.root)))
		error = errno ? errno : EIO;
	if (from >= 0)
		close(from);
	if (to >= 0)
		close(to);
	close(op.root);
	return error;
}
