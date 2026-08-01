// SPDX-License-Identifier: MIT
//
// The Sun-2 on-board Ethernet control register.
//
// wb_csr_sun2 is what a recreated Sun-2 puts in front of the MAC's host pins
// instead of wb_csr, so what these check is the register the ROM driver in
// doc/drivers/Sun2120_ROM expects: the bit positions and the active-low reset
// out of doc/sun2_ethernet.pdf and if_obie.h, and the two behaviours that are
// not obvious from either - channel attention is an edge, and the bus error
// latch can only be cleared by asserting reset.

#include "test.h"

#include "Vtb_top.h"

namespace wtb {

namespace {

// The register byte, as the two references number it.
constexpr uint32_t INT = 1u << 0;      // ro
constexpr uint32_t ERR = 1u << 1;      // ro
constexpr uint32_t INTEN = 1u << 4;
constexpr uint32_t CA = 1u << 5;
constexpr uint32_t NOLOOP = 1u << 6;   // LOOPB*
constexpr uint32_t NORESET = 1u << 7;  // RESET*

uint32_t reg(Env& env) { return env.sun2_host().read32(0) & 0xff; }
void set(Env& env, uint32_t v) { env.sun2_host().write32(0, v, 0x1); }

// Counts channel attention pulses on the pin for the rest of the test.
struct CaCounter {
  explicit CaCounter(Env& env) : env_(env) {
    env.sim().on_negedge(env.sysclk(), [this]() {
      if (env_.dut()->sun2_ca_o) n++;
    });
  }
  Env& env_;
  int n = 0;
};

}  // namespace

// "Initialization: cleared on all resets" - so the chip comes up held in reset
// and in loopback, and the driver has to let it out.  That is what
// `*obie = obie_reset' followed by `obie->obie_noreset = 1' in if_ie.c does.
TEST(unit_sun2_csr_comes_up_holding_the_chip_in_reset) {
  env.power_on_reset();
  CHECK_EQ(reg(env), uint32_t(0));
  CHECK_MSG(env.dut()->sun2_core_rst_o, "the chip was not held in reset");
  CHECK_MSG(env.dut()->sun2_loopback_o, "the interface did not come up in loopback");
  CHECK_MSG(!env.dut()->sun2_irq_o, "an interrupt was asserted out of reset");

  // The 82586 has the SCP address wired in; the Sun-2 has no way to move it.
  CHECK_EQ(env.dut()->sun2_scp_addr_o, uint32_t(0x00fffff6));
}

TEST(unit_sun2_csr_reset_and_loopback_are_active_low) {
  env.power_on_reset();

  set(env, NORESET);
  CHECK_EQ(reg(env), NORESET);
  CHECK_MSG(!env.dut()->sun2_core_rst_o, "the chip is still in reset");
  CHECK_MSG(env.dut()->sun2_loopback_o, "loopback was left when only reset changed");

  // The driver goes on the wire only after the chip is configured.
  set(env, NORESET | NOLOOP);
  CHECK_MSG(!env.dut()->sun2_loopback_o, "the interface is still in loopback");

  set(env, 0);
  CHECK_MSG(env.dut()->sun2_core_rst_o, "writing zeros did not reset the chip");
}

// ieca() writes the bit set and then clear, so the pin sees a level, not a
// pulse.  The 82586 latches channel attention on the rising edge of that pin.
TEST(unit_sun2_csr_channel_attention_is_an_edge) {
  env.power_on_reset();
  CaCounter ca(env);

  set(env, NORESET);
  CHECK_EQ(ca.n, 0);

  set(env, NORESET | CA);
  CHECK_EQ(ca.n, 1);
  CHECK_MSG(reg(env) & CA, "the channel attention bit does not read back");

  // Written set again without being cleared, it is one attention, not two.
  set(env, NORESET | CA);
  CHECK_EQ(ca.n, 1);

  set(env, NORESET);
  CHECK_EQ(ca.n, 1);
  set(env, NORESET | CA);
  CHECK_EQ(ca.n, 2);
}

TEST(unit_sun2_csr_interrupt_enable_masks_the_pin) {
  env.power_on_reset();
  set(env, NORESET);

  env.dut()->sun2_int_i = 1;
  env.tick(2);
  // The register reports what the chip is asking for either way; INTEN only
  // decides whether the CPU hears about it.
  CHECK_MSG(reg(env) & INT, "the interrupt request does not read back");
  CHECK_MSG(!env.dut()->sun2_irq_o, "a masked interrupt reached the pin");

  set(env, NORESET | INTEN);
  env.tick(2);
  CHECK_MSG(env.dut()->sun2_irq_o, "an enabled interrupt did not reach the pin");

  env.dut()->sun2_int_i = 0;
  env.tick(2);
  CHECK_MSG(!env.dut()->sun2_irq_o, "the interrupt outlasted the request");
}

// "Must poll to check for obie_buserr", and the only way to clear it is the
// reset bit: the manual says RESET "also clears the ERR condition when active".
TEST(unit_sun2_csr_latches_a_bus_error_until_reset) {
  env.power_on_reset();
  set(env, NORESET);
  CHECK_MSG(!(reg(env) & ERR), "a bus error was reported out of reset");

  env.dut()->sun2_bus_err_i = 1;
  env.tick(1);
  env.dut()->sun2_bus_err_i = 0;
  env.tick(2);
  CHECK_MSG(reg(env) & ERR, "the bus error was not latched");

  // Reading it, and writing anything that leaves reset alone, must not clear it.
  set(env, NORESET | INTEN);
  CHECK_MSG(reg(env) & ERR, "the bus error latch cleared on a write");

  set(env, 0);
  set(env, NORESET);
  CHECK_MSG(!(reg(env) & ERR), "asserting reset did not clear the bus error");
}

// Bit 2 is a transceiver type the Sun-2 found a use for and bit 3 is unused;
// neither is modelled, and neither may come back set.
TEST(unit_sun2_csr_read_only_bits_stay_clear) {
  env.power_on_reset();
  set(env, 0xff);
  CHECK_EQ(reg(env) & 0x0c, uint32_t(0));
}

}  // namespace wtb
