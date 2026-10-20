"""FLEX 6800 client (src/6800): FNETDRV and the utilities, run in emu68 against fnetd.

Needs the .CMD files from "make -C src/6800"; skipped without them.
"""

import os
import unittest

from emu68 import CPU, DRVTBL, ECHOFL, MEMEND, TTYEOL
from fnet import BUILD, Daemon, Pty

CMD = os.path.join(BUILD, "6800")
NAMES = ("FNETDRV", "FCREATE", "FDRIVE", "FEXIT", "FMOUNT", "MEMDUMP",
         "RCD", "RDELETE", "RDIR", "RESYNC", "RLIST")
MISSING = [n for n in NAMES if not os.path.exists(os.path.join(CMD, n + ".CMD"))]


def write_file(path, data):
    with open(path, "wb") as f:
        f.write(data)


@unittest.skipIf(MISSING, "6800 client not built (make -C src/6800)")
class Client(unittest.TestCase):
    """A fresh fnetd and emulated FLEX 6800 for each test."""

    def setUp(self):
        self.addCleanup(self.cleanup)
        self.pty = Pty()
        self.d = Daemon({"p1": {"type": "tty", "device": self.pty.path, "baud": "115200"}})
        self.prepare()
        self.d.start()
        self.cpu = CPU(self.pty.master)

    def prepare(self):
        pass

    def cleanup(self):
        self.d.cleanup()
        self.pty.close()

    def run_cmd(self, line, keys=b""):
        return self.cpu.run(os.path.join(CMD, line.split()[0] + ".CMD"), line, keys)

    def path(self, rel):
        return os.path.join(self.d.flex, rel)

    def netdrv(self):
        base = self.cpu.r16(MEMEND) + 1
        return list(self.cpu.m[base + 55:base + 59])

    def install(self):
        out = self.run_cmd("FNETDRV")
        self.assertIn("established", out)
        self.assertIn("installed", out)


