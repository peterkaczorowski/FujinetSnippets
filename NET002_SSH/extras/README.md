# Extras

## `altirra-custom-device/netsio.atdevice`

Modified NetSIO custom device script for the Altirra emulator with
HSIO (High-Speed SIO) support.  This file bridges the Altirra emulator
to FujiNet-PC over a network socket, handling the SIO protocol
translation including high-speed transfers.

### HSIO Changes

Four modifications were made to the original `netsio.atdevice` to
support HSIO negotiation initiated by the Atari side:

1. **`int hsio_pending`** (line 55) — new state variable, tracks
   whether a `$3F` (Get High Speed Index) command was recently sent
   to a network device.

2. **`$3F` detection** (line 160) — in `debug_command_frame()`, when
   command `$3F` is seen for a network device (`$71`), sets
   `hsio_pending = 1`.

3. **Speed detection** (lines 452–473) — in `rxbyte_thread_handler()`,
   when a baud rate change is detected (`out_cpb != $aux`):
   - If `hsio_pending == 1`: updates **both** `out_cpb` and `in_cpb`
     locally, clears `hsio_pending`, and does **not** send event `$80`
     to FujiNet-PC.
   - If `hsio_pending == 0`: standard behavior — updates `out_cpb`
     only and sends event `$80` to FujiNet-PC.

4. **Cold reset** (line 647) — sets `hsio_pending = 1` on system
   reset.  This ensures pre-patched ROMs (which start at high speed
   immediately without sending `$3F`) work correctly — the first
   speed change after reset is treated as HSIO negotiation (local-only
   update, no event `$80`).

### Why No Event `$80`

When HSIO is negotiated, NetSIO must NOT send event `$80` (baud rate
change notification) to FujiNet-PC.  In FujiNet-PC's `handle_netsio()`
function, if `_baud_peer` differs from `_baud` by more than 10%, all
received bytes are XOR-corrupted as a speed-mismatch error indicator.
Since FujiNet-PC's internal `_baud` stays at 19200 while the SIO bus
switches to ~112 kbit/s, sending `$80` would trigger this corruption.

The local-only update (changing both `in_cpb` and `out_cpb` inside
NetSIO without notifying FujiNet-PC) avoids the problem.  FujiNet-PC
continues to send/receive data at its own rate, and NetSIO handles
the speed translation transparently.

### Installation

Replace the `netsio.atdevice` file in your Altirra custom device
configuration with this version.  The file is typically loaded via
Altirra's **System > Configure System > Devices > Custom Device**
dialog.
