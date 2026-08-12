# Wish82586 interface contract

This is what the testbench in `tb/` assumes and what the RTL in `src/` has to
provide.  Changing anything here means changing both sides.

## What is the MAC and what is the machine around it

The real 82586 has no software-visible registers.  It has RESET, CA and INT as
pins, an interrupt, and its 24-bit view of memory; everything a driver writes
to "the chip" is really a register the machine's designer put in front of those
pins.  Every machine did it differently - `doc/sun2_ethernet.pdf` is the
Sun-2's, a single byte with `RESET*` and `LOOPB*` active low, CA at bit 5, and
a bus error bit that inhibits the channel until reset clears it.

`wish82586` follows the part: those signals are pins.

| module          | what it is                                              |
|-----------------|---------------------------------------------------------|
| `wish82586`     | the MAC.  Host side is RESET, CA, INT and the SCP address as pins, plus the Wishbone master |
| `wb_csr`        | the register block this document defines, driving those pins from a Wishbone slave port |
| `wb_csr_sun2`   | the Sun-2's register block instead, for recreating that machine |
| `wish82586_wb`  | `wb_csr` and the MAC wired together - the default, and what `tb/` and `cosim/` drive |

Recreating a machine means writing its register block and instantiating
`wish82586` beside it instead of using `wish82586_wb`.  Nothing in the MAC has
to change for that, which is the point of the split.  `wb_csr_sun2` is the
worked example; see below.

### The core's host pins

| signal      | direction | notes                                             |
|-------------|-----------|---------------------------------------------------|
| `core_rst_i`| in        | level; holds the core in reset                     |
| `ca_i`      | in        | channel attention, one `clk` cycle                 |
| `scp_addr_i`| in        | byte address of the System Configuration Pointer   |
| `cus_o`     | out       | command unit status, as it appears in the SCB      |
| `rus_o`     | out       | receive unit status, likewise                      |
| `busy_o`    | out       | the core is working on something                   |
| `int_o`     | out       | level; an unacknowledged SCB status bit            |
| `bus_err_o` | out       | one cycle per shared-memory access answered `ERR`  |

`int_o` is ungated: masking it is the register block's job, which is what
`wb_csr` does with `CTRL.IRQ_EN`.  `bus_err_o` has no consumer in `wb_csr` -
Wishbone leaves error reporting to the master's own `ERR` line - and exists for
register blocks like the Sun-2's that report it to software.

## Clocks and reset

| signal   | direction | notes                                              |
|----------|-----------|----------------------------------------------------|
| `clk`    | in        | system / Wishbone clock, 50 MHz in simulation       |
| `rst`    | in        | Wishbone `RST_I`, synchronous, active high          |

MII brings its own clocks (`mii_tx_clk`, `mii_rx_clk`, 25 MHz at 100 Mb/s).
They are asynchronous to `clk` and to each other; the testbench deliberately
skews them so the design cannot assume otherwise.

## Addressing

Wishbone is word addressed.  `ADR` carries the index of a data-bus-wide word,
not a byte address, and which bytes of that word are meant is said entirely by
`SEL`.  The bottom two bits of a byte address are therefore not on the bus at
all: a 32-bit data port covering a four gigabyte range has **30** address
lines, not 32.

Both ports follow that.  The register map and the memory offsets below are
written in byte terms, because that is how the 82586's structures and its
drivers are defined; divide by four to get what travels on `ADR`.

## Wishbone B4 classic slave - control registers

`wb_csr`, and so `wish82586_wb`.  6-bit address (a register index), 32-bit
data, byte selects honoured.
`ACK_O` comes back the cycle after the request; `ERR_O` is never asserted.

| offset | name       | access | contents                                            |
|--------|------------|--------|-----------------------------------------------------|
| `0x00` | `CTRL`     | rw     | `[0]` RST, `[1]` CA, `[8]` IRQ_EN                   |
| `0x04` | `STATUS`   | ro     | `[0]` INT, `[1]` BUSY, `[6:4]` CUS, `[10:8]` RUS    |
| `0x08` | `SCP_ADDR` | rw     | byte address of the System Configuration Pointer     |
| `0x10` | `ID`       | ro     | `0x82586001`                                        |

* `CTRL.RST` is a level, not a pulse, and reads back.  It is **set** after
  power-on reset, so the host has to clear it to let the chip run - the same
  shape as the `obie_noreset` bit the Sun driver toggles.
* `CTRL.CA` is write-1-to-pulse and always reads back as zero.  It is the
  channel attention pin of the real part.
