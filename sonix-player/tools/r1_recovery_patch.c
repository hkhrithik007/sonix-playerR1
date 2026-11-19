// One-time, explicitly authorized repair of one known R1 recovery eraseblock.
// Build separately; the player's explicit recovery wizard invokes this helper:
// cc -std=gnu11 -O2 -D_FILE_OFFSET_BITS=64 -I. tools/r1_recovery_patch.c src/system/core/sha256.c -o r1_recovery_patch
// --check opens only read-only descriptors. --apply requires the complete
// reviewed 128 KiB block, not the smaller compressed SquashFS extent.
// Errors stop immediately. No reboot, erase retry, bad-block skipping or rollback.
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <mtd/mtd-user.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/core/sha256.h"
#include "src/system/device/recovery_r1.h"

#define R1_INFO "/usr/resource/sonix/components/system-info.json"
#define R1_SELECTOR "/dev/mtd5"
#define SELECTOR_SIZE 256u

static unsigned char new_block[R1_RECOVERY_ERASE_SIZE];

static bool refuse(const char *reason) {
	fprintf(stderr, "R1_RECOVERY_REFUSED: %s\n", reason);
	return false;
}

static bool read_exact(int fd, void *data, size_t size, off_t offset) {
	unsigned char *out = data;
	while (size) {
		ssize_t got = pread(fd, out, size, offset);
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			return false;
		out += got;
		offset += got;
		size -= (size_t)got;
	}
	return true;
}

static bool hash_region(int fd, off_t offset, size_t size, char digest[SHA256_HEX_LEN]) {
	sha256_ctx context;
	sha256_init(&context);
	unsigned char bytes[8192];
	while (size) {
		size_t part = size < sizeof(bytes) ? size : sizeof(bytes);
		if (!read_exact(fd, bytes, part, offset))
			return false;
		sha256_update(&context, bytes, part);
		offset += (off_t)part;
		size -= part;
	}
	sha256_final_hex(&context, digest);
	return true;
}

static bool model_is_r1(void) {
	int fd = open(R1_INFO, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return refuse("model file cannot be opened");
	struct stat st;
	char json[4096];
	bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size < (off_t)sizeof(json) && read_exact(fd, json, (size_t)st.st_size, 0);
	if (close(fd) != 0)
		ok = false;
	if (!ok)
		return refuse("model file cannot be read completely");
	json[st.st_size] = '\0';
	if (strlen(json) != (size_t)st.st_size)
		return refuse("model file contains embedded NUL");
	// This local metadata uses string fields. Require one exact model key and
	// value; another model, duplicate key or an unexpected encoding is refused.
	const char *key = "\"device-name\"";
	char *p = strstr(json, key);
	if (!p || strstr(p + strlen(key), key))
		return refuse("model key is missing or ambiguous");
	p += strlen(key);
	while (isspace((unsigned char)*p))
		p++;
	if (*p++ != ':')
		return refuse("model field has an unexpected format");
	while (isspace((unsigned char)*p))
		p++;
	const char *model = "\"HiBy R1\"";
	if (strncmp(p, model, strlen(model)))
		return refuse("device is not the reviewed HiBy R1");
	p += strlen(model);
	while (isspace((unsigned char)*p))
		p++;
	if (*p != ',' && *p != '}')
		return refuse("model field is not terminated");
	return true;
}

static bool selector_is_normal(void) {
	int fd = open(R1_SELECTOR, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return refuse("boot selector cannot be opened");
	struct stat st;
	unsigned char actual[SELECTOR_SIZE], expected[SELECTOR_SIZE];
	memset(expected, ' ', sizeof(expected));
	memcpy(expected, "ota:kernel", strlen("ota:kernel"));
	bool ok = fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) && read_exact(fd, actual, sizeof(actual), 0) && memcmp(actual, expected, sizeof(actual)) == 0;
	if (close(fd) != 0)
		ok = false;
	return ok || refuse("boot selector is not exactly normal boot");
}

static bool check_geometry(int fd) {
	struct stat st;
	struct mtd_info_user info;
	if (fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode) || ioctl(fd, MEMGETINFO, &info) != 0)
		return refuse("recovery is not a readable MTD character device");
	if (info.type != MTD_NANDFLASH || info.size != R1_RECOVERY_SIZE || info.erasesize != R1_RECOVERY_ERASE_SIZE || info.writesize != R1_RECOVERY_WRITE_SIZE)
		return refuse("recovery NAND geometry differs from the reviewed device");
	// The reference was read through the stock BBT-aware device. It is only
	// equivalent to raw physical offsets when every physical block is good.
	for (int64_t offset = 0; offset < R1_RECOVERY_SIZE; offset += R1_RECOVERY_ERASE_SIZE) {
		if (ioctl(fd, MEMGETBADBLOCK, &offset) != 0)
			return refuse("a bad block or unreadable bad-block map prevents a physical-offset repair");
	}
	return true;
}

