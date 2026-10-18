# fnet
A Linux Server that support remote file sharing for MIKBUG, Flex 6800 and Flex 6809.

# Description

Consider this a precursor to building a Fujinet board for Flex and MIKBUG (with paper tape device control control). For right now I'll concentrate on getting the Linux fnet server working with MIKBUG ROM monitor, Flex for the 6800 and Flex for the 6809.

# Use claude to create a plan, have it query you to better understand the functionality.

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
  
## Paper Tape (S-Record)

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

SOT<ASCII Load filename>FS<ASCII Punch Filename><EOT>

Better:
|<CPU>:<ASCII Load filename>|<CPU>:<ASCII Punch Filename><CR> or <NL> or <EOT> or <NULL>

The <CPU> gives it the fnet service directory to start with.

User can add the text to memory and then call the println routine with the string address in X
