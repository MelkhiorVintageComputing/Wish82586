// SPDX-License-Identifier: MIT
//
// Model of the 82586 shared memory image.
//
// Everything the chip and the host exchange lives in memory: the SCP, the
// ISCP, the SCB, a linked list of command blocks and the receive frame area.
// This class builds those structures inside a WbMem exactly the way the
// vintage drivers in doc/drivers do, and reads back what the DUT wrote.
//
// All fields are little endian; control blocks are addressed by a 16-bit
// offset from CBBASE, buffers by a 24-bit absolute address.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "eth.h"
#include "wb.h"

namespace wtb {
namespace ie {

// ---- SCB status / command -------------------------------------------------
// The chip's own view of the word; see doc/drivers/NetBSD/i82586reg.h.  The
// Sun ROM headers look different because those machines byte swap in hardware
// between the CPU and the shared memory.
constexpr uint16_t SCB_ST_RNR = 1u << 12;
constexpr uint16_t SCB_ST_CNA = 1u << 13;
constexpr uint16_t SCB_ST_FR = 1u << 14;
constexpr uint16_t SCB_ST_CX = 1u << 15;
constexpr int SCB_ST_RUS_LSB = 4;
constexpr int SCB_ST_CUS_LSB = 8;

constexpr uint16_t SCB_CMD_ACK_RNR = 1u << 12;
constexpr uint16_t SCB_CMD_ACK_CNA = 1u << 13;
constexpr uint16_t SCB_CMD_ACK_FR = 1u << 14;
constexpr uint16_t SCB_CMD_ACK_CX = 1u << 15;
constexpr uint16_t SCB_CMD_RESET = 1u << 7;
constexpr int SCB_CMD_CUC_LSB = 8;
constexpr int SCB_CMD_RUC_LSB = 4;

enum Cuc : uint16_t { CUC_NOP = 0, CUC_START = 1, CUC_RESUME = 2, CUC_SUSPEND = 3, CUC_ABORT = 4 };
enum Ruc : uint16_t { RUC_NOP = 0, RUC_START = 1, RUC_RESUME = 2, RUC_SUSPEND = 3, RUC_ABORT = 4 };
enum Cus : uint16_t { CUS_IDLE = 0, CUS_SUSPENDED = 1, CUS_ACTIVE = 2 };
enum Rus : uint16_t { RUS_IDLE = 0, RUS_SUSPENDED = 1, RUS_NO_RESOURCE = 2, RUS_READY = 4 };

// ---- command block --------------------------------------------------------
constexpr uint16_t CB_ST_A = 1u << 12;
constexpr uint16_t CB_ST_OK = 1u << 13;
constexpr uint16_t CB_ST_B = 1u << 14;
constexpr uint16_t CB_ST_C = 1u << 15;

constexpr uint16_t CB_CMD_I = 1u << 13;
constexpr uint16_t CB_CMD_S = 1u << 14;
constexpr uint16_t CB_CMD_EL = 1u << 15;

enum Opcode : uint16_t {
  CMD_NOP = 0,
  CMD_IA_SETUP = 1,
  CMD_CONFIGURE = 2,
  CMD_MC_SETUP = 3,
  CMD_TRANSMIT = 4,
  CMD_TDR = 5,
  CMD_DUMP = 6,
  CMD_DIAGNOSE = 7,
};

// ---- transmit status ------------------------------------------------------
constexpr uint16_t TX_ST_NCOL_MASK = 0x000f;
constexpr uint16_t TX_ST_XCOLL = 1u << 5;
constexpr uint16_t TX_ST_HEART = 1u << 6;
constexpr uint16_t TX_ST_DEFER = 1u << 7;
constexpr uint16_t TX_ST_UNDERRUN = 1u << 8;
constexpr uint16_t TX_ST_NO_CTS = 1u << 9;
constexpr uint16_t TX_ST_NO_CRS = 1u << 10;
constexpr uint16_t TX_ST_LATECOLL = 1u << 11;

// ---- receive frame descriptor status --------------------------------------
constexpr uint16_t RFD_ST_SHORT = 1u << 7;
constexpr uint16_t RFD_ST_NO_EOF = 1u << 6;
constexpr uint16_t RFD_ST_OVERRUN = 1u << 8;
constexpr uint16_t RFD_ST_NO_SPACE = 1u << 9;
constexpr uint16_t RFD_ST_ALIGN = 1u << 10;
constexpr uint16_t RFD_ST_CRC = 1u << 11;
constexpr uint16_t RFD_ST_OK = 1u << 13;
constexpr uint16_t RFD_ST_B = 1u << 14;
constexpr uint16_t RFD_ST_C = 1u << 15;

constexpr uint16_t RFD_CMD_S = 1u << 14;
constexpr uint16_t RFD_CMD_EL = 1u << 15;

constexpr uint16_t RBD_EOF = 1u << 15;
constexpr uint16_t RBD_F = 1u << 14;
constexpr uint16_t RBD_EL = 1u << 15;
constexpr uint16_t RBD_COUNT_MASK = 0x3fff;

constexpr uint16_t NULL_PTR = 0xffff;

constexpr uint32_t SCP_ADDR_DEFAULT = 0x00fffff6;

// The twelve bytes of the CONFIGURE command; defaults are the ones the Sun
// ROM driver writes (iedefaultconf() in doc/drivers/*/if_ie.c).
struct Config {
  uint8_t byte_count = 12;
  uint8_t fifo_limit = 8;
  bool save_bad_frames = false;
  bool srdy = false;
  bool ext_loopback = false;
  bool int_loopback = false;
  uint8_t preamble_len = 2;   // 2 => 8 bytes
  bool addr_in_buffer = true; // AL-LOC: address and type live in the buffer
  uint8_t addr_len = 6;
  bool linear_backoff = false;
  uint8_t exp_priority = 0;
  uint8_t lin_priority = 0;
  uint8_t ifs = 96;
  uint16_t slot_time = 512;
  uint8_t retry = 15;
  bool pad = false;
  bool bitstuff = false;
  bool crc16 = false;
  bool no_crc_insert = false;
  bool no_carrier_ok = false;
  bool manchester = false;
  bool no_broadcast = false;
  bool promiscuous = false;
  uint8_t cdt_src = 0;
  uint8_t cdt_filter = 0;
  uint8_t crs_src = 0;
  uint8_t crs_filter = 0;
  uint8_t min_frame_len = 64;

