# Verification setup

Everything runs on open source tools: Verilator for simulation, Icarus and
Yosys as second opinions.  There is one binary containing the whole
regression; no per-test build, no external Python dependencies.

## Running it

```sh
make            # build the testbench
make test       # build and run everything
make test T=crc # only the tests whose name contains "crc"
make wave T=... # same, writing build/waves/<test>.vcd
make list       # list the registered tests
make lint       # Verilator lint of the RTL
make lint-icarus  # Icarus parse of the same files
make synth      # Yosys elaboration check of every module
```

Runner flags can be passed through `FLAGS`, e.g. `make test FLAGS=-v` to see
the notes tests emit and the reason a pending test failed.

## Layout

```
src/                  RTL
tb/sv/tb_top.sv       simulation top: DUT plus the leaves that have unit tests
tb/cpp/               testbench library
  sim.{h,cpp}         event-driven kernel, one entry per clock edge
  wb.{h,cpp}          Wishbone slave memory model and host master BFM
  mii_phy.{h,cpp}     MII PHY model: frame injection, capture, error injection
  eth.{h,cpp}         Ethernet frames and the FCS
  i82586.{h,cpp}      82586 shared memory image: SCP/ISCP/SCB/CB/RFA
  ie_driver.{h,cpp}   driver model, mirrors doc/drivers/*/if_ie.c
  env.{h,cpp}         per-test environment tying it all together
  test.{h,cpp}        test framework and runner
  tests/              the tests themselves
```

## How the kernel works

There are three clocks (system, `mii_tx_clk`, `mii_rx_clk`) that are not
multiples of each other, so the kernel jumps from one clock edge to the next
rather than ticking at a fixed resolution.

Testbench components attach to the **negative** edge of their clock.  At a
negedge the outputs the RTL produced at the preceding posedge are stable, and
anything driven then is set up well before the next posedge.  That gives the
same cycle semantics as RTL without the races that come from poking signals
around a rising edge.

Blocking helpers - `WbHost::read32()`, `IeDriver::transmit()` and friends -
pump the kernel internally until their transaction completes or their timeout
expires, so tests read like driver code rather than like state machines.

## Test categories

| prefix   | what it covers                                                    |
|----------|-------------------------------------------------------------------|
| `infra_` | the testbench itself: FCS, memory model, PHY model, memory image  |
| `unit_`  | RTL leaf modules against their software model                     |
| `sys_`   | the whole chip, driven the way a real driver drives it            |

`infra_` tests come first for a reason: if the PHY model computes a wrong FCS
or the memory image lays out a control block wrongly, every `sys_` result is
meaningless.

## Pending tests

The `sys_` tests describe what the chip has to do and were written before the
RTL exists.  They are declared with

```cpp
TEST_PENDING(sys_transmit_one_frame, "the transmit path is not implemented") {
```

which means: run it, expect it to fail, report it as `PENDING` rather than as
a failure.  When the corresponding RTL lands the runner reports `XPASS`, and
the marker gets changed to a plain `TEST`.  `make test` is green today and
stays green; the pending count is the todo list.

## Adding a test

Drop a `TEST(name) { ... }` into a file in `tb/cpp/tests/` - registration is
automatic, the Makefile picks the file up by wildcard.  Inside a test, `env`
gives access to everything:

```cpp
TEST(sys_something) {
  CHECK_DRV(env.drv().init());                 // fails with the driver's own message
  EthFrame f = env.make_frame(100);
  env.phy().inject(f);
  CHECK_DRV(env.drv().wait_rx(1));
  std::vector<ie::RxFrame> rx = env.img().collect_rx();
  CHECK_EQ(rx.size(), size_t(1));
  CHECK_EQ(rx[0].data, f.payload);
}
```

Checks: `CHECK`, `CHECK_MSG`, `CHECK_EQ`, `CHECK_NE`, `CHECK_DRV`.  They print
both values on mismatch, byte vectors as a hex dump.

## Debugging a failure

```sh
make wave T=sys_transmit_one_frame FLAGS=-v
```

then open `build/waves/sys_transmit_one_frame.vcd`.  The VCD timescale is 1 ps
and every signal of `tb_top` is in it.  On the software side `env.mem().log()`
holds every bus cycle the core issued, and `env.img().dump_scb()` /
`dump_cb()` print the control structures.

## Known tool quirks

* Icarus reports `sorry: constant selects in always_* processes are not
  currently supported` on the CSR read multiplexer.  It is a note about how it
  builds the sensitivity list of an `always_comb`; the semantics are correct
  and it does not affect the Verilator build.
* Yosys 0.23 does not accept `import pkg::*`, in a module header or a module
  body, so the RTL refers to package items as `wish82586_pkg::NAME`.
* Comments starting with the word "verilator" are parsed as pragmas - do not
  start a comment line with it.
