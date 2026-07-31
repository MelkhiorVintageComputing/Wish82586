// SPDX-License-Identifier: MIT
//
// MII PHY bus functional model.
//
// The PHY owns TX_CLK and RX_CLK (25 MHz at 100 Mb/s, 2.5 MHz at 10 Mb/s) and
// they are deliberately not phase aligned, so the MAC cannot get away with
// treating them as one clock.
//
//   RX: frames queued with inject() are sent preamble + SFD + data + FCS.
//       Malformed traffic is available too: bad FCS, dribble nibbles, runts,
//       RX_ER assertion, short preamble.
//   TX: everything the MAC transmits is captured, the preamble checked and
//       stripped and the FCS verified.
//   Half duplex: CRS follows carrier, COL can be forced at a chosen point in
//       the transmission to exercise the backoff logic.

#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "eth.h"
#include "sim.h"

namespace wtb {

struct MiiPorts {
  uint8_t* tx_clk = nullptr;  // driven by the model
  uint8_t* txd = nullptr;
  uint8_t* tx_en = nullptr;
  uint8_t* tx_er = nullptr;
  uint8_t* rx_clk = nullptr;  // driven by the model
  uint8_t* rxd = nullptr;     // driven by the model
  uint8_t* rx_dv = nullptr;   // driven by the model
  uint8_t* rx_er = nullptr;   // driven by the model
  uint8_t* crs = nullptr;     // driven by the model
  uint8_t* col = nullptr;     // driven by the model
};

// Reassembles a nibble stream (preamble + SFD + frame) into a WireFrame.
class NibbleCapture {
 public:
  void feed(bool dv, uint8_t nibble, bool err, u64 time_ps);
  bool empty() const { return done_.empty(); }
  size_t count() const { return done_.size(); }
  WireFrame pop();
  const std::vector<WireFrame>& frames() const { return done_; }
  void clear() { done_.clear(); }

 private:
  void finish(u64 time_ps);

  bool active_ = false;
  std::vector<uint8_t> nibbles_;
  bool err_ = false;
  u64 start_ps_ = 0;
  std::vector<WireFrame> done_;
};

class MiiPhy {
 public:
  enum class Speed { M10, M100 };

  MiiPhy(Sim& sim, MiiPorts ports, Speed speed = Speed::M100);

  Sim::Clock* tx_clock() const { return tx_clk_; }
  Sim::Clock* rx_clock() const { return rx_clk_; }
  u64 nibble_time_ps() const { return nibble_ps_; }

  // ---- receive path (PHY -> MAC) -----------------------------------------
  void inject(const EthFrame& f);            // adds a correct FCS
  void inject_bad_fcs(const EthFrame& f);    // corrupts the FCS
  void inject_wire(const Bytes& wire);       // exactly these bytes, FCS included
  void inject_nibbles(const std::vector<uint8_t>& nibbles,
                      int preamble_nibbles = 16);
  void set_next_rx_error(int nibble_index) { rx_er_nibble_ = nibble_index; }
  void set_ipg_nibbles(int n) { ipg_nibbles_ = n; }
  bool rx_busy() const { return rx_active_ || !rx_queue_.empty(); }
  size_t rx_pending() const { return rx_queue_.size(); }
  size_t rx_sent() const { return rx_sent_; }
  // What the model actually drove on the RX pins, for self checking.
  NibbleCapture& rx_monitor() { return rx_mon_; }

  // ---- transmit path (MAC -> PHY) ----------------------------------------
  bool has_tx() const { return !tx_cap_.empty(); }
  size_t tx_count() const { return tx_cap_.count(); }
  WireFrame pop_tx() { return tx_cap_.pop(); }
  const std::vector<WireFrame>& tx_frames() const { return tx_cap_.frames(); }
  bool tx_active() const { return tx_active_; }

  // ---- half duplex --------------------------------------------------------
  void set_full_duplex(bool on) { full_duplex_ = on; }
  // Assert COL after n nibbles of transmission; -1 disables.  It stays armed
  // until it fires, so a retry sees a clean medium.
  void force_collision(int nibbles_after_start) { col_at_ = nibbles_after_start; }
  void set_collision_length(int nibbles) { col_len_ = nibbles; }
  int collisions() const { return collisions_; }

 private:
  void rx_tick();
  void tx_tick();
  void start_next_rx();

  Sim& sim_;
  MiiPorts p_;
  Sim::Clock* tx_clk_;
  Sim::Clock* rx_clk_;
  u64 nibble_ps_;

  // receive
  std::deque<std::vector<uint8_t>> rx_queue_;   // nibble streams, preamble included
  std::vector<uint8_t> rx_cur_;
  size_t rx_pos_ = 0;
  bool rx_active_ = false;
  int rx_gap_ = 0;
  int ipg_nibbles_ = 24;      // 96 bit times
  int rx_er_nibble_ = -1;     // index inside the current stream, -1 = none
  int rx_er_pending_ = -1;
  size_t rx_sent_ = 0;
  NibbleCapture rx_mon_;

  // transmit
  NibbleCapture tx_cap_;
  bool tx_active_ = false;
  int tx_nibbles_ = 0;

  // half duplex
  bool full_duplex_ = false;
  int col_at_ = -1;
  int col_len_ = 8;
  int col_cnt_ = 0;
  int collisions_ = 0;
};

// Builds the nibble stream for a frame: preamble, SFD, then the bytes low
// nibble first.
std::vector<uint8_t> wire_to_nibbles(const Bytes& wire, int preamble_nibbles = 16);

}  // namespace wtb
