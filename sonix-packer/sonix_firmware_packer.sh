#!/bin/bash
#
# Builds HiByOS firmware images that boot Sonix Player instead of the stock
# music player -- one per model, from one binary and one set of assets.
#
# Everything it needs sits next to it:
#
#   sonix_firmware_packer.sh   this script
#   r3proii_original.upt       the stock firmware of the R3 Pro II
#   r1_original.upt            the stock firmware of the R1
#   sonix_player               the binary to install, the same one for both
#   sonix_launch               waits for the player and reboots when it exits
#                              (optional: without it the launcher's shell does)
#   kernel/xImage-R3PII        a kernel to install instead of the stock one
#   kernel/xImage-R1           (optional, per model: without it the stock
#                              kernel stays, untouched)
#   kernel/modules.builtin-R3PII  the drivers that kernel has built in, one
#   kernel/modules.builtin-R1     name per line: their .ko and loader scripts
#                              are taken out of module_driver/ (optional,
#                              read only with the xImage beside it)
#   assets/
#       R3PII/
#       R1/
#       |
#       v
#   r3proii.upt                one result per stock firmware present
#   r1.upt
#
# A model whose stock firmware is not here is skipped with a warning, so
# somebody who owns one of the two players can still run this and get theirs.
#
# One complete tree per model, and no shared layer: too much of what goes into
# an image belongs to the machine it is going into -- the touch driver is built
# against that kernel, the boot logos are drawn for that panel, and the audio
# pieces answer to that hardware. The files that really are the same in both,
# the language files and some of the images, are cheaper to copy than a rule
# about which layer wins.
#
# What that costs is the two trees drifting apart, so before each image this
# script lists the files the OTHER model has and this one does not. Usually
# that is deliberate; the once it is not, it says so.
#
# No questions asked: run it and it does the lot.
#
#   ./sonix_firmware_packer.sh                       the images
#   ./sonix_firmware_packer.sh --strip-debug-tools   the same, without the stock
#                                                    debugging tools (see
#                                                    STOCK_DEBUG_TOOLS below)

set -euo pipefail

# --- options ---
STRIP_DEBUG_TOOLS=0
while [ $# -gt 0 ]; do
	case "$1" in
	--strip-debug-tools) STRIP_DEBUG_TOOLS=1 ;;
	-h|--help) sed -n '2,/^set -euo pipefail/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
	*) echo "unknown option: $1 (see --help)" >&2; exit 1 ;;
	esac
	shift
done
export COLUMNS=1

# --- terminal colours ---
RED='\033[1;31m'
GREEN='\033[1;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Homebrew, for macOS.
export PATH="/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH"

say()  { echo -e "$@"; }
step() { echo -e "${BLUE}==>${NC} $*"; }
warn() { echo -e "${YELLOW}!!${NC}  $*"; }
die()  { echo -e "${RED}Error:${NC} $*" >&2; exit 1; }

# ==========================================================================
# Where everything lives
# ==========================================================================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"

PLAYER_BIN="$SCRIPT_DIR/sonix_player"
LAUNCH_BIN="$SCRIPT_DIR/sonix_launch"

# Kernels to install, one per model folder name: kernel/xImage-R3PII and so on.
# Not under assets/, which is copied into the rootfs.
KERNEL_DIR="$SCRIPT_DIR/kernel"

# The size of the partitions the two images are written to
KERNEL_MAX=$((0x500000))
ROOTFS_MAX=$((0x2d00000))

# The models, one line each: the assets folder, the stock firmware to start
# from, the image to write, and the device-name the player will read out of
# system-info.json. The last one is not decoration -- it is checked against the
# file the overlay actually landed, which is what stops an R1 image being built
# with the R3 Pro II's name in it and offering that model's updates.
MODELS=(
	"R3PII|r3proii_original.upt|r3proii.upt|HiBy R3 Pro II"
	"R1|r1_original.upt|r1.upt|HiBy R1"
)

WORK_DIR="$SCRIPT_DIR/temp"
OTA_DIR="$WORK_DIR/ota_v0"
SQUASH_DIR="$SCRIPT_DIR/squashfs-root"
MERGED_SQUASHFS="$SCRIPT_DIR/rootfs_original.squashfs"

# Paths inside the rootfs.
STOCK_PLAYER="usr/bin/hiby_player"
STOCK_LAUNCHER="usr/bin/hiby_player.sh"
NEW_LAUNCHER="usr/bin/sonix_player.sh"
INIT_SCRIPT="etc/init.d/S92_03_start_music_player"
SYSTEM_INFO="usr/resource/sonix/components/system-info.json"

# The stock interface's own resources. Nothing on the device reads them once
# the player is replaced, and they are several megabytes of an image that has
# to fit in flash.
#
# Note what is NOT in this list: usr/resource/sonix, which is where the assets
# folder lands. The stock fonts and strings live one level above it and go;
# ours stay.
STOCK_UI_DIRS=(usr/resource/litegui usr/resource/layout usr/resource/str usr/resource/fonts)

# Other parts of the stock firmware nothing uses once the stock player is gone:
# checked against both models' images, with nothing left that starts, links,
# loads (dlopen) or names them. Paths in the rootfs; shell patterns are
# allowed, and a library goes with its symlinks. A path the assets bring is
# never removed here.
#
#   etc/logo1.jpeg, logo2.jpeg   unused; etc/logo.jpeg stays, the boot logo's
#                                fallback (S11jpeg_display_shell, bootlogo.c)
#   usr/share/zoneinfo           the player sets TZ from its own rules
#                                (src/system/device/tzrules.c)
#   usr/resource/unicode         the stock player's codepage tables
#   usr/resource/...             the stock player's settings and lists; the
#                                player reads bt_name and hostname, which stay,
#                                and radio.txt from the card, not from here
#   bsa_server, bt_enable_bsa.sh Broadcom's BSA Bluetooth stack; the player
#                                uses bluetoothd and bluealsa
#   usr/lib/...                  the stock player's codecs and libraries,
#                                linked by nothing that is left (libpcrecpp,
#                                PCRE's C++ wrapper, needs libstdc++ and goes
#                                with it)
#
# The debugging tools (strace, btmon, the bt-* and dbus-* tools...), obexd
# and ntpd, and the R1's AIC8800 Wi-Fi files are unused too, but are kept on
# purpose -- and so is libical, which obexd links. --strip-debug-tools takes
# out the debugging tools (STOCK_DEBUG_TOOLS, below).
STOCK_UNUSED=(
	etc/logo1.jpeg etc/logo2.jpeg
	usr/share/zoneinfo
	usr/resource/unicode
	usr/resource/hl_json usr/resource/eula usr/resource/tzlookup.xml
	usr/resource/radio.txt usr/resource/coordinate usr/resource/region
	usr/resource/ot_devices.json usr/resource/r1pro_ot_devices.json
	usr/resource/song_genre.ini usr/resource/set_functions.json
	usr/resource/midi_set_functions.json usr/resource/midi_config.json
	usr/resource/r1pro_config.json usr/resource/r1pro_bt_name
	usr/resource/province.txt usr/resource/eq.ini usr/resource/diff_layout.txt
	usr/resource/config.json usr/resource/audio_back.conf
	usr/resource/dac_out_more_than_384k_fail usr/resource/bt_more_than_192k_fail
	usr/bin/bsa_server usr/bin/bt_enable_bsa.sh
	'usr/lib/libstdc++.so*' 'usr/lib/libcurl.so*'
	usr/lib/libwma.so usr/lib/libaac.so usr/lib/libmp3.so usr/lib/libopus.so
	usr/lib/libvorbis.so usr/lib/libdsd2pcm.so usr/lib/libqrencode.so
	'usr/lib/libjson-glib-1.0.so*' 'usr/lib/libevent*.so*'
	'usr/lib/libpcrecpp.so*'
)