* `SCP_ADDR` replaces the 82586's hard-wired `0xFFFFF6`.  It resets to
  `0x00FFFFF6` so a faithful vintage memory map works unchanged, and can be
  moved anywhere for an FPGA SoC.

Unmapped addresses read as zero and swallow writes.

## The Sun-2 control register - `wb_csr_sun2`

What the same three pins looked like on a machine that shipped, from
`doc/sun2_ethernet.pdf` and `doc/drivers/Sun2120_ROM/if_obie.h`, which agree
bit for bit.  One byte at word offset 0, byte lane 0:

| bit | name     | access | contents                                       |
|-----|----------|--------|------------------------------------------------|
| 7   | `RESET*` | rw     | 0 resets the 82586 (`obie_noreset`)            |
| 6   | `LOOPB*` | rw     | 0 selects transceiver loopback (`obie_noloop`) |
| 5   | `CA`     | rw     | channel attention (`obie_ca`)                  |
| 4   | `INTEN`  | rw     | interrupt enable (`obie_ie`)                   |
| 3   |          | ro     | unused                                          |
| 2   |          | ro     | transceiver level; not modelled, reads 0        |
| 1   | `ERR`    | ro     | the DMA took a bus error (`obie_buserr`)        |
| 0   | `INT`    | ro     | the 82586 wants service (`obie_intr`)           |

Nothing here matches `wb_csr`: different bit positions, both level signals
active low, and a bus error bit.  The byte is cleared by reset, so the chip
comes up held in reset *and* in loopback and the driver has to let it out -
`*obie = obie_reset` then `obie->obie_noreset = 1` in `if_ie.c`.

Three things are not obvious from the register map:

* **CA is a level, not a write-1-to-pulse bit.**  `ieca()` writes it set and
  then clear.  The 82586 latches channel attention on the rising edge of the
  pin, so that is what the module generates: one `ca_i` cycle per 0 -> 1
  transition.  Written set twice without being cleared in between, it is one
  channel attention, as on the hardware.
* **`ERR` can only be cleared by asserting `RESET*`.**  The manual says so and
  `if_obie.h` says the driver must poll for it.  It is latched from the core's
  `bus_err_o`.
* **`LOOPB*` is not the MAC's.**  On the Sun-2 it isolates the transceiver,
  which is why the driver stays in loopback across initialisation - "the chip
  does random things if its 'wire' is active between the time it's reset and
  the first CA".  The module carries the bit and brings it out as
  `loopback_o` for the system to wire to its PHY; the MAC never sees it.  It
  is not the 82586's own internal loopback, which is a CONFIGURE bit and works
  already.

The SCP address is a parameter, not a register: the real part has it wired to
`0xFFFFF6` and the Sun-2 has no way to move it.  The ROM driver instead
re-maps the page at `0xFFFFF6` onto its own memory for as long as the chip
needs to read the SCP, and puts the mapping back afterwards.

This is the register block and nothing else.  A working Sun-2 also needs the
byte swap that machine had between the CPU and the shared memory - see
`doc/drivers/NetBSD/README.md` - and whatever plays the part of its MMU.  Those
are the system's, not the MAC's.

## Wishbone B4 classic master - shared memory

30-bit word address, 32-bit data, `SEL_O` for byte lanes, classic
(non-pipelined) cycles.  The core is the only master modelled; the testbench memory answers
with a configurable number of wait states and can be made to return `ERR_I` in
a chosen address window.

The 82586 view of memory is little endian: 16-bit control fields, 24-bit
buffer addresses, control blocks addressed as 16-bit offsets from CBBASE.  How
that maps onto 32-bit bus cycles is up to the core; the testbench only checks
the resulting memory contents and that no access lands outside the model.

`WB_ADDR_W` is the number of word address lines and defaults to 30; the core
uses the bottom 22 of them, which is the 24-bit space the 82586 can reach.

The master port is 32 bits wide and stays that way.  A host that needs a
narrower bus is served by a width adapter in the Wishbone fabric, which is
where that conversion belongs - the MAC has no business knowing how wide the
machine it is plugged into happens to be.  `WB_DATA_W` exists to size the
ports and is checked at elaboration; it is not a knob.

## Interrupt

`irq_o` on `wish82586_wb` is a level, asserted while `CTRL.IRQ_EN` is set and
the core has an unacknowledged SCB status bit (CX, FR, CNA, RNR).
Acknowledging through the SCB command word clears it.  The core's own `int_o`
is the same signal without the enable.

