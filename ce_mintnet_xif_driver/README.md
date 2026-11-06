# CE_ETH.XIF - MiNTnet network over CosmosEx pico HDD

The MiNTnet interface driver for the merged pico HDD board (`devices/v40_pico_hdd`, firmware `ce_pico_hdd`). It's the MiNT
counterpart of `../ce_sting_stx_driver`: the same HOSTMOD 7 commands, answered on the pico, without the CE host. MiNT's own
stack (`inet4.xdd`) does everything from ARP up, this driver only moves whole ethernet frames - the "frame mode" of the
protocol, described in [../ce_sting_stx_driver/README.md](../ce_sting_stx_driver/README.md).

```
 MiNT program -> inet4.xdd (TCP / UDP / IP / ARP) -> CE_ETH.XIF --ACSI / SCSI--> pico (sting_net.cpp) -> W6100 socket 0 (MACRAW) -> LAN
```

**Status: builds, but not run yet - neither on hardware nor in an emulator.** It needs pico firmware with protocol
version 2. What was checked is listed under "How it was checked" below.

## Files

| File | What |
|---|---|
| `main.c` | the driver: `init`, open / close / output / ioctl, the poll |
| `mint.h` | the part of the kernel and `inet4.xdd` interface this needs, rewritten from FreeMiNT's headers |
| `xifglue.s` | entries for the `-mshort` kernel into the gcc code |
| `startup.s` | first code in the program, `inet4.xdd` jumps there |
| `../ce_sting_stx_driver/cenet.c`, `purecall.s` | shared with the STinG driver, built here too |

## Build

`make` here (needs `m68k-atari-mint-gcc`, builds `../libacsiscsi` if needed), or `docker/build_drivers.sh` from the repo root.
The result is `ce_eth.xif`, also copied to `../ce_cores/configdrive/drivers/`, so it's on the config drive in its `DRIVERS`
folder. It's 68000 code, which runs on any MiNT kernel.

## Setup

1. Copy `CE_ETH.XIF` into MiNT's system folder, next to `inet4.xdd`, e.g. `C:\MINT\1-19-68B\`. `inet4.xdd` loads all `*.xif`
   from there at boot, and prints the MAC under "Loading interfaces:".
2. Bring the interface up with a free static address in your network, for example:
   ```
   ifconfig en0 addr 192.168.123.181 netmask 255.255.255.0 up
   route add 192.168.123.0 en0
   route add default en0 gw 192.168.123.1
   ```
3. `ping 192.168.123.1`, `netstat`, `arp` to look around. The name servers are in `/etc/resolv.conf`.

The name is set by the driver (`en`), the number comes from `if_getfreeunit()`, so the first one is `en0`. MAC address: the
same one STinG uses on this device; `ifconfig en0 hwaddr` isn't supported, the pico decides it. Don't use STinG with the
driver at the same time - they share W6100 socket 0.

Plain 68000 machines need a 68000 kernel: the FreeMiNT 1.19 "TT / Falcon / clones" package has only 030 and up.

## How it works

- **`init`** (at boot, supervisor mode): allocates the DMA buffers, finds the device (CE_DD's ID from the `CEPI` cookie, then
  a scan), checks the protocol version, registers the interface. Fails cleanly when there's no device, so MiNT boots anyway.
- **`open` / `close`** (`ifconfig up` / `down`): CONFIG on, in frame mode, or off.
- **`output`**: `eth_build_hdr()`, queues the frame to `nif->snd`, and asks for a poll as soon as possible. No bus access.
- **`poll`**: the only place using the bus while running. Skips the round when `FLOCK` is set, else sends everything queued
  in one SEND (up to 8 KB) and gets what's waiting with RECV. Every 20 ms for a second after traffic, every 100 ms otherwise.

It runs from `addroottimeout()`, which is why this differs from the old EtherNEC driver's way of hooking the 200 Hz
interrupt. Root timeouts run in kernel context with interrupts on, so `hz_200` counts on and the timeouts of libacsiscsi
work. They are counted in 20 ms steps and run at a context switch, so more than 50 polls per second isn't possible.
`nif->timer` is no help - the kernel calls `nif->timeout` once per second whatever it says.

Things which would be easy to get wrong:

- **Memory for DMA.** The kernel loads a XIF with `kmalloc()`, so its BSS can be in Fast RAM on TT / Falcon, out of reach for
  ACSI DMA. The buffers come from `Mxalloc()` with ST RAM only + `F_KEEP`, and `netSetDmaBuffer()` gives cenet.c one for
  IDENTIFY too. `dmabuf_alloc()` from `struct kerinfo` looks right for this, but it's NULL in the kernel.
- **`-mshort`.** The kernel needs it, this code (like libacsiscsi) is built without. Calls into the kernel go through
  `pc_call_*()`, see the macros in `mint.h`; `output` and `ioctl` are entered through `xifglue.s`. `short` results must be cast
  to `int16`, the upper half of `d0` is undefined then.
- **`maxpackets = 5`** limits MiNT's TCP receive window to 5 full segments (`tcputil.c`), about 7.6 KB with headers, so a
  burst fits the W6100's 8 KB receive buffer of socket 0 even when the pico is busy with an ACSI command.
- **`eth_build_hdr()` may return another BUF**, the passed one isn't valid after it. FreeMiNT's `pamsdma.c` queues the old one.
- **The program start.** The linker puts its own stub with a jump to the entry point at the start of text, as in the
  XIFs FreeMiNT ships. That entry point is `startup.s`, because it's linked first.

## How it was checked

Checked against FreeMiNT's `master` sources on GitHub, and the binaries of the 1.19 package:

- builds and links without warnings; the structure offsets have `_Static_assert`s, with the numbers of `netif` counted from
  EtherNE's `IF.I`, which is independent of FreeMiNT's `if.h`
- the entry stub of `ce_eth.xif` points to `startup.s`, the same way as in the shipped `plip.xif`, and its first relocation
  is the `jmp` there
- the disassembled argument offsets of `xifglue.s`
- the pico side in frame mode has host tests (`make -C ce_pico_tests test-hdd`)

Not checked: any of it running. The first test is `ping`.

## Limits / not done yet

- Multicast frames are not delivered, `IFF_PROMISC` isn't supported.
- If `addroottimeout()` ever returns NULL, polling stops until the next outgoing frame.
- Not for MagiCNet: EtherNE's `.MIF` runs on both as it doesn't touch `kerinfo`, this one needs it for the timeouts.
