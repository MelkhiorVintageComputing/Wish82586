# Linux PHY reference

Unmodified copies from `https://raw.githubusercontent.com/torvalds/linux/master`,
kept as reference material under their original licences.

| file              | Linux path                             |
|-------------------|----------------------------------------|
| `realtek_main.c`  | `drivers/net/phy/realtek/realtek_main.c` |
| `mii.h`           | `include/uapi/linux/mii.h`             |

`mii.h` is where the clause 22 register numbers and bit positions in
`src/mdio_prog.sv` come from - BMCR, the advertisement register and the
1000BASE-T control register.

`realtek_main.c` is here to answer what an RTL8211EG needs beyond the
standard.  The answer is: for a GMII interface, nothing.  Its
`rtl8211e_config_init()` sets the RGMII transmit and receive delays on
extension page 0xa4 and returns without touching anything for every other
interface mode:

```c
default: /* the rest of the modes imply leaving delays as is. */
        return 0;
```

So `mdio_prog` sticks to clause 22, which is the honest implementation rather
than invented chip-specific magic.  If an RGMII variant is ever wanted, page
0xa4 register 0x1c is where it goes, reached through the page select at
register 0x1f.