  Bytes serialise() const;
  static Config parse(const Bytes& b);
};

// A receive frame recovered by walking the receive frame area.
//
// With AL-LOC = 1 - what both the Sun ROM and the NetBSD drivers configure,
// and the default here - the whole frame including its MAC header lands in the
// buffers and the address fields of the descriptor are left alone.  With
// AL-LOC = 0 the chip splits the header out into the descriptor instead.
// Either way dst, src, type_len and data below are the frame as sent.
struct RxFrame {
  uint16_t rfd_off = 0;
  uint16_t status = 0;
  MacAddr dst, src;
  uint16_t type_len = 0;
  Bytes data;             // payload; the FCS is stripped by the MAC
  Bytes raw;              // exactly what the buffers hold
  bool ok() const { return (status & RFD_ST_OK) != 0; }
  bool complete() const { return (status & RFD_ST_C) != 0; }
  std::string str() const;
};

// The shared memory image.  Control blocks are allocated from a bump pointer
// starting at cbbase, so the layout is deterministic and easy to dump.
class MemImage {
 public:
  MemImage(WbMem& mem, uint32_t cbbase, uint32_t scp_addr = SCP_ADDR_DEFAULT);

  // ---- initial structures -------------------------------------------------
  // Writes the SCP, the ISCP (busy = 1) and a cleared SCB.
  void build_init_structures(bool bus16 = true);

  uint32_t scp_addr() const { return scp_addr_; }
  uint32_t iscp_addr() const { return iscp_addr_; }
  uint32_t cbbase() const { return cbbase_; }
  uint16_t scb_off() const { return scb_off_; }
  uint32_t scb_addr() const { return cbbase_ + scb_off_; }
  uint32_t addr_of(uint16_t off) const { return cbbase_ + off; }

  uint8_t iscp_busy() const { return mem_.rd8(iscp_addr_ + 0); }

