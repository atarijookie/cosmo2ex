# netspeed - MiNTnet speed test

Measures TCP between MiNT (`inet4.xdd` + `CE_ETH.XIF`) and a PC, with nothing else in the way: no GEM, no GlueSTiK, no disk.

1. On the PC: `python3 netspeed_server.py` (port 5001, or give another one)
2. Copy `netspeed.ttp` to the Atari, then from bash:

```
./netspeed.ttp <PC IP>                  download, then upload, 4096 kB each
./netspeed.ttp <PC IP> get 8192         download only, 8 MB
./netspeed.ttp <PC IP> put              upload only
./netspeed.ttp <PC IP> check            download and check every byte
```

A dot is printed every 256 kB, so a stalled transfer is visible. `get` doesn't look at the data, so it's the real speed test;
`check` is slower because the CPU compares each byte. The server checks every byte of `put` itself. The upload time ends when
the server has answered, so data still in buffers on the ST isn't counted as sent.

`make` builds `netspeed.ttp`. `make netspeed_host` builds the same program for the PC, to try it without an Atari.
