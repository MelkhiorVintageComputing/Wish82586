# Co-simulation

Checking the MAC against the drivers themselves rather than against a model of
what they do.  `doc/cosimulation.md` explains the plan and the reasoning; this
is how to run it.

None of this is part of `make test`.  It is a separate, much slower loop.

## Rules this directory follows

* No QEMU or NetBSD source, binaries or images in git.  They go in `work/`,
  which is ignored, and these scripts are what put them back.
* Changes to QEMU live in `patches/` as patches against the release tarball,
  never as a copy of the modified tree.
* NetBSD is never modified.  The whole point is that an unmodified driver
  talks to this MAC.
* **These scripts never install system packages.**  `check-deps.sh` says what
  is missing and prints the command; running it is the machine owner's
  decision, not this repository's.

## Order

```sh
cosim/scripts/check-deps.sh      # what is missing, and the apt line for it
cosim/scripts/fetch-qemu.sh      # download, verify, unpack, apply patches/
cosim/scripts/build-qemu.sh      # i386 only, into work/qemu-install
cosim/scripts/fetch-netbsd.sh    # the serial console install ISO
cosim/scripts/make-image.py      # drive sysinst over serial into a disk image
cosim/scripts/boot-netbsd.sh     # boot what came out
```

## Regenerating a patch

The fetch script keeps a pristine copy of the source next to the working one,
so a change made in `work/qemu-src` can be turned back into a patch:

```sh
cd work && diff -urN qemu-src.orig qemu-src > ../cosim/patches/0001-whatever.patch
```
