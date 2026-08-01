#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Boot the installed NetBSD with the emulated 82586 card attached and check
# that the unmodified driver finds it, configures it, and passes traffic.
#
# This is the whole point of the co-simulation: the testbench in tb/ checks the
# MAC against a model of what the drivers do, and this checks it against the
# drivers themselves.  What it proves is small and specific - ef(4) attached,
# ARP resolved, ICMP came back - but none of it can be arranged by agreeing
# with ourselves.
#
#   run-cosim.py                 the software 82586 inside QEMU
#   run-cosim.py --card starlan10   the other board (see below)
#   run-cosim.py --keep          leave the disk image writable
#
# The default card is the 3Com 3C507.  The AT&T StarLAN 10 is emulated too and
# is the simpler board, but NetBSD's ai(4) faults the kernel during autoconf
# before it ever gets to the chip, so it cannot be used to test anything.

import argparse
import os
import sys

import pexpect

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
WORK = os.environ.get("WORK", os.path.join(ROOT, "work"))
QEMU = os.path.join(WORK, "qemu-install", "bin", "qemu-system-i386")
IMAGE = os.path.join(WORK, "images", "netbsd-10.1-i386.qcow2")

# The slirp network QEMU puts behind -netdev user.
GUEST_IP = "10.0.2.15"
GATEWAY = "10.0.2.2"
NAMESERVER = "10.0.2.3"

CARDS = {
    "3c507": "isa-3c507",
    "starlan10": "isa-starlan10",
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--card", choices=sorted(CARDS), default="3c507")
    ap.add_argument("--keep", action="store_true",
                    help="write to the disk image instead of a snapshot")
    ap.add_argument("--timeout", type=int, default=300)
    args = ap.parse_args()

    for path in (QEMU, IMAGE):
        if not os.path.exists(path):
            sys.exit(f"missing {path}; run the other scripts in {HERE} first")

    drive = f"file={IMAGE},format=qcow2,if=ide,index=0"
    if not args.keep:
        drive += ",snapshot=on"

    qemu_args = [
        "-M", "pc",
        "-m", "256",
        # The 82586 card is at IRQ 7, which the parallel port would otherwise
        # have; two edge-triggered ISA devices cannot share it.
        "-parallel", "none",
        "-drive", drive,
        "-boot", "c",
        "-display", "none",
        "-no-reboot",
        "-serial", "stdio",
        "-monitor", "none",
        "-netdev", "user,id=n0",
        "-device", f"{CARDS[args.card]},netdev=n0",
    ]

    print(f"== booting {IMAGE} with {CARDS[args.card]}")
    child = pexpect.spawn(QEMU, qemu_args, timeout=args.timeout,
                          encoding="utf-8", codec_errors="replace")
    child.logfile_read = sys.stdout

    failures = []
    try:
        # ai(4) takes the kernel down during autoconf; catch the debugger
        # prompt rather than sitting out the timeout waiting for a login.
        if child.expect(["login:", r"db\{0\}>"]) == 1:
            failures.append("the kernel dropped into ddb during autoconf")
            raise KeyboardInterrupt
        # The install leaves root without a password; see make-image.py.
        child.sendline("root")
        child.expect(r"# ")

        # dmesg has already gone past; ask the driver directly instead.
        run(child, "ifconfig -l")
        if "ef0" not in child.before and "ai0" not in child.before:
            failures.append("no 82586 interface attached")

        run(child, f"ifconfig {iface(args.card)} inet {GUEST_IP} "
                   f"netmask 255.255.255.0 up")
        run(child, f"route -q add default {GATEWAY}")

        for target in (GATEWAY, NAMESERVER):
            run(child, f"ping -c 4 {target}")
            if "0.0% packet loss" not in child.before:
                failures.append(f"ping {target} lost packets")

        run(child, "netstat -in")
        failures += counter_errors(child.before, iface(args.card))
        run(child, f"ifconfig {iface(args.card)}")

        child.sendline("halt -p")
        child.expect(pexpect.EOF)
    except pexpect.TIMEOUT:
        failures.append("timed out waiting for the guest")
    except KeyboardInterrupt:
        pass
    finally:
        child.close(force=True)

    print()
    if failures:
        for f in failures:
            print(f"FAIL: {f}")
        return 1
    print("PASS: the driver attached and passed traffic both ways")
    return 0


def iface(card):
    return "ef0" if card == "3c507" else "ai0"


def counter_errors(stats, name):
    """Pick the error and collision counters out of `netstat -in'.

    The per-address lines repeat the same counters, so only the <Link> line is
    read:  Name Mtu Network Address Ipkts Ierrs Opkts Oerrs Colls
    """
    for line in stats.splitlines():
        f = line.split()
        if len(f) >= 9 and f[0] == name and f[2] == "<Link>":
            if f[4] == "0":
                return ["no frames received"]
            if f[6] == "0":
                return ["no frames transmitted"]
            return [f"{what}: {f[i]}" for what, i in
                    (("input errors", 5), ("output errors", 7),
                     ("collisions", 8)) if f[i] != "0"]
    return [f"no counters for {name}"]


def run(child, cmd):
    child.sendline(cmd)
    child.expect(r"# ")


if __name__ == "__main__":
    sys.exit(main())
