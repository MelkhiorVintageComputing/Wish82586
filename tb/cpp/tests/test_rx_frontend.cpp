// SPDX-License-Identifier: MIT
//
// Unit tests for the MII receive front end and the clock crossing behind it.
//
// The front end instance in tb_top hangs off the same pins the DUT sees, so it
// is driven by the real PHY model rather than by hand-made waveforms.

#include "test.h"

#include "Vtb_top.h"

namespace wtb {

namespace {

// One word as it comes out of the front end.
struct RxWord {
  bool end;
  uint8_t err;    // {crc/rx_er, dribble, overrun}
  uint8_t data;
};

// Collects everything the front end emits until it has closed `frames` frames.
std::vector<RxWord> capture(Env& env, size_t frames, u64 timeout = 2 * MS) {
  std::vector<RxWord> out;
  size_t ends = 0;
  Vtb_top* d = env.dut();
  // The front end drives its FIFO port on the receive clock, so watch that.
  env.sim().on_negedge(env.phy().rx_clock(), [&]() {
    if (d->rxfe_wr) {
      RxWord w;
      w.end = (d->rxfe_data >> 11) & 1;
      w.err = uint8_t((d->rxfe_data >> 8) & 7);
      w.data = uint8_t(d->rxfe_data & 0xff);
      out.push_back(w);
      if (w.end) ends++;
    }
  });
  const bool ok = env.sim().run_until([&]() { return ends >= frames; }, timeout);
  CHECK_MSG(ok, "the receive front end never closed the expected frames");
  return out;
}

Bytes data_of(const std::vector<RxWord>& w) {
  Bytes b;
  for (const RxWord& x : w)
    if (!x.end) b.push_back(x.data);
  return b;
}

const RxWord& end_of(const std::vector<RxWord>& w) {
  for (size_t i = w.size(); i-- > 0;)
    if (w[i].end) return w[i];
  throw TestFailure("no end word was emitted");
}

constexpr uint8_t ERR_BAD = 4;      // FCS wrong or RX_ER seen
constexpr uint8_t ERR_DRIBBLE = 2;
constexpr uint8_t ERR_OVERRUN = 1;

}  // namespace

TEST(rxfe_good_frame_without_fcs) {
  EthFrame f(env.local_mac(), env.peer_mac(), 0x0800, random_payload(100, 21));
  env.phy().inject(f);

  std::vector<RxWord> w = capture(env, 1);
  // The FCS is the chip's business, not the driver's: it must not appear.
  CHECK_EQ(data_of(w), f.to_wire(false, true));
  CHECK_EQ(end_of(w).err, 0);
}

TEST(rxfe_minimum_and_maximum_length) {
  EthFrame small(env.local_mac(), env.peer_mac(), 0x0800, Bytes());
  env.phy().inject(small);
  std::vector<RxWord> w = capture(env, 1);
  CHECK_EQ(data_of(w).size(), ETH_MIN_FRAME);
  CHECK_EQ(end_of(w).err, 0);

  EthFrame big(env.local_mac(), env.peer_mac(), 0x0800, random_payload(1500, 22));
  env.phy().inject(big);
  w = capture(env, 1);
  CHECK_EQ(data_of(w).size(), ETH_MAX_FRAME);
  CHECK_EQ(end_of(w).err, 0);
  CHECK_EQ(data_of(w), big.to_wire(false, false));
}

TEST(rxfe_reports_a_bad_fcs) {
  EthFrame f(env.local_mac(), env.peer_mac(), 0x0800, random_payload(64, 23));
  env.phy().inject_bad_fcs(f);

  std::vector<RxWord> w = capture(env, 1);
  CHECK_EQ(end_of(w).err & ERR_BAD, ERR_BAD);
  // The frame is still handed on; whether to keep it is the receive unit's
  // decision, not the front end's.
  CHECK_EQ(data_of(w), f.to_wire(false, true));
}

TEST(rxfe_reports_a_dribble_nibble) {
  EthFrame f(env.local_mac(), env.peer_mac(), 0x0800, random_payload(60, 24));
  Bytes wire = f.to_wire(true, true);
  std::vector<uint8_t> nibbles;
  for (uint8_t b : wire) {
    nibbles.push_back(uint8_t(b & 0xf));
    nibbles.push_back(uint8_t(b >> 4));
  }
  nibbles.push_back(0x7);                 // the odd nibble
  env.phy().inject_nibbles(nibbles);

  std::vector<RxWord> w = capture(env, 1);
  CHECK_EQ(end_of(w).err & ERR_DRIBBLE, ERR_DRIBBLE);
  CHECK_EQ(data_of(w), f.to_wire(false, true));   // the odd nibble is dropped
}

TEST(rxfe_reports_rx_er) {
  EthFrame f(env.local_mac(), env.peer_mac(), 0x0800, random_payload(64, 25));
  env.phy().set_next_rx_error(60);
  env.phy().inject(f);

  std::vector<RxWord> w = capture(env, 1);
  CHECK_EQ(end_of(w).err & ERR_BAD, ERR_BAD);
}

TEST(rxfe_back_to_back_frames) {
  std::vector<EthFrame> sent;
  for (int i = 0; i < 3; i++) {
    EthFrame f(env.local_mac(), env.peer_mac(), uint16_t(0x0800 + i),
               random_payload(size_t(46 + i * 64), uint32_t(30 + i)));
    sent.push_back(f);
    env.phy().inject(f);
  }

  std::vector<RxWord> w = capture(env, 3);

  // Split the stream back into frames on the end markers.
  std::vector<Bytes> got;
  Bytes cur;
  for (const RxWord& x : w) {
    if (x.end) {
      CHECK_EQ(x.err, 0);
      got.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(x.data);
    }
  }
  CHECK_EQ(got.size(), size_t(3));
  for (size_t i = 0; i < got.size(); i++) CHECK_EQ(got[i], sent[i].to_wire(false, true));
}

TEST(rxfe_flags_an_overrun) {
  // Hold the FIFO full for the whole frame: no byte can be taken, and the
  // front end has to say so rather than quietly losing data.
  env.dut()->rxfe_full = 1;
  EthFrame f(env.local_mac(), env.peer_mac(), 0x0800, random_payload(64, 26));
  env.phy().inject(f);

  size_t seen = 0;
  env.sim().on_negedge(env.phy().rx_clock(), [&]() {
    if (env.dut()->rxfe_wr) seen++;
  });
  // Wait for the frame to go by on the wire, then let the end word out.
  CHECK(env.sim().run_until([&]() { return !env.phy().rx_busy(); }, 2 * MS));
  env.dut()->rxfe_full = 0;

  std::vector<RxWord> w = capture(env, 1);
  CHECK_EQ(end_of(w).err & ERR_OVERRUN, ERR_OVERRUN);
  (void)seen;
}

// ---------------------------------------------------------------------------
// The clock crossing itself
// ---------------------------------------------------------------------------

TEST(async_fifo_crosses_clocks_in_order) {
  Vtb_top* d = env.dut();
  Sim::Clock* wclk = env.phy().rx_clock();

  d->afifo_wr_en = 0;
  d->afifo_rd_en = 0;
  env.tick(4);
  CHECK(d->afifo_rempty);
  CHECK(!d->afifo_wfull);

  // Write on the PHY clock.
  for (int i = 0; i < 12; i++) {
    d->afifo_wr_data = uint16_t(0x100 + i);
    d->afifo_wr_en = 1;
    env.sim().run_posedges(wclk, 1);
  }
  d->afifo_wr_en = 0;

  // Read on the system clock, one word per cycle, so each pop is recorded
  // exactly once.
  std::vector<uint16_t> got;
  env.sim().on_negedge(env.sysclk(), [&]() {
    if (!d->afifo_rempty && got.size() < 12) {
      got.push_back(d->afifo_rd_data);
      d->afifo_rd_en = 1;
    } else {
      d->afifo_rd_en = 0;
    }
  });
  const bool ok = env.sim().run_until([&]() { return got.size() == 12; }, 100 * US);
  CHECK_MSG(ok, "words written on the PHY clock never arrived on the system clock");
  for (int i = 0; i < 12; i++) CHECK_EQ(got[size_t(i)], uint16_t(0x100 + i));

  env.tick(10);
  CHECK(d->afifo_rempty);
}

TEST(async_fifo_reports_full) {
  Vtb_top* d = env.dut();
  Sim::Clock* wclk = env.phy().rx_clock();
  d->afifo_rd_en = 0;

  // Depth is 16; nothing is being read, so it must stop accepting.
  for (int i = 0; i < 24; i++) {
    d->afifo_wr_data = uint16_t(i);
    d->afifo_wr_en = 1;
    env.sim().run_posedges(wclk, 1);
  }
  d->afifo_wr_en = 0;
  env.sim().run_posedges(wclk, 2);
  CHECK_MSG(d->afifo_wfull, "the FIFO accepted more than its depth");

  // What did get in is still in order and intact.
  std::vector<uint16_t> got;
  env.sim().on_negedge(env.sysclk(), [&]() {
    if (!d->afifo_rempty && got.size() < 16) {
      got.push_back(d->afifo_rd_data);
      d->afifo_rd_en = 1;
    } else {
      d->afifo_rd_en = 0;
    }
  });
  env.sim().run_until([&]() { return got.size() >= 16; }, 100 * US);
  CHECK_EQ(got.size(), size_t(16));
  for (int i = 0; i < 16; i++) CHECK_EQ(got[size_t(i)], uint16_t(i));
}

}  // namespace wtb
