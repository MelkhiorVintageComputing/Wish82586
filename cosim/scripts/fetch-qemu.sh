#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Fetch the QEMU source and apply whatever patches this repository carries.
# The source is not in git; the patches are.
source "$(dirname "$0")/common.sh"

tarball="$DOWNLOADS/$QEMU_TARBALL"
if [ ! -f "$tarball" ]; then
    say "downloading $QEMU_TARBALL"
    curl -fSL --progress-bar -o "$tarball.part" "$QEMU_URL"
    mv "$tarball.part" "$tarball"
fi

say "verifying the download"
have=$(sha512sum "$tarball" | awk '{print $1}')
[ "$have" = "$QEMU_TARBALL_SHA512" ] || \
    die "sha512 mismatch on $QEMU_TARBALL: got $have"
echo "  sha512 matches the pin in common.sh"

# Upstream signs the tarball but publishes no checksum file, so the pin above
# is only as good as the first download unless the signature is checked too.
# That needs QEMU's release key in a keyring, which is not something these
# scripts will go and fetch on their own.
sig="$DOWNLOADS/$QEMU_TARBALL.sig"
[ -f "$sig" ] || curl -fsSL -o "$sig" "$QEMU_URL.sig" || true
if [ -f "$sig" ] && command -v gpg >/dev/null; then
    if gpg --verify "$sig" "$tarball" >/dev/null 2>&1; then
        echo "  gpg signature ok"
    else
        warn "gpg could not verify the signature; QEMU's release key is"
        warn "probably not in your keyring.  See doc/cosimulation.md."
    fi
fi

if [ -d "$QEMU_SRC" ]; then
    say "source tree already unpacked; delete $QEMU_SRC to start over"
else
    say "unpacking"
    mkdir -p "$QEMU_SRC"
    tar -xf "$tarball" -C "$QEMU_SRC" --strip-components=1
    # Keep a pristine copy so patches can be regenerated as a diff later.
    say "keeping a pristine copy for generating patches"
    cp -a "$QEMU_SRC" "$QEMU_SRC.orig"
fi

shopt -s nullglob
patches=("$COSIM_DIR/patches"/*.patch)
if [ ${#patches[@]} -eq 0 ]; then
    say "no patches to apply yet"
else
    for p in "${patches[@]}"; do
        say "applying $(basename "$p")"
        patch -d "$QEMU_SRC" -p1 --forward --silent < "$p" || \
            die "patch $(basename "$p") did not apply"
    done
fi

say "source ready in $QEMU_SRC"
