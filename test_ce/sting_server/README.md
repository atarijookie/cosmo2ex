# STinG compatibility test

Checks that a STinG clone (fake STinG) behaves like real STinG on the API level that applications use.

The ST side is `test_sting` in `ce_tests.tos` (key **0** in its main menu). It calls the STinG API in normal and edge
cases against this test server and records what each call returned. It doesn't judge the results. The log of real
STinG becomes the reference, and the clone has to produce the same values.

```
 ST: ce_tests.tos / test_sting  --STinG API-->  real STinG (+ CE_ETH.STX)  --LAN-->  sting_server (Linux PC)
                                                fake STinG                 --LAN-->
```

## Steps

1. On a Linux PC in the same LAN as the ST: `make && ./sting_server`. Use `-q` for no log. If port 10000 is taken,
   use `-p <port>` for another base port and enter the same base port on the ST. The firewall must let TCP and UDP
   base+0 .. base+20 through, including base+9. Nothing listens on base+9, so the PC answers with a TCP reset and an
   ICMP 'port unreachable', and the 4001 and 5020 tests need exactly that. If the firewall drops base+9, those tests
   get a timeout instead. The PC must also answer ping (5xxx tests).
2. On the ST with real STinG, run `ce_tests.tos`, press **0**, then **R**. Enter the server IP and base port (both
   remembered in `STNGTEST.INF`), then **A** for all tests. This writes `STNGREAL.LOG` to the root of the current drive.
3. Boot with the fake STinG instead, and run the same steps with **F**. This gives `STNGFAKE.LOG`.
4. `./sting_diff.py STNGREAL.LOG STNGFAKE.LOG` lists every exact value that differs, with error codes and TCP states
   decoded. Add `--fuzzy` to also see the timing-dependent values. The exit code is 1 if any exact value differs.

Repeat the real STinG run once or twice and diff those logs against each other. Any exact value that changes between
two real runs is timing-dependent. It should become fuzzy in the test (`rec_f` instead of `rec_x`).

## Reference

`reference/sting126_tt.log` is the log of real STinG 1.26 with CE_ETH.STX on a TT (TOS 3.06), menu **A**, from
2026-11-03. Three runs agreed on every exact value. Diff a clone's log against it:
`./sting_diff.py reference/sting126_tt.log STNGFAKE.LOG`.

Real STinG 1.26 crashes on TT (2 bombs) in two tests, so they only run with menu **X**: 7004 `TCP_send()` with
length -1 (no check for negative length in `proto/tcp/tcp.c`), and 7005 a fragmented UDP reply (`reassembly()` in
`sting/fragm.c` calls `IP_discard(NULL)`).

## Log format

```
## module TRANSPORT_TCPIP          comments: STinG identity, server, local IP, ...
4010 # peer resets right after connect
4010 = err -4                      exact - must match
4010 ~ ms 115                      fuzzy - timing / config dependent, never fails
```

## Tests

| Group | What |
|---|---|
| 1xxx | invalid handles (10, -1, 200) on every function, `get_err_text()`, TCP send vs buffer size, `resolve()` |
| 2xxx | TCP / UDP echo of various sizes (`CNget_block` / `CNget_NDB`), UDP datagram boundaries, `CNbyte_count` + `CNget_char`, `CNgets`, `CNgetinfo`, `CNget_block` canaries |
| 3xxx | `TCP_open` / `UDP_open` addressing modes (`TCP_ACTIVE`, `TCP_PASSIVE`, `UDP_EXTEND`), passive open with server connect back, functions on a listening socket |
| 4xxx | connection refused, unanswered IP, reset by peer (with and without unread data), FIN with unread data, half close, data flushed on `TCP_close`, stalled peer (`E_OBUFFULL`), `CNget_block` / `CNgets` corner cases, TCP functions on UDP handles and the other way round, TCP states over the connection life, handle exhaustion and reuse |
| 5xxx | ICMP: `ICMP_handler` set / force / query / remove (also NULL), pings to the server and to an unanswered IP, a 2nd handler that doesn't take the reply, no calls after remove, does a handler see 'port unreachable' for our UDP |
| 6xxx | what apps check before connecting: `carrier_detect`, `query_port` of common port names, `on_port`, `getvstr` of mail / news config variables (only 'set or not' is exact), `setvstr`, `get_dftab`, `KRmalloc` / `KRrealloc` / `KRfree` / `KRgetfree` |
| 8xxx | speed (menu **S**, fuzzy only) |

Not tested: raw IP (`RAW_*`), port and layer control (`cntrl_port`, port parameters), `set_flag` / `clear_flag`. Only
network tools use those.

The 6xxx results depend on the setup. Real STinG with CE_ETH.STX has a port called `CosmosEx`, but old fake STinG
called its port `Internal`. Decide per difference whether the clone should copy it.

## Server ports

See the top of `sting_server.c` and `SCEN_*` in `../test_sting/stest.h`. The scenario port (+8) has a small receive
window, so a stalled reader fills up fast. The server prints every connection and scenario with timestamps, which
helps when a test result looks odd.
