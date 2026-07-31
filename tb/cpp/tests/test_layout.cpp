// SPDX-License-Identifier: MIT
//
// Pins the shared memory layout to an independent reference.
//
// The constants on the right hand side are copied verbatim from NetBSD's
// doc/drivers/NetBSD/i82586reg.h.  That header is the useful cross check
// because the same definitions drive both a little-endian ISA card (ai, plain
// bus_space_read_2) and big-endian Sun machines (ie, whose accessor byte swaps
// on the way through) - so they describe the chip's own view of memory, which
// is exactly what the RTL sees.
//
// Getting this wrong is silent: the testbench and the RTL agree with each
// other and every system test passes while neither matches real hardware.
// That is what these checks are here to catch.
//
// These tests tie the testbench to the reference.  The RTL is tied to the
// testbench by the system tests - sys_init_sequence waits on CX and CNA, and
// sys_command_suspend_and_resume waits on a non-zero CUS - so between them the
// RTL is pinned to the reference too.

#include "test.h"

namespace wtb {

TEST(layout_scb_matches_reference) {
  (void)env;
  // Status word: IE_ST_*
  CHECK_EQ(ie::SCB_ST_CX, 0x8000);
  CHECK_EQ(ie::SCB_ST_FR, 0x4000);
  CHECK_EQ(ie::SCB_ST_CNA, 0x2000);
  CHECK_EQ(ie::SCB_ST_RNR, 0x1000);

  // IE_CUS_MASK 0x0700, IE_RUS_MASK 0x0070
  CHECK_EQ(7u << ie::SCB_ST_CUS_LSB, 0x0700u);
  CHECK_EQ(7u << ie::SCB_ST_RUS_LSB, 0x0070u);
  CHECK_EQ(uint16_t(ie::CUS_ACTIVE << ie::SCB_ST_CUS_LSB), 0x0200);
  CHECK_EQ(uint16_t(ie::CUS_SUSPENDED << ie::SCB_ST_CUS_LSB), 0x0100);
  CHECK_EQ(uint16_t(ie::RUS_SUSPENDED << ie::SCB_ST_RUS_LSB), 0x0010);
  CHECK_EQ(uint16_t(ie::RUS_NO_RESOURCE << ie::SCB_ST_RUS_LSB), 0x0020);
  CHECK_EQ(uint16_t(ie::RUS_READY << ie::SCB_ST_RUS_LSB), 0x0040);

  // Command word: IE_ACK_*, IE_CUC_*, IE_RUC_*
  CHECK_EQ(ie::SCB_CMD_ACK_CX, 0x8000);
  CHECK_EQ(ie::SCB_CMD_ACK_FR, 0x4000);
  CHECK_EQ(ie::SCB_CMD_ACK_CNA, 0x2000);
  CHECK_EQ(ie::SCB_CMD_ACK_RNR, 0x1000);
  CHECK_EQ(7u << ie::SCB_CMD_CUC_LSB, 0x0700u);
  CHECK_EQ(7u << ie::SCB_CMD_RUC_LSB, 0x0070u);
  CHECK_EQ(uint16_t(ie::CUC_START << ie::SCB_CMD_CUC_LSB), 0x0100);
  CHECK_EQ(uint16_t(ie::CUC_RESUME << ie::SCB_CMD_CUC_LSB), 0x0200);
  CHECK_EQ(uint16_t(ie::CUC_SUSPEND << ie::SCB_CMD_CUC_LSB), 0x0300);
  CHECK_EQ(uint16_t(ie::CUC_ABORT << ie::SCB_CMD_CUC_LSB), 0x0400);
  CHECK_EQ(uint16_t(ie::RUC_START << ie::SCB_CMD_RUC_LSB), 0x0010);
  CHECK_EQ(uint16_t(ie::RUC_RESUME << ie::SCB_CMD_RUC_LSB), 0x0020);
  CHECK_EQ(uint16_t(ie::RUC_SUSPEND << ie::SCB_CMD_RUC_LSB), 0x0030);
  CHECK_EQ(uint16_t(ie::RUC_ABORT << ie::SCB_CMD_RUC_LSB), 0x0040);

  // The software reset bit is not in the NetBSD header, which resets in
  // hardware.  It comes from the Sun ROM's IECMD_RESET 0x8000, which is
  // 0x0080 once the machine's byte swap is undone.
  CHECK_EQ(ie::SCB_CMD_RESET, 0x0080);
}

TEST(layout_command_block_matches_reference) {
  (void)env;
  // IE_STAT_*
  CHECK_EQ(ie::CB_ST_C, 0x8000);
  CHECK_EQ(ie::CB_ST_B, 0x4000);
  CHECK_EQ(ie::CB_ST_OK, 0x2000);
  CHECK_EQ(ie::CB_ST_A, 0x1000);

  // IE_CMD_LAST / SUSPEND / INTR
  CHECK_EQ(ie::CB_CMD_EL, 0x8000);
  CHECK_EQ(ie::CB_CMD_S, 0x4000);
  CHECK_EQ(ie::CB_CMD_I, 0x2000);

  // IE_CMD_* opcodes
  CHECK_EQ(int(ie::CMD_NOP), 0);
  CHECK_EQ(int(ie::CMD_IA_SETUP), 1);
  CHECK_EQ(int(ie::CMD_CONFIGURE), 2);
  CHECK_EQ(int(ie::CMD_MC_SETUP), 3);
  CHECK_EQ(int(ie::CMD_TRANSMIT), 4);
  CHECK_EQ(int(ie::CMD_TDR), 5);
  CHECK_EQ(int(ie::CMD_DUMP), 6);
  CHECK_EQ(int(ie::CMD_DIAGNOSE), 7);

  // IE_XS_* transmit status
  CHECK_EQ(ie::TX_ST_NCOL_MASK, 0x000f);
  CHECK_EQ(ie::TX_ST_XCOLL, 0x0020);
  CHECK_EQ(ie::TX_ST_HEART, 0x0040);
  CHECK_EQ(ie::TX_ST_DEFER, 0x0080);
  CHECK_EQ(ie::TX_ST_UNDERRUN, 0x0100);
  CHECK_EQ(ie::TX_ST_NO_CTS, 0x0200);
  CHECK_EQ(ie::TX_ST_NO_CRS, 0x0400);
  CHECK_EQ(ie::TX_ST_LATECOLL, 0x0800);
}

TEST(layout_receive_side_matches_reference) {
  (void)env;
  // IE_FD_* receive frame descriptor
  CHECK_EQ(ie::RFD_ST_C, 0x8000);
  CHECK_EQ(ie::RFD_ST_B, 0x4000);
  CHECK_EQ(ie::RFD_ST_OK, 0x2000);
  CHECK_EQ(ie::RFD_ST_CRC, 0x0800);
  CHECK_EQ(ie::RFD_ST_ALIGN, 0x0400);
  CHECK_EQ(ie::RFD_ST_NO_SPACE, 0x0200);
  CHECK_EQ(ie::RFD_ST_OVERRUN, 0x0100);
  CHECK_EQ(ie::RFD_ST_SHORT, 0x0080);
  CHECK_EQ(ie::RFD_ST_NO_EOF, 0x0040);
  CHECK_EQ(ie::RFD_CMD_EL, 0x8000);
  CHECK_EQ(ie::RFD_CMD_S, 0x4000);

  // IE_RBD_* receive buffer descriptor
  CHECK_EQ(ie::RBD_EOF, 0x8000);   // IE_RBD_LAST
  CHECK_EQ(ie::RBD_F, 0x4000);     // IE_RBD_USED
  CHECK_EQ(ie::RBD_COUNT_MASK, 0x3fff);
  CHECK_EQ(ie::RBD_EL, 0x8000);
}

TEST(layout_structure_offsets_match_reference) {
  ie::MemImage& img = env.img();
  img.build_init_structures();
  WbMem& m = env.mem();

  // IE_SCP_ADDR is 0xfffff4 with the bus width byte at +2 and the ISCP
  // pointer at +8.  We name the SCP by its bus width byte, the way the Sun
  // ROM's IESCPADDR does, so our base sits two bytes further on and the
  // absolute addresses have to come out the same.
  CHECK_EQ(ie::SCP_ADDR_DEFAULT, 0x00fffff6u);           // 0xfffff4 + 2
  CHECK_EQ(ie::SCP_ADDR_DEFAULT + 6, 0x00fffffcu);       // 0xfffff4 + 8

  // IE_ISCP_BUSY +0, IE_ISCP_SCB +2, IE_ISCP_BASE +4
  CHECK_EQ(m.rd8(img.iscp_addr() + 0), 1);               // busy, as we set it
  CHECK_EQ(m.rd16(img.iscp_addr() + 2), img.scb_off());
  CHECK_EQ(m.rd24(img.iscp_addr() + 4), img.cbbase());

  // IE_CMD_COMMON: status +0, cmd +2, link +4.
  const uint16_t cb = img.add_nop();
  CHECK_EQ(m.rd16(img.addr_of(cb) + 0), img.cb_status(cb));
  CHECK_EQ(m.rd16(img.addr_of(cb) + 2), img.cb_cmd(cb));
  CHECK_EQ(m.rd16(img.addr_of(cb) + 4), img.cb_link(cb));

  // IE_CMD_XMIT: desc at common+0, address at common+2, length at common+8.
  EthFrame f(env.peer_mac(), env.local_mac(), 0x0800, random_payload(64, 1));
  const uint16_t tx = img.add_transmit(f, false);
  CHECK_EQ(m.read_block(img.addr_of(tx) + 6 + 2, 6),
           Bytes(f.dst.b.begin(), f.dst.b.end()));
  CHECK_EQ(m.rd8(img.addr_of(tx) + 6 + 8), uint8_t(f.type_len >> 8));

  // IE_XBD: flags +0, next +2, buffer +4, with the count in the low bits of
  // the flags word and IE_TBD_EOL on the last one.
  const uint16_t tbd = m.rd16(img.addr_of(tx) + 6);
  CHECK_EQ(m.rd16(img.addr_of(tbd)) & 0x8000, 0x8000);
  CHECK_EQ(m.rd16(img.addr_of(tbd)) & 0x3fff, uint16_t(f.payload.size()));
  CHECK_EQ(m.rd16(img.addr_of(tbd) + 2), ie::NULL_PTR);

  // IE_RFRAME: status +0, last +2, next +4, bufdesc +6, dst +8, src +14,
  // length +20.  IE_RBD: status +0, next +2, buffer +4, size +8.
  const uint16_t rfa = img.build_rfa(2, 2, 128);
  CHECK_EQ(m.rd16(img.addr_of(rfa) + 4), img.rfds()[1]);
  const uint16_t rbd = m.rd16(img.addr_of(rfa) + 6);
  CHECK_NE(rbd, ie::NULL_PTR);
  CHECK_EQ(m.rd16(img.addr_of(rbd) + 8) & 0x3fff, uint16_t(128));
}

}  // namespace wtb
