"""FlexNet protocol and dispatcher tests (FSD §5.3, §5.4)."""

import os
import time
import unittest

from fnet import ACK, NAK, ESC, SP, FlexClient, PtyTestCase, cksum


class Dispatcher(PtyTestCase):
    def prepare(self):
        self.d.mkimg("A.DSK")

    def test_020_no_sync_needed(self):
        """TEST-020: commands work without $55/$AA, also after a daemon restart."""
        self.assertEqual(self.c.mount("A"), b"\x06W")
        self.d.stop()
        self.d.start()
        self.assertEqual(self.c.q(), bytes([ACK]))
        self.assertEqual(self.c.mount("A"), b"\x06W")
        _, ck, raw = self.c.read(0, 0, 3)
        self.assertEqual(len(raw), 258)
        self.assertEqual(ck, cksum(raw[:256]))

    def test_021_punch_ignores_flexnet(self):
        """TEST-021: bytes during a punch are data, not commands."""
        self.pty.send(bytes([0x12]) + b"SQ\x55\r" + bytes([0x14]))
        self.assertEqual(self.pty.drain(0.4), b"")
        punch_dir = os.path.join(self.d.tape, "6800", "punch")
        files = os.listdir(punch_dir)
        self.assertEqual(len(files), 1)
        with open(os.path.join(punch_dir, files[0]), "rb") as f:
            self.assertEqual(f.read(), b"SQU\n")

    def test_022_transaction_timeout(self):
        """TEST-022: an incomplete parameter times out with no reply; sync still works."""
        self.pty.send(b"Pgames")
        self.assertEqual(self.pty.drain(1.5), b"")
        self.assertEqual(self.c.sync(), b"\x55\xaa")

    def test_023_sync_loss_by_garbage(self):
        """TEST-023: 16 junk bytes end the session (mounts cleared, cwd reset)."""
        os.makedirs(os.path.join(self.d.flex, "sub"))
        self.assertEqual(self.c.mount("A"), b"\x06W")
        self.assertEqual(self.c.cd("sub"), bytes([ACK]))
        self.pty.send(bytes([0x01]) * 16)
        self.assertEqual(self.pty.drain(0.3), b"")
        self.assertEqual(self.c.drive_name(0), b"")
        self.assertEqual(self.c.where(), b"/\r\x06")
        rc, st, _ = self.d.ctl_json("stats")
        self.assertEqual(st["data"]["stats"]["ports"][0]["sync_losses"], 1)

    def test_023b_sync_loss_by_timeouts(self):
        """TEST-023: 3 consecutive transaction timeouts end the session."""
        self.assertEqual(self.c.mount("A"), b"\x06W")
        for _ in range(3):
            self.pty.send(b"s\x00")
            time.sleep(1.3)
        self.assertEqual(self.c.drive_name(0), b"")
        rc, st, _ = self.d.ctl_json("stats")
        p = st["data"]["stats"]["ports"][0]
        self.assertEqual(p["sync_losses"], 1)
        self.assertEqual(p["xfer_timeouts"], 3)

    def test_024_never_unsolicited(self):
        """TEST-024: nothing is transmitted without a request, even when fnetctl changes mounts."""
        self.assertEqual(self.c.mount("A"), b"\x06W")
        self.d.fnetctl("umount", "p1", "0")
        self.d.fnetctl("mount", "p1", "1", "/A.DSK")
        self.assertEqual(self.pty.drain(1.0), b"")


