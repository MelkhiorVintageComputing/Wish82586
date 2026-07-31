# Wish82586

## Overview

A SystemVerilog Ethernet MAC, software-compatible with the Intel 82586 Ethernet device, atrgeted at FPGA.
The Wish82586 connects to a Wishbone B4 bus host-side and a MII or GMII interface to the PHY.
It can be used to support Ethernet on recreated vintage computers in a modern FPGA.

## Status

The chip works end to end: it comes up, runs command lists, receives frames
into the host's descriptor rings and transmits from them, with deferral,
padding, FCS, collision retry and internal loopback.  What is left is
gigabit receive, which needs the memory side to move whole words, and the
two approximations noted below.

| block                                     | state                      |
|-------------------------------------------|----------------------------|
| control registers (`wb_csr`)              | done, tested               |
| Wishbone master (`wb_master`)             | done, tested               |
| init sequencer and SCB handler (`ie_core`)| done, tested               |
| command unit (`ie_cu`)                    | NOP, IA, CONFIGURE, TRANSMIT |
| receive unit (`ie_ru`)                    | done, tested               |
| MII receive front end (`mii_rx`)          | done, tested               |
| clock crossing (`async_fifo`)             | done, tested               |
| MII transmitter (`mii_tx`)                | done, tested               |
| memory arbiter (`wb_arb`)                 | done, tested               |
| Ethernet FCS (`crc32_eth`)                | done, tested               |
| synchronous FIFO (`sync_fifo`)            | done, tested               |
| internal loopback                         | done, tested               |
| multicast address filtering               | exact, 8 entries           |
| saving bad frames (SAV-BF)                | done, tested               |
| AL-LOC = 0, both directions               | done, tested               |
| TDR and DIAGNOSE commands                 | done, tested               |
| DUMP command                              | reports failure            |
| GMII datapath and transmit                | done, tested               |
| GMII receive                              | needs word-wide DMA        |

Everything the reference drivers do on a normal bring-up now works, and
`sys_netbsd_style_bring_up` walks that sequence end to end.  Two things are
deliberate approximations, both marked TODO in the RTL: the collision backoff
uses an LFSR rather than true truncated binary exponential backoff, and the
command unit stages transmit frames a byte per bus cycle on a 32-bit bus.

Tests for work not yet done are written up front and marked pending, so the
pending count is the todo list; it is currently empty.

## Building and testing

Needs Verilator (5.x), a C++ compiler and GNU make; Icarus Verilog and Yosys
are optional extra checks.

```sh
make test               # build and run the whole regression (MII)
make test PHY=gmii      # the same tests against a GMII build
make test-all           # both
make test T=crc         # just the tests whose name contains "crc"
make wave T=<test>      # run with tracing, waveform in build/waves/
make lint               # Verilator lint
make synth              # Yosys elaboration check
```

## Documentation

* [`doc/interface.md`](doc/interface.md) - the register map, bus and MII
  contract between the RTL and the testbench.
* [`doc/verification.md`](doc/verification.md) - how the testbench is built
  and how to add tests.
* [`doc/drivers`](doc/) - source of drivers for the real chip, used as the
  reference for software-visible behaviour.

## Layout

```
src/      RTL
tb/sv/    simulation top level
tb/cpp/   testbench: bus models, MII PHY model, 82586 memory image, driver
          model, test framework, tests
doc/      interface and verification notes, vintage drivers
```
