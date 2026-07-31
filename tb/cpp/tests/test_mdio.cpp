// SPDX-License-Identifier: MIT
//
// The MDIO station and the block that programs the PHY through it.
//
// None of the 82586 drivers knows a PHY exists, so nothing in the software
// stack will ever set one up.  These check that it gets set up anyway.

#include "test.h"

#include "Vtb_top.h"

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

}  // namespace

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
  mdio_write(env, 1, 0x00, 0x1234);
  mdio_write(env, 1, 0x04, 0x0101);

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
  env.mdio_phy().set_reg(0x02, 0x001c);   // a PHY identifier, say
  env.mdio_phy().set_reg(0x03, 0xc915);
  CHECK_EQ(mdio_read(env, 1, 0x02), 0x001c);
  CHECK_EQ(mdio_read(env, 1, 0x03), 0xc915);

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
  CHECK_EQ(w[0].regad, 0);
  CHECK_EQ(w[0].value, 0x8000);

  // Then what to ask for, then negotiate on it.
  CHECK_EQ(w[1].regad, 4);
  CHECK_EQ(w[1].value, e.advertise);
  CHECK_EQ(w[2].regad, 9);
  CHECK_EQ(w[2].value, e.ctrl1000);
  CHECK_EQ(w[3].regad, 0);
  CHECK_EQ(w[3].value, uint16_t(0x1000 | 0x0200));  // enable and restart

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
  check_programmed(env, {0, "1000 full", 0x0001, 0x0200});
}

TEST(mdio_prog_100_full_duplex) {
  Vtb_top* d = env.dut();
  CHECK_MSG(wait_ready(env, &d->p1_ready), "the PHY was never programmed");
  check_programmed(env, {1, "100 full", 0x0101, 0x0000});
}

TEST(mdio_prog_10_half_duplex) {
  Vtb_top* d = env.dut();
  CHECK_MSG(wait_ready(env, &d->p2_ready), "the PHY was never programmed");
  check_programmed(env, {2, "10 half", 0x0021, 0x0000});
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
