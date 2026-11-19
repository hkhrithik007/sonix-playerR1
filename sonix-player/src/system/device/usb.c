#include "usb.h"

#include "src/system/device/adb.h"
#include "src/system/device/power.h"
#include "src/system/device/sysinfo.h"
#include "src/system/device/system.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <linux/netlink.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "src/system/audio/usbaudio.h"
#include "src/system/bluetooth/btlog.h"
#include "src/system/core/logging.h"
#include "src/system/core/utils.h"
#include "src/system/device/sysserver.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/library/library.h"

#include "lvgl/lvgl.h"

#include "src/gui/library/browser.h"
#include "src/gui/shell/gui.h"
#include "src/system/audio/audio.h"
#include "src/system/playback/device_state.h"

// ADB owns this separate gadget. While it is running or bound the card stays
// local; exporting it would unmount the filesystem underneath the ADB shell.
#define ADB_GADGET "/sys/kernel/config/usb_gadget/adb_demo"

// VBUS detection. The first node is what the stock binary polls; the second
// is the charger's own (the stock reads that one too). Which of them actually
// flips to 1 on this hardware differs between drivers, so *all* of them are
// polled and any reading 1 means a cable -- the same any-supply logic the
// battery scan uses, which is why the charge icon worked while a single-node
// poll here saw nothing.
static const char *const VBUS_CANDIDATES[] = {
	"/sys/class/power_supply/usb/online",
	"/sys/class/power_supply/mp2731-charger/online",
};

// How often the cable is looked at. Attaching feels immediate at one second;
// the check is two sysfs reads.
#define POLL_SECONDS 1
#define POLL_SECONDS_STANDBY 4

static char sd_device[128];
static char sd_mount[256];

static _Atomic bool storage_active;

// Export and restore run on the watcher thread, and a restore is also asked for
// by the threads that start DAC mode and switch ADB. One at a time: a restore
// landing in the middle of an export would remount the card under a host that
// is about to be handed it.
static pthread_mutex_t storage_lock = PTHREAD_MUTEX_INITIALIZER;

// Counts the times the controller has changed hands (DAC mode, ADB). The
// watcher forgets what it knew about the cable when it moves: an eject done by
// the host before the handover says nothing about the device after it, and a
// card taken back by someone else is no longer exported.
static unsigned owner_serial;

static void owner_changed(void) { __atomic_add_fetch(&owner_serial, 1, __ATOMIC_SEQ_CST); }

// Which way the gadget was exported, so the restore tears down the same one.
static enum { EXPORT_NONE, EXPORT_OWN_GADGET, EXPORT_ADB_COMPOSITE } export_mode;

// What the PC should see in its "device connected" notification: these two
// ids, and the strings below, named after the model this player is running on.
#define USB_ID_VENDOR "0x32BB"
// The product id the stock firmware gives its storage gadget (config.json calls
// it usb_pid, and keeps 0x0004 for the DAC's dac_pid). They were the same number
// here, so a host that had already met this device as a sound card was being
// shown a card reader wearing the same identity.
#define USB_ID_PRODUCT "0x0101"
#define USB_MANUFACTURER "HiBy"

// The product string: the model's name without the maker's, "R3 Pro II" or
// "R1", which is what the stock firmware of each writes.
const char *usb_product(void) {
	const sysinfo_model_t *model = sysinfo_model();
	if (!model) {
		return "R3 Pro II";
	}
	const char *name = model->name;
	return strncasecmp(name, "HiBy ", 5) == 0 ? name + 5 : name;
}

// What the LUN answers to a SCSI INQUIRY: vendor (8), product (16), revision
// (4), "HiBy    R3 Pro II U-DISK0100" on the R3. Without it the mass-storage
// function reports "Linux / File-Stor Gadget", which is what a bare gadget
// looks like; the stock writes its own, and some hosts -- Android among them --
// are fussier about a card reader than a PC is.
static void usb_inquiry_string(char *out, size_t out_size) {
	char product[40];
	snprintf(product, sizeof(product), "%s U-DISK", usb_product());
	snprintf(out, out_size, "%-8s%-16.16s0100", USB_MANUFACTURER, product);
}

#define STORAGE_GADGET "/sys/kernel/config/usb_gadget/android0"

bool usb_storage_active(void) { return storage_active; }

// ---------------------------------------------------------------------------
// UI notifications: the worker thread must not touch LVGL, so everything the
// user sees goes through gui_post onto the interface thread.
// ---------------------------------------------------------------------------

static void popup_connected_cb(void *unused) {
	(void)unused;
	gui_notify_popup("usb_storage_shared");
}

static void popup_disconnected_cb(void *unused) {
	(void)unused;
	// Covers the unplug and the PC-side eject alike.
	gui_notify_popup("usb_storage_available_again");
	browser_refresh(); // the PC may have changed the card content
}

static bool vbus_present(void) {
	for (size_t i = 0; i < sizeof(VBUS_CANDIDATES) / sizeof(VBUS_CANDIDATES[0]); i++) {
		char *content = read_file_content(VBUS_CANDIDATES[i]);
		if (!content) {
			continue;
		}
		bool on = content[0] == '1';
		free(content);
		if (on) {
			return true;
		}
	}
	return false;
}

// True while the ADB gadget exists and is bound to the USB controller (its
// UDC file holds the controller's name). Bound means the controller is busy
// and the mass-storage script's own gadget could never attach.
static bool adb_gadget_bound(void) {
	char *content = read_file_content(ADB_GADGET "/UDC");
	if (!content) {
		return false;
	}
	content[strcspn(content, "\r\n")] = '\0';
	bool bound = content[0] != '\0';
	free(content);
	return bound;
}

// Set while DAC mode owns the USB controller: nothing in this file may touch
// the gadget then. Without it the watcher thread sees a cable, calls
// ensure_storage_gadget() and rebuilds the mass-storage gadget straight over
// the audio one, taking the stream with it and handing the computer a card it
// should not have. Honoured in the two places that build or bind anything.
static volatile bool dac_owns_usb;

