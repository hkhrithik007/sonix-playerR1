#!/bin/bash
# build.sh STOCK_ROOTFS SYSROOT WORK_DIR OUTPUT_DIR
set -euo pipefail
test "$#" = 4 || { echo "usage: $0 STOCK_ROOTFS SYSROOT WORK_DIR OUTPUT_DIR" >&2; exit 2; }
here=$(cd "$(dirname "$0")" && pwd)
stock=$(realpath "$1")
sysroot=$(realpath "$2")
work=$(realpath -m "$3")
out=$(realpath -m "$4")
cross=${CROSS_PREFIX:-/root/sonix-tools/rockbox-toolchain/bin/mipsel-rockbox-linux-gnu}
abi_python=${ABI_PYTHON:-python3}
: "${PKG_CONFIG_LIBDIR:?Set target pkg-config directory for GLib, D-Bus and ALSA}"
test -f "$stock/lib/libc-2.22.so"
test -f "$sysroot/lib/libc-2.22.so"
test -f "$sysroot/usr/include/glib-2.0/glib.h"
test -x "$cross-gcc"
test ! -e "$work"
test ! -e "$out"
mkdir -p "$work" "$out"
url=https://www.kernel.org/pub/linux/bluetooth/bluez-5.54.tar.xz
sha=68cdab9e63e8832b130d5979dc8c96fdb087b31278f342874d992af3e56656dc
curl -fL --retry 2 "$url" -o "$work/source.tar.xz"
echo "$sha  $work/source.tar.xz" | sha256sum -c -
tar -xf "$work/source.tar.xz" -C "$work"
src=$work/bluez-5.54
(cd "$src" && patch --batch -p1 < "$here/bluez-5.54-sonix.patch")
mkdir "$work/build"
cd "$work/build"
# Use one coherent glibc 2.22 sysroot, including libc's linker script.
cat > "$work/cc" <<EOF
#!/bin/sh
exec "$cross-gcc" --sysroot="$sysroot" -Wl,-rpath-link,"$sysroot/lib" -Wl,-rpath-link,"$sysroot/usr/lib" "\$@"
EOF
chmod 700 "$work/cc"
export CC="$work/cc" AR="$cross-gcc-ar" RANLIB="$cross-gcc-ranlib" STRIP="$cross-strip"
export PKG_CONFIG_SYSROOT_DIR="$sysroot"
export CFLAGS="-O2 -g -flto -march=mips32r2 -ffunction-sections -fdata-sections"
export LDFLAGS="-flto -Wl,--gc-sections"
"$src/configure" --host=mipsel-rockbox-linux-gnu --prefix=/usr --sysconfdir=/etc \
 --localstatedir=/usr/data --disable-systemd --disable-udev --disable-obex --disable-cups \
 --disable-client --disable-tools --disable-monitor --disable-test --enable-midi \
 --disable-manpages --disable-testing --enable-experimental > "$out/configure.log" 2>&1
sed -i 's|#define STORAGEDIR ".*"|#define STORAGEDIR "/usr/data/bluetooth"|' config.h
make src/builtin.h > "$out/build.log" 2>&1
make -j"${JOBS:-4}" src/bluetoothd >> "$out/build.log" 2>&1
if grep -E 'warning:|error:|plugin needed' "$out/build.log"; then
 echo 'Unexpected compiler/linker diagnostic' >&2
 exit 1
fi
cp src/bluetoothd "$out/bluetoothd.debug"
"$STRIP" -o "$out/bluetoothd" src/bluetoothd
sha256sum "$out/bluetoothd" > "$out/bluetoothd.sha256"
"$abi_python" "$here/../system-libs/abicheck.py" "$stock" "$sysroot/usr/lib" "$out/bluetoothd" > "$out/abi.log"
python3 - "$here" "$src" "$work/build" "$out" "$cross" <<'PY'
from pathlib import Path
import hashlib,json,re,sys
here,source,build,out=map(Path,sys.argv[1:5])
cross=sys.argv[5]
plugins=re.findall(r'extern struct bluetooth_plugin_desc __bluetooth_builtin_(\w+);', (build/'src/builtin.h').read_text())
expected='hostname wiimote autopair policy a2dp avrcp network input hog gap scanparam deviceinfo midi battery'.split()
assert plugins==expected,plugins
assert '#define STORAGEDIR "/usr/data/bluetooth"' in (build/'config.h').read_text()
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
files=[here/'bluez-5.54-sonix.patch',build/'config.h',source/'profiles/audio/a2dp.c',source/'src/device.c',source/'src/shared/att.c',out/'bluetoothd',out/'bluetoothd.debug',Path(cross+'-gcc')]
manifest=dict(version='5.54',lto=True,optimization='O2',system_libraries_replaced=False,
    source_url='https://www.kernel.org/pub/linux/bluetooth/bluez-5.54.tar.xz',
    source_sha256=sha(source.parent/'source.tar.xz'),plugins=plugins,
    inputs={str(p):sha(p) for p in files},device_tested=False)
(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print('Daemon build/ABI/plugin closure passed. Real-device acceptance remains.')
PY