  // ---- SCB ----------------------------------------------------------------
  uint16_t scb_status() const { return mem_.rd16(scb_addr() + 0); }
  uint16_t scb_cmd() const { return mem_.rd16(scb_addr() + 2); }
  void set_scb_cmd(uint16_t v) { mem_.wr16(scb_addr() + 2, v); }
  uint16_t scb_cbl() const { return mem_.rd16(scb_addr() + 4); }
  void set_scb_cbl(uint16_t v) { mem_.wr16(scb_addr() + 4, v); }
  uint16_t scb_rfa() const { return mem_.rd16(scb_addr() + 6); }
  void set_scb_rfa(uint16_t v) { mem_.wr16(scb_addr() + 6, v); }
  uint16_t scb_crc_errs() const { return mem_.rd16(scb_addr() + 8); }
  uint16_t scb_aln_errs() const { return mem_.rd16(scb_addr() + 10); }
  uint16_t scb_rsc_errs() const { return mem_.rd16(scb_addr() + 12); }
  uint16_t scb_ovrn_errs() const { return mem_.rd16(scb_addr() + 14); }
  uint16_t scb_cus() const { return (scb_status() >> SCB_ST_CUS_LSB) & 7; }
  uint16_t scb_rus() const { return (scb_status() >> SCB_ST_RUS_LSB) & 7; }

  // ---- command blocks -----------------------------------------------------
  uint16_t alloc(size_t bytes, size_t align = 2);

  uint16_t add_nop(uint16_t flags = CB_CMD_EL | CB_CMD_I);
  uint16_t add_ia_setup(const MacAddr& mac, uint16_t flags = CB_CMD_EL | CB_CMD_I);
  uint16_t add_configure(const Config& cfg, uint16_t flags = CB_CMD_EL | CB_CMD_I);
  uint16_t add_mc_setup(const std::vector<MacAddr>& list,
                        uint16_t flags = CB_CMD_EL | CB_CMD_I);
  // Transmit command with a single buffer.  With Config::addr_in_buffer the
  // whole frame including addresses goes into the buffer; otherwise the
  // destination address and type come from the command block.
  uint16_t add_transmit(const EthFrame& f, bool addr_in_buffer = true,
                        uint16_t flags = CB_CMD_EL | CB_CMD_I);
  // Transmit command whose data is split over several buffers, to exercise
  // TBD chaining.
  uint16_t add_transmit_scattered(const EthFrame& f, const std::vector<size_t>& chunks,
                                  bool addr_in_buffer = true,
                                  uint16_t flags = CB_CMD_EL | CB_CMD_I);

  void link_cb(uint16_t cb, uint16_t next);
  uint16_t cb_status(uint16_t cb) const { return mem_.rd16(addr_of(cb) + 0); }
  uint16_t cb_cmd(uint16_t cb) const { return mem_.rd16(addr_of(cb) + 2); }
  uint16_t cb_link(uint16_t cb) const { return mem_.rd16(addr_of(cb) + 4); }

  // ---- receive frame area -------------------------------------------------
  // Builds a ring of n_rfd descriptors, each RFD followed by n_rbd buffer
  // descriptors of buf_size bytes.  Returns the offset of the first RFD.
  uint16_t build_rfa(int n_rfd, int n_rbd, size_t buf_size);
  uint16_t rfa() const { return rfa_off_; }
  const std::vector<uint16_t>& rfds() const { return rfd_offs_; }

  // Walks the receive frame area and returns every completed frame.
  // addr_in_buffer must match the AL-LOC bit the chip was configured with.
  std::vector<RxFrame> collect_rx(bool addr_in_buffer = true);
  // Marks the frames returned by collect_rx() as free again and relinks them
  // at the end of the list, the way a driver recycles descriptors.
  void recycle_rx();

  std::string dump_scb() const;
  std::string dump_cb(uint16_t cb) const;

  WbMem& mem() { return mem_; }

 private:
  uint16_t new_cb(Opcode op, uint16_t flags, size_t extra_bytes);
  uint32_t alloc_buffer(size_t bytes);

  WbMem& mem_;
  uint32_t scp_addr_;
  uint32_t iscp_addr_ = 0;
  uint32_t cbbase_;
  uint16_t scb_off_ = 0;
  uint16_t heap_ = 0;        // next free control block offset
  uint32_t buf_heap_ = 0;    // next free data buffer address
  uint16_t rfa_off_ = NULL_PTR;
  std::vector<uint16_t> rfd_offs_;
  std::vector<uint16_t> collected_;
};

}  // namespace ie
}  // namespace wtb
