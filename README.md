# fnet
A Linux Server that support remote file sharing for MIKBUG, Flex 6800 and Flex 6809.

# Description

Consider this a precursor to building a Fujinet board for Flex and MIKBUG (with paper tape device control control). For right now I'll concentrate on getting the Linux fnet server working with MIKBUG ROM monitor, Flex for the 6800 and Flex for the 6809.

At the moment (2026/10/17) these programs have not been tested. This is the first pass at this architecture and not all the pieces have been assembled.

## Help

There are three programs:

- `fnetd` is the daemon.
- `fnetctl` changes the running daemon without a restart.
- `fnetimg` creates and checks FLEX `.DSK` images.

The full details are in [docs/fnetd-guide.md](docs/fnetd-guide.md) (operator guide) and [docs/fnetd-fsd.md](docs/fnetd-fsd.md) (requirements).

### Build

```sh
make            # build/fnetd, build/fnetctl, build/fnetimg
make test       # unit tests + config check
make itest      # integration tests (needs Python 3)
```

You need Linux 5.6 or later, GCC 12+ or Clang 15+, and GNU make. No other libraries are required.

### Install

```sh
sudo make install                    # PREFIX=/usr/local by default
sudo systemd-sysusers                # creates user fnet, adds it to dialout
sudo install -d -o fnet -g fnet /srv/fnet/flex /srv/fnet/tape/6800 /srv/fnet/tape/6809 /srv/fnet/tape/MIKBUG
sudoedit /etc/fnetd/fnetd.conf
sudo fnetd -t -c /etc/fnetd/fnetd.conf          # check the config
sudo systemctl daemon-reload && sudo systemctl enable --now fnetd
sudo usermod -aG fnet $USER                     # lets you run fnetctl
```

To try it without installing, run `build/fnetd -f -c etc/fnetd.conf -l debug`. The `-f` flag keeps it in the foreground and logs to stderr.

### Configure

The config is `/etc/fnetd/fnetd.conf`, an INI file; [etc/fnetd.conf](etc/fnetd.conf) is a commented example. Its sections:

| Section       | Purpose                                                                 |
|---------------|-------------------------------------------------------------------------|
| `[daemon]`    | user, log sink and level, stats file, control socket                    |
| `[defaults]`  | settings every port inherits: roots, timeouts, tape pacing, punch mode  |
| `[tape.cpu]`  | optional processor tag aliases (`6811 = 68HC11`)                        |
| `[port.NAME]` | one per target machine; any default can be overridden here              |

Each port connects to one target's second 6850 ACIA in one of three ways:

```ini
[port.swtpc]            ; local serial port
type   = tty
device = /dev/ttyUSB0
baud   = 19200

[port.coco]             ; fnetd connects out to a terminal server (RFC-2217)
type   = rfc2217
host   = ts1.lan
port   = 7001
baud   = 38400

[port.lab]              ; a terminal server connects in to fnetd
type   = listen
bind   = 192.168.1.10
port   = 7101
allow  = 192.168.1.50/32 ; required: with no allow list, every connection is refused
baud   = 9600
```

Files are laid out like this:

```
/srv/fnet/flex/          flexnet_root: .DSK images (subdirectories allowed)
/srv/fnet/tape/<CPU>/    tape_root: S-Record files, one directory per CPU tag
```

To apply config changes, run `systemctl reload fnetd` or `fnetctl reload`. Ports whose settings didn't change keep their sessions.

### Use: FlexNet

The usual NETDRV/FNETDRV drivers and R\*/F\* utilities (RMOUNT, RDIR, FMOUNT, ...) work unchanged on the target.

- Paths are relative to `flexnet_root` and cannot leave it.
- The legacy `M` (RMOUNT) command mounts on `legacy_drive`. Set that to the drive where your NETDRV is installed.
- REXIT ends the session; the daemon keeps running.

### Use: Paper tape (S-Record)

From the target, send a selection string out the second ACIA, then the control characters:

```
|6800:GAME.S19|6800:DUMP.S19<CR>   select load file | punch file
DC1  start / resume load           DC3  pause load
DC2  start punch                   DC4  stop punch, close file
```

- An empty field keeps the current selection, and `-` clears it.
- If DC1 is sent with no load file selected, fnetd sends an `S9` record so the loader finishes.

### Manage the running daemon

```sh
fnetctl status
fnetctl ports
fnetctl sessions
fnetctl drives swtpc
fnetctl mount swtpc 1 /games/ADVENT.DSK [--ro] [--wait]
fnetctl umount swtpc all
fnetctl tape set swtpc load=6800:GAME.S19 punch=6800:OUT.S19
fnetctl port set swtpc legacy_drive=3
fnetctl log level --port swtpc trace       # hex dump of every byte on that port
fnetctl stats
```

- Runtime changes are not saved to the config file.
- Logs go to the journal (`journalctl -u fnetd`), and per-port counters go to `/run/fnetd/stats.json`.

### Disk images

```sh
fnetimg create GAMES.DSK --tracks 80 --sectors 36 [--track0 20] [--label GAMES]
fnetimg info  GAMES.DSK
fnetimg ls    GAMES.DSK
fnetimg check GAMES.DSK
```

## Claude stuff

### Use claude to create a plan, have it query you to better understand the functionality.

We have 4 problems:

1. Serial boot (snet) by DeRamp
2. Flexnet for the 6800
3. Flexnet for the 6809
4. Paper Tape load and punch using the ASCII device control characters
   a. Use the pipe string to select the load and punch files.

A lot of folks have built their own solutions and I seem to be doing the same.

What I want:

- A Linux daemon with CLI tools to make changes to the running daemon without restarting it.
- A solution that works over the serial port and a network connection (RFC-2217)
- A solution that allows a simple boot like snet but is a subset of Flexnet
- And still allow for full Flexnet once booted.
- Support for an disk format that Flex supports (all the way to 16M)
  - Flexnet for Flex 1.0? (disk format is different)
- Simplify the protocol (R, not READ cmd - for example)
- Unified code to support the 6800 and the 6809
  - extend for 6801, 68HC11 and 68K
- A Linux server that can support serial connections and network connections
  - this means the possibility of multiple machines
- Management tools

- stretch goal
  - Possible other OSs also
  - Fujinet? no initially
  
#### Paper Tape (S-Record)

- Sends a #21o/#11/DC1 - Device Control 1 to Start Paper Tape
- Sends a #23o/#13/DC3 - Device Control 3 to Stop Paper Tape

- Sends a #22o/#12/DC2 - Device Control 2 to Start Paper Tape
- Sends a #24o/#14/DC4 - Device Control 4 to Stop Paper Tape

| Oct | Dec | Hex | Char                   | Function    |
|-----|-----|-----|------------------------|-------------|
| 021 | 17  | 11  | DC1 (device control 1) | Load Start  |
| 022 | 18  | 12  | DC2 (device control 2) | Punch Start |
| 023 | 19  | 13  | DC3 (device control 3) | Load Stop   |
| 024 | 20  | 14  | DC4 (device control 4) | Punch Stop  |


The <CPU> gives it the fnet service directory to start with.

User can add the text to memory and then call the println routine with the string address in X
