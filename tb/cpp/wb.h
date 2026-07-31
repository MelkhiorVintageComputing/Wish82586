// SPDX-License-Identifier: MIT
//
// Wishbone B4 classic bus functional models.
//
//   WbMem  - slave.  Models the shared memory the MAC DMAs into, with
//            configurable wait states, an optional error window and a full
//            access log for the scoreboard.
//   WbHost - master.  Models the host CPU poking the control registers.
//            Its accessors are blocking: they pump the simulation until the
//            transfer completes, so tests read like driver code.
//
// Both attach to the negative edge of the bus clock: they see the values the
// DUT drove at the preceding rising edge and set up whatever the DUT will
// sample at the next one.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "eth.h"
#include "sim.h"

namespace wtb {

// Signals of a Wishbone slave interface, as seen by a slave model.
struct WbSlavePorts {
  uint8_t* cyc = nullptr;
  uint8_t* stb = nullptr;
  uint8_t* we = nullptr;
  uint8_t* sel = nullptr;
  uint32_t* adr = nullptr;
  uint32_t* dat_w = nullptr;  // master -> slave
  uint32_t* dat_r = nullptr;  // slave -> master (driven by the model)
  uint8_t* ack = nullptr;     // driven by the model
  uint8_t* err = nullptr;     // driven by the model
};

// Signals of a Wishbone master interface, as seen by a master model.
struct WbMasterPorts {
  uint8_t* cyc = nullptr;  // driven by the model
  uint8_t* stb = nullptr;  // driven by the model
  uint8_t* we = nullptr;   // driven by the model
  uint8_t* sel = nullptr;  // driven by the model
  uint32_t* adr = nullptr; // driven by the model
  uint32_t* dat_w = nullptr;  // master -> slave, driven by the model
  uint32_t* dat_r = nullptr;  // slave -> master
  uint8_t* ack = nullptr;
  uint8_t* err = nullptr;
};

class WbMem {
 public:
  struct Access {
    u64 time_ps;
    bool write;
    uint32_t adr;
    uint32_t data;
    uint8_t sel;
  };

  WbMem(Sim& sim, Sim::Clock* clk, WbSlavePorts ports, size_t size_bytes,
        uint32_t base = 0);

  // ---- backdoor access, little endian like the 82586 sees memory ----------
  uint8_t rd8(uint32_t a) const;
  uint16_t rd16(uint32_t a) const;
  uint32_t rd24(uint32_t a) const;
  uint32_t rd32(uint32_t a) const;
  void wr8(uint32_t a, uint8_t v);
  void wr16(uint32_t a, uint16_t v);
  void wr24(uint32_t a, uint32_t v);
  void wr32(uint32_t a, uint32_t v);
  void write_block(uint32_t a, const Bytes& b);
  Bytes read_block(uint32_t a, size_t n) const;
  void clear(uint8_t pattern = 0);

  uint32_t base() const { return base_; }
  size_t size() const { return mem_.size(); }
  bool contains(uint32_t a, size_t n = 1) const {
    return a >= base_ && (uint64_t(a) + n) <= (uint64_t(base_) + mem_.size());
  }

  // ---- configuration ------------------------------------------------------
  void set_wait_states(int n) { wait_states_ = n; }
  // Accesses inside [lo, hi) answer with ERR instead of ACK.
  void set_error_window(uint32_t lo, uint32_t hi) {
    err_lo_ = lo;
    err_hi_ = hi;
  }

  // ---- observation --------------------------------------------------------
  const std::vector<Access>& log() const { return log_; }
  // Forgets the log and the counters, so everything below is "since here".
  void clear_log() {
    log_.clear();
    reads_ = 0;
    writes_ = 0;
  }
  size_t reads() const { return reads_; }
  size_t writes() const { return writes_; }
  bool oob_seen() const { return oob_seen_; }
  uint32_t oob_addr() const { return oob_addr_; }
  // Bus cycles seen since the last clear_log(), useful to assert the core is
  // quiet when it should be.
  size_t accesses() const { return reads_ + writes_; }

 private:
  void tick();

  Sim& sim_;
  WbSlavePorts p_;
  std::vector<uint8_t> mem_;
  uint32_t base_;
  int wait_states_ = 0;
  int wait_cnt_ = 0;
  uint32_t err_lo_ = 1, err_hi_ = 0;  // empty window
  std::vector<Access> log_;
  size_t reads_ = 0, writes_ = 0;
  bool oob_seen_ = false;
  uint32_t oob_addr_ = 0;
};

class WbHost {
 public:
  WbHost(Sim& sim, Sim::Clock* clk, WbMasterPorts ports);

  void write32(uint32_t adr, uint32_t data, uint8_t sel = 0xf);
  uint32_t read32(uint32_t adr);

  bool last_error() const { return err_; }
  bool last_timeout() const { return timeout_; }
  u64 timeout_ps = 10 * US;

 private:
  void tick();
  void run_transaction();
  void deassert();

  enum class St { Idle, Start, Wait };

  Sim& sim_;
  WbMasterPorts p_;
  St st_ = St::Idle;
  uint32_t adr_ = 0, wdata_ = 0, rdata_ = 0;
  uint8_t sel_ = 0xf;
  bool write_ = false;
  bool err_ = false;
  bool timeout_ = false;
};

}  // namespace wtb
