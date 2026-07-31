# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A SystemVerilog Ethernet MAC that is software-compatible with the Intel 82586,
targeting FPGAs: Wishbone B4 on the host side, MII (GMII later) on the PHY
side.  The point is to let recreated vintage machines run their original,
unmodified drivers, so **the drivers in `doc/drivers/` are the specification**.
When a behavioural question comes up ("does the chip insert the source
address?", "what does the driver expect after the first channel attention?"),
answer it by reading the drivers, not from memory of the datasheet.

Which driver to read matters:

* **`doc/drivers/NetBSD/i82586reg.h` is the authority on bit positions and
  structure offsets.**  It serves both a little-endian ISA card and big-endian
  Suns, so it describes the chip's own view of memory, which is what the RTL
  sees.
* The `doc/drivers/Sun*_ROM` headers are a *byte-swapped* view - those machines
  swap in hardware between the CPU and the shared memory - so their bit
  numbers disagree with ours by design.  Read them for behaviour and driver
  sequencing, never for bit positions.  This already caused one wrong layout;
  `doc/drivers/NetBSD/README.md` has the correspondence table.

`tb/cpp/tests/test_layout.cpp` pins our constants to the NetBSD header.  If you
change anything in `wish82586_pkg.sv` or `i82586.h`, that test is what stops
the testbench and the RTL from quietly agreeing with each other and with
nothing else.

The MAC itself is mostly unwritten.  The verification infrastructure is
finished and the system-level tests are already there, marked pending.  Work
proceeds test first: pick a pending test, implement the RTL, drop the marker.

## Commands

```sh
make                    # build the testbench
make test               # build and run the whole regression (fast, ~1 s)
make test T=crc         # only tests whose name contains "crc"
make test FLAGS=-v      # show test notes and why pending tests failed
make wave T=<test>      # run with tracing -> build/waves/<test>.vcd
make list               # list registered tests
make lint               # Verilator lint, -Wall, must stay clean
make lint-icarus        # Icarus parse check
make synth              # Yosys elaboration check of every module
make clean
```

`make test` must stay green.  Green means 0 failed; pending tests are expected
failures and do not break the build.

## Architecture

### The two-sided contract

`doc/interface.md` is the agreement between `src/` and `tb/`.  Anything that
changes the register map, the bus behaviour or the MII framing has to change
the RTL, the testbench and that document together.

Two files hold the same 82586 constants and must be kept in step by hand:
`src/wish82586_pkg.sv` (RTL) and `tb/cpp/i82586.h` (testbench).  They are
deliberately independent - the testbench is not allowed to derive its
expectations from the RTL.

### RTL (`src/`)

`wish82586.sv` is the top: it instantiates `wb_csr`, `ie_core` and `wb_master`,
and holds the TODO block listing what still has to be built (command unit,
receive unit, TX/RX datapath).  Everything the core does not drive yet is tied
off there, including a `_unused` sink that keeps the linter quiet - shrink it
as signals get consumed.

Working blocks: `wb_csr` (control registers), `ie_core` (initialisation
sequencer and SCB handler), `wb_master` (24-bit 8/16-bit accesses onto the
32-bit Wishbone port), `crc32_eth` (Ethernet FCS, `DATA_W` 4 for MII or 8),
`sync_fifo`.

Blocks reach memory through `wb_master`'s internal port: hold `req_i` with the
address until `ack_o` pulses for one cycle.  `ie_core` drives it from a
combinational request decode plus a sequential state machine - follow that
shape for the units still to come, and note that `wb_master` refuses a new
request during its own `ack_o` cycle so a state can safely hold `req_i` high
across the handover.  There is exactly one master port today; a second
requester needs an arbiter in front of it.

Ordering that matters: `ie_core` writes the SCB status back *before* clearing
the SCB command word, so a driver that polls the command word and then reads
the status never sees stale bits.

The real 82586 has no software-visible registers, only RESET, CA and INT pins.
`wb_csr` exposes those as a small Wishbone slave, and makes the SCP address
programmable (it is hard-wired to `0xFFFFF6` on the real part).  `CTRL.RST` is
a level that reads back and comes out of power-on reset **set**, so the host
must clear it - the same shape as the `obie_noreset` bit the Sun driver pokes.

### Testbench (`tb/cpp/`)

Verilator plus a C++ testbench, everything in one binary.  Verilator and
Icarus both lack usable SystemVerilog class support, which is why the reusable
layer is C++ rather than SV.

`sim.{h,cpp}` is an event-driven kernel: three clocks that are not multiples of
each other (system, `mii_tx_clk`, `mii_rx_clk`, deliberately skewed), so it
jumps from edge to edge instead of ticking at a fixed resolution.

**Models attach on the negative edge of their clock.**  At a negedge the
outputs the RTL produced at the preceding posedge are stable and anything
driven takes effect at the next posedge.  New models must follow this; sampling
on the posedge sees post-edge values and silently gives wrong results.

Blocking helpers (`WbHost::read32`, `IeDriver::transmit`, ...) pump the kernel
internally via `Sim::run_until` until the transaction completes or a timeout
expires, so tests read like driver code.  Never call one from inside a clock
callback.

Layered as: `eth` (frames, FCS) -> `wb` (bus models) and `mii_phy` (PHY model)
-> `i82586` (shared memory image: SCP/ISCP/SCB, command blocks, descriptor
rings) -> `ie_driver` (the sequences from `if_ie.c`) -> `env` (per-test
instance of all of it) -> `test` (framework and runner).  A test never touches
Verilator signals for anything a model already covers.

`tb/sv/tb_top.sv` is the Verilator top: it wires the DUT plus the leaf modules
that have unit tests, so one binary covers both levels.  Adding a unit-tested
leaf means adding its ports here.

### Tests (`tb/cpp/tests/`)

Prefixes are meaningful and ordered by trust: `infra_` checks the testbench
itself, `unit_` checks RTL leaves against software models, `sys_` drives the
whole chip through shared memory the way a driver does.  If an `infra_` test
breaks, no `sys_` result means anything.

`TEST(name)` must pass.  `TEST_PENDING(name, "reason")` runs, is expected to
fail, and is reported as PENDING; when it starts passing the runner says XPASS
and the marker comes off.  The pending list is the project todo list - prefer
adding a pending test for missing behaviour over leaving it untested.

Tests self register; adding a file in `tb/cpp/tests/` is enough, the Makefile
globs it.  Checks are `CHECK`, `CHECK_MSG`, `CHECK_EQ`, `CHECK_NE` and
`CHECK_DRV` (reports the driver model's own error message).

## Conventions

RTL: SystemVerilog, `always_ff`/`always_comb`, synchronous active-high reset
(Wishbone `RST_I`), two-space indent, ports one per line with the direction
aligned, `_i`/`_o` suffixes on bus ports.

C++: C++14, two-space indent, `namespace wtb`, models own their signal pointers
through a small ports struct rather than including the Verilated header.

Comments explain intent and reference the vintage behaviour being reproduced;
they do not narrate the code.  British/plain spelling is used throughout
(`initialise`, `serialise`, `colour`) - match it.

## Tool quirks that will bite

* Verilator parses comments starting with the word "verilator" as pragmas.
  Never start a comment line with it.
* Yosys 0.23 rejects `import pkg::*` in both module headers and module bodies,
  so RTL refers to package items as `wish82586_pkg::NAME`.  Keep it that way or
  `make synth` breaks.
* Icarus reports `sorry: constant selects in always_* processes ...` on the CSR
  read mux.  It concerns sensitivity-list construction in an `always_comb` and
  is harmless; do not "fix" it by unrolling the register map.
* `make lint` runs with `-Wall` and must stay silent.  Unused package
  parameters and not-yet-connected signals are already suppressed with narrow
  `lint_off` pragmas - narrow them further rather than widening.
