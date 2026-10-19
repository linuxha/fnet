# fnetmock

`fnetmock` is a mock FLEX target for testing `fnetd` without hardware. On the
FlexNet side it behaves like NETDRV/FNETDRV and the R\*/F\* utilities. On the
paper tape side it acts as a monitor that loads and punches S-Records over the
second ACIA. It speaks byte for byte what a real 6800/6809 sends, and it checks
everything fnetd sends back.

It is built and installed with the rest of the project:

```sh
make                    # build/fnetmock
sudo make install       # $(PREFIX)/bin/fnetmock
```

## Synopsis

```
fnetmock TRANSPORT [options] [COMMAND [ARGS...]]
```

## Transports

Pick exactly one. Each transport pairs with one fnetd port type.

| Option | fnetd port | fnetmock acts as |
|---|---|---|
| `-C, --connect HOST:PORT` | `type = listen` | a terminal server dialling in to fnetd |
| `-L, --listen [ADDR:]PORT` | `type = rfc2217` | a terminal server that fnetd dials (waits for one connection) |
| `-T, --tty DEVICE` | `type = tty` | the target at the far end of a null-modem cable |
| `-P, --pty[=LINK]` | `type = tty` | a new pty. Its path is printed, and `LINK` becomes a symlink to it, which is removed on exit. |

On TCP, fnetmock takes the terminal server's side of Telnet: it accepts BINARY
and SGA. It acknowledges every RFC-2217 COM-PORT setting fnetd sends (baud, data
size, parity, stop size, flow control), echoing the value back. Use `-r` when the
fnetd port has `telnet = no`.

## Options

| Option | Meaning |
|---|---|
| `-r, --raw` | Raw TCP: no Telnet or RFC-2217 |
| `-b, --baud N` | tty speed (default 9600) |
| `-t, --timeout MS` | Reply timeout for each exchange (default 3000) |
| `-w, --wait MS` | With `-L`, give up if fnetd hasn't connected within MS. Otherwise, pause MS before the first command, for example so fnetd can open a new pty. |
| `-e, --exec 'CMD; CMD'` | Run commands. Can be repeated. |
| `-s, --script FILE` | Run commands from FILE (`-` = stdin) |
| `-k, --keep-going` | Continue after a failed command |
| `-v, --verbose` | Show Telnet/RFC-2217 negotiation. `-vv` adds a hex dump of every byte (`>` sent, `<` received). |
| `-V, --version` | Print the version |

Commands are taken from `-e` and `-s` in the order given, then from any
remaining arguments (as one command). If there are none of these, commands are
read from stdin, with a `fnetmock>` prompt when stdin is a terminal.

## Command syntax

- `;` separates commands, and `#` starts a comment.
- `'…'` or `"…"` quote an argument. `""` is an empty argument.
- Options are written `key=value`, or as a bare flag name (`nak`, `quiet`, `nodc4`).
- Numbers can be decimal, `0x1F` or `$1F`.
- **A leading `!` inverts the result:** the command passes only if it fails. Use
  this in scripts that check fnetd refuses something.

Before every FlexNet or tape command, fnetmock discards bytes left over from an
earlier exchange and warns about them.

## Commands

### FlexNet

| Command | Sends | Result |
|---|---|---|
| `sync` | `$55`, `$AA` | Each byte must be echoed back |
| `ping` | `Q` | ACK; prints the round-trip time |
| `pwd` | `?` | Prints the current directory |
| `cd PATH` | `P` path CR | ACK |
| `mount NAME` | `M` name CR | ACK + `W`/`R`. Mounts on fnetd's `legacy_drive`. |
| `mount DRV NAME` | `m` drv name CR | ACK + `W`/`R` |
| `drive DRV` | `d` drv | Prints the image on DRV, or `(none)` |
| `dir [PATTERN] [max=N]` | `A` pattern CR, then SP for each line | Lists `.DSK` files. `max=N` sends ESC after N entries. |
| `dirs [max=N]` | `I` CR SP, then SP for each line | Lists directories |
| `create [DRV] DIR NAME VOL TRK SEC` | `C`, or `c` drv, then 5 CR-terminated fields | ACK. With DRV the new image is also mounted. Use `.` for the current directory. |
| `delete NAME.DSK` | `D` name CR | ACK |
| `read DRV TRK SEC [out=FILE] [nak\|noack] [quiet]` | `s` drv trk sec | 256 bytes + checksum. Hex-dumped, or saved to FILE. NAKs a bad checksum, as NETDRV does. `nak` always NAKs; `noack` sends nothing, to test fnetd's timeout. |
| `write DRV TRK SEC FILE\|fill=XX [badck]` | `r` drv trk sec + 256 bytes + checksum | ACK. `badck` corrupts the checksum, so fnetd should NAK. |
| `fetch DRV FILE` | `s` for every sector | Copies the mounted image to FILE. The geometry comes from the SIR; a short (single-density) track 0 is handled. |
| `put DRV FILE` | `r` for every sector | Writes local image FILE over the image mounted on DRV |
| `rexit` | `E` | ACK; fnetd ends the session |

### Paper tape

