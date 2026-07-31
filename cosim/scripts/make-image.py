#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# Install NetBSD onto a disk image by driving sysinst over the serial console.
#
# There is no display anywhere in this setup, so the install is done the way it
# would be on a headless machine: boot the serial console image and answer the
# installer's menus.  The image is not in git - this script is, and it is what
# puts the image back.
#
# sysinst is a full screen program, so what comes out of the serial port is not
# a transcript: it is cursor movements, clears and redraws, and every redraw
# repeats every line of the screen it paints.  Matching patterns against that
# stream finds text that is no longer on screen - which is how an earlier
# version of this script answered "install to hard disk" and then immediately
# picked "exit install system", four lines below it on the same menu, and quit
# the installer.
#
# So the stream goes through a terminal emulator and the matching is done
# against the screen as rendered.  A line matches only if it is on screen now,
# and the menu letter is read off that same row, which also means the letters
# moving between screens stops mattering.

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

try:
    import pyte
except ImportError:
    sys.exit("python3-pyte is not installed; see doc/cosimulation.md")


class Console:
    """A QEMU serial console, rendered rather than merely logged."""

    def __init__(self, child, log, cols=80, rows=24):
        self.child = child
        self.log = log
        self.screen = pyte.Screen(cols, rows)
        self.stream = pyte.ByteStream(self.screen)

    def pump(self, timeout=0.2):
        """Reads what is available and paints it.  True if anything arrived."""
        try:
            data = self.child.read_nonblocking(8192, timeout)
        except pexpect.TIMEOUT:
            return False
        except pexpect.EOF:
            raise SystemExit("QEMU exited\n" + self.dump())
        self.log.write(data)
        self.log.flush()
        self.stream.feed(data)
        return True

    def text(self):
        return "\n".join(self.screen.display)

    def settled(self, quiet=1.0, limit=3600):
        """Pumps until the screen has stopped changing for `quiet` seconds.

        During the extract the screen changes constantly, so this simply keeps
        reading until it stops, which is when the next question is up.
        """
        prev = self.text()
        idle = 0.0
        start = time.time()
        while idle < quiet:
            if time.time() - start > limit:
                break
            self.pump(0.2)
            now = self.text()
            if now != prev:
                prev, idle = now, 0.0
            else:
                idle += 0.2
        return prev

    def send(self, text, delay=0.2):
        time.sleep(delay)
        self.child.send(text)

    def line(self, text=""):
        self.send(text + "\r")

    def letter_for(self, label, text=None):
        """The menu letter of the row whose text contains label, or None.

        Not anchored to the start of the line: a menu row renders as

            x>a: Installation messages in English         x

        where the x is the box border - pyte draws the DEC line drawing set as
        ASCII letters - and the > is the cursor on the selected entry.
        """
        if text is None:
            text = self.text()
        m = re.search(r"([a-z]):\s*" + label, text)
        return m.group(1) if m else None

    def dump(self):
        return "\n".join("  | " + r.rstrip() for r in self.screen.display)


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
        "-m", args.memory,        # the install ramdisk does not fit in 64
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
    child = pexpect.spawn(cmd[0], cmd[1:], timeout=None, encoding=None,
                          dimensions=(24, 80))
    con = Console(child, log)

    try:
        run_sysinst(con, args)
    finally:
        log.close()
        print("== console log in %s" % logpath)

    print("== installed to %s" % image)


