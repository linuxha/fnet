"""Daemon lifecycle, control, telemetry tests (FSD §5.1, §5.7)."""

import json
import os
import signal
import socket
import stat
import subprocess
import threading
import time
import unittest

from fnet import ACK, FNETCTL, FNETD, Daemon, FlexClient, Pty, cksum


class TwoPorts(unittest.TestCase):
    daemon_extra = ""

    def setUp(self):
        self.pa, self.pb = Pty(), Pty()
        self.d = Daemon({
            "a": {"type": "tty", "device": self.pa.path, "baud": "115200"},
            "b": {"type": "tty", "device": self.pb.path, "baud": "115200"},
        }, daemon_extra=self.daemon_extra)
        self.d.mkimg("A.DSK")
        self.ca, self.cb = FlexClient(self.pa), FlexClient(self.pb)

    def tearDown(self):
        self.d.stop()
        if os.environ.get("FNET_SHOWLOG"):
            print(self.d.read_log()[-6000:])
        self.d.cleanup()
        self.pa.close()
        self.pb.close()


class Lifecycle(TwoPorts):
    def test_001_ready_notify(self):
        """TEST-001: READY=1 via NOTIFY_SOCKET within 1 s."""
        path = os.path.join(self.d.dir, "notify")
        s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        s.bind(path)
        s.settimeout(1.0)
        t0 = time.monotonic()
        self.d.start(env={"NOTIFY_SOCKET": path})
        msg = s.recv(100)
        self.assertEqual(msg, b"READY=1")
        self.assertLess(time.monotonic() - t0, 1.0)
        s.close()

    def test_001_bad_config(self):
        """TEST-001: syntax error names file and line."""
        with open(self.d.conf) as f:
            lines = f.read().splitlines()
        while len(lines) < 11:
            lines.append("")
        lines.insert(11, "this is not valid")
        with open(self.d.conf, "w") as f:
            f.write("\n".join(lines) + "\n")
        r = subprocess.run([FNETD, "-f", "-c", self.d.conf], capture_output=True, text=True, timeout=5)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn(f"{self.d.conf}:12:", r.stderr)

    def test_002_reload(self):
        """TEST-002: SIGHUP restarts only the changed port; invalid reload keeps running."""
        self.d.start()
        self.assertEqual(self.ca.mount("A"), b"\x06W")
        ports = dict(self.d.ports)
        ports["b"] = dict(ports["b"], baud="9600")
        self.d.write_conf(ports)
        self.d.proc.send_signal(signal.SIGHUP)
        self.assertTrue(self.d.wait_log("[b] core: settings changed, restarting port"))
        self.assertFalse("[a] core: settings changed" in self.d.read_log())
        self.assertEqual(self.ca.drive_name(0), b"A.DSK")
        self.assertEqual(self.ca.read(0, 0, 3)[2][-2:] != b"\x00\x01", True)
        rc, out, _ = self.d.ctl_json("ports")
        self.assertEqual({p["name"]: p["baud"] for p in out["data"]["ports"]}, {"a": 115200, "b": 9600})
        # invalid key
        with open(self.d.conf, "a") as f:
            f.write("bogus_key = 1\n")
        rc, out, err = self.d.fnetctl("reload")
        self.assertEqual(rc, 1)
        self.assertIn("unknown key 'bogus_key'", err)
        self.assertEqual(self.ca.q(), bytes([ACK]))
        self.assertEqual(self.cb.q(), bytes([ACK]))

    def test_003_sigterm_during_writes(self):
        """TEST-003: SIGTERM during writes: exit <= 3 s, last ACKed sector on disk."""
        self.d.start()
        self.assertEqual(self.ca.mount("A"), b"\x06W")
        acked = []
        stop = threading.Event()

        def writer():
            i = 0
            while not stop.is_set():
                data = bytes([(i + k) & 0xFF for k in range(256)])
                trk, sec = 1 + (i // 18) % 39, 1 + i % 18
                if self.ca.write(0, trk, sec, data) == bytes([ACK]):
                    acked.append((trk, sec, data))
                else:
                    return
                i += 1

        t = threading.Thread(target=writer)
        t.start()
        time.sleep(0.3)
        t0 = time.monotonic()
        rc = self.d.stop()
        dt = time.monotonic() - t0
        stop.set()
        t.join(5)
        self.assertEqual(rc, 0)
        self.assertLess(dt, 3.0)
        self.assertTrue(acked)
        trk, sec, data = acked[-1]
        with open(os.path.join(self.d.flex, "A.DSK"), "rb") as f:
            f.seek((18 + (trk - 1) * 18 + sec - 1) * 256)
            self.assertEqual(f.read(256), data)

    def test_004_single_instance(self):
        """TEST-004: a second instance refuses to start."""
        self.d.start()
        r = subprocess.run([FNETD, "-f", "-c", self.d.conf], capture_output=True, text=True, timeout=5)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("already running", r.stderr)

    def test_014_port_add_remove(self):
        """TEST-014: add/disable/enable/remove a port while another transfers."""
        self.d.start()
        self.assertEqual(self.ca.mount("A"), b"\x06W")
        errors = []
        stop = threading.Event()

        def reader():
            while not stop.is_set():
                data, ck, raw = self.ca.read(0, 1, 1)
                if len(raw) != 258 or ck != cksum(data):
                    errors.append(raw)

        t = threading.Thread(target=reader)
        t.start()
        pc = Pty()
        try:
            for args in (("port", "add", "c", "type=tty", f"device={pc.path}", "baud=9600"),
                         ("port", "disable", "c"), ("port", "enable", "c"), ("port", "remove", "c")):
                rc, out, err = self.d.fnetctl(*args)
                self.assertEqual(rc, 0, err)
                time.sleep(0.2)
            rc, out, _ = self.d.ctl_json("ports")
            self.assertEqual([p["name"] for p in out["data"]["ports"]], ["a", "b"])
        finally:
            stop.set()
            t.join(5)
            pc.close()
        self.assertEqual(errors, [])
        rc, st, _ = self.d.ctl_json("stats", "--port", "a")
        p = st["data"]["stats"]["ports"][0]
        self.assertEqual(p["xfer_timeouts"] + p["cksum_err_tx"], 0)


class Control(TwoPorts):
    def setUp(self):
        super().setUp()
        self.d.start()

    def test_070_json_and_exit_codes(self):
        """TEST-070: every query command returns valid JSON; usage/connection errors exit 2."""
        for args in (("status",), ("version",), ("ports",), ("sessions",), ("drives", "a"),
                     ("stats",), ("tape", "show", "a")):
            rc, out, err = self.d.ctl_json(*args)
            self.assertEqual(rc, 0, (args, err))
            self.assertTrue(out["ok"])
            rc, text, err = self.d.fnetctl(*args)
            self.assertEqual(rc, 0)
            self.assertTrue(text)
        rc, _, _ = self.d.fnetctl("bogus")
        self.assertEqual(rc, 2)
        rc, _, _ = self.d.fnetctl("drives", "nope")
        self.assertEqual(rc, 1)
        r = subprocess.run([FNETCTL, "--socket", "/nonexistent/ctl", "status"], capture_output=True)
        self.assertEqual(r.returncode, 2)

    def test_071_busy_and_wait(self):
        """TEST-071: mount refused mid-transaction; --wait succeeds afterwards."""
        self.pa.send(b"r\x00\x01\x01")                  # start a write, hold it open
        time.sleep(0.1)
        rc, out, err = self.d.fnetctl("mount", "a", "0", "/A.DSK")
        self.assertEqual(rc, 1)
        self.assertIn("busy", err)
        p = subprocess.Popen([FNETCTL, "--socket", self.d.ctl, "mount", "a", "0", "/A.DSK", "--wait=3"],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(0.2)
        data = bytes(256)
        self.pa.send(data + bytes([0, 0]))
        self.assertEqual(self.pa.recv(1), bytes([0x15]))   # no image mounted yet -> NAK
        out, err = p.communicate(timeout=5)
        self.assertEqual(p.returncode, 0, err)
        self.assertEqual(self.ca.drive_name(0), b"A.DSK")
        with open(self.d.conf) as f:
            before = f.read()
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        free = s.getsockname()[1]
        s.close()
        r = subprocess.run([FNETCTL, "--socket", self.d.ctl, "port", "add", "x", "type=listen",
                            "bind=127.0.0.1", f"port={free}", "allow=127.0.0.1"],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("not written to the config file", r.stderr)
        self.assertIn("not persistent", r.stdout)
        with open(self.d.conf) as f:
            self.assertEqual(f.read(), before)

    def test_072_socket_perms(self):
        """TEST-072: control socket is 0660; state changes log the peer uid/pid."""
        mode = stat.S_IMODE(os.stat(self.d.ctl).st_mode)
        self.assertEqual(mode, 0o660)
        self.d.fnetctl("mount", "a", "1", "/A.DSK")
        self.assertTrue(self.d.wait_log(f"mount from uid {os.getuid()} pid"))

    def test_073_per_port_level(self):
        """TEST-073: per-port trace while the global level is warn."""
        self.assertEqual(self.d.fnetctl("log", "level", "warn")[0], 0)
        self.assertEqual(self.d.fnetctl("log", "level", "--port", "a", "trace")[0], 0)
        mark = len(self.d.read_log())
        self.ca.q()
        self.cb.q()
        time.sleep(0.2)
        new = self.d.read_log()[mark:]
        self.assertIn("[a] wire: rx", new)
        self.assertNotIn("[b] wire", new)

    def test_075_stats(self):
        """TEST-075: counters in stats.json; reset zeroes them."""
        self.assertEqual(self.ca.mount("A"), b"\x06W")
        for _ in range(100):
            self.ca.read(0, 0, 3)
        self.assertEqual(self.ca.write(0, 1, 1, bytes(256), ck=7), b"\x15")
        rc, _, err = self.d.fnetctl("stats", "--flush")
        self.assertEqual(rc, 0, err)
        with open(self.d.stats) as f:
            st = json.load(f)
        a = [p for p in st["ports"] if p["name"] == "a"][0]
        self.assertEqual(a["sectors_read"], 100)
        self.assertEqual(a["cksum_err_rx"], 1)
        self.assertEqual(a["mounts"], [{"drive": 0, "image": "/A.DSK", "rw": True}])
        self.d.fnetctl("stats", "--reset", "--port", "a")
        rc, out, _ = self.d.ctl_json("stats", "--port", "a")
        self.assertEqual(out["data"]["stats"]["ports"][0]["sectors_read"], 0)

    def test_075_atomic(self):
        """TEST-075: the stats file is never seen partially written."""
        end = time.monotonic() + 2.5
        seen = 0
        while time.monotonic() < end:
            try:
                with open(self.d.stats) as f:
                    json.load(f)
                seen += 1
            except FileNotFoundError:
                pass
        self.assertGreater(seen, 0)

    def test_035_two_ports_one_image(self):
        """TEST-035: second port mounting the same image gets R."""
        self.assertEqual(self.ca.mount("A"), b"\x06W")
        self.assertEqual(self.cb.mount("A"), b"\x06R")


class FileSink(TwoPorts):
    def setUp(self):
        super().setUp()
        self.sink = os.path.join(self.d.dir, "sink.log")
        self.d.daemon_extra = f"log_sink = file\nlog_file = {self.sink}"
        self.d.write_conf()
        self.d.start(foreground=False, level="debug")

    def test_074_reopen(self):
        """TEST-074: logrotate-style move + reopen."""
        self.assertTrue(os.path.exists(self.sink))
        os.rename(self.sink, self.sink + ".1")
        self.assertEqual(self.d.fnetctl("log", "reopen")[0], 0)
        self.d.fnetctl("reload")
        time.sleep(0.2)
        with open(self.sink) as f:
            self.assertIn("configuration reloaded", f.read())


if __name__ == "__main__":
    unittest.main()