// Entering and leaving DAC mode moves the card back and forth exactly the way
// a cable being plugged in does, which would announce "Memoria di nuovo
// disponibile" or "Collegato al PC" for something the user did not do and
// cannot see. The popups are silenced for the duration of the transition.
static volatile bool suppress_popups;
static void ensure_storage_gadget(void);
static void take_card_back(const char *why);

void usb_set_dac_mode(bool on) {
	// The flag goes up before anything else: setting it afterwards leaves a
	// window in which the watcher thread can wake, see a cable and re-export
	// the card this call is in the middle of taking back.
	suppress_popups = true;
	if (on) {
		dac_owns_usb = true;
		// Take the card back from the computer. In DAC mode the player is a
		// sound card and nothing else: handing over the microSD at the same
		// time is not a bonus, it is the card being pulled out from under the
		// player while it works.
		//
		// Only when it was handed over. A restore with nothing exported still
		// unmounts and remounts the card the player is reading from.
		take_card_back("DAC mode takes the controller");
	}
	dac_owns_usb = on;
	owner_changed();
	fprintf(stderr, "usb: controller %s\n", on ? "handed to DAC mode" : "back from DAC mode");
	if (!on) {
		// Entering DAC mode took the controller from ADB's gadget as well. With
		// ADB on it goes back to that one (adb_keep_controller(), at the next
		// poll): binding the card reader first would only be undone a second
		// later.
		if (adb_switched_on() && adb_is_running()) {
			fprintf(stderr, "usb: ADB is on; its gadget gets the controller back\n");
		} else {
			ensure_storage_gadget();
		}
	}
	suppress_popups = false;
}

bool usb_dac_mode(void) { return dac_owns_usb; }

// ---------------------------------------------------------------------------
// The Type-C role
//
// This is a dual-role port: the board carries a FUSB302B on the CC lines
// (module_driver/fusb302b.sh loads it with otg_id_gpio=PB24) and an MP2731 that
// can push 5 V back out, so the socket can be either end of a cable. Left to
// itself the controller advertises both and negotiates, which against a PC is
// no contest -- a PC only ever sources -- and this device lands as the
// peripheral every time.
//
// Against a phone it is a contest, and one this device tends to win. A phone is
// dual-role too and Android implements Try.SNK, meaning it would rather be the
// sink; so the two agree that the DAP is the host, the boost turns on, the DAP
// charges the phone, and the card is never offered to anything. From the user's
// side: "nothing happens". The stock player has exactly one function for this
// (set_usb_working_mode at 0x481f20 in hiby_player): it writes "sink" or "dual"
// to /sys/class/typec/port0/port_type and re-applies the saved choice at every
// boot, which is the "USB working mode: Device / Host" item in its menu.
//
// The host half is in use as well: USB-C headphones and USB DACs live on it
// (usbaudio.c). So the role is not pinned -- it is arbitrated, in
// arbitrate_port() over there, and the rule is that the port is dual while it
// is empty and becomes sink again a few seconds after something attaches that
// does not turn into a sound card. A phone is exactly that case; a port pinned
// dual for good could never connect to one.
//
// This function is the sink half of that rule, kept here because it also runs
// before the gadget is bound: it is the role that decides whether the
// controller has a gadget side at all.
//
// The R1 has none of this: no FUSB302B and no Type-C class, but a TCS1421 set
// through its own platform attribute. Its role is looked after entirely by the
// arbitration in usbaudio.c, and this function does nothing there.
// ---------------------------------------------------------------------------
static void typec_force_sink(void) {
	static bool announced;

	// Not while a DAC is being driven. This one line is the whole of the "the
	// sound stops a few seconds after the screen comes back on" bug: waking the
	// panel makes the loop below rebuild the storage gadget, rebuilding the
	// gadget writes "sink" here, and writing "sink" ends the host session the
	// headphones are living in. The port belongs to usbaudio.c while there is
	// something on it.
	if (usbaudio_active()) {
		return;
	}

	for (int port = 0; port < 4; port++) {
		char path[64];
		snprintf(path, sizeof(path), "/sys/class/typec/port%d/port_type", port);
		if (access(path, W_OK) != 0) {
			continue;
		}

		FILE *f = fopen(path, "w");
		if (!f) {
			continue;
		}
		bool ok = fputs("sink", f) >= 0;
		if (fclose(f) != 0) {
			ok = false;
		}
		if (!announced) {
			announced = true;
			fprintf(stderr, "usb: %s <- sink (%s)\n", path, ok ? "ok" : strerror(errno));
		}
		return;
	}

	if (!announced) {
		announced = true;
		fprintf(stderr, "usb: no writable /sys/class/typec/portN/port_type; leaving the role alone\n");
	}
}