class Utilities(Client):
    def prepare(self):
        self.d.mkimg("TEST.DSK", tracks=35, sectors=10)
        self.d.mkimg("OTHER.DSK", tracks=35, sectors=10)
        self.d.mkimg("sub/INSUB.DSK", tracks=35, sectors=10)

    def test_010_not_loaded(self):
        for line in ("RCD /", "FMOUNT", "RDIR", "FEXIT"):
            self.assertIn("not loaded", self.run_cmd(line), line)

    def test_020_install_resync_exit(self):
        self.install()
        self.assertLess(self.cpu.r16(MEMEND), 0x7FFF)
        self.assertIn("successfully", self.run_cmd("RESYNC"))
        base = self.cpu.r16(MEMEND) + 1
        out = self.run_cmd("FEXIT")
        self.assertIn("MEMEND restored", out)
        self.assertEqual(self.cpu.r16(MEMEND), 0x7FFF)
        for n in range(7):
            self.assertEqual(self.cpu.r16(DRVTBL + 3 * n + 1), 0xBF00 + 3 * n)
        self.assertFalse(any(self.cpu.m[base:0x8000]), "package not zero-filled")
        self.assertTrue(self.d.wait_log("REXIT"))
        self.assertIn("not loaded", self.run_cmd("RCD /"))

    def test_030_fdrive_rcd(self):
        self.install()
        self.assertIn("directory is  /", self.run_cmd("FDRIVE"))
        base = self.cpu.r16(MEMEND) + 1
        self.assertIn("executed OK", self.run_cmd("FDRIVE A"))
        self.assertEqual(self.cpu.m[base + 51], ord("@"))
        self.run_cmd("FDRIVE C")
        self.assertEqual(self.cpu.m[base + 51], ord("B"))
        self.assertEqual(self.cpu.m[TTYEOL], ord(":"))

        self.assertIn("executed OK", self.run_cmd("RCD sub"))
        self.assertIn("/sub", self.run_cmd("FDRIVE"))
        self.assertIn("Bad directory", self.run_cmd("RCD nowhere"))
        self.assertIn("executed OK", self.run_cmd("RCD /"))     # one character
        self.assertIn("Bad directory", self.run_cmd("RCD"))

    def test_040_listings(self):
        self.install()
        out = self.run_cmd("RDIR")
        self.assertIn("TEST.DSK", out)
        self.assertIn("OTHER.DSK", out)
        out = self.run_cmd("RDIR T*")
        self.assertIn("TEST.DSK", out)
        self.assertNotIn("OTHER", out)
        self.assertIn("sub", self.run_cmd("RLIST"))

    def test_050_fmount(self):
        self.install()
        self.assertIn("full access", self.run_cmd("FMOUNT 1 TEST.DSK"))
        self.assertEqual(self.netdrv()[1], 1)
        self.assertIn("Could not open", self.run_cmd("FMOUNT other"))   # host is case-sensitive
        self.assertIn("full access", self.run_cmd("FMOUNT OTHER"))      # default drive 3
        self.assertEqual(self.netdrv()[3], 3)
        self.assertIn("Could not open", self.run_cmd("FMOUNT 2 NOPE"))
        self.assertEqual(self.netdrv()[2], 0xFF)
        self.assertIn("Illegal file extension", self.run_cmd("FMOUNT 0 X.TXT"))
        out = self.run_cmd("FMOUNT")
        self.assertIn("Drive 1 -> TEST.DSK", out)
        self.assertIn("Drive 3 -> OTHER.DSK", out)
        self.assertIn("unmapped", self.run_cmd("FMOUNT 1"))
        self.assertEqual(self.netdrv()[1], 0xFF)

    def test_060_driver(self):
        self.install()
        self.run_cmd("FMOUNT 1 TEST")
        self.run_cmd("FMOUNT 3 OTHER")
        cpu, fcb = self.cpu, 0x2000

        def select(drv):
            cpu.m[fcb + 3] = drv
            cpu.x = fcb
            cpu.call(DRVTBL + 12)                   # DRIVE

        def sector(entry, trk, sec):
            cpu.x, cpu.a, cpu.b = 0x3000, trk, sec
            cpu.call(DRVTBL + entry)
            return cpu.b

        with open(self.path("TEST.DSK"), "rb") as f:
            img = f.read()
        select(1)
        self.assertEqual(sector(0, 0, 3), 0)       # READ the SIR
        self.assertEqual(bytes(cpu.m[0x3000:0x3100]), img[512:768])

        select(3)
        pat = bytes((i * 7 + 1) & 0xFF for i in range(256))
        cpu.m[0x3000:0x3100] = pat
        self.assertEqual(sector(3, 1, 1), 0)       # WRITE
        cpu.m[0x3000:0x3100] = bytes(256)
        self.assertEqual(sector(0, 1, 1), 0)
        self.assertEqual(bytes(cpu.m[0x3000:0x3100]), pat)

        cpu.local_calls.clear()
        select(0)
        self.assertEqual(cpu.local_calls, [4])     # local drive: FLEX's DRIVE

    def test_070_fcreate(self):
        self.install()
        out = self.run_cmd("FCREATE 2", b"\rNEW\r12\r3x\x08" b"5\r10\rY")   # backspace fixes 3x
        self.assertIn("executed OK", out)
        self.assertEqual(os.path.getsize(self.path("NEW.DSK")), 35 * 10 * 256)
        self.assertEqual(self.netdrv()[2], 2)

        out = self.run_cmd("FCREATE 0", b"sub\rZERO\r1\r\r2\r10\rY")         # empty number re-asked
        self.assertIn("executed OK", out)
        self.assertTrue(os.path.exists(self.path("sub/ZERO.DSK")))
        self.assertEqual(self.netdrv()[0], 0)

        self.assertIn("Error message", self.run_cmd("FCREATE", b"\rNEW\r1\r35\r10\rY"))
        self.assertIn("Bad drive", self.run_cmd("FCREATE 7"))
        self.run_cmd("FCREATE", b"\rABC\r1\r35\r10\rN")
        self.assertFalse(os.path.exists(self.path("ABC.DSK")))

        out = self.run_cmd("FMOUNT")
        self.assertIn("Drive 2 -> NEW.DSK", out)
        self.assertIn("Drive 0 -> ZERO.DSK", out)

    def test_080_rdelete(self):
        self.install()
        write_file(self.path("KEEP.DSK"), bytes(2560))
        out = self.run_cmd("RDELETE KEEP", b"N")
        self.assertIn("/KEEP.DSK (Y/N)", out)
        self.assertNotIn("//", out)
        self.assertIn("not deleted", out)
        self.assertTrue(os.path.exists(self.path("KEEP.DSK")))
        self.assertIn("executed OK", self.run_cmd("RDELETE KEEP.DSK", b"Y"))
        self.assertFalse(os.path.exists(self.path("KEEP.DSK")))

        self.run_cmd("FMOUNT 3 OTHER")
        self.assertIn("Error in processing", self.run_cmd("RDELETE OTHER", b"Y"))
        self.run_cmd("FMOUNT 3")                    # unmaps on FLEX only: still open
        self.assertIn("Error in processing", self.run_cmd("RDELETE OTHER", b"Y"))
        self.assertIn("Illegal file name", self.run_cmd("RDELETE"))

        self.run_cmd("RCD sub")
        out = self.run_cmd("RDELETE INSUB", b"Y")
        self.assertIn("/sub/INSUB.DSK", out)
        self.assertIn("executed OK", out)

    def test_090_memdump(self):
        self.cpu.m[0x4000:0x4010] = b"Hello, 6800!\x00\x01\x7f\x80"
        out = self.run_cmd("MEMDUMP 4000", b"q")
        self.assertIn("4000 48 65 6C 6C 6F 2C 20 36 38 30 30 21 00 01 7F 80   Hello, 6800!....", out)
        self.assertEqual(out.count("\r\n"), 16)
        self.assertEqual(self.cpu.m[ECHOFL], 0xFF)


class Paging(Client):
    def prepare(self):
        for i in range(45):
            write_file(self.path("IMG%02d.DSK" % i), bytes(2560))
        for i in range(25):
            os.makedirs(self.path("dir%02d" % i))

    def count(self, out, fmt, n):
        return sum(1 for i in range(n) if fmt % i in out)

    def test_010_rdir(self):
        self.install()
        out = self.run_cmd("RDIR", b"  ")
        self.assertEqual(self.count(out, "IMG%02d", 45), 45)
        self.assertEqual(out.count("Press spacebar"), 2)
        out = self.run_cmd("RDIR", b"x\x1b")        # x asks again, ESC stops
        self.assertEqual(self.count(out, "IMG%02d", 45), 19)
        out = self.run_cmd("RDIR IMG4*:CAT")         # stops at the EOL character
        self.assertEqual(self.count(out, "IMG%02d", 45), 5)
        self.assertEqual(self.cpu.m[TTYEOL], ord(":"))

    def test_020_rlist(self):
        self.install()
        n = self.count(self.run_cmd("RLIST", b"\x1b"), "dir%02d", 25)
        self.assertTrue(15 <= n < 25, n)
        self.assertEqual(self.count(self.run_cmd("RLIST", b" "), "dir%02d", 25), 25)


if __name__ == "__main__":
    unittest.main()
