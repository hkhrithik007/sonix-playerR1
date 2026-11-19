#!/usr/bin/env bash
# Rebuild only BlueALSA's daemon; preserve the existing libraries and plugins.
set -euo pipefail

pin=1935d6dcb8975f2d7a51aaafe61538d157224623
script_dir=$(cd -- "$(dirname -- "$0")" && pwd)
player_dir=$(cd -- "$script_dir/../.." && pwd)
: "${BLUEALSA_SOURCE:?Set the pristine BlueALSA 5.0 source checkout}"
: "${BLUEALSA_BUILD:?Set a new absolute build directory}"
: "${CC:?Set the MIPS compiler or sysroot-aware compiler wrapper}"
: "${CROSS_COMPILE:?Set the toolchain prefix ending in a hyphen}"
: "${PKG_CONFIG_LIBDIR:?Set the matching target pkg-config directory}"
: "${PKG_CONFIG_SYSROOT_DIR:?Set the target headers and real runtime libraries}"
: "${RUNTIME_ROOT:?Set the matching runtime ELF dependency root for ABI checks}"

test "$(git -C "$BLUEALSA_SOURCE" rev-parse HEAD)" = "$pin"
# The archive below reads the pinned Git object, never working-tree files.
# This also avoids CRLF/index differences when the source is stored on Windows.
case "$BLUEALSA_BUILD" in /*) ;; *) echo 'Use an absolute build directory' >&2; exit 2;; esac
test ! -e "$BLUEALSA_BUILD" && test ! -L "$BLUEALSA_BUILD"
case "${CFLAGS:-} ${LDFLAGS:-}" in *-ffast-math*|*-Ofast*) echo 'Unsafe math flags are not supported' >&2; exit 2;; esac

mkdir -m 700 -- "$BLUEALSA_BUILD"
printf '%s\n' "$BLUEALSA_BUILD" > "$BLUEALSA_BUILD/.owner"
mkdir -- "$BLUEALSA_BUILD/source" "$BLUEALSA_BUILD/artifacts"
git -C "$BLUEALSA_SOURCE" archive "$pin" | tar -x -C "$BLUEALSA_BUILD/source"
cd -- "$BLUEALSA_BUILD/source"
patch -p1 --forward --input="$script_dir/bluealsa-5.0-codec-bitrate.patch"
patch -p1 --forward --input="$script_dir/bluealsa-5.0-ldac-handles.patch"
mkdir -p m4
"${AUTORECONF:-autoreconf}" --install --force

export AR="${CROSS_COMPILE}gcc-ar" RANLIB="${CROSS_COMPILE}gcc-ranlib"
export STRIP="${CROSS_COMPILE}strip"
export CFLAGS="${CFLAGS:--O2 -march=mips32r2 -mno-madd4 -D_FILE_OFFSET_BITS=64} -flto=2 -DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_58 -DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_62"
export LDFLAGS="${LDFLAGS:-} -flto=2"
export ac_cv_prog_c_openmp=unsupported
./configure --host=mipsel-rockbox-linux-gnu --prefix=/usr --sysconfdir=/etc \
    --localstatedir=/usr/data --with-dbusconfdir=/usr/share/dbus-1/system.d \
    --with-alsaplugindir=/usr/lib/alsa-lib --with-alsaconfdir=/etc/alsa/conf.d \
    --enable-aac --enable-aptx --enable-aptx-hd --with-libopenaptx --enable-ldac \
    --disable-aplay --disable-rfcomm --disable-debug --disable-static --disable-manpages
make -j"${JOBS:-2}"

artifacts="$BLUEALSA_BUILD/artifacts"
cp -- src/bluealsad "$artifacts/bluealsad.debug"
"$STRIP" -o "$artifacts/bluealsad" "$artifacts/bluealsad.debug"
"${PYTHON:-python3}" "$player_dir/tools/system-libs/abicheck.py" \
    "$RUNTIME_ROOT" "$artifacts/bluealsad"
(
    cd -- "$artifacts"
    sha256sum bluealsad > bluealsad.sha256
    printf 'BlueALSA 5.0.0 source=%s LTO=2 ABI=checked native=unverified\n' "$pin" > BUILD.txt
    sha256sum "$script_dir/bluealsa-5.0-codec-bitrate.patch" >> BUILD.txt
    sha256sum "$script_dir/bluealsa-5.0-ldac-handles.patch" >> BUILD.txt
)
printf 'Built daemon only; native validation and matching firmware packaging remain: %s\n' "$artifacts"
