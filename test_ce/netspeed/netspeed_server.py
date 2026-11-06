#!/usr/bin/env python3
"""Server for netspeed.ttp, runs on a PC in the same LAN: python3 netspeed_server.py [port]"""
import socket
import sys
import threading

DEFAULT_PORT = 5001
PATTERN_MOD = 251                           # same as in netspeed.c
BLOCK = PATTERN_MOD * 256                   # about 64 kB, a multiple of PATTERN_MOD so blocks repeat seamlessly
PATTERN = bytes(i % PATTERN_MOD for i in range(BLOCK))


def read_line(conn):
    line = b""
    while not line.endswith(b"\n") and len(line) < 64:
        data = conn.recv(1)
        if not data:
            break
        line += data
    return line.decode("ascii", "replace").strip()


def do_get(conn, count):
    left = count
    while left > 0:
        chunk = min(left, BLOCK)
        conn.sendall(PATTERN[:chunk])
        left -= chunk
    return count, 0


def do_put(conn, count):
    pos = bad = 0
    while pos < count:
        data = conn.recv(min(count - pos, BLOCK))     # at most BLOCK: PATTERN * 2 covers that from any offset
        if not data:
            break
        offset = pos % BLOCK                # compare against the pattern at this stream position
        expected = (PATTERN + PATTERN)[offset:offset + len(data)]
        if data != expected:
            bad += sum(1 for a, b in zip(data, expected) if a != b)
        pos += len(data)
    conn.sendall(f"OK {pos} {bad}\n".encode("ascii"))
    return pos, bad


def handle(conn, peer):
    with conn:
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        try:
            parts = read_line(conn).split()
            if len(parts) != 2 or parts[0] not in ("GET", "PUT") or not parts[1].isdigit():
                print(f"{peer[0]}: bad command {parts}")
                return
            cmd, count = parts[0], int(parts[1])
            print(f"{peer[0]}: {cmd} {count} bytes ...", flush=True)
            done, bad = (do_get if cmd == "GET" else do_put)(conn, count)
            print(f"{peer[0]}: {cmd} done, {done} bytes, {bad} wrong", flush=True)
        except OSError as err:
            print(f"{peer[0]}: {err}", flush=True)


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_PORT
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(5)
    print(f"netspeed server on port {port}, Ctrl-C to stop", flush=True)
    while True:
        conn, peer = srv.accept()
        threading.Thread(target=handle, args=(conn, peer), daemon=True).start()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
