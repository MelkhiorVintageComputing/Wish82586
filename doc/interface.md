# Wish82586 interface contract

This is what the testbench in `tb/` assumes and what the RTL in `src/` has to
provide.  Changing anything here means changing both sides.

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

6-bit address (a register index), 32-bit data, byte selects honoured.
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

`irq_o` is a level, asserted while `CTRL.IRQ_EN` is set and the core has an
unacknowledged SCB status bit (CX, FR, CNA, RNR).  Acknowledging through the
SCB command word clears it.

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

### Receive rate at gigabit

The receive unit writes one byte per Wishbone transaction, which takes about
eighty nanoseconds at 50 MHz.  That is comfortable at 100 Mb/s, where a byte
arrives every eighty nanoseconds - a full length frame arrives with no
overrun - but at gigabit a byte arrives every eight, and no FIFO depth fixes a
sustained deficit.  So a GMII build receives nothing useful yet: the frames
are reported as overruns.

Transmit does not have the problem, because the command unit stages the whole
frame before the transmitter starts, so the memory side has no real time
constraint at all.

Moving the memory side to whole words is the fix, and it is the next piece of
work.  The receive tests are marked pending in a GMII build with that reason,
so `make test PHY=gmii` says so on every run rather than the limitation
sitting in a document nobody reads.

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

`mdc` / `mdio_*` are brought out for PHY management but are not driven yet.

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
