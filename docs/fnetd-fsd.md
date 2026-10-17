# fnetd — Feature Specification Document

## Revision History

| Version | Date       | Author      | Description   |
|---------|------------|-------------|---------------|
| v0.0.1  | 2026-10-16 | Neil Cherry | Initial draft |
| v0.0.2  | 2026-10-17 | Neil Cherry | Resolved open questions 1–6: add network-listen (RFC-2217/raw) ports; legacy `M` maps to `legacy_drive` (no default slot); second writer → read-only; tape CPU tag = any processor directory; punch EOL normalised to LF. Moved to `docs/`. |

Versioning follows Semantic Versioning 2.0.0 for both this document and the
software. Requirement and test IDs are stable: they are never renumbered, and
new items take the next free number.

---

## 1. Project Overview

### 1.1 Purpose

`fnetd` is a Linux daemon, written in C, that serves FLEX disk images
(FlexNet / NetPC protocol) and Motorola S-Record "paper tape" files (load and
punch via ASCII device-control characters) to Motorola 6800/6809 systems. Each
target system connects through a dedicated 6850 ACIA, either wired to a local
Linux serial port or to a network terminal server that `fnetd` reaches via
RFC-2217. One daemon serves many target systems at once and is managed at
runtime with CLI tools, without restarts.

### 1.2 Scope

In scope for v0.0.1:

- `fnetd` daemon (systemd service).
- `fnetctl` runtime control CLI.
- `fnetimg` offline FLEX disk image utility.
- Transports: local tty; RFC-2217 to a terminal server, either with fnetd
  connecting out (`rfc2217` ports) or with the terminal server connecting in
  (`listen` ports); raw TCP as an option on either kind.
- FlexNet server, wire-compatible with the existing NETDRV/FNETDRV clients for
  6800 and 6809, including the FLEXNet 4.1 multi-drive commands `m`, `c`, `d`.
- Paper tape S-Record load (DC1/DC3) and punch (DC2/DC4), with file selection
  via the `|<CPU>:<file>|<CPU>:<file>` string.
- Logging with runtime-adjustable levels, plus a statistics file.

Out of scope:

- snet (DeRamp serial boot) protocol.
- Any "simplified" or new FlexNet protocol variant; only the existing wire
  protocol is implemented.
- FLEX 1.0 disk format.
- Client-side (6800/6809/6801/68HC11/68K) driver or utility changes.
- FujiNet hardware, non-FLEX operating systems.
- Authentication or encryption of serial/terminal-server links. Network
  access control is limited to bind address and peer allowlist (REQ-133).

### 1.3 Assumptions & Dependencies

- Each target system uses a **second** 6850 ACIA (not the console) for fnetd.
  TCP/IP is invisible to the 6850; it just sees an async serial line.
- A single port carries either FlexNet or paper tape traffic, **never both at
  the same time**. A tape operation runs to completion; a FlexNet session runs
  until REXIT (`E`) or loss of sync.
- Link framing is 8N1 with no hardware or software flow control, and the link
  is 8-bit clean.
- The FLEX-side ACIA is polled and has a 1-byte receive buffer, so fnetd must
  never send a byte the target is not waiting for.
- Terminal servers support RFC-2217 (e.g. ser2net, Moxa, Lantronix, Digi),
  and either accept connections or connect out to a configured host. A server
  in raw TCP mode is also supported (REQ-135).
- Host: Debian 13 (Linux ≥ 6.1; `openat2()` requires ≥ 5.6), glibc, systemd.

### 1.4 Glossary

| Term | Definition |
|------|------------|
| ACIA | MC6850 Asynchronous Communications Interface Adapter |
| ACK / NAK | `$06` / `$15`, positive / negative acknowledge |
| DC1–DC4 | ASCII `$11`–`$14`, tape reader/punch control (Table 2.6-1) |
| DSK image | Raw FLEX disk image: 256-byte sectors, track-major order |
| FlexNet | Serial remote-disk protocol of NETDRV/FNETDRV and the R*/F* utilities |
| FCB | FLEX File Control Block |
| MIKBUG | Motorola 6800 ROM monitor |
| Port | One fnetd endpoint (a tty or one RFC-2217 connection) serving one target |
| RFC-2217 | Telnet Com Port Control Option: serial-port parameters over Telnet |
| S-Record | Motorola hex object format (S0/S1/S2/S3/S5/S7/S8/S9 records) |
| Service root | Directory tree that a port's FlexNet and tape paths are confined to |
| Session | Per-port protocol state (cwd, drive map, tape files/position) |
| SIR | FLEX System Information Record, track 0 sector 3 |
| T/S | Track/Sector address |

---

## 2. Functional Requirements

### 2.1 Daemon Lifecycle

> **REQ-001** — The system shall provide a daemon `fnetd` that runs as a systemd
> service (`Type=notify`) and also runs in the foreground with `-f` for debugging.

> **REQ-002** — `fnetd` shall read its configuration from `/etc/fnetd/fnetd.conf`
> (INI format), overridable with `-c <path>`.
> *Rationale: a single, human-editable source of truth for ports and paths.*

> **REQ-003** — `fnetd` shall refuse to start, with a diagnostic naming file and
> line, if the configuration is syntactically invalid.

> **REQ-004** — On `SIGHUP` or `fnetctl reload`, `fnetd` shall re-read the
> configuration and apply the differences. Ports whose settings did not change
> shall keep their sessions and connections.

> **REQ-005** — If a reloaded configuration is invalid, `fnetd` shall keep running
> with the previous configuration and report the error.

> **REQ-006** — On `SIGTERM`, `fnetd` shall finish any in-progress sector
> transaction, flush and close all images and tape files, and exit within 3 s.

> **REQ-007** — `fnetd` shall run as an unprivileged user (`fnet`, member of
> `dialout`) and shall not require root after startup.

> **REQ-008** — `fnetd` shall prevent a second instance from running on the same
> control socket and runtime directory (lock on `/run/fnetd/fnetd.lock`).

> **REQ-009** — `fnetd` shall use one single-threaded `epoll` event loop. No
> port's wire activity may block the servicing of another port for more than
> 10 ms.
> *Rationale: deterministic ordering and no locking. Local image I/O is pread/pwrite
> on 256-byte blocks and is short enough not to need threads.*

> **REQ-010** — A FLEX-side REXIT (`E`) shall end only that port's FlexNet
> session. It shall never terminate the daemon.

### 2.2 Transports and Ports

> **REQ-011** — `fnetd` shall support **local tty** ports: device path, baud
> rate (50–460800), 8N1, raw mode (`cfmakeraw`), with no RTS/CTS and no XON/XOFF.

> **REQ-012** — `fnetd` shall support **RFC-2217 client** ports. It connects to
> `host:port` on a terminal server, negotiates Telnet BINARY in both directions
> and SUPPRESS-GO-AHEAD, and sets baud, 8 data bits, no parity, 1 stop bit and
> no flow control via COM-PORT-OPTION.

> **REQ-013** — The RFC-2217 transport shall be 8-bit transparent: IAC (`$FF`)
> is doubled on transmit, and Telnet commands and subnegotiations are removed
> from the received data before the protocol layer sees it.

> **REQ-014** — If the terminal server rejects or ignores a COM-PORT-OPTION
> setting, `fnetd` shall log a warning and continue (the server port may be
> preset).

> **REQ-015** — When an RFC-2217 connection drops, `fnetd` shall reconnect with
> exponential backoff from 1 s up to 30 s, and keep session state (drive map,
> cwd, tape file selection) across the reconnect.

