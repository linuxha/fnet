# fnetd — Operator Guide (v0.0.1)

Requirements live in [fnetd-fsd.md](fnetd-fsd.md). This guide covers building,
installing, configuring and operating `fnetd`, `fnetctl`, `fnetimg` and `fnetmock`.

## Build and test

```sh
make            # build/fnetd, build/fnetctl, build/fnetimg, build/fnetmock
make test       # unit tests + config check of etc/fnetd.conf
make itest      # integration tests (pty FLEX emulator, mock RFC-2217 server)
make asan       # ASan/UBSan build in build-asan/ (+ unit tests)
FNET_BUILD=build-asan make itest   # integration tests against the sanitizer build
```

Requirements: Linux ≥ 5.6 (`openat2`), GCC ≥ 12 or Clang ≥ 15, GNU make,
Python 3 (tests only). No third-party libraries.

## Install

```sh
sudo make install                    # PREFIX=/usr/local by default
sudo systemd-sysusers                # creates user fnet, adds it to dialout
sudo install -d -o fnet -g fnet /srv/fnet/flex /srv/fnet/tape/6800 /srv/fnet/tape/6809
sudoedit /etc/fnetd/fnetd.conf
sudo fnetd -t -c /etc/fnetd/fnetd.conf
sudo systemctl daemon-reload && sudo systemctl enable --now fnetd
sudo usermod -aG fnet $USER          # allow fnetctl (control socket is 0660 fnet:fnet)
```

## Directory layout

```
/srv/fnet/flex/            flexnet_root: .DSK images, subdirectories allowed (RCD / P)
/srv/fnet/tape/<CPU>/      tape_root: one directory per processor tag
/srv/fnet/tape/<CPU>/punch auto-named punch captures when no punch file is selected
```

Every path a FLEX system sends is confined to its root (`openat2`
`RESOLVE_BENEATH`). `..`, absolute paths and symlinks that point outside the
root are refused. `?` (RWHICH) reports virtual paths starting at `/`.

## Ports

| type      | fnetd role                         | required keys            |
|-----------|------------------------------------|--------------------------|
| `tty`     | opens a local serial device        | `device`, `baud`         |
| `rfc2217` | connects out to a terminal server  | `host`, `port`, `baud`   |
| `listen`  | accepts one terminal server        | `bind`, `port`, `allow`  |

With RFC-2217, fnetd always sets the serial parameters (baud, 8N1, no flow
control), whichever side opened the TCP connection. Use `telnet = no` for a
terminal server port in raw TCP mode. On a `listen` port with no `allow`
list, every connection is refused.

Each port serves one target system. FlexNet and paper tape share the port but
never run at the same time.

## FlexNet

The existing NETDRV/FNETDRV drivers and R\*/F\* utilities work unchanged.
`fnetd` differs from older Linux servers in these ways:

- **No sync needed.** A resident driver keeps working after fnetd restarts, without RESYNC.
- **Multi-drive commands.** `FMOUNT`/`FCREATE` (`m`/`c`/`d`) are implemented, so each FLEX drive 0–3 can
  have its own image.
- **Legacy RMOUNT.** `M` mounts on `legacy_drive`. Set it to the drive your old NETDRV is
  installed on. Drives with nothing mounted return a read error. They do not fall back to another image.
- **Read errors are reported.** A failed read returns 256 zero bytes with checksum `$0001`, so FLEX reports a
  CRC error instead of reading a blank sector.
- **Failed mounts are harmless.** A failed mount keeps the previous image.
- **One writer per image.** A second session that mounts an image already open for writing gets it
  read-only (`R`).
- **REXIT** ends the session, unmounts its drives and resets cwd. The daemon keeps running.
- **Loss of sync.** 3 transaction timeouts or 16 junk bytes in a row also end the session.

## Paper tape

From the target, print the selection string through the second ACIA, then
send the control characters:

```
|6800:GAME.S19|6800:DUMP.S19<CR>     select load and punch files
DC1 ($11)                            start/resume the reader
DC3 ($13)                            pause the reader
DC2 ($12)                            start the punch
DC4 ($14)                            stop the punch, close the file
```

- **Fields.** An empty field keeps the current selection, and `-` clears it.
- **Processor tag.** `<CPU>` is any directory name under `tape_root` (case-insensitive), for example
  `MIKBUG` or `68HC11`. `[tape.cpu]` can alias tags.
- **Restarting a load.** DC1 at end of file restarts the tape from the beginning. A new selection also
  rewinds.
- **No load file.** DC1 with nothing selected sends `S9030000FC`, so a waiting loader finishes.
- **Pacing.** Use `char_delay_us` and `line_delay_ms` for monitors without flow control.
- **Punch output.** NUL/DEL/`$FF` leader is stripped, line ends become LF, and every record is
  checked. Problems are logged and the file is kept. In `overwrite` mode the
  file is replaced only when the punch finishes.

## fnetctl

```sh
fnetctl status
fnetctl ports
fnetctl sessions
fnetctl drives swtpc
fnetctl mount swtpc 1 /games/ADVENT.DSK [--ro] [--wait]
fnetctl umount swtpc all
fnetctl tape set swtpc load=6800:GAME.S19 punch=6800:OUT.S19
fnetctl tape show swtpc
fnetctl port set swtpc legacy_drive=3        # link keys (baud, device, ...) restart the port
fnetctl port add lab type=listen bind=10.0.0.5 port=7101 allow=10.0.0.9
fnetctl log level --port swtpc trace         # hex dump of every wire byte
fnetctl stats --flush
fnetctl reload                               # same as systemctl reload fnetd
fnetctl --json ports                         # machine-readable
```

