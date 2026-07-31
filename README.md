# Wish82586

## Overview

A SystemVerilog Ethernet MAC, software-compatible with the Intel 82586 Ethernet device, atrgeted at FPGA.
The Wish82586 connects to a Wishbone B4 bus host-side and a MII or GMII interface to the PHY.
It can be used to support Ethernet on recreated vintage computers in a modern FPGA.

## Status

The verification infrastructure is in place.  The chip comes up: it walks the
SCP and ISCP, reports itself initialised in the SCB and handles channel
attention.  The units that do the actual networking are next.

| block                                     | state                      |
|-------------------------------------------|----------------------------|
| control registers (`wb_csr`)              | done, tested               |
| Wishbone master (`wb_master`)             | done, tested               |
| init sequencer and SCB handler (`ie_core`)| done, tested               |
| Ethernet FCS (`crc32_eth`)                | done, tested               |
| synchronous FIFO (`sync_fifo`)            | done, tested               |
| command unit                              | to do                      |
| receive unit                              | to do                      |
| transmit / receive datapath, MII          | to do                      |
| GMII                                      | later                      |

The system-level tests for everything in the "to do" list are already written
and run on every build, marked pending.  They are the specification: as each
block lands, its tests turn from `PENDING` to `XPASS` and the marker comes off.

## Building and testing

Needs Verilator (5.x), a C++ compiler and GNU make; Icarus Verilog and Yosys
are optional extra checks.

```sh
make test               # build and run the whole regression
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
