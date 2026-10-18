"""Transport tests: RFC-2217 connect-out, listen ports, raw TCP, reconnects (FSD §5.2)."""

import os
import socket
import subprocess
import tempfile
import time
import unittest

from fnet import (ACK, COMPORT, Daemon, FlexClient, MockTerminalServer, Pty, connect_peer, cksum)

EXPECT_SB = [
    bytes([COMPORT, 1]) + (38400).to_bytes(4, "big"),
    bytes([COMPORT, 2, 8]),
    bytes([COMPORT, 3, 1]),
    bytes([COMPORT, 4, 1]),
    bytes([COMPORT, 5, 1]),
]


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class Base(unittest.TestCase):
    def tearDown(self):
        rc = self.d.stop()
        if os.environ.get("FNET_SHOWLOG"):
            print(self.d.read_log()[-6000:])
        self.d.cleanup()
        self.assertEqual(rc, 0)


class ConnectOut(Base):
    def start(self, telnet="yes", **mock):
        self.ts = MockTerminalServer(**mock)
        self.d = Daemon({"ts": {"type": "rfc2217", "host": "127.0.0.1", "port": str(self.ts.port),
                                "baud": "38400", "telnet": telnet}})
        self.d.mkimg("A.DSK")
        self.d.start(wait_ports=False)
        self.peer = self.ts.accept()
        self.peer.raw = telnet == "no"
        self.peer.pump(0.4)
        self.c = FlexClient(self.peer)

    def tearDown(self):
        self.peer.close()
        self.ts.close()
        super().tearDown()

    def test_011_negotiation(self):
        """TEST-011: BINARY, SGA, COM-PORT-OPTION and 38400 8N1 no-flow."""
        self.start()
        ev = self.peer.events
        for want in (("WILL", 0), ("DO", 0), ("WILL", 3), ("DO", 3), ("WILL", 44)):
            self.assertIn(want, ev)
        sbs = [e[1] for e in ev if e[0] == "SB"]
        self.assertEqual(sbs, EXPECT_SB)
        self.assertEqual(self.c.sync(), b"\x55\xaa")

    def test_012_iac_transparency(self):
        """TEST-012: $FF bytes round-trip; an IAC NOP mid-sector is ignored."""
        self.start()
        self.assertEqual(self.c.mount("A"), b"\x06W")
        data = bytes([0xFF] * 100 + list(range(156)))
        ck = cksum(data)
        msg = b"r" + bytes([0, 1, 1]) + data[:50]
        self.peer.send(msg)
        self.peer.send_raw(bytes([255, 241]))           # IAC NOP
        self.peer.send(data[50:] + bytes([ck >> 8, ck & 0xFF]))
        self.assertEqual(self.peer.recv(1), bytes([ACK]))
        got, gck, _ = self.c.read(0, 1, 1)
        self.assertEqual(got, data)
        self.assertEqual(gck, ck)

    def test_014_refused_control(self):
        """TEST-011 refusal: SET-CONTROL answered with 0 -> warning, port still works."""
        self.start(refuse={"SET-CONTROL"})
        self.assertTrue(self.d.wait_log("answered setting 5 with 0"))
        self.assertEqual(self.c.q(), bytes([ACK]))

    def test_014_refused_comport(self):
        self.start(refuse={COMPORT})
        self.assertTrue(self.d.wait_log("refused COM-PORT-OPTION"))
        self.assertEqual(self.c.q(), bytes([ACK]))

    def test_013_reconnect_keeps_session(self):
        """TEST-013: reconnect after the terminal server drops; mapping retained."""
        self.start()
        self.assertEqual(self.c.mount("A", drv=2), b"\x06W")
        self.peer.close()
        t0 = time.monotonic()
        self.peer = self.ts.accept(timeout=5)
        dt = time.monotonic() - t0
        self.assertGreater(dt, 0.8)
        self.assertLess(dt, 2.5)
        self.peer.pump(0.3)
        self.c = FlexClient(self.peer)
        self.assertEqual(self.c.drive_name(2), b"A.DSK")

    def test_019_keepalive(self):
        """REQ-134: keepalive is enabled (checked via ss timer)."""
        self.start()
        out = subprocess.run(["ss", "-tno", "state", "established", "dport", f"= :{self.ts.port}"],
                             capture_output=True, text=True).stdout
        self.assertIn("keepalive", out)

    def test_090_raw_tcp(self):
        """TEST-090: telnet=no: no negotiation, $FF passes unescaped."""
        self.start(telnet="no")
        self.assertEqual(self.peer.events, [])
        self.assertEqual(self.c.mount("A"), b"\x06W")
        data = bytes([0xFF] * 256)
        self.assertEqual(self.c.write(0, 2, 2, data), bytes([ACK]))
        got, ck, raw = self.c.read(0, 2, 2)
        self.assertEqual(raw[:256], data)


