/* SPDX-License-Identifier: MIT
 *
 * The C interface between the emulated 82586 card in QEMU and the Verilated
 * wish82586.
 *
 * The card knows nothing about Verilator: it loads this library at run time
 * and calls it for the four things its host interface can do - reset the chip,
 * poke channel attention, hand it a frame off the wire, and ask whether the
 * interrupt is asserted.  That is the same interface the software 82586 model
 * in the same device implements, which is what makes them interchangeable.
 *
 * The window of shared memory is not copied anywhere: the pointer the card
 * passes to wish_rtl_new() is what the core's Wishbone master reads and writes,
 * so the guest and the RTL really are looking at one buffer.
 *
 * Nothing here is re-entrant and nothing runs in the background.  A call that
 * gives the core work to do runs the simulation until the core has finished
 * it, so by the time it returns the shared memory is up to date - which is
 * what a driver that polls the SCB command word needs.
 */

#ifndef WISH_RTL_H
#define WISH_RTL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WISH_RTL_ABI 1

typedef struct WishRtl WishRtl;

/* Bumped whenever anything below changes shape.  The caller checks it. */
uint32_t wish_rtl_abi(void);

/* mem is the card's memory window; mem_base is the chip address it appears at,
 * which for every 82586 ISA board is 2^24 - mem_size. */
WishRtl *wish_rtl_new(uint8_t *mem, uint32_t mem_size, uint32_t mem_base);
void wish_rtl_free(WishRtl *r);

/* Hardware reset: hold the core in reset, release it, and leave it waiting for
 * the first channel attention. */
void wish_rtl_reset(WishRtl *r);

/* Channel attention, then run until the core is idle again. */
void wish_rtl_ca(WishRtl *r);

/* The core's interrupt output, as it stands now. */
int wish_rtl_irq(WishRtl *r);

/* Whether the receive unit is ready to take a frame. */
int wish_rtl_ru_ready(WishRtl *r);

/* Present a frame to the PHY - no FCS, this adds one - and run until the core
 * has finished with it. */
void wish_rtl_rx(WishRtl *r, const uint8_t *frame, uint32_t len);

/* Take one frame the MAC transmitted, FCS stripped.  Returns its length, 0 if
 * there is nothing waiting, or -1 if buf is too small for the next one. */
int wish_rtl_tx_pop(WishRtl *r, uint8_t *buf, uint32_t max);

/* Simulated time the core has run for, in nanoseconds.  For diagnostics. */
uint64_t wish_rtl_time_ns(WishRtl *r);

#ifdef __cplusplus
}
#endif

#endif /* WISH_RTL_H */