# The stock debugging tools, removed only with --strip-debug-tools: about
# 4.5 MB on each model. Checked against both models' images: nothing that
# stays starts or links any of them -- not the init scripts, not the scripts
# sys_server runs, not sys_server itself, not Sonix Player. A tool missing on
# one model is skipped there. Without them, debugging over adb has no strace
# and no UBI or flash maintenance tools.
#
# Kept, although they look like debugging tools, because something uses them:
#   btmon                        Sonix Player's Bluetooth log (btlog.c)
#   hciconfig, hciattach,        the bt_* scripts sys_server runs (and the
#   sdptool, bt_test, bt-media,  stock bt_init, which Sonix Player still
#   rfkill, aic_radio.sh         copes with)
#   bt-adapter, bt-device,       run by the R3 Pro II's sys_server
#   bt-agent, bluealsa-cli,
#   dbus-send
#   dbus-uuidgen                 S30dbus and Sonix Player
#   nanddump, nandwrite,         the boot logo marker (bootlogo.c) and the
#   flash_erase, mtdinfo         OTA scripts
#   ubiattach, ubidetach,        mount_ubifs.sh
#   ubimkvol
#
#   strace                       system call tracing
#   hcidump ... sbcinfo          Bluetooth tests and extras; hcitool goes
#                                with dut.sh, its only user
#   dbus-*, gdbus, gio ...       D-Bus and GLib command line tools
#   alsamixer ... arecord        ALSA command line tools
#   pcregrep ... cmd_gpio        tests
#   mtd_debug ... ubiupdatevol   flash and UBI maintenance
#   usr/lib/...                  linked only by the tools above: the ncurses
#                                form, menu and panel libraries (alsamixer),
#                                libbsd (hcitop), libpcreposix (pcregrep,
#                                pcretest), libiperf (iperf3)
STOCK_DEBUG_TOOLS=(
	usr/bin/strace usr/bin/strace-log-merge
	usr/bin/hcidump usr/bin/hcitop usr/bin/bccmd usr/bin/bluemoon
	usr/bin/ciptool usr/bin/l2ping usr/bin/l2test usr/bin/rctest
	usr/bin/gatttool usr/bin/btattach usr/bin/hex2hcd
	usr/bin/hcitool usr/bin/dut.sh
	usr/bin/bt-network usr/bin/bt-obex usr/bin/bt-connect
	usr/bin/bluealsa-aplay usr/bin/bluealsa-rfcomm usr/bin/mpris-proxy
	usr/bin/sbcdec usr/bin/sbcenc usr/bin/sbcinfo
	usr/bin/dbus-monitor usr/bin/dbus-test-tool usr/bin/dbus-binding-tool
	usr/bin/dbus-launch usr/bin/dbus-run-session usr/bin/dbus-cleanup-sockets
	usr/bin/dbus-update-activation-environment
	usr/bin/gdbus usr/bin/gio usr/bin/gio-launch-desktop
	usr/bin/gio-querymodules usr/bin/gresource usr/bin/gsettings
	usr/bin/gapplication
	usr/bin/alsamixer usr/bin/amixer usr/bin/aplay usr/bin/arecord
	usr/bin/pcregrep usr/bin/pcretest usr/bin/xmlwf usr/bin/memtester
	usr/bin/iperf3 usr/bin/tree usr/bin/getevent usr/bin/cmd_fb
	usr/bin/cmd_gpio
	usr/sbin/mtd_debug usr/sbin/nandtest usr/sbin/flashcp
	usr/sbin/flash_lock usr/sbin/flash_unlock
	usr/sbin/ubiblock usr/sbin/ubicrc32 usr/sbin/ubiformat usr/sbin/ubinfo
	usr/sbin/ubinize usr/sbin/ubirename usr/sbin/ubirmvol usr/sbin/ubirsvol
	usr/sbin/ubiupdatevol
	'usr/lib/libform.so*' 'usr/lib/libmenu.so*' 'usr/lib/libpanel.so*'
	'usr/lib/libbsd.so*' 'usr/lib/libpcreposix.so*' 'usr/lib/libiperf.so*'
)

# The working folders always go, whether the run succeeded or not.
cleanup() { rm -rf "$WORK_DIR" "$SQUASH_DIR" "$MERGED_SQUASHFS" 2>/dev/null || true; }
trap cleanup EXIT

say "${BLUE}###############################################${NC}"
say "${BLUE}###   SONIX PLAYER -- FIRMWARE PACKER       ###${NC}"
say "${BLUE}###############################################${NC}"
say ""

# ==========================================================================
# Portability: md5, file size, build stamp
# ==========================================================================
if command -v md5sum >/dev/null 2>&1; then
	get_md5() { md5sum "$1" | awk '{print $1}'; }
else
	get_md5() { md5 -q "$1"; }
fi

if stat -c%s . >/dev/null 2>&1; then
	get_size()  { stat -c%s "$1"; }
	# When the file was last written, as DDMMYYYYHHMM. GNU date takes the file
	# itself here; the BSD one takes seconds, which is why this is not one line
	# for both.
	get_stamp() { date -r "$1" +%d%m%Y%H%M; }
else
	get_size()  { stat -f%z "$1"; }
	get_stamp() { stat -f%Sm -t %d%m%Y%H%M "$1"; }
fi

# sed -i is spelled differently on the two systems, so nothing here uses it:
# the file is rewritten through a temporary and moved into place.
sed_file() {
	local expression="$1" file="$2"
	sed -E "$expression" "$file" > "$file.packer.tmp"
	mv "$file.packer.tmp" "$file"
}

# How many times a string appears in a file -- every occurrence, not the lines
# that hold one. A single line can kill both hiby_player.sh and hiby_player,
# and counting lines would report that as one.
count_in() { tr -c '[:alnum:]_./' '\n' < "$2" | grep -c -- "$1" || true; }

