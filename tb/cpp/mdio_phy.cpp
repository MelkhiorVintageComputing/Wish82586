// SPDX-License-Identifier: MIT

#include "mdio_phy.h"

#include "mii.h"

namespace wtb {

namespace {
constexpr uint8_t OP_READ = mii::OP_READ;
constexpr uint8_t OP_WRITE = mii::OP_WRITE;
constexpr uint8_t MII_BMCR = mii::REG_CONTROL;
constexpr uint16_t BMCR_RESET = mii::CTRL_RESET;
}  // namespace

MdioPhy::MdioPhy(Sim& sim, Sim::Clock* sample_clk, MdioPorts ports, uint8_t addr)
    : sim_(sim), p_(ports), addr_(addr) {
  *p_.mdio_i = 1;   // the bus idles high, pulled up
  sim_.on_negedge(sample_clk, [this]() { tick(); });
}

void MdioPhy::tick() {
  const bool mdc = *p_.mdc != 0;

  // What the wire actually carries: the station when it is driving, this
  // model when it is, and the pull-up otherwise.
  const bool station_drives = *p_.mdio_oe != 0;
  const bool bus = station_drives ? (*p_.mdio_o != 0)
                                  : (driving_ ? ((out_ >> out_bit_) & 1) : true);

  if (mdc && !prev_mdc_) on_rising(bus);
  else if (!mdc && prev_mdc_) on_falling();

  // Present whatever we are driving, so the station sees it.
  *p_.mdio_i = (!station_drives && driving_) ? uint8_t((out_ >> out_bit_) & 1)
                                             : 1;
  prev_mdc_ = mdc;
}

void MdioPhy::on_falling() {
  // Let go only once the last data bit has had its whole slot; releasing on
  // the rising edge that ends the frame would hand the station the pull-up
  // instead of that bit.
  if (driving_ && st_ == St::Preamble) driving_ = false;
}

void MdioPhy::on_rising(bool bit) {
  // The reply steps once per bit time for as long as we are driving, including
  // the step out of the turnaround.
  //
  // A read frame is 64 bit times and bit time N ends at rising edge N, so the
  // data is bit times 49 to 64 and the zero the PHY owes on the turnaround is
  // bit time 48.  A PHY drives between 0 and 300 ns after a rising edge, which
  // means it puts bit time N on the wire just after edge N-1: the turnaround
  // zero just after edge 47, and the first data bit just after edge 48.  So the
  // step has to happen on the second turnaround edge as well as on the data
  // ones, which is why there is no state test here.
  //
  // Holding the zero for the whole turnaround instead - the obvious reading,
  // and what this did first - puts every data bit one bit time late.  Our own
  // station read it anyway, because it was sampling a bit time late to match;
  // NetBSD's would not, which is how it was found.
  if (driving_ && out_bit_ > 0) out_bit_--;

  switch (st_) {
    case St::Preamble:
      if (bit) {
        ones_++;
      } else {
        // The first zero ends the preamble and is the first bit of the start
        // delimiter, which is 01.
        if (ones_ < mii::PREAMBLE_BITS) short_preamble_ = true;
        ones_ = 0;
        st_ = St::St1;
      }
      break;

    case St::St1:
      st_ = bit ? St::Op : St::Preamble;   // 01, anything else is noise
      count_ = 0;
      op_ = 0;
      break;

    case St::Op:
      op_ = uint8_t((op_ << 1) | (bit ? 1 : 0));
      if (++count_ == 2) {   // two opcode bits
        count_ = 0;
        phyad_ = 0;
        st_ = St::PhyAd;
      }
      break;

    case St::PhyAd:
      phyad_ = uint8_t((phyad_ << 1) | (bit ? 1 : 0));
      if (++count_ == mii::ADDR_BITS) {
        count_ = 0;
        regad_ = 0;
        st_ = St::RegAd;
      }
      break;

    case St::RegAd:
      regad_ = uint8_t((regad_ << 1) | (bit ? 1 : 0));
      if (++count_ == mii::ADDR_BITS) {
        count_ = 0;
        for_us_ = (phyad_ == addr_);
        st_ = St::Ta;
        if (for_us_ && op_ == OP_READ) {
          // Answer during the turnaround: the reply starts with the zero the
          // PHY owes on the second turnaround bit.
          out_ = regs_[regad_ & 0x1f];
          reads_++;
          if (regad_ == MII_BMCR && (out_ & BMCR_RESET)) {
            // A real PHY clears the reset bit once it has finished.  Doing it
            // after a configurable number of reads is what lets a test drive
            // the polling loop rather than skipping straight past it.
            if (reset_seen_ >= reset_reads_) regs_[MII_BMCR] &= uint16_t(~BMCR_RESET);
            else reset_seen_++;
          }
        }
      }
      break;

    case St::Ta:
      if (++count_ == 1 && for_us_ && op_ == OP_READ) {
        // Start driving.  out_ holds the register value, so bit 16 is a zero:
        // that is the turnaround bit the PHY owes, and the sixteen data bits
        // follow it most significant first.  It must not be shifted up - out_
        // is sixteen bits of data in a wider word, and shifting would push the
        // top bit off the end and put everything on the wire one place out.
        driving_ = true;
        out_bit_ = mii::DATA_BITS;
      }
      if (count_ == 2) {
        count_ = 0;
        data_ = 0;
        st_ = St::Data;
      }
      break;

    case St::Data:
      if (op_ == OP_WRITE) data_ = uint16_t((data_ << 1) | (bit ? 1 : 0));
      if (++count_ == mii::DATA_BITS) {
        if (for_us_ && op_ == OP_WRITE) {
          regs_[regad_ & 0x1f] = data_;
          writes_.push_back(Write{sim_.time_ps(), phyad_, regad_, data_});
          if (regad_ == MII_BMCR && (data_ & BMCR_RESET)) reset_seen_ = 0;
        }
        count_ = 0;
        ones_ = 0;
        st_ = St::Preamble;   // on_falling lets go of the bus
      }
      break;
  }
}

}  // namespace wtb
