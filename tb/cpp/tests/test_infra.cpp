// SPDX-License-Identifier: MIT
//
// Tests of the testbench itself.  If these fail, nothing said about the DUT
// can be trusted.

#include "test.h"

#include "Vtb_top.h"

namespace wtb {

// ---------------------------------------------------------------------------
// Ethernet helpers
// ---------------------------------------------------------------------------

TEST(infra_eth_fcs) {
  (void)env;
  // The classic check value: CRC-32 of "123456789" is 0xCBF43926.
  Bytes s{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  CHECK_EQ(eth_fcs(s), 0xcbf43926u);

  EthFrame f(MacAddr::broadcast(), MacAddr(0x02, 0, 0, 0, 0, 1), 0x0800,
             random_payload(46, 7));
  Bytes wire = f.to_wire(true, true);
  CHECK_EQ(wire.size(), size_t(64));
  CHECK(eth_fcs_valid(wire));

  wire[20] ^= 0x01;
  CHECK(!eth_fcs_valid(wire));

  // Round trip through the wire format.
  EthFrame back = EthFrame::from_wire(f.to_wire(true, false), true);
  CHECK_EQ(back.dst, f.dst);
  CHECK_EQ(back.src, f.src);
  CHECK_EQ(back.type_len, f.type_len);
  CHECK_EQ(back.payload, f.payload);
}

TEST(infra_frame_padding) {
  (void)env;
  EthFrame f(MacAddr::broadcast(), MacAddr(0x02, 0, 0, 0, 0, 1), 0x0806,
             random_payload(10, 3));
  CHECK_EQ(f.to_wire(false, true).size(), ETH_MIN_FRAME);
  CHECK_EQ(f.to_wire(true, true).size(), ETH_MIN_FRAME + 4);
  CHECK_EQ(f.to_wire(false, false).size(), size_t(24));
}

// ---------------------------------------------------------------------------
// Shared memory model
// ---------------------------------------------------------------------------

TEST(infra_wb_mem_backdoor) {
  WbMem& m = env.mem();
  m.wr32(0x100, 0x11223344);
  CHECK_EQ(m.rd8(0x100), 0x44);   // little endian, like the 82586 sees memory
  CHECK_EQ(m.rd8(0x103), 0x11);
  CHECK_EQ(m.rd16(0x100), 0x3344);
  CHECK_EQ(m.rd24(0x100), 0x223344u);
  CHECK_EQ(m.rd32(0x100), 0x11223344u);

  m.wr24(0x200, 0x00abcdef);
  CHECK_EQ(m.rd24(0x200), 0x00abcdefu);
  CHECK_EQ(m.rd8(0x203), 0);

  Bytes b = random_payload(37, 11);
  m.write_block(0x300, b);
  CHECK_EQ(m.read_block(0x300, b.size()), b);

  // Out of range accesses are flagged rather than silently wrapping.
  CHECK(!m.oob_seen());
  m.wr8(uint32_t(env.cfg().mem_size + 4), 0xaa);
  CHECK(m.oob_seen());
}

// ---------------------------------------------------------------------------
// Control register access over Wishbone
// ---------------------------------------------------------------------------

TEST(infra_csr_access) {
  CHECK_EQ(env.host().read32(csr::ID), csr::ID_VALUE);
  CHECK(!env.host().last_timeout());
  CHECK(!env.host().last_error());

  // The core comes out of power on reset held in reset.
  CHECK_EQ(env.host().read32(csr::CTRL) & csr::CTRL_RST, csr::CTRL_RST);

  // The SCP address is programmable and defaults to the historical 0xFFFFF6.
  CHECK_EQ(env.host().read32(csr::SCP_ADDR), ie::SCP_ADDR_DEFAULT);
  env.host().write32(csr::SCP_ADDR, 0x000ffff6);
  CHECK_EQ(env.host().read32(csr::SCP_ADDR), 0x000ffff6u);

  // Channel attention is a pulse and always reads back as zero.  The core is
  // still held in reset here, so it ignores it.
  env.host().write32(csr::CTRL, csr::CTRL_RST | csr::CTRL_CA);
  CHECK_EQ(env.host().read32(csr::CTRL) & csr::CTRL_CA, 0u);
  CHECK_EQ(env.host().read32(csr::CTRL) & csr::CTRL_RST, csr::CTRL_RST);

  env.host().write32(csr::CTRL, csr::CTRL_RST | csr::CTRL_IRQ_EN);
  CHECK_EQ(env.host().read32(csr::CTRL) & csr::CTRL_IRQ_EN, csr::CTRL_IRQ_EN);

  // A core held in reset must not touch the shared memory or interrupt.
  env.tick(200);
  CHECK_EQ(env.mem().accesses(), size_t(0));
  CHECK_EQ(env.dut()->dut_irq_o, 0);

  // Releasing reset sticks.  Nothing happens until a channel attention.
  env.host().write32(csr::CTRL, 0);
  CHECK_EQ(env.host().read32(csr::CTRL) & csr::CTRL_RST, 0u);
  env.tick(200);
  CHECK_EQ(env.mem().accesses(), size_t(0));
}

// ---------------------------------------------------------------------------
// MII PHY model
// ---------------------------------------------------------------------------

namespace {

// Waits for the PHY model to finish driving one frame onto the RX pins and
// returns what it actually drove.
WireFrame recv_one(Env& env) {
  const bool ok = env.sim().run_until(
      [&]() { return env.phy().rx_monitor().count() >= 1; }, 2 * MS);
  CHECK_MSG(ok, "the PHY model never finished sending an injected frame");
  return env.phy().rx_monitor().pop();
}

}  // namespace

TEST(infra_mii_rx_stream) {
  EthFrame f(env.local_mac(), env.peer_mac(), 0x0800, random_payload(100, 5));
  env.phy().inject(f);
  WireFrame w = recv_one(env);
  CHECK_MSG(w.preamble_ok, "the injected frame had a short preamble");
  CHECK(!w.dribble);
  CHECK(!w.rx_er);
  CHECK_MSG(w.fcs_ok, "the PHY model computed a wrong FCS");
  CHECK_EQ(w.data, f.to_wire(true, true));

  // 100 byte payload => 118 byte frame, 8 preamble bytes: 252 nibbles at 40 ns.
  const u64 expected = 252 * env.phy().nibble_time_ps();
  CHECK(w.end_ps - w.start_ps >= expected - 2 * env.phy().nibble_time_ps());
}

TEST(infra_mii_error_injection) {
  EthFrame f(env.local_mac(), env.peer_mac(), 0x0800, random_payload(60, 9));

  env.phy().inject_bad_fcs(f);
  WireFrame bad = recv_one(env);
  CHECK(!bad.fcs_ok);
  CHECK_EQ(bad.data.size(), f.to_wire(true, true).size());

  // A dribble nibble: an odd number of nibbles in the frame.
  Bytes wire = f.to_wire(true, true);
  std::vector<uint8_t> nibbles;
  for (uint8_t b : wire) {
    nibbles.push_back(uint8_t(b & 0xf));
    nibbles.push_back(uint8_t(b >> 4));
  }
  nibbles.push_back(0x3);
  env.phy().inject_nibbles(nibbles);
  WireFrame dribble = recv_one(env);
  CHECK_MSG(dribble.dribble, "the dribble nibble was not reported");
  CHECK(dribble.fcs_ok);

  // RX_ER asserted in the middle of a frame.
  env.phy().set_next_rx_error(40);
  env.phy().inject(f);
  WireFrame errored = recv_one(env);
  CHECK_MSG(errored.rx_er, "the injected RX_ER was not reported");

  // A runt: fewer than 64 bytes on the wire.
  env.phy().inject_wire(Bytes(20, 0x5a));
  WireFrame runt = recv_one(env);
  CHECK_EQ(runt.data.size(), size_t(20));
  CHECK(!runt.fcs_ok);
}

TEST(infra_mii_interframe_gap) {
  EthFrame f(env.local_mac(), env.peer_mac(), 0x0800, random_payload(46, 2));
  env.phy().set_ipg_nibbles(24);
  env.phy().inject(f);
  env.phy().inject(f);

  WireFrame a = recv_one(env);
  WireFrame b = recv_one(env);
  const u64 gap = b.start_ps - a.end_ps;
  const u64 min_gap = 24 * env.phy().nibble_time_ps();
  CHECK_MSG(gap >= min_gap, "the interframe gap was too short");
}

// ---------------------------------------------------------------------------
// 82586 memory image
// ---------------------------------------------------------------------------

TEST(infra_mem_image_init_structures) {
  ie::MemImage& img = env.img();
  img.build_init_structures(true);
  WbMem& m = env.mem();

  CHECK_EQ(m.rd8(img.scp_addr()), 0);                       // 16-bit bus
  CHECK_EQ(m.rd24(img.scp_addr() + 6), img.iscp_addr());
  CHECK_EQ(img.iscp_busy(), 1);
  CHECK_EQ(m.rd16(img.iscp_addr() + 2), img.scb_off());
  CHECK_EQ(m.rd24(img.iscp_addr() + 4), img.cbbase());
  CHECK_EQ(img.scb_status(), 0);
  CHECK_EQ(img.scb_cmd(), 0);
}

TEST(infra_mem_image_command_blocks) {
  ie::MemImage& img = env.img();
  img.build_init_structures();

  const uint16_t nop = img.add_nop();
  CHECK_EQ(img.cb_cmd(nop) & 7, uint16_t(ie::CMD_NOP));
  CHECK_EQ(img.cb_cmd(nop) & ie::CB_CMD_EL, ie::CB_CMD_EL);
  CHECK_EQ(img.cb_link(nop), ie::NULL_PTR);

  const MacAddr mac = env.local_mac();
  const uint16_t ia = img.add_ia_setup(mac);
  CHECK_EQ(img.cb_cmd(ia) & 7, uint16_t(ie::CMD_IA_SETUP));
  CHECK_EQ(env.mem().read_block(img.addr_of(ia) + 6, 6),
           Bytes(mac.b.begin(), mac.b.end()));

  ie::Config cfg;
  const uint16_t cf = img.add_configure(cfg);
  ie::Config back = ie::Config::parse(env.mem().read_block(img.addr_of(cf) + 6, 12));
  CHECK_EQ(back.byte_count, cfg.byte_count);
  CHECK_EQ(back.fifo_limit, cfg.fifo_limit);
  CHECK_EQ(back.addr_len, cfg.addr_len);
  CHECK_EQ(back.ifs, cfg.ifs);
  CHECK_EQ(back.slot_time, cfg.slot_time);
  CHECK_EQ(back.retry, cfg.retry);
  CHECK_EQ(back.min_frame_len, cfg.min_frame_len);
  CHECK_EQ(back.addr_in_buffer, cfg.addr_in_buffer);

  // Linking clears the end-of-list marker on the block being linked from.
  img.link_cb(nop, ia);
  CHECK_EQ(img.cb_link(nop), ia);
  CHECK_EQ(img.cb_cmd(nop) & ie::CB_CMD_EL, 0u);
}

TEST(infra_mem_image_transmit_block) {
  ie::MemImage& img = env.img();
  img.build_init_structures();
  WbMem& m = env.mem();

  EthFrame f(env.peer_mac(), env.local_mac(), 0x0800, random_payload(64, 4));

  // Address and type in the buffer: the buffer holds the whole frame.
  const uint16_t tx = img.add_transmit(f, true);
  CHECK_EQ(img.cb_cmd(tx) & 7, uint16_t(ie::CMD_TRANSMIT));
  const uint16_t tbd = m.rd16(img.addr_of(tx) + 6);
  CHECK_NE(tbd, ie::NULL_PTR);
  const uint16_t count = m.rd16(img.addr_of(tbd));
  CHECK_EQ(count & ie::RBD_EOF, ie::RBD_EOF);
  CHECK_EQ(count & ie::RBD_COUNT_MASK, uint16_t(f.to_wire(false, false).size()));
  CHECK_EQ(m.read_block(m.rd24(img.addr_of(tbd) + 4), count & ie::RBD_COUNT_MASK),
           f.to_wire(false, false));

  // Address and type in the command block: the buffer holds the payload only.
  const uint16_t tx2 = img.add_transmit(f, false);
  CHECK_EQ(m.read_block(img.addr_of(tx2) + 8, 6),
           Bytes(f.dst.b.begin(), f.dst.b.end()));
  CHECK_EQ(m.rd8(img.addr_of(tx2) + 14), uint8_t(f.type_len >> 8));
  const uint16_t tbd2 = m.rd16(img.addr_of(tx2) + 6);
  CHECK_EQ(m.rd16(img.addr_of(tbd2)) & ie::RBD_COUNT_MASK,
           uint16_t(f.payload.size()));

  // Scattered over three buffers, still one frame.
  const uint16_t tx3 = img.add_transmit_scattered(f, {20, 30}, true);
  uint16_t d = m.rd16(img.addr_of(tx3) + 6);
  Bytes joined;
  int guard = 0;
  while (d != ie::NULL_PTR && guard++ < 16) {
    const uint16_t c = m.rd16(img.addr_of(d));
    Bytes chunk = m.read_block(m.rd24(img.addr_of(d) + 4), c & ie::RBD_COUNT_MASK);
    joined.insert(joined.end(), chunk.begin(), chunk.end());
    if (c & ie::RBD_EOF) break;
    d = m.rd16(img.addr_of(d) + 2);
  }
  CHECK_EQ(joined, f.to_wire(false, false));
}

TEST(infra_mem_image_receive_area) {
  ie::MemImage& img = env.img();
  img.build_init_structures();
  WbMem& m = env.mem();

  const uint16_t rfa = img.build_rfa(4, 8, 256);
  CHECK_EQ(rfa, img.rfds()[0]);
  CHECK_EQ(img.scb_rfa(), rfa);
  CHECK_EQ(img.rfds().size(), size_t(4));

  // The descriptors form a ring and the last one carries EL.
  for (size_t i = 0; i < img.rfds().size(); i++) {
    const uint16_t next = m.rd16(img.addr_of(img.rfds()[i]) + 4);
    const uint16_t want = img.rfds()[(i + 1) % img.rfds().size()];
    CHECK_EQ(next, want);
  }
  CHECK_EQ(m.rd16(img.addr_of(img.rfds().back()) + 2) & ie::RFD_CMD_EL,
           ie::RFD_CMD_EL);

  // The first descriptor owns the buffer list.
  const uint16_t rbd0 = m.rd16(img.addr_of(rfa) + 6);
  CHECK_NE(rbd0, ie::NULL_PTR);
  CHECK_EQ(m.rd16(img.addr_of(rbd0) + 8) & ie::RBD_COUNT_MASK, uint16_t(256));

  // Walking the buffer ring gets back to the start and ends with EL set.
  uint16_t rbd = rbd0;
  int n = 0;
  bool saw_el = false;
  do {
    if (m.rd16(img.addr_of(rbd) + 8) & ie::RBD_EL) saw_el = true;
    rbd = m.rd16(img.addr_of(rbd) + 2);
    n++;
  } while (rbd != rbd0 && n < 32);
  CHECK_EQ(n, 8);
  CHECK(saw_el);

  // Nothing has been received yet.
  CHECK_EQ(img.collect_rx().size(), size_t(0));
}


TEST(infra_wishbone_is_word_addressed) {
  // Wishbone carries a word index on ADR, never a byte address: the bottom two
  // bits of a byte address are not on the bus at all, and SEL alone says which
  // bytes of the word are meant.  Checking the pin rather than the model is
  // the point - the model converts, so comparing the two sides of it would
  // prove nothing.
  Vtb_top* d = env.dut();
  std::vector<uint32_t> pins;
  bool prev_stb = false;
  // One record per strobe, caught on its rising edge: the memory model's own
  // callback has already answered by the time this one runs, so anything that
  // looked at ack would see every cycle already acknowledged.
  env.sim().on_negedge(env.sysclk(), [&]() {
    const bool stb = d->dut_wbm_cyc_o && d->dut_wbm_stb_o;
    if (stb && !prev_stb) pins.push_back(d->dut_wbm_adr_o);
    prev_stb = stb;
  });

  CHECK_DRV(env.drv().init());

  const std::vector<WbMem::Access>& log = env.mem().log();
  CHECK_MSG(!log.empty(), "the chip made no bus accesses to look at");

  // Every access the model saw, in byte terms, must be that word index times
  // four.  The SCP lives at 0x0ffff6, whose word index is odd, so a byte
  // address on the bus would not survive this.
  size_t checked = 0;
  for (size_t i = 0; i < log.size() && i < pins.size(); i++) {
    CHECK_EQ(pins[i], log[i].adr >> 2);
    CHECK_MSG((log[i].adr & 3u) == 0, "the model logged an unaligned word");
    checked++;
  }
  CHECK_MSG(checked >= 4, "not enough accesses to be sure");

  bool saw_odd_word = false;
  for (uint32_t w : pins)
    if (w & 1u) saw_odd_word = true;
  CHECK_MSG(saw_odd_word,
            "no access landed on an odd word, so this proves nothing");
}

}  // namespace wtb
