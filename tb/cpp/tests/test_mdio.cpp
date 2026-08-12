// SPDX-License-Identifier: MIT
//
// The MDIO station and the block that programs the PHY through it.
//
// None of the 82586 drivers knows a PHY exists, so nothing in the software
// stack will ever set one up.  These check that it gets set up anyway.

#include "test.h"

#include <string>

#include "Vtb_top.h"
#include "mii.h"

namespace wtb {

namespace {

// Register indices within wb_mdio, as word addresses; the host model takes
// byte addresses, so these are the offsets.
constexpr uint32_t MDIO_CTRL = 0x00;
constexpr uint32_t MDIO_WDATA = 0x04;
constexpr uint32_t MDIO_RDATA = 0x08;
constexpr uint32_t MDIO_STATUS = 0x0c;
constexpr uint32_t MDIO_DIV = 0x10;
constexpr uint32_t MDIO_ID = 0x14;

constexpr uint32_t CTRL_START = 1u << 24;
constexpr uint32_t CTRL_READ = 1u << 16;
constexpr uint32_t STATUS_BUSY = 1u << 0;
constexpr uint32_t STATUS_RVALID = 1u << 1;

uint32_t ctrl_word(uint8_t phyad, uint8_t regad, bool read) {
  return CTRL_START | (read ? CTRL_READ : 0) | (uint32_t(phyad & 0x1f) << 8) |
         (regad & 0x1f);
}

// Waits for the serial frame to finish.  A frame is 64 MDC periods and MDC is
// the bus clock over 26 by default, so this is not quick.
void wait_idle(Env& env) {
  const bool ok = env.sim().run_until(
      [&]() { return (env.mdio_host().read32(MDIO_STATUS) & STATUS_BUSY) == 0; },
      2 * MS);
  CHECK_MSG(ok, "the MDIO transfer never finished");
}

void mdio_write(Env& env, uint8_t phyad, uint8_t regad, uint16_t value) {
  env.mdio_host().write32(MDIO_WDATA, value);
  env.mdio_host().write32(MDIO_CTRL, ctrl_word(phyad, regad, false));
  wait_idle(env);
}

uint16_t mdio_read(Env& env, uint8_t phyad, uint8_t regad) {
  env.mdio_host().write32(MDIO_CTRL, ctrl_word(phyad, regad, true));
  wait_idle(env);
  CHECK_MSG(env.mdio_host().read32(MDIO_STATUS) & STATUS_RVALID,
            "the read finished without marking its data good");
  return uint16_t(env.mdio_host().read32(MDIO_RDATA));
}

// The frame Clause 22 asks for, written out as one character per MDC rising
// edge: '1' and '0' for bits the station drives, 'z' for bit times where it has
// let go of the wire.  Built from the field widths and opcodes, not from what
// wb_mdio does, so it is a statement about the standard rather than a copy of
// the implementation.
std::string expected_frame(uint8_t phyad, uint8_t regad, bool read,
                           uint16_t data) {
  std::string s(mii::PREAMBLE_BITS, '1');

  auto bits = [&s](uint32_t v, int n) {
    for (int i = n - 1; i >= 0; i--) s.push_back(((v >> i) & 1) ? '1' : '0');
  };

  bits(mii::ST, 2);
  bits(read ? mii::OP_READ : mii::OP_WRITE, 2);
  bits(phyad, mii::ADDR_BITS);
  bits(regad, mii::ADDR_BITS);

  if (read) {
    // The station lets go for the turnaround so the PHY can answer, and stays
    // off the wire for the data.  Where it lets go is the whole question: a bit
    // time late and it fights the PHY for the first turnaround bit, a bit time
    // early and the last register address bit never gets driven.
    s.append(mii::TA_BITS + mii::DATA_BITS, 'z');
  } else {
    bits(mii::TA_WRITE, mii::TA_BITS);
    bits(data, mii::DATA_BITS);
  }
  return s;
}

// Records what the station puts on MDIO, one character per MDC rising edge,
// which is the edge everything on this bus samples on.  Capture starts at the
// first bit the station drives - the head of the preamble - and stops after one
// whole frame, so a test does one transfer per recorder.
struct FrameRecorder {
  explicit FrameRecorder(Env& env) : env_(env) {
    env.sim().on_negedge(env.sysclk(), [this]() { tick(); });
  }
  const std::string& bits() const { return bits_; }

