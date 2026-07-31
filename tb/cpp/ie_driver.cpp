// SPDX-License-Identifier: MIT

#include "ie_driver.h"

namespace wtb {

IeDriver::IeDriver(Sim& sim, WbHost& host, ie::MemImage& img)
    : sim_(sim), host_(host), img_(img) {}

bool IeDriver::fail(const std::string& what) {
  err_ = what;
  return false;
}

void IeDriver::hold_reset(bool on) {
  host_.write32(csr::CTRL, (on ? csr::CTRL_RST : 0) | irq_en_);
}

void IeDriver::channel_attention() {
  host_.write32(csr::CTRL, csr::CTRL_CA | irq_en_);
}

void IeDriver::enable_irq(bool on) {
  irq_en_ = on ? csr::CTRL_IRQ_EN : 0;
  host_.write32(csr::CTRL, irq_en_);
}

uint32_t IeDriver::status() { return host_.read32(csr::STATUS); }

uint32_t IeDriver::chip_id() { return host_.read32(csr::ID); }

void IeDriver::set_scp_addr(uint32_t addr) { host_.write32(csr::SCP_ADDR, addr); }

bool IeDriver::init() {
  err_.clear();
  hold_reset(true);
  set_scp_addr(img_.scp_addr());
  img_.build_init_structures(true);
  hold_reset(false);
  channel_attention();

  if (!sim_.run_until([this]() { return img_.iscp_busy() == 0; }, t_init))
    return fail("ISCP busy never cleared after the first channel attention");

  // After initialisation the chip reports CX and CNA with both units idle.
  if (!sim_.run_until(
          [this]() {
            uint16_t st = img_.scb_status();
            return (st & (ie::SCB_ST_CX | ie::SCB_ST_CNA)) ==
                   (ie::SCB_ST_CX | ie::SCB_ST_CNA);
          },
          t_init))
    return fail("SCB did not report CX|CNA after initialisation");

  return ack_all();
}

bool IeDriver::wait_scb_cmd_accepted() {
  if (!sim_.run_until([this]() { return img_.scb_cmd() == 0; }, t_cmd))
    return fail("the SCB command word was never cleared by the chip");
  return true;
}

bool IeDriver::issue_scb(uint16_t cmd) {
  if (!wait_scb_cmd_accepted()) return false;
  img_.set_scb_cmd(cmd);
  channel_attention();
  return wait_scb_cmd_accepted();
}

bool IeDriver::ack_all() {
  uint16_t st = img_.scb_status();
  uint16_t ack = uint16_t(st & (ie::SCB_ST_CX | ie::SCB_ST_FR | ie::SCB_ST_CNA |
                                ie::SCB_ST_RNR));
  if (!ack) return true;
  return issue_scb(ack);
}

bool IeDriver::wait_scb_status(uint16_t mask, uint16_t value) {
  if (!sim_.run_until([&]() { return (img_.scb_status() & mask) == value; }, t_cmd))
    return fail("the SCB status never reached the expected value");
  return true;
}

bool IeDriver::run_cb(uint16_t cb) {
  img_.set_scb_cbl(cb);
  if (!issue_scb(uint16_t(ie::CUC_START << ie::SCB_CMD_CUC_LSB))) return false;
  if (!sim_.run_until([&]() { return (img_.cb_status(cb) & ie::CB_ST_C) != 0; },
                      t_cmd))
    return fail("the command block never completed");
  return ack_all();
}

bool IeDriver::nop() { return run_cb(img_.add_nop()); }

bool IeDriver::ia_setup(const MacAddr& mac) {
  mac_ = mac;
  return run_cb(img_.add_ia_setup(mac));
}

bool IeDriver::configure(const ie::Config& cfg) {
  return run_cb(img_.add_configure(cfg));
}

bool IeDriver::mc_setup(const std::vector<MacAddr>& list) {
  return run_cb(img_.add_mc_setup(list));
}

bool IeDriver::transmit(const EthFrame& f, uint16_t* cb_out) {
  uint16_t cb = img_.add_transmit(f, true);
  if (cb_out) *cb_out = cb;
  img_.set_scb_cbl(cb);
  if (!issue_scb(uint16_t(ie::CUC_START << ie::SCB_CMD_CUC_LSB))) return false;
  if (!sim_.run_until([&]() { return (img_.cb_status(cb) & ie::CB_ST_C) != 0; },
                      t_tx))
    return fail("the transmit command never completed");
  return ack_all();
}

bool IeDriver::ru_start() {
  if (img_.scb_rfa() == ie::NULL_PTR) return fail("no receive frame area built");
  if (!issue_scb(uint16_t(ie::RUC_START << ie::SCB_CMD_RUC_LSB))) return false;
  if (!sim_.run_until([this]() { return img_.scb_rus() == ie::RUS_READY; }, t_cmd))
    return fail("the receive unit never became ready");
  return true;
}

bool IeDriver::ru_abort() {
  return issue_scb(uint16_t(ie::RUC_ABORT << ie::SCB_CMD_RUC_LSB));
}

bool IeDriver::wait_rx(size_t n) {
  if (!sim_.run_until(
          [&]() {
            size_t done = 0;
            for (uint16_t rfd : img_.rfds())
              if (img_.mem().rd16(img_.addr_of(rfd)) & ie::RFD_ST_C) done++;
            return done >= n;
          },
          t_rx))
    return fail("the receive unit never completed enough frames");
  return true;
}

}  // namespace wtb