# Replaces every hiby_player with sonix_player, and proves it: the count before,
# every resulting line printed, and a hard stop if anything survived.
#
# One substitution covers both spellings -- a reference to `hiby_player` becomes
# `sonix_player`, and one to `hiby_player.sh` becomes `sonix_player.sh`, which
# is the name the launcher has just been given.
rename_player_in() {
	local file="$1" label="$2"
	local before
	before="$(count_in "hiby_player" "$file")"

	sed_file "s/hiby_player/sonix_player/g" "$file"

	if grep -q "hiby_player" "$file"; then
		die "$label still mentions hiby_player after the rewrite."
	fi
	say "    $label: $before occurrence(s) of hiby_player replaced"
	grep -n "sonix_player" "$file" | sed 's/^/        /'
}

# Copies FS_data, the HBC3000's FPGA configuration, out of the stock
# sa_sound_hbc3000.ko into a file: the array named by the module's symbol table,
# in its own section. perl because macOS and Linux both have it.
hbc3000_firmware() {
	perl -e '
		local $/;
		open(my $f, "<:raw", $ARGV[0]) or die "$ARGV[0]: $!\n";
		my $e = <$f>;
		substr($e, 0, 6) eq "\x7fELF\x01\x01" or die "$ARGV[0]: not a 32-bit little-endian ELF\n";
		my $shoff = unpack("V", substr($e, 0x20, 4));
		my ($shentsize, $shnum) = unpack("vv", substr($e, 0x2e, 4));
		my @sh = map { [unpack("V10", substr($e, $shoff + $_ * $shentsize, 40))] } 0 .. $shnum - 1;
		for my $s (grep { $_->[1] == 2 } @sh) {
			my $strtab = $sh[$s->[6]]->[4];
			for (my $o = 0; $o < $s->[5]; $o += 16) {
				my ($name, $value, $size, $info, $other, $shndx) =
					unpack("VVVCCv", substr($e, $s->[4] + $o, 16));
				next unless unpack("Z*", substr($e, $strtab + $name)) eq "FS_data";
				open(my $out, ">:raw", $ARGV[1]) or die "$ARGV[1]: $!\n";
				print $out substr($e, $sh[$shndx]->[4] + $value, $size);
				exit 0;
			}
		}
		die "$ARGV[0]: no FS_data\n";
	' "$1" "$2"
}

# Writes an image into the OTA folder the way the updater reads it: 512 KiB
# chunks named NAME.NNNN.<md5 of the previous chunk>, the first one named after
# the md5 of the whole image, and ota_md5_NAME.<md5 of the whole image> listing
# the md5 of every chunk in order. The image itself is consumed.
write_chunks() {
	local name="$1" image="$2"
	local whole_sum current_sum md5_file count suffix chunk f

	whole_sum="$(get_md5 "$image")"
	split -b 524288 -a 4 "$image" "$OTA_DIR/temp_chunk_"
	rm -f "$image"

	md5_file="$OTA_DIR/ota_md5_$name.$whole_sum"
	: > "$md5_file"

	count=0
	current_sum="$whole_sum"
	for f in "$OTA_DIR/temp_chunk_"*; do
		[ -e "$f" ] || continue
		suffix="$(printf "%04d" $count)"
		chunk="$OTA_DIR/$name.$suffix.$current_sum"
		mv "$f" "$chunk"
		current_sum="$(get_md5 "$chunk")"
		echo "$current_sum" >> "$md5_file"
		count=$((count + 1))
	done
	say "    $name: $count chunks"
}

# ==========================================================================
# What has to be there before anything is touched
# ==========================================================================
step "Checking what is needed"

# A string and not an array: bash 3.2, which is the one macOS ships, treats
# ${#array[@]} on an empty array as an unbound variable under `set -u`.
MISSING=""
for tool in 7z unsquashfs mksquashfs split tar; do
	command -v "$tool" >/dev/null 2>&1 || MISSING="$MISSING $tool"
done
# The ISO writer answers to more than one name depending on the system.
ISO_TOOL=""
for tool in mkisofs genisoimage xorrisofs; do
	if command -v "$tool" >/dev/null 2>&1; then
		ISO_TOOL="$tool"
		break
	fi
done
[ -n "$ISO_TOOL" ] || MISSING="$MISSING mkisofs"

if [ -n "$MISSING" ]; then
	die "missing:$MISSING
  Debian/Ubuntu:  sudo apt install p7zip-full squashfs-tools genisoimage
  macOS:          brew install p7zip squashfs cdrtools"
fi

[ -f "$PLAYER_BIN" ] || die "sonix_player is not next to this script."

ASSETS_DIR="$SCRIPT_DIR/assets"
[ -d "$ASSETS_DIR" ] || die "there is no assets folder next to this script."

# Which models can be built at all. A stock firmware that is not here is not an
# error -- it is a model this person does not own.
BUILDABLE=""
for entry in "${MODELS[@]}"; do
	IFS='|' read -r m_dir m_src m_out m_name <<< "$entry"
	if [ ! -d "$ASSETS_DIR/$m_dir" ]; then
		warn "assets/$m_dir is missing: $m_name will be skipped."
		continue
	fi
	if [ ! -f "$SCRIPT_DIR/$m_src" ]; then
		warn "$m_src is not next to this script: $m_name will be skipped."
		continue
	fi
	BUILDABLE="$BUILDABLE$entry
"
	say "    $m_name: $m_src -> $m_out"
done

[ -n "$BUILDABLE" ] || die "no stock firmware to start from. Put at least one of
  r3proii_original.upt / r1_original.upt next to this script."

BUILD_STAMP="$(get_stamp "$PLAYER_BIN")"
TODAY="$(date +%d%m%Y)"
say "    binary:  sonix_player, $(get_size "$PLAYER_BIN") bytes, built $BUILD_STAMP"
if [ "${BUILD_STAMP:0:8}" != "$TODAY" ]; then
	warn "sonix_player was not built today; build_version will still say when it was."
fi