> **REQ-016** — When a local tty disappears (e.g. a USB adapter is unplugged),
> `fnetd` shall close the port, retry opening it every 2 s, and keep session state.

> **REQ-017** — `fnetd` shall serve at least 16 ports concurrently, each with an
> independent session.

> **REQ-018** — `fnetctl` shall be able to add, remove, enable, disable and
> restart individual ports at runtime without affecting other ports.

> **REQ-019** — Protocol handlers shall see each port only as a byte stream,
> through a common transport interface (§4.2.1), so that they do not depend on
> the transport type.

> **REQ-020** — `fnetd` shall open listening network sockets **only** for
> configured `listen` ports (REQ-130). With no `listen` ports configured, it
> shall have no listening sockets.

#### 2.2.1 Network-Listen Ports

> **REQ-130** — `fnetd` shall support `listen` ports that accept inbound TCP
> connections from a terminal server on a configured `bind` address and port.
> `bind` is mandatory, and there shall be no wildcard default.

> **REQ-131** — After accepting a connection, `fnetd` shall perform the same
> Telnet/RFC-2217 negotiation as `rfc2217` ports (REQ-012–REQ-014). `fnetd`
> always acts as the side that sets the serial parameters; the terminal server
> is the access server, whichever side opened the TCP connection.

> **REQ-132** — A `listen` port shall carry one connection at a time. When a
> new connection arrives while one is active, `fnetd` shall act per `on_new`:
> `replace` (default) closes the old connection and takes the new one;
> `refuse` closes the new one. Either case shall be logged at warn.
> *Rationale: a terminal server that reboots reconnects before the old TCP
> session times out. `refuse` would lock it out.*

> **REQ-133** — Each `listen` port shall have an `allow` list of peer
> addresses/CIDRs. Connections from other peers shall be closed before any data
> is read, and logged at warn. An empty or missing `allow` shall refuse all
> connections.

> **REQ-134** — All TCP connections (`rfc2217` and `listen`) shall enable TCP
> keepalive (idle 30 s, interval 10 s, count 3). Session state shall be kept
> across disconnects, as in REQ-015.

> **REQ-135** — `rfc2217` and `listen` ports should support `telnet = no`: raw
> TCP with no Telnet negotiation and no IAC processing, for terminal servers in
> raw mode. Serial parameters are then the terminal server's responsibility.

### 2.3 Port Dispatcher (Mode Selection)

Each port has one dispatcher that reads command bytes at "top of loop":

| Byte | Action |
|------|--------|
| `$55`, `$AA` | FlexNet sync echo |
| FlexNet command bytes (§2.4) | FlexNet transaction |
| `|` (`$7C`) | Tape filename string (§2.6) |
| DC1 `$11` | Tape load start/resume |
| DC2 `$12` | Tape punch start |
| DC3 `$13`, DC4 `$14` at top of loop | Ignored (no tape active), logged at debug |
| anything else | Discarded, logged at debug, counted |

> **REQ-021** — The dispatcher shall accept FlexNet commands at top of loop
> **without** a prior `$55`/`$AA` sync, so that a resident driver keeps working
> across a daemon or port restart.

> **REQ-022** — The dispatcher shall enter the TAPE_LOAD state on DC1 and the
> TAPE_PUNCH state on DC2. While in either state, FlexNet bytes shall not be
> interpreted.

> **REQ-023** — A FlexNet session shall begin at the first FlexNet command (or
> sync) and end on `E` or on sync loss (REQ-025). Ending a session unmounts all
> its drives and resets cwd to the port's service root.

> **REQ-024** — Inside a FlexNet transaction (between a command byte and its
> final parameter byte), if no byte arrives for `xfer_timeout` (default
> 2000 ms), the transaction shall be abandoned with no reply and the dispatcher
> shall return to top of loop. Any partial write shall be discarded.
> *Rationale: fixes `getparam()` hangs (flexnet-protocol.md §9, fnetsrv-protocol.md §7.2).*

> **REQ-025** — **Sync loss** shall be declared when `sync_loss_count` (default
> 3) consecutive REQ-024 timeouts or `sync_loss_garbage` (default 16)
> consecutive discarded bytes occur. Sync loss ends the FlexNet session (REQ-023).
> A later `$55`/`$AA` or a valid command starts a new session.

> **REQ-026** — `fnetd` shall never transmit a byte on a port except as a reply
> to a received command or during an active tape load.

### 2.4 FlexNet Protocol

Wire format per `t/flexnet-protocol.md` (§3–§7). The implementation shall be
byte-compatible with existing 6800 and 6809 NETDRV/FNETDRV clients.

#### 2.4.1 Sync and Status

> **REQ-030** — `$55` and `$AA` received at top of loop shall be echoed
> unchanged.

> **REQ-031** — `Q` shall always reply ACK (RMOUNT sends `Q` before any image
> is mounted).

> **REQ-032** — `?` shall reply with the session's current directory as a
> **virtual path** relative to the service root, starting with `/`, followed by
> CR and ACK. Host absolute paths shall never be revealed.

> **REQ-033** — `V <text> CR` shall reply ACK and otherwise be ignored.

#### 2.4.2 Sector Transfer

> **REQ-034** — `s`/`S` `drv trk sec` shall reply with 256 data bytes plus a
> 16-bit additive checksum (high byte first), then read the client's ACK/NAK.
> The ACK/NAK shall be logged and counted, and nothing shall be retransmitted.

> **REQ-035** — `r`/`R` `drv trk sec data[256] ck-hi ck-lo` shall always read
> all 258 payload bytes before replying. It shall reply ACK only after the
> checksum matches and the `pwrite()` of the full 256 bytes succeeds. Otherwise
> it shall reply NAK.

> **REQ-036** — A read with no image mapped to `drv`, an invalid T/S or an I/O
> error shall send exactly 256 zero bytes followed by checksum `$00 $01`
> (exactly 258 bytes), so that the client reports a CRC error.
> *Rationale: fixes defects 9.2 and 9.3 (no stray byte; errors are surfaced).*

> **REQ-037** — A write with no image mapped, an invalid T/S, a read-only image
> or an I/O error shall reply NAK.

> **REQ-038** — T/S → block mapping shall be:
> track 0: `sec` 1…`t0_spt` → block `sec-1`, and `sec` 0 → block 0;
> track `t` ≥ 1: `sec` 1…`spt` → `t0_spt + (t-1)·spt + (sec-1)`.
> Sectors on track 0 shall be bounds-checked against `t0_spt`, not `spt`.
> *Rationale: fixes defect 9.4.*

> **REQ-039** — The `drv` byte (0–3) shall select the image mapped to that
> drive in the session's drive map (REQ-041). A `drv` with no mapping is
> unmapped (REQ-036/REQ-037). There is no fallback image.

#### 2.4.3 Mount and Drive Map

> **REQ-040** — The legacy command `M <name> CR` shall mount `<name>.DSK` (then
> `<name>.dsk`) from the cwd onto the port's `legacy_drive` (config, 0–3,
> default 0, settable at runtime with `fnetctl`), exactly as `m legacy_drive
> <name> CR` would. `M` shall not affect other drives.
> *Rationale: old 6803/6809 RMOUNT carries no drive number. Set
> `legacy_drive` to the FLEX drive that the old NETDRV is installed on.*

> **REQ-041** — `m drv <name> CR` shall mount `<name>.DSK`/`.dsk` on drive
> `drv` (0–3) and reply ACK followed by `R` or `W`, or NAK on failure.