class Protocol(PtyTestCase):
    def prepare(self):
        self.d.mkimg("A.DSK")
        self.d.mkimg("B.DSK")
        self.d.mkimg("C.DSK")
        self.d.mkimg("games/ADV.DSK")

    def test_030_basics(self):
        """TEST-030: sync, Q unmounted, ?, V."""
        self.assertEqual(self.c.sync(), b"\x55\xaa")
        self.assertEqual(self.c.q(), bytes([ACK]))
        where = self.c.where()
        self.assertEqual(where, b"/\r\x06")
        self.assertNotIn(self.d.flex.encode(), where)
        self.c.text_cmd("V", "A")
        self.assertEqual(self.pty.recv(1), bytes([ACK]))

    def test_031_per_drive_images(self):
        """TEST-031: different images on drive 0 and 2."""
        self.assertEqual(self.c.mount("A", drv=0), b"\x06W")
        self.assertEqual(self.c.mount("B", drv=2), b"\x06W")
        d0 = bytes([0xA0] * 256)
        d2 = bytes([0x2B] * 256)
        self.assertEqual(self.c.write(0, 5, 3, d0), bytes([ACK]))
        self.assertEqual(self.c.write(2, 5, 3, d2), bytes([ACK]))
        self.assertEqual(self.c.read(0, 5, 3)[0], d0)
        self.assertEqual(self.c.read(2, 5, 3)[0], d2)
        off = (18 + 4 * 18 + 2) * 256
        with open(os.path.join(self.d.flex, "A.DSK"), "rb") as f:
            f.seek(off)
            self.assertEqual(f.read(256), d0)
        with open(os.path.join(self.d.flex, "B.DSK"), "rb") as f:
            f.seek(off)
            self.assertEqual(f.read(256), d2)

    def test_032_error_reads_and_writes(self):
        """TEST-032: error reads are 258 bytes ending $00 $01; bad writes NAK."""
        self.assertEqual(self.c.mount("A", drv=0), b"\x06W")
        for drv, trk, sec in ((1, 0, 1), (0, 40, 1), (0, 1, 19)):
            data, ck, raw = self.c.read(drv, trk, sec, reply=NAK)
            self.assertEqual(len(raw), 258)
            self.assertEqual(raw[-2:], b"\x00\x01")
            self.assertEqual(data, bytes(256))
            self.assertEqual(self.c.q(), bytes([ACK]))     # still in sync
        data = bytes(256)
        self.assertEqual(self.c.write(1, 1, 1, data), bytes([NAK]))
        self.assertEqual(self.c.write(0, 1, 1, data, ck=0x1234), bytes([NAK]))
        self.assertEqual(self.c.write(0, 99, 1, data), bytes([NAK]))
        os.chmod(os.path.join(self.d.flex, "C.DSK"), 0o444)
        if os.geteuid() != 0:
            self.assertEqual(self.c.mount("C", drv=3), b"\x06R")
            self.assertEqual(self.c.write(3, 1, 1, data), bytes([NAK]))
        rc, st, _ = self.d.ctl_json("stats")
        p = st["data"]["stats"]["ports"][0]
        self.assertEqual(p["cksum_err_rx"], 1)
        self.assertEqual(p["cksum_err_tx"], 3)

    def test_034_legacy_drive(self):
        """TEST-034: M goes to legacy_drive only; failed mount keeps the old one."""
        self.assertEqual(self.c.mount("A"), b"\x06W")
        self.assertEqual(self.c.mount("B", drv=1), b"\x06W")
        self.assertEqual(self.c.mount("NOPE", drv=1), bytes([NAK]))
        self.assertEqual(self.c.drive_name(0), b"A.DSK")
        self.assertEqual(self.c.drive_name(1), b"B.DSK")
        self.assertEqual(self.c.drive_name(3), b"")
        self.assertEqual(self.c.read(3, 0, 3, reply=NAK)[2][-2:], b"\x00\x01")
        rc, out, err = self.d.fnetctl("port", "set", "p1", "legacy_drive=3")
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.c.mount("C"), b"\x06W")
        self.assertEqual(self.c.drive_name(3), b"C.DSK")
        self.assertEqual(self.c.drive_name(0), b"A.DSK")

    def test_035_write_lock(self):
        """TEST-035 (single port part): a second writer of the same image gets R."""
        self.assertEqual(self.c.mount("A", drv=0), b"\x06W")
        self.assertEqual(self.c.mount("A", drv=1), b"\x06R")
        self.assertTrue(self.d.wait_log("mounted read-only"))

    def test_036_cd_confinement(self):
        """TEST-036: cd inside the root only."""
        os.symlink("/etc", os.path.join(self.d.flex, "link-to-outside"))
        self.assertEqual(self.c.cd("games"), bytes([ACK]))
        self.assertEqual(self.c.where(), b"/games\r\x06")
        self.assertEqual(self.c.cd(".."), bytes([ACK]))
        self.assertEqual(self.c.cd(".."), bytes([NAK]))
        self.assertEqual(self.c.cd("/etc"), bytes([NAK]))
        self.assertEqual(self.c.cd("link-to-outside"), bytes([NAK]))
        self.assertEqual(self.c.where(), b"/\r\x06")
        self.assertEqual(self.c.mount("../A"), bytes([NAK]))

    def test_037_listings(self):
        """TEST-037: RDIR prefix/wildcard, RLIST, ESC."""
        for n in ("ADV.DSK", "adv2.dsk"):
            self.d.mkimg("l/" + n)
        for n in ("backupdsk", "X.TXT"):
            open(os.path.join(self.d.flex, "l", n), "w").close()
        os.makedirs(os.path.join(self.d.flex, "l", "a"))
        os.makedirs(os.path.join(self.d.flex, "l", "B"))
        self.assertEqual(self.c.cd("l"), bytes([ACK]))
        self.assertEqual(self.c.listing("A", "AD"), ["ADV.DSK", "adv2.dsk"])
        self.assertEqual(self.c.listing("A", "*2*"), ["adv2.dsk"])
        self.assertEqual(self.c.listing("A", ""), ["ADV.DSK", "adv2.dsk"])
        self.assertEqual(self.c.listing("I"), ["a", "B"])
        # ESC mid-listing
        self.c.text_cmd("A", "")
        self.assertEqual(self.pty.recv(2), b"\r\n")
        self.pty.send(SP)
        self.assertEqual(self.pty.recv_until(b"\r\n"), b"ADV.DSK\r\n")
        self.pty.send(ESC)
        self.assertEqual(self.pty.recv(1), bytes([ACK]))
        self.assertEqual(self.c.q(), bytes([ACK]))

    def test_038_create(self):
        """TEST-038: C and c create formatted images."""
        self.assertEqual(self.c.create("", "NEW", 5, 40, 18), bytes([ACK]))
        path = os.path.join(self.d.flex, "NEW.DSK")
        self.assertEqual(os.path.getsize(path), 40 * 18 * 256)
        import subprocess
        from fnet import FNETIMG
        r = subprocess.run([FNETIMG, "check", path], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("702 free", r.stdout)
        self.assertEqual(self.c.create("games", "BIG", 1, 80, 36, drv=2), bytes([ACK]))
        self.assertEqual(self.c.drive_name(2), b"BIG.DSK")
        self.assertEqual(self.c.create("", "HUGE", 1, 300, 18), bytes([NAK]))
        self.assertEqual(self.c.create("", "NEW", 5, 40, 18), bytes([NAK]))     # exists
        self.assertEqual(self.c.create("..", "OUT", 5, 40, 18), bytes([NAK]))

    def test_039_delete(self):
        """TEST-039: delete unmounted, mounted, missing."""
        self.assertEqual(self.c.mount("B", drv=0), b"\x06W")
        self.assertEqual(self.c.delete("C.DSK"), bytes([ACK]))
        self.assertFalse(os.path.exists(os.path.join(self.d.flex, "C.DSK")))
        self.assertEqual(self.c.delete("B.DSK"), bytes([NAK]))
        self.assertEqual(self.c.delete("NOPE.DSK"), bytes([NAK]))
        open(os.path.join(self.d.flex, "keep.txt"), "w").close()
        self.assertEqual(self.c.delete("keep.txt"), bytes([NAK]))

    def test_040_unknown_and_overflow(self):
        """TEST-040: unknown byte ignored; over-long parameter NAKs and keeps sync."""
        self.pty.send(b"\x01")
        self.assertEqual(self.pty.drain(0.3), b"")
        self.c.text_cmd("M", "X" * 200)
        self.assertEqual(self.pty.recv(1), bytes([NAK]))
        self.assertEqual(self.c.q(), bytes([ACK]))

    def test_041_turnaround(self):
        """TEST-041: reply turnaround p99 <= 5 ms."""
        lat = []
        for _ in range(500):
            t0 = time.perf_counter()
            self.pty.send(b"Q")
            r = self.pty.recv(1)
            lat.append(time.perf_counter() - t0)
            self.assertEqual(r, bytes([ACK]))
        lat.sort()
        self.assertLess(lat[int(len(lat) * 0.99)], 0.005)

    def test_053_rexit(self):
        """TEST-005: E ends the session, not the daemon."""
        self.assertEqual(self.c.mount("A", drv=1), b"\x06W")
        pid = self.d.proc.pid
        self.assertEqual(self.c.exit(), bytes([ACK]))
        self.assertEqual(self.c.drive_name(1), b"")
        self.assertIsNone(self.d.proc.poll())
        self.assertEqual(self.d.proc.pid, pid)


class ReadOnlyPort(PtyTestCase):
    port_extra = {"read_only": "yes"}

    def prepare(self):
        self.d.mkimg("A.DSK")

    def test_093_read_only(self):
        """TEST-035 (REQ-093): read_only port forces R and refuses C/D."""
        self.assertEqual(self.c.mount("A"), b"\x06R")
        self.assertEqual(self.c.create("", "N", 1, 10, 10), bytes([NAK]))
        self.assertEqual(self.c.delete("A.DSK"), bytes([NAK]))


class DDImage(PtyTestCase):
    def prepare(self):
        self.d.mkimg("DD.DSK", tracks=80, sectors=36, track0=20)

    def test_033_sd_track0(self):
        """TEST-033 over the wire: SD track 0 on a DD image."""
        self.assertEqual(self.c.mount("DD"), b"\x06W")
        self.assertEqual(self.c.read(0, 0, 20)[1], cksum(self.c.read(0, 0, 20)[0]))
        self.assertEqual(self.c.read(0, 0, 21, reply=NAK)[2][-2:], b"\x00\x01")
        marker = bytes([0x77] * 256)
        self.assertEqual(self.c.write(0, 1, 1, marker), bytes([ACK]))
        with open(os.path.join(self.d.flex, "DD.DSK"), "rb") as f:
            f.seek(20 * 256)
            self.assertEqual(f.read(256), marker)


if __name__ == "__main__":
    unittest.main()