// Builds the storage gadget -- descriptors with this device's real name, a
// mass-storage function with no medium in it -- and binds it to the USB
// controller. Idempotent: every step checks before it acts, so this both
// creates the gadget at startup and rebuilds it after ADB tore the configfs
// down.
//
// It is bound with an empty medium rather than only while exporting because a
// bound gadget is what lets the controller's state tell a PC from a wall
// charger: a PC enumerates the medium-less card reader and the UDC state reads
// CONFIGURED, a charger only supplies power and never does. The stock player
// makes the same distinction the same way, with its UAC gadget. Exporting is
// then just writing the block device into lun.0/file (the medium clicks in);
// un-exporting writes it empty again. The gadget itself never unbinds.
static void ensure_storage_gadget(void) {
	if (dac_owns_usb || usbaudio_active()) {
		return;
	}

	// The role first, every time: the gadget below is only worth binding to a
	// controller that is going to be the peripheral end of the cable.
	typec_force_sink();

	char inquiry[32];
	usb_inquiry_string(inquiry, sizeof(inquiry));
	const char *product = usb_product();

	char cmd[2048];
	// The descriptors are written every time the gadget is about to be bound,
	// not only when it is created. DAC mode builds its audio function in this
	// same gadget (usbdac.c) and leaves its own class, configuration name and
	// power draw behind; a card reader bound over those looks to the host like
	// an audio device with a disk in it, and a phone gives up on it.
	snprintf(cmd, sizeof(cmd),
			 "[ -d /sys/kernel/config/usb_gadget ] || mount -t configfs none /sys/kernel/config; "
			 "mkdir -p %s/strings/0x409 %s/configs/c.1/strings/0x409 && cd %s && "
			 "if ! grep -q '[a-zA-Z0-9]' UDC; then "
			 "echo 0x00 > bDeviceClass; "
			 "echo 0x00 > bDeviceSubClass; "
			 "echo 0x00 > bDeviceProtocol; "
			 "echo 0x0200 > bcdUSB; "
			 "echo 0x0100 > bcdDevice; "
			 "echo %s > idVendor; "
			 "echo %s > idProduct; "
			 "echo '%s' > strings/0x409/manufacturer; "
			 "echo '%s' > strings/0x409/product; "
			 "echo '%s %s' > strings/0x409/serialnumber; "
			 "echo 500 > configs/c.1/MaxPower; "
			 "echo 0x80 > configs/c.1/bmAttributes; "
			 "echo storage > configs/c.1/strings/0x409/configuration; "
			 "for l in configs/c.1/*; do "
			 "[ -L \"$l\" ] && [ \"$l\" != configs/c.1/mass_storage.0 ] && rm -f \"$l\"; "
			 "done; "
			 "fi; "
			 "if [ ! -d functions/mass_storage.0 ]; then "
			 "mkdir functions/mass_storage.0; "
			 "echo 0 > functions/mass_storage.0/lun.0/ro; "
			 "echo 1 > functions/mass_storage.0/lun.0/removable; "
			 "echo 1 > functions/mass_storage.0/lun.0/nofua; "
			 "echo 0 > functions/mass_storage.0/lun.0/cdrom; "
			 "echo '' > functions/mass_storage.0/lun.0/file; "
			 "echo '%s' > functions/mass_storage.0/lun.0/inquiry_string 2>/dev/null; "
			 "fi; "
			 "[ -e configs/c.1/mass_storage.0 ] || ln -s functions/mass_storage.0 configs/c.1; "
			 "grep -q '[a-zA-Z0-9]' UDC || echo $(ls /sys/class/udc/ | head -n1) > UDC",
			 STORAGE_GADGET, STORAGE_GADGET, STORAGE_GADGET, USB_ID_VENDOR, USB_ID_PRODUCT, USB_MANUFACTURER, product, USB_MANUFACTURER, product, inquiry);
	int rc = system(cmd);
	(void)rc;
}

// Makes the host enumerate the gadget again, without rebuilding it: the same
// soft-disconnect toggle DAC mode uses (usbdac.c). Silently does nothing on a
// controller with no such node, where the medium simply appears the way it
// always has.
static void storage_reenumerate(void) {
	static const char *const PATHS[] = {
		"/sys/class/usb_gadget/android0/soft_disconnect",
		STORAGE_GADGET "/soft_disconnect",
		NULL,
	};

	for (int i = 0; PATHS[i]; i++) {
		if (access(PATHS[i], W_OK) != 0) {
			continue;
		}
		char cmd[256];
		snprintf(cmd, sizeof(cmd), "echo 1 > %s", PATHS[i]);
		int rc = system(cmd);
		(void)rc;
		usleep(300 * 1000);
		snprintf(cmd, sizeof(cmd), "echo 0 > %s", PATHS[i]);
		rc = system(cmd);
		(void)rc;
		fprintf(stderr, "usb: re-enumerated with the medium in, via %s\n", PATHS[i]);
		return;
	}
}

// True when a HOST has actually enumerated the bound gadget -- the check that
// tells a PC apart from a wall charger. VBUS alone cannot: a charger raises
// it too, and exporting on VBUS meant every plug-in-to-charge stopped the
// music and unmounted the card mid-listen.
// The name of the one USB device controller, or false when there is none.
static bool first_udc(char *out, size_t out_size) {
	DIR *dir = opendir("/sys/class/udc");
	if (!dir) {
		return false;
	}
	bool found = false;
	struct dirent *de;
	while (!found && (de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.') {
			continue;
		}
		snprintf(out, out_size, "%s", de->d_name);
		found = true;
	}
	closedir(dir);
	return found;
}

static char *read_udc_state(char *name, size_t name_size) {
	snprintf(name, name_size, "%s", "13500000.otg_new");
	char *content = read_file_content("/sys/class/udc/13500000.otg_new/state");
	if (!content) {
		// Firmware revision with another controller name: take the first one.
		//
		// By hand, not with popen. This runs once a second for as long as the
		// screen is on and there is no cable, and popen forks the whole player
		// to run a shell -- fifty-odd megabytes of page tables on a device with
		// fifty-six, once a second, next to a thread that has to hand the card
		// a period of audio on time. A directory and a small read cost nothing.
		if (!first_udc(name, name_size)) {
			return NULL;
		}
		char path[NAME_MAX + 32];
		snprintf(path, sizeof(path), "/sys/class/udc/%s/state", name);
		content = read_file_content(path);
		if (!content) {
			return NULL;
		}
	}
	content[strcspn(content, "\r\n")] = '\0';
	return content;
}

static bool udc_configured(void) {
	char name[NAME_MAX + 1];
	char *content = read_udc_state(name, sizeof(name));
	if (!content) {
		return false;
	}
	bool configured = strcasecmp(content, "configured") == 0;
	bool suspended = strcasecmp(content, "suspended") == 0;
	free(content);
	if (configured || !suspended) {
		return configured;
	}

	// A suspended bus alone is not proof of a host. Configfs retains the
	// selected configuration through suspend, but clears it on reset or
	// disconnect. Accept it only for our gadget on this same controller.
	content = read_file_content(STORAGE_GADGET "/UDC");
	if (!content) {
		return false;
	}
	content[strcspn(content, "\r\n")] = '\0';
	bool our_controller = strcmp(content, name) == 0;
	free(content);
	if (!our_controller) {
		return false;
	}
	content = read_file_content("/sys/class/usb_gadget/android0/state");
	if (!content) {
		return false;
	}
	content[strcspn(content, "\r\n")] = '\0';
	configured = strcasecmp(content, "configured") == 0;
	free(content);
	return configured;
}