> **REQ-042** — A failed `M` or `m` shall leave the previous mapping for that
> drive or slot intact.
> *Rationale: fixes defect 9.5.*

> **REQ-043** — `d drv` shall reply with the name of the image mapped to `drv`,
> as raw bytes, then ACK. If nothing is mapped, it shall reply with just ACK.

> **REQ-044** — An image shall be mounted `R` if any of these hold: the file is
> not writable by `fnetd`, the port or the fnetctl mount is configured
> read-only, or another session holds the image's write lock (REQ-045).
> Otherwise it shall be mounted `W`.

> **REQ-045** — Image sharing shall use OFD byte-range locks (`F_OFD_SETLK`):
> many readers, one writer. A writer mount when other readers exist shall be
> allowed. A second writer shall be downgraded to `R`, with a warning logged.
> It shall not be refused.

#### 2.4.4 Directory and File Management

> **REQ-046** — `P <path> CR` shall change the session cwd. It shall reply ACK
> on success, or NAK if the target does not exist, is not a directory, or would
> resolve outside the service root (REQ-090).

> **REQ-047** — `A <pattern> CR` shall list the cwd's regular files whose
> extension is exactly `.DSK` (case-insensitive) and whose name starts with
> `<pattern>` (case-insensitive). The listing shall follow the SP/ESC/ACK
> paging handshake exactly.

> **REQ-048** — `A` should also accept `*` and `?` wildcards in `<pattern>`. A
> pattern without wildcards shall keep prefix-match semantics.

> **REQ-049** — `I CR SP` shall list the cwd's subdirectories (excluding `.`
> and `..`) with the same handshake.

> **REQ-050** — Listings shall be sorted in ASCII order, case-insensitive.

> **REQ-051** — `C path CR name CR vol CR trk CR sec CR` and `c drv path CR
> name CR vol CR trk CR sec CR` shall create a new formatted FLEX image
> (REQ-063) at `<cwd>/<path>/<name>.DSK`. They shall reply ACK on success, or
> NAK if the file exists, a parameter is out of range, or the path escapes the
> root. `c` shall also map the new image to `drv`.

> **REQ-052** — `D <name>.DSK CR` shall delete the image from the cwd and reply
> ACK, or NAK if it does not exist, is mounted by any session, or is
> write-protected.

> **REQ-053** — `E` shall reply ACK and end the FlexNet session (REQ-023).

> **REQ-054** — Unknown command bytes at top of loop shall be discarded with no
> reply (REQ-025 still applies).

> **REQ-055** — Text parameters shall be accepted up to 127 bytes. Longer input
> shall be read and discarded up to CR, and the command shall reply NAK (where
> the command has a reply).

> **REQ-056** — Each reply shall begin within 5 ms of receiving the last
> request byte, excluding transport latency.

### 2.5 FLEX Disk Images

> **REQ-060** — `fnetd` shall support raw FLEX `.DSK` images with 256-byte
> sectors, up to 256 tracks (0–255) × 255 sectors (1–255), i.e. ≤ 16,711,680
> bytes.

> **REQ-061** — Geometry shall come from the SIR (image offset `$200`):
> `endTrack` (SIR byte 22, abs `$226`) = max track and `endSector` (SIR byte 23,
> abs `$227`) = `spt`. Track 0's sector count `t0_spt` shall be derived as
> `file_size/256 − endTrack·spt`. It must satisfy 5 ≤ `t0_spt` ≤ `spt`;
> otherwise the image shall be rejected at mount, with a log message.
> *Rationale: supports DD disks with an SD track 0.*

> **REQ-062** — Image I/O shall use `pread`/`pwrite` on file descriptors (no
> stdio). Data shall be `fdatasync`ed on unmount, on session end, and at most
> every `sync_interval` (default 5 s) while dirty.
> *Rationale: fixes defect 9.6.*

> **REQ-063** — Image creation (REQ-051, `fnetimg create`) shall produce a valid
> empty FLEX volume:
> - boot sectors zeroed;
> - SIR holding the volume label, number, free-chain first/last T/S, free-sector
>   count, creation date, `endTrack`/`endSector`;
> - directory chain on track 0 from sector 5 to `t0_spt`;
> - free chain linking every sector on tracks 1…`endTrack`;
> - all multi-byte fields big-endian (structs per `docs/flexdsk.md`).

> **REQ-064** — Creation shall reject tracks < 2 or > 256, sectors < 5 or
> > 255, and volume numbers > 65535.

> **REQ-065** — `fnetd` shall not parse or modify directory or file contents
> during FlexNet serving. Images are opaque sector stores.

### 2.6 Paper Tape (S-Record)

Table 2.6-1 — Device control characters

| Oct | Dec | Hex | Char | Function    |
|-----|-----|-----|------|-------------|
| 021 | 17  | 11  | DC1  | Load Start  |
| 022 | 18  | 12  | DC2  | Punch Start |
| 023 | 19  | 13  | DC3  | Load Stop   |
| 024 | 20  | 14  | DC4  | Punch Stop  |

#### 2.6.1 File Selection

Syntax (received at top of loop):

```
|[<CPU>:]<load-file>[|[<CPU>:]<punch-file>]<term>
<term> ::= CR | LF | EOT ($04) | NUL ($00)
```

The target typically sends this string by putting it in memory and calling the
monitor's println routine with X pointing to it.

> **REQ-070** — The dispatcher shall parse the filename string from `|` up to
> `<term>`, with at most 127 bytes. On overflow it shall discard up to `<term>`
> and ignore the string. Bytes of the string shall not be echoed, and no reply
> shall be sent.

> **REQ-071** — `<CPU>` shall be any processor tag of 1–16 characters from
> `[A-Za-z0-9_-]` (e.g. `6800`, `6809`, `6801`, `68HC11`, `68K`, `MIKBUG`,
> `Z80`). It shall select `<tape_root>/<CPU>/`, matched case-insensitively
> against the existing directory names. The optional `[tape.cpu]` config map
> may alias one tag to another directory (e.g. `6811 = 68HC11`). A missing
> `<CPU>:` shall use the port's `default_cpu`. A tag with invalid characters,
> or one that resolves to no existing directory, shall cause the field to be
> rejected, with a log message.
> *Rationale: adding a processor means making a directory, not changing code.*

> **REQ-072** — An empty field shall leave the current selection for that
> direction unchanged. A field of a single `-` should clear it.

> **REQ-073** — File paths shall be confined to the selected CPU directory
> (REQ-090). Subdirectories are permitted.

> **REQ-074** — A new load-file selection shall reset the load position to the
> start of the file.

#### 2.6.2 Load (Reader)

> **REQ-075** — DC1 shall start transmitting the selected load file from the
> current position. If the position is at EOF, it shall restart from the
> beginning ("re-threaded tape").

> **REQ-076** — Transmission shall be paced by `char_delay_us` (default 0) and
> `line_delay_ms` (default 0, applied after each line end). At most one byte
> shall be queued in the transport beyond the pacing point, so that DC3 takes
> effect within one character time plus network RTT.

> **REQ-077** — Line endings in the file (LF, CR, or CRLF) shall be sent as
> `load_eol` (default CR LF). `load_eol = raw` shall send the file bytes
> unchanged.

> **REQ-078** — DC3 received during a load shall pause transmission and keep
> the position. The dispatcher shall return to top of loop, and a later DC1
> shall resume from that position.

> **REQ-079** — At EOF the load shall end, the position shall be set to EOF,
> and the dispatcher shall return to top of loop.