static bool current_image(int fd, bool *already_applied) {
	char whole[SHA256_HEX_LEN], block[SHA256_HEX_LEN];
	if (!check_geometry(fd) || !hash_region(fd, 0, R1_RECOVERY_SIZE, whole))
		return refuse("complete recovery preflight failed");
	*already_applied = strcmp(whole, R1_RECOVERY_NEW_SHA256) == 0;
	if (!*already_applied && strcmp(whole, R1_RECOVERY_OLD_SHA256))
		return refuse("complete recovery SHA256 is not whitelisted");
	if (!hash_region(fd, R1_RECOVERY_BLOCK_OFFSET, R1_RECOVERY_ERASE_SIZE, block) || strcmp(block, *already_applied ? R1_RECOVERY_NEW_BLOCK_SHA256 : R1_RECOVERY_OLD_BLOCK_SHA256))
		return refuse("target eraseblock SHA256 does not match the complete image");
	return true;
}

static bool load_new_block(const char *path) {
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return refuse("replacement block cannot be opened");
	struct stat st;
	bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == sizeof(new_block) && read_exact(fd, new_block, sizeof(new_block), 0);
	if (close(fd) != 0)
		ok = false;
	if (!ok)
		return refuse("replacement must be one complete readable 128 KiB regular file");
	sha256_ctx context;
	char digest[SHA256_HEX_LEN];
	sha256_init(&context);
	sha256_update(&context, new_block, sizeof(new_block));
	sha256_final_hex(&context, digest);
	return !strcmp(digest, R1_RECOVERY_NEW_BLOCK_SHA256) || refuse("replacement block SHA256 differs");
}

static bool same_device(int lock_fd, int fd) {
	struct stat locked, current;
	return fstat(lock_fd, &locked) == 0 && fstat(fd, &current) == 0 && S_ISCHR(current.st_mode) && locked.st_dev == current.st_dev && locked.st_ino == current.st_ino && locked.st_rdev == current.st_rdev;
}

static bool apply_checked_block(int lock_fd, bool *found_already) {
	int fd = open(R1_RECOVERY_DEVICE, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return refuse("recovery cannot be opened for the approved repair");
	// Revalidate the writable descriptor and selector immediately before erase.
	// A recreated node would have another inode and bypass the retained lock.
	bool already = false;
	bool ok = same_device(lock_fd, fd) && current_image(fd, &already) && selector_is_normal();
	if (!ok || already) {
		if (close(fd) != 0)
			return refuse("writable recovery close failed");
		*found_already = already;
		return ok || refuse("writable recovery preflight or node identity failed");
	}
	struct erase_info_user erase = {.start = R1_RECOVERY_BLOCK_OFFSET, .length = R1_RECOVERY_ERASE_SIZE};
	if (ioctl(fd, MEMERASE, &erase) != 0) {
		close(fd);
		return refuse("erase failed; stopped without retry or reboot");
	}
	for (size_t at = 0; at < sizeof(new_block); at += R1_RECOVERY_WRITE_SIZE) {
		ssize_t done = pwrite(fd, new_block + at, R1_RECOVERY_WRITE_SIZE, R1_RECOVERY_BLOCK_OFFSET + (off_t)at);
		if (done != R1_RECOVERY_WRITE_SIZE) {
			close(fd);
			return refuse("page write failed or was short; stopped without retry or reboot");
		}
	}
	if (close(fd) != 0)
		return refuse("write close failed; stopped without retry or reboot");
	fd = open(R1_RECOVERY_DEVICE, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return refuse("cannot reopen raw recovery for readback");
	already = false;
	ok = same_device(lock_fd, fd) && current_image(fd, &already) && already;
	if (close(fd) != 0)
		ok = false;
	return ok || refuse("complete raw readback failed; stopped without retry or reboot");
}

int main(int argc, char **argv) {
	bool apply = argc == 3 && strcmp(argv[1], "--apply") == 0;
	if (!apply && !(argc == 2 && strcmp(argv[1], "--check") == 0)) {
		fprintf(stderr, "Usage: r1_recovery_patch --check | --apply reviewed-128KiB-block.bin\n");
		return 2;
	}
	if (!model_is_r1() || !selector_is_normal())
		return 1;
	int fd = open(R1_RECOVERY_DEVICE, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return refuse("recovery cannot be opened read-only"), 1;
	// The separate read-only descriptor owns an exclusive advisory lock for
	// the entire apply, including writable close and the new readback open.
	// Every helper instance must acquire it before checking or changing NAND.
	// --check does not lock and does not create any local files.
	bool ok = !apply || flock(fd, LOCK_EX | LOCK_NB) == 0;
	if (!ok)
		refuse("another repair holds the device, or exclusive locking is unavailable");
	bool already = false;
	if (ok)
		ok = current_image(fd, &already);
	if (ok && apply && !already)
		ok = load_new_block(argv[2]) && apply_checked_block(fd, &already);
	if (close(fd) != 0)
		ok = refuse("read-only recovery/lock close failed");
	if (!ok)
		return 1;
	if (already)
		puts("R1_RECOVERY_ALREADY_APPLIED: complete partition and block verified; no writes");
	else if (apply)
		puts("R1_RECOVERY_PATCH_VERIFIED: complete raw partition SHA256 matches the reviewed fixed image");
	else
		puts("R1_RECOVERY_CHECK_READY: known old image, normal boot, exact geometry, no bad blocks; no writes");
	return 0;
}
