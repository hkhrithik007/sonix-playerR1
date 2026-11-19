#include "vorbisdec.h"
#include "growstdio.h"
#include "stb_vorbis_decl.h"

#include <limits.h>
#include <stdlib.h>

struct vorbisdec {
	stb_vorbis *decoder;
	growstdio_t *io;
	growfile_t *reader;
	FILE *file;
	long start;
	uint64_t length, position;
	int channels, rate;
	bool repair;
};

static bool position(vorbisdec_t *s, uint64_t target) {
	if (target > UINT_MAX) {
		return false;
	}
	if (!s->io) {
		return stb_vorbis_seek(s->decoder, (unsigned)target) != 0;
	}
	if (!growstdio_probe(s->io, true)) {
		return false;
	}
	long bytes = growstdio_length(s->io) - s->start;
	if (bytes < 0) {
		growstdio_probe(s->io, false);
		return false;
	}
	stb_vorbis_set_file_length(s->decoder, (unsigned)bytes);
	uint64_t available = stb_vorbis_stream_length_in_samples(s->decoder);
	uint64_t anchor = target;
	if (target > available) {
		// A partial last page may have yielded samples before a pause. Seek to
		// the last complete page, then replay only that page to the exact spot.
		anchor = available > (uint64_t)s->rate ? available - (uint64_t)s->rate : 0;
	}
	bool ok = target - anchor <= (uint64_t)s->rate * 30 && stb_vorbis_seek(s->decoder, (unsigned)anchor);
	short scratch[4096];
	while (ok && anchor < target) {
		uint64_t count = target - anchor;
		uint64_t capacity = sizeof(scratch) / sizeof(scratch[0]) / (unsigned)s->channels;
		if (count > capacity) {
			count = capacity;
		}
		int got = stb_vorbis_get_samples_short_interleaved(s->decoder, s->channels, scratch, (int)count * s->channels);
		if (got <= 0) {
			ok = false;
		} else {
			anchor += (unsigned)got;
		}
	}
	if (!growstdio_probe(s->io, false)) {
		ok = false;
	}
	return ok;
}

vorbisdec_t *vorbisdec_open(FILE *file, growstdio_t *io, growfile_t *reader) {
	vorbisdec_t *s = calloc(1, sizeof(*s));
	if (!s) {
		if (io) {
			growstdio_close(io);
		} else if (file) {
			fclose(file);
		}
		return NULL;
	}
	s->reader = reader;
	s->file = file;
	s->io = io;
	if (!file) {
		vorbisdec_close(s);
		return NULL;
	}
	int error = 0;
	s->start = ftell(file);
	s->decoder = stb_vorbis_open_file(file, 0, &error, NULL);
	if (!s->decoder) {
		vorbisdec_close(s);
		return NULL;
	}
	stb_vorbis_info info = stb_vorbis_get_info(s->decoder);
	s->channels = info.channels;
	s->rate = (int)info.sample_rate;
	s->length = stb_vorbis_stream_length_in_samples(s->decoder);
	uint32_t duration = growfile_duration(reader);
	if (duration) {
		s->length = (uint64_t)duration * (unsigned)s->rate / 1000;
	}
	if (s->io && !growstdio_probe(s->io, false)) {
		vorbisdec_close(s);
		return NULL;
	}
	return s;
}

int vorbisdec_channels(const vorbisdec_t *s) { return s ? s->channels : 0; }
int vorbisdec_rate(const vorbisdec_t *s) { return s ? s->rate : 0; }
uint64_t vorbisdec_length(const vorbisdec_t *s) { return s ? s->length : 0; }

uint64_t vorbisdec_read(vorbisdec_t *s, uint64_t frames, short *out) {
	if (!s || !out || s->channels <= 0) {
		return 0;
	}
	if (s->repair) {
		if (!position(s, s->position)) {
			return 0;
		}
		s->repair = false;
	}
	if (frames > (uint64_t)INT_MAX / (unsigned)s->channels) {
		frames = (uint64_t)INT_MAX / (unsigned)s->channels;
	}
	int got = stb_vorbis_get_samples_short_interleaved(s->decoder, s->channels, out, (int)frames * s->channels);
	s->position += (unsigned)got;
	if (s->io && (uint64_t)got < frames && s->position < s->length) {
		// Restore on the next read, after pause/rebuffering, never while a
		// stop or another track is waiting for this call to return.
		s->repair = true;
	}
	return (unsigned)got;
}

bool vorbisdec_seek(vorbisdec_t *s, uint64_t frame) {
	if (!s) {
		return false;
	}
	if (!position(s, frame)) {
		s->repair = !position(s, s->position);
		return false;
	}
	s->position = frame;
	s->repair = false;
	return true;
}

void vorbisdec_close(vorbisdec_t *s) {
	if (s) {
		if (s->decoder) {
			stb_vorbis_close(s->decoder);
		}
		growstdio_close(s->io);
		if (!s->io && s->file) {
			fclose(s->file);
		}
		free(s);
	}
}
