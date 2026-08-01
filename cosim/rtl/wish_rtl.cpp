// SPDX-License-Identifier: MIT
//
// The Verilated wish82586, wrapped up so an emulated ISA card inside QEMU can
// use it in place of a software 82586.
//
// Everything here is the testbench's own models doing their usual jobs, only
// with a different thing driving them:
//
//   WbMem   over the card's memory window, so the core's Wishbone master and
//           the guest read and write one buffer;
//   WbHost  on the control registers, which is how RESET and CA - two I/O
//           writes on the card - reach the core;
//   MiiPhy  on the wire, taking frames from QEMU's network back end and
//           handing back whatever the MAC transmits.
//
// The one thing that is not like the regression is *when* the simulation runs.
// A real card works alongside the host; here the core is stepped only when the
// guest touches it, and far enough that whatever it was asked to do is
// finished by the time the I/O write returns.  Every driver in doc/drivers
// polls the SCB or waits for the interrupt afterwards, so none of them can
// tell; what it buys is that the guest never observes a half-finished
// structure in shared memory.

#include <cstdint>
#include <cstring>
#include <memory>

#include <verilated.h>

#include "Vrtl_top.h"

#include "eth.h"
#include "ie_driver.h"   // the CSR map
#include "mii_phy.h"
#include "sim.h"
#include "wb.h"

#include "wish_rtl.h"

using namespace wtb;

namespace {

// 50 MHz, which is what the regression runs MII builds at.
constexpr u64 SYS_PERIOD_PS = 20 * NS;

// How long a single piece of work may take before we stop waiting for it.  A
// maximum length frame at 100 Mb/s is about 122 us on the wire, so this is
// generous by an order of magnitude; reaching it means something is wrong.
constexpr u64 SETTLE_LIMIT_PS = 4 * MS;

// System cycles per look at whether anything is still happening, and how many
// consecutive quiet looks count as finished.
constexpr int SETTLE_CHUNK = 64;
constexpr int SETTLE_QUIET = 4;

constexpr uint32_t RUS_READY = 4;

}  // namespace

struct WishRtl {
  std::unique_ptr<VerilatedContext> ctx;
  std::unique_ptr<Vrtl_top> dut;
  std::unique_ptr<Sim> sim;
  Sim::Clock* sysclk = nullptr;
  std::unique_ptr<WbMem> mem;
  std::unique_ptr<WbHost> host;
  std::unique_ptr<MiiPhy> phy;

  uint32_t scp_addr = 0;
  uint32_t ctrl = 0;          // what was last written to CSR_CTRL

  // Refreshed at the end of every settle, so the card can ask about the
  // interrupt and the receive unit without running the simulation.
  bool irq = false;
  bool ru_ready = false;

  void settle();
  void refresh();
};

void WishRtl::refresh() {
  irq = dut->irq_o != 0;
  const uint32_t st = host->read32(csr::STATUS);
  ru_ready = ((st >> csr::STAT_RUS_LSB) & 7) == RUS_READY;
}

// Run until the core has nothing left to do: no bus cycles, nothing on the
// wire, the command unit back to idle and the core not reporting itself busy.
void WishRtl::settle() {
  const u64 deadline = sim->time_ps() + SETTLE_LIMIT_PS;
  int quiet = 0;

  while (quiet < SETTLE_QUIET && sim->time_ps() < deadline) {
    mem->clear_log();
    sim->run_posedges(sysclk, SETTLE_CHUNK);

    const uint32_t st = host->read32(csr::STATUS);
    const bool busy = mem->accesses() > 0 ||
                      phy->tx_active() || phy->rx_busy() ||
                      (st & csr::STAT_BUSY) != 0 ||
                      ((st >> csr::STAT_CUS_LSB) & 7) != 0;
    quiet = busy ? 0 : quiet + 1;
  }
  mem->clear_log();
  refresh();
}

