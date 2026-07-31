// SPDX-License-Identifier: MIT
//
// Software model of an 82586 driver.
//
// The sequences below mirror what the Sun ROM drivers in doc/drivers do:
// release reset, let the chip fetch the SCP and ISCP, then drive everything
// through the SCB - command list for the command unit, receive frame area for
// the receive unit, channel attention to wake the chip up, acknowledgements to
// clear the status bits.
//
// Every call is blocking and bounded by a timeout; false means the DUT did not
// answer in time and last_error() says what was being waited on.

#pragma once

#include <string>
#include <vector>

#include "i82586.h"
#include "sim.h"
#include "wb.h"

namespace wtb {

// CSR offsets, see doc/interface.md and src/wish82586_pkg.sv.
namespace csr {
constexpr uint32_t CTRL = 0x00;
constexpr uint32_t STATUS = 0x04;
constexpr uint32_t SCP_ADDR = 0x08;
constexpr uint32_t ID = 0x10;

constexpr uint32_t CTRL_RST = 1u << 0;
constexpr uint32_t CTRL_CA = 1u << 1;
constexpr uint32_t CTRL_IRQ_EN = 1u << 8;

constexpr uint32_t STAT_INT = 1u << 0;
constexpr uint32_t STAT_BUSY = 1u << 1;
constexpr int STAT_CUS_LSB = 4;
constexpr int STAT_RUS_LSB = 8;

constexpr uint32_t ID_VALUE = 0x82586001;
}  // namespace csr

class IeDriver {
 public:
  IeDriver(Sim& sim, WbHost& host, ie::MemImage& img);

  // ---- raw CSR access -----------------------------------------------------
  void hold_reset(bool on);
  void channel_attention();
  void enable_irq(bool on);
  uint32_t status();
  uint32_t chip_id();
  void set_scp_addr(uint32_t addr);

  // ---- initialisation -----------------------------------------------------
  // Writes the SCP/ISCP/SCB, releases reset, gives channel attention and waits
  // for the chip to clear ISCP busy and report itself initialised.
  bool init();

  // ---- SCB level ----------------------------------------------------------
  bool wait_scb_cmd_accepted();
  bool issue_scb(uint16_t cmd);
  bool ack_all();
  bool wait_scb_status(uint16_t mask, uint16_t value);

  // ---- command unit -------------------------------------------------------
  // Points the SCB at cb, starts the command unit and waits for the block to
  // complete.  cb_status() then says whether it worked.
  bool run_cb(uint16_t cb);
  bool nop();
  bool ia_setup(const MacAddr& mac);
  bool configure(const ie::Config& cfg);
  bool mc_setup(const std::vector<MacAddr>& list);
  bool transmit(const EthFrame& f, uint16_t* cb_out = nullptr);

  // ---- receive unit -------------------------------------------------------
  bool ru_start();
  bool ru_abort();
  // Waits until the receive unit reports at least one frame.
  bool wait_rx(size_t n = 1);

  // ---- timeouts (adjust per test if needed) -------------------------------
  u64 t_init = 500 * US;
  u64 t_cmd = 200 * US;
  u64 t_tx = 5 * MS;
  u64 t_rx = 5 * MS;

  const std::string& last_error() const { return err_; }
  const MacAddr& mac() const { return mac_; }

 private:
  bool fail(const std::string& what);

  Sim& sim_;
  WbHost& host_;
  ie::MemImage& img_;
  std::string err_;
  MacAddr mac_{0x08, 0x00, 0x20, 0x01, 0x02, 0x03};  // a Sun OUI, why not
  uint32_t irq_en_ = 0;
};

}  // namespace wtb
