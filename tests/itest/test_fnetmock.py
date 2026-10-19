"""fnetmock: the mock FLEX target, driven against a real fnetd over each transport."""

import os
import socket
import subprocess
import tempfile
import time
import unittest

from fnet import FNETIMG, FNETMOCK, Daemon

GOOD = (b"S00600004844521B\n"
        b"S1130000285F245F2212226A000424290008237C2A\n"
        b"S9030000FC\n")
LONG = b"".join(b"S1130000285F245F2212226A000424290008237C2A\n" for _ in range(30)) + b"S9030000FC\n"


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def mock(*args, script=None, timeout=60):
    r = subprocess.run([FNETMOCK, *args], input=script, capture_output=True, text=True, timeout=timeout)
    return r.returncode, r.stdout, r.stderr


class Base(unittest.TestCase):
    telnet = "yes"
    defaults_extra = ""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="fnetmock-")
        self.port = free_port()
        self.d = Daemon({"lab": {"type": "listen", "bind": "127.0.0.1", "port": str(self.port),
                                 "allow": "127.0.0.1/32", "baud": "19200", "telnet": self.telnet}},
                        extra_defaults=self.defaults_extra)
        self.d.mkimg("A.DSK")
        with open(os.path.join(self.d.tape, "6800", "G.S19"), "wb") as f:
            f.write(GOOD)
        with open(os.path.join(self.d.tape, "6800", "LONG.S19"), "wb") as f:
            f.write(LONG)
        self.d.start()

    def tearDown(self):
        rc = self.d.stop()
        if os.environ.get("FNET_SHOWLOG"):
            print(self.d.read_log()[-6000:])
        self.d.cleanup()
        subprocess.run(["rm", "-rf", self.tmp])
        self.assertEqual(rc, 0)

    def t(self, name):
        return os.path.join(self.tmp, name)

    def run_mock(self, script, *extra):
        rc, out, err = mock("-C", f"127.0.0.1:{self.port}", *extra, "-s", "-", script=script)
        self.assertEqual(rc, 0, out + err)
        return out, err


class Connect(Base):
    def test_flexnet_session(self):
        out, err = self.run_mock(f"""
            sync; ping; pwd
            dir
            mount 1 A; drive 1; drive 2
            write 1 2 5 fill=$ff; read 1 2 5 out={self.t('s.bin')}
            !read 3 0 1 quiet       # nothing mounted: server read error
            !write 1 2 5 fill=1 badck
            fetch 1 {self.t('copy.dsk')}
            create . NEW 7 35 10; dir NEW; delete NEW.DSK
            dir max=0
            rexit
        """)
        self.assertIn("sync: ok", out)
        self.assertIn("mount: drive 1 A (W)", out)
        self.assertIn("drive 1: A.DSK", out)
        self.assertIn("drive 2: (none)", out)
        self.assertIn("NEW.DSK\n(1)", out)
        self.assertIn("(stopped after 0)", out)
        self.assertIn("read error reported by server", err)
        with open(self.t("s.bin"), "rb") as f:
            self.assertEqual(f.read(), b"\xff" * 256)       # IAC escaping both ways
        with open(self.t("copy.dsk"), "rb") as a, open(os.path.join(self.d.flex, "A.DSK"), "rb") as b:
            self.assertEqual(a.read(), b.read())
        self.assertTrue(self.d.wait_log("session ended: REXIT"))

    def test_tape_load_and_punch(self):
        src = self.t("in.s19")
        with open(src, "wb") as f:
            f.write(GOOD)
        out, err = self.run_mock(f"""
            select 6800:G.S19 6800:OUT.S19
            load out={self.t('got.s19')}
            punch {src} eol=cr leader=20 trailer=8
            sleep 300
        """)
        self.assertIn("load: 74 bytes, 3 records, 0 errors, terminated", out)
        with open(self.t("got.s19"), "rb") as f:
            self.assertEqual(f.read(), GOOD.replace(b"\n", b"\r\n"))
        with open(os.path.join(self.d.tape, "6800", "OUT.S19"), "rb") as f:
            self.assertEqual(f.read(), GOOD)

    def test_load_no_file_gets_s9(self):
        out, _ = self.run_mock("load")
        self.assertIn("load: 12 bytes, 1 records, 0 errors, terminated", out)

    def test_short_track0_fetch_and_put(self):
        img = self.t("SD0.DSK")
        subprocess.run([FNETIMG, "create", img, "--tracks", "40", "--sectors", "18", "--track0", "10",
                        "--label", "LOCAL"], check=True)
        self.d.mkimg("T.DSK", tracks=40, sectors=18, track0=10)
        out, _ = self.run_mock(f"mount 0 T; put 0 {img}; fetch 0 {self.t('back.dsk')}")
        self.assertIn("track 0: 10", out)
        with open(img, "rb") as a, open(self.t("back.dsk"), "rb") as b, \
                open(os.path.join(self.d.flex, "T.DSK"), "rb") as c:
            local = a.read()
            self.assertEqual(b.read(), local)
            self.assertEqual(c.read(), local)

    def test_exit_codes(self):
        rc, out, err = mock("-C", f"127.0.0.1:{self.port}", "-e", "ping; bogus; ping")
        self.assertEqual(rc, 1)
        self.assertEqual(out.count("ping: ACK"), 1)
        rc, out, err = mock("-C", f"127.0.0.1:{self.port}", "-k", "-e", "ping; bogus; ping")
        self.assertEqual(rc, 1)
        self.assertEqual(out.count("ping: ACK"), 2)
        rc, out, err = mock("-C", f"127.0.0.1:{self.port}", "!mount", "NOPE")
        self.assertEqual(rc, 0, err)
        self.assertEqual(mock("ping")[0], 2)                          # no transport
        self.assertEqual(mock("-C", f"127.0.0.1:{free_port()}", "ping")[0], 2)


