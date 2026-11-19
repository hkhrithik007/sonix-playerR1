#ifndef BAPCM_H
#define BAPCM_H

#include "dbuslite.h"

// Raw channel bytes retain both balance and each channel's mute bit.
typedef struct {
	uint8_t values[8];
	unsigned count;
	bool array;
} bapcm_volume_t;

bool bapcm_volume_read(dbus_reader_t *reader, const char *signature, bapcm_volume_t *volume);
void bapcm_volume_write(dbus_writer_t *writer, const bapcm_volume_t *volume);

#endif
