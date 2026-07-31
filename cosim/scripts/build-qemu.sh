#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Configure and build QEMU.  Only the i386 machine is built: that is the one
# with an ISA bus to hang an 82586 card off, and building the rest would cost
# many minutes per iteration for nothing.
source "$(dirname "$0")/common.sh"

[ -d "$QEMU_SRC" ] || die "no source; run fetch-qemu.sh first"

mkdir -p "$QEMU_BUILD"
cd "$QEMU_BUILD"

if [ ! -f build.ninja ]; then
    say "configuring QEMU $QEMU_VERSION"
    opts=(
        --target-list=i386-softmmu
        --prefix="$QEMU_PREFIX"
        --disable-werror
        # Nothing here needs a display, and leaving them out keeps the
        # dependency list short.
        --disable-gtk --disable-sdl --disable-vnc
        --disable-docs --disable-guest-agent
        --disable-tools
    )
    if pkg-config --exists libslirp; then
        opts+=(--enable-slirp)
    else
        warn "no libslirp: this build will have no -netdev user"
        opts+=(--disable-slirp)
    fi
    "$QEMU_SRC/configure" "${opts[@]}"
fi

say "building with $(nproc) jobs"
ninja
say "installing into $QEMU_PREFIX"
ninja install

"$QEMU_PREFIX/bin/qemu-system-i386" --version | head -1
say "built $(qemu_bin)"
