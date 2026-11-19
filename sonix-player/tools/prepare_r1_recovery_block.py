#!/usr/bin/env python3
"""Derive the reviewed R1 repair block from an ordinary recovery backup.
Reads host files only. Never writes a device, full recovery image or input file.
"""
import argparse
import hashlib
import os
from pathlib import Path
import stat

IMAGE_SIZE = 25165824
BLOCK_OFFSET = 4456448
BLOCK_SIZE = 131072
OLD_IMAGE = "458f3d0f2366ee992f30b3e611f463807b6f0ec6dd469b69e9a6ccd461ea6650"
NEW_IMAGE = "259b7d112b0bb49277dbf2dc9008411ddd839563601d17169dd97a428b8e56a0"
OLD_BLOCK = "f39d7d16f18633c21ec8f461dce2cbd25c958496e5c91d70d3a622586a3b027d"
NEW_BLOCK = "b347d0c257c74b77081c3e82998651af1426cbdbf5ea91aebc040e2212ffd091"
# Offset and expected/replacement bytes only; no proprietary firmware payload.
DELTA = "1ade9:80:81 1adeb:05:20 1adec:20:0c 1aded:6f:15 1adee:70:02 1aef5:4b:49 1aef8:61:08 1aef9:64:4a 1af57:77:20 1af66:54:55 1af68:0b:69 1af69:69:00 1af6a:73:07 1af6b:20:08 1af87:74:76 1af89:07:20 1af8a:20:28 1af8b:28:00 1af8c:25:11 1af8d:73:03 1af97:17:15 1af9a:00:0c 1af9b:00:0c 1afcb:4f:4d 1afce:69:04 1afcf:73:2a 1aff2:6f:6d 1aff5:65:0c 1aff6:6d:87 1b071:07:05 1b074:6d:04 1b075:65:07 1b081:03:01 1b084:20:0c 1b085:2d:01 1b0e3:0c:0e 1b0e5:06:61 1b0e6:61:5f 1b0e7:5f:00 1b0e8:63:5b 1b0e9:6f:02 1b100:5f:5d 1b103:73:0c 1b104:20:4c 1b106:08:09 1b108:04:76 1b109:76:0c 1b10a:25:09 1b10b:64:01 1b1b6:0f:0d 1b1b9:61:00 1b1ba:6c:4c 1b30f:9f:9d 1b312:e9:00 1b313:e9:00 1b315:ff:fd 1b318:9c:00 1b319:9c:00 1b31b:9f:9d 1b31e:7d:00 1b31f:7d:00 1b321:8f:8d 1b324:dd:00 1b325:dd:00 1b331:03:01 1b334:b8:00 1b335:b8:00 1b358:7f:7d 1b35b:6b:00 1b35c:6b:00 1b396:ff:fd 1b399:aa:00 1b39a:aa:00"

def digest(data):
    return hashlib.sha256(data).hexdigest()

def ordinary(path, output=False):
    absolute = str(path.resolve()).replace("\\", "/").lower()
    if path.is_symlink() or absolute.startswith(("/dev/", "/proc/", "/sys/", "//./", "//?/")):
        raise ValueError("Only ordinary host paths are accepted")
    if not output and not stat.S_ISREG(path.stat().st_mode):
        raise ValueError("Input must be a regular host file")

def prepare(image):
    if len(image) != IMAGE_SIZE or digest(image) != OLD_IMAGE:
        raise ValueError("Unrecognized original recovery backup")
    block = bytearray(image[BLOCK_OFFSET:BLOCK_OFFSET + BLOCK_SIZE])
    if digest(block) != OLD_BLOCK:
        raise ValueError("Original block hash mismatch")
    for change in DELTA.split():
        offset, expected, replacement = (int(part, 16) for part in change.split(":"))
        if block[offset] != expected:
            raise ValueError("Original byte mismatch")
        block[offset] = replacement
    if digest(block) != NEW_BLOCK:
        raise ValueError("Replacement block hash mismatch")
    candidate = image[:BLOCK_OFFSET] + block + image[BLOCK_OFFSET + BLOCK_SIZE:]
    if digest(candidate) != NEW_IMAGE:
        raise ValueError("Complete candidate hash mismatch")
    return block

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("backup", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    ordinary(args.backup)
    ordinary(args.output, output=True)
    block = prepare(args.backup.read_bytes())
    with args.output.open("xb") as target:
        target.write(block)
        target.flush()
        os.fsync(target.fileno())
    if digest(args.output.read_bytes()) != NEW_BLOCK:
        raise ValueError("Output readback mismatch")
    print("Verified private R1 repair block:", args.output)

if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        raise SystemExit("STOP: " + str(error))