static bool enumeration_started(void) {
	char name[NAME_MAX + 1];
	char *content = read_udc_state(name, sizeof(name));
	bool started = content && (strcasecmp(content, "default") == 0 || strcasecmp(content, "addressed") == 0);
	free(content);
	if (!started) {
		content = read_file_content("/sys/class/usb_gadget/android0/state");
		if (content) {
			content[strcspn(content, "\r\n")] = '\0';
			started = strcasecmp(content, "connected") == 0;
			free(content);
		}
	}
	return started;
}

// Wake on enumeration instead of sampling past its short configured window.
// Events never authorize export: the watcher rereads the current USB state and
// all ownership guards. A stale event after disconnect therefore does nothing.
static bool host_enumeration_event(const char *message, size_t size) {
	const char expected[] = "USB_STATE=CONFIGURED";
	const char connected[] = "USB_STATE=CONNECTED";
	while (size) {
		const char *end = memchr(message, '\0', size);
		if (!end) {
			return false;
		}
		size_t length = (size_t)(end - message);
		if ((length == sizeof(expected) - 1 && memcmp(message, expected, length) == 0) || (length == sizeof(connected) - 1 && memcmp(message, connected, length) == 0)) {
			return true;
		}
		message = end + 1;
		size -= length + 1;
	}
	return false;
}

static int host_event_socket(void) {
	int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
	if (fd < 0) {
		return -1;
	}
	// The SD listener reserves getpid(). Auto-binding could take that port
	// first, so use a separate process-specific port. Failure keeps polling.
	struct sockaddr_nl address = {.nl_family = AF_NETLINK, .nl_pid = (uint32_t)getpid() | UINT32_C(0x80000000), .nl_groups = 1};
	if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		close(fd);
		return -1;
	}
	int bytes = 16384;
	(void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
	return fd;
}

