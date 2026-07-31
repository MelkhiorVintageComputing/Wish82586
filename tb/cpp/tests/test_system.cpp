// SPDX-License-Identifier: MIT
//
// System level tests: a driver talking to the MAC through shared memory, with
// a PHY on the other side.  They are the specification of what Wish82586 has
// to do, written before the RTL exists, so they are marked pending: they run,
// they fail, and the runner reports them as expected failures until the
// corresponding piece of RTL lands.  Drop the pending marker as each one
// starts passing.

#include "test.h"

#include "Vtb_top.h"

namespace wtb {

namespace {

// Brings the chip up the way the Sun ROM driver does: initialise, set the
// individual address, configure, then start the receive unit.
void bring_up(Env& env) {
  CHECK_DRV(env.drv().init());
  CHECK_DRV(env.drv().ia_setup(env.local_mac()));
  ie::Config cfg;                    // the Sun driver's defaults
  CHECK_DRV(env.drv().configure(cfg));
  env.img().build_rfa(4, 8, 512);
  CHECK_DRV(env.drv().ru_start());
}

}  // namespace

// ---------------------------------------------------------------------------
// Initialisation
// ---------------------------------------------------------------------------

TEST(sys_init_sequence) {
  ie::MemImage& img = env.img();
  CHECK_DRV(env.drv().init());

  // The chip must have read the SCP and the ISCP and cleared the busy flag.
  CHECK_EQ(img.iscp_busy(), 0);
  CHECK_EQ(img.scb_cus(), uint16_t(ie::CUS_IDLE));
  CHECK_EQ(img.scb_rus(), uint16_t(ie::RUS_IDLE));

  // It must have fetched the SCP from the programmed address and nothing else
  // outside the structures we set up.
  bool read_scp = false;
  for (const auto& a : env.mem().log())
    if (!a.write && (a.adr & ~3u) == (img.scp_addr() & ~3u)) read_scp = true;
  CHECK_MSG(read_scp, "the chip never read the System Configuration Pointer");
  CHECK_MSG(!env.mem().oob_seen(), "the chip accessed memory outside the model");
}

TEST(sys_reset_stops_the_chip) {
  CHECK_DRV(env.drv().init());
  env.drv().hold_reset(true);
  env.tick(50);
  env.mem().clear_log();
  env.tick(500);
  CHECK_MSG(env.mem().accesses() == 0,
            "the chip kept using the bus while held in reset");
}

TEST(sys_interrupt_on_command_completion) {
  env.drv().enable_irq(true);
  CHECK_DRV(env.drv().init());
  CHECK_EQ(env.dut()->dut_irq_o, 0);

  const uint16_t cb = env.img().add_nop(ie::CB_CMD_EL | ie::CB_CMD_I);
  env.img().set_scb_cbl(cb);
  CHECK_DRV(env.drv().issue_scb(uint16_t(ie::CUC_START << ie::SCB_CMD_CUC_LSB)));
  CHECK_MSG(env.sim().run_until([&]() { return env.dut()->dut_irq_o != 0; },
                                200 * US),
            "no interrupt was raised when the command completed");
  CHECK_DRV(env.drv().ack_all());
  CHECK_EQ(env.dut()->dut_irq_o, 0);
}

// ---------------------------------------------------------------------------
// Command unit
// ---------------------------------------------------------------------------

TEST(sys_command_list) {
  CHECK_DRV(env.drv().init());
  ie::MemImage& img = env.img();

  // Three blocks chained together run in one go, each marked complete and OK.
  const uint16_t a = img.add_nop(0);
  const uint16_t b = img.add_ia_setup(env.local_mac(), 0);
  const uint16_t c = img.add_configure(ie::Config(), ie::CB_CMD_EL | ie::CB_CMD_I);
  img.link_cb(a, b);
  img.link_cb(b, c);

  CHECK_DRV(env.drv().run_cb(a));
  for (uint16_t cb : {a, b, c}) {
    CHECK_MSG(img.cb_status(cb) & ie::CB_ST_C, "a command block did not complete");
    CHECK_MSG(img.cb_status(cb) & ie::CB_ST_OK, "a command block reported failure");
    CHECK_MSG(!(img.cb_status(cb) & ie::CB_ST_B), "a command block is still busy");
  }
  CHECK_EQ(img.scb_cus(), uint16_t(ie::CUS_IDLE));
}

TEST(sys_command_suspend_and_resume) {
  CHECK_DRV(env.drv().init());
  ie::MemImage& img = env.img();

  const uint16_t a = img.add_nop(ie::CB_CMD_S);   // suspend after this one
  const uint16_t b = img.add_nop(ie::CB_CMD_EL);
  img.link_cb(a, b);

  img.set_scb_cbl(a);
  CHECK_DRV(env.drv().issue_scb(uint16_t(ie::CUC_START << ie::SCB_CMD_CUC_LSB)));
  CHECK_MSG(env.sim().run_until([&]() { return img.scb_cus() == ie::CUS_SUSPENDED; },
                                200 * US),
            "the command unit did not suspend");
  CHECK_MSG(!(img.cb_status(b) & ie::CB_ST_C),
            "the command unit ran past the suspend marker");

  CHECK_DRV(env.drv().issue_scb(uint16_t(ie::CUC_RESUME << ie::SCB_CMD_CUC_LSB)));
  CHECK_MSG(env.sim().run_until([&]() { return (img.cb_status(b) & ie::CB_ST_C) != 0; },
                                200 * US),
            "the command unit did not resume");
}

// ---------------------------------------------------------------------------
// Transmit
// ---------------------------------------------------------------------------

TEST(sys_transmit_one_frame) {
  bring_up(env);

  EthFrame f(env.peer_mac(), env.local_mac(), 0x0800, random_payload(100, 1));
  uint16_t cb = 0;
  CHECK_DRV(env.drv().transmit(f, &cb));

  CHECK_MSG(env.img().cb_status(cb) & ie::CB_ST_OK,
            "the transmit command reported failure");
  CHECK_MSG(env.sim().run_until([&]() { return env.phy().tx_count() >= 1; }, 1 * MS),
            "nothing came out on the MII transmit pins");

  WireFrame w = env.phy().pop_tx();
  CHECK_MSG(w.preamble_ok, "the transmitted preamble was too short");
  CHECK_MSG(w.fcs_ok, "the MAC appended a wrong FCS");
  CHECK(!w.dribble);
  CHECK_EQ(w.data, f.to_wire(true, true));
}

TEST(sys_transmit_pads_short_frames) {
  bring_up(env);

  // 10 bytes of payload: the MAC has to pad the frame out to 60 bytes plus FCS.
  EthFrame f(env.peer_mac(), env.local_mac(), 0x0800, random_payload(10, 2));
  CHECK_DRV(env.drv().transmit(f));
  CHECK_MSG(env.sim().run_until([&]() { return env.phy().tx_count() >= 1; }, 1 * MS),
            "nothing came out on the MII transmit pins");

  WireFrame w = env.phy().pop_tx();
  CHECK_EQ(w.data.size(), ETH_MIN_FRAME + 4);
  CHECK(w.fcs_ok);
}

TEST(sys_transmit_scattered_buffers) {
  bring_up(env);

  EthFrame f(env.peer_mac(), env.local_mac(), 0x0800, random_payload(400, 3));
  const uint16_t cb = env.img().add_transmit_scattered(f, {17, 64, 3, 200}, true);
  CHECK_DRV(env.drv().run_cb(cb));
  CHECK_MSG(env.sim().run_until([&]() { return env.phy().tx_count() >= 1; }, 1 * MS),
            "nothing came out on the MII transmit pins");

  WireFrame w = env.phy().pop_tx();
  CHECK_EQ(w.data, f.to_wire(true, true));
}

TEST(sys_transmit_maximum_length) {
  bring_up(env);
  EthFrame f(env.peer_mac(), env.local_mac(), 0x0800, random_payload(1500, 4));
  CHECK_DRV(env.drv().transmit(f));
  CHECK_MSG(env.sim().run_until([&]() { return env.phy().tx_count() >= 1; }, 2 * MS),
            "nothing came out on the MII transmit pins");
  WireFrame w = env.phy().pop_tx();
  CHECK_EQ(w.data.size(), size_t(1518));
  CHECK(w.fcs_ok);
}

TEST(sys_transmit_retries_after_collision) {
  bring_up(env);
  env.phy().set_full_duplex(false);
  env.phy().force_collision(40);      // collide inside the preamble/data

  EthFrame f(env.peer_mac(), env.local_mac(), 0x0800, random_payload(64, 5));
  uint16_t cb = 0;
  CHECK_DRV(env.drv().transmit(f, &cb));

  const uint16_t st = env.img().cb_status(cb);
  CHECK_MSG(st & ie::CB_ST_OK, "the retried transmission never succeeded");
  CHECK_MSG((st & ie::TX_ST_NCOL_MASK) >= 1,
            "the collision was not counted in the transmit status");
  CHECK_MSG(env.phy().tx_count() >= 2, "the frame was not retransmitted");
}

// ---------------------------------------------------------------------------
// Receive
// ---------------------------------------------------------------------------

TEST(sys_receive_one_frame) {
  bring_up(env);

  EthFrame f(env.local_mac(), env.peer_mac(), 0x0800, random_payload(200, 6));
  env.phy().inject(f);
  CHECK_DRV(env.drv().wait_rx(1));

  std::vector<ie::RxFrame> rx = env.img().collect_rx();
  CHECK_EQ(rx.size(), size_t(1));
  CHECK_MSG(rx[0].ok(), "the received frame was marked bad");
  CHECK_EQ(rx[0].dst, f.dst);
  CHECK_EQ(rx[0].src, f.src);
  CHECK_EQ(rx[0].type_len, f.type_len);
  CHECK_EQ(rx[0].data, f.payload);
  CHECK_EQ(env.img().scb_status() & ie::SCB_ST_FR, ie::SCB_ST_FR);
}

TEST(sys_receive_filters_by_address) {
  bring_up(env);

  EthFrame mine(env.local_mac(), env.peer_mac(), 0x0800, random_payload(64, 7));
  EthFrame other(MacAddr(0x02, 0x11, 0x22, 0x33, 0x44, 0x55), env.peer_mac(),
                 0x0800, random_payload(64, 8));
  EthFrame bcast(MacAddr::broadcast(), env.peer_mac(), 0x0800,
                 random_payload(64, 9));

  env.phy().inject(other);
  env.phy().inject(mine);
  env.phy().inject(bcast);
  CHECK_DRV(env.drv().wait_rx(2));
  env.tick(2000);

  std::vector<ie::RxFrame> rx = env.img().collect_rx();
  CHECK_MSG(rx.size() == 2, "the address filter kept the wrong number of frames");
  CHECK_EQ(rx[0].dst, mine.dst);
  CHECK_EQ(rx[1].dst, bcast.dst);
}

TEST(sys_receive_discards_bad_fcs) {
  bring_up(env);

  EthFrame f(env.local_mac(), env.peer_mac(), 0x0800, random_payload(64, 10));
  env.phy().inject_bad_fcs(f);
  env.tick(5000);

  // Without SAV-BF the frame is dropped and only the CRC counter moves.
  CHECK_EQ(env.img().collect_rx().size(), size_t(0));
  CHECK_EQ(env.img().scb_crc_errs(), uint16_t(1));
}

TEST(sys_receive_chains_buffers) {
  // Small buffers force the receive unit to chain descriptors for one frame.
  env.img().build_init_structures();
  CHECK_DRV(env.drv().init());
  CHECK_DRV(env.drv().ia_setup(env.local_mac()));
  CHECK_DRV(env.drv().configure(ie::Config()));
  env.img().build_rfa(2, 16, 64);
  CHECK_DRV(env.drv().ru_start());

  EthFrame f(env.local_mac(), env.peer_mac(), 0x0800, random_payload(500, 11));
  env.phy().inject(f);
  CHECK_DRV(env.drv().wait_rx(1));

  std::vector<ie::RxFrame> rx = env.img().collect_rx();
  CHECK_EQ(rx.size(), size_t(1));
  CHECK_EQ(rx[0].data, f.payload);
}

TEST(sys_receive_back_to_back) {
  bring_up(env);

  std::vector<EthFrame> sent;
  env.phy().set_ipg_nibbles(24);      // the minimum interframe gap
  for (int i = 0; i < 4; i++) {
    EthFrame f(env.local_mac(), env.peer_mac(), uint16_t(0x0800 + i),
               random_payload(size_t(60 + i * 100), uint32_t(20 + i)));
    sent.push_back(f);
    env.phy().inject(f);
  }
  CHECK_DRV(env.drv().wait_rx(4));

  std::vector<ie::RxFrame> rx = env.img().collect_rx();
  CHECK_EQ(rx.size(), size_t(4));
  for (size_t i = 0; i < rx.size(); i++) {
    CHECK_EQ(rx[i].type_len, sent[i].type_len);
    CHECK_EQ(rx[i].data, sent[i].payload);
  }
}

TEST(sys_receive_out_of_resources) {
  // One descriptor, two frames: the second one has nowhere to go and the
  // receive unit must report no resources rather than corrupt memory.
  CHECK_DRV(env.drv().init());
  CHECK_DRV(env.drv().ia_setup(env.local_mac()));
  CHECK_DRV(env.drv().configure(ie::Config()));
  env.img().build_rfa(1, 1, 128);
  CHECK_DRV(env.drv().ru_start());

  EthFrame f(env.local_mac(), env.peer_mac(), 0x0800, random_payload(64, 12));
  env.phy().inject(f);
  env.phy().inject(f);
  CHECK_DRV(env.drv().wait_rx(1));
  env.tick(5000);

  CHECK_EQ(env.img().scb_rus(), uint16_t(ie::RUS_NO_RESOURCE));
  CHECK_EQ(env.img().scb_status() & ie::SCB_ST_RNR, ie::SCB_ST_RNR);
  CHECK_MSG(!env.mem().oob_seen(), "the receive unit wrote outside its buffers");
}

// ---------------------------------------------------------------------------
// Loopback: the chip talking to itself, which is what the Sun driver does
// before putting the interface on the wire.
// ---------------------------------------------------------------------------

TEST(sys_internal_loopback) {
  CHECK_DRV(env.drv().init());
  CHECK_DRV(env.drv().ia_setup(env.local_mac()));
  ie::Config cfg;
  cfg.int_loopback = true;
  CHECK_DRV(env.drv().configure(cfg));
  env.img().build_rfa(4, 8, 512);
  CHECK_DRV(env.drv().ru_start());

  EthFrame f(env.local_mac(), env.local_mac(), 0x0800, random_payload(64, 13));
  CHECK_DRV(env.drv().transmit(f));
  CHECK_DRV(env.drv().wait_rx(1));

  std::vector<ie::RxFrame> rx = env.img().collect_rx();
  CHECK_EQ(rx.size(), size_t(1));
  CHECK_EQ(rx[0].data, f.payload);
  CHECK_MSG(env.phy().tx_count() == 0,
            "an internally looped back frame reached the MII pins");
}


// ---------------------------------------------------------------------------
// Configuration options that are wired up but not yet exercised elsewhere
// ---------------------------------------------------------------------------

TEST(sys_receive_promiscuous) {
  CHECK_DRV(env.drv().init());
  CHECK_DRV(env.drv().ia_setup(env.local_mac()));
  ie::Config cfg;
  cfg.promiscuous = true;
  CHECK_DRV(env.drv().configure(cfg));
  env.img().build_rfa(4, 8, 512);
  CHECK_DRV(env.drv().ru_start());

  // Addressed to somebody else entirely, and kept anyway.
  EthFrame f(MacAddr(0x02, 0x11, 0x22, 0x33, 0x44, 0x55), env.peer_mac(), 0x0800,
             random_payload(64, 40));
  env.phy().inject(f);
  CHECK_DRV(env.drv().wait_rx(1));

  std::vector<ie::RxFrame> rx = env.img().collect_rx();
  CHECK_EQ(rx.size(), size_t(1));
  CHECK_EQ(rx[0].dst, f.dst);
  CHECK_EQ(rx[0].data, f.payload);
}

TEST(sys_broadcast_can_be_disabled) {
  CHECK_DRV(env.drv().init());
  CHECK_DRV(env.drv().ia_setup(env.local_mac()));
  ie::Config cfg;
  cfg.no_broadcast = true;
  CHECK_DRV(env.drv().configure(cfg));
  env.img().build_rfa(4, 8, 512);
  CHECK_DRV(env.drv().ru_start());

  EthFrame bcast(MacAddr::broadcast(), env.peer_mac(), 0x0800,
                 random_payload(64, 41));
  EthFrame mine(env.local_mac(), env.peer_mac(), 0x0800, random_payload(64, 42));
  env.phy().inject(bcast);
  env.phy().inject(mine);
  CHECK_DRV(env.drv().wait_rx(1));
  env.tick(3000);

  std::vector<ie::RxFrame> rx = env.img().collect_rx();
  CHECK_MSG(rx.size() == 1, "the broadcast frame was not turned away");
  CHECK_EQ(rx[0].dst, mine.dst);
}

// ---------------------------------------------------------------------------
// Still to do.  These describe behaviour the drivers can ask for and the chip
// does not provide yet; they are the todo list.
// ---------------------------------------------------------------------------

TEST(sys_multicast_setup_and_filter) {
  CHECK_DRV(env.drv().init());
  CHECK_DRV(env.drv().ia_setup(env.local_mac()));
  CHECK_DRV(env.drv().configure(ie::Config()));

  const MacAddr group(0x01, 0x00, 0x5e, 0x00, 0x00, 0x01);
  const MacAddr other_group(0x01, 0x00, 0x5e, 0x7f, 0x7f, 0x7f);
  CHECK_DRV(env.drv().mc_setup({group}));

  env.img().build_rfa(4, 8, 512);
  CHECK_DRV(env.drv().ru_start());

  EthFrame wanted(group, env.peer_mac(), 0x0800, random_payload(64, 43));
  EthFrame unwanted(other_group, env.peer_mac(), 0x0800, random_payload(64, 44));
  env.phy().inject(unwanted);
  env.phy().inject(wanted);
  CHECK_DRV(env.drv().wait_rx(1));
  env.tick(3000);

  std::vector<ie::RxFrame> rx = env.img().collect_rx();
  CHECK_MSG(rx.size() == 1, "the multicast filter kept the wrong frames");
  CHECK_EQ(rx[0].dst, group);
}

TEST(sys_receive_saves_bad_frames) {
  CHECK_DRV(env.drv().init());
  CHECK_DRV(env.drv().ia_setup(env.local_mac()));
  ie::Config cfg;
  cfg.save_bad_frames = true;
  CHECK_DRV(env.drv().configure(cfg));
  env.img().build_rfa(4, 8, 512);
  CHECK_DRV(env.drv().ru_start());

  EthFrame f(env.local_mac(), env.peer_mac(), 0x0800, random_payload(64, 45));
  env.phy().inject_bad_fcs(f);
  CHECK_DRV(env.drv().wait_rx(1));

  std::vector<ie::RxFrame> rx = env.img().collect_rx();
  CHECK_EQ(rx.size(), size_t(1));
  CHECK_MSG(rx[0].status & ie::RFD_ST_CRC, "the CRC error was not reported");
  CHECK_MSG(!(rx[0].status & ie::RFD_ST_OK), "a bad frame was marked good");
  // Saving the frame does not stop it being counted.
  CHECK_EQ(env.img().scb_crc_errs(), uint16_t(1));
  CHECK_EQ(rx[0].data, f.payload);
}

TEST(sys_receive_reports_an_alignment_error) {
  CHECK_DRV(env.drv().init());
  CHECK_DRV(env.drv().ia_setup(env.local_mac()));
  ie::Config cfg;
  cfg.save_bad_frames = true;
  CHECK_DRV(env.drv().configure(cfg));
  env.img().build_rfa(4, 8, 512);
  CHECK_DRV(env.drv().ru_start());

  // A frame that ends on a nibble boundary: correct bytes, one nibble over.
  EthFrame f(env.local_mac(), env.peer_mac(), 0x0800, random_payload(64, 47));
  Bytes wire = f.to_wire(true, true);
  std::vector<uint8_t> nibbles;
  for (uint8_t b : wire) {
    nibbles.push_back(uint8_t(b & 0xf));
    nibbles.push_back(uint8_t(b >> 4));
  }
  nibbles.push_back(0x9);
  env.phy().inject_nibbles(nibbles);
  CHECK_DRV(env.drv().wait_rx(1));

  std::vector<ie::RxFrame> rx = env.img().collect_rx();
  CHECK_EQ(rx.size(), size_t(1));
  CHECK_MSG(rx[0].status & ie::RFD_ST_ALIGN, "the alignment error was not reported");
  CHECK_EQ(env.img().scb_aln_errs(), uint16_t(1));
}

TEST(sys_receive_good_frame_reports_no_errors) {
  bring_up(env);
  EthFrame f(env.local_mac(), env.peer_mac(), 0x0800, random_payload(100, 48));
  env.phy().inject(f);
  CHECK_DRV(env.drv().wait_rx(1));

  std::vector<ie::RxFrame> rx = env.img().collect_rx();
  CHECK_EQ(rx.size(), size_t(1));
  CHECK_EQ(rx[0].status, uint16_t(ie::RFD_ST_C | ie::RFD_ST_OK));
  CHECK_EQ(env.img().scb_crc_errs(), uint16_t(0));
  CHECK_EQ(env.img().scb_aln_errs(), uint16_t(0));
  CHECK_EQ(env.img().scb_ovrn_errs(), uint16_t(0));
}

TEST_PENDING(sys_transmit_with_header_in_the_command_block,
             "AL-LOC = 0, where the destination and type come from the command "
             "block and the source address from IA-SETUP, is not implemented") {
  CHECK_DRV(env.drv().init());
  CHECK_DRV(env.drv().ia_setup(env.local_mac()));
  ie::Config cfg;
  cfg.addr_in_buffer = false;
  CHECK_DRV(env.drv().configure(cfg));

  EthFrame f(env.peer_mac(), env.local_mac(), 0x0800, random_payload(80, 46));
  const uint16_t cb = env.img().add_transmit(f, false);
  CHECK_DRV(env.drv().run_cb(cb));
  CHECK_MSG(env.sim().run_until([&]() { return env.phy().tx_count() >= 1; }, 1 * MS),
            "nothing came out on the MII transmit pins");

  WireFrame w = env.phy().pop_tx();
  CHECK(w.fcs_ok);
  CHECK_EQ(w.data, f.to_wire(true, true));
}

TEST_PENDING(sys_tdr_command,
             "the time domain reflectometer command is not implemented") {
  CHECK_DRV(env.drv().init());
  const uint16_t cb = env.img().alloc(8);
  env.mem().wr16(env.img().addr_of(cb) + 0, 0);
  env.mem().wr16(env.img().addr_of(cb) + 2, ie::CB_CMD_EL | ie::CMD_TDR);
  env.mem().wr16(env.img().addr_of(cb) + 4, ie::NULL_PTR);
  env.mem().wr16(env.img().addr_of(cb) + 6, 0);

  CHECK_DRV(env.drv().run_cb(cb));
  CHECK_MSG(env.img().cb_status(cb) & ie::CB_ST_OK, "TDR reported failure");
  // A quiet, correctly terminated link: no open, no short, no transceiver
  // problem.
  const uint16_t tdr = env.mem().rd16(env.img().addr_of(cb) + 6);
  CHECK_EQ(tdr & 0x7000, 0);
}


TEST(sys_multicast_several_groups) {
  CHECK_DRV(env.drv().init());
  CHECK_DRV(env.drv().ia_setup(env.local_mac()));
  CHECK_DRV(env.drv().configure(ie::Config()));

  std::vector<MacAddr> groups;
  for (int i = 0; i < 8; i++)
    groups.push_back(MacAddr(0x01, 0x00, 0x5e, 0x00, 0x01, uint8_t(i)));
  CHECK_DRV(env.drv().mc_setup(groups));

  env.img().build_rfa(8, 16, 512);
  CHECK_DRV(env.drv().ru_start());

  // The first, the last, and one that was never asked for.
  EthFrame a(groups.front(), env.peer_mac(), 0x0800, random_payload(64, 50));
  EthFrame b(groups.back(), env.peer_mac(), 0x0800, random_payload(64, 51));
  EthFrame c(MacAddr(0x01, 0x00, 0x5e, 0x00, 0x01, 0x40), env.peer_mac(), 0x0800,
             random_payload(64, 52));
  env.phy().inject(c);
  env.phy().inject(a);
  env.phy().inject(b);
  CHECK_DRV(env.drv().wait_rx(2));
  env.tick(3000);

  std::vector<ie::RxFrame> rx = env.img().collect_rx();
  CHECK_MSG(rx.size() == 2, "the multicast filter kept the wrong number of frames");
  CHECK_EQ(rx[0].dst, a.dst);
  CHECK_EQ(rx[1].dst, b.dst);
}

TEST(sys_multicast_empty_list_disables_multicast) {
  CHECK_DRV(env.drv().init());
  CHECK_DRV(env.drv().ia_setup(env.local_mac()));
  CHECK_DRV(env.drv().configure(ie::Config()));
  CHECK_DRV(env.drv().mc_setup({MacAddr(0x01, 0x00, 0x5e, 0, 0, 1)}));
  CHECK_DRV(env.drv().mc_setup({}));      // an empty list turns them all off

  env.img().build_rfa(4, 8, 512);
  CHECK_DRV(env.drv().ru_start());

  EthFrame group(MacAddr(0x01, 0x00, 0x5e, 0, 0, 1), env.peer_mac(), 0x0800,
                 random_payload(64, 53));
  EthFrame mine(env.local_mac(), env.peer_mac(), 0x0800, random_payload(64, 54));
  env.phy().inject(group);
  env.phy().inject(mine);
  CHECK_DRV(env.drv().wait_rx(1));
  env.tick(3000);

  std::vector<ie::RxFrame> rx = env.img().collect_rx();
  CHECK_MSG(rx.size() == 1, "a multicast frame was kept after the list was cleared");
  CHECK_EQ(rx[0].dst, mine.dst);
}

TEST(sys_multicast_overflow_takes_everything) {
  // More addresses than the receive unit can hold: rather than filter on a
  // silently shortened list and drop frames the driver asked for, the chip
  // takes every multicast frame and lets software sort them out.
  CHECK_DRV(env.drv().init());
  CHECK_DRV(env.drv().ia_setup(env.local_mac()));
  CHECK_DRV(env.drv().configure(ie::Config()));

  std::vector<MacAddr> groups;
  for (int i = 0; i < 12; i++)
    groups.push_back(MacAddr(0x01, 0x00, 0x5e, 0x00, 0x02, uint8_t(i)));
  CHECK_DRV(env.drv().mc_setup(groups));

  env.img().build_rfa(4, 8, 512);
  CHECK_DRV(env.drv().ru_start());

  // One from beyond the eighth slot, which an exact-list-only filter would
  // have thrown away.
  EthFrame late(groups[11], env.peer_mac(), 0x0800, random_payload(64, 55));
  env.phy().inject(late);
  CHECK_DRV(env.drv().wait_rx(1));

  std::vector<ie::RxFrame> rx = env.img().collect_rx();
  CHECK_EQ(rx.size(), size_t(1));
  CHECK_EQ(rx[0].dst, late.dst);
}

}  // namespace wtb