- **Exit codes.** 0 means OK, 1 means fnetd reported an error, 2 means a usage or connection error.
- **Persistence.** Runtime changes are not written to the config file.
- **Busy ports.** `mount`/`umount` are refused while a port is mid-transaction. Use `--wait` to retry.

## Telemetry

- **Logs** go to the journal by default (`journalctl -u fnetd`). Use `log_sink = file` together with
  `fnetctl log reopen` (or SIGUSR1) for logrotate.
- **Stats.** `/run/fnetd/stats.json` holds per-port counters. It is rewritten atomically every
  `stats_interval` seconds.

## Signals

| Signal  | Action                                                        |
|---------|---------------------------------------------------------------|
| SIGHUP  | reload configuration (unchanged ports keep their sessions)    |
| SIGUSR1 | reopen the log file                                           |
| SIGTERM | finish the current transaction, flush images, exit (≤ 3 s)    |

## fnetimg

```sh
fnetimg create GAMES.DSK --tracks 80 --sectors 36 [--track0 20] [--label GAMES] [--volume 1]
fnetimg info GAMES.DSK
fnetimg ls [-a] GAMES.DSK
fnetimg check GAMES.DSK          # free chain, file chains, cross-links, lost sectors
```

## fnetmock

`fnetmock` stands in for the target, so you can exercise fnetd without hardware.
On TCP it takes the terminal server's side: it answers fnetd's Telnet and
RFC-2217 negotiation and acknowledges every COM-PORT setting.

| transport                | pairs with fnetd port | notes                                      |
|--------------------------|-----------------------|--------------------------------------------|
| `-C, --connect HOST:PORT`| `type = listen`       | `-r` for `telnet = no`                     |
| `-L, --listen [ADDR:]PORT`| `type = rfc2217`     | waits for fnetd to dial in (`-w MS` limit) |
| `-T, --tty DEVICE`       | `type = tty`          | null-modem cable, `-b BAUD`                |
| `--pty[=LINK]`           | `type = tty`          | `device = LINK`; use `-w 1500` so fnetd can open it first |

Commands come from `-e 'cmd; cmd'`, `-s FILE`, the remaining arguments
(one command), or stdin (an interactive prompt on a tty). `help` lists them.
A `!` prefix expects the command to fail. The exit code is 0 when every command
succeeded, 1 when one failed (`-k` keeps going), and 2 for a usage or
connection error. `-v` shows the negotiation, and `-vv` adds a hex dump of the wire.

```sh
# FlexNet
fnetmock -C fnet.lan:7101 -e 'sync; mount 1 GAMES; dir; read 1 0 3; fetch 1 games.dsk; rexit'
fnetmock -C fnet.lan:7101 -e 'mount 0 SCRATCH; put 0 local.dsk'
fnetmock -C fnet.lan:7101 -e '!write 0 1 1 fill=$E5 badck'      # server must NAK

# Paper tape: select, load like a monitor, punch like a punch
fnetmock -C fnet.lan:7101 -e 'select 6800:GAME.S19 6800:OUT.S19; load out=game.s19'
fnetmock -C fnet.lan:7101 -e 'select 6800:GAME.S19; load pause-every=256 pause-ms=500'
fnetmock -C fnet.lan:7101 -e 'select "" 6800:OUT.S19; punch out.s19 leader=50 trailer=50 char-us=1000'

# fnetd dials out to us
fnetmock -L 0.0.0.0:7001 -v
```

| command | sends | notes |
|---------|-------|-------|
| `sync`, `ping`, `pwd`, `cd PATH`, `rexit` | `$55 $AA`, `Q`, `?`, `P`, `E` | |
| `mount [DRV] NAME`, `drive DRV` | `M` / `m`, `d` | prints `W`/`R` |
| `dir [PAT] [max=N]`, `dirs [max=N]` | `A`, `I` | `max` sends ESC after N entries |
| `create [DRV] DIR NAME VOL TRK SEC`, `delete NAME.DSK` | `C` / `c`, `D` | |
| `read DRV TRK SEC [out=F] [nak\|noack] [quiet]` | `s` | NAKs a bad checksum, like NETDRV |
| `write DRV TRK SEC FILE\|fill=XX [badck]` | `r` | |
| `fetch DRV FILE`, `put DRV FILE` | `s`… / `r`… | whole image; handles a short track 0 |
| `select [LOAD] [PUNCH] [term=cr\|lf\|eot\|nul]` | `\|LOAD\|PUNCH` | `""` keeps a field, `-` clears it |
| `load [out=F\|-] [idle=MS] [eof=record\|idle] [pause-every=N pause-ms=MS] [stop-after=N]` | DC1 (DC3) | checks each S-record, reports cps and bytes after DC3 |
| `punch FILE\|- [eol=crlf\|cr\|lf] [leader=N] [trailer=N] [char-us=N] [line-ms=N] [nodc4]` | DC2 … DC4 | `nodc4` tests `punch_timeout` |
| `dc1`…`dc4`, `send TOKEN…`, `recv [MS]`, `sleep MS`, `status` | | `send '$55' 'text\r'` |

## Test coverage

The automated suites implement the FSD's tests. Exceptions:

- **TEST-042 and TEST-067** (hardware-in-loop on real 6800/6809 systems) are manual.
- **TEST-019** checks that TCP keepalive is enabled. It does not simulate packet loss, which
  needs root and iptables.
- **TEST-072** checks the socket mode and peer logging. It does not test access by a user outside
  the `fnet` group.
