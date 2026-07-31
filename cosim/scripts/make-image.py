#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Install NetBSD onto a disk image by driving sysinst over the serial console.
#
# There is no display anywhere in this setup, so the install is done the same
# way it would be on a headless machine: boot the serial console image, and
# answer the installer's menus by sending the keys a person would.  The image
# is not in git - this script is, and it is what puts the image back.
#
# The menus are matched on short, stable pieces of text rather than on exact
# screens, because sysinst redraws constantly and the exact layout shifts
# between releases.  Run with --interactive to be dropped into the console
# when something does not match, which is the quickest way to find out what
# a new release is asking for.

import argparse
import os
import subprocess
import sys
import time

try:
    import pexpect
except ImportError:
    sys.exit("python3-pexpect is not installed; see doc/cosimulation.md")


class Installer:
    def __init__(self, child, log):
        self.child = child
        self.log = log

    def wait(self, patterns, timeout=180, what=""):
        """Waits for any of patterns; returns which one matched."""
        if isinstance(patterns, str):
            patterns = [patterns]
        try:
            return self.child.expect(patterns, timeout=timeout)
        except pexpect.TIMEOUT:
            tail = self.child.before[-2000:] if self.child.before else b""
            raise SystemExit(
                "timed out waiting for %s\nlast output:\n%s"
                % (what or patterns, tail.decode("utf-8", "replace")))
        except pexpect.EOF:
            raise SystemExit("QEMU exited while waiting for %s" % (what or patterns))

    def send(self, text, delay=0.3):
        time.sleep(delay)
        self.child.send(text)

    def line(self, text, delay=0.3):
        self.send(text + "\r", delay)


def build(args):
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(os.path.dirname(here))
    work = os.environ.get("WORK", os.path.join(root, "work"))
    iso = os.path.join(work, "downloads", args.iso)
    image = args.image or os.path.join(work, "images", "netbsd-10.1-i386.qcow2")
    logpath = os.path.join(work, "install.log")

    if not os.path.exists(iso):
        sys.exit("no ISO at %s; run fetch-netbsd.sh first" % iso)

    os.makedirs(os.path.dirname(image), exist_ok=True)
    if os.path.exists(image):
        if not args.force:
            sys.exit("%s exists; pass --force to rebuild it" % image)
        os.unlink(image)

    qemu_img = args.qemu_img or "qemu-img"
    subprocess.check_call([qemu_img, "create", "-f", "qcow2", image, args.size],
                          stdout=subprocess.DEVNULL)

    cmd = [
        args.qemu,
        # The pc machine, not isapc: qemu's isapc does not present an ATAPI
        # CD-ROM on its ISA IDE, so the installer boots in a loop looking for
        # its root.  pc still has an ISA bus hanging off the LPC bridge, which
        # is where the 82586 card will go, so nothing is given up by using it.
        "-M", "pc",
        "-m", "256",              # the install ramdisk does not fit in 64
        "-drive", "file=%s,format=qcow2,if=ide,index=0" % image,
        "-drive", "file=%s,format=raw,if=ide,index=2,media=cdrom" % iso,
        "-boot", "d",
        "-nographic",
        "-serial", "mon:stdio",
        "-net", "none",
    ]
    print("== booting the installer")
    print("   " + " ".join(cmd))

    log = open(logpath, "wb")
    child = pexpect.spawn(cmd[0], cmd[1:], timeout=300, encoding=None)
    child.logfile = log
    inst = Installer(child, log)

    try:
        run_sysinst(inst, args)
    finally:
        log.close()
        print("== console log in %s" % logpath)

    print("== installed to %s" % image)


