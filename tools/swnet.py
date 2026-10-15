#!/usr/bin/env python3
"""Client for the adaptive_triggers network control (net=1 in config.ini).

  swnet.py discover                      find the console on the local network
  swnet.py doctor                        why the console can't be reached (token, network, port, scan)
  swnet.py cmd "<command>"               run one command (try "help"), text replies are printed
  swnet.py read <addr> <len> <out.bin>   save game memory to a file
  swnet.py get <remote> <local>          download a project file
  swnet.py put <local> <remote>          upload a project file (checked with CRC32)

Settings: SWNET_TOKEN (the net_token from config.ini), SWNET_HOST (skips discovery), SWNET_PORT (default 7330).
Without them: ~/.config/dualsensefx/net-token holds the token and ~/.config/dualsensefx/console the last address
(host=...). If that address doesn't answer: UDP discovery, then a scan of the local /24 network for the port; the
address found is remembered.
Also usable as a module: Switch().cmd("info"), .read("player+C40", 4), ...
"""
import concurrent.futures
import os
import socket
import sys
import zlib

PORT = int(os.environ.get("SWNET_PORT", "7330"))
CONF_DIR = os.path.expanduser("~/.config/dualsensefx")


def _conf(name):
    try:
        with open(os.path.join(CONF_DIR, name)) as f:
            return f.read().strip()
    except OSError:
        return None


def remembered_host():
    text = _conf("console") or ""
    for line in text.splitlines():
        if line.startswith("host="):
            return line[5:].strip()
    return None


def remember_host(host):
    try:
        os.makedirs(CONF_DIR, exist_ok=True)
        with open(os.path.join(CONF_DIR, "console"), "w") as f:
            f.write(f"host={host}\nport={PORT}\n")
    except OSError:
        pass


def port_open(host, timeout=0.6):
    s = socket.socket()
    s.settimeout(timeout)
    try:
        s.connect((host, PORT))
        return True
    except OSError:
        return False
    finally:
        s.close()


def scan_subnet():
    """The console on the local /24 network (UDP discovery does not reach every computer)"""
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        probe.connect(("192.0.2.1", 9))     # no traffic: only picks the outgoing interface
        mine = probe.getsockname()[0]
    except OSError:
        return None
    finally:
        probe.close()
    prefix = mine.rsplit(".", 1)[0]
    hosts = [f"{prefix}.{i}" for i in range(1, 255) if f"{prefix}.{i}" != mine]
    with concurrent.futures.ThreadPoolExecutor(64) as ex:
        for host, ok in zip(hosts, ex.map(port_open, hosts)):
            if ok:
                return host
    return None


def find_console():
    host = remembered_host()
    if host and port_open(host, 1.5):
        return host
    host = discover() or scan_subnet()
    if host:
        remember_host(host)
    return host


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
        host = host or os.environ.get("SWNET_HOST") or find_console()
        token = token or os.environ.get("SWNET_TOKEN") or _conf("net-token")
        if not host:
            raise SystemExit("console not found: run `swnet.py doctor` to see which step fails "
                             "(usually the console sleeps or is off; ask the user to wake it)")
        if not token:
            raise SystemExit("set SWNET_TOKEN to net_token from config.ini (or put it in ~/.config/dualsensefx/net-token)")
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


def doctor():
    """Step by step: what is set up on this computer and where the connection stops"""
    token = os.environ.get("SWNET_TOKEN") or _conf("net-token")
    print("token:", "found" if token else f"MISSING (~/.config/dualsensefx/net-token or SWNET_TOKEN)")
    host = os.environ.get("SWNET_HOST") or remembered_host()
    print("last known console:", host or "none")
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        probe.connect(("192.0.2.1", 9))
        mine = probe.getsockname()[0]
        print("this computer:", mine)
        if host and mine.rsplit(".", 1)[0] != host.rsplit(".", 1)[0]:
            print(f"  -> this computer is not on the console's network ({host.rsplit('.', 1)[0]}.x): another Wi-Fi, "
                  "or a VPN takes the traffic; the console can only be reached from the home network")
    except OSError as e:
        print("this computer: no network route:", e)
    finally:
        probe.close()
    if host:
        s = socket.socket()
        s.settimeout(3)
        try:
            s.connect((host, PORT))
            print(f"{host}:{PORT}: open")
            s.close()
        except OSError as e:
            s.close()
            print(f"{host}:{PORT}: {e!r}")
            if isinstance(e, PermissionError):
                print("  -> the network is blocked for this process (sandbox?): run without the sandbox / on the user's Mac")
            elif isinstance(e, ConnectionRefusedError):
                print("  -> the console answers but the server is not listening: just woke up (wait ~30 s), net=0, "
                      "or the module did not start (check log.old.txt after the next working connection)")
            else:
                print("  -> no answer: the console sleeps or is off (Wi-Fi is off in sleep), or got another address")
    found = discover() or scan_subnet()
    print("discovery / subnet scan:", found or "nothing on this /24 network")
    if found:
        remember_host(found)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    what = argv[1]
    if what == "doctor":
        doctor()
        return 0
    if what == "discover":
        host = find_console()
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
