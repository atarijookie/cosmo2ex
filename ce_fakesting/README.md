# CE_STING.PRG - fake STinG for cosmo2ex

A STinG replacement for the AUTO folder. It installs the STinG API (`STiK` cookie, `TRANSPORT_TCPIP` layer), and every
TCP / UDP / ICMP / resolve call goes to the CosmosEx network module (`HOSTMOD_NETWORK_ADAPTER`, ACSI / SCSI
commands `NET_CMD_*` in `ce_commands.h`). The host side does the real networking with Linux sockets. That's faster than
real STinG doing TCP on the 68000.

Real STinG with `../ce_sting/CE_ETH.STX` is the other option, and the reference for compatibility. Compare the two
with `../test_ce/test_sting` and `../test_ce/sting_server/sting_diff.py` against
`../test_ce/sting_server/reference/sting126_tt.log`.

**Status (2026-11-03):** the Atari side is ported to the current `../libacsiscsi`. The host side is in
`../ce_cores/hdd/network/` (`NetAdapter`, ported from `ce-atari/ce_main_app/network`) and has unit tests in
`../ce_cores/tests/test_netadapter.cpp`. Neither has run on an ST yet.

Use either real STinG or this one, not both: STinG installs the `STiK` cookie, and this one then quits with
"Already installed". It doesn't need `CE_ETH.STX` or STinG's config. Networking is done by the host with its own IP
(the one of its default route), which the ST sees as its local IP.

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

`make` (needs `m68k-atari-mint-gcc`, builds `../libacsiscsi` if needed). The result is `ce_sting.prg`.
