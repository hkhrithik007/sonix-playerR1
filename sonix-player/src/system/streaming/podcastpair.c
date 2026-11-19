#define _GNU_SOURCE
#include "podcastpair.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PREFIX ".sonix-pair-"
#define MAGIC "SONIX-PODCAST-PAIR-1\n"
#define NAME_MAX_BYTES 239
#define HASH_CHUNK 32768
#define MARKER ".sonix-pair.txn"

enum { FINAL_A, FINAL_T, STAGE_A, STAGE_T, BACK_A, BACK_T, FILES };
enum { OLD_A, OLD_T, NEW_A, NEW_T, DIGESTS };
typedef struct {
	bool present;
	long long size;
	char hash[SHA256_HEX_LEN];
} digest_t;
typedef struct {
	bool committed;
	char names[FILES][NAME_MAX_BYTES + 1];
	digest_t data[DIGESTS];
} record_t;
typedef struct {
	bool present;
	struct stat st;
	digest_t data;
} file_t;

// A RAM reservation may span a download's network wait, never an FD/card lock.
// A worker releases only its own reservation, before its owner is freed.
static _Atomic(void *) owner;

bool podcastpair_reserve(void *token) {
	void *empty = NULL;
	return token && atomic_compare_exchange_strong(&owner, &empty, token);
}
void podcastpair_release(void *token) { atomic_compare_exchange_strong(&owner, &token, NULL); }
bool podcastpair_busy(void) { return atomic_load(&owner) != NULL; }

static bool cancelled(const podcastpair_io_t *io) { return io->cancelled && io->cancelled(io->user); }
static bool basename_ok(const char *s) {
	size_t n = s ? strlen(s) : 0;
	if (!n || n > NAME_MAX_BYTES || !strcmp(s, ".") || !strcmp(s, ".."))
		return false;
	for (; *s; s++)
		if ((unsigned char)*s < 32 || *s == '/' || *s == '\\')
			return false;
	return true;
}
static bool hash_ok(const char *s) {
	if (strlen(s) != 64)
		return false;
	for (int i = 0; i < 64; i++)
		if (!strchr("0123456789abcdef", s[i]))
			return false;
	return true;
}
static _Atomic unsigned mutation_serial;
static void mutation_begin(void) { atomic_fetch_add(&mutation_serial, 1); }
static void mutation_end(void) { atomic_fetch_add(&mutation_serial, 1); }
static void marker_name(const char *file, char out[96]) {
	(void)file;
	snprintf(out, 96, "%s", MARKER);
}
static bool native_stage(const char *name, const char *final, bool tags) {
	char prefix[256];
	int n = snprintf(prefix, sizeof(prefix), ".%s%s-", final, tags ? ".tags" : "");
	if (n <= 0 || (size_t)n >= sizeof(prefix) || strncmp(name, prefix, (size_t)n))
		return false;
	const char *p = name + n;
	if (*p < '0' || *p > '9')
		return false;
	while (*p >= '0' && *p <= '9')
		p++;
	if (*p++ != '-' || *p < '0' || *p > '9')
		return false;
	while (*p >= '0' && *p <= '9')
		p++;
	return !strcmp(p, tags ? ".tmp" : ".part");
}
static bool schema(const record_t *r) {
	if (!basename_ok(r->names[FINAL_A]) || r->names[FINAL_A][0] == '.')
		return false;
	char tags[246], a[240], t[240], digest[SHA256_HEX_LEN];
	snprintf(tags, sizeof(tags), "%s.tags", r->names[FINAL_A]);
	if (strcmp(tags, r->names[FINAL_T]) || !native_stage(r->names[STAGE_A], r->names[FINAL_A], false) || !native_stage(r->names[STAGE_T], r->names[FINAL_A], true))
		return false;
	sha256_ctx sha;
	sha256_init(&sha);
	sha256_update(&sha, r->names[STAGE_A], strlen(r->names[STAGE_A]));
	sha256_final_hex(&sha, digest);
	snprintf(a, sizeof(a), PREFIX "%.32s.a.old", digest);
	snprintf(t, sizeof(t), PREFIX "%.32s.t.old", digest);
	if (strcmp(a, r->names[BACK_A]) || strcmp(t, r->names[BACK_T]))
		return false;
	for (int i = 0; i < FILES; i++) {
		if (!basename_ok(r->names[i]))
			return false;
		for (int j = 0; j < i; j++)
			if (!strcmp(r->names[i], r->names[j]))
				return false;
	}
	for (int i = 0; i < DIGESTS; i++)
		if (r->data[i].size < 0 || (r->data[i].present && !hash_ok(r->data[i].hash)) || (!r->data[i].present && r->data[i].size))
			return false;
	return r->data[NEW_A].present && r->data[NEW_T].present && r->data[NEW_A].size > 0 && r->data[NEW_T].size > 0;
}
static bool parse_digest(const char *text, digest_t *d) {
	if ((text[0] != '0' && text[0] != '1') || text[1] != ' ' || text[2] < '0' || text[2] > '9')
		return false;
	char *end;
	errno = 0;
	unsigned long long size = strtoull(text + 2, &end, 10);
	if (errno == ERANGE || size > LLONG_MAX || *end != ' ')
		return false;
	d->present = text[0] == '1';
	d->size = (long long)size;
	if (d->present) {
		if (!hash_ok(end + 1))
			return false;
		memcpy(d->hash, end + 1, SHA256_HEX_LEN);
	} else if (size || strcmp(end + 1, "-"))
		return false;
	return true;
}
static bool sync_file(int fd) {
	for (int tries = 0; tries < 8; tries++) {
		if (fsync(fd) == 0)
			return true;
		if (errno != EINTR)
			return false;
	}
	return false;
}
static bool sync_dir(int dir) { return sync_file(dir) || errno == EINVAL || errno == ENOTSUP; }
static bool same_stat(const struct stat *a, const struct stat *b) { return S_ISREG(b->st_mode) && a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size && a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec && a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec; }

