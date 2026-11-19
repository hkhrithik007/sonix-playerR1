#include "streamcover.h"

#include "src/system/image/stb_image_decl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

bool streamcover_valid(const void *data, size_t size) {
	const unsigned char *p = data;
	if (!p || size < 12 || size > STREAMCOVER_MAX_BYTES) {
		return false;
	}
	bool jpeg = p[0] == 0xff && p[1] == 0xd8 && p[2] == 0xff && p[size - 2] == 0xff && p[size - 1] == 0xd9;
	bool png = memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0 &&
			   memcmp(p + size - 12, "\0\0\0\0IEND\xae\x42\x60\x82", 12) == 0;
	bool gif = (memcmp(p, "GIF87a", 6) == 0 || memcmp(p, "GIF89a", 6) == 0) && p[size - 1] == 0x3b;
	int w, h, channels;
	return (jpeg || png || gif) && stbi_info_from_memory(p, (int)size, &w, &h, &channels) && w > 0 && h > 0;
}

bool streamcover_file_valid(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	struct stat st;
	bool ok = fstat(fileno(f), &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 12 && st.st_size <= STREAMCOVER_MAX_BYTES;
	unsigned char *data = ok ? malloc((size_t)st.st_size) : NULL;
	ok = data && fread(data, 1, (size_t)st.st_size, f) == (size_t)st.st_size && streamcover_valid(data, (size_t)st.st_size);
	free(data);
	if (fclose(f) != 0) {
		ok = false;
	}
	return ok;
}

bool streamcover_store(const char *path, const void *data, size_t size) {
	if (!streamcover_valid(data, size)) {
		return false;
	}
	char tmp[640];
	int n = snprintf(tmp, sizeof(tmp), "%s.part-XXXXXX", path);
	if (n < 0 || (size_t)n >= sizeof(tmp)) {
		return false;
	}
	int fd = mkstemp(tmp);
	if (fd < 0) {
		return false;
	}
	FILE *f = fdopen(fd, "wb");
	bool ok = f && fwrite(data, 1, size, f) == size;
	if (f) {
		if (fclose(f) != 0) {
			ok = false;
		}
	} else {
		close(fd);
	}
	if (!ok || rename(tmp, path) != 0) {
		unlink(tmp);
		return false;
	}
	return true;
}
