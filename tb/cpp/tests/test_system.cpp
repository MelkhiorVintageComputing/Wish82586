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

// The control registers are not part of the MAC.  A machine being recreated
// brings its own - doc/sun2_ethernet.pdf is the Sun-2's, one byte with RESET
// and LOOPB active low and CA at bit 5 - so the core has to work when
// something other than wb_csr drives its three host pins.  tb_top ORs these
// into wb_csr's, which is as close as this testbench gets to being that other
// machine.
TEST(sys_channel_attention_needs_no_registers) {
  CHECK_DRV(env.drv().init());
  ie::MemImage& img = env.img();

  const uint16_t cb = img.add_nop(ie::CB_CMD_EL | ie::CB_CMD_I);
  img.set_scb_cbl(cb);
  img.set_scb_cmd(uint16_t(ie::CUC_START << ie::SCB_CMD_CUC_LSB));

  // One cycle on the pin, and nothing on the bus: no CSR write at all.
  env.mem().clear_log();
  const uint32_t csr_before = env.host().read32(csr::CTRL);
  env.dut()->dut_ca_i = 1;
  env.tick(1);
  env.dut()->dut_ca_i = 0;

  CHECK_MSG(env.sim().run_until(
                [&]() { return (img.cb_status(cb) & ie::CB_ST_C) != 0; },
                env.drv().t_cmd),
            "the command never completed after channel attention on the pin");
  CHECK_MSG(img.cb_status(cb) & ie::CB_ST_OK, "the command reported failure");
  CHECK_EQ(env.host().read32(csr::CTRL), csr_before);

  // The interrupt is the core's own pin, and follows the published status.
  CHECK_MSG(env.sim().run_until([&]() { return env.dut()->dut_int_o != 0; },
                                env.drv().t_cmd),
            "the core never asked for an interrupt");
  CHECK_MSG(img.scb_status() & ie::SCB_ST_CX,
            "the interrupt came before the status reached memory");

  // And what the register block reads back is those pins, not its own idea.
  CHECK_EQ(uint32_t(env.dut()->dut_cus_o), uint32_t(img.scb_cus()));
  CHECK_EQ(uint32_t(env.dut()->dut_rus_o), uint32_t(img.scb_rus()));
  CHECK_MSG(!env.dut()->dut_bus_err_o, "the core reported a bus error");
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

// A PHY that holds CRS asserted with nothing on the wire - out of reset, with
// no link, or with the pin on a pull-up, which is how a QMTech Wukong arrives -
// used to stop the machine dead.  The real 82586 defers forever and the drivers
// wait on the command block's done bit with no timeout of their own, so there
// was nothing printed and no bus error to show for it.  mii_tx gives up after
// DEFER_LIMIT symbol times of continuous carrier instead and reports the frame
// the way an unusable medium is normally reported.
//
// This is the only test that runs into the millisecond range: the limit is
// 65536 symbol times, 2.6 ms at the 25 MHz transmit clock of a 100 Mb/s link.
// Waiting it out is the point - a shorter limit would not prove the frame ends
// rather than merely stalls.
TEST(sys_transmit_gives_up_on_a_medium_that_never_goes_quiet) {
  bring_up(env);
  env.phy().set_full_duplex(false);
  env.phy().set_stuck_carrier(true);

  EthFrame f(env.peer_mac(), env.local_mac(), 0x0800, random_payload(64, 41));
  uint16_t cb = 0;
  CHECK_MSG(env.drv().transmit(f, &cb),
            "the transmit command never completed: the chip is deferring for ever");

  const uint16_t st = env.img().cb_status(cb);
  CHECK_MSG(!(st & ie::CB_ST_OK), "a frame that never reached the wire was called OK");
  CHECK_MSG(st & ie::TX_ST_XCOLL,
            "giving up was not reported as the medium being unusable");
  CHECK_MSG(st & ie::TX_ST_DEFER, "the deferral itself was not reported");
  CHECK_MSG(env.phy().tx_count() == 0,
            "something was put on the wire while carrier was asserted");

  // And the chip is not wedged: once the carrier goes away the next frame goes
  // out normally, which is what a driver retrying after the error needs.
  env.phy().set_stuck_carrier(false);
  CHECK_DRV(env.drv().transmit(f));
  CHECK_MSG(env.sim().run_until([&]() { return env.phy().tx_count() >= 1; }, 1 * MS),
            "nothing came out once the medium went quiet again");
  CHECK_EQ(env.phy().pop_tx().data, f.to_wire(true, true));
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

TEST(sys_receive_frame_that_exactly_fills_a_buffer) {
  // A driver sizes its receive buffers for one maximum frame and no more --
  // SunOS uses IE_BUFSZ, which is ETHERMTU or the header plus it depending on
  // where the header lands -- so a full-length frame ends on the very last
  // byte of the buffer.  That buffer is then both full and the end of the
  // frame, and EOF is what tells the driver which.  sys/sunif/if_ie.c reads a
  // buffer closed without EOF as "length > IE_BUFSZ", prints "giant packet"
  // and drops the frame; NFS, whose replies are all full-length, then stalls
  // while everything smaller works.
  env.img().build_init_structures();
  CHECK_DRV(env.drv().init());
  CHECK_DRV(env.drv().ia_setup(env.local_mac()));
  CHECK_DRV(env.drv().configure(ie::Config()));

  // The header lands in the buffer, not the descriptor, so an exact fit is
  // the whole frame: fourteen bytes of header and the payload behind it.
  const size_t payload = 512;
  const size_t frame   = 14 + payload;
  env.img().build_rfa(2, 4, frame);          // exactly one frame per buffer
  CHECK_DRV(env.drv().ru_start());

  EthFrame f(env.local_mac(), env.peer_mac(), 0x0800, random_payload(payload, 37));
  env.phy().inject(f);
  CHECK_DRV(env.drv().wait_rx(1));

  std::vector<ie::RxFrame> rx = env.img().collect_rx();
  CHECK_EQ(rx.size(), size_t(1));
  CHECK_EQ(rx[0].data, f.payload);

  const uint16_t rbd = env.mem().rd16(env.img().addr_of(rx[0].rfd_off) + 6);
  const uint16_t cnt = env.mem().rd16(env.img().addr_of(rbd) + 0);
  CHECK_MSG(cnt & ie::RBD_EOF,
            "the buffer was closed without EOF, which a driver reads as a giant packet");
  CHECK_EQ(size_t(cnt & ie::RBD_COUNT_MASK), frame);
  CHECK_MSG(env.mem().rd16(env.img().addr_of(rbd) + 2) == ie::NULL_PTR ||
            !(env.mem().rd16(env.img().addr_of(env.mem().rd16(env.img().addr_of(rbd) + 2)) + 0)
              & ie::RBD_F),
            "a second buffer was consumed for a frame that fitted in one");
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

#if PHY_DATA_W == 4
// An alignment error means the frame ended part way through a byte, which
// only a nibble wide interface can do.
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
  std::vector<uint8_t> syms = env.phy().to_symbols(f.to_wire(true, true));
  syms.push_back(0x9);
  env.phy().inject_symbols(syms);
  CHECK_DRV(env.drv().wait_rx(1));

  std::vector<ie::RxFrame> rx = env.img().collect_rx();
  CHECK_EQ(rx.size(), size_t(1));
  CHECK_MSG(rx[0].status & ie::RFD_ST_ALIGN, "the alignment error was not reported");
  CHECK_EQ(env.img().scb_aln_errs(), uint16_t(1));
}
#endif

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

TEST(sys_transmit_with_header_in_the_command_block) {
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

TEST(sys_tdr_command) {
  CHECK_DRV(env.drv().init());
  const uint16_t cb = env.img().alloc(8);
  env.mem().wr16(env.img().addr_of(cb) + 0, 0);
  env.mem().wr16(env.img().addr_of(cb) + 2, ie::CB_CMD_EL | ie::CMD_TDR);
  env.mem().wr16(env.img().addr_of(cb) + 4, ie::NULL_PTR);
  env.mem().wr16(env.img().addr_of(cb) + 6, 0);

  CHECK_DRV(env.drv().run_cb(cb));
  CHECK_MSG(env.img().cb_status(cb) & ie::CB_ST_OK, "TDR reported failure");
  // A quiet, correctly terminated link: no open, no short, no transceiver
  // problem.  NetBSD's ie_run_tdr() returns without complaining only if the
  // success bit is set, so it has to be there and not merely implied.
  const uint16_t tdr = env.mem().rd16(env.img().addr_of(cb) + 6);
  CHECK_MSG(tdr & 0x8000, "the TDR success bit was not set");
  CHECK_EQ(tdr & 0x7000, 0);
}

TEST(sys_diagnose_command) {
  CHECK_DRV(env.drv().init());
  const uint16_t cb = env.img().alloc(6);
  env.mem().wr16(env.img().addr_of(cb) + 0, 0);
  env.mem().wr16(env.img().addr_of(cb) + 2, ie::CB_CMD_EL | ie::CMD_DIAGNOSE);
  env.mem().wr16(env.img().addr_of(cb) + 4, ie::NULL_PTR);

  CHECK_DRV(env.drv().run_cb(cb));
  CHECK_MSG(env.img().cb_status(cb) & ie::CB_ST_OK, "self test reported failure");
}

TEST(sys_dump_command_reports_failure) {
  // DUMP would have to invent the contents of registers this design does not
  // have, so it says it did not work rather than handing back made up state.
  CHECK_DRV(env.drv().init());
  const uint16_t cb = env.img().alloc(8);
  env.mem().wr16(env.img().addr_of(cb) + 0, 0);
  env.mem().wr16(env.img().addr_of(cb) + 2, ie::CB_CMD_EL | ie::CMD_DUMP);
  env.mem().wr16(env.img().addr_of(cb) + 4, ie::NULL_PTR);
  env.mem().wr16(env.img().addr_of(cb) + 6, 0);

  CHECK_DRV(env.drv().run_cb(cb));
  CHECK_MSG(env.img().cb_status(cb) & ie::CB_ST_C, "DUMP never completed");
  CHECK_MSG(!(env.img().cb_status(cb) & ie::CB_ST_OK),
            "DUMP claimed to have worked");
}

TEST(sys_netbsd_style_bring_up) {
  // The order i82586_init() uses: configure, set the address, run the TDR,
  // load the multicast list, then start the receiver.  Every step has to come
  // back OK or the driver logs a complaint.
  CHECK_DRV(env.drv().init());
  CHECK_DRV(env.drv().configure(ie::Config()));
  CHECK_DRV(env.drv().ia_setup(env.local_mac()));

  const uint16_t tdr = env.img().alloc(8);
  env.mem().wr16(env.img().addr_of(tdr) + 0, 0);
  env.mem().wr16(env.img().addr_of(tdr) + 2, ie::CB_CMD_EL | ie::CMD_TDR);
  env.mem().wr16(env.img().addr_of(tdr) + 4, ie::NULL_PTR);
  env.mem().wr16(env.img().addr_of(tdr) + 6, 0);
  CHECK_DRV(env.drv().run_cb(tdr));
  CHECK_MSG(env.mem().rd16(env.img().addr_of(tdr) + 6) & 0x8000,
            "a driver would log a TDR complaint here");

  CHECK_DRV(env.drv().mc_setup({MacAddr(0x01, 0x00, 0x5e, 0, 0, 1)}));
  env.img().build_rfa(4, 8, 512);
  CHECK_DRV(env.drv().ru_start());

  // And it works afterwards, both ways.
  EthFrame in(env.local_mac(), env.peer_mac(), 0x0800, random_payload(100, 70));
  env.phy().inject(in);
  CHECK_DRV(env.drv().wait_rx(1));
  CHECK_EQ(env.img().collect_rx().at(0).data, in.payload);

  EthFrame out(env.peer_mac(), env.local_mac(), 0x0800, random_payload(100, 71));
  CHECK_DRV(env.drv().transmit(out));
  CHECK_MSG(env.sim().run_until([&]() { return env.phy().tx_count() >= 1; }, 1 * MS),
            "nothing was transmitted after the bring-up sequence");
  CHECK_EQ(env.phy().pop_tx().data, out.to_wire(true, true));
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


TEST(sys_receive_with_header_in_the_descriptor) {
  // AL-LOC = 0 on the way in: the first fourteen bytes go into the descriptor
  // and the buffers hold only what follows.
  CHECK_DRV(env.drv().init());
  CHECK_DRV(env.drv().ia_setup(env.local_mac()));
  ie::Config cfg;
  cfg.addr_in_buffer = false;
  CHECK_DRV(env.drv().configure(cfg));
  env.img().build_rfa(4, 8, 512);
  CHECK_DRV(env.drv().ru_start());

  EthFrame f(env.local_mac(), env.peer_mac(), 0x0806, random_payload(120, 60));
  env.phy().inject(f);
  CHECK_DRV(env.drv().wait_rx(1));

  std::vector<ie::RxFrame> rx = env.img().collect_rx(false);
  CHECK_EQ(rx.size(), size_t(1));
  CHECK(rx[0].ok());
  CHECK_EQ(rx[0].dst, f.dst);
  CHECK_EQ(rx[0].src, f.src);
  CHECK_EQ(rx[0].type_len, f.type_len);
  // The buffer holds the payload alone, with no header in front of it.
  CHECK_EQ(rx[0].raw, f.payload);
}

TEST(sys_alloc0_round_trip_through_loopback) {
  // Transmit builds the header from the command block, receive takes it apart
  // again into the descriptor; the frame has to survive both.
  CHECK_DRV(env.drv().init());
  CHECK_DRV(env.drv().ia_setup(env.local_mac()));
  ie::Config cfg;
  cfg.addr_in_buffer = false;
  cfg.int_loopback = true;
  CHECK_DRV(env.drv().configure(cfg));
  env.img().build_rfa(4, 8, 512);
  CHECK_DRV(env.drv().ru_start());

  EthFrame f(env.local_mac(), env.local_mac(), 0x1234, random_payload(64, 61));
  const uint16_t cb = env.img().add_transmit(f, false);
  CHECK_DRV(env.drv().run_cb(cb));
  CHECK_DRV(env.drv().wait_rx(1));

  std::vector<ie::RxFrame> rx = env.img().collect_rx(false);
  CHECK_EQ(rx.size(), size_t(1));
  CHECK_EQ(rx[0].dst, f.dst);
  CHECK_EQ(rx[0].src, f.src);
  CHECK_EQ(rx[0].type_len, f.type_len);
  CHECK_EQ(rx[0].raw, f.payload);
  CHECK_MSG(env.phy().tx_count() == 0, "a looped back frame reached the wire");
}


TEST(sys_transmit_stages_in_words) {
  // Staging reads whole words where it can.  A byte at a time would be one
  // bus read per byte of frame; this checks it is nothing like that, which is
  // what stops the transmit side monopolising a bus the receive unit needs.
  bring_up(env);
  EthFrame f(env.peer_mac(), env.local_mac(), 0x0800, random_payload(1000, 80));

  env.mem().clear_log();
  CHECK_DRV(env.drv().transmit(f));

  size_t reads = 0;
  for (const WbMem::Access& a : env.mem().log())
    if (!a.write) reads++;

  const size_t frame_len = f.to_wire(false, false).size();   // 1014 bytes
  CHECK_MSG(reads < frame_len / 2,
            "the frame was staged a byte at a time");
  logf("%zu bus reads to stage %zu bytes", reads, frame_len);

  CHECK_MSG(env.sim().run_until([&]() { return env.phy().tx_count() >= 1; }, 2 * MS),
            "nothing was transmitted");
  CHECK_EQ(env.phy().pop_tx().data, f.to_wire(true, true));
}

TEST(sys_transmit_from_an_unaligned_buffer) {
  // A transmit buffer need not start on a word boundary, and its count need
  // not be a multiple of four.  Both ends have to fall back to bytes.
  bring_up(env);
  EthFrame f(env.peer_mac(), env.local_mac(), 0x0800, random_payload(83, 81));
  const Bytes wire = f.to_wire(false, false);

  // Build the command block by hand so the buffer can sit at an odd address.
  const uint32_t buf = 0x0003'0001;            // deliberately not word aligned
  env.mem().write_block(buf, wire);

  const uint16_t tbd = env.img().alloc(8);
  env.mem().wr16(env.img().addr_of(tbd) + 0,
                 uint16_t((wire.size() & 0x3fff) | ie::RBD_EOF));
  env.mem().wr16(env.img().addr_of(tbd) + 2, ie::NULL_PTR);
  env.mem().wr24(env.img().addr_of(tbd) + 4, buf);

  const uint16_t cb = env.img().alloc(16);
  for (int i = 0; i < 16; i++) env.mem().wr8(env.img().addr_of(cb) + uint32_t(i), 0);
  env.mem().wr16(env.img().addr_of(cb) + 2, ie::CB_CMD_EL | ie::CMD_TRANSMIT);
  env.mem().wr16(env.img().addr_of(cb) + 4, ie::NULL_PTR);
  env.mem().wr16(env.img().addr_of(cb) + 6, tbd);

  CHECK_DRV(env.drv().run_cb(cb));
  CHECK_MSG(env.img().cb_status(cb) & ie::CB_ST_OK, "the transmit failed");
  CHECK_MSG(env.sim().run_until([&]() { return env.phy().tx_count() >= 1; }, 1 * MS),
            "nothing was transmitted");
  CHECK_EQ(env.phy().pop_tx().data, f.to_wire(true, true));
}

}  // namespace wtb