def run_sysinst(inst, args):
    # The bootloader counts down on its own; the installer comes up asking for
    # a terminal type first.
    print("== waiting for the installer")
    inst.wait([br"Terminal type", br"a: Installation messages"], timeout=600,
              what="the first sysinst prompt")

    # Terminal type: the default (vt100) is right for a serial console.
    inst.line("")

    print("== choosing the language and starting the install")
    inst.wait(br"a: Installation messages in English", what="the language menu")
    inst.line("a")

    inst.wait(br"a: Install NetBSD to hard disk", what="the main menu")
    inst.line("a")

    inst.wait(br"Shall we continue", what="the confirmation")
    inst.line("b")          # b: Yes

    print("== partitioning")
    # Only one disk is attached, so it is the first entry.
    inst.wait([br"Available disks", br"a: wd0"], what="the disk list")
    inst.line("a")

    # Some releases ask which of MBR or GPT; take the default if asked.
    idx = inst.wait([br"a: This is the correct geometry",
                     br"a: Use the entire disk",
                     br"Do you want to install NetBSD"], timeout=120,
                    what="the geometry or whole disk question")
    if idx == 0:
        inst.line("a")
        inst.wait(br"a: Use the entire disk", what="the whole disk question")
    inst.line("a")

    # Bootblocks and the partition table.
    idx = inst.wait([br"install the NetBSD bootcode", br"a: Set sizes",
                     br"Partition table"], timeout=180, what="the bootcode question")
    if idx == 0:
        inst.line("b")      # b: Yes, install the bootcode

    print("== accepting the default partition sizes")
    inst.wait([br"a: Set sizes of NetBSD partitions",
               br"b: Use existing partition sizes"], what="the sizes menu")
    inst.line("b")          # keep it simple: the defaults are fine

    inst.wait([br"Partition sizes ok", br"x: Partition sizes ok"], timeout=120,
              what="the partition summary")
    inst.line("x")

    idx = inst.wait([br"Shall we continue", br"name for your NetBSD disk"],
                    timeout=120, what="the disk name or confirmation")
    if idx == 1:
        inst.line("")       # accept the default disk name
        inst.wait(br"Shall we continue", what="the confirmation")
    inst.line("b")          # b: Yes

    print("== selecting the sets (this takes a while)")
    idx = inst.wait([br"a: Full installation", br"Selected sets"], timeout=600,
                    what="the distribution set menu")
    if args.minimal:
        # d: Minimal installation - base and kernel only, which is all that is
        # needed to load a driver and run ifconfig.
        inst.line("d")
    else:
        inst.line("a")

    print("== installing from the CD")
    inst.wait([br"a: CD-ROM / DVD", br"Install from"], timeout=300,
              what="the install medium menu")
    inst.line("a")
    idx = inst.wait([br"Continue", br"x: Continue"], timeout=180, what="the medium confirmation")
    inst.line("x")

    # The extract takes minutes.
    print("== extracting")
    inst.wait([br"Hit enter to continue", br"configure additional items",
               br"Installation of NetBSD.*complete"], timeout=3600,
              what="the end of the extract")
    inst.line("")

    print("== finishing up")
    # A few post-install questions, all of which have a sensible default.
    for _ in range(12):
        idx = inst.wait([br"Hit enter to continue",
                         br"a: Configure network",
                         br"x: Finished configuring",
                         br"e: Exit Install System",
                         br"# "], timeout=600, what="a post-install prompt")
        if idx == 0:
            inst.line("")
        elif idx == 1:
            inst.line("x")          # skip network configuration
        elif idx == 2:
            inst.line("x")
        elif idx == 3:
            inst.line("e")
        else:
            break

    print("== shutting the guest down")
    inst.line("halt -p")
    time.sleep(5)
    inst.child.close(force=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--qemu", default="qemu-system-i386",
                   help="which QEMU to install with")
    p.add_argument("--qemu-img", default=None)
    p.add_argument("--iso", default="NetBSD-10.1-i386-boot-com.iso")
    p.add_argument("--image", default=None)
    p.add_argument("--size", default="4G")
    p.add_argument("--memory", default="256")
    p.add_argument("--minimal", action="store_true", default=True,
                   help="base and kernel sets only")
    p.add_argument("--full", dest="minimal", action="store_false")
    p.add_argument("--force", action="store_true", help="overwrite an existing image")
    build(p.parse_args())


if __name__ == "__main__":
    main()
