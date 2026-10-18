"""Integration test helpers: run fnetd, emulate a FLEX client, mock an RFC-2217 server."""

import json
import os
import select
import shutil
import socket
import subprocess
import tempfile
import termios
import threading
import time
import tty
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BUILD = os.environ.get("FNET_BUILD", os.path.join(ROOT, "build"))
FNETD = os.path.join(BUILD, "fnetd")
FNETCTL = os.path.join(BUILD, "fnetctl")
FNETIMG = os.path.join(BUILD, "fnetimg")

ACK, NAK, CR, LF, ESC, SP = 0x06, 0x15, 0x0D, 0x0A, 0x1B, 0x20
DC1, DC2, DC3, DC4 = 0x11, 0x12, 0x13, 0x14


def cksum(data):
    return sum(data) & 0xFFFF


class Wire:
    """Byte stream with exact reads and timeouts (pty master or decoded socket)."""

    def __init__(self, fd):
        self.fd = fd
        self.buf = bytearray()

    def _fill(self, timeout):
        r, _, _ = select.select([self.fd], [], [], timeout)
        if not r:
            return False
        try:
            data = os.read(self.fd, 4096)
        except OSError:
            return False
        if not data:
            return False
        self.buf += self.decode(data)
        return True

    def decode(self, data):
        return data

    def send(self, data):
        if isinstance(data, int):
            data = bytes([data])
        elif isinstance(data, str):
            data = data.encode("latin-1")
        os.write(self.fd, self.encode(data))

    def encode(self, data):
        return data

    def recv(self, n, timeout=2.0):
        end = time.monotonic() + timeout
        while len(self.buf) < n:
            left = end - time.monotonic()
            if left <= 0 or not self._fill(left):
                if time.monotonic() >= end:
                    break
        out = bytes(self.buf[:n])
        del self.buf[:n]
        return out

    def recv_until(self, term, timeout=2.0):
        end = time.monotonic() + timeout
        while term not in self.buf:
            left = end - time.monotonic()
            if left <= 0:
                break
            self._fill(left)
        i = self.buf.find(term)
        if i < 0:
            out = bytes(self.buf)
            self.buf.clear()
            return out
        out = bytes(self.buf[: i + len(term)])
        del self.buf[: i + len(term)]
        return out

    def drain(self, timeout=0.3):
        """Return everything that arrives within timeout."""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self._fill(max(0.0, end - time.monotonic()))
        out = bytes(self.buf)
        self.buf.clear()
        return out


class Pty(Wire):
    def __init__(self):
        self.master, self.slave = os.openpty()
        tty.setraw(self.slave)
        self.path = os.ttyname(self.slave)
        super().__init__(self.master)

    def close(self):
        for fd in (self.master, self.slave):
            try:
                os.close(fd)
            except OSError:
                pass


class FlexClient:
    """Speaks the NETDRV/FNETDRV side of the FlexNet protocol byte-for-byte."""

    def __init__(self, wire):
        self.w = wire

    def sync(self):
        self.w.send(0x55)
        a = self.w.recv(1)
        self.w.send(0xAA)
        b = self.w.recv(1)
        return a + b

    def q(self):
        self.w.send(b"Q")
        return self.w.recv(1)

    def where(self):
        self.w.send(b"?")
        return self.w.recv_until(bytes([ACK]))

    def text_cmd(self, cmd, *fields, drv=None):
        msg = cmd.encode()
        if drv is not None:
            msg += bytes([drv])
        for f in fields:
            msg += f.encode("latin-1") + b"\r"
        self.w.send(msg)

    def mount(self, name, drv=None):
        """M (legacy) or m (drv given). Returns b'\\x06W', b'\\x06R' or b'\\x15'."""
        self.text_cmd("m" if drv is not None else "M", name, drv=drv)
        first = self.w.recv(1)
        if first == bytes([ACK]):
            return first + self.w.recv(1)
        return first

    def drive_name(self, drv):
        self.w.send(b"d" + bytes([drv]))
        return self.w.recv_until(bytes([ACK]))[:-1]

    def cd(self, path):
        self.text_cmd("P", path)
        return self.w.recv(1)

    def read(self, drv, trk, sec, reply=ACK):
        self.w.send(b"s" + bytes([drv, trk, sec]))
        r = self.w.recv(258)
        if reply is not None:
            self.w.send(reply)
        return r[:256], (r[256] << 8 | r[257]) if len(r) == 258 else None, r

    def write(self, drv, trk, sec, data, ck=None):
        if ck is None:
            ck = cksum(data)
        self.w.send(b"r" + bytes([drv, trk, sec]) + bytes(data) + bytes([ck >> 8, ck & 0xFF]))
        return self.w.recv(1)

    def listing(self, cmd, pattern=""):
        """RDIR ('A') or RLIST ('I'). Returns list of names."""
        if cmd == "A":
            self.text_cmd("A", pattern)
        else:
            self.w.send(b"I\r ")
        head = self.w.recv(2)
        assert head == b"\r\n", head
        names = []
        while True:
            self.w.send(SP)
            first = self.w.recv(1)
            if first == bytes([ACK]):
                return names
            line = first + self.w.recv_until(b"\r\n")
            names.append(line[:-2].decode("latin-1"))

    def create(self, path, name, vol, trk, sec, drv=None):
        self.text_cmd("c" if drv is not None else "C", path, name, str(vol), str(trk), str(sec), drv=drv)
        return self.w.recv(1, timeout=5)

    def delete(self, name):
        self.text_cmd("D", name)
        return self.w.recv(1)

    def exit(self):
        self.w.send(b"E")
        return self.w.recv(1)


