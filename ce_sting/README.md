# CE_ETH.STX - STinG network over CosmosEx pico HDD

A STinG port driver for the merged pico HDD board (`devices/v40_pico_hdd`, firmware `ce_pico_hdd`). The Atari runs the
original STinG kernel with its TCP, UDP and ICMP modules. This driver only moves IP datagrams between STinG and the pico.
The pico puts them on the ethernet as raw frames through the W6100's MACRAW socket.

Why it's done this way: an earlier CosmosEx network ("fake STinG", `ce-atari/cosmosex_fakesting`) replaced STinG's API
with Linux sockets, and existing ST programs didn't always work with it. Here STinG itself handles the sockets, so
programs behave exactly as on any other STinG network card. The W6100's 8 hardware sockets don't limit the number of
STinG connections either - STinG's own memory pool (`ALLOCMEM`) does, as with every other STinG port.

```
 ST program -> STinG (TCP / UDP / IP) -> CE_ETH.STX --ACSI / SCSI--> pico (sting_net.cpp) -> W6100 socket 0 (MACRAW) -> LAN
                                                                     pico: ARP, MAC filter     sockets 1..3: HDD / IKBD / UDP to CE host
```

- STinG gets its **own MAC address**: the pico's MAC with bit 2 of the first byte flipped (still locally administered,
  unicast). The W6100's TCP / UDP sockets keep using the pico's MAC and IP. Socket 0 runs without the chip's MAC filter,
  and the pico keeps only frames for STinG's MAC plus broadcast ARP. No clash between the two.
- STinG needs its **own IP address** (static, set in STinG - see Setup). STinG has no DHCP client.
- The **pico answers ARP** for STinG's IP and resolves the next hop MAC for outgoing packets. The ST sees no ARP and no
  broadcast traffic.
