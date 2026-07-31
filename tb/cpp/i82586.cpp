// SPDX-License-Identifier: MIT

#include "i82586.h"

#include <cstdio>
#include <sstream>

namespace wtb {
namespace ie {

namespace {
constexpr uint16_t CB_HDR = 6;          // status, command, link
constexpr uint16_t SCB_SIZE = 16;
constexpr uint16_t ISCP_SIZE = 8;
constexpr uint16_t RFD_SIZE = 22;
constexpr uint16_t RBD_SIZE = 10;
constexpr uint32_t BUF_AREA_OFF = 0x10000;  // data buffers live above the CBs
}  // namespace

Bytes Config::serialise() const {
  Bytes b(12, 0);
  b[0] = uint8_t(byte_count & 0x0f);
  b[1] = uint8_t(fifo_limit & 0x0f);
  b[2] = uint8_t((save_bad_frames ? 0x80 : 0) | (srdy ? 0x40 : 0));
  b[3] = uint8_t((ext_loopback ? 0x80 : 0) | (int_loopback ? 0x40 : 0) |
                 ((preamble_len & 3) << 4) | (addr_in_buffer ? 0x08 : 0) |
                 (addr_len & 7));
  b[4] = uint8_t((linear_backoff ? 0x80 : 0) | ((exp_priority & 7) << 4) |
                 (lin_priority & 7));
  b[5] = ifs;
  b[6] = uint8_t(slot_time & 0xff);
  b[7] = uint8_t(((retry & 0xf) << 4) | ((slot_time >> 8) & 7));
  b[8] = uint8_t((pad ? 0x80 : 0) | (bitstuff ? 0x40 : 0) | (crc16 ? 0x20 : 0) |
                 (no_crc_insert ? 0x10 : 0) | (no_carrier_ok ? 0x08 : 0) |
                 (manchester ? 0x04 : 0) | (no_broadcast ? 0x02 : 0) |
                 (promiscuous ? 0x01 : 0));
  b[9] = uint8_t(((cdt_src & 1) << 7) | ((cdt_filter & 7) << 4) |
                 ((crs_src & 1) << 3) | (crs_filter & 7));
  b[10] = min_frame_len;
  b[11] = 0;
  return b;
}

Config Config::parse(const Bytes& b) {
  Config c;
  if (b.size() < 12) return c;
  c.byte_count = uint8_t(b[0] & 0x0f);
  c.fifo_limit = uint8_t(b[1] & 0x0f);
  c.save_bad_frames = (b[2] & 0x80) != 0;
  c.srdy = (b[2] & 0x40) != 0;
  c.ext_loopback = (b[3] & 0x80) != 0;
  c.int_loopback = (b[3] & 0x40) != 0;
  c.preamble_len = uint8_t((b[3] >> 4) & 3);
  c.addr_in_buffer = (b[3] & 0x08) != 0;
  c.addr_len = uint8_t(b[3] & 7);
  c.linear_backoff = (b[4] & 0x80) != 0;
  c.exp_priority = uint8_t((b[4] >> 4) & 7);
  c.lin_priority = uint8_t(b[4] & 7);
  c.ifs = b[5];
  c.slot_time = uint16_t(b[6] | ((b[7] & 7) << 8));
  c.retry = uint8_t((b[7] >> 4) & 0xf);
  c.pad = (b[8] & 0x80) != 0;
  c.bitstuff = (b[8] & 0x40) != 0;
  c.crc16 = (b[8] & 0x20) != 0;
  c.no_crc_insert = (b[8] & 0x10) != 0;
  c.no_carrier_ok = (b[8] & 0x08) != 0;
  c.manchester = (b[8] & 0x04) != 0;
  c.no_broadcast = (b[8] & 0x02) != 0;
  c.promiscuous = (b[8] & 0x01) != 0;
  c.cdt_src = uint8_t((b[9] >> 7) & 1);
  c.cdt_filter = uint8_t((b[9] >> 4) & 7);
  c.crs_src = uint8_t((b[9] >> 3) & 1);
  c.crs_filter = uint8_t(b[9] & 7);
  c.min_frame_len = b[10];
  return c;
}

std::string RxFrame::str() const {
  std::ostringstream os;
  os << "rfd@0x" << std::hex << rfd_off << " status=0x" << status << std::dec
     << " " << dst.str() << " <- " << src.str() << " type=0x" << std::hex
     << type_len << std::dec << " " << data.size() << "B";
  return os.str();
}

// ---------------------------------------------------------------------------

MemImage::MemImage(WbMem& mem, uint32_t cbbase, uint32_t scp_addr)
    : mem_(mem), scp_addr_(scp_addr), cbbase_(cbbase) {
  iscp_addr_ = cbbase_;
  scb_off_ = ISCP_SIZE + 8;              // ISCP, then the SCB, both aligned
  heap_ = uint16_t(scb_off_ + SCB_SIZE);
  buf_heap_ = cbbase_ + BUF_AREA_OFF;
}

void MemImage::build_init_structures(bool bus16) {
  // SCP: bus width byte then the 24-bit ISCP address at offset 6.
  for (int i = 0; i < 10; i++) mem_.wr8(scp_addr_ + uint32_t(i), 0);
  mem_.wr8(scp_addr_ + 0, bus16 ? 0 : 1);
  mem_.wr24(scp_addr_ + 6, iscp_addr_);

  // ISCP: busy, SCB offset, control block base.
  mem_.wr8(iscp_addr_ + 0, 1);
  mem_.wr8(iscp_addr_ + 1, 0);
  mem_.wr16(iscp_addr_ + 2, scb_off_);
  mem_.wr24(iscp_addr_ + 4, cbbase_);
  mem_.wr8(iscp_addr_ + 7, 0);

  for (uint32_t i = 0; i < SCB_SIZE; i++) mem_.wr8(scb_addr() + i, 0);
  set_scb_cbl(NULL_PTR);
  set_scb_rfa(NULL_PTR);
}

uint16_t MemImage::alloc(size_t bytes, size_t align) {
  uint16_t off = heap_;
  if (align > 1 && (off % align) != 0) off = uint16_t(off + align - (off % align));
  heap_ = uint16_t(off + bytes);
  return off;
}

uint32_t MemImage::alloc_buffer(size_t bytes) {
  uint32_t a = buf_heap_;
  buf_heap_ = uint32_t((buf_heap_ + bytes + 3) & ~uint32_t(3));
  return a;
}

uint16_t MemImage::new_cb(Opcode op, uint16_t flags, size_t extra_bytes) {
  uint16_t off = alloc(CB_HDR + extra_bytes);
  uint32_t a = addr_of(off);
  for (size_t i = 0; i < CB_HDR + extra_bytes; i++) mem_.wr8(uint32_t(a + i), 0);
  mem_.wr16(a + 0, 0);                      // status
  mem_.wr16(a + 2, uint16_t(flags | op));   // command
  mem_.wr16(a + 4, NULL_PTR);               // link
  return off;
}

void MemImage::link_cb(uint16_t cb, uint16_t next) {
  mem_.wr16(addr_of(cb) + 4, next);
  if (next != NULL_PTR) {
    uint16_t cmd = mem_.rd16(addr_of(cb) + 2);
    mem_.wr16(addr_of(cb) + 2, uint16_t(cmd & ~CB_CMD_EL));
  }
}

uint16_t MemImage::add_nop(uint16_t flags) { return new_cb(CMD_NOP, flags, 0); }

uint16_t MemImage::add_ia_setup(const MacAddr& mac, uint16_t flags) {
  uint16_t off = new_cb(CMD_IA_SETUP, flags, 6);
  for (int i = 0; i < 6; i++) mem_.wr8(addr_of(off) + CB_HDR + uint32_t(i), mac.b[i]);
  return off;
}

uint16_t MemImage::add_configure(const Config& cfg, uint16_t flags) {
  Bytes b = cfg.serialise();
  uint16_t off = new_cb(CMD_CONFIGURE, flags, b.size());
  mem_.write_block(addr_of(off) + CB_HDR, b);
  return off;
}

uint16_t MemImage::add_mc_setup(const std::vector<MacAddr>& list, uint16_t flags) {
  uint16_t off = new_cb(CMD_MC_SETUP, flags, 2 + list.size() * 6);
  mem_.wr16(addr_of(off) + CB_HDR, uint16_t(list.size() * 6));  // MC-CNT in bytes
  uint32_t a = addr_of(off) + CB_HDR + 2;
  for (const MacAddr& m : list) {
    for (int i = 0; i < 6; i++) mem_.wr8(a + uint32_t(i), m.b[i]);
    a += 6;
  }
  return off;
}

uint16_t MemImage::add_transmit(const EthFrame& f, bool addr_in_buffer,
                                uint16_t flags) {
  Bytes wire = f.to_wire(false, false);          // no FCS, no padding
  Bytes buf = addr_in_buffer ? wire : f.payload;
  return add_transmit_scattered(f, {buf.size()}, addr_in_buffer, flags);
}

uint16_t MemImage::add_transmit_scattered(const EthFrame& f,
                                          const std::vector<size_t>& chunks,
                                          bool addr_in_buffer, uint16_t flags) {
  // The transmit command block: header, TBD pointer, destination, type.
  uint16_t off = new_cb(CMD_TRANSMIT, flags, 10);
  Bytes data = addr_in_buffer ? f.to_wire(false, false) : f.payload;

  if (!addr_in_buffer) {
    for (int i = 0; i < 6; i++)
      mem_.wr8(addr_of(off) + 8 + uint32_t(i), f.dst.b[i]);
    // The type/length field goes on the wire big endian.
    mem_.wr8(addr_of(off) + 14, uint8_t(f.type_len >> 8));
    mem_.wr8(addr_of(off) + 15, uint8_t(f.type_len & 0xff));
  } else {
    for (int i = 0; i < 8; i++) mem_.wr8(addr_of(off) + 8 + uint32_t(i), 0);
  }

  // Split the data over the requested buffers and chain the descriptors.
  std::vector<size_t> sizes;
  size_t total = 0;
  for (size_t c : chunks) {
    size_t n = (total + c <= data.size()) ? c : (data.size() - total);
    sizes.push_back(n);
    total += n;
    if (total >= data.size()) break;
  }
  if (total < data.size()) sizes.push_back(data.size() - total);
  if (sizes.empty()) sizes.push_back(0);

  uint16_t first_tbd = NULL_PTR, prev_tbd = NULL_PTR;
  size_t pos = 0;
  for (size_t i = 0; i < sizes.size(); i++) {
    uint16_t tbd = alloc(8);
    uint32_t ta = addr_of(tbd);
    const bool last = (i + 1 == sizes.size());
    uint32_t buf = alloc_buffer(sizes[i] ? sizes[i] : 1);
    mem_.write_block(buf, Bytes(data.begin() + long(pos),
                                data.begin() + long(pos + sizes[i])));
    pos += sizes[i];

    mem_.wr16(ta + 0, uint16_t((sizes[i] & 0x3fff) | (last ? RBD_EOF : 0)));
    mem_.wr16(ta + 2, NULL_PTR);
    mem_.wr24(ta + 4, buf);
    mem_.wr8(ta + 7, 0);

    if (first_tbd == NULL_PTR) first_tbd = tbd;
    if (prev_tbd != NULL_PTR) mem_.wr16(addr_of(prev_tbd) + 2, tbd);
    prev_tbd = tbd;
  }

  mem_.wr16(addr_of(off) + 6, first_tbd);
  return off;
}

uint16_t MemImage::build_rfa(int n_rfd, int n_rbd, size_t buf_size) {
  rfd_offs_.clear();
  std::vector<uint16_t> rbds;

  for (int i = 0; i < n_rfd; i++) {
    uint16_t rfd = alloc(RFD_SIZE);
    uint32_t a = addr_of(rfd);
    for (uint32_t k = 0; k < RFD_SIZE; k++) mem_.wr8(a + k, 0);
    mem_.wr16(a + 0, 0);                 // status
    mem_.wr16(a + 2, 0);                 // command
    mem_.wr16(a + 4, NULL_PTR);          // link, fixed up below
    mem_.wr16(a + 6, NULL_PTR);          // RBD pointer, only the first one has it
    rfd_offs_.push_back(rfd);
  }

  for (int i = 0; i < n_rbd; i++) {
    uint16_t rbd = alloc(RBD_SIZE);
    uint32_t a = addr_of(rbd);
    uint32_t buf = alloc_buffer(buf_size);
    mem_.wr16(a + 0, 0);                                    // count
    mem_.wr16(a + 2, NULL_PTR);                             // next
    mem_.wr24(a + 4, buf);
    mem_.wr8(a + 7, 0);
    mem_.wr16(a + 8, uint16_t(buf_size & 0x3fff));          // size, EL fixed below
    rbds.push_back(rbd);
  }

  for (size_t i = 0; i < rbds.size(); i++) {
    uint32_t a = addr_of(rbds[i]);
    if (i + 1 < rbds.size()) {
      mem_.wr16(a + 2, rbds[i + 1]);
    } else {
      mem_.wr16(a + 2, rbds[0]);                            // ring back to the start
      mem_.wr16(a + 8, uint16_t((buf_size & 0x3fff) | RBD_EL));
    }
  }

  for (size_t i = 0; i < rfd_offs_.size(); i++) {
    uint32_t a = addr_of(rfd_offs_[i]);
    const bool last = (i + 1 == rfd_offs_.size());
    mem_.wr16(a + 4, last ? rfd_offs_[0] : rfd_offs_[i + 1]);
    mem_.wr16(a + 2, last ? RFD_CMD_EL : 0);
  }
  if (!rbds.empty() && !rfd_offs_.empty())
    mem_.wr16(addr_of(rfd_offs_[0]) + 6, rbds[0]);

  rfa_off_ = rfd_offs_.empty() ? NULL_PTR : rfd_offs_[0];
  set_scb_rfa(rfa_off_);
  return rfa_off_;
}

std::vector<RxFrame> MemImage::collect_rx(bool addr_in_buffer) {
  std::vector<RxFrame> out;
  collected_.clear();
  for (uint16_t rfd : rfd_offs_) {
    uint32_t a = addr_of(rfd);
    uint16_t st = mem_.rd16(a + 0);
    if (!(st & RFD_ST_C)) continue;

    RxFrame f;
    f.rfd_off = rfd;
    f.status = st;
    if (!addr_in_buffer) {
      f.dst = MacAddr(mem_.read_block(a + 8, 6).data());
      f.src = MacAddr(mem_.read_block(a + 14, 6).data());
      Bytes t = mem_.read_block(a + 20, 2);
      f.type_len = uint16_t((t[0] << 8) | t[1]);
    }

    // Follow the buffer descriptor chain until the end of frame marker.
    uint16_t rbd = mem_.rd16(a + 6);
    int guard = 0;
    while (rbd != NULL_PTR && guard++ < 256) {
      uint32_t ra = addr_of(rbd);
      uint16_t cnt = mem_.rd16(ra + 0);
      uint32_t buf = mem_.rd24(ra + 4);
      Bytes chunk = mem_.read_block(buf, cnt & RBD_COUNT_MASK);
      f.raw.insert(f.raw.end(), chunk.begin(), chunk.end());
      if (cnt & RBD_EOF) break;
      if (!(cnt & RBD_F)) break;  // the chip has not filled this one yet
      rbd = mem_.rd16(ra + 2);
    }

    if (addr_in_buffer) {
      // The header is at the front of the buffer contents.
      if (f.raw.size() >= 6) f.dst = MacAddr(f.raw.data());
      if (f.raw.size() >= 12) f.src = MacAddr(f.raw.data() + 6);
      if (f.raw.size() >= 14)
        f.type_len = uint16_t((f.raw[12] << 8) | f.raw[13]);
      if (f.raw.size() > 14) f.data.assign(f.raw.begin() + 14, f.raw.end());
    } else {
      f.data = f.raw;
    }
    out.push_back(f);
    collected_.push_back(rfd);
  }
  return out;
}

void MemImage::recycle_rx() {
  for (uint16_t rfd : collected_) {
    uint32_t a = addr_of(rfd);
    mem_.wr16(a + 0, 0);
  }
  collected_.clear();
}

std::string MemImage::dump_scb() const {
  char buf[160];
  snprintf(buf, sizeof(buf),
           "SCB@0x%06x status=0x%04x cmd=0x%04x cbl=0x%04x rfa=0x%04x "
           "crc=%u aln=%u rsc=%u ovrn=%u",
           scb_addr(), scb_status(), scb_cmd(), scb_cbl(), scb_rfa(),
           scb_crc_errs(), scb_aln_errs(), scb_rsc_errs(), scb_ovrn_errs());
  return buf;
}

std::string MemImage::dump_cb(uint16_t cb) const {
  char buf[128];
  snprintf(buf, sizeof(buf), "CB@0x%04x status=0x%04x cmd=0x%04x link=0x%04x", cb,
           cb_status(cb), cb_cmd(cb), cb_link(cb));
  return buf;
}

}  // namespace ie
}  // namespace wtb
