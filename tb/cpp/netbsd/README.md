# Just enough kernel to compile one NetBSD file

`doc/drivers/NetBSD/mii_bitbang.c` is compiled into the testbench unmodified,
so that the MDIO frames our PHY model is checked against come from an
implementation nobody here wrote.  See `tb/cpp/netbsd_station.h` for why, and
`doc/drivers/NetBSD/README.md` for what it does and does not prove.

Unmodified means it still asks for the kernel headers it was written against:

```c
#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/device.h>
#include <dev/mii/mii.h>
#include <dev/mii/mii_bitbang.h>
```

These are those five headers, cut down to what that one file uses - three
typedefs, one empty macro and one function declaration between them.  The two
under `dev/mii/` just forward to the vendored copies, so there is one authority
for the MII constants and it is the one `mdio_constants_match_the_reference`
pins us to.

This tree is on the include path for `mii_bitbang.c` alone (see `NETBSD_CFLAGS`
in the Makefile) and must stay that way: it is not a compatibility layer, and
nothing else in `tb/` may include from it.  If a second NetBSD file ever earns
its place here, extend these rather than starting a parallel set - but weigh it
first, because a shim that grows starts to be a thing that can be wrong.
