# CE_STING.PRG - fake STinG for cosmo2ex

A STinG replacement for the AUTO folder. It installs the STinG API (`STiK` cookie, `TRANSPORT_TCPIP` layer), and every
TCP / UDP / ICMP / resolve call goes to the CosmosEx network module (`HOSTMOD_NETWORK_ADAPTER`, ACSI / SCSI
commands `NET_CMD_*` in `ce_commands.h`). The host side does the real networking with Linux sockets. That's faster than
real STinG doing TCP on the 68000.

Real STinG with `../ce_sting/CE_ETH.STX` is the other option, and the reference for compatibility. Compare the two
with `../test_ce/test_sting` and `../test_ce/sting_server/sting_diff.py` against
`../test_ce/sting_server/reference/sting126_tt.log`.

**Status (2026-11-03):** works on a TT (TOS 3.06, SCSI). In `../test_ce/test_sting` (menu **A**) it gives the same
results as real STinG 1.26 for all 911 compared values, except one on purpose: `resolve("localhost")` gives 127.0.0.1,
real STinG asks DNS for `localhost.sting.org`. The host side is `../ce_cores/hdd/network/` (`NetAdapter`), with unit
tests in `../ce_cores/tests/test_netadapter.cpp`.

## Install

`CE_STING.PRG` is on the config drive in `DRIVERS` (next to `CE_DD.PRG` and `CE_ETH.STX`). Put it into the AUTO folder,
or start it from the desktop. It finds the CosmosEx through CE_DD (boot sector or AUTO), or by scanning the buses. Use either real STinG or this one, not both: STinG installs the
`STiK` cookie, and this one then quits with "Already installed". It doesn't need `CE_ETH.STX` or STinG's config files.
The host does the networking with its own IP (the one of its default route), and the ST sees that as its local IP.
ICMP (ping, ICMP errors for the handlers) needs `ce_cores` running as root - it uses a raw socket.
On a Cygwin host `TCLOSE_WAIT` (remote closed) shows only after the program read the data still waiting - Linux and
macOS see it right away, as real STinG does (`NetAdapter::seesRemoteCloseWithData()`).

## How it matches STinG 1.26

Protocol 0x0101 (`ce_commands.h`, host: `netadapter_commands.h`) - what was added to the ce-atari version, so the fake
behaves like real STinG:

- per handle a one-shot error (`E_RRESET`, `E_REFUSE` - returned once, then `E_EOF`) and an "in use" flag in
  `CN_UPDATE_INFO`, states `TCLOSE_WAIT` (remote closed, sending still works), `TTIME_WAIT` / `TLAST_ACK` lingering
  after `TCP_close()` (a 2nd close gives `E_BADCLOSE`), `E_OBUFFULL` at 2x the `TCP_open()` buffer size unacknowledged
- 96 handles, given round robin (a closed one isn't reused right away), two `TCP_PASSIVE` on the same port allowed
- `TCP_close()` gets the timeout, `CNgets()` gets the line length (a line may contain 0 bytes)
- `CN_PEEK` (0x48): up to 4000 bytes of the stream into a local line cache, see Speed

On the ST: STinG's order of checks (pending error, `E_LISTEN`, `E_EOF` only when no data is left), `TCP_info()` with the
`TCPIB` layout, the error texts of 1.26 (`-31` gives two texts glued together, as in STinG), case insensitive
`getvstr()`, working `setvstr()`, ports `Internal` and `CosmosEx` for `query_port()`, ICMP handlers called newest
first, CIBs updated from the VBL (apps watch `CNgetinfo()->rhost` while waiting for a connection).

**Trap in this code:** `globdefs.h` has `typedef int int16` / `unsigned int uint16` - 32 bit with gcc. STinG apps
push 16 bit words, so `tpl_middle.c` reads `int16` parameters with `getIntFromSP()` (sign extends) and `uint16` ones
with `getWordFromSP()` (zero extends) - gcc relies on the extension done by the caller. And structures apps read
(`CIB`, `NDB`, `TCPIB`, `PORT`) must be built through byte offsets (`setCIB()`, `PO_*`, `storeWord()`), not gcc fields.

## Speed (TT, 2026-11-03)

`../test_ce/test_sting` menu **S** and **D** (FTP download of a 4 MB file without GEM), against the CE host:

| | real STinG + CE_ETH.STX | fake STinG |
|---|---|---|
| TCP receive, `CNgets()` (1000 lines) | 71 kB/s | 148 kB/s |
| TCP receive, `CNget_block()` (1.7 MB) | 95-145 kB/s | 890 kB/s |
| TCP receive, `CNget_NDB()` (1.7 MB) | 116-145 kB/s | 315 kB/s |
| TCP send (100 kB) | 82 kB/s | 588 kB/s |
| FTP download 4 MB to C: (menu **D**) | - | 524 kB/s (902 kB/s without the `Fwrite()` time) |

What it took:

- `CNgets()` and `CNget_char()` take up to 4000 bytes with one `CN_PEEK` into a shared 4 KB line cache, lines are found
  there - before, each line was one ACSI command (57 kB/s). The host keeps the bytes until the ST reports them used
  (16 bit count in the next `CN_PEEK`, above 255 the byte-count commands send a short `CN_PEEK` first).
- No forced `CN_UPDATE_INFO` after local reads: the fake lowers its copy of the host's byte count by what the host takes
  out (`hostRemoved()`). Before, `CNbyte_count()` after each `CNget_char()` cost a 3 sector status update.
- The update rate limit counts VBLs (`vblTicks`) - no `Supexec()` for the 200 Hz timer on each API call.

GEM FTP clients are much slower, and it isn't the stack: both read 4 KB with `CNget_block()` per GEM event loop round,
and then wait. On the fake STinG MG-FTP gets 18-22 kB/s (a round each ~180 ms, ~10 ms of it STinG + `Fwrite()`), aFTP
1.55 about 6 kB/s (a round each ~560 ms - its history says "evnt_timer depends on cps", the wait gets longer when the
measured speed is low, so it stays low). The host meanwhile has 40-70 KB waiting in the socket. A client reading in a
loop without GEM (menu **D**) gets the 524 kB/s above - with 32 KB `Fwrite()`s the disk is then 40% of the time.

## History

From `ce-atari/cosmosex_fakesting` (2014-2019). In 2024 it was moved to the shared libacsiscsi, then removed for CE
lite. That 2024 version had 3 bugs, fixed in the port:

- `requiredVersion` was never set, because the old device search that read it was dropped, so it never installed.
  `cecmd.c` finds the device again like CE_ETH.STX does (CE_DD's `CEPI` cookie first, then the buses) and reads
  the version and local IP from `NET_CMD_IDENTIFY`.
- `hdIf.cmd()` and `getTicks()` of libacsiscsi need supervisor mode, but the STinG API runs in the application's user
  mode. That would be 2 bombs on the first call. `ceCmd()` / `ceTicks()` use `Supexec()`, or a direct call from the VBL.
- TT SCSI DMA needs longword aligned buffers and can't reach TT fast RAM. The DMA and middle buffers are aligned now,
  application buffers that DMA can't use go through the middle buffer (`ceNeedsBounce()`), and NDB data are aligned.

And one older bug: `CNget_NDB()` read whole sectors into a buffer of the exact byte size, so it overwrote up to 511
bytes of STinG's memory pool after it.

## Build

`make` (needs `m68k-atari-mint-gcc`, builds `../libacsiscsi` if needed). The result is `ce_sting.prg`, copied also to
`../ce_cores/configdrive/drivers/`. `../docker/build_drivers.sh` and `../release/build.sh` build it with the other
drivers.
