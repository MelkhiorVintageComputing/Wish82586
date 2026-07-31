# Documentation

Content:

* interface.md: register map, Wishbone and MII contract between the RTL and
  the testbench
* verification.md: how the testbench is built, how to run it and how to add
  tests
* drivers: source files from existing drivers
 - drivers/Linux: Linux's clause 22 definitions and Realtek PHY driver, the
   reference for the MDIO side.  See drivers/Linux/README.md.
 - drivers/NetBSD: NetBSD's 82586 support, for both a little-endian ISA card
   and big-endian Suns.  This is the reference for the shared memory layout;
   see drivers/NetBSD/README.md for why.
 - drivers/Sun3280_ROM: from the ROM of a Sun 3/280
 - drivers/Sun2120_ROM: from the ROM of a Sun 2/120

