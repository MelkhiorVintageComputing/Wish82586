// SPDX-License-Identifier: MIT

#include "sim.h"

#include <verilated_vcd_c.h>

#include <algorithm>

namespace wtb {

Sim::Sim(std::function<void()> eval) : eval_(std::move(eval)) {}

Sim::~Sim() = default;

Sim::Clock* Sim::add_clock(uint8_t* sig, u64 period_ps, const std::string& name,
                           u64 phase_ps) {
  clocks_.emplace_back(new Clock());
  Clock* c = clocks_.back().get();
  c->sig = sig;
  c->half_ps = period_ps / 2;
  c->next_ps = time_ps_ + phase_ps + c->half_ps;
  c->name = name;
  *sig = 0;
  return c;
}

void Sim::eval() { eval_(); }

void Sim::dump() {
  if (tfp_) tfp_->dump(static_cast<uint64_t>(time_ps_));
}

void Sim::flush_trace() {
  if (tfp_) tfp_->flush();
}

void Sim::step() {
  u64 next = 0;
  bool found = false;
  for (auto& c : clocks_) {
    if (!c->running) continue;
    if (!found || c->next_ps < next) {
      next = c->next_ps;
      found = true;
    }
  }
  if (!found) return;

  time_ps_ = next;

  // Toggle every clock whose edge falls on this instant.
  std::vector<Clock*> rising, falling;
  for (auto& c : clocks_) {
    if (!c->running || c->next_ps != time_ps_) continue;
    *c->sig = *c->sig ? 0 : 1;
    c->next_ps += c->half_ps;
    (*c->sig ? rising : falling).push_back(c.get());
  }

  eval_();

  // Callbacks observe the settled state and drive inputs for the next edge.
  for (Clock* c : rising)
    for (auto& cb : c->posedge_cbs) cb();
  for (Clock* c : falling)
    for (auto& cb : c->negedge_cbs) cb();

  if (!rising.empty() || !falling.empty()) eval_();
  dump();
}

void Sim::run_ps(u64 dt_ps) {
  const u64 end = time_ps_ + dt_ps;
  while (time_ps_ < end) step();
}

void Sim::run_cycles(Clock* c, u64 n) {
  for (u64 i = 0; i < n; i++) {
    const u64 target = c->next_ps + c->half_ps;  // one full period ahead
    while (time_ps_ < target) step();
  }
}

void Sim::run_posedges(Clock* c, u64 n) {
  u64 done = 0;
  while (done < n) {
    const uint8_t before = *c->sig;
    step();
    if (!before && *c->sig) done++;
  }
}

bool Sim::run_until(const std::function<bool()>& cond, u64 timeout_ps) {
  const u64 deadline = time_ps_ + timeout_ps;
  if (cond()) return true;
  while (time_ps_ < deadline) {
    step();
    if (cond()) return true;
  }
  return false;
}

}  // namespace wtb
