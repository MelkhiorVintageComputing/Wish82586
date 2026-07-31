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

## Wishbone B4 classic slave - control registers

8-bit address, 32-bit data, byte selects honoured.  `ACK_O` comes back the
cycle after the request; `ERR_O` is never asserted.

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

32-bit address, 32-bit data, `SEL_O` for byte lanes, classic (non-pipelined)
cycles.  The core is the only master modelled; the testbench memory answers
with a configurable number of wait states and can be made to return `ERR_I` in
a chosen address window.

The 82586 view of memory is little endian: 16-bit control fields, 24-bit
buffer addresses, control blocks addressed as 16-bit offsets from CBBASE.  How
that maps onto 32-bit bus cycles is up to the core; the testbench only checks
the resulting memory contents and that no access lands outside the model.

`WB_DATA_W` is a parameter.  Only 32 is wired up today, 16 is planned; the
testbench memory model is already width-parameterised on the C++ side.

## Interrupt

`irq_o` is a level, asserted while `CTRL.IRQ_EN` is set and the core has an
unacknowledged SCB status bit (CX, FR, CNA, RNR).  Acknowledging through the
SCB command word clears it.

## MII

Standard 4-bit MII, PHY-sourced clocks:

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

Command opcodes, status bits and structure offsets are in
`src/wish82586_pkg.sv` (RTL) and `tb/cpp/i82586.h` (testbench).  The two are
kept in step by hand - if you touch one, touch the other.