> **REQ-080** — DC1 with no load file selected, or an unreadable one, should
> send `load_missing` (default: the terminator record `S9030000FC` + CR LF) so
> that a waiting loader terminates. `load_missing = none` shall send nothing.
> The event shall be logged at warn.

> **REQ-081** — Bytes other than DC3 received during a load shall be discarded
> and counted.

> **REQ-082** — `fnetd` should optionally validate the load file's S-Record
> syntax and checksums when it is selected (`load_validate = warn|reject|off`,
> default `warn`).

#### 2.6.3 Punch

> **REQ-083** — DC2 shall open the selected punch file and capture the
> received bytes until DC4. The open mode comes from `punch_mode` (default
> `overwrite`): `overwrite` | `append` | `version`. `version` writes
> `NAME.n.EXT` with the next free n.

> **REQ-084** — DC2 with no punch file selected shall capture to
> `<tape_root>/<default_cpu>/punch/<port>-YYYYMMDD-HHMMSS.s19`, with an info
> log message.

> **REQ-085** — NUL (`$00`), DEL (`$7F`) and rubout (`$FF`) leader/trailer
> bytes shall be stripped. CR, LF and CRLF line ends shall be normalised to a
> single LF in the file.

> **REQ-086** — DC4 shall flush, `fsync` and close the punch file and return
> the dispatcher to top of loop.

> **REQ-087** — If no byte arrives for `punch_timeout` (default 30 s) during a
> punch, `fnetd` shall close the file as if DC4 had been received and log a
> warning.

> **REQ-088** — Captured S-Records shall be checked (record syntax, byte
> count, checksum, and presence of an S9/S8/S7 terminator). Problems shall be
> logged at warn with line numbers, and the file shall still be kept.

> **REQ-089** — The punch file shall be written via a temporary file plus
> `rename()` in `overwrite` mode, so that an aborted punch never truncates the
> previous file until DC4 or the timeout.

### 2.7 Security and Path Confinement

> **REQ-090** — All client-supplied paths (FlexNet `P`/`M`/`m`/`C`/`c`/`D`,
> tape filenames) shall be resolved with `openat2(..., RESOLVE_BENEATH |
> RESOLVE_NO_MAGICLINKS)` relative to the port's root directory fd. Escape via
> `..`, absolute paths or symlinks shall be refused.

> **REQ-091** — Each port shall have a configurable `flexnet_root` and
> `tape_root`. Ports may share roots.

> **REQ-094** — `listen` sockets shall be bound only to the configured address
> (REQ-130) and filtered by `allow` (REQ-133). `fnetd` shall log the peer
> address of every accepted and every rejected connection.

> **REQ-092** — The control socket `/run/fnetd/ctl` shall be mode 0660, owned by
> `fnet:fnet`. Peer credentials (`SO_PEERCRED`) shall be logged for every
> state-changing command.

> **REQ-093** — Configuration-defined `read_only = yes` on a port shall force
> every mount on that port to `R` and refuse `C`/`c`/`D`.

### 2.8 Runtime Control (`fnetctl`)

> **REQ-100** — `fnetctl` shall communicate with `fnetd` over the control
> socket. Each command shall return exit 0 on success, 1 on a daemon-reported
> error, and 2 on usage or connection errors.

> **REQ-101** — `fnetctl` shall support: `status`, `ports`, `port
> add|remove|enable|disable|restart`, `sessions`, `mount`, `umount`, `drives`,
> `tape set|show|rewind|cancel`, `log level`, `log reopen`, `stats`, `reload`,
> `version` (§4.2.3).

> **REQ-102** — `fnetctl` output shall be human-readable by default and JSON
> with `--json`.

> **REQ-103** — Every runtime change made with `fnetctl` shall take effect
> without restarting `fnetd` or disturbing other ports.

> **REQ-104** — `fnetctl mount`/`umount` shall be refused (error, not queued)
> while that port is mid-transaction, and retried by the client with `--wait`
> (timeout default 5 s).

> **REQ-105** — Runtime changes shall not be written back to the config file.
> `fnetctl port add` should warn that the change is not persistent.

### 2.9 Telemetry

> **REQ-110** — Log levels shall be `error`, `warn`, `notice`, `info`,
> `debug`, `trace`. `trace` hex-dumps every wire byte with direction and port
> name.

> **REQ-111** — The log level shall be settable globally and per port, in the
> config and at runtime (`fnetctl log level [--port P] <level>`).

> **REQ-112** — Log sinks shall be `journal` (default under systemd), `syslog`
> (facility `daemon`) and `file`, selectable in the config. The file sink shall
> reopen on `SIGUSR1` or `fnetctl log reopen` (for logrotate).

> **REQ-113** — Each log record shall carry timestamp (µs), level, port name
> and subsystem (`core`, `xport`, `rfc2217`, `flexnet`, `tape`, `image`,
> `ctl`).

> **REQ-114** — `fnetd` shall write `/run/fnetd/stats.json` atomically every
> `stats_interval` (default 10 s) and on `fnetctl stats --flush`. Per port, it
> shall contain: link state, connect count, rx/tx bytes, sectors read/written,
> checksum errors (each direction), NAKs sent/received, transaction timeouts,
> sync losses, discarded bytes, tape bytes loaded/punched, current mode and
> mounts.

> **REQ-115** — Counters shall be monotonic since daemon start, and should be
> resettable with `fnetctl stats --reset [--port P]`.

### 2.10 Offline Image Tool (`fnetimg`)

> **REQ-120** — `fnetimg create <file> --tracks T --sectors S [--track0 S0]
> [--label L] [--volume N]` shall create an image per REQ-063/064.

> **REQ-121** — `fnetimg info <file>` shall print geometry, SIR fields and
> free-sector count.

> **REQ-122** — `fnetimg ls <file>` shall list directory entries (name, ext,
> start/end T/S, sectors, random flag, date) and skip deleted entries unless
> `-a` is given.

> **REQ-123** — `fnetimg check <file>` shall verify the free chain and file
> chains: no cycles, no cross-links, the free count matches the SIR, and every
> sector is accounted for. It shall exit non-zero on any error.

> **REQ-124** — `fnetimg` and `fnetd` shall share one image library (`libflexdsk`).

---

## 3. Hardware & Platform Constraints

- **Target hardware (host)**: any x86-64 or ARM64 Linux machine, including a
  Raspberry Pi 3 or newer.
- **Target hardware (client)**: 6800/6809 (later 6801/68HC11/68K) systems with
  a second MC6850 ACIA, 8N1, polled, with a 1-byte RX buffer (see REQ-026,
  REQ-076).
- **Memory constraints (host)**: RSS < 8 MB with 16 ports idle. Images are not
  loaded into RAM; they are accessed by pread/pwrite (REQ-062).
- **Operating environment**: Debian 13, Linux ≥ 5.6 (`openat2`, REQ-090),
  glibc, systemd (sd_notify via a socket write, no libsystemd dependency).
- **Toolchain**: C11 (`-std=c11 -D_GNU_SOURCE`), GCC ≥ 12 or Clang ≥ 15, GNU
  make. Builds clean with `-Wall -Wextra -Wpedantic -Werror -Wconversion`.
  `make asan` target builds with ASan/UBSan.
- **Peripheral interfaces**: POSIX ttys (`/dev/ttyS*`, `/dev/ttyUSB*`,
  `/dev/ttyACM*`); TCP to/from terminal servers (RFC-2217 or raw), outbound
  (`rfc2217`) or inbound (`listen`, REQ-130).
