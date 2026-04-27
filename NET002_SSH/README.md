# NET002_SSH — FujiNet SSH Terminal for Atari 8-bit

An interactive SSH terminal client for the Atari 800/800XL, communicating
with a remote shell via the FujiNet Network Device over SIO.  Supports
VT100 terminal emulation, interrupt-driven I/O (no polling), and ZMODEM
file receive.

## Requirements

- **FujiNet** (hardware) or **FujiNet-PC** (software emulation) with SSH
  support (libssh compiled in).
- **cc65 toolchain** (`cl65`, `ca65`, `ld65`) for building.
- An Atari 800/800XL or compatible emulator (e.g. Altirra) with FujiNet
  attached as the network device.

## Building

```bash
./build.sh
```

Or manually:

```bash
cl65 -t atari -o build/ssh_terminal.com src/ssh_terminal.c src/intr.s
```

Output: `build/ssh_terminal.com` (~18 KB).

## Usage

1. Boot the Atari with FujiNet connected (or FujiNet-PC running).
2. Load and run `ssh_terminal.com`.
3. Enter the SSH host address, login, and password when prompted.
4. Use the terminal normally.  Type `exit` or `logout` to disconnect.

To receive a file via ZMODEM, run `sz filename` on the remote host.
The terminal detects the ZMODEM sequence automatically, prompts for
binary or text mode, and saves the file to `D:`.

## Controls

