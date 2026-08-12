// SPDX-License-Identifier: MIT
//
// NetBSD's own MDIO station, wired to the testbench's pins.
//
// Everything else on the MDIO side checks wb_mdio against tb/cpp/mdio_phy.cpp,
// and both of those were written here from clause 22, so between them they can
// only show that the station and the model agree.  The 82586 side has a way out
// of that - the drivers are the specification and cosim/ runs a real one - but
// no driver in doc/drivers has ever heard of a PHY, so the MDIO side had nothing
// outside itself at all.
//
// This is the way out: doc/drivers/NetBSD/mii_bitbang.c, compiled unmodified
// and pointed at a MdioPhy.  It is the code that has bit-banged MDIO for every
// NetBSD machine whose MAC had no station of its own since 1999, so a frame it
// builds is a frame real PHYs have answered for twenty-odd years.  If our model
// understands it, the model is no longer just our reading of the standard - and
// the ten tests that check wb_mdio against that model inherit the difference.
//
// It drives the model directly and no RTL is involved, which is why the tests
// using it are infra_: they check the testbench, and the trust ordering in
// CLAUDE.md puts that below everything that depends on it.
//
// Two things are worth knowing.
//
// The driver paces the bus with delay(), so simulated time has to advance
// inside it; the tests that use this therefore call it from test context and
// never from a clock callback, the same rule as the blocking bus helpers.
//
// It samples read data while MDC is low, just before raising it, where wb_mdio
// samples on the rising edge.  Both are within what clause 22 allows, and that
// they land on the same bit is a fact about the model's timing rather than an
// assumption of it - which is most of what this is here to check.

#pragma once

#include <cstdint>

#include "mdio_phy.h"
#include "sim.h"

namespace wtb {

class NetBsdStation {
 public:
  // Only one of these may exist at a time: delay() has to find the simulation
  // from a file static, because the driver's own signature has nowhere to carry
  // it.  The constructor claims that pointer and the destructor drops it.
  explicit NetBsdStation(Sim& sim);
  ~NetBsdStation();

  NetBsdStation(const NetBsdStation&) = delete;
  NetBsdStation& operator=(const NetBsdStation&) = delete;

  // The four wires, for a MdioPhy to attach to.  Nothing in the RTL is
  // connected: this is one model driving another.
  MdioPorts ports();

  // Both return what the driver returned: 0 for success, and for a read -1 if
  // the PHY never drove the zero it owes on the turnaround.
  int read(uint8_t phyad, uint8_t regad, uint16_t* val);
  int write(uint8_t phyad, uint8_t regad, uint16_t val);

  // How many MDC rising edges have gone by, so a test can show the driver is
  // clocking the bus rather than sitting still.
  int edges() const { return edges_; }

  // Called from the driver's ops; public only because they are.
  uint32_t pins_in() const;
  void pins_out(uint32_t v);

 private:
  Sim& sim_;
  uint8_t mdc_ = 0;
  uint8_t mdio_o_ = 1;
  uint8_t mdio_oe_ = 0;
  uint8_t mdio_i_ = 1;   // written by the PHY model
  int edges_ = 0;
};

}  // namespace wtb
