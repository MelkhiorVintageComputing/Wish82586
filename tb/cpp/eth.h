// SPDX-License-Identifier: MIT
//
// Ethernet frame helpers: MAC addresses, frames and the FCS.
//
// Frames are kept as raw wire bytes (destination address first, FCS last when
// present) so that malformed frames - runts, bad FCS, odd nibble counts - can
// be modelled just as easily as well formed ones.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace wtb {

using Bytes = std::vector<uint8_t>;

// Ethernet FCS over the given bytes: CRC-32 with the reflected polynomial
// 0xEDB88320, preset to ones, final complement.  The returned value is
// appended to the frame least significant byte first.
uint32_t eth_fcs(const uint8_t* data, size_t len);
uint32_t eth_fcs(const Bytes& data);

// True when data (frame + its four FCS bytes) carries a valid FCS.
bool eth_fcs_valid(const Bytes& data);

struct MacAddr {
  std::array<uint8_t, 6> b{};

  MacAddr() = default;
  MacAddr(uint8_t a0, uint8_t a1, uint8_t a2, uint8_t a3, uint8_t a4, uint8_t a5)
      : b{{a0, a1, a2, a3, a4, a5}} {}
  explicit MacAddr(const uint8_t* p) {
    for (int i = 0; i < 6; i++) b[i] = p[i];
  }

  static MacAddr broadcast() { return MacAddr(0xff, 0xff, 0xff, 0xff, 0xff, 0xff); }
  bool is_broadcast() const;
  bool is_multicast() const { return (b[0] & 1) != 0; }

  bool operator==(const MacAddr& o) const { return b == o.b; }
  bool operator!=(const MacAddr& o) const { return b != o.b; }
  std::string str() const;
};

constexpr size_t ETH_MIN_FRAME = 60;   // without FCS
constexpr size_t ETH_MAX_FRAME = 1514; // without FCS

struct EthFrame {
  MacAddr dst;
  MacAddr src;
  uint16_t type_len = 0;
  Bytes payload;

  EthFrame() = default;
  EthFrame(const MacAddr& d, const MacAddr& s, uint16_t t, Bytes p)
      : dst(d), src(s), type_len(t), payload(std::move(p)) {}

  // Serialise to wire bytes.  pad_to_min inserts zeroes so the frame reaches
  // the 60 byte minimum; with_fcs appends the four FCS bytes.
  Bytes to_wire(bool with_fcs = true, bool pad_to_min = true) const;

  // Parse wire bytes.  has_fcs strips (and does not check) the trailing FCS.
  static EthFrame from_wire(const Bytes& wire, bool has_fcs = true);

  size_t wire_len(bool with_fcs = true, bool pad_to_min = true) const {
    return to_wire(with_fcs, pad_to_min).size();
  }
  std::string str() const;
};

// A frame as seen on the wire by the PHY model, preamble and SFD already
// stripped.  Everything the MAC layer might want to complain about is kept.
struct WireFrame {
  Bytes data;              // destination address .. FCS
  bool fcs_ok = false;
  bool dribble = false;    // an odd number of nibbles was received
  bool rx_er = false;      // the PHY signalled an error during the frame
  bool preamble_ok = false;
  size_t preamble_nibbles = 0;
  uint64_t start_ps = 0;
  uint64_t end_ps = 0;

  size_t len() const { return data.size(); }
  Bytes payload_no_fcs() const {
    return data.size() >= 4 ? Bytes(data.begin(), data.end() - 4) : Bytes();
  }
  std::string str() const;
};

std::string hex_dump(const Bytes& b, size_t max_bytes = 64);

// Deterministic pseudo random payload, so a failing test always reproduces.
Bytes random_payload(size_t len, uint32_t seed);

}  // namespace wtb
