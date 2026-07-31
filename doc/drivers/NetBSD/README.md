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
| `if_ie_obio_sun2.c`   | `sys/arch/sun2/dev/if_ie_obio.c`     |
| `if_ie_obio_sun3.c`   | `sys/arch/sun3/dev/if_ie_obio.c`     |
| `if_ie_vme_sun3.c`    | `sys/arch/sun3/dev/if_ie_vme.c`      |
| `if_ievar.h`          | `sys/arch/sun3/dev/if_ievar.h`       |
| `if_aireg.h`          | `sys/dev/isa/if_aireg.h`             |

`if_aireg.h` is the StarLAN 10 card's host interface - the I/O register map
that `cosim/` has to emulate.  See `doc/cosimulation.md`.

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
