#define _GNU_SOURCE 1
#include "growstdio.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

struct growstdio {
	growfile_t *reader;
	FILE *file;
	long limit;
	bool probe;
};

static ssize_t read_cookie(void *cookie, char *out, size_t size) {
	growstdio_t *s = cookie;
	if (s->probe) {
		long left = s->limit - growfile_tell(s->reader);
		if (left <= 0) {
			return 0;
		}
		if (size > (size_t)left) {
			size = (size_t)left;
		}
	}
	return (ssize_t)growfile_read(s->reader, out, size);
}

static int seek_cookie(void *cookie, off64_t *offset, int origin) {
	growstdio_t *s = cookie;
	int64_t base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? growfile_tell(s->reader) : origin == SEEK_END ? (s->probe ? s->limit : growfile_available(s->reader)) : -1;
	if (base < 0 || *offset < -base || *offset > LONG_MAX - base || !growfile_seek(s->reader, (long)(base + *offset), 0)) {
		errno = EINVAL;
		return -1;
	}
	*offset = base + *offset;
	return 0;
}

static int close_cookie(void *cookie) {
	((growstdio_t *)cookie)->file = NULL;
	return 0;
}

growstdio_t *growstdio_open(growfile_t *reader) {
	if (!reader) {
		return NULL;
	}
	growstdio_t *s = calloc(1, sizeof(*s));
	if (!s) {
		return NULL;
	}
	s->reader = reader;
	s->probe = true;
	s->limit = growfile_available(reader);
	cookie_io_functions_t io = {.read = read_cookie, .seek = seek_cookie, .close = close_cookie};
	s->file = fopencookie(s, "r", io);
	if (!s->file || s->limit < 0) {
		growstdio_close(s);
		return NULL;
	}
	return s;
}

FILE *growstdio_file(growstdio_t *s) { return s ? s->file : NULL; }
long growstdio_length(const growstdio_t *s) { return s ? s->limit : 0; }

bool growstdio_probe(growstdio_t *s, bool enabled) {
	if (!s || !s->file || fseeko(s->file, 0, SEEK_CUR) != 0) {
		return false;
	}
	long available = growfile_available(s->reader);
	if (available < 0) {
		return false;
	}
	s->limit = available;
	s->probe = enabled;
	clearerr(s->file);
	return true;
}

void growstdio_close(growstdio_t *s) {
	if (s) {
		if (s->file) {
			fclose(s->file);
		}
		free(s);
	}
}
