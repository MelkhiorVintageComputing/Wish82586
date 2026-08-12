/* SPDX-License-Identifier: MIT
 *
 * Enough of NetBSD's <sys/param.h> for doc/drivers/NetBSD/mii_bitbang.c: the
 * fixed width integer types, and delay(), which is the kernel's spin for a
 * number of microseconds.
 *
 * delay() is what paces the bus, so the testbench implements it by advancing
 * simulated time - see tb/cpp/netbsd_station.cpp.  That is the one place where
 * this file is more than a typedef, and it is what lets the driver's own
 * timing, rather than a rewrite of it, drive the model.
 */
#pragma once

#include <stdint.h>

void delay(unsigned int usec);
