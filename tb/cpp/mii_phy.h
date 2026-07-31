// SPDX-License-Identifier: MIT
//
// MII / GMII PHY bus functional model.
//
// The PHY owns TX_CLK and RX_CLK - 25 MHz at 100 Mb/s, 2.5 MHz at 10 Mb/s,
// 125 MHz for GMII - and they are deliberately not phase aligned, so the MAC
// cannot get away with treating them as one clock.  (A real GMII MAC sources
// its own transmit clock; here the model drives it either way, which is the
// same thing as far as the RTL can tell.)
//
// A "symbol" below is what crosses the interface in one clock: a nibble on
// MII, a whole byte on GMII.
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

// Reassembles a symbol stream (preamble + SFD + frame) into a WireFrame.
class NibbleCapture {
 public:
  explicit NibbleCapture(int width = 4) : width_(width) {}
  void set_width(int width) { width_ = width; }
  void feed(bool dv, uint8_t sym, bool err, u64 time_ps);
  bool empty() const { return done_.empty(); }
  size_t count() const { return done_.size(); }
  WireFrame pop();
  const std::vector<WireFrame>& frames() const { return done_; }
  void clear() { done_.clear(); }

 private:
  void finish(u64 time_ps);

  int width_ = 4;
  bool active_ = false;
  std::vector<uint8_t> nibbles_;
  bool err_ = false;
  u64 start_ps_ = 0;
  std::vector<WireFrame> done_;
};

class MiiPhy {
 public:
  // The interface the model speaks.  G1000 is GMII: eight bits at 125 MHz.
  enum class Speed { M10, M100, G1000 };

  MiiPhy(Sim& sim, MiiPorts ports, Speed speed = Speed::M100);

  int width() const { return width_; }
  bool is_gmii() const { return width_ == 8; }

  Sim::Clock* tx_clock() const { return tx_clk_; }
  Sim::Clock* rx_clock() const { return rx_clk_; }
  u64 nibble_time_ps() const { return nibble_ps_; }

  // ---- receive path (PHY -> MAC) -----------------------------------------
  void inject(const EthFrame& f);            // adds a correct FCS
  void inject_bad_fcs(const EthFrame& f);    // corrupts the FCS
  void inject_wire(const Bytes& wire);       // exactly these bytes, FCS included
  // Raw symbols, preamble included automatically.  Used to build traffic the
  // frame level cannot express, such as a frame that ends half way through a
  // byte on MII.
  void inject_symbols(const std::vector<uint8_t>& symbols,
                      int preamble_symbols = -1);
  // Splits a byte stream into interface symbols, low nibble first on MII.
  std::vector<uint8_t> to_symbols(const Bytes& wire) const;
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
  int width_;

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

// Builds the symbol stream for a frame: preamble, SFD, then the frame itself.
std::vector<uint8_t> wire_to_symbols(const Bytes& wire, int width,
                                     int preamble_symbols = -1);

}  // namespace wtb