## MII and GMII

`PHY_DATA_W` selects the interface: 4 for MII, 8 for GMII.  It is an
elaboration parameter, not a runtime one, so a build is one or the other; the
regression runs both (`make test-all`).

The framing is the same either way - seven bytes of 0x55, the 0xD5 delimiter,
the frame, the FCS - and only the width of a symbol changes.  On MII a byte
crosses as two nibbles, low nibble first; on GMII it crosses whole.

With GMII the transmit clock is sourced by the MAC rather than the PHY.  The
core still takes `mii_tx_clk` as an input: feed it from the fabric's 125 MHz
and drive the PHY's GTX_CLK from the same place.  Half duplex, carrier
extension and frame bursting are not implemented; gigabit is full duplex here.

### Keeping up with the wire

Frame data moves between memory and the MAC a word at a time.  Bytes land in
their own lane of an accumulator in the receive unit and the word is posted as
soon as the last lane fills, so the bus transaction overlaps the next four
bytes coming out of the FIFO.  Partial words - an unaligned buffer start, or
the tail of a frame - go out as one transaction with the byte lanes that were
actually filled, so it is always one transaction per word of buffer touched.

What that costs, per byte of frame:

| interface  | byte arrives every | bus clock needed          |
|------------|--------------------|---------------------------|
| MII 10     | 800 ns             | anything                  |
| MII 100    | 80 ns              | 50 MHz is comfortable     |
| GMII       | 8 ns               | 125 MHz, with no headroom |

At gigabit the accumulator has to take a byte every clock and the posted write
has to complete in four, which is exactly what a 125 MHz Wishbone gives.  The
`sys_period_ps` in the testbench environment follows that.

The receive FIFO is 256 entries.  That is not about the sustained rate, which
the word-wide path settles: it is to ride out the pause while the receive unit
closes one buffer and fetches the next descriptor.  With small buffers at
gigabit those pauses are what decide whether a frame survives.

Transmit never had the same problem, because the command unit stages a whole
frame before the transmitter starts and so has no real time constraint.  It
reads in words too, though, dropping to bytes only for an unaligned buffer
start or a short tail: the bus is shared with the receive unit, which does
have a constraint, and a frame being staged used to take one transaction per
byte of it.

Standard MII, PHY-sourced clocks:

```
mii_tx_clk  in    mii_rx_clk  in
mii_txd[3:0] out  mii_rxd[3:0] in
mii_tx_en   out   mii_rx_dv   in
mii_tx_er   out   mii_rx_er   in
                  mii_crs     in
                  mii_col     in
```

Nibbles go out low nibble first, preceded by seven `0x55` preamble bytes and
the `0xD5` start frame delimiter.  The FCS is appended least significant byte
first.  Half duplex behaviour (deferral on CRS, backoff on COL) is driven from
the CONFIGURE command parameters.

## MDIO

PHY management is a separate pair of blocks rather than part of the MAC,
because it is not the MAC's business: the 82586 predates MDIO and none of the
drivers in `doc/drivers` knows a PHY exists.  `wish82586` has no MDIO pins at
all; these two are what a system wires to the PHY, alongside the MAC.

`wb_mdio` is a Wishbone slave that runs the clause 22 serial bus.  Registers,
byte offsets, so the word address is the offset over four:

| offset | name     | contents                                                |
|--------|----------|---------------------------------------------------------|
| `0x00` | `CTRL`   | `[4:0]` register, `[12:8]` PHY, `[16]` read, `[24]` start |
| `0x04` | `WDATA`  | data for a write                                          |
| `0x08` | `RDATA`  | data from the last read                                   |
| `0x0c` | `STATUS` | `[0]` busy, `[1]` the last read finished                  |
| `0x10` | `DIV`    | MDC divider: MDC = clk / (2 * (DIV + 1))                  |
| `0x14` | `ID`     | `0x4d444a4f`                                              |

The station changes MDIO on the falling edge of MDC, and for a read it takes its
bit at the *end* of the low period, immediately before driving MDC high.  Both
follow from a PHY being allowed 300 ns after a rising edge to answer: the bit an
edge clocks in is the one that has been on the wire throughout the low period
before it, so sampling anywhere after the edge is sampling while the PHY may
still be changing.  `doc/drivers/NetBSD/README.md` has the bit-time numbering
this comes from, and the two cancelling off-by-ones that were found by checking
it against a station from outside this project.

