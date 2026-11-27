#!/bin/sh

ulimit -s 1024
export MALLOC_ARENA_MAX=1

killall    sonix_player >/dev/null 2>&1
killall -9 sonix_player >/dev/null 2>&1

if [ -f "/usr/bin/batd" ]; then
    killall    batd >/dev/null 2>&1
    killall -9 batd >/dev/null 2>&1
    #/usr/bin/batd -v -s -t5 -o /mnt/sd_0/batlog.txt &
fi

# `/usr/data/copy-from-sd0` file is a flag to enable copying sonix_player off
# of the SD card, and running that player persistantly (as long as the flag is
# enabled)
#
# if flag is set, mount SD card, check for a file called sonix_player, copy it
# out to the device storage, then launch that binary instead of the one that's
# installed
if [ -f /usr/data/copy-from-sd0 ] &&
   mount /dev/mmcblk0p1 /mnt/sd_0 &&
   [ -f /mnt/sd_0/sonix_player ] &&
   cp /mnt/sd_0/sonix_player /tmp/sonix_player &&
   umount /mnt/sd_0; then

    # start the player copied from from sd0
    chmod +x /tmp/sonix_player
    /tmp/sonix_player
else
    # ensure sd0 is not mounted before starting the player
    if mountpoint -q /mnt/sd_0; then
        umount /mnt/sd_0
    fi

    # start original player
    /usr/bin/sonix_player
fi

sleep 1
reboot
