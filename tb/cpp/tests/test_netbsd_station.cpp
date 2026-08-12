// SPDX-License-Identifier: MIT
//
// The PHY model, against a station nobody here wrote.
//
// tb/cpp/mdio_phy.cpp is what every other MDIO test measures wb_mdio with, and
// it was written from the same reading of clause 22 as wb_mdio itself.  These
// point NetBSD's own bit-banging station at it instead - the unmodified
// doc/drivers/NetBSD/mii_bitbang.c - so that the frames the model is asked to
// understand come from somewhere else.  See tb/cpp/netbsd_station.h.
//
// No RTL is involved: this is one model driving another, which is what makes
// these infra_ rather than unit_.  If they fail, every other MDIO result is
// worth nothing, because they all go through the model these check.

#include "test.h"

#include "mii.h"
#include "netbsd_station.h"

namespace wtb {

namespace {

// A PHY at address 1 listening to NetBSD's station, with a couple of registers
// worth reading.  MDC comes from the driver, so the model's sample clock only
// has to be quicker than the bus - the system clock is, by a wide margin.
struct Rig {
  explicit Rig(Env& env, uint8_t addr = 1)
      : station(env.sim()), phy(env.sim(), env.sysclk(), station.ports(), addr) {}
  NetBsdStation station;
  MdioPhy phy;
};

}  // namespace

// The driver's read path: preamble, header, turnaround, sixteen bits back.  It
// samples while MDC is low, just before raising it, where wb_mdio samples on the
// rising edge - so this says the model presents each bit for the whole bit time
// and not merely at the instant our own station looks.
TEST(infra_mdio_phy_answers_the_netbsd_driver) {
  Rig rig(env);
  rig.phy.set_reg(mii::REG_ID1, 0x2cb3);
  rig.phy.set_reg(mii::REG_ID2, 0x0e1a);

  uint16_t v = 0;
  CHECK_EQ(rig.station.read(1, mii::REG_ID1, &v), 0);
  CHECK_EQ(v, 0x2cb3);

  CHECK_EQ(rig.station.read(1, mii::REG_ID2, &v), 0);
  CHECK_EQ(v, 0x0e1a);

  // All ones and all zeroes, because the turnaround is where a read goes wrong
  // and a register of ones hides a station that never let go of the wire.
  rig.phy.set_reg(mii::REG_STATUS, 0xffff);
  CHECK_EQ(rig.station.read(1, mii::REG_STATUS, &v), 0);
  CHECK_EQ(v, 0xffff);

  rig.phy.set_reg(mii::REG_STATUS, 0x0000);
  CHECK_EQ(rig.station.read(1, mii::REG_STATUS, &v), 0);
  CHECK_EQ(v, 0x0000);

  // Each frame is 64 bit times and the driver sends its own 32 bit preamble
  // ahead of every one, so four reads cannot have cost fewer than this.
  CHECK_MSG(rig.station.edges() >= 4 * mii::FRAME_BITS,
            "the driver did not clock a whole frame per transfer");
}

TEST(infra_mdio_phy_takes_a_write_from_the_netbsd_driver) {
  Rig rig(env);

  CHECK_EQ(rig.station.write(1, mii::REG_ADVERTISE,
                             mii::ADV_CSMA | mii::ADV_100_FDX),
           0);
  CHECK_EQ(rig.station.write(1, mii::REG_CONTROL, mii::CTRL_RESET), 0);

  const std::vector<MdioPhy::Write>& w = rig.phy.writes();
  CHECK_EQ(w.size(), size_t(2));
  CHECK_EQ(w[0].phyad, 1);
  CHECK_EQ(w[0].regad, mii::REG_ADVERTISE);
  CHECK_EQ(w[0].value, uint16_t(mii::ADV_CSMA | mii::ADV_100_FDX));
  CHECK_EQ(w[1].regad, mii::REG_CONTROL);
  CHECK_EQ(w[1].value, mii::CTRL_RESET);

  CHECK_MSG(!rig.phy.short_preamble_seen(),
            "the model thought the driver sent a short preamble");
}

// Reading a register straight back is how a driver checks it took, and it is
// also the one sequence that exercises both directions against each other.
TEST(infra_mdio_phy_round_trips_with_the_netbsd_driver) {
  Rig rig(env);

  const uint16_t value = mii::ADV_CSMA | mii::ADV_10_HDX | mii::ADV_100_FDX;
  CHECK_EQ(rig.station.write(1, mii::REG_ADVERTISE, value), 0);

  uint16_t v = 0;
  CHECK_EQ(rig.station.read(1, mii::REG_ADVERTISE, &v), 0);
  CHECK_EQ(v, value);
}

TEST(infra_mdio_phy_ignores_the_netbsd_driver_on_another_address) {
  Rig rig(env);

  CHECK_EQ(rig.station.write(7, mii::REG_ADVERTISE, 0xaaaa), 0);
  CHECK_MSG(rig.phy.writes().empty(),
            "a write addressed to another PHY was taken");
  CHECK_EQ(rig.phy.reg(mii::REG_ADVERTISE), 0x0000);
}

// How NetBSD finds out which of the 32 addresses have a PHY on them: it reads,
// and an address with nothing on it leaves the wire on the pull-up through the
// turnaround, where a PHY would have driven a zero.  mii_bitbang_readreg calls
// that -1.  Our model has to be absent convincingly, not just quiet.
TEST(infra_netbsd_driver_finds_no_phy_at_an_empty_address) {
  Rig rig(env);
  rig.phy.set_reg(mii::REG_ID1, 0x2cb3);

  uint16_t v = 0xdead;
  CHECK_EQ(rig.station.read(9, mii::REG_ID1, &v), -1);
  CHECK_MSG(v == 0xdead, "a failed read wrote to the caller's value");

  // And the address that is there still answers afterwards, so nothing about
  // the failed probe left the model confused.
  CHECK_EQ(rig.station.read(1, mii::REG_ID1, &v), 0);
  CHECK_EQ(v, 0x2cb3);
}

}  // namespace wtb