- **Timing**:
  - Reply turnaround ≤ 5 ms (REQ-056).
  - At 19200 baud a sector read takes about 135 ms on the wire.
  - Client timeouts are polling loops (`256 × odelc`). Over RFC-2217, network
    RTT counts against them, so a terminal server should be on the same LAN
    (RTT < 10 ms).
- **Power constraints**: N/A.
- **Standards compliance**:
  - SEI CERT C as guidance: no unchecked return values, no VLAs, bounds-checked
    buffers.
  - Semantic Versioning 2.0.0.
  - RFC 854/856/858 (Telnet, BINARY, SGA).
  - RFC 2217.

---

## 4. API & Interface Definitions

### 4.1 External Hardware Interfaces

| Interface | Details |
|-----------|---------|
| Local serial | termios raw, 8N1, `CLOCAL`, `~CRTSCTS`, `~IXON/IXOFF`. Baud from config. |
| RFC-2217 | TCP to `host:port`. Options: BINARY(0) both ways, SGA(3), COM-PORT-OPTION(44). Subneg: SET-BAUDRATE(1), SET-DATASIZE(2)=8, SET-PARITY(3)=1 (none), SET-STOPSIZE(4)=1, SET-CONTROL(5)=1 (no flow). PURGE-DATA(12) on port restart. NOTIFY-LINESTATE/MODEMSTATE are accepted and logged at debug. |
| Network listen | TCP `bind:port`, one peer at a time (`on_new` replace/refuse), `allow` CIDR list. Same Telnet/RFC-2217 negotiation as above after accept, or raw with `telnet = no`. |
| Target side | Second MC6850 ACIA; all cases look identical to it. |

### 4.2 Software APIs

#### 4.2.1 Internal Modules

```c
/* xport.h — byte-stream transport (tty, rfc2217, listen; telnet on/off) */
typedef struct xport xport_t;
typedef struct {
    int     (*open)(xport_t *x);
    ssize_t (*read)(xport_t *x, uint8_t *buf, size_t n);   /* non-blocking, telnet stripped */
    ssize_t (*write)(xport_t *x, const uint8_t *buf, size_t n);
    size_t  (*pending_tx)(xport_t *x);                      /* bytes not yet on the wire */
    int     (*fd)(xport_t *x);                              /* for epoll */
    void    (*close)(xport_t *x);
} xport_ops_t;

/* flexdsk.h — image library (libflexdsk) */
typedef struct { uint8_t max_trk, spt, t0_spt; uint32_t blocks; } flex_geom_t;
int  flexdsk_open(int dirfd, const char *name, bool want_rw, flexdsk_t **out);
int  flexdsk_read(flexdsk_t *d, uint8_t trk, uint8_t sec, uint8_t buf[256]);
int  flexdsk_write(flexdsk_t *d, uint8_t trk, uint8_t sec, const uint8_t buf[256]);
int  flexdsk_create(int dirfd, const char *name, const flex_create_t *p);
int  flexdsk_ts2blk(const flex_geom_t *g, uint8_t trk, uint8_t sec, uint32_t *blk);
void flexdsk_close(flexdsk_t *d);
uint16_t flex_cksum(const uint8_t buf[256]);

/* flexnet.h — per-port protocol state machine */
void flexnet_feed(session_t *s, uint8_t byte);       /* called by dispatcher */
void flexnet_timeout(session_t *s);                  /* REQ-024 */
void flexnet_end(session_t *s, const char *why);     /* REQ-023 */

/* tape.h */
int  tape_select(session_t *s, const char *spec);    /* parsed '|' string */
void tape_load_start(session_t *s);                  /* DC1 */
void tape_load_pause(session_t *s);                  /* DC3 */
void tape_punch_start(session_t *s);                 /* DC2 */
void tape_punch_feed(session_t *s, uint8_t byte);
void tape_punch_end(session_t *s);                   /* DC4 / timeout */
int  srec_check_line(const char *line, srec_info_t *out);

/* log.h */
void flog(int level, const port_t *p, const char *subsys, const char *fmt, ...);
```

#### 4.2.2 Configuration File (`/etc/fnetd/fnetd.conf`)

```ini
[daemon]
user            = fnet
log_sink        = journal        ; journal | syslog | file
log_file        = /var/log/fnetd/fnetd.log
log_level       = info
stats_file      = /run/fnetd/stats.json
stats_interval  = 10
ctl_socket      = /run/fnetd/ctl

[defaults]
flexnet_root    = /srv/fnet/flex
tape_root       = /srv/fnet/tape
default_cpu     = 6800
xfer_timeout_ms = 2000
sync_loss_count = 3
sync_loss_garbage = 16
sync_interval   = 5
char_delay_us   = 0
line_delay_ms   = 0
load_eol        = crlf           ; crlf | cr | lf | raw
load_missing    = s9             ; s9 | none
load_validate   = warn           ; warn | reject | off
punch_mode      = overwrite      ; overwrite | append | version
punch_timeout   = 30
read_only       = no
legacy_drive    = 0              ; drive used by legacy 'M' (REQ-040)

[tape.cpu]                       ; optional aliases; any <tape_root>/<tag>/ dir works
6811 = 68HC11

[port.swtpc]
type     = tty
device   = /dev/serial/by-id/usb-FTDI_FT232R-if00-port0
baud     = 19200
default_cpu = 6800

[port.coco]
type     = rfc2217
host     = ts1.lan
port     = 7001
baud     = 38400
default_cpu = 6809
log_level = debug

[port.lab6800]
type     = listen                ; terminal server connects to us
bind     = 192.168.1.10
port     = 7101
allow    = 192.168.1.50/32
on_new   = replace               ; replace | refuse
telnet   = yes                   ; no = raw TCP
baud     = 9600
default_cpu = MIKBUG
legacy_drive = 1
```

#### 4.2.3 CLI Synopsis

```
fnetd   [-f] [-c conf] [-l level] [-V]
fnetctl [--socket path] [--json] <command>
  status | version | reload
  ports
  port add <name> type=tty|rfc2217|listen [key=val ...]
  port remove|enable|disable|restart <name>
  port set <name> legacy_drive=<0-3>
  sessions
  drives <port>
  mount  <port> <drv> <path> [--ro] [--wait]
  umount <port> <drv|all> [--wait]
  tape set <port> [load=<cpu:file>] [punch=<cpu:file>]
  tape show|rewind|cancel <port>
  log level [--port P] <level>
  log reopen
  stats [--port P] [--flush] [--reset]
fnetimg create|info|ls|check <file> [options]
```

#### 4.2.4 Control Socket Protocol

Unix `SOCK_SEQPACKET` socket. Each message is one JSON object.

Request: `{"cmd":"mount","port":"swtpc","drv":1,"path":"/games/ADVENT.DSK","ro":false}`

Reply: `{"ok":true,"data":{...}}` or `{"ok":false,"err":"EBUSY","msg":"..."}`.

Messages are at most 64 KiB. The protocol is versioned by
`{"cmd":"hello","proto":1}`.

### 4.3 Communication Protocols

#### 4.3.1 FlexNet

Normative reference: `t/flexnet-protocol.md` §1–§7, with the deviations
required by REQ-021, 024, 025, 036, 038, 040–045, 048, 051–053, 055. Summary:

| Byte | Request | Reply | fnetd |
|------|---------|-------|-------|
| `$55` / `$AA` | — | echo | REQ-030 |
| `Q` | — | ACK | REQ-031 |
| `?` | — | vpath CR ACK | REQ-032 |
| `V` | text CR | ACK | REQ-033 |
| `s`/`S` | drv trk sec | 256 + ck(2); client ACK/NAK | REQ-034/036 |
| `r`/`R` | drv trk sec 256 ck(2) | ACK/NAK | REQ-035/037 |
| `M` | name CR | ACK R/W or NAK | REQ-040 |
| `m` | drv name CR | ACK R/W or NAK | REQ-041 |
| `d` | drv | name… ACK | REQ-043 |
| `P` | path CR | ACK/NAK | REQ-046 |
| `A` | pattern CR + SP/ESC | CR LF, lines, ACK | REQ-047/048 |
| `I` | CR SP + SP/ESC | CR LF, lines, ACK | REQ-049 |
| `C` | 5 × field CR | ACK/NAK | REQ-051 |
| `c` | drv + 5 × field CR | ACK/NAK | REQ-051 |
| `D` | name.DSK CR | ACK/NAK | REQ-052 |
| `E` | — | ACK; session ends | REQ-053 |

#### 4.3.2 Paper Tape

| State | Input | Action | Next |
|-------|-------|--------|------|
| TOP | `|` | collect selection to `<term>` | TOP |
| TOP | DC1 | start/resume load | LOAD |
| TOP | DC2 | open punch file | PUNCH |
| LOAD | DC3 | pause | TOP |
| LOAD | (EOF) | end | TOP |
| LOAD | other | discard | LOAD |
| PUNCH | DC4 | close | TOP |
| PUNCH | timeout | close + warn | TOP |
| PUNCH | other | filter + write | PUNCH |

#### 4.3.3 RFC-2217

`fnetd` always takes the role that sets the port parameters. The TCP direction
is independent of that role: `rfc2217` ports connect out, and `listen` ports
accept (REQ-131). See §4.1 for the option set. `telnet = no` disables Telnet
entirely (REQ-135).

### 4.4 Third-Party Libraries / SDKs

- glibc / Linux UAPI only (epoll, timerfd, signalfd, openat2 via `syscall()`).
- INI parser: vendored `inih` (BSD-3-Clause), or a ~300-line in-tree parser.
- JSON: an in-tree minimal emitter and parser for the control socket (no
  external library).
- No libsystemd. `sd_notify` is implemented by writing to `$NOTIFY_SOCKET`.
- Tests: plain C test runner plus Python 3 (stdlib only) for the
  integration harness.

---

## 5. Test Plans

Test harness: `make test` runs unit tests. `make itest` starts `fnetd` against
a `socat`-created pty pair, plus an in-tree mock RFC-2217 terminal server
(`tests/mock2217.py`), and drives a scripted FLEX-client emulator
(`tests/flexclient.py`) that speaks the 6800/6809 driver protocol byte-for-byte.

### 5.1 Daemon Lifecycle

> **TEST-001** *(validates REQ-001, REQ-002, REQ-003)*
> **Method**: Integration
> **Procedure**: Start with a valid config, under systemd and with `-f`. Then
> start with a config containing a syntax error on line 12.
> **Pass Criteria**: The valid config reaches READY (sd_notify) within 1 s. The
> invalid config exits non-zero with a message naming the file and line 12.

> **TEST-002** *(validates REQ-004, REQ-005)*
> **Method**: Integration
> **Procedure**: With two active ports, edit only port B's baud and send
> SIGHUP. Then introduce an invalid key and run `fnetctl reload`.
> **Pass Criteria**: Port A's session and mounts survive (reads continue
> uninterrupted) and port B reopens at the new baud. The invalid reload returns
> an error and both ports keep running.

> **TEST-003** *(validates REQ-006, REQ-062)*
> **Method**: Integration
> **Procedure**: Send SIGTERM during a stream of `r` writes.
> **Pass Criteria**: Exit in ≤ 3 s. The last ACKed sector is present in the
> image file. No partial sector is written.

> **TEST-004** *(validates REQ-007, REQ-008)*
> **Method**: Integration
> **Procedure**: Check the process uid. Start a second instance.
> **Pass Criteria**: The uid is `fnet`. The second instance exits non-zero with
> "already running".

> **TEST-005** *(validates REQ-010, REQ-053, REQ-023)*
> **Method**: Integration
> **Procedure**: Mount on drive 1, send `E`, then send `d 1`.
> **Pass Criteria**: `E` gets ACK and the daemon PID is unchanged. `d 1` replies
> with ACK only (no mapping).

### 5.2 Transports

> **TEST-010** *(validates REQ-011, REQ-017, REQ-009)*
> **Method**: Integration
> **Procedure**: Run 16 pty ports, each doing continuous sector reads at
> 115200 baud for 60 s.
> **Pass Criteria**: All checksums are correct, no port stalls > 10 ms, and
> each reply turnaround is ≤ 5 ms at p99.

> **TEST-011** *(validates REQ-012, REQ-014)*
> **Method**: Integration (mock2217)
> **Procedure**: Connect, and capture the negotiation. Repeat with the mock
> refusing SET-CONTROL.
> **Pass Criteria**: BINARY, SGA and COM-PORT-OPTION are negotiated, with
> baud/8/N/1/no-flow subnegotiations. The refusal run logs a warning and the
> port still works.

> **TEST-012** *(validates REQ-013)*
> **Method**: Unit and integration
> **Procedure**: Read and write a sector that contains `$FF` bytes and a
> `$FF $FF` run over RFC-2217, and inject an IAC NOP mid-sector.
> **Pass Criteria**: The data round-trips exactly and the checksum matches.

> **TEST-013** *(validates REQ-015, REQ-016)*
> **Method**: Integration
> **Procedure**: Kill the mock2217 connection, and delete/recreate the pty
> symlink.
> **Pass Criteria**: Reconnect attempts follow 1,2,4…30 s. After reconnect,
> `d drv` returns the previous mapping without a remount.

> **TEST-014** *(validates REQ-018, REQ-103)*
> **Method**: Integration
> **Procedure**: `fnetctl port add/disable/enable/remove` on port C while port A
> transfers.
> **Pass Criteria**: Port A shows zero errors and port C follows each command.

> **TEST-015** *(validates REQ-020, REQ-094)*
> **Method**: Integration / script
> **Procedure**: Run `ss -ltnp` with no `listen` ports, then with one `listen`
> port bound to 127.0.0.2:7101.
> **Pass Criteria**: No listening sockets in the first case. Exactly one,
> `127.0.0.2:7101`, in the second.

> **TEST-016** *(validates REQ-130, REQ-131)*
> **Method**: Integration (mock2217 in connect-out mode)
> **Procedure**: The mock connects to a `listen` port. Capture the negotiation,
> then run a sector read/write.
> **Pass Criteria**: The negotiation is identical to TEST-011 and the data
> round-trips.

> **TEST-017** *(validates REQ-132)*
> **Method**: Integration
> **Procedure**: With connection A active, open connection B, first with
> `on_new = replace` and then with `refuse`.
> **Pass Criteria**: replace: A is closed and B serves (session state kept).
> refuse: B is closed and A is unaffected. Both cases log a warning.

> **TEST-018** *(validates REQ-133, REQ-094)*
> **Method**: Integration
> **Procedure**: Connect from an address outside `allow`, then from inside it,
> then with `allow` empty.
> **Pass Criteria**: The outside peer is closed with 0 bytes read and logged.
> The inside peer is accepted. With `allow` empty, every peer is refused.

