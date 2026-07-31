// SPDX-License-Identifier: MIT

#include "eth.h"

#include <cstdio>
#include <sstream>

namespace wtb {

namespace {
uint32_t crc32_update(uint32_t crc, uint8_t byte) {
  crc ^= byte;
  for (int i = 0; i < 8; i++)
    crc = (crc & 1) ? ((crc >> 1) ^ 0xedb88320u) : (crc >> 1);
  return crc;
}
}  // namespace

uint32_t eth_fcs(const uint8_t* data, size_t len) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < len; i++) crc = crc32_update(crc, data[i]);
  return ~crc;
}

uint32_t eth_fcs(const Bytes& data) { return eth_fcs(data.data(), data.size()); }

bool eth_fcs_valid(const Bytes& data) {
  if (data.size() < 4) return false;
  uint32_t crc = 0xffffffffu;
  for (uint8_t b : data) crc = crc32_update(crc, b);
  return crc == 0xdebb20e3u;  // residue after a correct frame + FCS
}

bool MacAddr::is_broadcast() const {
  for (uint8_t x : b)
    if (x != 0xff) return false;
  return true;
}

std::string MacAddr::str() const {
  char buf[24];
  snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2],
           b[3], b[4], b[5]);
  return buf;
}

Bytes EthFrame::to_wire(bool with_fcs, bool pad_to_min) const {
  Bytes w;
  w.reserve(payload.size() + 18);
  w.insert(w.end(), dst.b.begin(), dst.b.end());
  w.insert(w.end(), src.b.begin(), src.b.end());
  w.push_back(uint8_t(type_len >> 8));  // the type/length field is big endian
  w.push_back(uint8_t(type_len & 0xff));
  w.insert(w.end(), payload.begin(), payload.end());
  if (pad_to_min)
    while (w.size() < ETH_MIN_FRAME) w.push_back(0);
  if (with_fcs) {
    uint32_t fcs = eth_fcs(w);
    for (int i = 0; i < 4; i++) w.push_back(uint8_t((fcs >> (8 * i)) & 0xff));
  }
  return w;
}

EthFrame EthFrame::from_wire(const Bytes& wire, bool has_fcs) {
  EthFrame f;
  size_t len = wire.size();
  if (has_fcs && len >= 4) len -= 4;
  if (len >= 6) f.dst = MacAddr(wire.data());
  if (len >= 12) f.src = MacAddr(wire.data() + 6);
  if (len >= 14) f.type_len = uint16_t((wire[12] << 8) | wire[13]);
  if (len > 14) f.payload.assign(wire.begin() + 14, wire.begin() + long(len));
  return f;
}

std::string EthFrame::str() const {
  std::ostringstream os;
  os << dst.str() << " <- " << src.str() << " type=0x" << std::hex << type_len
     << std::dec << " payload=" << payload.size() << "B";
  return os.str();
}

std::string WireFrame::str() const {
  std::ostringstream os;
  os << data.size() << "B fcs=" << (fcs_ok ? "ok" : "BAD");
  if (dribble) os << " dribble";
  if (rx_er) os << " rx_er";
  if (!preamble_ok) os << " preamble=" << preamble_nibbles << "nib";
  return os.str();
}

std::string hex_dump(const Bytes& b, size_t max_bytes) {
  std::ostringstream os;
  size_t n = b.size() < max_bytes ? b.size() : max_bytes;
  char buf[8];
  for (size_t i = 0; i < n; i++) {
    snprintf(buf, sizeof(buf), "%02x", b[i]);
    if (i && (i % 16) == 0) os << "\n      ";
    else if (i) os << ' ';
    os << buf;
  }
  if (n < b.size()) os << " ... (" << b.size() << " bytes total)";
  return os.str();
}

Bytes random_payload(size_t len, uint32_t seed) {
  Bytes out(len);
  uint32_t x = seed ? seed : 0x12345678u;
  for (size_t i = 0; i < len; i++) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    out[i] = uint8_t(x >> 24);
  }
  return out;
}

}  // namespace wtb