# sonix_launch has to be a 32-bit little-endian MIPS executable: the launcher
# execs it, and a busybox shell that cannot exec it exits without starting the
# player. The first 20 bytes of the ELF header say so.
if [ -f "$LAUNCH_BIN" ]; then
	ELF_HEAD="$(od -An -tx1 -N20 "$LAUNCH_BIN" | tr -d ' \n')"
	[ "${ELF_HEAD:0:12}" = "7f454c460101" ] && [ "${ELF_HEAD:32:8}" = "02000800" ] ||
		die "sonix_launch is not a 32-bit little-endian MIPS executable.
	  Build it with \`make target\` (or \`make sonix_launch\`) in sonix-player."
	say "    launch:  sonix_launch, $(get_size "$LAUNCH_BIN") bytes"
else
	warn "sonix_launch is not next to this script: the launcher keeps its shell for the whole session."
fi
say ""

# ==========================================================================
# Modules the kernel has built in
# ==========================================================================
# drop_builtin_modules LIST: for every name in LIST (module names, one per
# line) that has a module_driver/<name>.ko in the image, removes the .ko, the
# module_driver/*.sh whose only job is to insmod it, and the `sh <script>`
# line that runs that script from driver_default_init_script.sh. A script that
# does anything else as well is kept and named, to be edited by hand.
#
# The parameters a removed script passed are printed: a built-in driver gets
# none and runs on its defaults, so one that was there on purpose has to
# become the driver's default in the kernel.
drop_builtin_modules() {
	local list="$1" md="$SQUASH_DIR/module_driver"
	local drv_list="$md/driver_default_init_script.sh"
	local name ko kore sh shre other args left removed=0
	[ -d "$md" ] || return 0
	step "[$MODEL_NAME] removing the modules kernel/xImage-$MODEL_DIR has built in"
	while IFS= read -r name; do
		# module names treat - and _ as the same character
		name="${name%%#*}"; name="$(echo "$name" | tr -d ' \t\r' | tr - _)"
		[ -n "$name" ] || continue
		ko=""
		for f in "$md"/*.ko; do
			[ -e "$f" ] || continue
			if [ "$(basename "$f" .ko | tr - _)" = "$name" ]; then ko="$f"; break; fi
		done
		[ -n "$ko" ] || continue
		kore="$(basename "$ko" | sed 's/\./\\./g')"
		rm -f "$ko"
		removed=$((removed + 1))
		say "    module_driver/$(basename "$ko") removed: built into the kernel"
		# the scripts that load it
		for sh in "$md"/*.sh; do
			[ -e "$sh" ] || continue
			[ "$sh" != "$drv_list" ] || continue
			grep -qE "insmod[[:space:]]+(\./)?$kore([[:space:]]|\\\\|$)" "$sh" || continue
			# anything besides comments, blank lines and that one insmod
			# (with its continuation lines)?
			other="$(awk '
				/^[[:space:]]*(#|$)/ { next }
				cont { cont = /\\[[:space:]]*$/; next }
				/^[[:space:]]*insmod[[:space:]]/ && !seen { seen = 1; cont = /\\[[:space:]]*$/; next }
				{ print }' "$sh")"
			if [ -n "$other" ]; then
				warn "module_driver/$(basename "$sh") loads $(basename "$ko") but does more than that: kept, edit it by hand"
				continue
			fi
			args="$(awk '
				/^[[:space:]]*#/ { next }
				/^[[:space:]]*insmod[[:space:]]/ || cont { line = line " " $0; cont = /\\[[:space:]]*$/ }
				END { print line }' "$sh" | tr -d '\\' | tr -s ' \t' ' ' |
				sed -E 's/^ *insmod +[^ ]+ *//; s/ *$//')"
			rm -f "$sh"
			say "    module_driver/$(basename "$sh") removed"
			if [ -n "$args" ]; then
				say "      it passed: $args"
				say "      (a built-in driver runs on its defaults instead)"
			fi
			if [ -f "$drv_list" ]; then
				shre="$(basename "$sh" | sed 's/\./\\./g')"
				sed -i.bak -E "/^[[:space:]]*(sh|\.)[[:space:]]+(\.\/)?$shre[[:space:]]*$/d" "$drv_list"
				rm -f "$drv_list.bak"
			fi
		done
		# anything else that still names the .ko
		left="$(grep -rlE "$kore" "$md" "$SQUASH_DIR/etc/init.d" 2>/dev/null | sed "s|^$SQUASH_DIR/||" | tr '\n' ' ' || true)"
		[ -z "$left" ] || warn "$(basename "$ko") is still named in: $left"
	done < "$list"
	say "    $removed modules removed"
	say ""
}

# Removes paths of the rootfs, given as patterns (see STOCK_UNUSED), and says
# what went and how much it was. A path the assets bring is kept; a symlink
# left pointing into something that went is reported. Runs inside
# build_one(), whose SQUASH_DIR and MODEL_DIR it uses.
remove_stock_paths() {
	local pattern path rel freed=0 count=0 kb dangling removed_paths=""
	for pattern in "$@"; do
		# the pattern expands inside the rootfs; no match leaves it as it is
		for path in "$SQUASH_DIR"/$pattern; do
			[ -e "$path" ] || [ -L "$path" ] || continue
			rel="${path#"$SQUASH_DIR"/}"
			if [ -e "$ASSETS_DIR/$MODEL_DIR/$rel" ] || [ -L "$ASSETS_DIR/$MODEL_DIR/$rel" ]; then
				warn "$rel is in the assets: kept"
				continue
			fi
			kb=""
			[ -L "$path" ] || kb="$(du -sk "$path" | cut -f1)"
			rm -rf "$path"
			[ ! -e "$path" ] && [ ! -L "$path" ] || die "could not remove $rel."
			freed=$((freed + ${kb:-0}))
			count=$((count + 1))
			removed_paths="$removed_paths/$rel"$'\n'
			say "    $rel removed${kb:+ (${kb} KiB)}"
		done
	done
	# A symlink left pointing into something that went. Only those: the stock
	# tree has links to files that exist only at runtime (/etc/resolv.conf in
	# /tmp, /etc/localtime on userdata), which are fine.
	dangling="$(cd "$SQUASH_DIR" && find . -type l 2>/dev/null |
		while IFS= read -r l; do
			t="$(readlink "$l")"
			case "$t" in
			/*) ;;
			*) t="/$(cd "$(dirname "$l")" && pwd -P | sed "s|^$(cd "$SQUASH_DIR" && pwd -P)||")/$t" ;;
			esac
			t="$(echo "$t" | sed -E ':a; s|/[^/]+/\.\./|/|; ta; s|/\./|/|g; s|//+|/|g')"
			while IFS= read -r r; do
				[ -n "$r" ] || continue
				case "$t" in "$r"|"$r"/*) echo "${l#./}"; break ;; esac
			done <<< "$removed_paths"
		done)"
	[ -z "$dangling" ] || warn "symlinks left pointing at removed files: $(echo "$dangling" | tr '\n' ' ')"
	say "    $count removed, $freed KiB"
}

# ==========================================================================
# One image, start to finish
# ==========================================================================
# Called once per model. Everything it touches -- the work folder, the unpacked
# rootfs -- is torn down by cleanup() before the next model starts, so the two
# builds cannot see each other's files.
build_one() {
	local MODEL_DIR="$1" MODEL_NAME="$2"
	local SOURCE_UPT="$SCRIPT_DIR/$3" OUTPUT_UPT="$SCRIPT_DIR/$4"

	say "${YELLOW}###############################################${NC}"
	say "${YELLOW}###   $MODEL_NAME${NC}"
	say "${YELLOW}###############################################${NC}"
	say ""

	# ==========================================================================
	# 1. Unpack the stock firmware
	# ==========================================================================
	step "[$MODEL_NAME] unpacking $(basename "$SOURCE_UPT")"

	rm -rf "$WORK_DIR" "$SQUASH_DIR" "$MERGED_SQUASHFS"
	mkdir -p "$WORK_DIR"

	7z x "$SOURCE_UPT" -o"$WORK_DIR" -y > /dev/null

	[ -d "$OTA_DIR" ] || die "there is no ota_v0 folder inside the .upt: not a firmware of this kind."

	# R1 units shipped with HiBy firmware 2.0 use a newer recovery bootloader
	# that refuses OTA packages without this board compatibility marker.
	# Always emit it for R1, including when the supplied base predates 2.0.
	if [ "$MODEL_DIR" = "R1" ]; then
		printf '%s\n' 'compatibility=hiby_r1_x1600_auto_rf_auto' > "$OTA_DIR/ota_compatibility.in"
	fi

	say "    Joining the rootfs chunks..."
	( cd "$OTA_DIR" && cat rootfs.squashfs.* ) > "$MERGED_SQUASHFS"

	say "    Extracting the filesystem..."
	unsquashfs -f -d "$SQUASH_DIR" "$MERGED_SQUASHFS" > /dev/null
	rm -f "$MERGED_SQUASHFS"

	# The old chunks go now: what gets written back must not sit next to what it
	# replaces, or the image ends up carrying both.
	rm -f "$OTA_DIR/ota_md5_rootfs.squashfs."* "$OTA_DIR/rootfs."*

	[ -f "$SQUASH_DIR/$STOCK_LAUNCHER" ] || die "the rootfs has no $STOCK_LAUNCHER: unexpected firmware."
	[ -f "$SQUASH_DIR/$INIT_SCRIPT" ]    || die "the rootfs has no $INIT_SCRIPT: unexpected firmware."
	say ""

	# ==========================================================================
	# 2. The binary
	# ==========================================================================
	step "[$MODEL_NAME] installing sonix_player"

	chmod 777 "$PLAYER_BIN"

	if [ -e "$SQUASH_DIR/$STOCK_PLAYER" ]; then
		rm -f "$SQUASH_DIR/$STOCK_PLAYER"
		say "    $STOCK_PLAYER removed"
	else
		warn "$STOCK_PLAYER was not there to begin with"
	fi

	cp -f "$PLAYER_BIN" "$SQUASH_DIR/usr/bin/sonix_player"
	chmod 777 "$SQUASH_DIR/usr/bin/sonix_player"
	say "    usr/bin/sonix_player installed (777)"
	say ""

	# ==========================================================================
	# 3. The launcher script
	# ==========================================================================
	step "[$MODEL_NAME] renaming the launcher"

	mv "$SQUASH_DIR/$STOCK_LAUNCHER" "$SQUASH_DIR/$NEW_LAUNCHER"
	say "    $STOCK_LAUNCHER -> $NEW_LAUNCHER"
	rename_player_in "$SQUASH_DIR/$NEW_LAUNCHER" "$NEW_LAUNCHER"
	# The mode is set after the overlay, not here: the overlay may carry its own
	# copy of this file and would bring its own mode with it. See step 6.
	say ""

	# ==========================================================================
	# 4. The init script
	# ==========================================================================
	step "[$MODEL_NAME] updating $INIT_SCRIPT"

	rename_player_in "$SQUASH_DIR/$INIT_SCRIPT" "$INIT_SCRIPT"
	say ""

	# ==========================================================================
	# 4b. The HBC3000's configuration
	# ==========================================================================
	#
	# The open sa_sound_hbc3000.ko loads the FPGA's configuration from
	# lib/firmware/hbc3000.fw. The configuration is HiBy's and is not in the
	# assets: it is taken here from the stock module, before the overlay puts
	# the open one in its place.
	if [ -f "$ASSETS_DIR/$MODEL_DIR/module_driver/sa_sound_hbc3000.ko" ]; then
		step "[$MODEL_NAME] taking the HBC3000 configuration from the stock module"

		command -v perl >/dev/null 2>&1 || die "perl is needed to read the stock sa_sound_hbc3000.ko."
		STOCK_HBC="$SQUASH_DIR/module_driver/sa_sound_hbc3000.ko"
		[ -f "$STOCK_HBC" ] || die "the stock firmware has no module_driver/sa_sound_hbc3000.ko.
	  The open one would have no configuration to load: the outputs would stay silent."
		mkdir -p "$SQUASH_DIR/lib/firmware"
		hbc3000_firmware "$STOCK_HBC" "$SQUASH_DIR/lib/firmware/hbc3000.fw" ||
			die "no configuration in the stock sa_sound_hbc3000.ko."
		say "    lib/firmware/hbc3000.fw: $(wc -c < "$SQUASH_DIR/lib/firmware/hbc3000.fw" | tr -d ' ') bytes"
		say ""
	fi

	# ==========================================================================
	# 4c. Room for the system libraries in the assets
	# ==========================================================================
	#
	# assets/MODEL/usr/lib holds libraries rebuilt from newer sources under
	# their sonames (tools/system-libs in sonix-player). In the stock image a
	# soname is usually a symlink to a versioned file, which the overlay would
	# leave behind beside the new library: the symlink and that file go, and
	# every other symlink to the file (libfoo.so) is pointed at the soname.
	LIB_ASSETS="$(cd "$ASSETS_DIR/$MODEL_DIR" && find usr/lib -maxdepth 1 -type f -name 'lib*.so.*' 2>/dev/null | sort)"
	if [ -n "$LIB_ASSETS" ]; then
		step "[$MODEL_NAME] making room for the updated system libraries"

		for rel in $LIB_ASSETS; do
			soname="$(basename "$rel")"
			dst="$SQUASH_DIR/$rel"
			[ -L "$dst" ] || continue
			target="$(readlink "$dst")"
			rm -f "$dst"
			case "$target" in
			*/*) continue ;; # outside usr/lib: not this library's file
			esac
			if [ -f "$SQUASH_DIR/usr/lib/$target" ] && [ ! -L "$SQUASH_DIR/usr/lib/$target" ]; then
				rm -f "$SQUASH_DIR/usr/lib/$target"
				say "    usr/lib/$target removed: $soname replaces it"
			fi
			for link in "$SQUASH_DIR"/usr/lib/*; do
				[ -L "$link" ] && [ "$(readlink "$link")" = "$target" ] || continue
				ln -sfn "$soname" "$link"
				say "    usr/lib/$(basename "$link") -> $soname"
			done
		done

		# The assets' ntfs-3g has libntfs-3g linked in, and nothing else in
		# the image uses the stock library.
		NTFS_ASSET="$ASSETS_DIR/$MODEL_DIR/usr/bin/ntfs-3g"
		if [ -f "$NTFS_ASSET" ] && ! LC_ALL=C grep -aq 'libntfs-3g\.so\.' "$NTFS_ASSET"; then
			for f in "$SQUASH_DIR"/usr/lib/libntfs-3g.so*; do
				[ -e "$f" ] || [ -L "$f" ] || continue
				rm -f "$f"
				say "    usr/lib/$(basename "$f") removed: the new ntfs-3g carries it"
			done
		fi
		say ""
	fi

	# ==========================================================================
	# 5. The overlay
	# ==========================================================================
	step "[$MODEL_NAME] copying the overlay onto the root of the rootfs"

	# tar rather than cp: it merges into directories that already exist,
	# overwrites the files that clash, carries the hidden ones, and behaves the
	# same way on macOS and on Linux -- none of which is true of `cp -a` on both.
	( cd "$ASSETS_DIR/$MODEL_DIR" && tar cf - . ) | ( cd "$SQUASH_DIR" && tar xf - )

	MODEL_N="$(cd "$ASSETS_DIR/$MODEL_DIR" && find . -type f | wc -l | tr -d ' ')"
	say "    $MODEL_DIR/: $MODEL_N files"

	# The streaming keys go in sealed, as streaming-keys.bin. One left in the
	# clear in the assets is not shipped: an image is unpacked by anyone who
	# downloads it. See tools/seal_streamkeys.py in sonix-player.
	PLAIN_KEYS="usr/resource/sonix/components/streaming-keys.ini"
	if [ -f "$SQUASH_DIR/$PLAIN_KEYS" ]; then
		rm -f "$SQUASH_DIR/$PLAIN_KEYS"
		warn "$MODEL_DIR/$PLAIN_KEYS is in the clear and was left out; seal it with tools/seal_streamkeys.py"
	fi
	if [ ! -f "$SQUASH_DIR/usr/resource/sonix/components/streaming-keys.bin" ]; then
		warn "no streaming-keys.bin in $MODEL_DIR: Tidal, Qobuz and podcasts will be off"
	fi

	# The stock Wi-Fi/Bluetooth driver goes once bcm_wlbt_power and brcmfmac
	# have taken its place: only when the module list no longer loads it.
	DRIVER_LIST="$SQUASH_DIR/module_driver/driver_default_init_script.sh"
	if [ -f "$ASSETS_DIR/$MODEL_DIR/module_driver/bcm_wlbt_power.ko" ] &&
		grep -q '^sh bcm_wlbt_power\.sh' "$DRIVER_LIST" 2>/dev/null &&
		! grep -q 'cywdhd' "$DRIVER_LIST"; then
		for rel in module_driver/cywdhd.ko module_driver/cywdhd.sh; do
			if [ -f "$SQUASH_DIR/$rel" ]; then
				rm -f "$SQUASH_DIR/$rel"
				say "    $rel removed (bcm_wlbt_power and brcmfmac replace it)"
			fi
		done
	fi

	# What the other model's tree carries and this one does not. Nothing is
	# copied across and nothing fails: whatever the stock firmware had at that
	# path is left alone, which for a boot logo is this model's own at its own
	# size. It is listed because the other reason for the difference is having
	# added a file to one tree and forgotten the other.
	for other in "${MODELS[@]}"; do
		IFS='|' read -r o_dir o_src o_out o_name <<< "$other"
		[ "$o_dir" != "$MODEL_DIR" ] || continue
		[ -d "$ASSETS_DIR/$o_dir" ] || continue
		while IFS= read -r rel; do
			[ -n "$rel" ] || continue
			say "    only in $o_dir/: ${rel#./} -- the stock firmware's own is kept here"
		done < <(comm -23 \
			<(cd "$ASSETS_DIR/$o_dir" && find . -type f | sort) \
			<(cd "$ASSETS_DIR/$MODEL_DIR" && find . -type f | sort))
	done
	say ""

	# ==========================================================================
	# 5a. Boot logo
	# ==========================================================================
	#
	# The player shows /etc/logo.jpeg during charge-screen boot
	# (chargescreen.c: show_boot_logo). The stock HiBy logo is at that path;
	# the assets ship per-theme alternatives in etc/boot-logo/ but nothing
	# ever copies one of them to etc/logo.jpeg. Do it here: prefer dark-theme
	# as the firmware-wide default (matches the out-of-box dark theme), falling
	# back to whichever theme variant is present, then leaving the stock logo.
	step "[$MODEL_NAME] installing the boot logo"
	LOGO_DST="$SQUASH_DIR/etc/logo.jpeg"
	LOGO_SRC=""
	for candidate in \
		"$SQUASH_DIR/etc/boot-logo/dark-theme-blue.jpeg" \
		"$SQUASH_DIR/etc/boot-logo/dark-theme-green.jpeg" \
		"$SQUASH_DIR/etc/boot-logo/dark-theme-purple.jpeg" \
		"$SQUASH_DIR/etc/boot-logo/dark-theme-orange.jpeg" \
		"$SQUASH_DIR/etc/boot-logo/dark-theme-pink.jpeg" \
		"$SQUASH_DIR/etc/boot-logo/dark-theme-yellow.jpeg" \
		"$SQUASH_DIR/etc/boot-logo/"*.jpeg
	do
		[ -f "$candidate" ] && { LOGO_SRC="$candidate"; break; }
	done
	if [ -n "$LOGO_SRC" ]; then
		cp -f "$LOGO_SRC" "$LOGO_DST"
		say "    etc/logo.jpeg <- $(basename "$LOGO_SRC")"
	else
		warn "no boot-logo found in etc/boot-logo/: the stock HiBy logo stays"
	fi
	say ""

	# ==========================================================================
	# 5b. The modes that decide whether any of this runs
	# ==========================================================================
	#
	# After the overlay, because the overlay wins: it may carry its own copy of
	# the launcher, and tar brings the mode along with the file.
	#
	# This is not housekeeping. The init script is
	#
	#     PL01=/usr/bin/sonix_player.sh
	#     [ -f $PL01 ] && ( $PL01 & ) || echo "file <$PL01> don't exist."
	#
	# and `-f` asks whether the file EXISTS, not whether it can be run. A
	# launcher that arrives without its execute bit passes that test, fails to
	# start, takes the `&&` branch so the `||` message is never printed, and
	# leaves a device sitting on its boot logo with nothing in any log and no
	# reboot to hint at it. An assets folder that has been through a zip is
	# enough to cause it, which is why this is checked rather than assumed.
	step "[$MODEL_NAME] making the boot path executable"

	# Readable by everyone first, as the stock tree is. tar brings every mode
	# along -- the directories' too, the top one included -- so an assets folder
	# checked out under a strict umask (700 directories, 600 files) makes those
	# parts of the image readable by root alone. Some of what reads them checks
	# for "others" whoever it runs as: thttpd answers 403 "not world-readable"
	# for the Wi-Fi transfer page in a directory like that. Only read, and
	# execute where a directory or the owner already has it, is added; nothing
	# is taken away. Not through symlinks: chmod follows them, and their
	# targets are paths on the device, not in the image.
	( cd "$ASSETS_DIR/$MODEL_DIR" && find . ! -type l -print0 ) | ( cd "$SQUASH_DIR" && xargs -0 chmod go+rX )
	say "    everything the assets added is readable by all"

	for rel in "$NEW_LAUNCHER" "$INIT_SCRIPT" usr/bin/bluealsa usr/bin/shairport_on.sh etc/init.d/S80_bt_init; do
		if [ -f "$SQUASH_DIR/$rel" ]; then
			chmod 755 "$SQUASH_DIR/$rel"
			say "    $rel is 755"
		fi
	done

	# Anything else the overlay put in a directory of executables, the
	# libraries, the kernel-module scripts and the init scripts with it.
	for rel in $(cd "$ASSETS_DIR/$MODEL_DIR" && find usr/bin usr/lib module_driver etc/init.d -type f 2>/dev/null); do
		[ -f "$SQUASH_DIR/$rel" ] || continue
		chmod 755 "$SQUASH_DIR/$rel"
	done

	# The two that decide whether the player is ever started at all.
	for rel in "$NEW_LAUNCHER" "$INIT_SCRIPT"; do
		[ -x "$SQUASH_DIR/$rel" ] || die "$rel is not executable in the image.
	  Nothing would start the player and the device would sit on its boot logo."
	done
	say ""

	# ==========================================================================
	# 5b'. Modules the new kernel has built in
	# ==========================================================================
	#
	# kernel/modules.builtin-<model> lists them by name, one per line
	# (hiby-custom-kernel's build writes it next to the xImage). A .ko of the
	# same name -- stock or from the assets -- would only be refused at boot,
	# so it goes, with the script that loads it and that script's line in the
	# module list. Only with our own kernel: the stock one has none of them
	# built in. After 5b, which walks the overlay's file list and would trip
	# over a file taken out before it.
	BUILTIN_LIST="$KERNEL_DIR/modules.builtin-$MODEL_DIR"
	if [ -f "$KERNEL_DIR/xImage-$MODEL_DIR" ] && [ -f "$BUILTIN_LIST" ]; then
		drop_builtin_modules "$BUILTIN_LIST"
	elif [ -f "$BUILTIN_LIST" ]; then
		warn "kernel/modules.builtin-$MODEL_DIR is here but kernel/xImage-$MODEL_DIR is not: the stock kernel stays, so no module is removed"
	elif [ -f "$KERNEL_DIR/xImage-$MODEL_DIR" ]; then
		warn "no kernel/modules.builtin-$MODEL_DIR: every module in module_driver/ is kept, built into kernel/xImage-$MODEL_DIR or not"
	fi

	# ==========================================================================
	# 5c. sonix_launch
	# ==========================================================================
	#
	# The launcher execs sonix_launch just before the line that starts the
	# player, so the shell is replaced by a process of a few kB that does what
	# the rest of the script does: runs the player, then `sleep 1; reboot`. The
	# original lines stay under it and still run on a rootfs without the binary.
	if [ -f "$LAUNCH_BIN" ]; then
		step "[$MODEL_NAME] starting the player through sonix_launch"

		cp -f "$LAUNCH_BIN" "$SQUASH_DIR/usr/bin/sonix_launch"
		chmod 755 "$SQUASH_DIR/usr/bin/sonix_launch"
		say "    usr/bin/sonix_launch installed (755)"

		LAUNCHER_FILE="$SQUASH_DIR/$NEW_LAUNCHER"
		if grep -q "sonix_launch" "$LAUNCHER_FILE"; then
			say "    $NEW_LAUNCHER already calls it"
		else
			PLAYER_LINES="$(grep -c '^/usr/bin/sonix_player$' "$LAUNCHER_FILE" || true)"
			if [ "$PLAYER_LINES" = "1" ]; then
				awk '$0 == "/usr/bin/sonix_player" { print "[ -x /usr/bin/sonix_launch ] && exec /usr/bin/sonix_launch /usr/bin/sonix_player" } { print }' \
					"$LAUNCHER_FILE" > "$LAUNCHER_FILE.new"
				mv -f "$LAUNCHER_FILE.new" "$LAUNCHER_FILE"
				chmod 755 "$LAUNCHER_FILE"
				grep -q "exec /usr/bin/sonix_launch" "$LAUNCHER_FILE" || die "$NEW_LAUNCHER was not rewritten."
				say "    $NEW_LAUNCHER execs sonix_launch"
			else
				rm -f "$SQUASH_DIR/usr/bin/sonix_launch"
				warn "$NEW_LAUNCHER has $PLAYER_LINES lines that are exactly /usr/bin/sonix_player, not one:
	  left as it is, and sonix_launch not installed."
			fi
		fi
		say ""
	fi

	# ==========================================================================
	# 6. The stock interface's resources
	# ==========================================================================
	step "[$MODEL_NAME] removing the stock interface's folders"

	for dir in "${STOCK_UI_DIRS[@]}"; do
		if [ -e "$SQUASH_DIR/$dir" ]; then
			rm -rf "$SQUASH_DIR/$dir"
			[ -e "$SQUASH_DIR/$dir" ] && die "could not remove $dir."
			say "    $dir removed"
		else
			say "    $dir was not there"
		fi
	done
	say ""

	# ==========================================================================
	# 6b. The rest of the stock firmware nothing uses
	# ==========================================================================
	step "[$MODEL_NAME] removing the stock files nothing uses"

	remove_stock_paths "${STOCK_UNUSED[@]}"
	say ""

	# ==========================================================================
	# 6c. The debugging tools, with --strip-debug-tools
	# ==========================================================================
	if [ "$STRIP_DEBUG_TOOLS" = 1 ]; then
		step "[$MODEL_NAME] removing the debugging tools (--strip-debug-tools)"
		remove_stock_paths "${STOCK_DEBUG_TOOLS[@]}"
		say ""
	fi

	# ==========================================================================
	# 7. The build stamp
	# ==========================================================================
	step "[$MODEL_NAME] system-info.json"

	JSON="$SQUASH_DIR/$SYSTEM_INFO"
	[ -f "$JSON" ] || die "$SYSTEM_INFO is not in the rootfs.
	  It should come from assets/$MODEL_DIR/$SYSTEM_INFO"

	# The name in the file against the model being built. Everything the player
	# decides by model -- the panel it opens the simulator at, the .upt it will
	# accept, the converter and the serial it prints -- hangs off this one string,
	# and an image carrying the other model's name would offer that model's update
	# to a device that cannot survive it.
	FOUND_NAME="$(tr -d '\r' < "$JSON" | tr ',' '\n' | grep -i '"device-name"' | head -1 | sed -E 's/.*"device-name"[[:space:]]*:[[:space:]]*"([^"]*)".*/\1/')"
	[ -n "$FOUND_NAME" ] || die "$SYSTEM_INFO has no device-name."
	[ "$FOUND_NAME" = "$MODEL_NAME" ] || die "$SYSTEM_INFO says device-name \"$FOUND_NAME\",
	  but this is the $MODEL_NAME image. Check assets/$MODEL_DIR/$SYSTEM_INFO."
	say "    device-name = $FOUND_NAME"

	# Whatever case the key was written in. The player reads it case-insensitively
	# (find_nocase in src/system/device/sysinfo.c), so "Build_version" and
	# "build_version" are equally correct in the file and this has to accept the
	# same. The spelling is read out first and then used literally in the
	# substitution, because sed's case-insensitive flag is a GNU extension and this
	# has to run on macOS too.
	KEY="$(grep -oE '"[Bb][Uu][Ii][Ll][Dd]_[Vv][Ee][Rr][Ss][Ii][Oo][Nn]"' "$JSON" | head -1 || true)"
	[ -n "$KEY" ] || die "$SYSTEM_INFO has no build_version key, in any case."

	sed_file "s/($KEY[[:space:]]*:[[:space:]]*\")[^\"]*(\")/\\1$BUILD_STAMP\\2/" "$JSON"

	grep -q "\"$BUILD_STAMP\"" "$JSON" || die "$KEY was not rewritten."
	say "    $KEY = $BUILD_STAMP"
	say ""

	# ==========================================================================
	# 8. Repack
	# ==========================================================================
	say "${YELLOW}###############################${NC}"
	say "${YELLOW}###   NEW FILESYSTEM        ###${NC}"
	say "${YELLOW}###############################${NC}"
	say ""

	step "[$MODEL_NAME] clearing macOS clutter"
	find "$SQUASH_DIR" -name '.DS_Store' -type f -delete 2>/dev/null || true
	find "$SQUASH_DIR" -name '._*' -type f -delete 2>/dev/null || true

	# Nothing this script writes belongs in the image.
	find "$SQUASH_DIR" -name '*.packer.tmp' -type f -delete 2>/dev/null || true

	ROOTFS_NEW="$OTA_DIR/rootfs.squashfs"
	KERNEL_NEW="$KERNEL_DIR/xImage-$MODEL_DIR"

	step "[$MODEL_NAME] building the filesystem"
	mksquashfs "$SQUASH_DIR" "$ROOTFS_NEW" -comp lzo -all-root > /dev/null

	ORIGINAL_SUM="$(get_md5 "$ROOTFS_NEW")"
	SIZE="$(get_size "$ROOTFS_NEW")"
	say "    rootfs.squashfs: $SIZE bytes, md5 $ORIGINAL_SUM (max $ROOTFS_MAX)"
	[ "$SIZE" -le "$ROOTFS_MAX" ] ||
		die "rootfs.squashfs is $SIZE bytes, $((SIZE - ROOTFS_MAX)) more than the $ROOTFS_MAX-byte rootfs partition."

	step "[$MODEL_NAME] updating ota_update.in"
	if [ -f "$KERNEL_NEW" ]; then
		# A kernel of our own: it replaces the stock one, chunks and md5 chain
		# alike. It has to fit in the kernel partition.
		X_SIZE="$(get_size "$KERNEL_NEW")"
		X_MD5="$(get_md5 "$KERNEL_NEW")"
		[ "$X_SIZE" -le "$KERNEL_MAX" ] ||
			die "kernel/xImage-$MODEL_DIR is $X_SIZE bytes, $((X_SIZE - KERNEL_MAX)) more than the $KERNEL_MAX-byte kernel partition."
		say "    xImage: kernel/xImage-$MODEL_DIR, $X_SIZE bytes, md5 $X_MD5 (max: $KERNEL_MAX bytes)"
	else
		# The stock kernel stays, so its two lines are carried over exactly as
		# they were: recomputing them from a file this script never writes
		# would be a way of getting them wrong.
		X_SIZE="$(grep -A 3 'img_name=xImage' "$OTA_DIR/ota_update.in" | grep 'img_size' | cut -d= -f2 | tr -d '\r ')"
		X_MD5="$(grep -A 3 'img_name=xImage' "$OTA_DIR/ota_update.in" | grep 'img_md5' | cut -d= -f2 | tr -d '\r ')"
		[ -n "$X_SIZE" ] && [ -n "$X_MD5" ] || die "cannot read the kernel entry from ota_update.in."
		say "    xImage: the stock kernel, unchanged"
	fi

	cat > "$OTA_DIR/ota_update.in" <<-EOF
	ota_version=0

	img_type=kernel
	img_name=xImage
	img_size=$X_SIZE
	img_md5=$X_MD5

	img_type=rootfs
	img_name=rootfs.squashfs
	img_size=$SIZE
	img_md5=$ORIGINAL_SUM
	EOF

	step "[$MODEL_NAME] splitting into chunks and building the md5 chains"
	write_chunks rootfs.squashfs "$ROOTFS_NEW"
	if [ -f "$KERNEL_NEW" ]; then
		rm -f "$OTA_DIR/ota_md5_xImage."* "$OTA_DIR/xImage."*
		cp "$KERNEL_NEW" "$OTA_DIR/xImage"
		write_chunks xImage "$OTA_DIR/xImage"
	fi
	say ""

	# ==========================================================================
	# 9. The image
	# ==========================================================================
	say "${YELLOW}###############################${NC}"
	say "${YELLOW}###   FIRMWARE IMAGE        ###${NC}"
	say "${YELLOW}###############################${NC}"
	say ""

	step "[$MODEL_NAME] writing $(basename "$OUTPUT_UPT")"
	rm -f "$OUTPUT_UPT"
	"$ISO_TOOL" -o "$OUTPUT_UPT" -J -r "$WORK_DIR" > /dev/null 2>&1

	[ -f "$OUTPUT_UPT" ] || die "the image was not written."

	cleanup

	say "  $OUTPUT_UPT"
	say "  $(get_size "$OUTPUT_UPT") bytes, build_version $BUILD_STAMP"
	say ""
}

# ==========================================================================
# Every model that can be built
# ==========================================================================
while IFS='|' read -r m_dir m_src m_out m_name; do
	[ -n "$m_dir" ] || continue
	build_one "$m_dir" "$m_name" "$m_src" "$m_out"
done <<< "$BUILDABLE"

say "${GREEN}#############################${NC}"
say "${GREEN}###   DONE                ###${NC}"
say "${GREEN}#############################${NC}"
say ""
say "${GREEN}  Ready to be copied to the SD card.${NC}"
say ""
