#ifndef RECOVERY_R1_H
#define RECOVERY_R1_H

// One reviewed HiBy R1 recovery image and its read-only card-mount repair.
// These hashes cover the complete partition, including its erased padding.
// The block hashes cover the entire physical eraseblock, not just the LZO data.
#define R1_RECOVERY_DEVICE "/dev/mtd4"
#define R1_RECOVERY_SIZE 25165824u
#define R1_RECOVERY_ERASE_SIZE 131072u
#define R1_RECOVERY_WRITE_SIZE 2048u
#define R1_RECOVERY_BLOCK_OFFSET 4456448u
#define R1_RECOVERY_OLD_SHA256 "458f3d0f2366ee992f30b3e611f463807b6f0ec6dd469b69e9a6ccd461ea6650"
#define R1_RECOVERY_NEW_SHA256 "259b7d112b0bb49277dbf2dc9008411ddd839563601d17169dd97a428b8e56a0"
#define R1_RECOVERY_OLD_BLOCK_SHA256 "f39d7d16f18633c21ec8f461dce2cbd25c958496e5c91d70d3a622586a3b027d"
#define R1_RECOVERY_NEW_BLOCK_SHA256 "b347d0c257c74b77081c3e82998651af1426cbdbf5ea91aebc040e2212ffd091"

#endif