`mdio_prog` is a Wishbone master that drives those registers.  After reset it
resets the PHY, waits for the reset bit to clear, advertises exactly the one
ability it was parameterised for, restarts auto-negotiation and raises
`ready_o`.  `SPEED` is 10, 100 or 1000 and `FULL_DUPLEX` picks the duplex.

Speed is asked for by advertising it rather than by forcing it into BMCR.
Forcing works at 10 and 100 but is not allowed for 1000BASE-T, which needs
negotiation to settle master and slave, so advertising one ability is the
approach that is right at every speed instead of two approaches.

A PHY that never clears its reset bit is given up on after a bounded number of
polls: `failed_o` says so and the sequence carries on.  Wedging the bring-up
of a machine because its PHY is unhappy would be worse than finishing with a
PHY that may not be configured.

The RTL8211EG needs nothing beyond clause 22 for a GMII interface; see
`doc/drivers/Linux/README.md` for why that is the answer rather than a guess.

## Software-visible behaviour

The chip is driven entirely through shared memory, exactly as the drivers in
`doc/drivers` expect:

1. Host writes the SCP (bus width, ISCP address), the ISCP (busy = 1, SCB
   offset, CBBASE) and a cleared SCB, then releases `CTRL.RST` and pulses
   `CTRL.CA`.
2. The chip reads the SCP from `SCP_ADDR`, then the ISCP, clears ISCP busy,
   and reports CX + CNA in the SCB status.
3. From then on: command blocks off `SCB.CBL` driven by the command unit,
   receive frame area off `SCB.RFA` driven by the receive unit, channel
   attention to make the chip look at the SCB command word, and acknowledge
   bits to clear status.

### The AL-LOC bit

CONFIGURE bit 3 of byte 3 decides where a frame's MAC header lives, and it
applies both ways round.

* **AL-LOC = 1** - what both reference drivers configure.  The whole frame,
  header included, is in the transmit buffer and lands in the receive buffer.
* **AL-LOC = 0**.  On transmit the destination address and the type or length
  field come from the command block at offsets 8 and 14, and the source
  address is inserted from the last IA-SETUP.  On receive the first fourteen
  bytes go into the descriptor - destination, source and type land contiguously
  from offset 8 - and the buffers hold only what follows.

Neither driver in `doc/drivers` uses AL-LOC = 0, so the byte order of the
command block's type field is taken to be wire order, most significant byte at
offset 14.  Nothing in the tree confirms that; it is the assumption to check
first if a real driver ever disagrees.

### Multicast filtering

MC-SETUP hands the chip a list of addresses.  Wish82586 holds eight of them
and matches exactly.  A list longer than that makes it accept **every**
multicast frame rather than filter on a silently shortened list: too many
frames is something the driver's software filter deals with every day, too
few is a bug the driver cannot see.  NetBSD's `ie_mc_reset()` does the same
thing on its own side once the list outgrows the transmit buffer.

An MC-SETUP with a byte count of zero clears the list, which turns multicast
reception off.  Broadcast is not affected: it has its own configuration bit.

The real part is understood to hash addresses into a table rather than hold
them exactly, which is how it scales past a handful.  Nothing in
`doc/drivers` defines that hash, and guessing it would produce a filter that
agrees with our own testbench and with nothing else, so exact matching with
an honest fallback is what is implemented.  If the hash is ever pinned down
from a datasheet, this is the piece to revisit.

Command opcodes, status bits and structure offsets are in
`src/wish82586_pkg.sv` (RTL) and `tb/cpp/i82586.h` (testbench).  The two are
kept in step by hand - if you touch one, touch the other - and
`tb/cpp/tests/test_layout.cpp` checks both against the independent reference
in `doc/drivers/NetBSD/i82586reg.h`.

### Endianness

Everything above is the chip's own view of memory: little-endian 16-bit
fields, with the SCB status word carrying CX, FR, CNA and RNR at bits 15 to
12, CUS at [10:8] and RUS at [6:4].  The Wishbone side is little endian, so
that is what Wish82586 implements.

The vintage ROM drivers in `doc/drivers/Sun*` appear to disagree - they put
CU start at bit 0 rather than bit 8, for instance - because those machines
byte swap in hardware between the CPU and the shared memory, and their headers
describe the swapped view.  A big-endian host that wants to run one of those
drivers unmodified needs the same swap in its bus glue.  Wish82586 does not do
it: which end of the machine is big is not the MAC's business.
`doc/drivers/NetBSD/README.md` works the correspondence through field by
field.