static int64_t host_event_ms(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void host_event_wait(int *fd, unsigned seconds) {
	if (*fd < 0) {
		sleep(seconds);
		return;
	}
	int64_t deadline = host_event_ms() + (int64_t)seconds * 1000;
	for (;;) {
		int64_t remaining = deadline - host_event_ms();
		if (remaining <= 0) {
			return;
		}
		struct pollfd ready = {.fd = *fd, .events = POLLIN};
		int rc = poll(&ready, 1, (int)remaining);
		if (rc < 0 && errno == EINTR) {
			continue;
		}
		if (!rc) {
			return;
		}
		if (rc < 0 || (ready.revents & (POLLERR | POLLHUP | POLLNVAL))) {
			close(*fd);
			*fd = -1; // Retain ordinary polling if event delivery is unavailable.
			return;
		}
		for (int i = 0; i < 32; i++) {
			char message[4096];
			struct sockaddr_nl sender = {0};
			socklen_t length = sizeof(sender);
			ssize_t size = recvfrom(*fd, message, sizeof(message), MSG_DONTWAIT, (struct sockaddr *)&sender, &length);
			if (size <= 0) {
				break;
			}
			if (length >= sizeof(sender) && sender.nl_family == AF_NETLINK && sender.nl_pid == 0 && host_enumeration_event(message, (size_t)size)) {
				return;
			}
		}
	}
}

// Takes the storage gadget off the USB controller. Used both to hand the
// controller to ADB and to stop paying for the PHY while the device sits in
// standby with no cable.
// Binds the gadget to the controller, or lets go of it. Two writes to one file:
// the controller's name to take it, an empty line to give it back.
//
// Written straight, not through a shell, and for the same reason as first_udc()
// above: this pair runs on the standby and wake path, which is exactly when
// music is playing with the screen dark. A fork here is heard.
static bool gadget_write_udc(const char *text) {
	FILE *f = fopen(STORAGE_GADGET "/UDC", "w");
	if (!f) {
		return false;
	}
	bool ok = fprintf(f, "%s\n", text) >= 0;
	if (fclose(f) != 0) {
		ok = false; // the driver refusing the bind surfaces here, at close
	}
	return ok;
}

static void gadget_unbind(void) { gadget_write_udc(""); }

// Puts the gadget back on the controller. False when there is no gadget to put
// back -- it has never been built, or something removed it -- and the caller
// falls back to building one.
static bool gadget_rebind(void) {
	char udc[NAME_MAX + 1];
	if (access(STORAGE_GADGET "/UDC", W_OK) != 0 || !first_udc(udc, sizeof(udc))) {
		return false;
	}
	return gadget_write_udc(udc);
}

// Called right before ADB's init script starts: two gadgets cannot bind at
// once.
//
// A card handed to the host comes back first. With ADB on the card stays with
// the player (see the watcher), and unbinding under an exported medium left it
// unmounted here and gone from the host, until the cable was pulled.
void usb_gadget_yield_to_adb(void) {
	take_card_back("ADB is starting");
	owner_changed();
	gadget_unbind();
}

// Called after ADB shuts down. Its stop script also unmounts the whole
// configfs, so the gadget is rebuilt from nothing.
//
// A card that was riding on ADB's gadget comes back first; the watcher then
// hands it to the host again through the card reader, if a host is there.
void usb_gadget_reclaim_from_adb(void) {
	take_card_back("ADB stopped");
	// The composite restore binds ADB's gadget again, and with adbd gone that
	// gadget has nothing left to carry but still holds the controller.
	if (!adb_is_running()) {
		FILE *f = fopen(ADB_GADGET "/UDC", "w");
		if (f) {
			fputs("\n", f);
			fclose(f);
		}
	}
	owner_changed();
	ensure_storage_gadget();
}

// ADB switched on and adbd up, and yet its gadget is not on the controller:
// the card reader's is, or nothing is. Hands the controller over.
//
// It happens at boot. With the switch left on, adbd is started first, by the
// firmware's script, and it binds its gadget on its own a moment later (on
// the R1 adbd runs /sbin/usb_adb_enable.sh once its endpoints are open). The
// card reader's gadget is built during that moment, takes the one controller,
// and adbd's bind fails for good: a PC then finds an empty card reader and no
// ADB, until the switch is turned off and on again.
//
// Checked on every poll, and cheap when all is well: one read of the ADB
// gadget's UDC file. adbd is looked for in /proc only when that is empty, and
// nothing is written unless the gadget is ready to bind (its functionfs is
// mounted, which adbd needs before it can write its descriptors).
static void adb_keep_controller(void) {
	if (!adb_switched_on() || adb_gadget_bound() || access(ADB_GADGET "/functions/ffs.adb", F_OK) != 0) {
		return;
	}
	if (!adb_is_running()) {
		return;
	}

	char udc[NAME_MAX + 1];
	if (!first_udc(udc, sizeof(udc))) {
		return;
	}
	gadget_unbind(); // the card reader lets go; failing when it holds nothing is fine

	bool ok = false;
	FILE *f = fopen(ADB_GADGET "/UDC", "w");
	if (f) {
		ok = fprintf(f, "%s\n", udc) >= 0;
		if (fclose(f) != 0) {
			ok = false;
		}
	}

	// Once per outcome: adbd not yet ready is retried every poll.
	static int said = -1;
	if (said != (int)ok) {
		said = ok;
		fprintf(stderr, "usb: ADB is on but its gadget was not bound; %s\n", ok ? "handed it the controller" : "adbd is not ready yet, retrying");
	}
}

// ---------------------------------------------------------------------------
// Around a system suspend
//
// The storage gadget is deliberately kept bound to the USB controller at all
// times (see above: a bound gadget is what lets the controller tell a PC from
// a wall charger). That is fine while the board is awake and fatal when it
// tries to sleep: suspending with a live gadget on the controller makes the
// device answer with a reset instead of a resume, and the player starts from
// scratch.
//
// So the controller is handed nothing at all before the SoC goes down, and the
// gadget is rebuilt on the way back. The stock player does not need this: it
// only builds a gadget while a host is actually attached.
// ---------------------------------------------------------------------------

// The cable, as the USB subsystem itself sees it -- not as the battery poll's
// five-second-old "charging" reading sees it. The suspend rule needs the
// truthful answer: a PC that enumerates the card must never have the gadget
// pulled out from under it.
bool usb_vbus_present(void) { return vbus_present(); }

void usb_suspend_prepare(void) {
	if (!sd_device[0]) {
		return; // USB mode was never started (host build, or no card)
	}
	// Every gadget, not just ours: with ADB on it is adbd's gadget that holds
	// the controller.
	int rc = system("for u in /sys/kernel/config/usb_gadget/*/UDC; do echo '' > $u 2>/dev/null; done");
	(void)rc;
	fprintf(stderr, "usb: gadgets unbound for suspend\n");
}

void usb_resume_restore(void) {
	if (!sd_device[0]) {
		return;
	}
	ensure_storage_gadget();
	fprintf(stderr, "usb: storage gadget rebound after resume\n");
}

static void composite_restore(void) {
	char cmd[1024];
	snprintf(cmd, sizeof(cmd),
			 "cd %s && "
			 "echo '' > UDC; "
			 "echo '' > functions/mass_storage.0/lun.0/file; "
			 "rm -f configs/c.1/mass_storage.0; "
			 "rmdir functions/mass_storage.0; "
			 "echo $(ls /sys/class/udc/ | head -n1) > UDC",
			 ADB_GADGET);
	int rc = system(cmd);
	fprintf(stderr, "usb: composite restore exit code %d\n", rc);
}

// Whole disks and their partitions overlap even when their /dev paths differ.
static bool block_path(const char *device, char *out) {
	const char *name = strrchr(device, '/');
	char path[PATH_MAX];
	if (!name || snprintf(path, sizeof(path), "/sys/class/block/%s", name + 1) >= (int)sizeof(path)) {
		return false;
	}
	return realpath(path, out) != NULL;
}

static bool block_overlap(const char *a, const char *b) {
	size_t an = strlen(a), bn = strlen(b);
	return (an <= bn && strncmp(a, b, an) == 0 && (b[an] == '/' || b[an] == '\0')) || (bn < an && strncmp(a, b, bn) == 0 && a[bn] == '/');
}

// True if the mount point or raw device is still mounted, or inspection fails.
// Only a known absence is sufficient evidence for raw-device export.
static bool mount_still_up(void) {
	// realpath() takes a buffer of PATH_MAX: given less it may write past it.
	char wanted[PATH_MAX];
	char wanted_device[PATH_MAX], wanted_block[PATH_MAX];
	if (!realpath(sd_mount, wanted) || !realpath(sd_device, wanted_device) || !block_path(wanted_device, wanted_block)) {
		return true; // unknown is not proof that the raw device is free
	}

	FILE *f = fopen("/proc/mounts", "r");
	if (!f) {
		return true;
	}

	char device[256], point[256];
	bool found = false;
	int fields;
	while ((fields = fscanf(f, "%255s %255s %*[^\n]", device, point)) == 2) {
		char mounted_device[PATH_MAX], mounted_block[PATH_MAX];
		bool same_device = strcmp(device, sd_device) == 0 || (realpath(device, mounted_device) && (strcmp(mounted_device, wanted_device) == 0 || (block_path(mounted_device, mounted_block) && block_overlap(wanted_block, mounted_block))));
		if (strcmp(point, wanted) == 0 || same_device) {
			found = true;
			break;
		}
	}

	if (ferror(f) || (!found && fields != EOF)) {
		found = true;
	}
	fclose(f);
	return found;
}

// True when the host has ejected the medium: on a removable lun the "safely
// remove" on the PC closes the gadget's backing file, so lun.0/file reads
// empty. That is the signal to take the card back -- otherwise the player
// sits saying "no card" until the cable is physically pulled.
static const char *medium_path(void) { return export_mode == EXPORT_ADB_COMPOSITE ? ADB_GADGET "/functions/mass_storage.0/lun.0/file" : STORAGE_GADGET "/functions/mass_storage.0/lun.0/file"; }

// -1 cannot inspect, 0 released, 1 still owned by the gadget.
static int medium_state_at(const char *name, bool missing_is_released) {
	FILE *f = fopen(name, "r");
	if (!f) {
		return missing_is_released && errno == ENOENT ? 0 : -1;
	}
	char path[PATH_MAX];
	bool read = fgets(path, sizeof(path), f) != NULL;
	bool failed = ferror(f);
	fclose(f);
	if (failed) {
		return -1;
	}
	if (!read) {
		return 0;
	}
	path[strcspn(path, "\r\n")] = '\0';
	return path[0] ? 1 : 0;
}

static int medium_state(void) {
	return medium_state_at(medium_path(), true); // a removed function has no backing file
}

static bool medium_write(const char *device) {
	FILE *f = fopen(medium_path(), "w");
	if (!f) {
		return false;
	}
	bool ok = fprintf(f, "%s\n", device) >= 0;
	return fclose(f) == 0 && ok;
}

static bool host_ejected(void) { return medium_state() == 0; }

static void storage_restore(void);

// Hands the card to the host. Ordering is what the stock player does: stop
// touching the filesystem, unmount, then export the raw device.
static void storage_export(void) {
	if (storage_active || dac_owns_usb || adb_is_running() || adb_gadget_bound()) {
		return;
	}
	fprintf(stderr, "usb: host attached, exporting %s\n", sd_device);

	// Said first, not last.
	//
	// The card supervisor stands down while this flag is up (see
	// storage_recheck_card): a card handed to a PC on purpose must not be taken
	// back from under it. Everything below -- stopping playback, closing the
	// databases, up to three unmount attempts a second apart, the
	// re-enumeration -- takes longer than the supervisor's four-second poll, and
	// all the supervisor needs to see is the moment after the unmount: no mount,
	// nobody claiming the card, so it mounts the card again. From then on the PC
	// and the player write to the same medium, and the player reads a stale
	// snapshot of the filesystem.
	storage_active = true;

	if (!storage_release_for_usb()) {
		storage_active = false;
		return;
	}

	// Only an ordinary unmount releases every local reference to the volume.
	// Forced/lazy detaches can leave writers alive on the filesystem; the stock
	// daemon's private implementation is not part of this handoff contract.
	for (int attempt = 0; attempt < 4 && mount_still_up() && !dac_owns_usb && !adb_is_running(); attempt++) {
		if (attempt) {
			sleep(1);
		}
		if (umount(sd_mount) != 0) {
			fprintf(stderr, "usb: ordinary unmount refused: %s\n", strerror(errno));
		}
	}
	if (dac_owns_usb || adb_is_running() || adb_gadget_bound() || mount_still_up()) {
		fprintf(stderr, "usb: export refused: card not released or USB owner changed\n");
		storage_restore();
		return;
	}

	export_mode = EXPORT_OWN_GADGET;
	if (!medium_write(sd_device) || medium_state() != 1) {
		fprintf(stderr, "usb: inserting the medium failed; reclaiming it\n");
		storage_restore();
		return;
	}
	storage_reenumerate();

	// What the controller thinks now, for the log: "CONFIGURED" means the PC
	// accepted the device.
	int lrc = system("for u in /sys/class/udc/*/state; do echo \"usb: udc $u: $(cat $u)\" >&2; done");
	(void)lrc;

	if (!suppress_popups) {
		gui_post(popup_connected_cb, NULL);
	}
}

// Takes the card back from the host and puts everything the export tore down
// back in place.
static void storage_restore(void) {
	fprintf(stderr, "usb: host detached, reclaiming %s\n", sd_device);

	if (export_mode != EXPORT_NONE) {
		bool released = medium_write("");
		if (medium_state() != 0) {
			fprintf(stderr, "usb: backing file not confirmed released (write %s); card stays offline\n", released ? "ok" : "failed");
			return;
		}
		if (export_mode == EXPORT_ADB_COMPOSITE) {
			composite_restore();
		}
		export_mode = EXPORT_NONE;
	}

	// Mount it back, and check rather than hope: a remount that fails silently
	// leaves the player with no card until a reboot. Try the daemon, then the
	// syscall, then busybox, until the mount point actually reads.
	for (int attempt = 0; attempt < 6 && !mount_still_up(); attempt++) {
		if (attempt) {
			sleep(1);
		}
		if (sysserver_available() && sysserver_mount(sd_device, sd_mount) == 0) {
			continue;
		}
		// The same order as at startup (mount_sd_directly in system.c): NTFS
		// last, and ntfs-3g before the kernel's ntfs module, which mounts
		// read-only and would leave the card browsable and nothing else after
		// every USB session. The types are named rather than guessed: busybox
		// guessing tries ntfs like any other type.
		char remount[1600];
		snprintf(remount, sizeof(remount),
				 "mount -t exfat,vfat,ext4,ext3,ext2,ntfs3 %s %s 2>/dev/null || "
				 "ntfs-3g %s %s -o big_writes,noatime 2>/dev/null || mount -t ntfs %s %s",
				 sd_device, sd_mount, sd_device, sd_mount, sd_device, sd_mount);
		int mrc = system(remount);
		(void)mrc;
	}

	// The storage layer checks the mount before reopening anything. A failed
	// remount must never create databases or logs on the internal mount directory.
	storage_active = false;
	storage_resume_after_usb();
	fprintf(stderr, "usb: local card services on %s: %s\n", sd_mount, storage_card_attached() ? "ready" : "offline");

	if (!suppress_popups) {
		gui_post(popup_disconnected_cb, NULL);
	}
}

// Takes the card back from the host if it was handed over, from any thread.
static void take_card_back(const char *why) {
	pthread_mutex_lock(&storage_lock);
	if (storage_active) {
		fprintf(stderr, "usb: %s; taking the card back from the host\n", why);
		storage_restore();
	}
	pthread_mutex_unlock(&storage_lock);
}

// The watcher's own export and restore, under the same lock.
static void locked_export(void) {
	pthread_mutex_lock(&storage_lock);
	storage_export();
	pthread_mutex_unlock(&storage_lock);
}

static void locked_restore(void) {
	pthread_mutex_lock(&storage_lock);
	if (storage_active) {
		storage_restore();
	}
	pthread_mutex_unlock(&storage_lock);
}

static bool reader_retry_owned(unsigned serial) { return !storage_active && !dac_owns_usb && !usbaudio_active() && !adb_switched_on() && !adb_is_running() && !adb_gadget_bound() && serial == __atomic_load_n(&owner_serial, __ATOMIC_SEQ_CST) && medium_state_at(STORAGE_GADGET "/functions/mass_storage.0/lun.0/file", false) == 0; }

// Recover a stalled EP0 exchange once, without handing the card to anybody.
// This does not release/remount storage or insert a medium. The watcher must
// still observe a fresh configuration before its ordinary guarded export.
static void retry_empty_reader(unsigned serial, bool *spent, bool *left_unbound) {
	pthread_mutex_lock(&storage_lock);
	if (*spent || !reader_retry_owned(serial) || !vbus_present() || udc_configured()) {
		pthread_mutex_unlock(&storage_lock);
		return;
	}
	char *controller = read_file_content(STORAGE_GADGET "/UDC");
	if (!controller) {
		pthread_mutex_unlock(&storage_lock);
		return;
	}
	controller[strcspn(controller, "\r\n")] = '\0';
	if (!controller[0] || strlen(controller) > NAME_MAX) {
		free(controller);
		pthread_mutex_unlock(&storage_lock);
		return;
	}
	*spent = true; // Kernel write failure must not create an automatic retry loop.
	fprintf(stderr, "usb: enumeration stalled; restarting the empty reader once\n");
	bool unbound = gadget_write_udc("");
	char *current = read_file_content(STORAGE_GADGET "/UDC");
	bool verified_unbound = false;
	if (current) {
		current[strcspn(current, "\r\n")] = '\0';
		verified_unbound = !current[0];
		*left_unbound = verified_unbound;
	} else {
		*left_unbound = true; // Unknown binding stays out of the automatic bind path.
	}
	free(current);
	if (unbound && verified_unbound) {
		usleep(100000);
		if (reader_retry_owned(serial) && vbus_present()) {
			*left_unbound = !gadget_write_udc(controller);
		}
	}
	fprintf(stderr, "usb: empty reader %s; card remains local\n", *left_unbound ? "left unbound" : "bound");
	free(controller);
	pthread_mutex_unlock(&storage_lock);
}

static void *usb_thread(void *arg) {
	int event_fd = (int)(intptr_t)arg;
	thread_be_background("usb watcher");

	bool exported = false;
	// Set after the PC ejects the medium: the card is back but the cable is
	// still in, and re-exporting now would undo the eject the user just asked
	// for. Cleared when VBUS finally drops.
	bool await_unplug = false;

	// Said once, not once a second.
	bool said_adb = false;

	// The gadget is unbound from the controller while the device sits in
	// standby with no cable (see below).
	bool parked = false;
	int64_t enumeration_since = 0;
	bool enumeration_retried = false, recovery_unbound = false;

	unsigned seen_owner = __atomic_load_n(&owner_serial, __ATOMIC_SEQ_CST);

	for (;;) {
		// DAC mode owns the controller: stand completely down. Not just "do
		// not rebuild" -- do not look, do not export, do not park. Everything
		// this loop does ends in a write to the gadget, and every one of those
		// writes would land on the audio gadget instead.
		if (dac_owns_usb) {
			sleep(1);
			continue;
		}

		// And the same for a DAC or a pair of headphones on the port: the
		// controller is the host's then, and everything below ends in a write
		// to a gadget that has no business existing while it is.
		if (usbaudio_active()) {
			sleep(1);
			continue;
		}

		// Slower while the panel is dark. A cable being plugged in is
		// something the user just did, so a second's resolution is worth
		// having with the screen on; in standby it is one more thread waking
		// the CPU every second for the whole night.
		host_event_wait(&event_fd, power_screen_is_on() ? POLL_SECONDS : POLL_SECONDS_STANDBY);
		if (dac_owns_usb || usbaudio_active()) {
			continue;
		}

		// The controller changed hands since the last look: whatever was
		// known about the cable belongs to the gadget before. And a card
		// taken back by DAC mode or ADB is not exported any more, whatever
		// this loop last did with it: believing otherwise read the emptied
		// medium as an eject by the host, and the card was not offered again
		// until the cable was pulled.
		unsigned owner = __atomic_load_n(&owner_serial, __ATOMIC_SEQ_CST);
		if (owner != seen_owner) {
			seen_owner = owner;
			await_unplug = false;
			said_adb = false;
			enumeration_since = 0;
			enumeration_retried = false;
			parked |= recovery_unbound;
			recovery_unbound = false;
		}
		if (exported && !storage_active) {
			exported = false;
		}

		adb_keep_controller();

		// A host that has enumerated this device is a cable, whatever the
		// charger says. On a phone the two can disagree: the power role and the
		// data role are separate on Type-C, so a phone can act as the host
		// while the charger node reads nothing at all -- and treating that as
		// "no cable" is how the gadget ended up being unbound from under a host
		// that was talking to it.
		bool vbus = vbus_present() || udc_configured();

		if (!vbus) {
			if (exported) {
				locked_restore();
				exported = storage_active;
			}
			await_unplug = false;
			enumeration_since = 0;
			enumeration_retried = false;
			parked |= recovery_unbound;
			recovery_unbound = false;

			// No cable, panel dark: take the gadget off the controller. A
			// bound gadget keeps the USB PHY and the OTG block clocked --
			// /sys/kernel/debug/clk/clk_summary shows gate_usbphy and
			// gate_otg alive a minute into standby with nothing plugged in --
			// and that is current spent waiting for a cable that is not there.
			// The stock player only ever builds a gadget once a host has
			// turned up; this is the same idea, but only in standby, so
			// nothing about plugging a cable into a device in use changes.
			//
			// Never while ADB is running: the controller is ADB's then, and
			// reclaiming it would cut the connection.
			//
			// And not while audio is open either. Both of these are
			// housekeeping for a cable that is not attached: nothing is
			// waiting for the gadget, so there is no reason to touch the
			// controller in the middle of a track -- a rebind runs the whole
			// gadget script, shell and all, and costs a hole in the sound.
			// Both happen at the next poll after playback stops, and a cable
			// turning up is handled below whatever this decided.
			if (audio_device_is_open()) {
				continue;
			}

			if (!parked && !power_screen_is_on() && !adb_is_running()) {
				gadget_unbind();
				parked = true;
				fprintf(stderr, "usb: no cable and the screen is dark; gadget unbound\n");
			} else if (parked && power_screen_is_on()) {
				// Unless ADB has been switched on meanwhile: the controller is
				// its gadget's now, and adb_keep_controller() sees to it.
				if (!adb_switched_on() && !gadget_rebind()) {
					ensure_storage_gadget();
				}
				parked = false;
				fprintf(stderr, "usb: awake again; gadget rebound\n");
			}
			continue;
		}
		if (recovery_unbound) {
			continue; // Bind failure waits for a physical disconnect or a new owner.
		}

		// A cable turned up while the gadget was parked: put it back before
		// anything else, or the host will never see this device.
		if (parked) {
			// Here there is a host waiting, so this happens whatever playback
			// is doing -- but by the cheap route first: the gadget was only
			// unbound, not taken apart, so putting it back is one write.
			typec_force_sink();
			if (!adb_switched_on() && !gadget_rebind()) {
				ensure_storage_gadget();
			}
			parked = false;
			fprintf(stderr, "usb: cable in; gadget rebound\n");
			continue; // give the controller a poll to enumerate
		}

		if (exported) {
			if (host_ejected()) {
				fprintf(stderr, "usb: host ejected the medium, reclaiming the card\n");
				locked_restore();
				exported = storage_active;
				await_unplug = !storage_active;
			}
			continue;
		}

		if (await_unplug) {
			continue;
		}

		// ADB is on: the cable is here to carry a shell, not to be a card
		// reader. Exporting the card now unmounts it from under that shell --
		// which is why /mnt/sd_0 came back "Read-only file system" over ADB --
		// and the stock firmware does not do it either. Leave the card where
		// it is.
		if (adb_switched_on() || adb_is_running() || adb_gadget_bound()) {
			if (!said_adb) {
				fprintf(stderr, "usb: ADB is running; the card stays mounted (no mass storage)\n");
				said_adb = true;
			}
			continue;
		}
		said_adb = false;

		// VBUS alone is not a PC -- a wall charger raises it too, and taking
		// the card away (stopping the music with it) for a power brick was
		// exactly the "track would not start while charging" bug. Only a host
		// that has enumerated the gadget counts.
		if (!udc_configured()) {
			int64_t now = host_event_ms();
			if (!enumeration_since && enumeration_started()) {
				enumeration_since = now;
			}
			if (enumeration_since && now - enumeration_since >= 3000 && !enumeration_retried) {
				retry_empty_reader(seen_owner, &enumeration_retried, &recovery_unbound);
			}
			if (recovery_unbound) {
				continue;
			}
			if (!udc_configured()) {
				continue;
			}
		}
		enumeration_since = 0;

		locked_export();
		exported = storage_active;
		if (!exported) {
			await_unplug = true; // retry only after unplug, not every second
		}
	}

	return NULL;
}

void usb_start(const char *device, const char *mount_point) {
	if (!mount_point) {
		// A card that failed to mount can still be exported for host-side repair.
		mount_point = storage_sd_mount_point();
	}
	if (!device || !device[0] || !mount_point || !mount_point[0]) {
		fprintf(stderr, "usb: no card device/mount known; USB mode disabled\n");
		return; // host build, or no card: nothing to export
	}

	bool any_vbus = false;
	for (size_t i = 0; i < sizeof(VBUS_CANDIDATES) / sizeof(VBUS_CANDIDATES[0]); i++) {
		bool have = access(VBUS_CANDIDATES[i], R_OK) == 0;
		fprintf(stderr, "usb: vbus node %s: %s\n", VBUS_CANDIDATES[i], have ? "present" : "missing");
		any_vbus |= have;
	}
	if (!any_vbus) {
		fprintf(stderr, "usb: no VBUS node found; USB mode disabled\n");
		return;
	}
	if (access("/sys/kernel/config", F_OK) != 0) {
		fprintf(stderr, "usb: no configfs on this kernel; USB mode disabled\n");
		return;
	}

	// The port is a peripheral from here on, whatever is at the other end of
	// the cable. The stock player re-applies its own saved choice at every boot
	// for the same reason: the role is not remembered across one.
	typec_force_sink();

	snprintf(sd_device, sizeof(sd_device), "%s", device);
	snprintf(sd_mount, sizeof(sd_mount), "%s", mount_point);
	int event_fd = host_event_socket();
	if (event_fd < 0) {
		fprintf(stderr, "usb: enumeration events unavailable; using state polling\n");
	}

	// The empty-medium gadget goes up right away, unless ADB owns the
	// controller -- then its gadget provides the enumeration signal instead.
	// "Owns" includes the moment before adbd has bound it: started just
	// before this, it may not have got that far, and a card reader bound now
	// would keep it off the controller (see adb_keep_controller()).
	if (!adb_gadget_bound() && !(adb_switched_on() && adb_is_running())) {
		ensure_storage_gadget();
	}

	pthread_t thread;
	if (pthread_create(&thread, NULL, usb_thread, (void *)(intptr_t)event_fd) == 0) {
		pthread_detach(thread);
		fprintf(stderr, "usb: watching for a USB host (exporting %s)\n", sd_device);
	} else if (event_fd >= 0) {
		close(event_fd);
	}
}