| Key         | Action                                    |
|-------------|-------------------------------------------|
| RETURN      | Send CR to remote shell                   |
| BACKSPACE   | Send BS (0x08) to remote                  |
| Arrow keys  | Send VT100 cursor sequences (ESC[A/B/C/D) |
| CTRL+A..Z   | Send control characters (0x01..0x1A)      |
| TAB         | Send HT (0x09) to remote                 |

## Architecture

### Direct SIO — No CIO/NDEV.COM

This terminal talks to FujiNet directly through the Atari SIO bus,
bypassing both CIO and the NDEV.COM N: device driver entirely.  Every
network operation is a raw SIO command written to the DCB registers at
`$0300`:

| SIO Command | Code   | Direction | Purpose                          |
|-------------|--------|-----------|----------------------------------|
| OPEN        | `0x4F` | Write     | Open SSH connection via URL       |
| STATUS      | `0x53` | Read      | Query bytes available + connected |
| READ        | `0x52` | Read      | Read available bytes              |
| WRITE       | `0x57` | Write     | Send bytes to remote shell        |
| CLOSE       | `0x43` | None      | Close connection                  |

The FujiNet Network Device is addressed at device ID `0x71`, unit 1.
The connection URL follows the format:

```
N:SSH://user:password@host:port/
```

Advantages of direct SIO over CIO/NDEV.COM:
- No need to load NDEV.COM driver.
- Full control over SIO timing and buffer sizes.
- Works with any FujiNet firmware (no CIO handler version dependencies).

### PROCEED Interrupt — Eliminating Polling

The key optimization in this terminal is the use of the SIO PROCEED
interrupt to eliminate STATUS polling.  Without it, the main loop would
call `net_status()` on every iteration — each a full SIO bus transaction
taking ~2 ms — even when no data is available.

#### How PROCEED Works

FujiNet asserts the SIO PROCEED line (directly wired to PIA CA1)
whenever data is available for reading.  On the Atari, the PIA generates
an IRQ when CA1 transitions, and the OS dispatches through the VPRCED
vector at `$0202`.

#### The Rapid-Pulse Problem

FujiNet-PC (and real FujiNet hardware) pulses the PROCEED line rapidly
in a `-_-_-_-_` oscillation pattern rather than holding it steady.  A
naive interrupt handler that simply sets a flag and returns would cause
the CPU to be trapped in continuous IRQ re-entry: each RTI allows one
instruction before the next edge triggers another IRQ.  The main loop
never gets CPU time.

#### The Solution: Disable-on-Entry, Re-enable-on-Process

The interrupt handler (`intr.s`) disables the PROCEED interrupt at the
PIA level before returning:

```asm
_ih:    LDA     PACTL       ; PIA port A control register ($D302)
        AND     #$FE        ; clear bit 0 — disable PROCEED IRQ
        STA     PACTL
        LDA     #$01        ; set trip flag
        STA     _trip
        PLA                 ; restore A (pushed by OS IRQ dispatcher)
        RTI
```

The main loop re-enables the interrupt after processing data:

```c
while (1) {
    /* Purely interrupt-driven wait — no polling */
    while (!trip && !key_available())
        ;

    sts = net_status();
    /* ... read and display data ... */

    /* Re-arm: clear flag, re-enable PROCEED IRQ */
    trip = 0;
    POKE(PACTL, PEEK(PACTL) | 1);
}
```

This ensures exactly one IRQ fires per data batch.  The CPU spins in an
empty loop (consuming near-zero SIO bandwidth) until either FujiNet
signals new data or the user presses a key.

#### Installation and Removal

The handler is installed after a successful SSH connection and removed
before disconnect:

```c
static void install_proceed(void) {
    trip = 0;
    old_vprced = PEEKW(VPRCED);            /* save OS vector */
    old_pactl_bit = PEEK(PACTL) & 1;      /* save enable state */
    POKE(PACTL, PEEK(PACTL) & ~1);        /* disable interrupt */
    POKEW(VPRCED, (unsigned)ih);           /* install handler */
    POKE(PACTL, PEEK(PACTL) | 1);         /* enable interrupt */
}

static void remove_proceed(void) {
    POKE(PACTL, PEEK(PACTL) & ~1);        /* disable interrupt */
    POKEW(VPRCED, old_vprced);             /* restore OS vector */
    if (old_pactl_bit)
        POKE(PACTL, PEEK(PACTL) | 1);     /* restore enable state */
    trip = 0;
}
```

#### Kernel Interaction

The Altirra kernel's IRQ dispatcher reads PORTA (`$D300`) before
jumping through VPRCED, which clears the PIA CA1 interrupt flag.  The
handler does not need to clear the flag itself.  SIOV does not modify
PACTL at all (only PBCTL at `$D303` for the SIO command line).

### Keyboard Input — Direct K: Handler

The terminal opens the Atari K: keyboard handler directly on IOCB 6
(`$03A0`), bypassing the E: screen editor.  This is necessary because
E:'s GET BYTE waits for RETURN before delivering input (line-read mode),
which would buffer characters and inject extra bytes into the SSH
stream.

K: delivers each keypress individually as an ATASCII code.  The CH
register (`$02FC`) is polled for key availability (`0xFF` = no key), and
CIO GET BYTE on IOCB 6 retrieves the actual character.

ATASCII-to-ASCII conversion handles:
- EOL (`0x9B`) to CR (`0x0D`)
- Backspace (`0x7E`) to BS (`0x08`)
- Tab (`0x7F`) to HT (`0x09`)
- Arrow keys to VT100 escape sequences
- Control keys (`0x01`..`0x1A`) pass through unchanged

### Screen Output — E: Handler with CIO Block Writes

Display output uses the standard E: handler on IOCB 0.  Performance is
optimized through CIO block writes (`PUT BYTES`, command `0x0B`) instead
of individual `putchar()` calls.  Pre-filled buffers for cursor movement
and space characters allow bulk operations:

| Buffer      | Fill value | Purpose           |
|-------------|------------|-------------------|
| `blk_left`  | `0x1E`     | Cursor left (80)  |
| `blk_right` | `0x1F`     | Cursor right (80) |
| `blk_up`    | `0x1C`     | Cursor up (24)    |
| `blk_down`  | `0x1D`     | Cursor down (24)  |
| `blk_space` | `0x20`     | Space fill (80)   |

A `cursor_moveto(row, col)` function uses these block writes for fast
absolute cursor positioning.  ROWCRS/COLCRS (`$0054`/`$0055`) are read
to determine current position, and the appropriate block buffer is
written with the required count.  Direct POKE to ROWCRS/COLCRS is
avoided for compatibility with VBXE CON 80.

### Screen Width Detection

At startup, the terminal reads RMARGN (`$0053`): a value of 39 indicates
40-column mode, 79 indicates 80-column mode (e.g. VBXE CON 80).  The
detected width is sent to the remote via `stty cols N rows 24`.

### VT100 Terminal Emulation

The terminal implements a state-machine parser for ANSI/VT100 escape
sequences, translating them to ATASCII screen operations.

#### Parser States

| State         | Description                               |
|---------------|-------------------------------------------|
| `ANSI_NORMAL` | Default: process printable chars, detect ESC |
| `ANSI_ESC`    | After ESC: check for `[`, `]`, or VT100 codes |
| `ANSI_CSI`    | CSI sequence: collect params, execute on final byte |
| `ANSI_OSC`    | OSC string: skip until BEL or ST          |

#### Supported CSI Sequences

| Sequence         | Code | Function                     |
|------------------|------|------------------------------|
| `ESC[nA`         | CUU  | Cursor up N lines            |
| `ESC[nB`         | CUD  | Cursor down N lines          |
| `ESC[nC`         | CUF  | Cursor forward N columns     |
| `ESC[nD`         | CUB  | Cursor backward N columns    |
| `ESC[r;cH`       | CUP  | Cursor position (row, col)   |
| `ESC[r;cf`       | HVP  | Same as CUP                  |
| `ESC[nJ`         | ED   | Erase in display (0/1/2/3)   |
| `ESC[nK`         | EL   | Erase in line (0/1/2)        |
| `ESC[nL`         | IL   | Insert N lines               |
| `ESC[nM`         | DL   | Delete N lines               |
| `ESC[nP`         | DCH  | Delete N characters          |
| `ESC[s`          | SCP  | Save cursor position         |
| `ESC[u`          | RCP  | Restore cursor position      |
| `ESC[...m`       | SGR  | Set graphic rendition (ignored) |
| `ESC[...h/l`     |      | Mode set/reset (ignored)     |
| `ESC[t;br`       | DECSTBM | Set scroll region         |
| `ESC[6n`         | DSR  | Device status report (sends cursor pos) |

#### Supported Escape Sequences

| Sequence | Function                                        |
|----------|-------------------------------------------------|
| `ESC D`  | IND — Index (cursor down, scroll at bottom)     |
| `ESC M`  | RI — Reverse index (cursor up, scroll at top)   |
| `ESC 7`  | DECSC — Save cursor position                    |
| `ESC 8`  | DECRC — Restore cursor position                 |
| `ESC c`  | RIS — Full reset (clear screen, reset regions)  |

#### Character Mapping (ASCII to ATASCII)

| ASCII     | ATASCII    | Action               |
|-----------|------------|----------------------|
| LF (0x0A) | 0x1D/0x9B  | Cursor down or scroll |
| CR (0x0D) | —          | Move to column 0     |
| BS (0x08) | 0x1E       | Cursor left          |
| TAB (0x09)| 0x20...    | Spaces to next tab stop |
| BEL (0x07)| 0xFD       | Atari bell           |
| 0x20-0x7E | same       | Printable characters |

#### Scroll Region Support

The terminal supports the VT100 DECSTBM (Set Top and Bottom Margins)
sequence.  Scroll operations within a defined region use a two-step
delete/insert algorithm: the native Atari delete-line (`0x9C`) and
insert-line (`0x9D`) codes affect the full screen, so scrolling within
a restricted region requires deleting at one edge and inserting at the
other to restore lines outside the region.

Full-screen scrolling uses the native EOL character (`0x9B`) at the last
row for maximum speed.

### Remote Terminal Configuration

After connection, the terminal automatically configures the remote
shell:

1. **Drains** the initial MOTD/prompt output (waits up to 30 VBI
   frames).
2. **Sets** `TERM=vt100` and `PROMPT_EOL_MARK=''` (disables zsh's
   partial-line marker which uses ESC[K sequences incompatible with
   the Atari's non-delayed autowrap).
3. **Sets** terminal dimensions via `stty cols N rows 24`.
4. **Clears** the screen via `clear` (output flows through the VT100
   parser to initialize the display).

### ZMODEM Receive Protocol

The terminal implements a ZMODEM receive-side protocol for downloading
files from the remote host to `D:` on the Atari.

#### Auto-Detection

A 4-state finite state machine watches the incoming byte stream for the
ZMODEM auto-download sequence `** ZDLE B` (hex: `2A 2A 18 42`).  On
match, the 4 bytes are pushed into a LIFO pushback buffer so
`zm_read_header()` can read them as a normal ZRQINIT frame header.

#### Protocol Flow

1. Detect `**\x18B` in SSH output stream.
2. Disable PROCEED interrupt (ZMODEM uses direct STATUS polling for
   maximum throughput).
3. Read ZRQINIT from sender.
4. Send ZRINIT (advertise CANFDX, 1024-byte max data length).
5. Read ZFILE header and filename subpacket.
6. Prompt user for binary or text transfer mode.
7. Open file on `D:` via CIO IOCB 1.
8. Send ZRPOS 0 (start from beginning).
9. Receive ZDATA frames, writing to file.  Respond with ZACK to
   ZCRCQ and ZCRCW subpackets.
10. On ZEOF, close file.
11. Send ZRINIT, receive ZFIN, send ZFIN.
12. Consume "OO" (Over and Out) and drain trailing bytes.
13. Re-enable PROCEED interrupt.
14. Send CR so the shell prompt reappears.

#### Data Reception Optimization

The `zm_read_data_to_file()` function is batch-optimized: it scans
`rxbuf` directly for ZDLE markers and copies non-escaped byte runs via
`memcpy` instead of per-byte function calls.  Data is accumulated in a
1024-byte I/O buffer (`zm_iobuf`) and flushed to disk in full blocks.

#### Text Mode

When text mode is selected, `zm_file_write()` converts LF (`0x0A`) to
Atari EOL (`0x9B`) and strips CR (`0x0D`) on the fly.

#### CRC

CRC-16-CCITT is implemented (bit-by-bit, no lookup table) for
generating CRC values in outgoing hex headers.  Incoming CRC bytes are
consumed but not yet verified (marked as TODO).

#### File I/O

File operations use CIO IOCB 1 (`$0350`):
- OPEN with write mode (`AX1=0x08`)
- PUT BYTES for block writes
- CLOSE on completion

Filenames from the remote host are sanitized for Atari DOS: path
components are stripped, illegal characters replaced with `_`, and the
`D:` prefix and CIO terminator (`0x9B`) are added.

#### Progress Display

During transfer, a progress bar is displayed: `D:filename [.......] N bytes received.`  Each dot represents 1 KB of data received.

### SIO Buzzer

The SIO buzzer sound is silenced during terminal operation by zeroing
SOUNDR at `$41`.  The original value is restored on exit.

## Source Files

| File                | Lines | Description                                    |
|---------------------|-------|------------------------------------------------|
| `src/ssh_terminal.c`| ~1935 | Main terminal: SIO, keyboard, VT100, ZMODEM   |
| `src/intr.s`        | 22    | PROCEED interrupt handler (6502 assembly)      |
| `build.sh`          | 12    | Build script                                   |

## Memory Layout

Typical memory map (from linker output):

| Segment | Address Range   | Size     |
|---------|-----------------|----------|
| CODE    | `$206F`–`$64EE` | ~17.5 KB |
| RODATA  | `$64EF`–`$685B` | ~0.9 KB  |
| DATA    | `$685C`–`$68E8` | ~0.1 KB  |
| BSS     | `$68E9`–`$7570` | ~3.2 KB  |

Total binary size: ~18 KB.  The BSS segment (uninitialized data) includes
the 1024-byte receive buffer, 1024-byte ZMODEM I/O buffer, and various
state variables.  `_trip` is at the start of BSS (`$68E9`).

## Limitations

- **No ZMODEM send.**  Only file receive (from remote to Atari) is
  implemented.  Sending files from Atari to remote is not supported.
- **CRC verification is not performed** on received ZMODEM data.  CRC
  bytes are consumed but not checked against the data.
- **No color or attribute support.**  SGR sequences (bold, underline,
  colors) are silently ignored.  The Atari display is monochrome text.
- **Single file per ZMODEM session.**  Batch file transfers are not
  handled.
- **No delayed autowrap.**  The Atari E: handler wraps immediately when
  writing at the last column, unlike VT100's delayed wrap.  This can
  cause minor display differences with some programs.

## License

Public domain.

## Authors

Claude / Piotr, 2026.
