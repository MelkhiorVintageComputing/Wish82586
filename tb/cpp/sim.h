// SPDX-License-Identifier: MIT
//
// Tiny event-driven kernel around a Verilated model.
//
// The design has three clocks that are not multiples of each other (system
// clock, MII TX_CLK, MII RX_CLK), so instead of ticking at a fixed resolution
// the kernel jumps from clock edge to clock edge.
//
// Testbench components register their callbacks on the *negative* edge of the
// clock they belong to.  At a negedge the outputs a module produced at the
// preceding posedge are stable, and anything driven takes effect at the next
// posedge - which is exactly the cycle-level semantics of RTL, without any of
// the races that come from poking signals around a posedge.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class VerilatedVcdC;

namespace wtb {

using u64 = uint64_t;

constexpr u64 NS = 1000;        // picoseconds per nanosecond
constexpr u64 US = 1000 * NS;
constexpr u64 MS = 1000 * US;

class Sim {
 public:
  struct Clock {
    uint8_t* sig = nullptr;
    u64 half_ps = 0;
    u64 next_ps = 0;
    bool running = true;
    std::string name;
    std::vector<std::function<void()>> posedge_cbs;
    std::vector<std::function<void()>> negedge_cbs;
  };

  explicit Sim(std::function<void()> eval);
  ~Sim();

  Sim(const Sim&) = delete;
  Sim& operator=(const Sim&) = delete;

  // period_ps is the full period; the clock starts low and first rises at
  // phase_ps + half period.
  Clock* add_clock(uint8_t* sig, u64 period_ps, const std::string& name,
                   u64 phase_ps = 0);

  void on_posedge(Clock* c, std::function<void()> cb) {
    c->posedge_cbs.push_back(std::move(cb));
  }
  void on_negedge(Clock* c, std::function<void()> cb) {
    c->negedge_cbs.push_back(std::move(cb));
  }

  void step();                       // advance to the next clock edge
  void run_ps(u64 dt_ps);
  void run_cycles(Clock* c, u64 n);
  void run_posedges(Clock* c, u64 n); // stops just after the n-th rising edge
  // Runs until cond() holds or the timeout expires; true if cond() held.
  bool run_until(const std::function<bool()>& cond, u64 timeout_ps);

  u64 time_ps() const { return time_ps_; }
  double time_ns() const { return double(time_ps_) / 1000.0; }
  void eval();

  void set_trace(VerilatedVcdC* tfp) { tfp_ = tfp; }
  void flush_trace();

 private:
  void dump();

  std::function<void()> eval_;
  std::vector<std::unique_ptr<Clock>> clocks_;
  u64 time_ps_ = 0;
  VerilatedVcdC* tfp_ = nullptr;
};

}  // namespace wtb
