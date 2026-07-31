# Co-simulation with QEMU

The Verilator testbench checks the MAC against a model of what the drivers do.
Co-simulation checks it against the drivers themselves: a real NetBSD, booting
on a real emulated machine, finding the card and using it.  What the testbench
cannot tell us is whether our reading of the drivers was right, and that is
exactly what this is for.

Nothing here is built by `make test`; it is a separate, much slower loop.

## What runs where

```
QEMU i386 machine                       this repository
  NetBSD/i386, unmodified               cosim/qemu/          device model
    ai(4) driver                        cosim/scripts/       build and image
      |  ISA I/O ports and shared RAM   cosim/patches/       QEMU changes
      v
  emulated StarLAN 10 card  <------>  Verilated wish82586
```

The card being emulated is the AT&T StarLAN 10, which NetBSD drives with
`ai(4)`.  It was chosen over the other 82586 ISA cards because:

* `ai(4)` sits on `sys/dev/ic/i82586.c`, the same driver core this project has
  been checking its shared memory layout against all along, so a success here
  validates the reading of that code rather than a second reading of something
  else.
* Its host interface is a window of shared RAM plus a couple of I/O bits for
  reset and channel attention, which is close to what `wish82586` already
  presents: memory on the Wishbone master, RESET and CA in the CSR.

## Versions

QEMU **7.2**, which is what Debian 12 ships (7.2.22) and what is already
installed here.  Nothing in this plan needs anything newer: a memory-mapped
ISA device with a couple of I/O ports and an IRQ is old, stable QEMU API, and
staying on the distribution version means the from-source build can be
compared against a known-good binary of exactly the same version.

NetBSD **10.1**/i386, installed from `boot-com.iso`, the serial console
install image.  Serial rather than VGA because the install has to be driven by
a script and the whole thing has to run without a display.

## What is and is not in git

In git: the scripts that build QEMU and create the disk image, any patch
against QEMU source, the device model, and this document.

Not in git: QEMU source or binaries, NetBSD source, binaries or ISOs, and the
disk image.  All of that lands in `work/`, which is ignored.  Re-creating it
is what the scripts are for.

NetBSD is never modified.  The entire point is that an unmodified driver, from
a distribution nobody involved here controls, talks to this MAC.

## Dependencies

Already present on this machine: `ninja`, `pkg-config`, `gcc`, `g++`, `make`,
`python3` (with `venv`), `git`, `flex`, `bison`, `curl`, `libglib2.0-dev`,
`libpixman-1-dev`, `zlib1g-dev`.

Needed and not present:

| what              | why                                                        | how |
|-------------------|------------------------------------------------------------|-----|
| `meson`           | QEMU 7.2 will not configure without it, and a release tarball has no bundled copy | Debian `meson`, or pip into a virtualenv |
| `python3-pexpect` | drives the NetBSD installer over the serial console         | Debian `python3-pexpect`, or pip into a virtualenv |
| `libslirp-dev`    | QEMU 7.2 dropped its bundled slirp, so a from-source build has no `-netdev user` without it.  Only needed to prove the card passes traffic to the outside; the driver finding and attaching to the card does not need it | Debian package only |

`meson` and `pexpect` can both be had without touching the system, by creating
a virtualenv under `work/`.  `libslirp-dev` cannot: it is a C library QEMU
links against.

Installing Debian packages is not something this repository's tooling does on
its own - see the note at the top of `cosim/scripts/`.

## Order of work

1. Build QEMU 7.2 from source and check it against the packaged one, then
   install NetBSD to a disk image and boot it.  Nothing custom yet: this is
   the baseline that everything later is compared against.
2. Add the StarLAN 10 device model to QEMU, backed by a plain software 82586
   so the driver can be brought up before the RTL is in the loop.
3. Replace that software model with the Verilated `wish82586`, and check that
   `ai(4)` still attaches, sees its address, and passes packets.

Step 1 is where this document currently stops.
