#!/usr/bin/env python3
"""Client for the adaptive_triggers network control (net=1 in config.ini).

  swnet.py discover                      find the console on the local network
  swnet.py cmd "<command>"               run one command (try "help"), text replies are printed
  swnet.py read <addr> <len> <out.bin>   save game memory to a file
  swnet.py get <remote> <local>          download a project file
  swnet.py put <local> <remote>          upload a project file (checked with CRC32)

Settings: SWNET_TOKEN (the net_token from config.ini), SWNET_HOST (skips discovery), SWNET_PORT (default 7330).
Also usable as a module: Switch().cmd("info"), .read("player+C40", 4), ...
"""
import os
import socket
import sys
import zlib

PORT = int(os.environ.get("SWNET_PORT", "7330"))


def discover(timeout=2.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.settimeout(timeout)
    s.sendto(b"atdiscover", ("255.255.255.255", PORT + 1))
    try:
        reply, (host, _) = s.recvfrom(64)
        if reply.startswith(b"adaptive_triggers"):
            return host
    except socket.timeout:
        pass
    return None


class Switch:
    def __init__(self, host=None, token=None, timeout=10.0):
        host = host or os.environ.get("SWNET_HOST") or discover()
        token = token or os.environ.get("SWNET_TOKEN")
        if not host:
            raise SystemExit("console not found (is net=1 set and the console on the same Wi-Fi?)")
        if not token:
            raise SystemExit("set SWNET_TOKEN to net_token from config.ini")
        self.host = host
        self.sock = socket.create_connection((host, PORT), timeout=timeout)
        self.file = self.sock.makefile("rb")
        ok = self.cmd(f"auth {token}")
        if not ok.startswith("OK"):
            raise SystemExit(f"auth failed: {ok}")

    def _line(self):
        line = self.file.readline()
        if not line:
            raise ConnectionError("connection closed")
        return line.decode(errors="replace").rstrip("\r\n")

    def _send(self, text):
        self.sock.sendall(text.encode() + b"\n")

    def _reply(self):
        """Returns the status line, or (payload bytes, final status line) for DATA replies."""
        line = self._line()
        if line.startswith("DATA "):
            n = int(line.split()[1])
            data = self.file.read(n)
            return data, self._line()
        return line

    def cmd(self, text):
        self._send(text)
        r = self._reply()
        if isinstance(r, tuple):
            data, status = r
            return data.decode(errors="replace") + ("" if status == "OK" else status)
        return r

    def read(self, addr, length):
        self._send(f"read {addr} {length:X}" if isinstance(length, int) else f"read {addr} {length}")
        r = self._reply()
        if not isinstance(r, tuple):
            raise RuntimeError(r)
        return r[0]

    def get(self, remote):
        self._send(f"get {remote}")
        r = self._reply()
        if not isinstance(r, tuple):
            raise RuntimeError(r)
        return r[0]

    def put(self, remote, data):
        self._send(f"put {remote} {len(data)}")
        ready = self._line()
        if ready != "READY":
            raise RuntimeError(ready)
        self.sock.sendall(data)
        status = self._line()
        crc = f"{zlib.crc32(data) & 0xFFFFFFFF:08X}"
        if not status.startswith("OK") or crc not in status:
            raise RuntimeError(f"upload check failed: {status}, local crc {crc}")
        return status


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    what = argv[1]
    if what == "discover":
        host = discover()
        print(host or "not found")
        return 0 if host else 1
    sw = Switch()
    if what == "cmd":
        print(sw.cmd(" ".join(argv[2:])))
    elif what == "read":
        data = sw.read(argv[2], int(argv[3], 16))
        open(argv[4], "wb").write(data)
        print(f"{len(data)} bytes -> {argv[4]}")
    elif what == "get":
        data = sw.get(argv[2])
        open(argv[3], "wb").write(data)
        print(f"{len(data)} bytes -> {argv[3]}")
    elif what == "put":
        print(sw.put(argv[3], open(argv[2], "rb").read()))
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
