// SPDX-License-Identifier: MIT
//
// MII management constants for the testbench.
//
// The same arrangement as wish82586_pkg.sv and i82586.h: the RTL has its own
// copy - the localparams in mdio_prog.sv - and this is the testbench's,
// written out separately on purpose so the two can disagree and be caught at
// it.  Note the RTL uses Linux's names for these bits and this uses plainer
// ones; that is deliberate, since a shared name is the easiest way for one
// mistake to end up in both places.
//
// doc/drivers/NetBSD/mii.h is the reference both are pinned to, by
// mdio_constants_match_the_reference in tests/test_mdio.cpp.  It is a better
// authority than either of us: it has driven every PHY NetBSD supports for
// twenty-odd years, so a wrong bit position in it would have been found.
//
// None of this has anything to do with the 82586, which predates MDIO by a
// decade and whose drivers know nothing about PHYs.  It is here because an
// FPGA build needs its PHY set up regardless - see wb_mdio and mdio_prog.

#pragma once

#include <cstdint>

namespace wtb {
namespace mii {

// ---- the Clause 22 frame ---------------------------------------------------
// 64 bit times: preamble, start delimiter, opcode, PHY address, register
// address, turnaround, data.  Everything on the bus samples on the rising edge
// of MDC; the station changes MDIO on the falling edge.
constexpr int PREAMBLE_BITS = 32;
constexpr int FRAME_BITS = 64;
constexpr int ADDR_BITS = 5;      // both addresses, so 32 PHYs of 32 registers
constexpr int TA_BITS = 2;
constexpr int DATA_BITS = 16;

constexpr uint8_t ST = 0x1;       // start delimiter, 01
constexpr uint8_t OP_READ = 0x2;  // 10
constexpr uint8_t OP_WRITE = 0x1; // 01
constexpr uint8_t TA_WRITE = 0x2; // the station drives 10 for a write

constexpr int NPHY = 32;
constexpr uint8_t ADDR_MASK = 0x1f;

// ---- registers -------------------------------------------------------------
constexpr uint8_t REG_CONTROL = 0x00;
constexpr uint8_t REG_STATUS = 0x01;
constexpr uint8_t REG_ID1 = 0x02;
constexpr uint8_t REG_ID2 = 0x03;
constexpr uint8_t REG_ADVERTISE = 0x04;
constexpr uint8_t REG_LP_ABILITY = 0x05;
constexpr uint8_t REG_GIG_CONTROL = 0x09;

// Control register, 0.
constexpr uint16_t CTRL_RESET = 0x8000;
constexpr uint16_t CTRL_LOOPBACK = 0x4000;
constexpr uint16_t CTRL_SPEED_LSB = 0x2000;
constexpr uint16_t CTRL_AUTONEG_EN = 0x1000;
constexpr uint16_t CTRL_POWERDOWN = 0x0800;
constexpr uint16_t CTRL_ISOLATE = 0x0400;
constexpr uint16_t CTRL_AUTONEG_RESTART = 0x0200;
constexpr uint16_t CTRL_FULL_DUPLEX = 0x0100;
constexpr uint16_t CTRL_SPEED_MSB = 0x0040;

// Status register, 1.
constexpr uint16_t STAT_AUTONEG_DONE = 0x0020;
constexpr uint16_t STAT_AUTONEG_ABLE = 0x0008;
constexpr uint16_t STAT_LINK = 0x0004;
constexpr uint16_t STAT_EXT_CAP = 0x0001;

// Advertisement, 4.  The selector field says CSMA/CD, which every Ethernet
// PHY has to claim or nothing negotiates at all.
constexpr uint16_t ADV_CSMA = 0x0001;
constexpr uint16_t ADV_10_HDX = 0x0020;
constexpr uint16_t ADV_10_FDX = 0x0040;
constexpr uint16_t ADV_100_HDX = 0x0080;
constexpr uint16_t ADV_100_FDX = 0x0100;

// Gigabit control, 9.  Gigabit is advertised here and not in register 4.
constexpr uint16_t GIG_ADV_1000_HDX = 0x0100;
constexpr uint16_t GIG_ADV_1000_FDX = 0x0200;

}  // namespace mii
}  // namespace wtb
