# SPDX-License-Identifier: MIT
# Shared settings for the co-simulation scripts.  Sourced, not run.

set -euo pipefail

COSIM_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT_DIR="$(cd "$COSIM_DIR/.." && pwd)"

# Everything downloaded, built or generated lands here.  None of it is in git:
# these scripts are what put it back.
WORK="${WORK:-$ROOT_DIR/work}"
DOWNLOADS="$WORK/downloads"
QEMU_SRC="$WORK/qemu-src"
QEMU_BUILD="$WORK/qemu-build"
QEMU_PREFIX="$WORK/qemu-install"
IMAGES="$WORK/images"

# The version Debian 12 ships, so the build from source can be compared
# against a known good binary of exactly the same thing.
QEMU_VERSION="7.2.22"
QEMU_TARBALL="qemu-$QEMU_VERSION.tar.xz"
QEMU_URL="https://download.qemu.org/$QEMU_TARBALL"
# Upstream publishes only a detached GPG signature next to the tarball, no
# plain checksum.  fetch-qemu.sh verifies the signature when a trusted key is
# available; this pin is what stops a re-download quietly turning into
# something else in the meantime.
QEMU_TARBALL_SHA512="c4ca21ab13d73d58f220d6059b404aad7ee3247bd26a395bb58f7907ebd796946e923d01831f12a5781772c6d493082e6df40465aa98a215ce530dbed8aac09b"

NETBSD_VERSION="10.1"
NETBSD_ARCH="i386"
NETBSD_ISO="NetBSD-$NETBSD_VERSION-$NETBSD_ARCH-boot-com.iso"
NETBSD_ISO_URL="https://cdn.netbsd.org/pub/NetBSD/NetBSD-$NETBSD_VERSION/$NETBSD_ARCH/installation/cdrom/boot-com.iso"
NETBSD_ISO_SHA512="2b8e78cb90c87b4bd3300e0c39617d2f70b4148a209df0a6656999a5bdbd25fd2c9aabd06ee0500e1f62fa82aee9994e4ae0200d9f9ac153521f327948a96cbd"

DISK_IMAGE="$IMAGES/netbsd-$NETBSD_VERSION-$NETBSD_ARCH.qcow2"
DISK_SIZE="${DISK_SIZE:-4G}"

# The QEMU we built, falling back to whatever is on the path so the baseline
# can be checked before the source build exists.
qemu_bin() {
    if [ -x "$QEMU_PREFIX/bin/qemu-system-i386" ]; then
        echo "$QEMU_PREFIX/bin/qemu-system-i386"
    else
        command -v qemu-system-i386
    fi
}

say()  { printf '\033[36m==\033[0m %s\n' "$*"; }
warn() { printf '\033[33m!!\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[31m!!\033[0m %s\n' "$*" >&2; exit 1; }

mkdir -p "$DOWNLOADS" "$IMAGES"
