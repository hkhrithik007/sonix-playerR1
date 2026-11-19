#ifndef GROWSTDIO_H
#define GROWSTDIO_H

#include "growfile.h"
#include <stdbool.h>
#include <stdio.h>

typedef struct growstdio growstdio_t;

// The adapter borrows the growfile reader. Close it before growfile_close().
growstdio_t *growstdio_open(growfile_t *reader);
FILE *growstdio_file(growstdio_t *stream);
// Length/index probes see a bounded snapshot, not a wait for future pages.
bool growstdio_probe(growstdio_t *stream, bool enabled);
long growstdio_length(const growstdio_t *stream);
void growstdio_close(growstdio_t *stream);

#endif