extern "C" {

uint32_t wish_rtl_abi(void) { return WISH_RTL_ABI; }

WishRtl *wish_rtl_new(uint8_t *mem, uint32_t mem_size, uint32_t mem_base)
{
    auto *r = new WishRtl();

    r->ctx.reset(new VerilatedContext());
    r->ctx->timeunit(-12);
    r->ctx->timeprecision(-12);
    r->dut.reset(new Vrtl_top(r->ctx.get(), "wish82586"));
    r->sim.reset(new Sim([r]() { r->dut->eval(); }));
    r->sysclk = r->sim->add_clock(&r->dut->clk, SYS_PERIOD_PS, "clk");

    Vrtl_top *d = r->dut.get();

    WbSlavePorts sp;
    sp.cyc = &d->wbm_cyc_o;
    sp.stb = &d->wbm_stb_o;
    sp.we = &d->wbm_we_o;
    sp.sel = &d->wbm_sel_o;
    sp.adr = &d->wbm_adr_o;
    sp.dat_w = &d->wbm_dat_o;
    sp.dat_r = &d->wbm_dat_i;
    sp.ack = &d->wbm_ack_i;
    sp.err = &d->wbm_err_i;
    r->mem.reset(new WbMem(*r->sim, r->sysclk, sp, mem, mem_size, mem_base));

    WbMasterPorts mp;
    mp.cyc = &d->wbs_cyc_i;
    mp.stb = &d->wbs_stb_i;
    mp.we = &d->wbs_we_i;
    mp.sel = &d->wbs_sel_i;
    mp.adr = &d->wbs_adr_i;
    mp.dat_w = &d->wbs_dat_i;
    mp.dat_r = &d->wbs_dat_o;
    mp.ack = &d->wbs_ack_o;
    mp.err = &d->wbs_err_o;
    r->host.reset(new WbHost(*r->sim, r->sysclk, mp));

    MiiPorts pp;
    pp.tx_clk = &d->mii_tx_clk;
    pp.txd = &d->mii_txd;
    pp.tx_en = &d->mii_tx_en;
    pp.tx_er = &d->mii_tx_er;
    pp.rx_clk = &d->mii_rx_clk;
    pp.rxd = &d->mii_rxd;
    pp.rx_dv = &d->mii_rx_dv;
    pp.rx_er = &d->mii_rx_er;
    pp.crs = &d->mii_crs;
    pp.col = &d->mii_col;
    r->phy.reset(new MiiPhy(*r->sim, pp, MiiPhy::Speed::M100));
    // There is no shared medium behind QEMU's network back end, so there is
    // nothing to collide with.
    r->phy->set_full_duplex(true);

    // The System Configuration Pointer is wired to 0xfffff4 in the real chip
    // and both ISA boards put the window at the top of the address space, so
    // it always lands ten bytes from the end.  What the core wants is the
    // address of the bus width byte, two into the structure.
    r->scp_addr = mem_base + mem_size - 10;

    d->rst = 1;
    d->mdio_i = 0;
    r->sim->eval();
    wish_rtl_reset(r);
    return r;
}

void wish_rtl_free(WishRtl *r)
{
    if (!r) {
        return;
    }
    if (r->dut) {
        r->dut->final();
    }
    delete r;
}

void wish_rtl_reset(WishRtl *r)
{
    r->dut->rst = 1;
    r->sim->run_posedges(r->sysclk, 8);
    r->dut->rst = 0;
    r->sim->run_posedges(r->sysclk, 2);

    // CTRL.RST comes out of power-on reset set, the same shape as the bit the
    // Sun driver pokes; the host has to clear it.  The interrupt enable stays
    // on for good: the card has its own latch and mask in front of it.
    r->ctrl = csr::CTRL_IRQ_EN;
    r->host->write32(csr::CTRL, csr::CTRL_RST | r->ctrl);
    r->host->write32(csr::SCP_ADDR, r->scp_addr);
    r->host->write32(csr::CTRL, r->ctrl);
    r->sim->run_posedges(r->sysclk, 4);
    r->refresh();
}

void wish_rtl_ca(WishRtl *r)
{
    r->host->write32(csr::CTRL, csr::CTRL_CA | r->ctrl);
    r->settle();
}

int wish_rtl_irq(WishRtl *r) { return r->irq ? 1 : 0; }

int wish_rtl_ru_ready(WishRtl *r) { return r->ru_ready ? 1 : 0; }

void wish_rtl_rx(WishRtl *r, const uint8_t *frame, uint32_t len)
{
    Bytes wire(frame, frame + len);
    // QEMU hands over frames without their FCS; the wire has one.
    const uint32_t fcs = eth_fcs(wire);
    for (int i = 0; i < 4; i++) {
        wire.push_back(uint8_t((fcs >> (8 * i)) & 0xff));
    }
    r->phy->inject_wire(wire);
    r->settle();
}

int wish_rtl_tx_pop(WishRtl *r, uint8_t *buf, uint32_t max)
{
    if (!r->phy->has_tx()) {
        return 0;
    }
    const WireFrame f = r->phy->pop_tx();
    const Bytes out = f.payload_no_fcs();
    if (out.size() > max) {
        return -1;
    }
    memcpy(buf, out.data(), out.size());
    return int(out.size());
}

uint64_t wish_rtl_time_ns(WishRtl *r) { return r->sim->time_ps() / 1000; }

}  // extern "C"
