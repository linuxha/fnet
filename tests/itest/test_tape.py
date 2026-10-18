"""Paper tape S-Record load/punch tests (FSD §5.6)."""

import os
import time
import unittest

from fnet import DC1, DC2, DC3, DC4, PtyTestCase

GOOD = (b"S00600004844521B\n"
        b"S1130000285F245F2212226A000424290008237C2A\n"
        b"S9030000FC\n")
BAD3 = (b"S1130000285F245F2212226A000424290008237C2A\n"
        b"S1130000285F245F2212226A000424290008237C2A\n"
        b"S1130000285F245F2212226A000424290008237C2B\n"
        b"S9030000FC\n")


def crlf(b):
    return b.replace(b"\n", b"\r\n")


class TapeBase(PtyTestCase):
    def put(self, cpu, name, data):
        path = os.path.join(self.d.tape, cpu, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
        return path

    def show(self):
        rc, out, err = self.d.ctl_json("tape", "show", "p1")
        self.assertEqual(rc, 0, err)
        return out["data"]

    def select(self, s, term=b"\r"):
        self.pty.send(s.encode() + term)
        time.sleep(0.15)


class Selection(TapeBase):
    def prepare(self):
        self.put("6800", "A.S19", GOOD)
        self.put("6809", "B.S19", GOOD)
        self.put("6800", "C.S19", GOOD)
        self.put("MIKBUG", "D.S19", GOOD)

    def test_060_selection(self):
        """TEST-060: selection string parsing, case-insensitive tags, confinement."""
        self.select("|6800:A.S19|6809:B.S19")
        s = self.show()
        self.assertEqual((s["load"], s["punch"]), ("/6800/A.S19", "/6809/B.S19"))
        self.select("||6800:C.S19", term=b"\x00")
        s = self.show()
        self.assertEqual((s["load"], s["punch"]), ("/6800/A.S19", "/6800/C.S19"))
        self.select("|mikbug:D.S19", term=b"\x04")
        self.assertEqual(self.show()["load"], "/MIKBUG/D.S19")
        for bad in ("|ZZZ:X", "|6800:../../etc/passwd", "|68$K:X", "|6800:../6809/B.S19"):
            self.select(bad)
            self.assertEqual(self.show()["load"], "/MIKBUG/D.S19", bad)
        self.select("|" + "x" * 200)
        self.assertEqual(self.show()["load"], "/MIKBUG/D.S19")
        self.select("|C.S19")                      # default_cpu = 6800
        self.assertEqual(self.show()["load"], "/6800/C.S19")
        self.select("|-|-")
        s = self.show()
        self.assertEqual((s["load"], s["punch"]), ("", ""))
        self.assertEqual(self.pty.drain(0.2), b"")   # nothing echoed

    def test_fnetctl_tape_set(self):
        rc, out, err = self.d.fnetctl("tape", "set", "p1", "load=6809:B.S19", "punch=6800:OUT.S19")
        self.assertEqual(rc, 0, err)
        s = self.show()
        self.assertEqual((s["load"], s["punch"]), ("/6809/B.S19", "/6800/OUT.S19"))
        rc, out, err = self.d.fnetctl("tape", "set", "p1", "load=NOPE:X")
        self.assertEqual(rc, 1)


class Load(TapeBase):
    def prepare(self):
        self.put("6800", "A.S19", GOOD)

    def test_061_load_and_rethread(self):
        """TEST-061: DC1 sends the file with CRLF; DC1 at EOF restarts."""
        self.select("|A.S19")
        self.pty.send(DC1)
        self.assertEqual(self.pty.recv(len(crlf(GOOD)), timeout=3), crlf(GOOD))
        self.assertEqual(self.pty.drain(0.3), b"")
        self.pty.send(DC1)
        self.assertEqual(self.pty.recv(len(crlf(GOOD)), timeout=3), crlf(GOOD))

    def test_080_missing_sends_s9(self):
        """TEST-063: DC1 with nothing selected sends an S9 terminator."""
        self.pty.send(DC1)
        self.assertEqual(self.pty.recv(12), b"S9030000FC\r\n")
        self.assertTrue(self.d.wait_log("sending S9 terminator"))
        self.select("|NOFILE.S19")
        self.pty.send(DC1)
        self.assertEqual(self.pty.recv(12), b"S9030000FC\r\n")


class LoadNone(TapeBase):
    defaults_extra = "load_missing = none\nload_eol = raw"

    def prepare(self):
        self.put("6800", "A.S19", GOOD)

    def test_063_missing_none_and_raw(self):
        self.pty.send(DC1)
        self.assertEqual(self.pty.drain(0.4), b"")
        self.select("|A.S19")
        self.pty.send(DC1)
        self.assertEqual(self.pty.recv(len(GOOD)), GOOD)


class Paced(TapeBase):
    defaults_extra = "char_delay_us = 2000"

    def prepare(self):
        self.data = b"".join(b"S1130000285F245F2212226A000424290008237C2A\n" for _ in range(20)) + b"S9030000FC\n"
        self.put("6800", "P.S19", self.data)

    def test_062_pause_resume(self):
        """TEST-062: DC3 pauses within ~1 char; DC1 resumes where it stopped."""
        self.select("|P.S19")
        self.pty.send(DC1)
        time.sleep(0.1)
        self.pty.send(DC3)
        part1 = self.pty.drain(0.05)
        late = self.pty.drain(0.3)
        self.assertLessEqual(len(late), 2)
        self.assertGreater(len(part1), 10)
        s = self.show()
        self.assertEqual(s["mode"], "idle")
        self.pty.send(DC1)
        rest = self.pty.recv(len(crlf(self.data)) - len(part1) - len(late), timeout=10)
        self.assertEqual(part1 + late + rest, crlf(self.data))


class Validate(TapeBase):
    defaults_extra = "load_validate = reject"

    def prepare(self):
        self.put("6800", "BAD.S19", BAD3)
        self.put("6800", "OK.S19", GOOD)

    def test_064_reject(self):
        """TEST-064: reject refuses a bad file and logs the line."""
        self.select("|OK.S19")
        self.select("|BAD.S19")
        self.assertEqual(self.show()["load"], "/6800/OK.S19")
        self.assertTrue(self.d.wait_log("line 3: checksum error"))


class Punch(TapeBase):
    def punch(self, payload):
        self.pty.send(bytes([DC2]) + payload + bytes([DC4]))
        time.sleep(0.3)

    def read(self, cpu, name):
        with open(os.path.join(self.d.tape, cpu, name), "rb") as f:
            return f.read()

    def test_065_overwrite(self):
        """TEST-065: NUL leader/trailer stripped, CRLF -> LF, overwrite via rename."""
        self.select("||6809:OUT.S19")
        self.punch(b"\x00\x00\x00" + crlf(GOOD) + b"\x00\x00\xff\x7f")
        self.assertEqual(self.read("6809", "OUT.S19"), GOOD)
        self.punch(b"S9030000FC\r\n")
        self.assertEqual(self.read("6809", "OUT.S19"), b"S9030000FC\n")
        self.assertEqual([f for f in os.listdir(os.path.join(self.d.tape, "6809")) if f.startswith(".")], [])

    def test_065_auto_name(self):
        self.punch(crlf(GOOD))
        d = os.path.join(self.d.tape, "6800", "punch")
        files = os.listdir(d)
        self.assertEqual(len(files), 1)
        self.assertTrue(files[0].startswith("p1-") and files[0].endswith(".s19"), files)

    def test_065_abort_keeps_previous(self):
        """REQ-089: an unfinished overwrite punch leaves the old file intact."""
        self.select("||6800:KEEP.S19")
        self.punch(crlf(GOOD))
        self.pty.send(bytes([DC2]) + b"S1130000")
        time.sleep(0.2)
        self.assertEqual(self.read("6800", "KEEP.S19"), GOOD)
        rc, _, err = self.d.fnetctl("tape", "cancel", "p1")
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.read("6800", "KEEP.S19"), GOOD)
        self.assertEqual([f for f in os.listdir(os.path.join(self.d.tape, "6800")) if f.startswith(".")], [])

    def test_088_validation_warnings(self):
        """TEST-066 (part): bad line and missing terminator are logged, file kept."""
        self.select("||6800:W.S19")
        self.punch(b"S1130000285F245F2212226A000424290008237C2B\r\n")
        self.assertTrue(self.d.wait_log("line 1: checksum error"))
        self.assertTrue(self.d.wait_log("no S7/S8/S9 terminator"))
        self.assertTrue(os.path.exists(os.path.join(self.d.tape, "6800", "W.S19")))