> **TEST-019** *(validates REQ-134)*
> **Method**: Integration
> **Procedure**: Drop the peer's packets (iptables DROP) on an active TCP port.
> **Pass Criteria**: The connection is declared dead within ≈ 60 s and the
> session state is retained for the reconnect.

> **TEST-090** *(validates REQ-135)*
> **Method**: Integration
> **Procedure**: Use `telnet = no` on `rfc2217` and `listen` ports, and transfer
> a sector containing `$FF` bytes.
> **Pass Criteria**: No IAC bytes are sent or interpreted, and the data
> round-trips byte-exact.

### 5.3 Dispatcher

> **TEST-020** *(validates REQ-021)*
> **Method**: Integration
> **Procedure**: Mount, restart `fnetd`, then send `s` with no sync and `M`.
> **Pass Criteria**: The commands are handled without `$55`/`$AA`.

> **TEST-021** *(validates REQ-022, REQ-081)*
> **Method**: Integration
> **Procedure**: During a punch, send bytes `S`, `Q`, `$55`.
> **Pass Criteria**: The bytes are written to the punch file and no FlexNet
> reply is sent.

> **TEST-022** *(validates REQ-024)*
> **Method**: Integration
> **Procedure**: Send `P games` with no CR and wait 2.5 s, then send `$55`.
> **Pass Criteria**: No reply to `P`, then `$55` is echoed.

> **TEST-023** *(validates REQ-025, REQ-023)*
> **Method**: Integration
> **Procedure**: Send 16 junk bytes, then `d 0`. Separately, cause 3 transaction
> timeouts.
> **Pass Criteria**: Sync loss is logged, the session ends (mounts cleared, cwd
> at root), and the sync-loss counter increases by 1.

> **TEST-024** *(validates REQ-026)*
> **Method**: Integration
> **Procedure**: Idle for 60 s with mounts, and change mounts via `fnetctl`.
> **Pass Criteria**: Zero bytes are transmitted on the port.

### 5.4 FlexNet

> **TEST-030** *(validates REQ-030–REQ-033)*
> **Method**: Integration
> **Procedure**: Send `$55`, `$AA`, `Q` (unmounted), `?`, `V A`.
> **Pass Criteria**: Echoes, ACK, a `/`-rooted virtual path + CR ACK with no
> host path prefix, and ACK.

> **TEST-031** *(validates REQ-034, REQ-035, REQ-039)*
> **Method**: Integration
> **Procedure**: Map different images to drv 0 and 2. Read and write T5S3 on
> each and compare with the image files.
> **Pass Criteria**: The bytes match per drive and the checksums are correct.

> **TEST-032** *(validates REQ-036, REQ-037)*
> **Method**: Integration
> **Procedure**: Read an unmapped drv, `trk > max`, and `sec > spt`. Write to
> an R image. Send a bad-checksum write.
> **Pass Criteria**: Each read is exactly 258 bytes ending `$00 $01`, and the
> next `Q` gets a clean ACK. Each write gets NAK.

> **TEST-033** *(validates REQ-038, REQ-061)*
> **Method**: Unit
> **Procedure**: Use a DD image with SD track 0 (`t0_spt` = 10, `spt` = 18).
> Map T0S10, T0S11, T1S1, T0S0.
> **Pass Criteria**: Blocks 9, invalid, 10, 0.

> **TEST-034** *(validates REQ-039, REQ-040, REQ-041, REQ-042, REQ-043)*
> **Method**: Integration
> **Procedure**: With `legacy_drive` = 0: `M A`, then `m 1 B`, then `m 1 NOPE`,
> then `d 0`/`d 1`/`d 3`, then read drv 3. Next, `fnetctl port set
> legacy_drive=3` and `M C`.
> **Pass Criteria**: `d 0`=`A` and `d 1`=`B` (the failed mount leaves B in
> place). `d 3` = ACK only, and the drv 3 read ends `$00 $01`. After the
> change, `d 3`=`C` and `d 0` is still `A`.

> **TEST-035** *(validates REQ-044, REQ-045, REQ-093)*
> **Method**: Integration
> **Procedure**: Two ports `m` the same image. Also mount a mode-0444 image and
> a mount on a `read_only` port.
> **Pass Criteria**: The first gets `W`, the second gets `R` and a warning is
> logged. The 0444 image and the read-only-port mount get `R`.

> **TEST-036** *(validates REQ-046, REQ-090)*
> **Method**: Integration
> **Procedure**: `P games`, `P ..` (from root), `P /etc`, `P link-to-outside`.
> **Pass Criteria**: ACK, NAK, NAK, NAK, with cwd unchanged after each NAK.

> **TEST-037** *(validates REQ-047, REQ-048, REQ-049, REQ-050)*
> **Method**: Integration
> **Procedure**: The directory holds `ADV.DSK`, `adv2.dsk`, `backupdsk`, `X.TXT`
> and subdirs `a`, `B`. Send `A AD`, `A *2*`, `I`, and ESC mid-list.
> **Pass Criteria**: `ADV.DSK`, `adv2.dsk` (sorted); `adv2.dsk`; `a`, `B`. ESC
> gives an immediate ACK. The handshake matches §6.5 byte-for-byte.

> **TEST-038** *(validates REQ-051, REQ-063, REQ-064)*
> **Method**: Integration
> **Procedure**: `C` with 40 tracks/18 sectors, then `fnetimg check` it. `c 2`
> with 80/36. `C` with 300 tracks.
> **Pass Criteria**: ACK, check passes, and the free count = 39·18. `c` maps to
> drv 2. 300 tracks gets NAK.

> **TEST-039** *(validates REQ-052)*
> **Method**: Integration
> **Procedure**: `D` an unmounted image, a mounted image, and a missing name.
> **Pass Criteria**: ACK + file removed, NAK, NAK.

> **TEST-040** *(validates REQ-054, REQ-055)*
> **Method**: Integration
> **Procedure**: Send unknown byte `$01`, then `M` + 200 chars + CR.
> **Pass Criteria**: No reply to `$01`. `M` gets NAK and sync is kept (the next
> `Q` gets ACK).

> **TEST-041** *(validates REQ-056)*
> **Method**: Integration
> **Procedure**: Timestamp the last request byte and the first reply byte over
> 10k `Q`s on a pty.
> **Pass Criteria**: p99 ≤ 5 ms.

> **TEST-042** *(validates REQ-034–REQ-053)*
> **Method**: Hardware-in-loop
> **Procedure**: On real 6800 and 6809 FLEX systems, run FLEXNET/FNETDRV, RDIR,
> RMOUNT, FMOUNT, FCREATE, RDELETE, COPY a 50-sector file both ways, and REXIT.
> Use both a local tty and a terminal server.
> **Pass Criteria**: All utilities complete without "Communication is not
> working", and the copied files compare equal.

### 5.5 Disk Images

> **TEST-050** *(validates REQ-060, REQ-061)*
> **Method**: Unit
> **Procedure**: Open images of 35×10, 77×26, 80×36 (DD + SD t0), 256×255
> (16,711,680 bytes), plus a truncated image and an image with a bad SIR.
> **Pass Criteria**: Correct geometry for the valid images. The truncated and
> bad-SIR images are rejected with a log message.

> **TEST-051** *(validates REQ-062)*
> **Method**: Unit (strace)
> **Procedure**: Write sectors, then unmount.
> **Pass Criteria**: Only `pread`/`pwrite` are used, and `fdatasync` is called
> before close.

> **TEST-052** *(validates REQ-065)*
> **Method**: Unit
> **Procedure**: Serve an image with a corrupt directory.
> **Pass Criteria**: Sectors are served unchanged.