class Paced(Base):
    defaults_extra = "char_delay_us = 1000"

    def test_pause_and_stop(self):
        out, _ = self.run_mock("select 6800:LONG.S19; load pause-every=200 pause-ms=200")
        size = len(LONG.replace(b"\n", b"\r\n"))
        self.assertIn(f"load: {size} bytes, 31 records, 0 errors, terminated", out)
        self.assertRegex(out, r"[1-9] pauses, max [0-2] byte\(s\) after DC3")
        out, _ = self.run_mock("select 6800:LONG.S19; load stop-after=100")
        self.assertRegex(out, r"stopped with DC3 after 100 bytes \([0-2] late\)")


class Raw(Base):
    telnet = "no"

    def test_raw_tcp(self):
        out, _ = self.run_mock(f"mount 0 A; write 0 1 1 fill=$ff; read 0 1 1 out={self.t('r.bin')}", "-r")
        with open(self.t("r.bin"), "rb") as f:
            self.assertEqual(f.read(), b"\xff" * 256)


class ListenAndPty(unittest.TestCase):
    """fnetmock as the terminal server fnetd dials, and as a pty for a tty port."""

    def tearDown(self):
        rc = self.d.stop()
        if os.environ.get("FNET_SHOWLOG"):
            print(self.d.read_log()[-6000:])
        self.d.cleanup()
        self.assertEqual(rc, 0)

    def test_listen_rfc2217(self):
        p = free_port()
        m = subprocess.Popen([FNETMOCK, "-L", f"127.0.0.1:{p}", "-w", "10000", "-v",
                              "-e", "sync; ping; status"], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             text=True)
        time.sleep(0.2)
        self.d = Daemon({"ts": {"type": "rfc2217", "host": "127.0.0.1", "port": str(p), "baud": "38400"}})
        self.d.start(wait_ports=False)
        out, err = m.communicate(timeout=20)
        self.assertEqual(m.returncode, 0, out + err)
        self.assertIn("sync: ok", out)
        self.assertIn("rfc2217 baud 38400", out)
        self.assertIn("SET-DATASIZE 8", err)

    def test_pty(self):
        link = os.path.join(tempfile.mkdtemp(prefix="fnetmock-"), "tty0")
        m = subprocess.Popen([FNETMOCK, f"--pty={link}", "-w", "1500", "-e", "sync; mount 0 A; read 0 0 3 quiet"],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        end = time.monotonic() + 5
        while not os.path.islink(link) and time.monotonic() < end:
            time.sleep(0.02)
        self.d = Daemon({"p1": {"type": "tty", "device": link, "baud": "115200"}})
        self.d.mkimg("A.DSK")
        self.d.start()
        out, err = m.communicate(timeout=20)
        self.assertEqual(m.returncode, 0, out + err)
        self.assertIn("read: drive 0 T0 S3 ok", out)
        self.assertFalse(os.path.lexists(link))
        os.rmdir(os.path.dirname(link))


if __name__ == "__main__":
    unittest.main()
