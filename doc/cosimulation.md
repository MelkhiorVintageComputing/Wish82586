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
  NetBSD/i386, unmodified               cosim/patches/   the card, in QEMU
    ef(4) driver                        cosim/rtl/       the core, in a library
      |  ISA I/O ports and shared RAM   cosim/scripts/   build, image, run
      v
  emulated 3C507 card
      |  reset, channel attention, a frame off the wire
      v
  Verilated wish82586, sharing the card's memory window by pointer
```

Any 82586 ISA card would do, as long as its NetBSD driver sits on
`sys/dev/ic/i82586.c` - the driver core this project has been checking its
shared memory layout against all along - so that a success here validates the
reading of that code rather than a second reading of something else.  NetBSD
ships three such drivers, and which one to use took some finding out.

## Which card

The obvious choice is the **AT&T StarLAN 10** and its `ai(4)` driver: sixteen
bytes of I/O of which four matter, one to reset the chip and one for channel
attention, plus a window of shared RAM.  That is almost exactly what
`wish82586` already presents, `CTRL.RST` and `CTRL.CA` standing in for the two
I/O bytes.

It cannot be used.  `ai(4)` faults the kernel before it reaches the chip:

```c
/* ai_find_mem_size(), sys/dev/isa/if_ai.c */
if (bus_space_map(memt, maddr, size, 0, &memh) == 0) {
        size = check_ie_present(sc, memt, maddr, size);
```

`check_ie_present()` takes a `bus_space_handle_t`, and gets handed `maddr`, the
ISA physical address, instead of the `memh` that `bus_space_map()` just
returned.  On i386 that handle is a kernel virtual address, so the first write
inside `check_ie_present()` lands on an unmapped page:

```
uvm_fault(0xc15ad960, 0xdf000, 2) -> 0xe
fatal page fault in supervisor mode
Stopped in pid 0.0 (system) at netbsd:bus_space_set_region_1+0x27
ai_match.part.0(...,7,360,0,d0000,0) at netbsd:ai_match.part.0+0x19e
```

Any real StarLAN 10 answering at the configured port would do the same to a
modern NetBSD/i386.  The card is long gone, so nobody has noticed.  Fixing it
is a one-line change to NetBSD, and changing NetBSD is exactly what this
exercise is not allowed to do.

So the co-simulation uses the **3Com 3C507**, driven by `ef(4)`, which sits on
the same `sys/dev/ic/i82586.c`, probes cleanly, and reads its ethernet address
off the card - something `ai(4)` does not do either, leaving `ai0` with
whatever was on the stack.  The StarLAN 10 is emulated as well, since it was
written before the fault was found and it is the simpler board; `run-cosim.py
--card starlan10` will demonstrate the panic.

## The card's host interface

From `doc/drivers/NetBSD/if_efreg.h`.  Sixteen bytes of I/O with three banks
over the first six, and a window of shared RAM:

| offset      | what                                                       |
|-------------|------------------------------------------------------------|
| I/O + 0..5  | bank 0: `*3COM*`; bank 1: ethernet address; bank 2: part number, revision |
| I/O + 6     | control: bank select, interrupt enable, reset (active low)  |
| I/O + 10    | any write clears the card's interrupt latch                 |
| I/O + 11    | any write is a channel attention                            |
| I/O + 13/14/15 | media, memory window address and size, interrupt line    |
| memory      | the shared RAM window the 82586 and the host both work in   |

Reset and channel attention are `CTRL.RST` and `CTRL.CA` in the CSR; the memory
window is what the Wishbone master already reads and writes.  Everything else
is the board's own paperwork: constants, and an interrupt latch the card holds
between the chip and the ISA bus.

The chip sees a 16 MB address space with the window at the very *top* - both
drivers compute every pointer as `offset + 2^24 - window_size` - so the SCP at
`0xfffff4` is the last twelve bytes of the window.

## Versions

QEMU **7.2**, which is what Debian 12 ships (7.2.22) and what is already
installed here.  Nothing in this plan needs anything newer: a memory-mapped
ISA device with a couple of I/O ports and an IRQ is old, stable QEMU API, and
staying on the distribution version means the from-source build can be
compared against a known-good binary of exactly the same version.

NetBSD **10.1**/i386, installed from `boot-com.iso`, the serial console
install image.  Serial rather than VGA because the install has to be driven by
a script and the whole thing has to run without a display.

The QEMU machine is `pc`, not `isapc`.  `isapc` looks like the obvious choice
for an ISA card, but its ISA IDE never presents an ATAPI CD-ROM: the installer
boots, probes, finds no root and resets, over and over.  Booting an
already-installed image there does not work either - NetBSD attaches `wdc0` and
`wdc1` and then finds no drive on them, and its MP table is bad enough that the
kernel complains about every APIC pin.  `pc` has a working IDE and still has an
ISA bus hanging off its LPC bridge, which is where the 82586 card goes, so
nothing is given up by using it.  256 MB of memory, because the install ramdisk
does not fit in 64.

Two things about that machine bite an ISA card with a memory window:

* **The parallel port owns IRQ 7.**  So does the card, in the kernel's
  configuration for it, and two edge-triggered ISA devices cannot share a line:
  `isa_intr_establish()` fails, the driver notes it silently, and the interface
  then transmits once and stops.  `-parallel none` clears it.
* **The chipset shadows the card's memory window.**  On the `pc` machine the
  i440FX PAM registers decide whether `0xc0000-0xfffff` is DRAM or the bus, and
  SeaBIOS sets the whole range to read-only DRAM before the guest OS starts.
  An ISA card claiming its window in the ISA address space is then simply not
  there: the driver's writes are dropped and its reads come back as zeros.  The
  symptom is a chip that answers `i82586_proberam()` perfectly - because the
  busy flag it is supposed to clear reads as zero whether it cleared it or not
  - and then never executes a single command.  The device model therefore
  claims its window in the system address space, over the top of the shadow,
  which is how a machine whose BIOS left that range to the bus would behave.

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
| `python3-pexpect` | drives the NetBSD installer, and `run-cosim.py`, over the serial console | Debian `python3-pexpect`, or pip into a virtualenv |
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
2. Add the card to QEMU, backed by a plain software 82586, so the driver can be
   brought up before the RTL is in the loop.  The chip is one file and the two
   boards are another each, because step 3 replaces the chip and leaves the
   boards alone.

   There is no shell-only version of this that gets an attach.  Both drivers
   gate on `i82586_proberam()`:

   ```c
   write16(SCP + 2, IE_SYSBUS_16BIT);   /* bus use byte      */
   write16(ISCP + 0, 1);                /* set the busy flag */
   hwreset();
   chan_attn();
   delay(100);
   result = read16(ISCP + 0) == 0;      /* must have cleared */
   ```

   So the device has to walk the pointer chain and clear the ISCP busy flag on
   channel attention before NetBSD will believe there is a chip there at all -
   which is the same sequence `src/ie_core.sv` already implements, read from
   the other side.  After that come the SCB command loop, the command unit, and
   the receive unit, all working in the shared window.

   Delivered as a patch against the QEMU tarball in `cosim/patches/`, since no
   QEMU source belongs in this repository.  `fetch-qemu.sh` keeps a pristine
   copy next to the working tree for exactly that.
3. Replace that software model with the Verilated `wish82586`, and check that
   `ef(4)` still attaches, sees its address, and passes packets.

All three are done.  `cosim/scripts/` builds QEMU 7.2.22 from source with the
card patched in, installs NetBSD 10.1/i386 over the network into
`work/images/`, and `run-cosim.py` boots it and checks the result - with
`--rtl`, against the RTL:

```
ef0 at isa0 port 0x360-0x36f iomem 0xd0000-0xdffff irq 7
    address 52:54:00:12:34:56, type 3C507-TP, rev. 10
...
4 packets transmitted, 4 packets received, 0.0% packet loss
Name  Mtu   Network    Address            Ipkts Ierrs  Opkts Oerrs Colls
ef0   1500  <Link>     52:54:00:12:34:56     12     0     19     0     0
```

That is an unmodified NetBSD driver initialising the chip, configuring it,
setting its address, starting the receive unit, transmitting ARP and ICMP and
receiving the replies, with no errors counted on either side.

## How the RTL gets into the loop

`cosim/rtl/` builds the Verilated core into a shared library with a handful of
C entry points, and the card in QEMU loads it by path:

```sh
make -C cosim/rtl              # work/lib/libwish82586rtl.so
make -C cosim/rtl check        # drive it the way the driver does, no QEMU
cosim/scripts/run-cosim.py --rtl
```

Nothing about Verilator or C++ reaches QEMU: `hw/net/i82586_rtl.c` is fifty
lines of `dlopen` and four forwarding calls, which are the same four the
software model implements.  What crosses the boundary is the memory window,
shared by pointer, so the guest's writes and the core's Wishbone master really
are the same buffer - no copying, and nothing to keep in step.

Inside the library it is the regression's own models doing their usual jobs:
`WbMem` over that window, `WbHost` on the control registers turning the card's
two I/O bytes into `CTRL.RST` and `CTRL.CA`, and `MiiPhy` on the wire.

The one thing that is unlike hardware is *when* the core runs.  A real card
works alongside the host; here the simulation is stepped only when the guest
touches the card, and far enough that whatever it was asked to do is finished
before the I/O write returns.  Every driver in `doc/drivers` polls the SCB or
waits for the interrupt afterwards, so none of them can tell the difference,
and the guest never sees a half-written structure in shared memory.  What it
costs is visible in the ping times: about 1.5 ms round trip through the
software model, 3 to 6 ms through the RTL.

`make -C cosim/rtl check` is the thing to run first when the guest misbehaves.
It writes the same structures at the same offsets that `ef_attach()` and
`i82586_init()` write, using nothing but the C interface the card uses, so if
it passes and the guest still does not work, the fault is on the QEMU side.

## What was learned

Three things worth keeping:

* A process that is alive, writing to its log and growing its output file can
  still be going in circles.  The installer script installed NetBSD three times
  over while every check said it was healthy.  Counting how often the same
  screen has been answered is what catches it, and the loop now refuses to
  answer any one screen more than a handful of times.
* A device that answers the probe is not a device that works.  The 82586's
  probe asks the chip to *clear* a flag, so a memory window that reads back as
  zeros passes it perfectly while nothing at all is connected.  Every negative
  check of this shape wants a positive one next to it.
* Writing the software 82586 first was worth it.  It is scaffolding and it is
  now redundant, but it was what turned "the guest does not work" into a
  question with one unknown at a time: the ISA glue, the memory mapping, the
  interrupt path and the driver's expectations were all settled and working
  before the RTL was asked to do anything.
