/* SPDX-License-Identifier: MIT
 *
 * Enough of NetBSD's <sys/device.h> for doc/drivers/NetBSD/mii_bitbang.c,
 * which only ever passes a device_t straight back to the callbacks in its ops
 * struct and never looks inside one.  In the kernel it is a pointer to the
 * device's autoconf node; here it points at the NetBsdStation driving the pins.
 */
#pragma once

typedef void *device_t;
