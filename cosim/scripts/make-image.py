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
import re
import shutil
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

    def choose(self, label, timeout=180):
        """Picks the menu entry whose text contains label.

        sysinst's menus keep the same wording but not the same letters - "use
        the entire disk" is a on one screen and b on the next - so the letter
        is read off the screen rather than assumed.  That is also what makes
        this survive a NetBSD release adding an option somewhere.
        """
        pat = re.compile(br"([a-z]):\s*" + label.encode())
        try:
            self.child.expect(pat, timeout=timeout)
        except pexpect.TIMEOUT:
            tail = self.child.before[-2000:] if self.child.before else b""
            raise SystemExit("no menu entry matching %r\nlast output:\n%s"
                             % (label, tail.decode("utf-8", "replace")))
        except pexpect.EOF:
            raise SystemExit("QEMU exited looking for a menu entry %r" % label)
        letter = self.child.match.group(1).decode()
        print("   %s -> %s" % (label, letter))
        self.line(letter)
        return letter


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

    # Our QEMU is built with --disable-tools, since building them costs time
    # and the image format is not what is under test.  Any qemu-img will do.
    qemu_img = args.qemu_img
    if not qemu_img or not os.path.exists(qemu_img):
        qemu_img = shutil.which("qemu-img")
    if not qemu_img:
        sys.exit("no qemu-img anywhere; install qemu-utils or build with tools")
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
    """Answers whatever screen sysinst puts up, until the install is done.

    Driving sysinst blind, a fixed sequence of answers is always one surprise
    behind: screens are optional, their order shifts, and the letters move.
    So this is a table instead - each entry is something that might appear and
    what to do about it - and the loop just keeps answering until the install
    says it has finished.  Adding support for a new release usually means one
    more row.
    """
    done = ["no"]

    def pick(label):
        return lambda: inst.choose(label)

    def enter():
        inst.line("")

    def finish():
        done[0] = "yes"

    # Ordered: the first pattern that matches wins, so put the specific ones
    # above the general ones.
    table = [
        (br"Terminal type",                      enter),
        (br"Installation messages in English",   pick("Installation messages in English")),
        (br"Install NetBSD to hard disk",        pick("Install NetBSD to hard disk")),
        (br"partitioning scheme",                pick("Master Boot Record")),
        (br"This is the correct geometry",       pick("This is the correct geometry")),
        (br"Use the entire disk",                pick("Use the entire disk")),
        (br"install the NetBSD bootcode",        pick("Yes")),
        (br"Use existing partition sizes",       pick("Use existing partition sizes")),
        (br"Partition sizes ok",                 pick("Partition sizes ok")),
        (br"name for your NetBSD disk",          enter),
        (br"Minimal installation",
            pick("Minimal installation") if args.minimal else pick("Full installation")),
        (br"CD-ROM",                             pick("CD-ROM")),
        (br"([a-z]):\s*Continue",                pick("Continue")),
        (br"Finished configuring",               pick("Finished configuring")),
        (br"Configure network",                  pick("Finished configuring")),
        (br"Exit Install System",                finish),
        (br"Hit enter to continue",              enter),
        # sysinst asks this both to start and after each destructive step.
        (br"([a-z]):\s*Yes",                     pick("Yes")),
        (br"[Ss]hall we continue",               pick("Yes")),
        (br"#\s",                                finish),
    ]

    patterns = [p for p, _ in table]
    seen = {}
    for step in range(200):
        idx = inst.wait(patterns, timeout=3600, what="any sysinst screen")
        label = patterns[idx][:40].decode("ascii", "replace")
        # A screen that comes back many times means an answer is not being
        # accepted; stopping beats spinning until the timeout.
        seen[idx] = seen.get(idx, 0) + 1
        if seen[idx] > 12:
            raise SystemExit("stuck answering %r; see the console log" % label)
        print("   [%d] %s" % (step, label))
        table[idx][1]()
        if done[0] == "yes":
            break
    else:
        raise SystemExit("the installer never finished; see the console log")

    print("== halting the guest")
    inst.wait([br"#\s", br"Exit Install System"], timeout=600, what="a shell prompt")
    inst.line("")
    inst.line("halt -p")
    time.sleep(10)
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
