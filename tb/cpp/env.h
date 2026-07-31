// SPDX-License-Identifier: MIT
//
// Per-test environment: a fresh DUT, clocks, bus models, PHY and driver.
//
// Each test gets its own Env, so nothing leaks from one test to the next and
// a failing test can be rerun on its own with a waveform.

#pragma once

#include <memory>
#include <string>

#include "i82586.h"
#include "ie_driver.h"
#include "mii_phy.h"
#include "sim.h"
#include "wb.h"

class VerilatedContext;
class VerilatedVcdC;
class Vtb_top;

namespace wtb {

// Set by the build: 4 for MII, 8 for GMII.  See the PHY variable in the
// Makefile.
#ifndef PHY_DATA_W
#define PHY_DATA_W 4
#endif

struct EnvConfig {
  // 50 MHz is plenty for MII.  Gigabit needs a byte moved every eight
  // nanoseconds, so the bus has to run at 125 MHz to have any chance.
  u64 sys_period_ps = (PHY_DATA_W == 8) ? 8 * NS : 20 * NS;
  size_t mem_size = 1u << 20;          // 1 MiB of shared memory at address 0
  uint32_t mem_base = 0;
  uint32_t cbbase = 0x0000'2000;       // control blocks live here
  uint32_t scp_addr = 0x000f'fff6;     // 0xFFFFF6 truncated into the model memory
  MiiPhy::Speed speed =
      (PHY_DATA_W == 8) ? MiiPhy::Speed::G1000 : MiiPhy::Speed::M100;
  int mem_wait_states = 0;
};

class Env {
 public:
  Env(const std::string& test_name, bool trace, EnvConfig cfg = EnvConfig());
  ~Env();

  Env(const Env&) = delete;
  Env& operator=(const Env&) = delete;

  // Pulses the global (power on) reset and lets the CSR block settle.
  void power_on_reset(int cycles = 8);
  void tick(int cycles = 1);

  Vtb_top* dut() { return dut_.get(); }
  Sim& sim() { return *sim_; }
  Sim::Clock* sysclk() { return sysclk_; }
  WbMem& mem() { return *mem_; }
  WbHost& host() { return *host_; }
  MiiPhy& phy() { return *phy_; }
  ie::MemImage& img() { return *img_; }
  IeDriver& drv() { return *drv_; }
  const EnvConfig& cfg() const { return cfg_; }
  const std::string& name() const { return name_; }

  // Convenience for tests that only care about the frame level.
  EthFrame make_frame(size_t payload_len, uint16_t type = 0x0800,
                      uint32_t seed = 1) const;
  MacAddr local_mac() const { return MacAddr(0x08, 0x00, 0x20, 0x01, 0x02, 0x03); }
  MacAddr peer_mac() const { return MacAddr(0x02, 0x00, 0xde, 0xad, 0xbe, 0xef); }

 private:
  void bind_models();

  std::string name_;
  EnvConfig cfg_;
  std::unique_ptr<VerilatedContext> ctx_;
  std::unique_ptr<Vtb_top> dut_;
  std::unique_ptr<VerilatedVcdC> tfp_;
  std::unique_ptr<Sim> sim_;
  Sim::Clock* sysclk_ = nullptr;
  std::unique_ptr<WbMem> mem_;
  std::unique_ptr<WbHost> host_;
  std::unique_ptr<MiiPhy> phy_;
  std::unique_ptr<ie::MemImage> img_;
  std::unique_ptr<IeDriver> drv_;
};

}  // namespace wtb
