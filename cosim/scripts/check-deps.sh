#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Says what is missing and what to install.  It never installs anything: the
# packages are the machine owner's business, not this repository's.
source "$(dirname "$0")/common.sh"

missing_pkgs=()
ok=0
need_cmd() {   # command, package, why
    printf '  %-18s ' "$1"
    if command -v "$1" >/dev/null 2>&1; then echo "ok"; ok=$((ok+1))
    else echo "MISSING   ($3)"; missing_pkgs+=("$2"); fi
}
need_pc() {    # pkg-config name, package, why
    printf '  %-18s ' "$1"
    if pkg-config --exists "$1" 2>/dev/null; then echo "ok $(pkg-config --modversion "$1")"; ok=$((ok+1))
    else echo "MISSING   ($3)"; missing_pkgs+=("$2"); fi
}
need_py() {    # module, package, why
    printf '  %-18s ' "python3 $1"
    if python3 -c "import $1" 2>/dev/null; then echo "ok"; ok=$((ok+1))
    else echo "MISSING   ($3)"; missing_pkgs+=("$2"); fi
}

echo "Build tools:"
need_cmd gcc          gcc            "compiling QEMU"
need_cmd g++          g++            "compiling QEMU"
need_cmd ninja        ninja-build    "QEMU's build backend"
need_cmd meson        meson          "QEMU will not configure without it"
need_cmd pkg-config   pkg-config     "finding QEMU's dependencies"
need_cmd python3      python3        "QEMU's build scripts"
need_cmd curl         curl           "fetching sources"

echo "Libraries:"
need_pc  glib-2.0     libglib2.0-dev "QEMU core"
need_pc  pixman-1     libpixman-1-dev "QEMU display"
need_pc  zlib         zlib1g-dev     "QEMU core"
need_pc  slirp        libslirp-dev   "user mode networking; QEMU 7.2 no longer bundles it"

echo "Python:"
need_py  venv         python3-venv   "QEMU's build scripts"
need_py  pexpect      python3-pexpect "driving the NetBSD installer over serial"

echo
if [ ${#missing_pkgs[@]} -eq 0 ]; then
    say "everything needed is present"
    exit 0
fi

# Deduplicate while keeping the order.
uniq_pkgs=$(printf '%s\n' "${missing_pkgs[@]}" | awk '!seen[$0]++' | tr '\n' ' ')
warn "missing: $uniq_pkgs"
echo
echo "To install them:"
echo
echo "    sudo apt install $uniq_pkgs"
echo
echo "doc/cosimulation.md explains what each one is for."
exit 1