class PunchAppend(Punch):
    defaults_extra = "punch_mode = append"

    def test_065_overwrite(self):
        self.select("||6800:APP.S19")
        self.punch(b"S9030000FC\r\n")
        self.punch(b"S9030000FC\r\n")
        self.assertEqual(self.read("6800", "APP.S19"), b"S9030000FC\nS9030000FC\n")

    def test_065_abort_keeps_previous(self):
        pass


class PunchVersion(Punch):
    defaults_extra = "punch_mode = version"

    def test_065_overwrite(self):
        self.select("||6800:V.S19")
        for _ in range(3):
            self.punch(b"S9030000FC\r\n")
        self.assertEqual(sorted(os.listdir(os.path.join(self.d.tape, "6800"))),
                         ["V.1.S19", "V.2.S19", "V.S19"])

    def test_065_abort_keeps_previous(self):
        pass


class PunchTimeout(TapeBase):
    defaults_extra = "punch_timeout = 1"

    def test_087_timeout(self):
        """TEST-066: punch idle timeout closes the file."""
        self.pty.send(bytes([DC2]) + b"S9030000FC\r\n")
        time.sleep(1.6)
        self.assertTrue(self.d.wait_log("punch idle for 1 s"))
        self.assertEqual(self.show()["mode"], "idle")
        self.assertEqual(self.c.q(), b"\x06")       # back to top of loop


if __name__ == "__main__":
    unittest.main()
