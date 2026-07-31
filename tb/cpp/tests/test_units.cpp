// SPDX-License-Identifier: MIT
//
// Unit tests for the RTL leaf modules, checked against the software models in
// the testbench.

#include "test.h"

#include "Vtb_top.h"

namespace wtb {

namespace {

// Feeds a byte stream through the nibble-wide CRC in the DUT, low nibble
// first, exactly as MII puts it on the wire.
uint32_t rtl_fcs_nibble(Env& env, const Bytes& data) {
  Vtb_top* d = env.dut();
  d->crc4_en = 0;
  d->crc4_init = 1;
  env.tick(1);
  d->crc4_init = 0;
  for (uint8_t b : data) {
    d->crc4_data = uint8_t(b & 0xf);
    d->crc4_en = 1;
    env.tick(1);
    d->crc4_data = uint8_t(b >> 4);
    env.tick(1);
  }
  d->crc4_en = 0;
  env.tick(1);
  return d->crc4_fcs;
}

uint32_t rtl_fcs_byte(Env& env, const Bytes& data) {
  Vtb_top* d = env.dut();
  d->crc8_en = 0;
  d->crc8_init = 1;
  env.tick(1);
  d->crc8_init = 0;
  for (uint8_t b : data) {
    d->crc8_data = b;
    d->crc8_en = 1;
    env.tick(1);
  }
  d->crc8_en = 0;
  env.tick(1);
  return d->crc8_fcs;
}

}  // namespace

TEST(unit_crc32_nibble_matches_model) {
  const Bytes check{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  CHECK_EQ(rtl_fcs_nibble(env, check), 0xcbf43926u);

  for (size_t len : {size_t(1), size_t(14), size_t(60), size_t(64), size_t(511)}) {
    Bytes data = random_payload(len, uint32_t(len * 7 + 1));
    CHECK_EQ(rtl_fcs_nibble(env, data), eth_fcs(data));
  }

  EthFrame f(env.peer_mac(), env.local_mac(), 0x0800, random_payload(46, 3));
  Bytes wire = f.to_wire(false, true);
  CHECK_EQ(rtl_fcs_nibble(env, wire), eth_fcs(wire));
}

TEST(unit_crc32_byte_matches_model) {
  const Bytes check{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  CHECK_EQ(rtl_fcs_byte(env, check), 0xcbf43926u);

  for (size_t len : {size_t(1), size_t(60), size_t(1514)}) {
    Bytes data = random_payload(len, uint32_t(len + 5));
    CHECK_EQ(rtl_fcs_byte(env, data), eth_fcs(data));
  }
}

TEST(unit_crc32_recognises_a_good_frame) {
  // Feeding a frame plus its FCS leaves the magic residue behind, which is how
  // the receive path will decide whether a frame is intact.
  EthFrame f(env.peer_mac(), env.local_mac(), 0x0800, random_payload(100, 8));
  Bytes wire = f.to_wire(true, true);

  rtl_fcs_nibble(env, wire);
  CHECK_MSG(env.dut()->crc4_ok, "a good frame was not recognised by crc32_eth");

  wire[30] ^= 0x80;
  rtl_fcs_nibble(env, wire);
  CHECK_MSG(!env.dut()->crc4_ok, "a corrupted frame was accepted by crc32_eth");
}

TEST(unit_sync_fifo) {
  Vtb_top* d = env.dut();
  d->fifo_flush = 0;
  d->fifo_wr_en = 0;
  d->fifo_rd_en = 0;
  env.tick(2);

  CHECK(d->fifo_empty);
  CHECK(!d->fifo_full);
  CHECK_EQ(d->fifo_level, 0);

  // Fill it right up.
  for (int i = 0; i < 16; i++) {
    CHECK_MSG(!d->fifo_full, "the FIFO reported full too early");
    d->fifo_wr_data = uint8_t(0xa0 + i);
    d->fifo_wr_en = 1;
    env.tick(1);
  }
  d->fifo_wr_en = 0;
  env.tick(1);
  CHECK(d->fifo_full);
  CHECK(!d->fifo_empty);
  CHECK_EQ(d->fifo_level, 16);

  // A write into a full FIFO is dropped, not wrapped around.
  d->fifo_wr_data = 0xff;
  d->fifo_wr_en = 1;
  env.tick(1);
  d->fifo_wr_en = 0;
  env.tick(1);
  CHECK_EQ(d->fifo_level, 16);

  // Drain it: first word fall through, so the data is there before rd_en.
  for (int i = 0; i < 16; i++) {
    CHECK_EQ(d->fifo_rd_data, uint8_t(0xa0 + i));
    d->fifo_rd_en = 1;
    env.tick(1);
  }
  d->fifo_rd_en = 0;
  env.tick(1);
  CHECK(d->fifo_empty);
  CHECK(!d->fifo_full);
  CHECK_EQ(d->fifo_level, 0);

  // Flush drops whatever is in flight.
  for (int i = 0; i < 5; i++) {
    d->fifo_wr_data = uint8_t(i);
    d->fifo_wr_en = 1;
    env.tick(1);
  }
  d->fifo_wr_en = 0;
  env.tick(1);
  CHECK_EQ(d->fifo_level, 5);
  d->fifo_flush = 1;
  env.tick(1);
  d->fifo_flush = 0;
  env.tick(1);
  CHECK(d->fifo_empty);
  CHECK_EQ(d->fifo_level, 0);
}

}  // namespace wtb
