# NetBSD 82586 drivers

Unmodified copies of NetBSD's 82586 support, fetched from
`https://raw.githubusercontent.com/NetBSD/src/trunk`.  They are here as
reference material only; they keep their original NetBSD Foundation and
Regents of the University of California licences, which are in the file
headers.

| file                  | NetBSD path                          |
|-----------------------|--------------------------------------|
| `i82586.c`            | `sys/dev/ic/i82586.c`                |
| `i82586var.h`         | `sys/dev/ic/i82586var.h`             |
| `i82586reg.h`         | `sys/dev/ic/i82586reg.h`             |
| `if_ai.c`             | `sys/dev/isa/if_ai.c`                |
| `if_aireg.h`          | `sys/dev/isa/if_aireg.h`             |
| `if_ef.c`             | `sys/dev/isa/if_ef.c`                |
| `if_efreg.h`          | `sys/dev/isa/if_efreg.h`             |
| `if_ie_obio_sun2.c`   | `sys/arch/sun2/dev/if_ie_obio.c`     |
| `if_ie_obio_sun3.c`   | `sys/arch/sun3/dev/if_ie_obio.c`     |
| `if_ie_vme_sun3.c`    | `sys/arch/sun3/dev/if_ie_vme.c`      |
| `if_ievar.h`          | `sys/arch/sun3/dev/if_ievar.h`       |
| `mii.h`               | `sys/dev/mii/mii.h`                  |

`if_aireg.h` and `if_efreg.h` are the two ISA cards' host interfaces - the I/O
register maps that `cosim/` emulates.  `ef(4)` is the one the co-simulation
uses, because `ai(4)` cannot attach on a modern kernel; see
`doc/cosimulation.md`.

`mii.h` is the odd one out: it has nothing to do with the 82586, which predates
MDIO by a decade.  It is here as the reference for `wb_mdio` and `mdio_prog`,
which exist because an FPGA build has a PHY to set up whatever the drivers
think - see "Why `mii.h` is the MDIO reference" below.

## Why this is the layout reference

`i82586reg.h` is shared by drivers on hosts of both endiannesses, and the
machine-dependent glue is where the difference is absorbed:

* `if_ai.c` drives an ISA card on a little-endian host, and its accessors are
  plain `bus_space_read_2` / `bus_space_write_2` - no swapping at all.
* `if_ie_obio_sun2.c` drives the same chip from a big-endian 68k Sun, and its
  accessors byte swap every 16-bit field on the way through.

Both therefore work in *the chip's own view* of the shared memory, which is
what the RTL sees on the Wishbone side, so `i82586reg.h` is the definition to
implement against.

## What it settled

The Sun ROM headers in `../Sun3280_ROM` and `../Sun2120_ROM` describe the
same words with different bit numbers - for instance `IECMD_CU_START` is 1
there but `IE_CUC_START` is `0x0100` here.  The ROM drivers access the shared
memory through hardware that swaps bytes, so their headers are written for the
*swapped* view.  Undo the swap and the two agree exactly, on every field:

| field              | chip's view (NetBSD) | Sun ROM header, swapped |
|--------------------|----------------------|-------------------------|
| SCB status CX      | `0x8000`             | `0x80`  -> `0x8000`     |
| SCB command CU start | `0x0100`           | `1`     -> `0x0100`     |
| SCB command RU start | `0x0010`           | `1<<12` -> `0x0010`     |
| SCB status CUS     | `[10:8]`             | byte 1 bits `[2:0]`     |
| SCB status RUS     | `[6:4]`              | byte 0 bits `[6:4]`     |

Wish82586 implements the chip's view.  A big-endian host wanting to run an
unmodified vintage driver needs the same byte swap in its bus glue that the
original machines had; it is not the MAC's job.

The one bit that is not in `i82586reg.h` is the software reset in the SCB
command word - NetBSD resets in hardware.  It comes from the ROM header's
`IECMD_RESET 0x8000`, which is bit 7 once the swap is undone.

`tb/cpp/tests/test_layout.cpp` checks our constants against the values in
`i82586reg.h` so this cannot drift again.

## Why `mii.h` is the MDIO reference

The MDIO side has the same problem as the layout did, without the same escape.
Nothing in `doc/drivers` touches a PHY, so there is no vintage driver to read:
`wb_mdio` and `tb/cpp/mdio_phy.cpp` were both written from clause 22, by the
same hand, at the same sitting.  Left alone they would only ever prove that
the station and the model made the same mistakes.

`mii.h` is the cheapest independent authority available.  It has driven every
PHY NetBSD supports for twenty-odd years, so a wrong bit position in it would
have been found long ago, and its `MII_COMMAND_*` values pin the frame itself -
the start delimiter and the two opcodes - not just the register bits.  Note that
`src/mdio_prog.sv` spells these bits with Linux's names (`BMCR_ANENABLE`) and
`mii.h` does not (`BMCR_AUTOEN`); keep it that way, because a shared name is the
easiest route for one mistake to reach both copies.

`mdio_constants_match_the_reference` in `tb/cpp/tests/test_mdio.cpp` pins
`tb/cpp/mii.h` to this file, and the programming-sequence tests pin
`mdio_prog.sv` to `tb/cpp/mii.h`, so the RTL is tied to the reference through
the testbench - the same two-step as the layout.

What none of that reaches is the timing: which edge each side drives and
samples, and where the station lets go of the wire for a read.  A model written
alongside the station tolerates whatever the station does.
`mdio_write_frame_matches_the_standard` and its read counterpart compare the
MDIO pin against a frame written out bit by bit instead, which is why they exist
as well.
