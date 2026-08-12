// SPDX-License-Identifier: MIT

#include "netbsd_station.h"

// The driver's own declarations.  Its header wants device_t and the fixed width
// types and nothing else, so it is included here directly rather than through
// tb/cpp/netbsd/ - that shim is on the include path for mii_bitbang.c alone,
// because headers called sys/param.h have no business shadowing the real ones
// for the rest of the testbench.
extern "C" {
typedef void *device_t;
#include "../../doc/drivers/NetBSD/mii_bitbang.h"
}

namespace wtb {

namespace {

// Where each MDIO wire sits in the word the driver reads and writes.  A real
// card has a register with the four signals somewhere in it; this is that
// register, invented for the purpose.  MII_BIT_DIR_PHY_HOST is zero because
// letting go is the absence of the drive bit rather than a bit of its own,
// which is how the cards NetBSD supports mostly do it.
constexpr uint32_t BIT_MDO = 1u << 0;
constexpr uint32_t BIT_MDI = 1u << 1;
constexpr uint32_t BIT_MDC = 1u << 2;
constexpr uint32_t BIT_DIR_HOST_PHY = 1u << 3;
constexpr uint32_t BIT_DIR_PHY_HOST = 0;

// delay() has no argument to carry the simulation in, so it comes from here.
Sim* g_sim = nullptr;

}  // namespace

// ---------------------------------------------------------------------------
// what the driver calls
// ---------------------------------------------------------------------------

extern "C" void delay(unsigned int usec) {
  if (g_sim != nullptr) g_sim->run_ps(u64(usec) * US);
}

extern "C" uint32_t wtb_netbsd_mdio_read(device_t sc) {
  return static_cast<NetBsdStation*>(sc)->pins_in();
}

extern "C" void wtb_netbsd_mdio_write(device_t sc, uint32_t v) {
  static_cast<NetBsdStation*>(sc)->pins_out(v);
}

namespace {

const struct mii_bitbang_ops kOps = {
    wtb_netbsd_mdio_read,
    wtb_netbsd_mdio_write,
    {BIT_MDO, BIT_MDI, BIT_MDC, BIT_DIR_HOST_PHY, BIT_DIR_PHY_HOST},
};

}  // namespace

// ---------------------------------------------------------------------------

NetBsdStation::NetBsdStation(Sim& sim) : sim_(sim) { g_sim = &sim; }

NetBsdStation::~NetBsdStation() { g_sim = nullptr; }

MdioPorts NetBsdStation::ports() {
  MdioPorts p;
  p.mdc = &mdc_;
  p.mdio_o = &mdio_o_;
  p.mdio_oe = &mdio_oe_;
  p.mdio_i = &mdio_i_;
  return p;
}

uint32_t NetBsdStation::pins_in() const {
  // Only MDI is an input.  The rest of the word reads back as whatever was
  // last written, which is what a real register would do and what the driver
  // assumes when it ORs MDC into a value it already holds.
  return (mdio_i_ != 0 ? BIT_MDI : 0) | (mdio_o_ != 0 ? BIT_MDO : 0) |
         (mdc_ != 0 ? BIT_MDC : 0) |
         (mdio_oe_ != 0 ? BIT_DIR_HOST_PHY : BIT_DIR_PHY_HOST);
}

void NetBsdStation::pins_out(uint32_t v) {
  const bool mdc = (v & BIT_MDC) != 0;
  if (mdc && mdc_ == 0) edges_++;
  mdc_ = mdc ? 1 : 0;
  mdio_o_ = (v & BIT_MDO) != 0 ? 1 : 0;
  // The driver names the two directions separately, so honour the bit it uses
  // for "host drives" rather than assuming its absence means anything.
  mdio_oe_ = (v & BIT_DIR_HOST_PHY) != 0 ? 1 : 0;
}

int NetBsdStation::read(uint8_t phyad, uint8_t regad, uint16_t* val) {
  return mii_bitbang_readreg(this, &kOps, phyad, regad, val);
}

int NetBsdStation::write(uint8_t phyad, uint8_t regad, uint16_t val) {
  return mii_bitbang_writereg(this, &kOps, phyad, regad, val);
}

}  // namespace wtb
