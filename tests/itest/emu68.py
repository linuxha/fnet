"""Minimal MC6800 + FLEX 6800 DOS-call emulator for testing the FlexNet utilities.

The ACIA at $8008/$8009 is bridged to a file descriptor (a pty master that
fnetd's tty port is attached to). FLEX DOS entry points are trapped and done
in Python; registers the FLEX manual does not promise to keep are clobbered.
"""
import os
import select

H, I, N, Z, V, C = 0x20, 0x10, 0x08, 0x04, 0x02, 0x01

WARMS, GETCHR, PUTCHR, PSTRNG, PCRLF, NXTCH = 0xAD03, 0xAD15, 0xAD18, 0xAD1E, 0xAD24, 0xAD27
OUTHEX, GETHEX, OUTADR = 0xAD3C, 0xAD42, 0xAD45
TTYEOL, LSTTRM, TTYLBP, MEMEND, ECHOFL, PRCNT = 0xAC02, 0xAC11, 0xAC14, 0xAC2B, 0xAC2F, 0xAC34
LINBUF, DRVTBL = 0xA080, 0xBE80
ACIAC = 0x8008
JUNK = 0x5A


class Halt(Exception):
    pass


class CPU:
    def __init__(self, fd):
        self.m = bytearray(65536)
        self.a = self.b = 0
        self.x = 0
        self.sp = 0xA07F
        self.pc = 0
        self.cc = 0xD0
        self.fd = fd
        self.rx = bytearray()
        self.out = bytearray()
        self.keys = bytearray()
        self.steps = 0
        self.local_calls = []
        self.m[TTYEOL] = ord(":")
        self.m[ECHOFL] = 0xFF
        self.w16(MEMEND, 0x7FFF)
        # FLEX driver table: 10 JMPs to trap stubs at $BF00 + 3n
        for n in range(10):
            self.m[DRVTBL + 3 * n] = 0x7E
            self.w16(DRVTBL + 3 * n + 1, 0xBF00 + 3 * n)

    # -- memory
    def r8(self, a):
        a &= 0xFFFF
        if a == ACIAC:
            if not self.rx:
                r, _, _ = select.select([self.fd], [], [], 0.00002)
                if r:
                    self.rx += os.read(self.fd, 4096)
            return 0x02 | (1 if self.rx else 0)
        if a == ACIAC + 1:
            if self.rx:
                v = self.rx[0]
                del self.rx[0]
                return v
            return 0
        return self.m[a]

    def w8(self, a, v):
        a &= 0xFFFF
        v &= 0xFF
        if a == ACIAC + 1:
            os.write(self.fd, bytes([v]))
            return
        if a == ACIAC:
            return
        if 0xAD00 <= a < 0xAE00 or 0xE000 <= a:
            raise Halt(f"write to ROM/DOS ${a:04X} at pc ${self.pc:04X}")
        self.m[a] = v

    def r16(self, a):
        return self.r8(a) << 8 | self.r8(a + 1)

    def w16(self, a, v):
        self.m[a & 0xFFFF] = (v >> 8) & 0xFF
        self.m[(a + 1) & 0xFFFF] = v & 0xFF

    # -- stack
    def push(self, v):
        self.m[self.sp] = v & 0xFF
        self.sp = (self.sp - 1) & 0xFFFF

    def pull(self):
        self.sp = (self.sp + 1) & 0xFFFF
        return self.m[self.sp]

    def rts(self):
        hi = self.pull()
        self.pc = hi << 8 | self.pull()

    # -- flags
    def f(self, flag, cond):
        if cond:
            self.cc |= flag
        else:
            self.cc &= ~flag

    def nz(self, r):
        self.f(N, r & 0x80)
        self.f(Z, (r & 0xFF) == 0)

    def nz16(self, r):
        self.f(N, r & 0x8000)
        self.f(Z, (r & 0xFFFF) == 0)

    def add(self, a, b, c=0):
        r = a + b + c
        self.f(H, ((a & 15) + (b & 15) + c) > 15)
        self.nz(r)
        self.f(V, (~(a ^ b) & (a ^ r)) & 0x80)
        self.f(C, r > 0xFF)
        return r & 0xFF

    def sub(self, a, b, c=0):
        r = a - b - c
        self.nz(r)
        self.f(V, ((a ^ b) & (a ^ r)) & 0x80)
        self.f(C, r < 0)
        return r & 0xFF

    def logic(self, r):
        self.nz(r)
        self.f(V, 0)
        return r & 0xFF

    # -- read-modify-write ops, index = low nibble of 4x/5x/6x/7x
    def rmw(self, op, v):
        c = self.cc & C
        if op == 0x0:      # NEG
            r = (-v) & 0xFF
            self.nz(r); self.f(V, r == 0x80); self.f(C, r != 0)
        elif op == 0x3:    # COM
            r = (~v) & 0xFF
            self.nz(r); self.f(V, 0); self.f(C, 1)
        elif op == 0x4:    # LSR
            r = v >> 1
            self.f(C, v & 1); self.nz(r)
        elif op == 0x6:    # ROR
            r = (v >> 1) | (c << 7)
            self.f(C, v & 1); self.nz(r)
        elif op == 0x7:    # ASR
            r = (v >> 1) | (v & 0x80)
            self.f(C, v & 1); self.nz(r)
        elif op == 0x8:    # ASL
            r = (v << 1) & 0xFF
            self.f(C, v & 0x80); self.nz(r)
        elif op == 0x9:    # ROL
            r = ((v << 1) | c) & 0xFF
            self.f(C, v & 0x80); self.nz(r)
        elif op == 0xA:    # DEC
            r = (v - 1) & 0xFF
            self.nz(r); self.f(V, r == 0x7F)
            return r
        elif op == 0xC:    # INC
            r = (v + 1) & 0xFF
            self.nz(r); self.f(V, r == 0x80)
            return r
        elif op == 0xD:    # TST
            self.nz(v); self.f(V, 0); self.f(C, 0)
            return None
        elif op == 0xF:    # CLR
            self.nz(0); self.f(V, 0); self.f(C, 0)
            return 0
        else:
            raise Halt(f"bad rmw op {op:X} at ${self.pc:04X}")
        if op in (0x4, 0x6, 0x7, 0x8, 0x9):
            self.f(V, bool(self.cc & N) != bool(self.cc & C))
        return r

    def fetch(self):
        v = self.m[self.pc]
        self.pc = (self.pc + 1) & 0xFFFF
        return v

    def fetch16(self):
        hi = self.fetch()
        return hi << 8 | self.fetch()

    def branch(self, op):
        off = self.fetch()
        if off & 0x80:
            off -= 256
        n, z, v, c = (bool(self.cc & k) for k in (N, Z, V, C))
        take = {
            0x0: True, 0x1: False, 0x2: not (c or z), 0x3: c or z,
            0x4: not c, 0x5: c, 0x6: not z, 0x7: z, 0x8: not v, 0x9: v,
            0xA: not n, 0xB: n, 0xC: n == v, 0xD: n != v,
            0xE: not z and n == v, 0xF: z or n != v,
        }[op & 15]
        if take:
            self.pc = (self.pc + off) & 0xFFFF

    def ea(self, mode):
        if mode == 1:
            return self.fetch()
        if mode == 2:
            return (self.x + self.fetch()) & 0xFFFF
        return self.fetch16()

    # -- one instruction
    def step(self):
        if self.pc in TRAPS:
            TRAPS[self.pc](self)
            return
        if 0xBF00 <= self.pc < 0xBF1E:
            self.local_calls.append((self.pc - 0xBF00) // 3)
            self.b = 0
            self.f(Z, 1)
            self.f(C, 0)
            self.rts()
            return
        if self.pc < 0x100 or 0xAD00 <= self.pc < 0xAE00:
            raise Halt(f"jump into ${self.pc:04X}")
        self.steps += 1
        op = self.fetch()
        hi, lo = op >> 4, op & 15

        if op == 0x01: pass
        elif op == 0x06: self.cc = self.a | 0xC0
        elif op == 0x07: self.a = self.cc | 0xC0
        elif op == 0x08: self.x = (self.x + 1) & 0xFFFF; self.f(Z, self.x == 0)
        elif op == 0x09: self.x = (self.x - 1) & 0xFFFF; self.f(Z, self.x == 0)
        elif op == 0x0A: self.f(V, 0)
        elif op == 0x0B: self.f(V, 1)
        elif op == 0x0C: self.f(C, 0)
        elif op == 0x0D: self.f(C, 1)
        elif op == 0x0E: self.f(I, 0)
        elif op == 0x0F: self.f(I, 1)
        elif op == 0x10: self.a = self.sub(self.a, self.b)
        elif op == 0x11: self.sub(self.a, self.b)
        elif op == 0x16: self.b = self.logic(self.a)
        elif op == 0x17: self.a = self.logic(self.b)
        elif op == 0x1B: self.a = self.add(self.a, self.b)
        elif hi == 0x2: self.branch(op)
        elif op == 0x30: self.x = (self.sp + 1) & 0xFFFF
        elif op == 0x31: self.sp = (self.sp + 1) & 0xFFFF
        elif op == 0x32: self.a = self.pull()
        elif op == 0x33: self.b = self.pull()
        elif op == 0x34: self.sp = (self.sp - 1) & 0xFFFF
        elif op == 0x35: self.sp = (self.x - 1) & 0xFFFF
        elif op == 0x36: self.push(self.a)
        elif op == 0x37: self.push(self.b)
        elif op == 0x39: self.rts()
        elif op == 0x3F: raise Halt(f"SWI at ${self.pc - 1:04X}")
        elif hi == 0x4:
            r = self.rmw(lo, self.a)
            if r is not None: self.a = r
        elif hi == 0x5:
            r = self.rmw(lo, self.b)
            if r is not None: self.b = r
        elif hi in (0x6, 0x7):
            a = self.ea(2 if hi == 6 else 3)
            if lo == 0xE:
                self.pc = a
            else:
                r = self.rmw(lo, self.r8(a))
                if r is not None: self.w8(a, r)
        elif hi >= 0x8:
            mode = hi & 3            # 0 imm, 1 dir, 2 idx, 3 ext
            isb = hi >= 0xC
            if op == 0x8D:           # BSR
                off = self.fetch()
                off = off - 256 if off & 0x80 else off
                self.push(self.pc & 0xFF); self.push(self.pc >> 8)
                self.pc = (self.pc + off) & 0xFFFF
                return
            if lo == 0xD and not isb and mode in (2, 3):   # JSR
                a = self.ea(mode)
                self.push(self.pc & 0xFF); self.push(self.pc >> 8)
                self.pc = a
                return
            if lo in (0xC, 0xE, 0xF):       # CPX, LDS/LDX, STS/STX
                if mode == 0:
                    v = self.fetch16(); a = None
                else:
                    a = self.ea(mode); v = None
                if lo == 0xC and not isb:
                    m = v if a is None else self.r16(a)
                    r = self.x - m
                    self.nz16(r)
                    self.f(V, ((self.x ^ m) & (self.x ^ r)) & 0x8000)
                elif lo == 0xE:
                    m = v if a is None else self.r16(a)
                    if isb: self.x = m
                    else: self.sp = m
                    self.nz16(m); self.f(V, 0)
                elif lo == 0xF:
                    m = self.x if isb else self.sp
                    self.w8(a, m >> 8); self.w8(a + 1, m)
                    self.nz16(m); self.f(V, 0)
                else:
                    raise Halt(f"bad op ${op:02X} at ${self.pc:04X}")
                return
            if lo == 0x7:            # STA
                a = self.ea(mode)
                r = self.b if isb else self.a
                self.w8(a, r); self.logic(r)
                return
            m = self.fetch() if mode == 0 else self.r8(self.ea(mode))
            acc = self.b if isb else self.a
            c = 1 if self.cc & C else 0
            if lo == 0x0: r = self.sub(acc, m)
            elif lo == 0x1: self.sub(acc, m); r = None
            elif lo == 0x2: r = self.sub(acc, m, c)
            elif lo == 0x4: r = self.logic(acc & m)
            elif lo == 0x5: self.logic(acc & m); r = None
            elif lo == 0x6: r = self.logic(m)
            elif lo == 0x8: r = self.logic(acc ^ m)
            elif lo == 0x9: r = self.add(acc, m, c)
            elif lo == 0xA: r = self.logic(acc | m)
            elif lo == 0xB: r = self.add(acc, m)
            else:
                raise Halt(f"bad op ${op:02X} at ${self.pc:04X}")
            if r is not None:
                if isb: self.b = r
                else: self.a = r
        else:
            raise Halt(f"bad op ${op:02X} at ${(self.pc - 1) & 0xFFFF:04X}")

    def call(self, addr, limit=5_000_000):
        """JSR addr and run until it returns."""
        self.push(0x00); self.push(0x01)      # return to $0100 = sentinel
        self.pc = addr
        while self.pc != 0x0100:
            self.step()
            if self.steps > limit:
                raise Halt("step limit")

    # -- FLEX
    def load_cmd(self, path):
        with open(path, "rb") as f:
            data = f.read()
        i, entry = 0, None
        while i < len(data):
            t = data[i]
            if t == 0x02:
                a = data[i + 1] << 8 | data[i + 2]
                n = data[i + 3]
                self.m[a:a + n] = data[i + 4:i + 4 + n]
                i += 4 + n
            elif t == 0x16:
                entry = data[i + 1] << 8 | data[i + 2]
                i += 3
            elif t == 0:
                i += 1
            else:
                raise ValueError(f"bad record {t:#x}")
        return entry

    def run(self, path, line, keys=b"", limit=20_000_000):
        """Run a FLEX command line such as 'RCD /sub'. Returns the console output."""
        entry = self.load_cmd(path)
        text = (line + "\r").encode()
        self.m[LINBUF:LINBUF + len(text)] = text
        p = text.find(b" ")
        if p < 0:
            p = text.find(b"\r")
            self.m[LSTTRM] = 0x0D
            self.w16(TTYLBP, LINBUF + p)
        else:
            self.m[LSTTRM] = 0x20
            self.w16(TTYLBP, LINBUF + p + 1)
        self.keys = bytearray(keys)
        self.out = bytearray()
        self.sp = 0xA07F
        self.pc = entry
        self.steps = 0
        try:
            while True:
                self.step()
                if self.steps > limit:
                    raise Halt("step limit")
        except StopIteration:
            pass
        return self.out.decode("latin-1")


# ---------------------------------------------------------------- DOS traps
def _ret(cpu, keep):
    """Clobber registers not in keep, then RTS."""
    if "a" not in keep: cpu.a = JUNK
    if "b" not in keep: cpu.b = JUNK
    if "x" not in keep: cpu.x = 0xDEAD
    cpu.rts()


def t_warms(cpu):
    raise StopIteration


def t_putchr(cpu):
    cpu.out.append(cpu.a)
    _ret(cpu, "bx")


def t_getchr(cpu):
    if not cpu.keys:
        raise Halt("GETCHR with no keys left; output so far:\n" + cpu.out.decode("latin-1"))
    cpu.a = cpu.keys.pop(0)
    if cpu.m[ECHOFL]:
        cpu.out.append(cpu.a)
    _ret(cpu, "abx")


def t_pstrng(cpu):
    cpu.out += b"\r\n"
    x = cpu.x
    while cpu.m[x] != 4:
        cpu.out.append(cpu.m[x])
        x += 1
    _ret(cpu, "b")


def t_pcrlf(cpu):
    cpu.out += b"\r\n"
    _ret(cpu, "x")


def nxtch(cpu):
    p = cpu.r16(TTYLBP)
    ch = cpu.m[p]
    if ch != 0x0D and ch != cpu.m[TTYEOL]:
        p += 1
        if ch == 0x20:
            while cpu.m[p] == 0x20:
                p += 1
    cpu.w16(TTYLBP, p)
    return ch


def t_nxtch(cpu):
    cpu.a = nxtch(cpu)
    alnum = chr(cpu.a).isalnum()
    cpu.f(C, not alnum)
    if not alnum:
        cpu.m[LSTTRM] = cpu.a
    _ret(cpu, "abx")


def t_gethex(cpu):
    v, n, bad = 0, 0, False
    while True:
        ch = nxtch(cpu)
        c = chr(ch)
        if not c.isalnum():
            cpu.m[LSTTRM] = ch
            break
        if c.upper() in "0123456789ABCDEF":
            v = (v * 16 + int(c, 16)) & 0xFFFF
            n += 1
        else:
            bad = True
    cpu.x = v
    cpu.b = 1 if n else 0
    cpu.f(C, bad)
    _ret(cpu, "bx")


def t_outhex(cpu):
    cpu.out += b"%02X" % cpu.m[cpu.x]
    _ret(cpu, "bx")


def t_outadr(cpu):
    cpu.out += b"%04X" % cpu.r16(cpu.x)
    _ret(cpu, "")


TRAPS = {WARMS: t_warms, PUTCHR: t_putchr, GETCHR: t_getchr, PSTRNG: t_pstrng,
         PCRLF: t_pcrlf, NXTCH: t_nxtch, GETHEX: t_gethex, OUTHEX: t_outhex,
         OUTADR: t_outadr}
