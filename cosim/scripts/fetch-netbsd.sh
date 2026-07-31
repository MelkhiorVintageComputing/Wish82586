#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Fetch the NetBSD serial console install image.  Serial because the whole
# install is driven by a script with no display anywhere.
source "$(dirname "$0")/common.sh"

iso="$DOWNLOADS/$NETBSD_ISO"
if [ ! -f "$iso" ]; then
    say "downloading $NETBSD_ISO"
    curl -fSL --progress-bar -o "$iso.part" "$NETBSD_ISO_URL"
    mv "$iso.part" "$iso"
fi

say "verifying against NetBSD's published checksum"
have=$(sha512sum "$iso" | awk '{print $1}')
[ "$have" = "$NETBSD_ISO_SHA512" ] || die "checksum mismatch on $NETBSD_ISO"
echo "  sha512 ok"
say "ready: $iso"
