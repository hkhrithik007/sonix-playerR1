#include "firmware.h"
#include "src/system/device/recoveryprep.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/audio/audio.h"
#include "src/system/core/config.h"
#include "src/system/device/clock.h"
#include "src/system/device/power.h"
#include "src/system/device/sysinfo.h"
#include "src/system/device/system.h"
#include "src/system/playback/device_state.h"
#include "src/system/remote/dlna.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/provider.h"

// The card is scanned once and every name compared case-insensitively,
// rather than guessing at spellings: FAT is case-insensitive but exFAT and
// ext4 are not, and a file written by Windows can land as UPDATE.UPT while
// the stock player only ever looks for update.upt. One readdir covers them
// all -- and there is no cheaper way to be right about it.
static bool find_in_dir(const char *root, const char *stem, char *out, size_t out_size) {
	char wanted[80];
	snprintf(wanted, sizeof(wanted), "%s.upt", stem);

	DIR *dir = opendir(root);
	if (!dir) {
		return false;
	}

	bool found = false;
	struct dirent *de;
	while (!found && (de = readdir(dir)) != NULL) {
		if (strcasecmp(de->d_name, wanted) != 0) {
			continue;
		}
		snprintf(out, out_size, "%s/%s", root, de->d_name);

		struct stat st;
		if (stat(out, &st) == 0 && S_ISREG(st.st_mode)) {
			found = true;
		}
	}

	closedir(dir);
	if (!found) {
		out[0] = '\0';
	}
	return found;
}

const char *firmware_update_stem(char *buffer, size_t size) {
	// ONLY the name of THIS model -- deliberately NOT the stock binary's
	// "update.upt" fallback.
	//
	// The recovery kernel does not check what it is given: a .upt built for
	// another HiBy model, dropped on the card under the generic name, would be
	// written to the flash exactly the same way, and the device would not come
	// back. "update.upt" is a name anyone might use for anything; "r3proii.upt"
	// is a claim about which player the file is for.
	//
	// An unrecognised player therefore gets no name at all: a fallback would
	// hand an R1 the R3 Pro II's image. The config key is the way out for
	// anyone who knows what they are doing.
	const char *name = config_get_copy("firmware", "name", "", buffer, size);
	if (name && *name) {
		return name;
	}
	const sysinfo_model_t *model = sysinfo_model();
	if (!model) {
		fprintf(stderr, "firmware: device-name in system-info.json names no player this build knows"
						" ('%s'); no update file will be looked for\n",
				sysinfo_device_name());
		return NULL;
	}
	return model->update_stem;
}

bool firmware_update_file_find(char *out, size_t out_size) {
	if (!out || out_size == 0) {
		return false;
	}
	out[0] = '\0';

	const char *root = storage_sd_root();
	if (!root || !*root) {
		return false;
	}

	const char *name = firmware_update_stem((char[CONFIG_VALUE_SIZE]){0}, CONFIG_VALUE_SIZE);
	if (!name) {
		return false;
	}
	return find_in_dir(root, name, out, out_size);
}

#ifndef HOST_BUILD
static bool set_bootmode(bool recovery) {
	const char *command = recovery ? "/usr/bin/bootmode.sh Recovery" : "/usr/bin/bootmode.sh Normal";
	if (system(command) != 0)
		return false;
	// Read the character device, not a cached block alias of the selector.
	// A successful shell command alone does not prove what U-Boot will read.
	int fd = open("/dev/mtd5", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	char data[256];
	size_t got = 0;
	while (got < sizeof(data)) {
		ssize_t n = read(fd, data + got, sizeof(data) - got);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		got += (size_t)n;
	}
	bool ok = close(fd) == 0 && got == sizeof(data);
	const char *wanted = recovery ? "ota:kernel2" : "ota:kernel";
	size_t length = strlen(wanted);
	ok = ok && memcmp(data, wanted, length) == 0;
	for (size_t i = length; ok && i < sizeof(data); i++)
		ok = data[i] == ' ';
	return ok;
}
#endif

firmware_update_result_t firmware_update_start(void) {
	if (recovery_prep_active())
		return FIRMWARE_UPDATE_PREPARE_FAILED;
#ifdef HOST_BUILD
	printf("firmware: (host build) would arm recovery and reboot\n");
	return FIRMWARE_UPDATE_SIMULATED;
#else
	printf("firmware: preparing update\n");

	// The screen fades out the way the power key puts it out, first: what
	// follows holds this thread for seconds, and a frozen page reads as a
	// device that has hung. A dark one reads as a device that is restarting.
	power_screen_off();

	// Playback down first: the card is about to be handed to the recovery
	// kernel, and the decoder still has it open.
	device_state_remember_flush();
	audio_stop();

	// The streaming caches (tracks and covers) survive no power cycle, the
	// update's included.
	stream_provider_clear_on_exit_all();
	podcastcache_clear_on_exit();
	dlna_clear_on_exit();

	// The clock goes into the RTC on the way out, like every other exit path
	// -- recovery reboots the device a second time when it is done, and the
	// time would otherwise be whatever the RTC last knew.
	clock_shutdown();

	// The recovery kernel reads the update off the card: it finds it clean.
	if (!storage_prepare_for_update()) {
		fprintf(stderr, "firmware: card preparation failed; no recovery arm or reboot\n");
		power_screen_on();
		return FIRMWARE_UPDATE_PREPARE_FAILED;
	}

	// Two commands: erase the first block of /dev/mtd5, write "ota:kernel2"
	// into it. That is the whole of the stock firmware's bootmode.sh, and it
	// is what U-Boot reads to decide which kernel to start.
	if (!set_bootmode(true)) {
		fprintf(stderr, "firmware: recovery selector not confirmed; restoring normal boot\n");
		bool restored = set_bootmode(false);
		if (restored)
			storage_cancel_update();
		else {
			fprintf(stderr, "firmware: normal boot selector not confirmed; automatic reboot forbidden\n");
			power_hold_screen_on(true);
			power_set_auto_off(false, 0);
		}
		power_screen_on();
		return restored ? FIRMWARE_UPDATE_PREPARE_FAILED : FIRMWARE_UPDATE_BOOTMODE_UNCERTAIN;
	}

	sync();
	sleep(1);

	int rc = system("reboot");
	(void)rc;

	// Spins here rather than returning, as the stock binary does: with recovery
	// already armed there is nothing sensible left to do, and going back to the
	// settings page would leave the device one power-cycle away from an update
	// the user is no longer expecting.
	sleep(10);
	reboot(RB_AUTOBOOT);
	for (;;) {
		sleep(1);
	}
#endif
}