def run_sysinst(con, args):
    """Answers whatever screen sysinst puts up, until the install is done.

    Each row is something that might be on screen and what to do about it.  The
    first row whose text is on the *current* screen wins, so the order only
    matters where two of them could be up at once.
    """
    want_sets = "Minimal installation" if args.minimal else "Full installation"
    state = {"done": False, "extracted": False}

    def pick(label):
        def act(text):
            letter = con.letter_for(label, text)
            if letter is None:
                raise SystemExit("no entry %r on screen:\n%s" % (label, con.dump()))
            print("        %s -> %s" % (label, letter))
            con.line(letter)
        return act

    def enter(text):
        con.line()

    def exit_installer(text):
        # "Exit Install System" is also on the main menu, so it only means the
        # end once the sets have actually gone in.
        if not state["extracted"]:
            pick("Install NetBSD to hard disk")(text)
            return
        pick("Exit Install System")(text)
        state["done"] = True

    def extracted(text):
        state["extracted"] = True
        con.line()

    table = [
        # The boot loader counts down to booting anyway; RETURN takes its
        # default straight away.  Once only: pressing it does not clear the
        # screen, the spinner just starts changing underneath the menu, so a
        # row that could fire twice would fire until the guard stopped it.
        ("Choose an option; RETURN for default", enter, True),
        ("Terminal type",                    enter),
        ("Installation messages in English", pick("Installation messages in English")),
        # Only one disk is attached, so wd0 is the one to install onto.
        ("Available disks",                  pick("wd0")),
        ("partitioning scheme",              pick("Master Boot Record")),
        ("This is the correct geometry",     pick("This is the correct geometry")),
        ("Use the entire disk",              pick("Use the entire disk")),
        ("install the NetBSD bootcode",      pick("Yes")),
        # 10.1 offers "default"; older releases said "existing".
        (r"Use (default|existing) partition sizes",
                                             pick(r"Use (default|existing) partition sizes")),
        ("Partition sizes ok",               pick("Partition sizes ok")),
        ("name for your NetBSD disk",        enter),
        (want_sets,                          pick(want_sets)),
        ("CD-ROM",                           pick("CD-ROM")),
        ("Hit enter to continue",            extracted),
        ("Finished configuring",             pick("Finished configuring")),
        ("Configure network",                pick("Finished configuring")),
        ("Install NetBSD to hard disk",      pick("Install NetBSD to hard disk")),
        ("Exit Install System",              exit_installer),
        (r"[a-z]:\s*Continue",               pick("Continue")),
        (r"[a-z]:\s*Yes",                    pick("Yes")),
    ]

    seen = {}
    spent = set()          # rows that have had their turn
    pending = None         # a row that has been answered and must go away
    quiet_since = time.time()
    for step in range(600):
        text = con.settled()
        # Wait for the question just answered to leave the screen before
        # answering anything else.  Comparing whole screens is not enough:
        # pressing a letter moves the selection marker, so the screen differs
        # while the same menu is still up, and the question gets answered
        # twice.  The second keystroke then lands on whatever came next -
        # pressing b for "master boot record" twice put the second b into the
        # following menu, where it meant "set the geometry by hand", and the
        # install ended up in a prompt asking for a sector count.
        if pending is not None:
            if re.search(table[pending][0], text, re.M):
                continue
            pending = None
        hit = None
        for i, row in enumerate(table):
            pat = row[0]
            if i in spent:
                continue
            if re.search(pat, text, re.M):
                hit = i
                break
        if hit is None:
            # Not every screen is a question.  The kernel probing devices, the
            # boot loader loading, the sets going in - all of them sit there
            # with nothing to answer, sometimes for minutes.  Only give up if
            # nothing worth answering has appeared for a long time.
            if time.time() - quiet_since > args.stall:
                raise SystemExit(
                    "nothing to answer for %d seconds:\n%s"
                    % (args.stall, con.dump()))
            continue
        quiet_since = time.time()
        name = table[hit][0]
        if len(table[hit]) > 2 and table[hit][2]:
            spent.add(hit)
        seen[hit] = seen.get(hit, 0) + 1
        if seen[hit] > 15:
            raise SystemExit("stuck on %r:\n%s" % (name, con.dump()))
        print("   [%2d] %s" % (step, name))
        table[hit][1](text)
        pending = hit
        if state["done"]:
            break
    else:
        raise SystemExit("the installer never finished:\n" + con.dump())

    print("== waiting for a shell")
    for _ in range(60):
        text = con.settled(quiet=1.0, limit=120)
        if re.search(r"#\s*$", text, re.M):
            break
        con.line()
    print("== halting the guest")
    con.line("halt -p")
    time.sleep(10)
    con.child.close(force=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--qemu", default="qemu-system-i386",
                   help="which QEMU to install with")
    p.add_argument("--qemu-img", default=None)
    p.add_argument("--iso", default="NetBSD-10.1-i386-boot-com.iso")
    p.add_argument("--image", default=None)
    p.add_argument("--size", default="4G")
    p.add_argument("--memory", default="256")
    p.add_argument("--stall", type=int, default=1200,
                   help="give up after this many seconds with no question")
    p.add_argument("--minimal", action="store_true", default=True,
                   help="base and kernel sets only")
    p.add_argument("--full", dest="minimal", action="store_false")
    p.add_argument("--force", action="store_true", help="overwrite an existing image")
    build(p.parse_args())


if __name__ == "__main__":
    main()
