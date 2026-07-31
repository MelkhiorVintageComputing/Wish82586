#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Boot the installed image on the serial console.  Ctrl-a x quits QEMU.
source "$(dirname "$0")/common.sh"

[ -f "$DISK_IMAGE" ] || die "no image at $DISK_IMAGE; run make-image.py first"

qemu="$(qemu_bin)"
say "booting $DISK_IMAGE with $qemu"

exec "$qemu" \
    -M pc \
    -m 256 \
    -drive "file=$DISK_IMAGE,format=qcow2,if=ide,index=0" \
    -boot c \
    -nographic \
    -serial mon:stdio \
    "$@"
