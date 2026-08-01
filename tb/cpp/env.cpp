// SPDX-License-Identifier: MIT

#include "env.h"

#include <verilated.h>
#include <verilated_vcd_c.h>

#include <sys/stat.h>
#include <sys/types.h>

#include "Vtb_top.h"

namespace wtb {

Env::Env(const std::string& test_name, bool trace, EnvConfig cfg)
    : name_(test_name), cfg_(cfg) {
  ctx_.reset(new VerilatedContext());
  ctx_->timeunit(-12);       // the kernel counts in picoseconds
  ctx_->timeprecision(-12);
  if (trace) ctx_->traceEverOn(true);

  dut_.reset(new Vtb_top(ctx_.get(), (name_ + "_dut").c_str()));

  sim_.reset(new Sim([this]() { dut_->eval(); }));

  if (trace) {
    tfp_.reset(new VerilatedVcdC());
    dut_->trace(tfp_.get(), 99);
    ::mkdir("build", 0755);
    ::mkdir("build/waves", 0755);
    tfp_->open(("build/waves/" + name_ + ".vcd").c_str());
    sim_->set_trace(tfp_.get());
  }

  sysclk_ = sim_->add_clock(&dut_->clk, cfg_.sys_period_ps, "clk");
  bind_models();
}

void Env::bind_models() {
  Vtb_top* d = dut_.get();

  // Shared memory hanging off the DUT's Wishbone master.
  WbSlavePorts sp;
  sp.cyc = &d->dut_wbm_cyc_o;
  sp.stb = &d->dut_wbm_stb_o;
  sp.we = &d->dut_wbm_we_o;
  sp.sel = &d->dut_wbm_sel_o;
  sp.adr = &d->dut_wbm_adr_o;
  sp.dat_w = &d->dut_wbm_dat_o;
  sp.dat_r = &d->dut_wbm_dat_i;
  sp.ack = &d->dut_wbm_ack_i;
  sp.err = &d->dut_wbm_err_i;
  mem_.reset(new WbMem(*sim_, sysclk_, sp, cfg_.mem_size, cfg_.mem_base));
  mem_->set_wait_states(cfg_.mem_wait_states);

  // Host CPU driving the control registers.
  WbMasterPorts mp;
  mp.cyc = &d->dut_wbs_cyc_i;
  mp.stb = &d->dut_wbs_stb_i;
  mp.we = &d->dut_wbs_we_i;
  mp.sel = &d->dut_wbs_sel_i;
  mp.adr = &d->dut_wbs_adr_i;
  mp.dat_w = &d->dut_wbs_dat_i;
  mp.dat_r = &d->dut_wbs_dat_o;
  mp.ack = &d->dut_wbs_ack_o;
  mp.err = &d->dut_wbs_err_o;
  host_.reset(new WbHost(*sim_, sysclk_, mp));

  // PHY.
  MiiPorts pp;
  pp.tx_clk = &d->dut_mii_tx_clk;
  pp.txd = &d->dut_mii_txd;
  pp.tx_en = &d->dut_mii_tx_en;
  pp.tx_er = &d->dut_mii_tx_er;
  pp.rx_clk = &d->dut_mii_rx_clk;
  pp.rxd = &d->dut_mii_rxd;
  pp.rx_dv = &d->dut_mii_rx_dv;
  pp.rx_er = &d->dut_mii_rx_er;
  pp.crs = &d->dut_mii_crs;
  pp.col = &d->dut_mii_col;
  phy_.reset(new MiiPhy(*sim_, pp, cfg_.speed));

  // The MDIO station, driven straight from a test, and its PHY.
  WbMasterPorts mp2;
  mp2.cyc = &d->mdio_wbs_cyc_i;
  mp2.stb = &d->mdio_wbs_stb_i;
  mp2.we = &d->mdio_wbs_we_i;
  mp2.sel = &d->mdio_wbs_sel_i;
  mp2.adr = &d->mdio_wbs_adr_i;
  mp2.dat_w = &d->mdio_wbs_dat_i;
  mp2.dat_r = &d->mdio_wbs_dat_o;
  mp2.ack = &d->mdio_wbs_ack_o;
  mp2.err = &d->mdio_wbs_err_o;
  mdio_host_.reset(new WbHost(*sim_, sysclk_, mp2));

  MdioPorts dp;
  dp.mdc = &d->mdio_mdc;
  dp.mdio_o = &d->mdio_mdio_o;
  dp.mdio_oe = &d->mdio_mdio_oe;
  dp.mdio_i = &d->mdio_mdio_i;
  mdio_phy_.reset(new MdioPhy(*sim_, sysclk_, dp, 1));

  // The PHYs on the far side of the programming blocks.
  MdioPorts pp0, pp1, pp2;
  pp0.mdc = &d->p0_mdc; pp0.mdio_o = &d->p0_mdio_o;
  pp0.mdio_oe = &d->p0_mdio_oe; pp0.mdio_i = &d->p0_mdio_i;
  pp1.mdc = &d->p1_mdc; pp1.mdio_o = &d->p1_mdio_o;
  pp1.mdio_oe = &d->p1_mdio_oe; pp1.mdio_i = &d->p1_mdio_i;
  pp2.mdc = &d->p2_mdc; pp2.mdio_o = &d->p2_mdio_o;
  pp2.mdio_oe = &d->p2_mdio_oe; pp2.mdio_i = &d->p2_mdio_i;
  prog_phy_[0].reset(new MdioPhy(*sim_, sysclk_, pp0, 1));
  prog_phy_[1].reset(new MdioPhy(*sim_, sysclk_, pp1, 1));
  prog_phy_[2].reset(new MdioPhy(*sim_, sysclk_, pp2, 1));

  // The Sun-2 control register block; see doc/sun2_ethernet.pdf.
  WbMasterPorts mp3;
  mp3.cyc = &d->sun2_wbs_cyc_i;
  mp3.stb = &d->sun2_wbs_stb_i;
  mp3.we = &d->sun2_wbs_we_i;
  mp3.sel = &d->sun2_wbs_sel_i;
  mp3.adr = &d->sun2_wbs_adr_i;
  mp3.dat_w = &d->sun2_wbs_dat_i;
  mp3.dat_r = &d->sun2_wbs_dat_o;
  mp3.ack = &d->sun2_wbs_ack_o;
  mp3.err = &d->sun2_wbs_err_o;
  sun2_host_.reset(new WbHost(*sim_, sysclk_, mp3));

  img_.reset(new ie::MemImage(*mem_, cfg_.cbbase, cfg_.scp_addr));
  drv_.reset(new IeDriver(*sim_, *host_, *img_));

  d->rst = 1;
  // The core's own host pins, ORed into wb_csr's inside tb_top; a test that
  // wants to drive the core without the register block raises them itself.
  d->dut_core_rst_i = 0;
  d->dut_ca_i = 0;
  d->sun2_int_i = 0;
  d->sun2_bus_err_i = 0;
  sim_->eval();
}

Env::~Env() {
  if (tfp_) {
    tfp_->flush();
    tfp_->close();
  }
  if (dut_) dut_->final();
}

void Env::power_on_reset(int cycles) {
  dut_->rst = 1;
  sim_->run_posedges(sysclk_, uint64_t(cycles));
  dut_->rst = 0;
  sim_->run_posedges(sysclk_, 2);
}

void Env::tick(int cycles) { sim_->run_posedges(sysclk_, uint64_t(cycles)); }

EthFrame Env::make_frame(size_t payload_len, uint16_t type, uint32_t seed) const {
  return EthFrame(peer_mac(), local_mac(), type, random_payload(payload_len, seed));
}

}  // namespace wtb