| Command | Sends | Result |
|---|---|---|
| `select [LOAD] [PUNCH] [term=cr\|lf\|eot\|nul]` | `\|LOAD\|PUNCH` + terminator | Sets the files. A field is `CPU:FILE` (or `FILE` for `default_cpu`). `""` keeps the current selection; `-` clears it. |
| `load [options]` | DC1 (+ DC3/DC1) | Reads the tape and checks every S-record |
| `punch FILE\|- [options]` | DC2, data, DC4 | Punches FILE (or stdin) |

`load` options:

| Option | Default | Meaning |
|---|---|---|
| `out=FILE` | none | Save the received bytes as they arrived. `out=-` writes to stdout. |
| `idle=MS` | 1000 | End the load when the line has been quiet this long |
| `eof=record\|idle` | `record` | Stop at the S7/S8/S9 terminator, or only on idle |
| `pause-every=N` | off | Send DC3 every N bytes, wait, then DC1 again (a reader with flow control) |
| `pause-ms=MS` | 500 | How long each pause lasts |
| `stop-after=N` | off | Send DC3 after N bytes and stop, leaving the tape paused |

The summary line reports the byte count, records, checksum errors, whether a
terminator arrived, elapsed time and characters per second. With pauses it also
reports the most bytes that arrived after a DC3. fnetd promises at most about one.

`punch` options:

| Option | Default | Meaning |
|---|---|---|
| `eol=crlf\|cr\|lf` | `crlf` | Line ending sent for each line |
| `leader=N` / `trailer=N` | 0 | NUL bytes before and after the data |
| `char-us=N` | 0 | Delay after every byte (a slow punch) |
| `line-ms=N` | 0 | Extra delay after every line |
| `nodc4` | off | Leave out the closing DC4, to test fnetd's `punch_timeout` |

### Raw and utility

| Command | Meaning |
|---|---|
| `dc1` `dc2` `dc3` `dc4` | Send that control character |
| `send TOKEN...` | Send raw bytes. `$XX` or `0xXX` is one byte; anything else is text with `\r` `\n` `\t` `\xHH` escapes. |
| `recv [MS]` | Hex-dump whatever arrives until the line is quiet for MS (default 500) |
| `sleep MS` | Wait |
| `echo TEXT...` | Print text (for script output) |
| `status` | Link, Telnet option state, and the last RFC-2217 baud from fnetd |
| `help` | List the commands |
| `quit` / `exit` | Stop |

## Exit status

| Code | Meaning |
|---|---|
| 0 | Every command succeeded |
| 1 | A command failed. fnetmock stops at the first failure unless `-k` is given. |
| 2 | Usage error, or the connection could not be made |

## Examples

Use an fnetd `listen` port:

```ini
[port.lab]
type   = listen
bind   = 127.0.0.1
port   = 7101
allow  = 127.0.0.1/32
baud   = 19200
```

```sh
# FlexNet session
fnetmock -C 127.0.0.1:7101 -e 'sync; pwd; dir; mount 1 GAMES; drive 1; read 1 0 3; rexit'

# Back up and restore a whole image
fnetmock -C 127.0.0.1:7101 -e 'mount 0 WORK; fetch 0 work-backup.dsk'
fnetmock -C 127.0.0.1:7101 -e 'mount 0 WORK; put 0 work-backup.dsk'

# Negative tests: fnetd must refuse these
fnetmock -C 127.0.0.1:7101 -e '!mount NOPE; mount 0 WORK; !write 0 1 1 fill=$E5 badck'

# Load a tape the way a monitor would, with flow control, and keep a copy
fnetmock -C 127.0.0.1:7101 -e 'select 6800:GAME.S19; load out=game.s19 pause-every=256 pause-ms=300'

# Punch to a chosen file on a slow punch
fnetmock -C 127.0.0.1:7101 -e 'select "" 6809:DUMP.S19; punch dump.s19 leader=50 trailer=50 char-us=1000'

# Interactive
fnetmock -C 127.0.0.1:7101
```

Let fnetd dial out to fnetmock (an fnetd `rfc2217` port with `host = 127.0.0.1`,
`port = 7001`):

```sh
fnetmock -L 127.0.0.1:7001 -w 30000 -v -e 'sync; status'
```

Use a pty for an fnetd `tty` port with `device = /tmp/fnet-tty0`:

```sh
fnetmock --pty=/tmp/fnet-tty0 -w 2500 -e 'sync; mount 0 WORK; dir'
```

Script file (`session.fm`):

```
# smoke test
sync
ping
mount 1 A
fetch 1 a.dsk
select 6800:HELLO.S19
load out=hello.s19
rexit
```

```sh
fnetmock -C 127.0.0.1:7101 -s session.fm
```

## Debugging

- **Wire traces.** `-vv` prints every byte in both directions. Compare this with
  `fnetctl log level --port lab trace` on the fnetd side.
- **Reference sequences.** These traces are the exact sequences a real target has
  to produce, which makes them a reference when porting the target-side drivers
  (see `src/6809/6809-analyze.md`).
- **Stray bytes.** A "discarding N stray byte(s)" warning means fnetd sent more
  than the previous exchange expected.

## See also

- [docs/fnetd-guide.md](../../docs/fnetd-guide.md): fnetd operator guide, including fnetmock
- [docs/fnetd-fsd.md](../../docs/fnetd-fsd.md): protocol and requirements
- `tests/itest/test_fnetmock.py`: fnetmock driven against a live fnetd over every transport
