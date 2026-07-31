// SPDX-License-Identifier: MIT

#include "mii_phy.h"

namespace wtb {

std::vector<uint8_t> wire_to_nibbles(const Bytes& wire, int preamble_nibbles) {
  std::vector<uint8_t> n;
  n.reserve(wire.size() * 2 + size_t(preamble_nibbles));
  for (int i = 0; i < preamble_nibbles; i++)
    n.push_back(i == preamble_nibbles - 1 ? 0xd : 0x5);  // ...0x55 0x55 0xd5
  for (uint8_t b : wire) {
    n.push_back(uint8_t(b & 0xf));
    n.push_back(uint8_t(b >> 4));
  }
  return n;
}

// ---------------------------------------------------------------------------
// NibbleCapture
// ---------------------------------------------------------------------------

void NibbleCapture::feed(bool dv, uint8_t nibble, bool err, u64 time_ps) {
  if (dv) {
    if (!active_) {
      active_ = true;
      nibbles_.clear();
      err_ = false;
      start_ps_ = time_ps;
    }
    nibbles_.push_back(uint8_t(nibble & 0xf));
    if (err) err_ = true;
  } else if (active_) {
    finish(time_ps);
  }
}

void NibbleCapture::finish(u64 time_ps) {
  active_ = false;
  WireFrame f;
  f.start_ps = start_ps_;
  f.end_ps = time_ps;
  f.rx_er = err_;

  // Strip the preamble: nibbles of 0x5 terminated by the 0xd of the SFD.
  size_t i = 0;
  while (i < nibbles_.size() && nibbles_[i] == 0x5) i++;
  f.preamble_nibbles = i;
  if (i < nibbles_.size() && nibbles_[i] == 0xd) {
    i++;
    f.preamble_ok = (f.preamble_nibbles >= 15);
  } else {
    // No SFD at all: keep everything so the test can see what went wrong.
    i = 0;
    f.preamble_ok = false;
  }

  const size_t data_nibbles = nibbles_.size() - i;
  f.dribble = (data_nibbles & 1) != 0;
  for (size_t k = 0; k + 1 < data_nibbles; k += 2)
    f.data.push_back(uint8_t(nibbles_[i + k] | (nibbles_[i + k + 1] << 4)));
  f.fcs_ok = eth_fcs_valid(f.data);
  done_.push_back(f);
  nibbles_.clear();
}

WireFrame NibbleCapture::pop() {
  WireFrame f = done_.front();
  done_.erase(done_.begin());
  return f;
}

// ---------------------------------------------------------------------------
// MiiPhy
// ---------------------------------------------------------------------------

MiiPhy::MiiPhy(Sim& sim, MiiPorts ports, Speed speed) : sim_(sim), p_(ports) {
  const u64 period = (speed == Speed::M100) ? 40 * NS : 400 * NS;
  nibble_ps_ = period;
  // The two clocks are independent; a deliberate skew keeps the MAC honest.
  tx_clk_ = sim_.add_clock(p_.tx_clk, period, "mii_tx_clk", 0);
  rx_clk_ = sim_.add_clock(p_.rx_clk, period, "mii_rx_clk", period / 8);

  *p_.rxd = 0;
  *p_.rx_dv = 0;
  *p_.rx_er = 0;
  *p_.crs = 0;
  *p_.col = 0;

  sim_.on_negedge(rx_clk_, [this]() { rx_tick(); });
  sim_.on_negedge(tx_clk_, [this]() { tx_tick(); });
}

void MiiPhy::inject(const EthFrame& f) { inject_wire(f.to_wire(true, true)); }

void MiiPhy::inject_bad_fcs(const EthFrame& f) {
  Bytes w = f.to_wire(true, true);
  w[w.size() - 1] ^= 0xff;
  inject_wire(w);
}

void MiiPhy::inject_wire(const Bytes& wire) {
  rx_queue_.push_back(wire_to_nibbles(wire, 16));
}

void MiiPhy::inject_nibbles(const std::vector<uint8_t>& nibbles,
                            int preamble_nibbles) {
  std::vector<uint8_t> s;
  for (int i = 0; i < preamble_nibbles; i++)
    s.push_back(i == preamble_nibbles - 1 ? 0xd : 0x5);
  s.insert(s.end(), nibbles.begin(), nibbles.end());
  rx_queue_.push_back(s);
}

void MiiPhy::start_next_rx() {
  rx_cur_ = rx_queue_.front();
  rx_queue_.pop_front();
  rx_pos_ = 0;
  rx_active_ = true;
  rx_er_pending_ = rx_er_nibble_;
  rx_er_nibble_ = -1;
  rx_sent_++;
}

void MiiPhy::rx_tick() {
  // Drive the pins for the coming clock period.
  if (rx_active_) {
    if (rx_pos_ < rx_cur_.size()) {
      *p_.rxd = rx_cur_[rx_pos_];
      *p_.rx_dv = 1;
      *p_.rx_er = (rx_er_pending_ >= 0 && size_t(rx_er_pending_) == rx_pos_) ? 1 : 0;
      rx_pos_++;
    } else {
      rx_active_ = false;
      rx_gap_ = ipg_nibbles_;
      *p_.rxd = 0;
      *p_.rx_dv = 0;
      *p_.rx_er = 0;
    }
  } else if (rx_gap_ > 0) {
    rx_gap_--;
  } else if (!rx_queue_.empty()) {
    start_next_rx();
    *p_.rxd = rx_cur_[0];
    *p_.rx_dv = 1;
    *p_.rx_er = 0;
    rx_pos_ = 1;
  }

  // Carrier sense follows receive activity, plus our own transmission when
  // the medium is half duplex.
  *p_.crs = (rx_active_ || (!full_duplex_ && tx_active_)) ? 1 : 0;

  // A collision is receive and transmit overlapping, or one forced by a test.
  bool col = !full_duplex_ && rx_active_ && tx_active_;
  if (col_cnt_ > 0) {
    col = true;
    col_cnt_--;
  }
  if (*p_.col == 0 && col) collisions_++;
  *p_.col = col ? 1 : 0;

  rx_mon_.feed(*p_.rx_dv != 0, *p_.rxd, *p_.rx_er != 0, sim_.time_ps());
}

void MiiPhy::tx_tick() {
  const bool en = *p_.tx_en != 0;

  if (en && !tx_active_) {
    tx_active_ = true;
    tx_nibbles_ = 0;
  }
  if (en) {
    tx_nibbles_++;
    if (col_at_ >= 0 && tx_nibbles_ >= col_at_) {
      col_at_ = -1;
      col_cnt_ = col_len_;
    }
  } else if (tx_active_) {
    tx_active_ = false;
  }

  tx_cap_.feed(en, *p_.txd, *p_.tx_er != 0, sim_.time_ps());
}

}  // namespace wtb
