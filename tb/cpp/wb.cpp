// SPDX-License-Identifier: MIT

#include "wb.h"

namespace wtb {

// ---------------------------------------------------------------------------
// WbMem
// ---------------------------------------------------------------------------

WbMem::WbMem(Sim& sim, Sim::Clock* clk, WbSlavePorts ports, size_t size_bytes,
             uint32_t base)
    : sim_(sim), p_(ports), mem_(size_bytes, 0), base_(base) {
  *p_.ack = 0;
  *p_.err = 0;
  *p_.dat_r = 0;
  sim_.on_negedge(clk, [this]() { tick(); });
}

uint8_t WbMem::rd8(uint32_t a) const {
  if (!contains(a)) return 0;
  return mem_[a - base_];
}

uint16_t WbMem::rd16(uint32_t a) const {
  return uint16_t(rd8(a) | (uint16_t(rd8(a + 1)) << 8));
}

uint32_t WbMem::rd24(uint32_t a) const {
  return uint32_t(rd16(a)) | (uint32_t(rd8(a + 2)) << 16);
}

uint32_t WbMem::rd32(uint32_t a) const {
  return uint32_t(rd16(a)) | (uint32_t(rd16(a + 2)) << 16);
}

void WbMem::wr8(uint32_t a, uint8_t v) {
  if (!contains(a)) {
    oob_seen_ = true;
    oob_addr_ = a;
    return;
  }
  mem_[a - base_] = v;
}

void WbMem::wr16(uint32_t a, uint16_t v) {
  wr8(a, uint8_t(v & 0xff));
  wr8(a + 1, uint8_t(v >> 8));
}

void WbMem::wr24(uint32_t a, uint32_t v) {
  wr16(a, uint16_t(v & 0xffff));
  wr8(a + 2, uint8_t((v >> 16) & 0xff));
}

void WbMem::wr32(uint32_t a, uint32_t v) {
  wr16(a, uint16_t(v & 0xffff));
  wr16(a + 2, uint16_t(v >> 16));
}

void WbMem::write_block(uint32_t a, const Bytes& b) {
  for (size_t i = 0; i < b.size(); i++) wr8(uint32_t(a + i), b[i]);
}

Bytes WbMem::read_block(uint32_t a, size_t n) const {
  Bytes out(n);
  for (size_t i = 0; i < n; i++) out[i] = rd8(uint32_t(a + i));
  return out;
}

void WbMem::clear(uint8_t pattern) {
  for (auto& b : mem_) b = pattern;
}

void WbMem::tick() {
  // A cycle acknowledged in the previous clock period is over.
  if (*p_.ack || *p_.err) {
    *p_.ack = 0;
    *p_.err = 0;
    wait_cnt_ = wait_states_;
    return;
  }

  if (!(*p_.cyc && *p_.stb)) {
    wait_cnt_ = wait_states_;
    return;
  }

  if (wait_cnt_ > 0) {
    wait_cnt_--;
    return;
  }

  const uint32_t adr = *p_.adr & ~uint32_t(3);  // 32-bit port, byte address
  const uint8_t sel = *p_.sel & 0xf;
  const bool write = *p_.we != 0;

  if (err_lo_ < err_hi_ && adr >= err_lo_ && adr < err_hi_) {
    *p_.err = 1;
    return;
  }

  if (!contains(adr, 4)) {
    oob_seen_ = true;
    oob_addr_ = adr;
  }

  uint32_t data;
  if (write) {
    data = *p_.dat_w;
    for (int b = 0; b < 4; b++)
      if (sel & (1u << b)) wr8(adr + uint32_t(b), uint8_t((data >> (8 * b)) & 0xff));
    writes_++;
  } else {
    data = rd32(adr);
    *p_.dat_r = data;
    reads_++;
  }
  log_.push_back(Access{sim_.time_ps(), write, adr, data, sel});
  *p_.ack = 1;
}

// ---------------------------------------------------------------------------
// WbHost
// ---------------------------------------------------------------------------

WbHost::WbHost(Sim& sim, Sim::Clock* clk, WbMasterPorts ports)
    : sim_(sim), p_(ports) {
  deassert();
  sim_.on_negedge(clk, [this]() { tick(); });
}

void WbHost::deassert() {
  *p_.cyc = 0;
  *p_.stb = 0;
  *p_.we = 0;
  *p_.sel = 0;
  *p_.adr = 0;
  *p_.dat_w = 0;
}

void WbHost::tick() {
  switch (st_) {
    case St::Idle:
      break;
    case St::Start:
      *p_.cyc = 1;
      *p_.stb = 1;
      *p_.we = write_ ? 1 : 0;
      *p_.sel = sel_;
      *p_.adr = adr_;
      *p_.dat_w = wdata_;
      st_ = St::Wait;
      break;
    case St::Wait:
      if (*p_.err) {
        err_ = true;
        deassert();
        st_ = St::Idle;
      } else if (*p_.ack) {
        rdata_ = *p_.dat_r;
        deassert();
        st_ = St::Idle;
      }
      break;
  }
}

void WbHost::run_transaction() {
  err_ = false;
  timeout_ = false;
  st_ = St::Start;
  const bool ok = sim_.run_until([this]() { return st_ == St::Idle; }, timeout_ps);
  if (!ok) {
    timeout_ = true;
    deassert();
    st_ = St::Idle;
  }
}

void WbHost::write32(uint32_t adr, uint32_t data, uint8_t sel) {
  adr_ = adr;
  wdata_ = data;
  sel_ = sel;
  write_ = true;
  run_transaction();
}

uint32_t WbHost::read32(uint32_t adr) {
  adr_ = adr;
  wdata_ = 0;
  sel_ = 0xf;
  write_ = false;
  rdata_ = 0;
  run_transaction();
  return rdata_;
}

}  // namespace wtb