 private:
  void tick() {
    Vtb_top* d = env_.dut();
    const bool mdc = d->mdio_mdc != 0;
    const bool driving = d->mdio_mdio_oe != 0;
    if (mdc && !prev_mdc_ && int(bits_.size()) < mii::FRAME_BITS &&
        (driving || !bits_.empty())) {
      bits_.push_back(driving ? (d->mdio_mdio_o ? '1' : '0') : 'z');
    }
    prev_mdc_ = mdc;
  }
  Env& env_;
  std::string bits_;
  bool prev_mdc_ = false;
};

}  // namespace

// Pins the constants in tb/cpp/mii.h to an independent reference, the way
// test_layout.cpp pins the 82586 structures to i82586reg.h.
//
// The right hand sides are copied from doc/drivers/NetBSD/mii.h, with its own
// names in the comments.  Ours are Linux's names and NetBSD's are not, which is
// the point: nothing is shared between the two but the numbers, so a wrong bit
// position cannot be spelled the same way in both places.  Everything in this
// file and in mdio_prog.sv is built out of these, so getting one wrong would
// otherwise be invisible - the station, the PHY model and the tests would agree
// with each other and with no real PHY.
TEST(mdio_constants_match_the_reference) {
  (void)env;
  // Register numbers.
  CHECK_EQ(mii::REG_CONTROL, 0x00);       // MII_BMCR
  CHECK_EQ(mii::REG_STATUS, 0x01);        // MII_BMSR
  CHECK_EQ(mii::REG_ID1, 0x02);           // MII_PHYIDR1
  CHECK_EQ(mii::REG_ID2, 0x03);           // MII_PHYIDR2
  CHECK_EQ(mii::REG_ADVERTISE, 0x04);     // MII_ANAR
  CHECK_EQ(mii::REG_LP_ABILITY, 0x05);    // MII_ANLPAR
  CHECK_EQ(mii::REG_GIG_CONTROL, 0x09);   // MII_GTCR

  // Frame fields.  MII_COMMAND_* are the values NetBSD shifts out when it has
  // to drive the lines by hand, so they pin the delimiter and the opcodes.
  CHECK_EQ(mii::ST, 0x01);          // MII_COMMAND_START
  CHECK_EQ(mii::OP_READ, 0x02);     // MII_COMMAND_READ
  CHECK_EQ(mii::OP_WRITE, 0x01);    // MII_COMMAND_WRITE
  CHECK_EQ(mii::NPHY, 32);          // MII_NPHY
  CHECK_EQ(mii::ADDR_BITS, 5);      // MII_ADDRBITS
  CHECK_EQ(int(mii::ADDR_MASK), 0x1f);   // MII_ADDRMASK

  // Control register.
  CHECK_EQ(mii::CTRL_RESET, 0x8000);            // BMCR_RESET
  CHECK_EQ(mii::CTRL_LOOPBACK, 0x4000);         // BMCR_LOOP
  CHECK_EQ(mii::CTRL_SPEED_LSB, 0x2000);        // BMCR_SPEED0
  CHECK_EQ(mii::CTRL_AUTONEG_EN, 0x1000);       // BMCR_AUTOEN
  CHECK_EQ(mii::CTRL_POWERDOWN, 0x0800);        // BMCR_PDOWN
  CHECK_EQ(mii::CTRL_ISOLATE, 0x0400);          // BMCR_ISO
  CHECK_EQ(mii::CTRL_AUTONEG_RESTART, 0x0200);  // BMCR_STARTNEG
  CHECK_EQ(mii::CTRL_FULL_DUPLEX, 0x0100);      // BMCR_FDX
  CHECK_EQ(mii::CTRL_SPEED_MSB, 0x0040);        // BMCR_SPEED1

  // Status register.
  CHECK_EQ(mii::STAT_AUTONEG_DONE, 0x0020);   // BMSR_ACOMP
  CHECK_EQ(mii::STAT_AUTONEG_ABLE, 0x0008);   // BMSR_ANEG
  CHECK_EQ(mii::STAT_LINK, 0x0004);           // BMSR_LINK
  CHECK_EQ(mii::STAT_EXT_CAP, 0x0001);        // BMSR_EXTCAP

  // Advertisement, and the gigabit register it does not cover.
  CHECK_EQ(mii::ADV_CSMA, 0x0001);            // ANAR_CSMA
  CHECK_EQ(mii::ADV_10_HDX, 0x0020);          // ANAR_10
  CHECK_EQ(mii::ADV_10_FDX, 0x0040);          // ANAR_10_FD
  CHECK_EQ(mii::ADV_100_HDX, 0x0080);         // ANAR_TX
  CHECK_EQ(mii::ADV_100_FDX, 0x0100);         // ANAR_TX_FD
  CHECK_EQ(mii::GIG_ADV_1000_HDX, 0x0100);    // GTCR_ADV_1000THDX
  CHECK_EQ(mii::GIG_ADV_1000_FDX, 0x0200);    // GTCR_ADV_1000TFDX
}

// ---------------------------------------------------------------------------
// The frame on the wire
// ---------------------------------------------------------------------------
//
// Everything else here checks the station against tb/cpp/mdio_phy.cpp, and I
// wrote both, so between them they can only prove the two agree.  These two
// compare the MDIO pin against a frame written out bit by bit instead, which is
// the one check in this file that does not go through the model.

TEST(mdio_write_frame_matches_the_standard) {
  FrameRecorder rec(env);

  // A value with no run longer than two, so a bit dropped or repeated shows up
  // as a mismatch rather than landing on an identical neighbour.
  mdio_write(env, 1, mii::REG_ADVERTISE, 0x2cb3);

  CHECK_EQ(rec.bits(), expected_frame(1, mii::REG_ADVERTISE, false, 0x2cb3));

  // And the model read the same frame the same way.
  CHECK_EQ(env.mdio_phy().reg(mii::REG_ADVERTISE), 0x2cb3);
}

TEST(mdio_read_frame_matches_the_standard) {
  env.mdio_phy().set_reg(mii::REG_ID1, 0x2cb3);

  FrameRecorder rec(env);
  CHECK_EQ(mdio_read(env, 1, mii::REG_ID1), 0x2cb3);

  // The station's half of a read: the same header, then eighteen bit times off
  // the wire.  Nothing here looks at what the PHY drove back, only at when the
  // station stopped driving and that it stayed off until the frame ended.
  CHECK_EQ(rec.bits(), expected_frame(1, mii::REG_ID1, true, 0));
}

TEST(mdio_registers_read_back) {
  CHECK_EQ(env.mdio_host().read32(MDIO_ID), 0x4d444a4fu);
  CHECK(!env.mdio_host().last_error());

  // Nothing is in flight to start with.
  CHECK_EQ(env.mdio_host().read32(MDIO_STATUS) & STATUS_BUSY, 0u);

  env.mdio_host().write32(MDIO_WDATA, 0xbeef);
  CHECK_EQ(env.mdio_host().read32(MDIO_WDATA), 0xbeefu);
  env.mdio_host().write32(MDIO_DIV, 3);
  CHECK_EQ(env.mdio_host().read32(MDIO_DIV), 3u);

  // The start bit is a pulse and never reads back.
  env.mdio_host().write32(MDIO_CTRL, ctrl_word(5, 9, true));
  CHECK_EQ(env.mdio_host().read32(MDIO_CTRL) & CTRL_START, 0u);
  CHECK_EQ(env.mdio_host().read32(MDIO_CTRL) & 0x1fu, 9u);
  CHECK_EQ((env.mdio_host().read32(MDIO_CTRL) >> 8) & 0x1fu, 5u);
  wait_idle(env);
}

TEST(mdio_writes_reach_the_phy) {
  mdio_write(env, 1, mii::REG_CONTROL, 0x1234);
  mdio_write(env, 1, mii::REG_ADVERTISE, 0x0101);

  const std::vector<MdioPhy::Write>& w = env.mdio_phy().writes();
  CHECK_EQ(w.size(), size_t(2));
  CHECK_EQ(w[0].phyad, 1);
  CHECK_EQ(w[0].regad, 0);
  CHECK_EQ(w[0].value, 0x1234);
  CHECK_EQ(w[1].regad, 4);
  CHECK_EQ(w[1].value, 0x0101);
  CHECK_MSG(!env.mdio_phy().short_preamble_seen(),
            "the station sent a short preamble");
}

TEST(mdio_reads_come_back) {
  env.mdio_phy().set_reg(mii::REG_ID1, 0x001c);   // a PHY identifier, say
  env.mdio_phy().set_reg(mii::REG_ID2, 0xc915);
  CHECK_EQ(mdio_read(env, 1, mii::REG_ID1), 0x001c);
  CHECK_EQ(mdio_read(env, 1, mii::REG_ID2), 0xc915);

  // All ones and all zeroes, since the turnaround is where a read goes wrong.
  env.mdio_phy().set_reg(0x05, 0xffff);
  CHECK_EQ(mdio_read(env, 1, 0x05), 0xffff);
  env.mdio_phy().set_reg(0x05, 0x0000);
  CHECK_EQ(mdio_read(env, 1, 0x05), 0x0000);
}

TEST(mdio_ignores_other_phy_addresses) {
  // The model answers on address 1 only.
  mdio_write(env, 7, 0x04, 0xaaaa);
  CHECK_MSG(env.mdio_phy().writes().empty(),
            "a write addressed to another PHY was taken");
  CHECK_EQ(env.mdio_phy().reg(0x04), 0x0000);
}

TEST(mdio_divider_changes_the_clock) {
  // MDC is the bus clock over 2*(DIV+1).  Time a few frames at two settings
  // and check the faster one really is faster.
  env.mdio_host().write32(MDIO_DIV, 24);
  const u64 t0 = env.sim().time_ps();
  mdio_write(env, 1, 0x04, 0x0001);
  const u64 slow = env.sim().time_ps() - t0;

  env.mdio_host().write32(MDIO_DIV, 2);
  const u64 t1 = env.sim().time_ps();
  mdio_write(env, 1, 0x04, 0x0002);
  const u64 fast = env.sim().time_ps() - t1;

  CHECK_MSG(fast * 4 < slow, "the divider did not change the MDC rate");
  CHECK_EQ(env.mdio_phy().reg(0x04), 0x0002);
}

// ---------------------------------------------------------------------------
// The programming block
// ---------------------------------------------------------------------------

namespace {

// What each of the three configurations in tb_top should end up asking for.
struct Expect {
  int index;
  const char* name;
  uint16_t advertise;    // register 4
  uint16_t ctrl1000;     // register 9
};

void check_programmed(Env& env, const Expect& e) {
  MdioPhy& phy = env.prog_phy(e.index);
  const std::vector<MdioPhy::Write>& w = phy.writes();

  CHECK_MSG(w.size() == 4, "the programming sequence was not four writes");

  // Reset first, so nothing that follows is lost to it.
  CHECK_EQ(w[0].regad, mii::REG_CONTROL);
  CHECK_EQ(w[0].value, mii::CTRL_RESET);

  // Then what to ask for, then negotiate on it.
  CHECK_EQ(w[1].regad, mii::REG_ADVERTISE);
  CHECK_EQ(w[1].value, e.advertise);
  CHECK_EQ(w[2].regad, mii::REG_GIG_CONTROL);
  CHECK_EQ(w[2].value, e.ctrl1000);
  CHECK_EQ(w[3].regad, mii::REG_CONTROL);
  CHECK_EQ(w[3].value,
           uint16_t(mii::CTRL_AUTONEG_EN | mii::CTRL_AUTONEG_RESTART));

  // The restart has to come after the advertisement, or it negotiates on
  // whatever the PHY had before.
  CHECK_MSG(w[3].time_ps > w[1].time_ps && w[3].time_ps > w[2].time_ps,
            "auto-negotiation was restarted before the advertisement was set");
  CHECK_MSG(!phy.short_preamble_seen(), "the station sent a short preamble");
}

bool wait_ready(Env& env, uint8_t* ready) {
  return env.sim().run_until([&]() { return *ready != 0; }, 20 * MS);
}

}  // namespace

TEST(mdio_prog_gigabit_full_duplex) {
  Vtb_top* d = env.dut();
  CHECK_MSG(wait_ready(env, &d->p0_ready), "the PHY was never programmed");
  CHECK_MSG(!d->p0_failed, "the block reported the PHY had not come out of reset");
  // Gigabit is advertised in register 9 and nothing is claimed in register 4
  // beyond the selector, so the link cannot fall back to 100 or 10.
  check_programmed(env, {0, "1000 full", mii::ADV_CSMA, mii::GIG_ADV_1000_FDX});
}

TEST(mdio_prog_100_full_duplex) {
  Vtb_top* d = env.dut();
  CHECK_MSG(wait_ready(env, &d->p1_ready), "the PHY was never programmed");
  check_programmed(env,
                   {1, "100 full", mii::ADV_CSMA | mii::ADV_100_FDX, 0x0000});
}

TEST(mdio_prog_10_half_duplex) {
  Vtb_top* d = env.dut();
  CHECK_MSG(wait_ready(env, &d->p2_ready), "the PHY was never programmed");
  check_programmed(env,
                   {2, "10 half", mii::ADV_CSMA | mii::ADV_10_HDX, 0x0000});
}

TEST(mdio_prog_waits_for_the_reset_to_finish) {
  // The model holds the reset bit set for a few reads.  The block has to poll
  // it rather than carrying on into a PHY that is still resetting.
  Vtb_top* d = env.dut();
  CHECK_MSG(wait_ready(env, &d->p0_ready), "the PHY was never programmed");
  CHECK_MSG(env.prog_phy(0).reads() >= 2,
            "the block never read the control register back");
  CHECK_MSG(!d->p0_failed, "it gave up on a PHY that did come out of reset");
}

TEST(mdio_prog_gives_up_on_a_dead_phy) {
  // A PHY that never clears its reset bit must not wedge the block: it says
  // so and carries on, because a wedged bring-up is worse than a wrong one.
  env.prog_phy(1).set_reset_reads(1000);
  Vtb_top* d = env.dut();
  CHECK_MSG(wait_ready(env, &d->p1_ready),
            "the block never finished with an unresponsive PHY");
  CHECK_MSG(d->p1_failed, "it did not report that the PHY was unresponsive");
  // It still wrote the rest of the sequence.
  CHECK_EQ(env.prog_phy(1).writes().size(), size_t(4));
}

}  // namespace wtb
