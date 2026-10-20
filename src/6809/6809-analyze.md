# FlexNet 6809 sources: analysis for an MC6800 port

This covers the FLEX09 client side of FlexNet: the resident `FNETDRV` driver and
the `R*`/`F*` utilities in this directory. It is broken down so each piece can be
re-coded for FLEX 6800 on an MC6800. References use `file:line`.

**Contents**

1. [Inventory](#1-inventory)
2. [Architecture](#2-architecture)
3. [The shared driver block (the ABI)](#3-the-shared-driver-block-the-abi)
4. [fnetdrv.asm, section by section](#4-fnetdrvasm-section-by-section)
5. [Utilities, section by section](#5-utilities-section-by-section)
6. [Wire protocol as used, and fnetd behaviour](#6-wire-protocol-as-used-and-fnetd-behaviour)
7. [FLEX09 to FLEX 6800 equates](#7-flex09-to-flex-6800-equates)
8. [6809 to 6800 instruction translation](#8-6809-to-6800-instruction-translation)
9. [Porting hazards, by location](#9-porting-hazards-by-location)
10. [Defects in the 6809 sources](#10-defects-in-the-6809-sources)
11. [Recommended port plan](#11-recommended-port-plan)
12. [Open questions](#12-open-questions)

---

## 1. Inventory

| File | Builds | Role | FlexNet command(s) | Lines | 6800 effort |
|---|---|---|---|---|---|
| `fnetdrv.asm` | `FNETDRV.CMD` | Resident disk driver plus a one-time installer | `$55 $AA ? Q s r` | 733 | **High.** Position-independent code, Y, D, PCR |
| `fmount.asm` | `FMOUNT.CMD` | Mount an image on drive 0–3, or show the mount map | `Q m d` | 376 | Medium |
| `fcreate.asm` | `FCREATE.CMD` | Prompt for and create an image, optionally mounting it | `C` / `c` | 392 | Medium |
| `fdrive.asm` | `FDRIVE.CMD` | RDRIVE for FNETDRV: change the host "drive", or show cwd | `V ?` | 252 | Low |
| `rdrive.asm` | `RDRIVE.CMD` | The same for the older NETDRV (different block offset) | `V ?` | 254 | Low (likely drop) |
| `fexit.asm` | `FEXIT.CMD` | REXIT for FNETDRV: end the session and uninstall the driver | `E` | 181 | Medium |
| `rexit.asm` | `REXIT.CMD` | The same for NETDRV (`size` at another offset) | `E` | 181 | Medium (likely drop) |
| `rcd.asm` | `RCD.CMD` | Change the host directory | `P` | 166 | Low |
| `rdir.asm` | `RDIR.CMD` | List `.DSK` files, 20 lines per page | `A` + SP/ESC | 208 | Low |
| `rlist.asm` | `RLIST.CMD` | List directories, 20 lines per page | `I` + SP/ESC | 207 | Low |
| `rdelete.asm` | `RDELETE.CMD` | Delete an image after a Y/N prompt | `? D` | 273 | Low |
| `resync.asm` | `RESYNC.CMD` | Reset the ACIA and redo the sync handshake | `$55 $AA` | 134 | Low |
| `acia.asm` | include | ACIA addresses (system-specific) | — | 16 | Trivial: change the addresses |
| `flexequ.asm` | include | FLEX09 equates and ASCII constants | — | 34 | Trivial: new values (§7) |
| `cdelay.asm` | `CDELAY.CMD` | `INDEC`, then stores X at `$BFEB` (a system patch) | — | 5 | Unknown (§12) |
| `memdump.asm` | `MEMDUMP.CMD` | Generic paged hex dump, not FlexNet | — | 82 | Low |
| `README.md` | — | NETPC user documentation (RCD, RDIR, …) | — | 176 | — |

Missing: there is no `RMOUNT` (legacy `M`) source here. `FMOUNT` covers it.

## 2. Architecture

```
            FLEX09 memory                               host (fnetd)
 $C100 ┌──────────────────────┐ installer, run once
       │ FNETDRV installer    │──sync $55/$AA, '?'────────►
       └──────────────────────┘
 $DE00 ┌──────────────────────┐ FLEX driver jump table (7 of 10 entries patched)
       │ JMP nread  ...       │──┐
       └──────────────────────┘  │
 MEMEND+1 ┌───────────────────┐  │ resident package (relocated copy of org $0000)
       │ fread..fquick (JMPs) │◄─┘ original FLEX entries, copied from $DE00
       │ 'netUUdrv' signature │◄── utilities find this by scanning MEMEND..$C000
       │ LBRA schar/rchar/reset
       │ 'FLEXNet 4.1.0'      │
       │ drvltr netdrv[4] ... │◄── utilities patch netdrv[] and drvltr
       │ nread nwrite ...     │──'s'/'r' sector traffic──►
       └──────────────────────┘
 $C100 ┌──────────────────────┐ each utility, run on demand
       │ FMOUNT / RDIR / ...  │──JMP to schar/rchar in the package──►
       └──────────────────────┘
```

- **Installer** (`fnetdrv.asm:496–733`, assembled at `$C100`):
  1. Refuses to load twice (signature scan).
  2. Parses the drive numbers.
  3. Resets the ACIA and syncs.
  4. Asks `?`.
  5. Copies FLEX's driver jump table into the package.
  6. Moves the package below `MEMEND`, patches `$DE01…`, and lowers `MEMEND`.
- **Resident package** (`fnetdrv.asm:40–466`, assembled at `$0000`): it must be
  position-independent, because it runs wherever `MEMEND` puts it. The 6809 gets
  this for free from `,pcr` addressing, `lbsr`/`lbra` and `leax ...,pcr`.
- **Utilities**: each scans for the signature, copies the `schar`/`rchar`
  addresses into its own `JMP` stubs, then talks FlexNet directly through the
  resident serial routines. They never touch the ACIA themselves.

## 3. The shared driver block (the ABI)

Every utility depends on these offsets (`fnetdrv.asm:43–83`). A 6800 port must
keep the same layout so that one set of utilities works with the driver, or else
change the driver and all utilities together.

| Offset from block start | From `sgnst` | Size | Field | Used by |
|---|---|---|---|---|
| `+0` | −21 | 21 | `fread … fquick`: copies of FLEX's 7 `JMP`s | driver (fall-through), `fexit` (restore) |
| `+21` | 0 | 8 | `'netUUdrv'` signature | every utility (scan) |
| `+29` | +8 | 3 | `LBRA schar` | every utility |
| `+32` | +11 | 3 | `LBRA rchar` | every utility |
| `+35` | +14 | 3 | `LBRA reset` | `resync` |
| `+38` | +17 | 13 | `'FLEXNet 4.1.0'` (FNETDRV marker) | `fmount`, `fcreate` (detect) |
| `+51` | +30 | 4 | `drvltr` (only byte 0 used) | `fdrive` (`drvptr`), `dlyset` |
| `+55` | +34 | 4 | `netdrv[0..3]`: `d` = drive d is remote, `$FF` = local | driver, `fmount`, `fcreate` |
| `+59` | +38 | 1 | `qcheck` (send `Q` before every sector) | driver |
| `+60` | +39 | 1 | `slowpc` (insert delays) | driver |
| `+61` | +40 | 2 | `size` (= `drvend`) | `fexit` (`ldd 29,x` with `x = sgnst+11`) |

Notes:
- **6800 vectors.** On the 6800 the three `LBRA`s become `JMP abs` (also 3 bytes,
  opcode `$7E`). Their operands then become **relocation entries** (§11).
- **Older NETDRV.** NETDRV, which has no `'FLEXNet 4.1.0'` string, has `drvltr` at
  `sgnst+17` and `size` at `sgnst+19`. That is why `rdrive.asm`/`rexit.asm` exist
  next to `fdrive.asm`/`fexit.asm`. A 6800 port has no installed base, so ship only
  the F-variants.

## 4. fnetdrv.asm, section by section

### 4.1 Header block and locals (`:40–95`)
- `fread…fquick` are `rmb 3` each. The installer fills them with FLEX's original
  `JMP xxxx` (opcode `$7E` on both CPUs).
- **Locals:**
  - `curdrv`
  - `curtrk` (track and sector)
  - `chksum` (16-bit; also the last error status for `nverfy`)
  - `cnt`
  - `lstdrv`
  - `delcnt`
  - `odelc` (outer timeout)
- `tmp`/`tries` reuse `lstdrv`/`cnt` during install (`:486–487`).

### 4.2 `nread`: READ entry (`:99–194`)
FLEX contract: **X** = sector buffer (FCB+64), **A** = track, **B** = sector.
On exit, **B** = status and **Z** = 1 on success.
1. Get the drive with `lda -64+3,x` (FCB+3) and look it up in `netdrv[]`. If it is
   not remote, go to `fread`.
2. If `qcheck` is set, send `Q` and expect ACK. If `slowpc` is set, `delay`.
3. Send `s`, drive, track, sector.
4. Receive 256 bytes into the buffer, adding each to a 16-bit sum.
5. Receive the checksum, MSB first, and compare.
6. Reply ACK and return B=0. On a mismatch reply NAK and return B=9; on a timeout
   return B=16.
7. B is saved in `chksum` (for `nverfy`), then `tstb` sets Z.

### 4.3 `nwrite`: WRITE entry (`:198–293`)
Mirror image of `nread`: send `r` drv trk sec, 256 bytes, then the checksum MSB
and LSB, and expect ACK. Returns 0, 10 (NAK) or 16 (timeout). Records `lstdrv`
for VERIFY.

### 4.4 `nverfy`, `nrestr`, `ndrsel`, `ncheck`, `nquick` (`:297–387`)
- `nverfy` returns the stored status of the last write if `lstdrv` is remote.
- `nrestr` and `ndrsel` return B=0 for remote drives.
- `ncheck` and `nquick` send `Q` and expect ACK. They return B=0, or B=16 with C set.
- All of them use `3,x` (X = FCB) to get the drive. Non-remote drives `lbne` to
  the saved FLEX entry.

### 4.5 `rchar` / `schar` (`:393–432`)
Polled ACIA I/O with a two-level timeout: the inner count is `delcnt` (256) and
the outer is `odelc`, held in Y. **C = 1** means success. On timeout C is still 0
from the last `asrb`, which is an implicit contract (see §9). B is destroyed and
Y is preserved.
- `rchar`: RDRF is status bit 0 (`asrb` puts it in C).
- `schar`: TDRE is bit 1 (`asrb; asrb`).

### 4.6 `delay`, `reset`, `dlyset` (`:436–463`)
- `delay`: about 12.8 k iterations on the stack byte. Only used when `slowpc` is set.
- `reset`: writes `$03` to the ACIA (master reset), then `$15` (8N1, ÷16, RTS low).
- `dlyset`: runs **on every character**. It sets `odelc` to 100, or to 65535 when
  `drvltr == '@'` (the host is on floppy A:). This means the installer's
  `odelc = $4000` (`:587`) is dead: it gets overwritten.

### 4.7 Installer (`:496–721`)
| Step | Lines | What it does | 6800 notes |
|---|---|---|---|
| `newtbl` | 500–506 | 7 package-relative entry offsets | Become absolute addresses that are relocated |
| `init` | 518–529 | Clears DP; prints the version | 6800 has no DP: drop it |
| `search` | 534–550 | Scans `MEMEND+1..$BFFF` for the 8-byte signature using `b,x`/`b,y` | Use two pointers in memory; the bound becomes `$A000` |
| drive args | 555–575 | `gethex` loop: `netdrv[d] = d` for each drive given | Needs a 16-bit add to index `netdrv` |
| `nonum` | 577–583 | `LIB ACIA`, then `jsr reset` | See defect D1 |
| sync | 594–626 | Up to 5 tries of `$55`, then `$AA`; each must echo | Direct port |
| where | 631–667 | Sends `?`; takes the first char to make `drvltr`; prints it; discards bytes up to ACK | Direct port |
| `movtbl` | 671–678 | Copies 21 bytes from `$DE00` to the package at `$0000` | `$BE80` on 6800 (`DRVTBL` equate); one pointer plus a temp |
| `movpkg` | 685–695 | `MEMEND -= drvend`; copies the package down | Must also apply relocations |
| `movadr` | 699–709 | Patches `$DE01+3n` with `newtbl[n] + MEMEND` | `ADDD` becomes an add with carry |
| `memend` | 711–713 | `MEMEND -= 1` | — |
| report | 715–721 | Prints the first drive in `netdrv` (`lda netdrv` prints entry 0, D3) | — |

## 5. Utilities, section by section

### 5.1 Common prologue (repeated in every utility)
1. `search`: the same signature scan as the installer. Not found means
   "NETDRV is not loaded".
2. `leax len,x; stx schar+1; leax 3,x; stx rchar+1`. This points the local `JMP`
   stubs at the package's `LBRA` vectors.
3. *(FMOUNT, FCREATE)* Compare `sgnst+17` with `'FLEXNet 4.1.0'`. If they match,
   set `fnprsnt=1` and `ndarray = sgnst+34`.
4. *(FDRIVE)* Set `drvptr = sgnst+30`, which is `drvltr`.
5. *(RESYNC)* Also set `reset+1 = sgnst+14`.

**Port as one shared include.** In the port this is `src/6800/fnutil.inc`. Its
`netfind` finds the signature, fills the three stubs and `ndarray`/`drvptr`, and
sets C=1 when found. That removes about 30 duplicated lines from each of 10
programs. The include also holds the command line helpers (`argcnt`, `sndarg`)
and `pdata`. RDIR and RLIST share their pager through `fnlist.inc`.

### 5.2 Command-line handling idioms
- `TTYLBP`/`NXTCH` give raw access to the line buffer.
- `TTYEOL` is saved and cleared, so that `\`, `:` and `;` in host paths are not
  treated as FLEX command separators. It is restored on every exit path.
- Utilities count the characters to the CR (`chrcnt`) before sending anything.
- Messages use `PSTRNG` (EOT-terminated) or a local `PDATA` (NUL-terminated).

### 5.3 Per-utility behaviour
| Utility | Sends | Expects | Notes for the port |
|---|---|---|---|
| `fmount` | `Q`, then `m` drv(0–3, binary) name `CR` | ACK then `W`/`R`, or NAK | `.ext` is replaced by CR, so only `NAME` is sent and the host adds `.DSK`. With no argument, `d` drv for each mapped drive shows the map. With one character (`FMOUNT 2`), the drive is unmapped **locally only**. |
| `fcreate` | `C`/`c` [drv] path`CR` name`CR` vol`CR` trk`CR` sec`CR` | ACK/NAK (up to 255 `rchar` timeouts) | Builds a NUL-terminated buffer, then streams it (see defects D3–D5). Uses `[INCHNE]` (indirect) for unechoed input. |
| `fdrive` | `V` text `CR` | ACK/NAK | Sets `drvltr` from the first character: `'A'` becomes `'@'`, which means floppy and long timeouts. With no argument it sends `?` and prints the cwd. |
| `rcd` | `P` path `CR` | ACK/NAK | — |
| `rdir` | `A` pattern `CR` | `CR LF`, then a SP for each line, then ACK | 20 lines per page. ESC (from the user) is sent through; then it waits for ACK. |
| `rlist` | `I CR SP` | as RDIR | The extra SP starts the listing (fnetd `FN_LIST_FIRST_SP`). |
| `rdelete` | `?` (for display only), then `D` name`.DSK` `CR` | ACK/NAK | Always appends `.DSK`, so typing `X.DSK` sends `X.DSK.DSK`. The display joins the cwd and name with `\`. |
| `fexit` | `E` | ACK | Restores the 21 bytes to `$DE00`, zero-fills the package, and raises `MEMEND` only if the package sits directly above it. |
| `resync` | ACIA reset, then `$55`/`$AA` | echo | Same algorithm as the installer sync. |

## 6. Wire protocol as used, and fnetd behaviour

Every exchange is byte-at-a-time and host-driven, and the target always speaks
first. fnetd (`src/fnetd/flexnet.c`) is compatible with all of the above. The
things a port should know:

- **Path syntax.** `?` returns a virtual path such as `/` or `/sub`, and fnetd
  paths use `/`. The `\` that RDELETE displays is cosmetic.
- **Timeouts.** `drvltr` is computed from the first character of the `?` reply:
  `'/'-1 & $5E = $0E`. That is never `'@'`, so fnetd always gets the short
  timeout. This is correct, because fnetd answers in microseconds.
- **`V` (RDRIVE)** is ACKed and ignored. There are no DOS drives.
- **`slowpc`.** It defaults to **1** in the driver (`:77`), which adds a `delay`
  after every command byte and slows things down for nothing. Ship the 6800 port
  with `slowpc = 0` and `qcheck = 0`.
- **Failed read.** A failed read comes back as 256 zero bytes plus checksum
  `$0001`. The driver NAKs it and returns status 9, so FLEX reports a read error.
- **Unmapped drive.** `FMOUNT n` unmaps only on the target; fnetd keeps the image
  open until REXIT, a remount or session loss. This is harmless.
- **Session end.** fnetd ends a session after `sync_loss_count` (3) transaction
  timeouts or `sync_loss_garbage` (16) junk bytes. A 6800 client that is slow
  between bytes must stay under `xfer_timeout_ms` (2000 ms) per transaction. At
  1 MHz this is not a concern.
- **Reference traces.** `fnetmock -C host:port -vv -e '…'` prints the exact byte
  sequences fnetd expects for every command above. Use these as reference traces
  when debugging the 6800 code on an emulator.

## 7. FLEX09 to FLEX 6800 equates

FLEX 6800 sits `$2000` below FLEX09 for the system area, the DOS entry points
and the FMS. **Check every value against your FLEX 6800 adaptation**, especially
the driver table and the console vectors.

| Symbol | FLEX09 | FLEX 6800 | Notes |
|---|---|---|---|
| Line buffer | `$C080` | `$A080` | |
| Utility command space | `$C100` | `$A100` | `ORG` for every utility and the installer |
| Scan upper bound | `$C000` | `$A000` | `cpx #$c000` in every `search` |
| `TTYEOL` | `$CC02` | `$AC02` | |
| `LSTTRM` | `$CC11` | `$AC11` | |
| `TTYLBP` (line buffer pointer) | `$CC14` | `$AC14` | |
| `MEMEND` | `$CC2B` | `$AC2B` | |
| Input echo flag (`memdump`) | `$CC2F` | `$AC2F` | |
| `WARMS` | `$CD03` | `$AD03` | |
| `GETCHR` / `PUTCHR` / `INBUF` | `$CD15/18/1B` | `$AD15/18/1B` | |
| `PSTRNG` / `CLASS` / `PCRLF` / `NXTCH` | `$CD1E/21/24/27` | `$AD1E/21/24/27` | |
| `GETFIL` / `OUTDEC` / `OUTHEX` | `$CD2D/39/3C` | `$AD2D/39/3C` | |
| `GETHEX` / `OUTADR` / `INDEC` | `$CD42/45/48` | `$AD42/45/48` | |
| `FMSCLS` / `FMS` | `$D403/$D406` | `$B403/$B406` | Not used by FlexNet |
| `INCHNE` vector | `$D3E5` | *not used* | FCREATE calls `GETCHR` (`$AD15`) with the echo flag (`$AC2F`) cleared, then restores the flag, as `memdump.asm:66–74` does (§12.5) |
| Driver jump table | `$DE00` | `$BE80` (`t/dskdrv.lst`) | READ/WRITE/VERIFY/RESTORE/DRIVE/CHKRDY/QUICK at +0…+18, then INIT/WARM/SEEK. Make it a `DRVTBL` equate |
| `PRCNT` (spooler task running) | `$CC34` | `$AC34` | Used by `dskdrv` to suspend the spooler (§9.11) |
| `$BFEB` (`cdelay`) | system patch | inside the 6800 driver area | See §12 |
| ACIA | `$E000+16·port` | SWTPC MP-S: `$8000+4·port` | Control = base, data = base+1. Use the **second** 6850. |

## 8. 6809 to 6800 instruction translation

The 6800 has A, B, X, SP and CC only. Its indexed mode is `n,X` with an unsigned
offset of 0–255. It has no PC-relative addressing, no long branches, no Y/U/DP/D
and no `PSHX`. (The 6801/6803 adds `LDD/STD/ADDD/SUBD/ABX/PSHX/PULX/MUL`, which
shrinks most of this table.)

| 6809 (as used here) | 6800 replacement | Where |
|---|---|---|
| `lda label,pcr` / `sta label,pcr` | `ldaa label` / `staa label` (extended; **relocated**) | the whole driver |
| `leax label,pcr` | `ldx #label` (**relocated**) | `:102`, `:202`, … |
| `lbsr x` / `lbra x` / `lbne x` | `jsr x` / `jmp x` / `beq *+5` + `jmp x` | everywhere |
| `lda ,x+` / `sta ,x+` | `ldaa 0,x` + `inx` | sector loops, `movtbl`, `movpkg` |
| `lda -64+3,x` | Not needed: take the drive from `curdrv`, saved by the DRIVE entry (§12.1) | `nread`/`nwrite` |
| `leax a,x` / `ldaa b,x` | an add to the low byte with carry into the high byte through a temp | `netdrv` lookup, `search` |
| `abx` | `stx t` + `addb t+1` + `stab t+1` + `ldab t` + `adcb #0` + `stab t` + `ldx t` | `fmount:115` |
| `ldd`/`std`/`addd`/`subd`/`cmpd` | A:B pairs: `ldaa hi`+`ldab lo`; `addb lo`+`adca hi`; compare A then B | `memend` math, checksum compare |
| `ldy #n` / `leay -1,y` (counter) | `ldx #n` / `dex` (save X first) or a 16-bit counter in memory | `rchar`, `schar`, `dlyset` |
| `ldy #ptr` / `cmpa b,y` (second pointer) | a second pointer in memory, swapped with `ldx` | `search` |
| `pshs a,x` / `puls x,a` | `psha` + `stx save` / `ldx save` + `pula` | the drive-check preamble |
| `tfr a,b` / `tfr x,d` / `tfr d,x` | `tab` / `stx t` + `ldaa t` + `ldab t+1` / `staa t` + `stab t+1` + `ldx t` | `nread:170`, installer |
| `clr a` / `dec a` / `cmp a #` (spaced syntax) | `clra` / `deca` / `cmpa #` | utilities |
| `sta [drvptr]` / `jsr [INCHNE]` | `ldx drvptr` + `staa 0,x` / `GETCHR` with echo off | `fdrive:151`, `fcreate:261` |
| `cpx #n` + `blo` | `cpx` sets **only Z reliably** on the 6800: use `bne` loops or compare the bytes | `movpkg:694` |
| `clr a; tfr a,dp` | delete | installer `:518` |

## 9. Porting hazards, by location

1. **Direct-page sizing (critical).** The package is assembled at `org $0000`
   (`fnetdrv.asm:40`). A 6800 assembler will use **direct** (2-byte) addressing
   for every label below `$100`. After relocation those instructions would still
   point at page 0. Either assemble the package at a non-zero-page origin, or
   force extended addressing (`>label`, depending on the assembler).
2. **Flags across restores.** `nread`/`nwrite`/… do `cmpa 0,x`, then
   `puls x,a`, then `bne`. `PULS` keeps the flags, but the 6800 replacement
   `ldx save` changes Z. Branch first and restore after, or recompute.
3. **The carry contract in `rchar`/`schar`.** On timeout, C=0 is left over from
   `asrb` because `leay`/`dec` don't touch C. On the 6800, `dex`/`dec` don't touch
   C either, but end both paths with an explicit `clc`/`sec` so the contract
   doesn't depend on that.
4. **Timing retune.** The polling loop cycle counts differ: 6809 indexed-PCR is
   slower than 6800 extended. Retune `odelc` for about 0.5–1 s per character at
   your clock, and drop the floppy (`'@'`) special case: fnetd is never slow.
5. **Negative index offsets.** `-64+3,x` (FCB drive from the buffer pointer)
   cannot be encoded. Don't port it. As in `t/dskdrv.lst`, save the drive in the
   DRIVE/RESTORE/CHKRDY/QUICK entries, which get X = FCB, and use that saved value
   in READ/WRITE (§12.1).
6. **Relocation.** Every absolute reference into the package must be fixed after
   the copy. That includes `jmp fread…`, `ldx #netdrv`, all local variables, and
   the three vector `JMP`s. See §11 for the two-origin method.
7. **The 6800 `cpx` compare** (`movpkg:694` uses `blo` after `cmpy`) must become
   an equality loop.
8. **`dlyset` clobbers Y** (and so X in a 6800 port) on every character. Keep the
   sector buffer pointer in memory, not in X, around `rchar`/`schar`, or make the
   serial routines save X.
9. **Stack use.** `delay` uses `dec 0,s`. The 6800 needs `tsx` + `dec 0,x`, and
   `tsx` points at SP+1.
10. **Signature scan cost.** Scanning about 40 KB a byte at a time with two memory
    pointers is slow on a 6800. First check `MEMEND+1+21`, where the installer
    normally puts the signature, then fall back to the scan.
11. **Spooler and interrupts.** FNETDRV doesn't handle the print spooler.
    `t/dskdrv.lst:135–138` does:
    - If `PRCNT` (`$AC34`) is non-zero, the call came from the spooler task, so
      it executes `swi` to yield first.
    - It holds `sei` for the whole serial transfer and does `cli` on every exit.

    Do the same in `nread`/`nwrite` for remote drives. An IRQ in the middle of
    a sector loses characters, because the 6850 has only one byte of buffer.

## 10. Defects in the 6809 sources

Fix these while porting rather than carrying them across.

| # | Where | Defect | Effect |
|---|---|---|---|
| D1 | `acia.asm:1` / `fnetdrv.asm:581` | `acia.asm` begins with `NAM ACIA` and is pulled in with `LIB` in the middle of the installer code. Some assemblers reject a second `NAM`. Keep includes to equates only. | Assembler-dependent build failure |
| D2 | `fnetdrv.asm:587–588`, `:457–462` | `odelc = $4000` is always overwritten by `dlyset` (100 or 65535) | The tuning knob is dead; the timeout is fixed |
| D3 | `fcreate.asm:115`, `:123`, `:223` | `cmpb 0` compares with **memory $0000**, not immediate 0 | `fnprsnt` tests read garbage |
| D4 | `fcreate.asm:385` vs `fnetdrv.asm:67` | It looks for `'FLEXNet 4.0.1'` but the driver has `'FLEXNet 4.1.0'` | FNETDRV is never detected, so it always sends `C`, never `c` |
| D5 | `fcreate.asm:135–141`, `:193–194` | The parsed drive is never stored in `drv`. A binary drive **0** in the buffer ends the NUL-terminated send. | `c 0 …` is truncated; the mapping always uses drive 3 |
| D6 | `fcreate.asm:304–307` | Digit check `< $40` accepts `: ; < = > ?` | Non-digits are passed as numbers |
| D7 | `fmount.asm:169–177`, `:203–209` | `drv = char & 3` is always ≤ 3, so the "no drive given → 3" branch (`bgt`) is dead. With no drive, the first letter of the name is sent as the drive. | `FMOUNT NAME` mounts on a random drive with a truncated name |
| D8 | `rdelete.asm:139–146` | Always appends `.DSK` | `RDELETE X.DSK` fails |
| D9 | `fexit.asm:424` / `rexit.asm:424` | `restor` message has no EOT | It prints "MEMEND restored" and then "Program Exit" |
| D10 | `fdrive.asm` vs `rdrive.asm:364` | `drvptr = sgnst+17` in RDRIVE points at the `'F'` of `'FLEXNet 4.1.0'` | RDRIVE on FNETDRV corrupts the marker, so FMOUNT/FCREATE stop detecting FNETDRV |
| D11 | `fnetdrv.asm:717` | Reports `netdrv[0]`, which is `$FF` unless D0 is mapped | Prints the wrong drive number |
| D12 | `rexit.asm:87` | `ldd 8,x` reads `size` at the old NETDRV offset. Used on FNETDRV, it reads string bytes as the size. | Zero-fills the wrong amount of memory |
| D13 | `fcreate.asm:50`, `flexequ.asm:8` | `memend` and `MEMEND` are both defined | Assembler-dependent duplicate symbol |

## 11. Recommended port plan

1. **Includes first.**
   - `flex68.inc`: the §7 values.
   - `acia68.inc`: equates only, for the second 6850.
   - `fnutil.inc`: the §5.1 prologue as a subroutine (`netfind`).
2. **Pick a relocation strategy for the driver:**
   - **(a) Fixed origin (fastest bring-up).** Assemble the package at a set address
     below the top of RAM (for example `ORG $9C00`). The installer only checks
     `MEMEND ≥ ORG+size-1`, copies the jump table, patches `$BE01…` and sets
     `MEMEND = ORG-1`. No relocation code at all.
   - **(b) Relocatable (keeps "load anywhere").** Assemble the package twice, at
     `$1000` and `$1100`. A host script diffs the two binaries: every byte that
     differs by exactly 1 is the high byte of an absolute address. Emit that as a
     bitmap or offset table appended to the installer. Then copy the package to a
     **page-aligned** base (`(MEMEND+1-size) & $FF00`) and add
     `(base_hi - $10)` to each flagged byte. This costs less than 256 bytes of
     slack and needs no low-byte fixups.
3. **Driver core.** Port `rchar`/`schar`/`reset` and get `RESYNC` working against
   fnetd (§6), which proves the ACIA and timing. Then port `nread`, then `nwrite`,
   then the four trivial entries.
4. **Utilities, simplest first:** `rcd`, `rdir`/`rlist`, `fdrive`, `fmount`,
   `rdelete`, `fcreate`, `fexit`. Drop `rdrive`/`rexit` (NETDRV-only).
   `memdump` is independent.
5. **Testing.** Run a 6800/SWTPC emulator whose second ACIA is a pty, and point an
   fnetd `type = tty` port at it.
   - Use `fnetctl log level --port P trace` for a hex dump of every byte.
   - Use `fnetmock -vv` traces as the expected sequences.
   - Use `fnetimg check` afterwards to confirm that writes produced a consistent
     image.

```asm
* Example: 6800 rchar (C=1/A=char on success, C=0 on timeout; X preserved, B destroyed)
rchar   stx     rcx             save caller's X
        ldx     odelc           outer count
        clr     delcnt          inner count = 256
rcha04  ldab    aciac
        asrb                    RDRF -> C
        bcs     rcha08
        dec     delcnt
        bne     rcha04
        dex
        bne     rcha04
        ldx     rcx
        clc                     time-out
        rts
rcha08  ldaa    aciad
        ldx     rcx
        sec
        rts

* Example: drive lookup, A = drive (0-3). Returns Z=1 if remote. Destroys B and X.
isnet   ldx     #netdrv         (relocated)
        tab
isn02   tstb
        beq     isn04
        inx
        decb
        bra     isn02
isn04   cmpa    0,x             netdrv[d] == d ?
        rts
```

## 12. Open questions

Answers recorded 2026-10-19.

1. **FLEX 6800 driver table.** *Resolved: model it on `t/dskdrv.lst`.* That
   listing is the FLEX 3.0 serial-disk driver for the SWTPC 6800 (MP-S in slot 2).
   - **Address.** The jump table is at **`$BE80`**, not `$BE00`. The driver
     fills `$BE80–$BFF9`. Put the address in a `DRVTBL` equate.
   - **Order.** It has 10 `JMP`s in the same order as FLEX09: READ `+0`,
     WRITE `+3`, VERIFY `+6`, RESTORE `+9`, DRIVE `+12`, CHKRDY `+15`,
     QUICK `+18`, INIT `+21`, WARM `+24`, SEEK `+27`. FNETDRV patches only the
     first 7, so `movtbl` and `fexit` still move 21 bytes.
   - **Register contract.**
     - READ/WRITE: X = buffer, A = track, B = sector.
     - DRIVE/RESTORE/CHKRDY/QUICK: X = FCB, with the drive at `3,x`.
     - Return B = status and Z = 1 on success (`clrb`). `dskdrv` returns
       B = `$08` (CRC error) on any failure.
   - **Finding the drive.** `dskdrv` never looks in the FCB from READ/WRITE.
     RESTORE, DRIVE, CHKRDY and QUICK all go to `select`, which saves
     `3,x & 3` in `reqDrv`, and READ/WRITE use `reqDrv`
     (`t/dskdrv.lst:178–180`). FLEX 3.0 always selects the drive before it
     reads or writes. The 6800 FNETDRV does the same:
     - `ndrsel`, `nrestr`, `ncheck` and `nquick` store `3,x` in `curdrv`
       before the `netdrv[]` lookup.
     - `nread`/`nwrite` use `curdrv`.
     - This removes the `-64+3,x` / `-61` arithmetic (§8, §9.5) and doesn't
       touch the §3 block.
   - **Spooler.** Copy the `PRCNT`/`swi` and `sei`/`cli` handling (§9.11).
   - **Chaining.** The installer copies the table at `DRVTBL`, whatever driver
     put it there (`dskdrv` or a floppy driver). Local drives then fall through
     to that driver unchanged.
2. **`cdelay.asm`.** *Resolved: ignore it.* It is not ported.
3. **6801/6803.** *Resolved: 6800 only.* Write no conditional code for the 6801.
4. **Interoperability.** *Resolved: yes.* Keep the §3 driver block layout
   byte-for-byte. Keep `netdrv[0..3]` as four bytes; do not reduce it to a
   bit mask.
5. **Console input.** *Resolved: use `GETCHR` with the echo flag cleared.*
   FCREATE (`fcreate.asm:261`, `jsr [INCHNE]`) does this for each character,
   following `memdump.asm:66–74`:
   1. Save `$AC2F` (the echo flag).
   2. Clear it.
   3. `jsr GETCHR` (`$AD15`).
   4. Put the saved flag back without changing A (`psha`/`pula`).

   Restore the flag on every path out, including ESC and errors, or the
   console stays unechoed after FCREATE exits.
