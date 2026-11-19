#ifndef USB_H
#define USB_H

#include <stdbool.h>

// USB mass storage with an exclusive filesystem handoff.
//
// Local services close first. Only a successful ordinary unmount and a known
// absence from /proc/mounts allow the raw device to reach the host. A busy or
// uncertain card stays local; no forced/lazy unmount is used for export.
// Local services resume only after the gadget releases its backing file and
// the card has a verified mount. ADB leaves the card local.
//
// Detection polls /sys/class/power_supply/usb/online from a small background
// thread. A wall charger raises the same signal; exporting the gadget then is
// harmless (nothing enumerates it) but unmounting the card for a power brick
// would not be, so the thread requires a configured USB host. Kernel
// enumeration events wake the same watcher without increasing idle polling.
// Suspend alone is insufficient: the matching bound gadget must still report
// CONFIGURED. Events never bypass current-state or filesystem ownership checks.

// Starts the watcher thread. Pass the SD block device (e.g. /dev/mmcblk0p1)
// and the mount point it lives on; either NULL disables the whole feature
// (host build).
void usb_start(const char *sd_device, const char *sd_mount_point);

// True while USB claims the card, including handoff and an uncertain release.
// Local writers and mount recovery must stay off the card during this interval.
bool usb_storage_active(void);

// Hands the USB controller to DAC mode, and takes it back.
//
// While it is handed over, everything in usb.c stands down: no gadget is
// rebuilt, the card is not exported, and the watcher thread does not touch the
// controller at all. Without this the watcher sees a cable, rebuilds the
// mass-storage gadget over the audio one, and the computer both loses the
// audio stream and gains a microSD it should not have.
void usb_set_dac_mode(bool on);
bool usb_dac_mode(void);

// Coordination with ADB, whose init script needs the one USB controller for
// its own gadget: yield unbinds the storage gadget before adbd starts, reclaim
// rebuilds and rebinds it after adbd stops (its stop script tears the whole
// configfs down). Both take a card handed to the host back first. Called by
// adb.c around the init scripts.
void usb_gadget_yield_to_adb(void);
void usb_gadget_reclaim_from_adb(void);

// True while a cable is supplying VBUS (a PC or a charger). Read straight
// from the power-supply nodes, so it is current rather than cached.
bool usb_vbus_present(void);

// Around a system suspend. The storage gadget stays bound to the USB
// controller for as long as the player runs, and a bound gadget is something
// this SoC will not suspend past -- the board reset instead of resuming.
// prepare() hands the controller back before the SoC goes down; restore()
// rebuilds and rebinds the gadget afterwards.
void usb_suspend_prepare(void);
void usb_resume_restore(void);

// The product string: the model's name without the maker's
const char *usb_product(void);

#endif /* USB_H */
