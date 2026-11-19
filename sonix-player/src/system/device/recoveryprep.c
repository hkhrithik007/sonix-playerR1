#define _GNU_SOURCE
#include "recoveryprep.h"
#include "src/system/core/sha256.h"
#include "src/system/device/recovery_r1.h"
#include "src/system/device/system.h"
#include "src/system/device/usb.h"
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef RECOVERY_PREP_HELPER
#define RECOVERY_PREP_HELPER "/usr/bin/sonix_recovery_r1"
#endif
#ifndef RECOVERY_PREP_KERNEL
#define RECOVERY_PREP_KERNEL "/dev/mtd3"
#endif
#define HELPER RECOVERY_PREP_HELPER
#define BLOCK "/usr/resource/sonix/components/r1-recovery-ro.bin"
#define BACKUP_DIR "RecoveryBackup"
extern char **environ;
static atomic_bool maintenance_active;
bool recovery_prep_active(void) { return maintenance_active; }
void recovery_prep_set_active(bool active) { maintenance_active = active; }

#ifndef HOST_BUILD
static char saved_directory[128];
static bool backup_ready;
static char saved_kernel_sha[SHA256_HEX_LEN];

static bool verify_file(int fd, size_t bytes, const char *expected) {
	if (fd < 0)
		return false;
	struct stat st;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size != (off_t)bytes)
		return false;
	sha256_ctx hash;
	sha256_init(&hash);
	unsigned char data[16384];
	while (bytes) {
		ssize_t got = read(fd, data, bytes < sizeof(data) ? bytes : sizeof(data));
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			return false;
		sha256_update(&hash, data, (size_t)got);
		bytes -= (size_t)got;
	}
	char digest[SHA256_HEX_LEN];
	sha256_final_hex(&hash, digest);
	return strcmp(digest, expected) == 0;
}
static bool run_helper(bool apply, char *answer, size_t size) {
	int pipefd[2];
	if (pipe2(pipefd, O_CLOEXEC) != 0) {
		return false;
	}
	posix_spawn_file_actions_t actions;
	if (posix_spawn_file_actions_init(&actions) != 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return false;
	}
	int rc = posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
	if (!rc)
		rc = posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
	char *argv[] = {HELPER, apply ? "--apply" : "--check", apply ? BLOCK : NULL, NULL};
	pid_t child = -1;
	if (!rc)
		rc = posix_spawn(&child, HELPER, &actions, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&actions);
	close(pipefd[1]);
	size_t used = 0;
	char part[256];
	ssize_t got;
	while ((got = read(pipefd[0], part, sizeof(part))) > 0 || (got < 0 && errno == EINTR)) {
		if (got <= 0)
			continue;
		size_t keep = (size_t)got < size - 1 - used ? (size_t)got : size - 1 - used;
		memcpy(answer + used, part, keep);
		used += keep;
	}
	answer[used] = '\0';
	close(pipefd[0]);
	if (rc)
		return false;
	int status;
	while (waitpid(child, &status, 0) < 0) {
		if (errno != EINTR)
			return false;
	}
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool copy_verified(int directory, const char *source, const char *name, size_t bytes, const char *expected) {
	int in = open(source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (in < 0 || flock(in, LOCK_SH | LOCK_NB) != 0) {
		if (in >= 0)
			close(in);
		return false;
	}
	char temporary[128];
	snprintf(temporary, sizeof(temporary), ".%s.%ld.part", name, (long)getpid());
	int out = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (out < 0) {
		close(in);
		return false;
	}
	sha256_ctx hash;
	sha256_init(&hash);
	unsigned char data[16384];
	size_t left = bytes;
	bool ok = true;
	while (left && ok) {
		size_t want = left < sizeof(data) ? left : sizeof(data);
		ssize_t n = read(in, data, want);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			ok = false;
			break;
		}
		sha256_update(&hash, data, (size_t)n);
		size_t at = 0;
		while (at < (size_t)n) {
			ssize_t wrote = write(out, data + at, (size_t)n - at);
			if (wrote < 0 && errno == EINTR)
				continue;
			if (wrote <= 0) {
				ok = false;
				break;
			}
			at += (size_t)wrote;
		}
		left -= (size_t)n;
	}
	char digest[SHA256_HEX_LEN];
	sha256_final_hex(&hash, digest);
	if (expected && strcmp(digest, expected))
		ok = false;
	if (fsync(out) != 0)
		ok = false;
	if (close(out) != 0)
		ok = false;
	if (close(in) != 0)
		ok = false;
	out = openat(directory, temporary, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	sha256_init(&hash);
	left = bytes;
	while (out >= 0 && left && ok) {
		ssize_t n = read(out, data, left < sizeof(data) ? left : sizeof(data));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			ok = false;
			break;
		}
		sha256_update(&hash, data, (size_t)n);
		left -= (size_t)n;
	}
	char readback[SHA256_HEX_LEN];
	sha256_final_hex(&hash, readback);
	if (out < 0 || left || strcmp(digest, readback))
		ok = false;
	if (out >= 0 && close(out) != 0)
		ok = false;
	// A saved backup is never silently replaced by another run.
	if (ok && linkat(directory, temporary, directory, name, 0) != 0) {
		// FAT has no hard links: rename is permitted only into a fresh directory.
		struct stat st;
		if (fstatat(directory, name, &st, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT || renameat(directory, temporary, directory, name) != 0)
			ok = false;
	}
	unlinkat(directory, temporary, 0);
	if (ok && fsync(directory) != 0)
		ok = false;
	if (ok && !expected)
		snprintf(saved_kernel_sha, sizeof(saved_kernel_sha), "%s", digest);
	return ok;
}
#endif

recovery_prep_result_t recovery_prep_check(void) {
#ifdef HOST_BUILD
	return RECOVERY_PREP_UNSUPPORTED;
#else
	char answer[512];
	if (!run_helper(false, answer, sizeof(answer)))
		return RECOVERY_PREP_UNSUPPORTED;
	if (strstr(answer, "R1_RECOVERY_ALREADY_APPLIED:"))
		return RECOVERY_PREP_FIXED;
	return strstr(answer, "R1_RECOVERY_CHECK_READY:") ? RECOVERY_PREP_READY : RECOVERY_PREP_UNSUPPORTED;
#endif
}

recovery_prep_result_t recovery_prep_backup(void) {
#ifdef HOST_BUILD
	return RECOVERY_PREP_UNSUPPORTED;
#else
	if (recovery_prep_check() != RECOVERY_PREP_READY)
		return RECOVERY_PREP_FAILED;
	backup_ready = false;
	char root[1024];
	int pin = storage_pin_card(root, sizeof(root));
	if (pin < 0)
		return RECOVERY_PREP_FAILED;
	struct statvfs space;
	bool ok = storage_sd_writable() && fstatvfs(pin, &space) == 0 && (uint64_t)space.f_bavail * space.f_frsize >= 40u * 1024u * 1024u;
	if (ok && mkdirat(pin, BACKUP_DIR, 0700) != 0 && errno != EEXIST)
		ok = false;
	int parent = ok ? openat(pin, BACKUP_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW) : -1;
	char directory[64];
	snprintf(directory, sizeof(directory), "r1-%ld-%ld", (long)time(NULL), (long)getpid());
	if (parent < 0 || mkdirat(parent, directory, 0700) != 0)
		ok = false;
	int dir = ok ? openat(parent, directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW) : -1;
	if (dir < 0)
		ok = false;
	if (ok)
		ok = copy_verified(dir, R1_RECOVERY_DEVICE, "rootfs2.bin", R1_RECOVERY_SIZE, R1_RECOVERY_OLD_SHA256);
	if (ok)
		ok = copy_verified(dir, RECOVERY_PREP_KERNEL, "kernel2.bin", 5u * 1024u * 1024u, NULL);
	if (ok)
		snprintf(saved_directory, sizeof(saved_directory), BACKUP_DIR "/%s", directory);
	if (dir >= 0 && close(dir) != 0)
		ok = false;
	if (parent >= 0) {
		if (fsync(parent) != 0)
			ok = false;
		close(parent);
	}
	if (fsync(pin) != 0)
		ok = false;
	close(pin);
	backup_ready = ok;
	return ok ? RECOVERY_PREP_BACKED_UP : RECOVERY_PREP_FAILED;
#endif
}

recovery_prep_result_t recovery_prep_apply(void) {
#ifdef HOST_BUILD
	return RECOVERY_PREP_UNSUPPORTED;
#else
	if (!backup_ready)
		return RECOVERY_PREP_FAILED;
	sync_battery_from_sysfs();
	const char *level = read_battery_percent();
	char *end;
	long percent = level ? strtol(level, &end, 10) : -1;
	if (!level || end == level || *end || percent < 50 || percent > 100 || !usb_vbus_present())
		return RECOVERY_PREP_FAILED;
	char root[1024];
	int pin = storage_pin_card(root, sizeof(root));
	int directory = pin >= 0 ? openat(pin, saved_directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW) : -1;
	int backup = directory >= 0 ? openat(directory, "rootfs2.bin", O_RDONLY | O_CLOEXEC | O_NOFOLLOW) : -1;
	bool ok = verify_file(backup, R1_RECOVERY_SIZE, R1_RECOVERY_OLD_SHA256);
	if (backup >= 0)
		close(backup);
	backup = directory >= 0 ? openat(directory, "kernel2.bin", O_RDONLY | O_CLOEXEC | O_NOFOLLOW) : -1;
	if (!verify_file(backup, 5u * 1024u * 1024u, saved_kernel_sha))
		ok = false;
	if (backup >= 0)
		close(backup);
	if (directory >= 0)
		close(directory);
	if (!ok) {
		if (pin >= 0)
			close(pin);
		return RECOVERY_PREP_FAILED;
	}
	char answer[512];
	// The helper independently rechecks the exact image, selector, NAND map,
	// replacement block and full readback under its exclusive device lock.
	ok = run_helper(true, answer, sizeof(answer));
	close(pin);
	backup_ready = false;
	if (!ok) {
		recovery_prep_result_t checked = recovery_prep_check();
		return checked == RECOVERY_PREP_FIXED ? checked : checked == RECOVERY_PREP_READY ? RECOVERY_PREP_FAILED : RECOVERY_PREP_UNCERTAIN;
	}
	return recovery_prep_check() == RECOVERY_PREP_FIXED ? RECOVERY_PREP_FIXED : RECOVERY_PREP_UNCERTAIN;
#endif
}
