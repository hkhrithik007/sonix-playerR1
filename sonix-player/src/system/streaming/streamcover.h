#ifndef STREAMCOVER_H
#define STREAMCOVER_H

#include <stdbool.h>
#include <stddef.h>

#define STREAMCOVER_MAX_BYTES (2 * 1024 * 1024)

// Bounded header and end-marker checks, not a full pixel decode. Disk callers
// must hold a storage pin; HTTP waits must happen before taking that pin.
bool streamcover_valid(const void *data, size_t size);
bool streamcover_file_valid(const char *path);
// Writes alongside the destination and publishes only after checked close.
// Failure leaves the previous file intact and removes this call's temp file.
bool streamcover_store(const char *path, const void *data, size_t size);

#endif