### 5.6 Paper Tape

> **TEST-060** *(validates REQ-070, REQ-071, REQ-072, REQ-073)*
> **Method**: Integration
> **Procedure**: The tape_root holds dirs `6800`, `6809`, `MIKBUG`. Send
> `|6800:A.S19|6809:B.S19\r`, `||6800:C.S19\0`, `|mikbug:D.S19\r`,
> `|ZZZ:X\r`, `|6800:../../etc/passwd\r`, `|68$K:X\r`, and a 200-byte string.
> **Pass Criteria**: Selections are A/B, then A/C, then D from `MIKBUG/`
> (case-insensitive). ZZZ (no dir) is rejected, the escape is rejected,
> `68$K` (invalid char) is rejected, and the overflow is ignored. Nothing is
> echoed.

> **TEST-061** *(validates REQ-075, REQ-077, REQ-079)*
> **Method**: Integration
> **Procedure**: Use a file with LF endings. Send DC1 and read to EOF, then send
> DC1 again.
> **Pass Criteria**: The output equals the file with CRLF endings, and the
> second DC1 restarts from byte 0.

> **TEST-062** *(validates REQ-076, REQ-078)*
> **Method**: Integration
> **Procedure**: Set `char_delay_us` = 1000. Send DC1, then DC3 after 100 ms,
> wait, then DC1 again.
> **Pass Criteria**: ≤ 2 bytes arrive after DC3 (pty). Concatenating the two
> parts equals the file.

> **TEST-063** *(validates REQ-080)*
> **Method**: Integration
> **Procedure**: Send DC1 with no selection, with `load_missing` = s9 and = none.
> **Pass Criteria**: `S9030000FC\r\n` is sent with a warn log, or nothing is sent.

> **TEST-064** *(validates REQ-082)*
> **Method**: Unit
> **Procedure**: Select a file with a bad checksum on line 3 under warn/reject.
> **Pass Criteria**: warn logs line 3. reject refuses the selection.

> **TEST-065** *(validates REQ-083, REQ-084, REQ-085, REQ-086, REQ-089)*
> **Method**: Integration
> **Procedure**: Send DC2, NULs, S1 records with CR LF, NULs, DC4. Use each
> `punch_mode`, and also punch with no selection. Kill the daemon mid-punch in
> overwrite mode.
> **Pass Criteria**: The files contain only the records with LF endings.
> append/version behave as specified. The auto-name lands in `punch/`. After
> the kill, the previous file is intact.

> **TEST-066** *(validates REQ-087, REQ-088)*
> **Method**: Integration
> **Procedure**: Punch with no S9 and stop sending for 31 s. Punch a record with
> a bad byte count.
> **Pass Criteria**: The file is closed at about 30 s with warnings for the
> missing terminator and for the bad line.

> **TEST-067** *(validates REQ-070–REQ-086)*
> **Method**: Hardware-in-loop
> **Procedure**: Under MIKBUG on a 6800, println the selection string, run `L`
> to load, then `P` to punch a range. Repeat with a 6809 monitor.
> **Pass Criteria**: The loaded program runs, and the punched file re-loads and
> verifies.

### 5.7 Control, Telemetry, Security

> **TEST-070** *(validates REQ-100, REQ-101, REQ-102)*
> **Method**: Integration
> **Procedure**: Run every `fnetctl` command with and without `--json`, plus bad
> usage and a stopped daemon.
> **Pass Criteria**: Valid JSON with `--json`, and exit codes 0/1/2 as
> specified.

> **TEST-071** *(validates REQ-104, REQ-105)*
> **Method**: Integration
> **Procedure**: Run `fnetctl mount` during a held-open `r` transaction, with
> and without `--wait`. Run `port add`, then check the config file.
> **Pass Criteria**: EBUSY without `--wait`, success after the transaction with
> `--wait`. The config file is unchanged and a warning is printed.

> **TEST-072** *(validates REQ-092)*
> **Method**: Integration
> **Procedure**: Connect to ctl as a user not in `fnet`. Run `mount` as a member.
> **Pass Criteria**: Permission denied, and the log line includes uid/pid.

> **TEST-073** *(validates REQ-110, REQ-111, REQ-113)*
> **Method**: Integration
> **Procedure**: Set port A to trace and the global level to warn, then do a
> read on A and on B.
> **Pass Criteria**: Hex dump lines appear for A only, with timestamp, level,
> port and subsystem fields.

> **TEST-074** *(validates REQ-112)*
> **Method**: Integration
> **Procedure**: Use the file sink. `mv` the log file, then run
> `fnetctl log reopen`.
> **Pass Criteria**: New records go to a freshly created file.

> **TEST-075** *(validates REQ-114, REQ-115)*
> **Method**: Integration
> **Procedure**: Do 100 reads and 1 bad write, then `stats --flush`, then
> `--reset`.
> **Pass Criteria**: stats.json is valid with sectors_read=100 and
> cksum_err_rx=1, and the reset zeroes them. The file is never observed partial
> (polling reader).

### 5.8 Image Tool

> **TEST-080** *(validates REQ-120, REQ-121, REQ-123, REQ-124)*
> **Method**: Unit
> **Procedure**: Run `create` with each geometry in TEST-050, then `info` and
> `check`.
> **Pass Criteria**: check exits 0 and info matches the parameters.

> **TEST-081** *(validates REQ-122, REQ-123)*
> **Method**: Unit
> **Procedure**: Run `ls` on a reference image with known files. Run `check` on
> images with a cross-link, a cycle, and a wrong free count.
> **Pass Criteria**: The listing matches the reference, and each corruption is
> reported with exit ≠ 0.

---

## Appendix

### A. Reference Documents

- `t/flexnet-protocol.md`: FlexNet/NetPC wire protocol and defect list (normative).
- `t/fnetsrv-protocol.md`: second analysis of the same protocol (Wurtz `flexnet.c`).
- `docs/flexdsk.md`: FLEX SIR and directory structures.
- `t/fds-sds-protocol.md`: FDC+ serial disk protocol (background only).
- RFC 854, 856, 858, 2217. Motorola S-Record format (M68000PRM Appendix C).

### B. Related Projects

- `t/flexnet.l`: flexnet_final.c and client sources (6800/6803/6809/68000).
- `t/Flexnet.l`: original NetPC (DOS, C++).
- `t/flexnet-linux.l`, `t/fnetsrv.l`, `t/fnetdrv.l`: other Linux servers and the FNETDRV driver.
- `t/fdc-sds.l`: FDC+ serial disk server (DeRamp), as a model for multi-drive management.

### C. Open Questions / TBD

Resolved in v0.0.2:

- Network-listen ports (REQ-130–REQ-135).
- Sync-loss thresholds as in REQ-025.
- No default slot; legacy `M` → `legacy_drive` (REQ-039/REQ-040).
- Second writer → `R` (REQ-045).
- Any processor tag (REQ-071).
- Punch EOL normalised (REQ-085).

Open:

1. **`legacy_drive` default** (REQ-040): is 0 right, or should it be an `auto`
   mode that binds to the `drv` of the first `s`/`r` after `M`?
2. **Persisting runtime changes** (REQ-105): should `fnetctl save` write the
   running config back? Deferred.
3. **6801/68HC11/68K**: the server is CPU-agnostic. Only the client drivers are
   affected. Deferred.
4. **FLEX 1.0 image format**: deferred.
5. **`load_missing` S9 record** (REQ-080): verify that MIKBUG `L` terminates
   cleanly on `S9030000FC`.
