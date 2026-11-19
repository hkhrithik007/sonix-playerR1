#ifndef VORBISDEC_H
#define VORBISDEC_H

#include "growfile.h"
#include "growstdio.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct vorbisdec vorbisdec_t;
// Takes file/io ownership, including on failure. reader is optional/borrowed.
vorbisdec_t *vorbisdec_open(FILE *file, growstdio_t *io, growfile_t *reader);
int vorbisdec_channels(const vorbisdec_t *decoder);
int vorbisdec_rate(const vorbisdec_t *decoder);
uint64_t vorbisdec_length(const vorbisdec_t *decoder);
uint64_t vorbisdec_read(vorbisdec_t *decoder, uint64_t frames, short *out);
bool vorbisdec_seek(vorbisdec_t *decoder, uint64_t frame);
void vorbisdec_close(vorbisdec_t *decoder);

#endif