- The commands are answered on the pico, **not forwarded to the CE host** - networking works with the host off too
  (but only after the pico got its own IP from DHCP, and while it's trying to connect to the host, answers can be slow).

## Files

| File | What |
|---|---|
| `main.c` | STinG module: loading, `DRIVER` / `PORT`, the 4 driver functions |
| `cenet.c`, `cenet.h` | the commands to the pico, through `../libacsiscsi` |
| `sting.h` | the part of STinG's API this needs, rewritten for gcc |
| `purec.s` | calling convention glue between gcc and STinG (Pure C), fast copy |
| `startup.s` | program start |
| `../ce_pico_hdd/sting_net.cpp` | the pico side |
| `../ce_pico_tests/tests/hdd/test_sting_net.cpp` | host tests of the pico side |

## Build

`make` here (needs `m68k-atari-mint-gcc`; builds `../libacsiscsi` if needed), or `docker/build_drivers.sh` from the repo
root. The result is `ce_eth.stx`, copied also to `../ce_cores/configdrive/drivers/` next to `ce_dd.prg` - so it's on the
config drive, in its `DRIVERS` folder. The program flags are set to no fast RAM, so its DMA buffers (in BSS) are in ST RAM.

### Calling convention

STinG is built with Pure C. Its `cdecl` functions take arguments on the stack in their real size (`int16` = 2 bytes),
return the result in `d0` (pointers too - STinG's own assembler port drivers read `KRmalloc()` that way), and may destroy
`d0-d2/a0-a2`. This gcc (no `-mshort`, same as libacsiscsi) puts every argument on the stack as 4 bytes and expects `d2/a2`
to survive. So:

- every call into STinG goes through `pc_call_*()` in `purec.s` - see the macros in `sting.h` (`KRmalloc`, `IP_discard`, ...)
- STinG calls `set_state()` and `cntrl()` with `int16` arguments, so they're entered through `pc_set_state` / `pc_cntrl`,
  which widen them. `send()` / `receive()` take just a pointer and are plain gcc functions.
- `IP_HDR` in `sting.h` has plain bytes instead of STinG's bit fields, `_Static_assert`s check the structure layouts.

## Setup on the Atari

1. Copy `CE_ETH.STX` from the config drive's `DRIVERS` folder into STinG's module folder (where the other `*.STX` are).
2. Give the port named **`CosmosEx`** an IP address and subnet mask in STinG's port setup (`STNGPORT.CPX`), and make sure it's
   activated.
3. `ROUTE.TAB` - route the local network and everything else through it, e.g.:
   ```
   192.168.1.0    255.255.255.0    CosmosEx    0.0.0.0
   0.0.0.0        0.0.0.0          CosmosEx    192.168.1.1
   ```
4. `DEFAULT.CFG` - for speed: `THREADING = 10` (default 50 ms between STinG's polls), and set `NAMESERVER`.

## How the driver works

STinG calls `receive()` and `send()` of every port from its 200 Hz timer interrupt, every `THREADING` ms, in supervisor mode.

- **Both skip the poll when `FLOCK` (0x43e) is set** - somebody (floppy, harddisk driver) uses the DMA chip. The packets
  wait on the pico for the next poll. They also skip while `set_state()` / `cntrl()` talk to the device from user mode.
- `receive()`: one RECV command fetches everything that arrived since the last poll (up to 8 KB). It's sized so STinG's
  memory pool can store all of it (`KRgetfree()` minus a reserve) - what stays on the pico is not lost, what the ST got and
  can't store is. A second RECV follows if the pico says more packets are waiting.
- `send()`: all of STinG's send queue (up to 8 KB) goes in one SEND command, each datagram with its next hop: the
  destination when it's on the local network, the gateway otherwise (same as STinG's `ports/ether`).
- `set_state(TRUE)`: finds the device on the first call, then CONFIG with STinG's IP. It first tries the ID and interface
  of CE_DD (from its `CEPI` cookie, `hdIf.ceDeviceId`), and when that doesn't answer IDENTIFY, it scans all IDs - ACSI on
  ST, ACSI then SCSI on TT, SCSI on Falcon. When STinG's IP changes later, the next poll sends CONFIG again.
- `cntrl()`: `CTL_ETHER_GET_MAC`, `CTL_ETHER_INQ_SUPPTYPE`, `CTL_ETHER_SET_TYPE` / `GET_TYPE` (type 0 only).
  `CTL_ETHER_SET_MAC` is not supported - the pico decides the MAC.

## Protocol

The commands use HOSTMOD **7** (`HOSTMOD_RAW_NET`). HOSTMODs 1..6 belong to the CE host modules, and the host answers an
unknown one as a plain TEST UNIT READY - status OK, but no data. That's how IDENTIFY tells old firmware from new.

All parameters go in the command bytes, so each command is either a DMA read, a DMA write, or no data at all.

### Command formats

```
SCSI(6):  [0] ID << 5 | 0x00   [1] 'C'  [2] 'E'  [3] 0x07   [4] function  [5] parameter
ICD10:    [0] ID << 5 | 0x1F   [1] 0x20  [2] 'C'  [3] 'E'  [4] 0x07  [5] function  [6..10] parameters
```

ICD10 is a 10-byte (group 1) SCSI command in the ACSI ICD form - 11 bytes on ACSI. On SCSI the TT / Falcon send the 10 byte
CDB, and the pico turns it into the same ICD form. libacsiscsi sets bit 7 of the HOSTMOD byte (`0x87`) on a retry - the
pico ignores that bit; the batch numbers below take care of repeated transfers.

| func | Name | Form | Parameters | DMA | Status |
|---|---|---|---|---|---|
| 0x00 | IDENTIFY | SCSI(6) | - | read, 1 sector, 32 B | 0 |
| 0x01 | CONFIG | ICD10 | [6] flags: bit 0 network on, bit 1 receive IP broadcasts; [7..10] STinG's IPv4 | none | 0, 4 = MACRAW socket couldn't be opened |
| 0x02 | SEND | ICD10 | [6..8] byte count (24 bit), [9] batch number, [10] packet count | write, `(count + 511) / 512` sectors | see below |
| 0x03 | RECV | ICD10 | [6] max sectors the ST takes (1..255), [7] batch number | read, up to [6] sectors | 0 = packets, 1 = empty |
| 0x04 | GET_STATS | SCSI(6) | - | read, 1 sector, 32 B | 0 |

Status 2 = bad parameters (also: CONFIG / SEND / RECV not in ICD10 form), 3 = SEND while the network is off.

The parameters are big endian. The pico ends each read with 16 B padding: the last 1..15 bytes of an ST DMA read would
stay in the DMA chip's FIFO and never reach RAM.

### IDENTIFY data

| Offset | Size | What |
|---|---|---|
| 0 | 8 | `"CERAWNET"` |
| 8 | 2 | protocol version (1) |
| 10 | 6 | STinG's MAC address |
| 16 | 1 | ethernet link: 1 up, 0 down |
| 17 | 1 | bit 0: network on (CONFIG), bit 1: MACRAW socket open |
| 18 | 2 | max packet data in one SEND / RECV (16384) |
| 20 | 4 | the pico's own IP (just for info) |
| 24 | 8 | reserved (0) |

### CONFIG

Turning the network on opens W6100 socket 0 in MACRAW mode, empties the queues and sends a gratuitous ARP for STinG's IP.
Turning it off closes the socket. CONFIG again while on just updates the IP / flags.

### SEND data

The packets one after another, each:

```
[0..1]  length of the IP datagram
[2..5]  next hop IPv4 - destination or gateway; 255.255.255.255 goes to the ethernet broadcast
[6..]   the IP datagram (header, options, data)
        1 byte padding when the length is odd, so the next entry starts on an even address
```

The pico sends each datagram in an ethernet frame (STinG's MAC as source, type 0x0800). When the next hop's MAC is not in
its ARP cache yet, it sends an ARP request and keeps the packet (8 slots) until the reply comes - 3 tries, 1 s apart, then
it's dropped. The ARP cache has 16 entries for 10 minutes.

Status: bits 0..6 - 0 OK, 2 bad data, 3 network off. **Bit 7 set: received packets are waiting on the pico.**

**Batch number:** the pico remembers the number of the last SEND it transmitted. A SEND with the same number again (a
retry of a transfer that failed on the ST side after the data got through) is taken but not transmitted again. The driver
uses a new number for each SEND.

### RECV data

A 16 byte header, then the packets:

```
header:
[0]      0xCE
[1]      batch number - the one from the command
[2..3]   bytes of packet data after the header (without the final padding)
[4..5]   packets in this batch
[6..7]   packets still waiting on the pico after this batch
[8]      ethernet link: 1 up, 0 down
[9..15]  reserved (0)

each packet:
[0..1]   length of the IP datagram
[2..]    the IP datagram, padded to even length
```

The total is padded to a multiple of 16. The pico puts in as many packets as fit into the ST's max sectors (at most 16 KB
of packet data); a single packet that would never fit is dropped.

**Batch number:** the pico keeps the last batch until a RECV with a **different** number comes. A RECV with the same
number gets the same batch again. The driver asks for the next number only after it got a batch fine (header 0xCE + its
number), so a failed DMA transfer doesn't lose packets - the next poll asks for the same batch again.

### GET_STATS data

8 big endian 32 bit counters since the pico started: received frames (all), IP packets given to ST, IP packets dropped
(ring full / too big), IP packets sent, IP packets not sent (no ARP reply / no free slot), ARP requests sent, ARP replies
sent, commands with bad parameters. Not used by the driver - for debugging.

### What the pico receives

Frames for STinG's MAC or broadcast, then:

- ARP for STinG's IP: answered; the sender is remembered. ARP replies fill the ARP cache.
- IPv4 to STinG's MAC: stored for RECV (32 KB ring, the ethernet padding of short frames cut off by the IP total length).
- IPv4 broadcast: only with CONFIG flag bit 1.
- everything else (IPv6, other MACs, ...): dropped without reading the rest of the frame over SPI.

The ring drains in the pico's core 0 main loop, and also during long HDD reads from the host, so the W6100's 8 KB receive
buffer of socket 0 doesn't overflow meanwhile.

## Status

Tested on a plain ST (TOS 1.02, ACSI, STinG 1.26) on 2026-11-02:

- ping both ways (LAN -> ST, also from the CE host PC; ST -> host and -> gateway)
- TCP: MG-FTP lists directories and downloads, about 12.5 kB/s - why not faster was not looked into yet
- HDD transfers without STinG unchanged (1225 kB/s); with the network off the pico does no extra SPI traffic

Old FTP clients and servers: MG-FTP waits for `150` before reading the data connection (a server answering `125 Data
connection already open` gives an empty listing), and splits listing lines at LF only (a CR stays in the names). aFTP needs
AES 3.40+ / MiNT and crashes on TOS 1.02.

## W6100 MACRAW notes

Things found on hardware - each one looked like "sent OK" on the pico, but nothing usable got on the wire:

- `Sn_MR` for MACRAW is 0x07 on W6100 (the library's `SnMR::MACRAW` is the W5x00 value 0x04). `EthernetMacraw::begin()`
  tries 0x07 first, then 0x04.
- `EthernetClass::socketSendAvailable()` returns 0 for anything else than a connected TCP socket - not usable for MACRAW.
- `Sn_IR` is read only on W6100, its flags are cleared through `Sn_IRCLR` (0x0028). Writing `Sn_IR`, as the library does
  for TCP / UDP, does nothing there.
- Reading `Sn_TX_WR` gives the value from before the last SEND, so a frame written in parts needs the offset of each part
  in `socketBufferData()` - with offset 0 each part overwrites the previous one and only the last one gets sent (runt
  frames, which the switch drops).
- Never send ARP for STinG's IP from the W6100's own MAC - the peers then send STinG's traffic to the W6100.

The pico counts frames, ARP and drops (GET_STATS) - read them when debugging, there's no periodic log.

## Limits / not done yet

- Static IP only. The pico could get a DHCP lease for STinG's MAC later (IDENTIFY has reserved space for that).
- Subnet broadcasts (e.g. 192.168.1.255) as next hop get no ARP reply, so they're dropped; 255.255.255.255 works.
- `CTL_ETHER_GET_STAT` / `GET_ARP` are not done (GET_STATS could serve them).
- Speed: see below. Left as it is - the fast way to network is the fake STinG (`../ce_fakesting`).
- The pico side has host tests (`make -C ce_pico_tests test-hdd`).

## Speed (TT, 2026-11-03)

Measured with `../test_ce/test_sting`, menu **S** (TT, TOS 3.06, SCSI), against `../test_ce/sting_server` on the CE host:

| | `RCV_WND = 10000` | `RCV_WND = 4000` |
|---|---|---|
| TCP receive, `CNgets()` (1000 lines) | 71 kB/s | 70 kB/s |
| TCP receive, `CNget_block()` (1.7 MB) | 95 kB/s | 116 kB/s |
| TCP receive, `CNget_NDB()` (1.7 MB) | 145 kB/s | 116 kB/s |
| TCP send (100 kB) | 82 kB/s | 82 kB/s |

- With STinG's default `RCV_WND = 10000` the sender's bursts don't fit into the 8 KB MACRAW receive buffer of the W6100
  (socket 0, `W6100_RX_SIZES_KB` in `../third_party/Ethernet/Ethernet.h`) when the pico is busy with an ACSI command -
  frames are dropped, the CE host saw 2-3 TCP retransmission timeouts per second in the `CNget_block()` test. With
  `RCV_WND = 4000` no drops, but the window caps the speed. Something around 7000 (4 full segments fit into 8 KB) should
  be the best of both - not tried.
- The 12.5 kB/s of MG-FTP on a plain ST (above) isn't the limit of this driver - MG-FTP reads 4 KB per GEM event loop
  round and waits between the rounds (see `../ce_fakesting/README.md`).