// Each chunk is opened/read/closed independently. A missing/replaced card or
// changed file stops the digest rather than mixing generations of its bytes.
static bool hash_file(const podcastpair_io_t *io, const char *name, file_t *out, bool safety) {
	memset(out, 0, sizeof(*out));
	sha256_ctx sha;
	sha256_init(&sha);
	long long offset = 0;
	char buf[HASH_CHUNK];
	for (;;) {
		if (!safety && cancelled(io))
			return false;
		int dir = io->open_dir(io->user);
		if (dir < 0)
			return false;
		int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
		if (fd < 0) {
			bool absent = !offset && errno == ENOENT;
			close(dir);
			return absent;
		}
		struct stat st;
		bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0;
		if (ok && !out->present) {
			out->present = out->data.present = true;
			out->st = st;
			out->data.size = st.st_size;
		} else if (ok)
			ok = same_stat(&out->st, &st);
		size_t want = ok && st.st_size > offset ? (size_t)(st.st_size - offset > HASH_CHUNK ? HASH_CHUNK : st.st_size - offset) : 0;
		if (ok && lseek(fd, (off_t)offset, SEEK_SET) < 0)
			ok = false;
		ssize_t got = 0;
		if (ok && want) {
			do {
				got = read(fd, buf, want);
			} while (got < 0 && errno == EINTR);
			ok = got > 0;
		}
		if (close(fd) != 0)
			ok = false;
		if (close(dir) != 0)
			ok = false;
		if (!ok)
			return false;
		if (!want)
			break;
		sha256_update(&sha, buf, (size_t)got);
		offset += got;
	}
	sha256_final_hex(&sha, out->data.hash);
	return true;
}
static bool matches(const file_t *f, const digest_t *d) { return f->present == d->present && (!f->present || (f->data.size == d->size && !strcmp(f->data.hash, d->hash))); }
static bool at_matches(int dir, const char *name, const file_t *f) {
	struct stat st;
	if (fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
		return !f->present && errno == ENOENT;
	return f->present && same_stat(&f->st, &st);
}
static bool record_matches(int dir, const record_t *expected, const file_t *witness);
static bool remove_file(const podcastpair_io_t *io, const char *name, file_t *f, const record_t *record, const file_t *witness) {
	if (!f->present)
		return true;
	int dir = io->open_dir(io->user);
	if (dir < 0)
		return false;
	mutation_begin();
	bool ok = record_matches(dir, record, witness) && at_matches(dir, name, f) && unlinkat(dir, name, 0) == 0;
	if (ok)
		f->present = f->data.present = false;
	close(dir);
	mutation_end();
	return ok;
}
static bool move_file(const podcastpair_io_t *io, const char *from, file_t *src, const char *to, file_t *dst, const record_t *record, const file_t *witness) {
	int dir = io->open_dir(io->user);
	if (dir < 0)
		return false;
	mutation_begin();
	bool ok = record_matches(dir, record, witness) && src->present && !dst->present && at_matches(dir, from, src) && at_matches(dir, to, dst) && renameat(dir, from, dir, to) == 0;
	if (ok) {
		*dst = *src;
		src->present = src->data.present = false;
		ok = fstatat(dir, to, &dst->st, AT_SYMLINK_NOFOLLOW) == 0;
	}
	close(dir);
	mutation_end();
	return ok;
}

static void hex_name(const char *s, char out[480]) {
	static const char hex[] = "0123456789abcdef";
	size_t i;
	for (i = 0; s[i]; i++) {
		out[2 * i] = hex[(unsigned char)s[i] >> 4];
		out[2 * i + 1] = hex[(unsigned char)s[i] & 15];
	}
	out[2 * i] = 0;
}
static bool unhex_name(char *s, char out[240]) {
	size_t n = strlen(s);
	if (!n || n % 2 || n / 2 > NAME_MAX_BYTES)
		return false;
	for (size_t i = 0; i < n / 2; i++) {
		static const char hex[] = "0123456789abcdef";
		const char *a = strchr(hex, s[2 * i]), *b = strchr(hex, s[2 * i + 1]);
		if (!a || !b)
			return false;
		out[i] = (char)(((a - hex) << 4) | (b - hex));
		if (!out[i])
			return false;
	}
	out[n / 2] = 0;
	return basename_ok(out);
}
static bool line(FILE *f, char *out, size_t size) {
	if (!fgets(out, (int)size, f))
		return false;
	size_t n = strlen(out);
	if (!n || out[n - 1] != '\n')
		return false;
	out[n - 1] = 0;
	return true;
}
static bool read_record(int dir, const char *name, record_t *r, file_t *record) {
	memset(r, 0, sizeof(*r));
	memset(record, 0, sizeof(*record));
	int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return false;
	bool ok = fstat(fd, &record->st) == 0 && S_ISREG(record->st.st_mode) && record->st.st_size > 0 && record->st.st_size < 4096;
	FILE *f = ok ? fdopen(fd, "r") : NULL;
	if (!f) {
		close(fd);
		return false;
	}
	char text[512];
	ok = fgets(text, sizeof(text), f) && !strcmp(text, MAGIC) && line(f, text, sizeof(text)) && (!strcmp(text, "0") || !strcmp(text, "1"));
	if (ok)
		r->committed = text[0] == '1';
	for (int i = 0; ok && i < FILES; i++)
		ok = line(f, text, sizeof(text)) && unhex_name(text, r->names[i]);
	for (int i = 0; ok && i < DIGESTS; i++)
		ok = line(f, text, sizeof(text)) && parse_digest(text, &r->data[i]);
	ok = ok && fgetc(f) == EOF && !ferror(f);
	if (fclose(f) != 0)
		ok = false;
	char expected[96], next[112];
	marker_name(r->names[FINAL_A], expected);
	snprintf(next, sizeof(next), "%s.next", expected);
	ok = ok && (!strcmp(name, expected) || !strcmp(name, next)) && schema(r);
	record->present = ok;
	return ok;
}
static bool record_matches(int dir, const record_t *expected, const file_t *witness) {
	record_t current;
	file_t file;
	return at_matches(dir, MARKER, witness) && read_record(dir, MARKER, &current, &file) && !memcmp(&current, expected, sizeof(current));
}
static bool store_record(const podcastpair_io_t *io, const char *name, record_t *r, file_t *old) {
	if (r->committed && cancelled(io))
		return false;
	int dir = io->open_dir(io->user);
	if (dir < 0)
		return false;
	char tmp[112];
	snprintf(tmp, sizeof(tmp), "%s.next", name);
	const char *dest = old->present ? tmp : name;
	mutation_begin();
	int fd = openat(dir, dest, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0) {
		close(dir);
		mutation_end();
		return false;
	}
	struct stat created;
	bool witnessed = fstat(fd, &created) == 0;
	int witness = witnessed ? fcntl(fd, F_DUPFD_CLOEXEC, 0) : -1;
	FILE *f = witness >= 0 ? fdopen(fd, "w") : NULL;
	bool ok = f != NULL;
	if (f) {
		ok = fprintf(f, MAGIC "%u\n", (unsigned)r->committed) > 0;
		for (int i = 0; ok && i < FILES; i++) {
			char hex[480];
			hex_name(r->names[i], hex);
			ok = fprintf(f, "%s\n", hex) > 0;
		}
		for (int i = 0; ok && i < DIGESTS; i++)
			ok = fprintf(f, "%u %lld %s\n", (unsigned)r->data[i].present, r->data[i].size, r->data[i].present ? r->data[i].hash : "-") > 0;
		ok = ok && !ferror(f) && fflush(f) == 0 && sync_file(fileno(f));
		if (fclose(f) != 0)
			ok = false;
	} else
		close(fd);
	if (witness >= 0) {
		witnessed = fstat(witness, &created) == 0;
		close(witness);
	}
	if (ok && old->present) {
		record_t previous = *r;
		previous.committed = false;
		ok = record_matches(dir, &previous, old) && renameat(dir, tmp, dir, name) == 0;
	}
	if (ok)
		ok = fstatat(dir, name, &old->st, AT_SYMLINK_NOFOLLOW) == 0 && sync_dir(dir);
	if (ok)
		old->present = true;
	else if (witnessed) {
		// Only this call's O_EXCL-created inode, never a colliding record/temp.
		struct stat now;
		if (fstatat(dir, dest, &now, AT_SYMLINK_NOFOLLOW) == 0 && same_stat(&created, &now)) {
			unlinkat(dir, dest, 0);
			sync_dir(dir);
		}
	}
	close(dir);
	mutation_end();
	return ok;
}

static podcastpair_result_t recover_one(const podcastpair_io_t *io, const char *marker, bool *published, bool safety) {
	if (published)
		*published = false;
	int dir = io->open_dir(io->user);
	if (dir < 0)
		return PODCASTPAIR_RECOVERY;
	record_t r;
	file_t mark;
	bool valid = read_record(dir, marker, &r, &mark);
	close(dir);
	if (!valid)
		return PODCASTPAIR_CONFLICT;
	char next_name[112];
	snprintf(next_name, sizeof(next_name), "%s.next", marker);
	file_t next_file;
	if (!hash_file(io, next_name, &next_file, safety))
		return PODCASTPAIR_RECOVERY;
	if (next_file.present) {
		record_t next;
		file_t next_record;
		dir = io->open_dir(io->user);
		if (dir < 0)
			return PODCASTPAIR_RECOVERY;
		bool next_valid = read_record(dir, next_name, &next, &next_record);
		close(dir);
		next.committed = r.committed;
		if (!next_valid || memcmp(&r, &next, sizeof(r)))
			return PODCASTPAIR_CONFLICT;
	}
	file_t files[FILES];
	for (int i = 0; i < FILES; i++) {
		if (!hash_file(io, r.names[i], &files[i], safety))
			return PODCASTPAIR_RECOVERY;
		if (!files[i].present)
			continue;
		bool known = i == FINAL_A ? matches(&files[i], &r.data[OLD_A]) || matches(&files[i], &r.data[NEW_A]) : i == FINAL_T ? matches(&files[i], &r.data[OLD_T]) || matches(&files[i], &r.data[NEW_T]) : i == STAGE_A ? matches(&files[i], &r.data[NEW_A]) : i == STAGE_T ? matches(&files[i], &r.data[NEW_T]) : i == BACK_A ? matches(&files[i], &r.data[OLD_A]) : matches(&files[i], &r.data[OLD_T]);
		if (!known)
			return PODCASTPAIR_CONFLICT;
	}
	bool new_ready = r.committed && matches(&files[FINAL_A], &r.data[NEW_A]) && matches(&files[FINAL_T], &r.data[NEW_T]);
	if (published)
		*published = new_ready;
	if (!new_ready) {
		// Validate both old sources before removing either new public file.
		for (int i = 0; i < 2; i++)
			if (r.data[i].present && !matches(&files[i], &r.data[i]) && !matches(&files[BACK_A + i], &r.data[i]))
				return PODCASTPAIR_RECOVERY;
		for (int i = 0; i < 2; i++) {
			if (matches(&files[i], &r.data[i]))
				continue;
			if (!remove_file(io, r.names[i], &files[i], &r, &mark))
				return PODCASTPAIR_RECOVERY;
			if (r.data[i].present && !move_file(io, r.names[BACK_A + i], &files[BACK_A + i], r.names[i], &files[i], &r, &mark))
				return PODCASTPAIR_RECOVERY;
		}
	}
	for (int i = STAGE_A; i < FILES; i++)
		if (!remove_file(io, r.names[i], &files[i], &r, &mark))
			return PODCASTPAIR_RECOVERY;
	if (!remove_file(io, next_name, &next_file, &r, &mark))
		return PODCASTPAIR_RECOVERY;
	dir = io->open_dir(io->user);
	if (dir < 0)
		return PODCASTPAIR_RECOVERY;
	mutation_begin();
	bool ok = record_matches(dir, &r, &mark) && sync_dir(dir) && unlinkat(dir, marker, 0) == 0 && sync_dir(dir);
	close(dir);
	mutation_end();
	return ok ? PODCASTPAIR_OK : PODCASTPAIR_RECOVERY;
}

podcastpair_result_t podcastpair_publish(const podcastpair_io_t *io, const char *a, const char *t, const char *final, const podcastpair_audio_t *audio) {
	if (!io || !io->open_dir || !io->user || !audio || !basename_ok(a) || !basename_ok(t) || !basename_ok(final) || !hash_ok(audio->sha256) || audio->size <= 0)
		return PODCASTPAIR_FAILED;
	bool reserved = atomic_load(&owner) == io->user;
	if (!reserved && !podcastpair_reserve(io->user))
		return PODCASTPAIR_BUSY;
	podcastpair_result_t result = PODCASTPAIR_FAILED;
	record_t r = {0};
	file_t mark = {0}, files[FILES] = {0};
	char marker[96];
	marker_name(final, marker);
	int dir = io->open_dir(io->user);
	if (dir < 0)
		goto done;
	struct stat st;
	bool pending = fstatat(dir, marker, &st, AT_SYMLINK_NOFOLLOW) == 0;
	close(dir);
	if (pending) {
		result = recover_one(io, marker, NULL, true);
		if (result != PODCASTPAIR_OK)
			goto done;
		result = PODCASTPAIR_FAILED;
	}
	char next_name[112];
	snprintf(next_name, sizeof(next_name), "%s.next", marker);
	dir = io->open_dir(io->user);
	if (dir < 0)
		goto done;
	bool next_absent = fstatat(dir, next_name, &st, AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT;
	close(dir);
	if (!next_absent) {
		result = PODCASTPAIR_CONFLICT;
		goto done;
	}
	snprintf(r.names[FINAL_A], sizeof(r.names[FINAL_A]), "%s", final);
	if (snprintf(r.names[FINAL_T], sizeof(r.names[FINAL_T]), "%s.tags", final) >= (int)sizeof(r.names[FINAL_T]))
		goto done;
	snprintf(r.names[STAGE_A], sizeof(r.names[STAGE_A]), "%s", a);
	snprintf(r.names[STAGE_T], sizeof(r.names[STAGE_T]), "%s", t);
	char nonce[SHA256_HEX_LEN];
	sha256_ctx hash;
	sha256_init(&hash);
	sha256_update(&hash, a, strlen(a));
	sha256_final_hex(&hash, nonce);
	snprintf(r.names[BACK_A], sizeof(r.names[BACK_A]), PREFIX "%.32s.a.old", nonce);
	snprintf(r.names[BACK_T], sizeof(r.names[BACK_T]), PREFIX "%.32s.t.old", nonce);
	for (int i = 0; i < FILES; i++) {
		if (i == STAGE_A) {
			dir = io->open_dir(io->user);
			if (dir < 0)
				goto done;
			bool ok = fstatat(dir, a, &files[i].st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(files[i].st.st_mode) && files[i].st.st_dev == audio->dev && files[i].st.st_ino == audio->ino && files[i].st.st_size == audio->size;
			close(dir);
			if (!ok)
				goto done;
			files[i].present = files[i].data.present = true;
			files[i].data.size = audio->size;
			snprintf(files[i].data.hash, sizeof(files[i].data.hash), "%s", audio->sha256);
		} else if (!hash_file(io, r.names[i], &files[i], false))
			goto done;
	}
	if (!files[STAGE_T].present || files[STAGE_T].data.size <= 0 || files[BACK_A].present || files[BACK_T].present || cancelled(io))
		goto done;
	r.data[OLD_A] = files[FINAL_A].data;
	r.data[OLD_T] = files[FINAL_T].data;
	r.data[NEW_A] = files[STAGE_A].data;
	r.data[NEW_T] = files[STAGE_T].data;
	if (!schema(&r))
		goto done;
	if (!store_record(io, marker, &r, &mark)) {
		dir = io->open_dir(io->user);
		bool absent = dir >= 0 && fstatat(dir, marker, &st, AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT;
		if (dir >= 0)
			close(dir);
		result = absent ? PODCASTPAIR_FAILED : PODCASTPAIR_RECOVERY;
		goto done;
	}
	for (int i = 0; i < 2; i++)
		if (files[i].present && !move_file(io, r.names[i], &files[i], r.names[BACK_A + i], &files[BACK_A + i], &r, &mark))
			goto rollback;
	for (int i = 0; i < 2; i++)
		if (cancelled(io) || !move_file(io, r.names[STAGE_A + i], &files[STAGE_A + i], r.names[i], &files[i], &r, &mark))
			goto rollback;
	dir = io->open_dir(io->user);
	if (dir < 0)
		goto rollback;
	bool synced = sync_dir(dir);
	close(dir);
	if (!synced || cancelled(io))
		goto rollback;
	r.committed = true;
	if (cancelled(io))
		goto rollback;
	if (!store_record(io, marker, &r, &mark))
		goto rollback;
	// A surviving record, including COMMITTED, remains unready until verified
	// cleanup succeeds. This avoids a positive hash cache on the GUI thread.
	result = PODCASTPAIR_RECOVERY;
	for (int i = BACK_A; i < FILES; i++)
		if (!remove_file(io, r.names[i], &files[i], &r, &mark))
			goto done;
	dir = io->open_dir(io->user);
	if (dir >= 0) {
		mutation_begin();
		if (record_matches(dir, &r, &mark) && sync_dir(dir) && unlinkat(dir, marker, 0) == 0 && sync_dir(dir))
			result = PODCASTPAIR_OK;
		close(dir);
		mutation_end();
	}
	goto done;
rollback: {
	bool published = false;
	podcastpair_result_t recovered = recover_one(io, marker, &published, true);
	result = recovered == PODCASTPAIR_OK ? (published ? PODCASTPAIR_OK : PODCASTPAIR_FAILED) : recovered;
}
done:
	if (!reserved)
		podcastpair_release(io->user);
	return result;
}

podcastpair_result_t podcastpair_recover(const podcastpair_io_t *io) {
	if (!io || !io->open_dir || !io->user)
		return PODCASTPAIR_FAILED;
	bool reserved = atomic_load(&owner) == io->user;
	if (!reserved && !podcastpair_reserve(io->user))
		return PODCASTPAIR_BUSY;
	int dir = io->open_dir(io->user);
	podcastpair_result_t result = PODCASTPAIR_RECOVERY;
	if (dir >= 0) {
		struct stat st;
		bool record = fstatat(dir, MARKER, &st, AT_SYMLINK_NOFOLLOW) == 0;
		int error = errno;
		bool next = fstatat(dir, MARKER ".next", &st, AT_SYMLINK_NOFOLLOW) == 0;
		int next_error = errno;
		close(dir);
		if (record)
			result = recover_one(io, MARKER, NULL, false);
		else if (error == ENOENT && !next && next_error == ENOENT)
			result = PODCASTPAIR_OK;
		else if (next)
			result = PODCASTPAIR_CONFLICT;
	}
	if (!reserved)
		podcastpair_release(io->user);
	return result;
}

bool podcastpair_pending_fd(int dir) {
	struct stat st;
	if (fstatat(dir, MARKER, &st, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT)
		return true;
	return fstatat(dir, MARKER ".next", &st, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT;
}

bool podcastpair_ready_at(int dir, const char *file) {
	if (!basename_ok(file))
		return false;
	unsigned serial = atomic_load(&mutation_serial);
	if (serial & 1)
		return false;
	struct stat st;
	bool record = fstatat(dir, MARKER, &st, AT_SYMLINK_NOFOLLOW) == 0;
	int error = errno;
	bool next = fstatat(dir, MARKER ".next", &st, AT_SYMLINK_NOFOLLOW) == 0;
	int next_error = errno;
	bool ready = !record && error == ENOENT && !next && next_error == ENOENT;
	// Any update record blocks this folder until worker ownership checks run.
	if (record && !next && next_error == ENOENT) {
		record_t r;
		file_t mark;
		struct stat wanted, target;
		if (read_record(dir, MARKER, &r, &mark) && fstatat(dir, file, &wanted, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(wanted.st_mode)) {
			if (fstatat(dir, r.names[FINAL_A], &target, AT_SYMLINK_NOFOLLOW) != 0)
				ready = errno == ENOENT;
			else
				ready = S_ISREG(target.st_mode) && (target.st_dev != wanted.st_dev || target.st_ino != wanted.st_ino);
		}
	}
	return ready && serial == atomic_load(&mutation_serial);
}

bool podcastpair_ready(const char *path) {
	if (!path || !strrchr(path, '/'))
		return false;
	const char *file = strrchr(path, '/') + 1;
	char parent[768];
	size_t n = (size_t)(file - path - 1);
	if (n >= sizeof(parent))
		return false;
	memcpy(parent, path, n);
	parent[n] = 0;
	int dir = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (dir < 0)
		return false;
	bool ready = podcastpair_ready_at(dir, file);
	close(dir);
	return ready;
}