class Listen(Base):
    def start(self, **kv):
        self.lport = free_port()
        cfg = {"type": "listen", "bind": "127.0.0.1", "port": str(self.lport), "baud": "38400",
               "allow": "127.0.0.0/8"}
        cfg.update(kv)
        self.d = Daemon({"l1": cfg})
        self.d.mkimg("A.DSK")
        self.d.start()

    def test_015_listening_sockets(self):
        """TEST-015: exactly the configured listen socket."""
        self.start()
        out = subprocess.run(["ss", "-Hltnp"], capture_output=True, text=True).stdout
        mine = [l for l in out.splitlines() if f"pid={self.d.proc.pid}," in l]
        self.assertEqual(len(mine), 1, out)
        self.assertIn(f"127.0.0.1:{self.lport}", mine[0])

    def test_016_dial_in(self):
        """TEST-016: terminal server connects in; same negotiation; data works."""
        self.start()
        peer = connect_peer("127.0.0.1", self.lport)
        peer.pump(0.4)
        sbs = [e[1] for e in peer.events if e[0] == "SB"]
        self.assertEqual(sbs, EXPECT_SB)
        c = FlexClient(peer)
        self.assertEqual(c.mount("A"), b"\x06W")
        self.assertEqual(c.read(0, 0, 3)[0][16:20], b"A\x00\x00\x00")
        peer.close()

    def test_017_replace(self):
        """TEST-017: on_new=replace closes the old connection, keeps the session."""
        self.start()
        a = connect_peer("127.0.0.1", self.lport)
        a.pump(0.3)
        self.assertEqual(FlexClient(a).mount("A", drv=1), b"\x06W")
        b = connect_peer("127.0.0.1", self.lport)
        b.pump(0.3)
        self.assertEqual(a.sock.recv(100), b"")          # old one closed
        self.assertEqual(FlexClient(b).drive_name(1), b"A.DSK")
        self.assertTrue(self.d.wait_log("replaces"))
        a.close()
        b.close()

    def test_017_refuse(self):
        self.start(on_new="refuse")
        a = connect_peer("127.0.0.1", self.lport)
        a.pump(0.3)
        b = connect_peer("127.0.0.1", self.lport)
        b.sock.settimeout(2)
        self.assertEqual(b.sock.recv(100), b"")
        self.assertEqual(FlexClient(a).q(), bytes([ACK]))
        self.assertTrue(self.d.wait_log("refused connection"))
        a.close()
        b.close()

    def test_018_allow_list(self):
        """TEST-018: peers outside allow are closed before any data."""
        self.start(allow="10.0.0.0/8")
        p = connect_peer("127.0.0.1", self.lport)
        p.sock.settimeout(2)
        self.assertEqual(p.sock.recv(100), b"")
        self.assertTrue(self.d.wait_log("not in allow list"))
        p.close()

    def test_018_empty_allow(self):
        self.start(allow="")
        self.assertTrue(self.d.wait_log("allow list is empty"))
        p = connect_peer("127.0.0.1", self.lport)
        p.sock.settimeout(2)
        self.assertEqual(p.sock.recv(100), b"")
        p.close()

    def test_090_raw_listen(self):
        self.start(telnet="no")
        p = connect_peer("127.0.0.1", self.lport, raw=True)
        c = FlexClient(p)
        self.assertEqual(c.mount("A"), b"\x06W")
        data = bytes([0xFF] * 256)
        self.assertEqual(c.write(0, 3, 3, data), bytes([ACK]))
        self.assertEqual(c.read(0, 3, 3)[0], data)
        self.assertEqual(p.events, [])
        p.close()


class TtyReopen(Base):
    def test_016_tty_disappears(self):
        """TEST-013 (REQ-016): tty vanishes and comes back; session kept."""
        pty1 = Pty()
        link = os.path.join(tempfile.mkdtemp(prefix="fnet-tty-"), "ttyLINK")
        os.symlink(pty1.path, link)
        self.d = Daemon({"t": {"type": "tty", "device": link, "baud": "9600"}})
        self.d.mkimg("A.DSK")
        self.d.start()
        self.assertEqual(FlexClient(pty1).mount("A", drv=0), b"\x06W")
        pty1.close()
        self.assertTrue(self.d.wait_log("link closed"))
        pty2 = Pty()
        os.unlink(link)
        os.symlink(pty2.path, link)
        self.d.wait_links(timeout=6)
        self.assertEqual(FlexClient(pty2).drive_name(0), b"A.DSK")
        pty2.close()
        os.unlink(link)
        os.rmdir(os.path.dirname(link))


if __name__ == "__main__":
    unittest.main()
