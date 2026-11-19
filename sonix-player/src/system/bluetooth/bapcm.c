#include "bapcm.h"
#include <string.h>

bool bapcm_volume_read(dbus_reader_t *reader, const char *signature, bapcm_volume_t *volume) {
	bapcm_volume_t parsed = {0};
	if (!strcmp(signature, "q")) {
		uint16_t packed;
		if (!dbus_r_u16(reader, &packed))
			return false;
		parsed.values[0] = packed >> 8;
		parsed.values[1] = packed & 0xff;
		parsed.count = 2;
	} else if (!strcmp(signature, "ay")) {
		dbus_array_iter_t iterator;
		if (!dbus_r_array_begin(reader, "y", &iterator))
			return false;
		while (dbus_r_array_more(reader, &iterator)) {
			if (parsed.count == sizeof(parsed.values) || !dbus_r_byte(reader, &parsed.values[parsed.count]))
				return false;
			parsed.count++;
		}
		if (!parsed.count || reader->bad)
			return false;
		parsed.array = true;
	} else
		return false;
	*volume = parsed;
	return true;
}

void bapcm_volume_write(dbus_writer_t *writer, const bapcm_volume_t *volume) {
	if (!volume->count || volume->count > sizeof(volume->values) || (!volume->array && volume->count != 2)) {
		dbus_w_reject(writer);
		return;
	}
	if (volume->array)
		dbus_w_variant_bytes(writer, volume->values, volume->count);
	else
		dbus_w_variant_u16(writer, (uint16_t)((volume->values[0] << 8) | volume->values[1]));
}