class Daemon:
    """Runs fnetd in a private sandbox."""

    def __init__(self, ports=None, extra_defaults="", daemon_extra="", tape_dirs=("6800", "6809", "MIKBUG")):
        self.dir = tempfile.mkdtemp(prefix="fnet-itest-")
        self.flex = os.path.join(self.dir, "flex")
        self.tape = os.path.join(self.dir, "tape")
        self.run = os.path.join(self.dir, "run")
        os.makedirs(self.flex)
        os.makedirs(self.run)
        for d in tape_dirs:
            os.makedirs(os.path.join(self.tape, d))
        self.ctl = os.path.join(self.run, "ctl")
        self.stats = os.path.join(self.run, "stats.json")
        self.conf = os.path.join(self.dir, "fnetd.conf")
        self.log = os.path.join(self.dir, "fnetd.log")
        self.extra_defaults = extra_defaults
        self.daemon_extra = daemon_extra
        self.ports = ports or {}
        self.proc = None
        self.write_conf()

    def write_conf(self, ports=None):
        if ports is not None:
            self.ports = ports
        lines = [
            "[daemon]",
            "user =",
            f"ctl_socket = {self.ctl}",
            f"stats_file = {self.stats}",
            "stats_interval = 1",
            "log_level = debug",
            self.daemon_extra,
            "[defaults]",
            f"flexnet_root = {self.flex}",
            f"tape_root = {self.tape}",
            "xfer_timeout_ms = 1000",
            self.extra_defaults,
        ]
        for name, kv in self.ports.items():
            lines.append(f"[port.{name}]")
            for k, v in kv.items():
                lines.append(f"{k} = {v}")
        with open(self.conf, "w") as f:
            f.write("\n".join(lines) + "\n")

    def start(self, wait_ports=True, foreground=True, env=None, level="trace"):
        self.logf = open(self.log, "ab")
        args = [FNETD, "-c", self.conf] + (["-f"] if foreground else []) + (["-l", level] if level else [])
        self.proc = subprocess.Popen(args, stdout=self.logf, stderr=self.logf,
                                     env=dict(os.environ, **(env or {})))
        end = time.monotonic() + 5
        while not os.path.exists(self.ctl):
            if self.proc.poll() is not None:
                raise RuntimeError("fnetd exited: " + self.read_log())
            if time.monotonic() > end:
                raise RuntimeError("fnetd did not start: " + self.read_log())
            time.sleep(0.02)
        if wait_ports:
            self.wait_links()
        return self

    def wait_links(self, want=("up", "listening", "disabled"), timeout=5):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            rc, out, _ = self.ctl_json("ports")
            if rc == 0 and all(p["link"] in want for p in out["data"]["ports"]):
                return
            time.sleep(0.05)
        raise RuntimeError("ports not up: " + str(self.ctl_json("ports")))

    def fnetctl(self, *args, timeout=10):
        r = subprocess.run([FNETCTL, "--socket", self.ctl, *args], capture_output=True, text=True,
                           timeout=timeout)
        return r.returncode, r.stdout, r.stderr

    def ctl_json(self, *args):
        rc, out, err = self.fnetctl("--json", *args) if args and args[0] != "--json" else self.fnetctl(*args)
        try:
            return rc, json.loads(out), err
        except ValueError:
            return rc, None, err + out

    def read_log(self):
        try:
            with open(self.log, "rb") as f:
                return f.read().decode("latin-1")
        except OSError:
            return ""

    def wait_log(self, text, timeout=3):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if text in self.read_log():
                return True
            time.sleep(0.05)
        return False

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        if self.proc:
            self.logf.close()
        rc = self.proc.returncode if self.proc else None
        self.proc = None
        return rc

    def cleanup(self):
        self.stop()
        if os.environ.get("FNET_KEEP"):
            print("kept", self.dir)
        else:
            shutil.rmtree(self.dir, ignore_errors=True)

    def mkimg(self, rel, tracks=40, sectors=18, track0=None):
        path = os.path.join(self.flex, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        cmd = [FNETIMG, "create", path, "--tracks", str(tracks), "--sectors", str(sectors)]
        if track0:
            cmd += ["--track0", str(track0)]
        subprocess.run(cmd, check=True)
        return path


class PtyTestCase(unittest.TestCase):
    """One fnetd with one tty port backed by a pty."""

    port_extra = {}
    defaults_extra = ""

    def setUp(self):
        self.pty = Pty()
        kv = {"type": "tty", "device": self.pty.path, "baud": "115200"}
        kv.update(self.port_extra)
        self.d = Daemon({"p1": kv}, extra_defaults=self.defaults_extra)
        self.prepare()
        self.d.start()
        self.c = FlexClient(self.pty)

    def prepare(self):
        pass

    def tearDown(self):
        rc = self.d.stop()
        if os.environ.get("FNET_SHOWLOG") or (hasattr(self, "_outcome") and not self._ok()):
            print(self.d.read_log()[-6000:])
        self.d.cleanup()
        self.pty.close()
        self.assertEqual(rc, 0, "fnetd did not exit cleanly")

    def _ok(self):
        res = getattr(self._outcome, "result", None)
        if res is None:
            return True
        return not any(t is self for t, _ in res.errors + res.failures)


# ---------------------------------------------------------------- RFC-2217 mock

IAC, SB, SE, WILL, WONT, DO, DONT = 255, 250, 240, 251, 252, 253, 254
COMPORT = 44


class TelnetPeer(Wire):
    """Terminal-server side of a Telnet/RFC-2217 connection."""

    def __init__(self, sock, refuse=(), raw=False):
        self.sock = sock
        self.raw = raw
        self.refuse = set(refuse)
        self.events = []          # ("WILL", opt) / ("SB", bytes)
        self.state = 0
        self.sb = bytearray()
        self.cmd = 0
        super().__init__(sock.fileno())

    def encode(self, data):
        return data if self.raw else data.replace(b"\xff", b"\xff\xff")

    def send_raw(self, data):
        self.sock.sendall(data)

    def send(self, data):
        if isinstance(data, int):
            data = bytes([data])
        self.sock.sendall(self.encode(data))

    def decode(self, data):
        if self.raw:
            return data
        out = bytearray()
        for b in data:
            if self.state == 0:
                if b == IAC:
                    self.state = 1
                else:
                    out.append(b)
            elif self.state == 1:
                if b == IAC:
                    out.append(IAC)
                    self.state = 0
                elif b in (WILL, WONT, DO, DONT):
                    self.cmd = b
                    self.state = 2
                elif b == SB:
                    self.sb = bytearray()
                    self.state = 3
                else:
                    self.state = 0
            elif self.state == 2:
                self.on_cmd(self.cmd, b)
                self.state = 0
            elif self.state == 3:
                if b == IAC:
                    self.state = 4
                else:
                    self.sb.append(b)
            elif self.state == 4:
                if b == IAC:
                    self.sb.append(IAC)
                    self.state = 3
                else:
                    self.on_sb(bytes(self.sb))
                    self.state = 0
        return bytes(out)

    def on_cmd(self, cmd, opt):
        name = {WILL: "WILL", WONT: "WONT", DO: "DO", DONT: "DONT"}[cmd]
        self.events.append((name, opt))
        if cmd == WILL:
            self.send_raw(bytes([IAC, DONT if opt in self.refuse else DO, opt]))
        elif cmd == DO:
            self.send_raw(bytes([IAC, WONT if opt in self.refuse else WILL, opt]))

    def on_sb(self, sb):
        self.events.append(("SB", sb))
        if sb and sb[0] == COMPORT and len(sb) >= 2 and COMPORT not in self.refuse:
            c = sb[1]
            if c == 5 and ("SET-CONTROL" in self.refuse):
                val = bytes([0])
            else:
                val = sb[2:]
            self.send_raw(bytes([IAC, SB, COMPORT, c + 100]) + val.replace(b"\xff", b"\xff\xff")
                          + bytes([IAC, SE]))

    def pump(self, timeout=0.3):
        """Process negotiation without consuming data."""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self._fill(max(0.0, end - time.monotonic()))

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


class MockTerminalServer:
    """Listens like a terminal server's RFC-2217 port (fnetd connects out)."""

    def __init__(self, **peer_kw):
        self.ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.ls.bind(("127.0.0.1", 0))
        self.ls.listen(4)
        self.port = self.ls.getsockname()[1]
        self.peer_kw = peer_kw

    def accept(self, timeout=10):
        self.ls.settimeout(timeout)
        s, _ = self.ls.accept()
        s.setblocking(True)
        return TelnetPeer(s, **self.peer_kw)

    def close(self):
        self.ls.close()


def connect_peer(host, port, **kw):
    """Terminal server dialing in to an fnetd listen port."""
    s = socket.create_connection((host, port), timeout=5)
    return TelnetPeer(s, **kw)
