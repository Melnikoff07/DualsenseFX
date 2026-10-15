#!/usr/bin/env python3
"""Log the names of the game's short sounds as they start (BotW 1.6.0, network control net=1 needed).

  sndlog.py OUT.txt SECONDS

Each line: time, ground material under the player, sound name. Use it to find the names for sounds.ini: do something
in the game and look which name shows up at that moment. Read-only: it only reads game memory.
How it works (details in source/sounds.c): main+2CC1670 is the nn::atk SoundArchivePlayer, +128 its pool of 64
WaveSound objects (0x3A0 bytes); a slot's +200 points to the FWAV being played, whose BARS package has the name in
the AMTA STRG block.
"""
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from swnet import Switch

POOL_SIZE, SLOT = 64, 0x3A0


def amta_name(sw, amta):
    h = sw.read(f"{amta:X}", 0x200)
    if h[:4] != b"AMTA":
        return None
    i = h.find(b"STRG")
    if i < 0:
        return None
    n = struct.unpack_from("<I", h, i + 4)[0]
    return h[i + 8:i + 8 + n].split(b"\0")[0].decode(errors="replace")


def sound_name(sw, fwav, back=0x400000, step=0x40000):
    """Look backwards from the FWAV for the BARS package whose table points at it"""
    hi = fwav
    while hi > fwav - back:
        lo = hi - step
        blk = sw.read(f"{lo:X}", step + 4)
        i = blk.rfind(b"BARS")
        while i >= 0:
            bars = lo + i
            count = struct.unpack("<I", sw.read(f"{bars + 0xC:X}", 4))[0]
            if 0 < count < 5000:
                table = sw.read(f"{bars + 0x10 + 4 * count:X}", 8 * count)
                for k in range(count):
                    amta, f = struct.unpack_from("<II", table, 8 * k)
                    if bars + f == fwav:
                        return amta_name(sw, bars + amta)
            i = blk.rfind(b"BARS", 0, i)
        hi = lo
    return None


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    out, secs = sys.argv[1], float(sys.argv[2])
    sw = Switch()
    info = sw.cmd("info").splitlines()
    base = int([l.split() for l in info if l.startswith("main")][0][1], 16)
    q = lambda a: struct.unpack("<Q", sw.read(f"{a:X}", 8))[0]
    pool = q(q(base + 0x2CC1670) + 0x128)
    names, prev, t0 = {}, None, time.time()
    with open(out, "w") as f:
        while time.time() - t0 < secs:
            d = sw.read(f"{pool:X}", SLOT * POOL_SIZE)
            ground = struct.unpack("<I", sw.read("player+1CA8", 4))[0]
            t = time.time() - t0
            if prev:
                for k in range(POOL_SIZE):
                    o = k * SLOT
                    if d[o + 0x100:o + 0x104] == b"\xff" * 4 or d[o + 0x104:o + 0x108] == prev[o + 0x104:o + 0x108]:
                        continue
                    fwav = struct.unpack_from("<Q", d, o + 0x200)[0]
                    if fwav not in names:
                        names[fwav] = (sound_name(sw, fwav) if fwav else None) or "?"
                    f.write(f"{t:8.2f} ground {ground:2} {names[fwav]}\n")
                f.flush()
            prev = d
    print(f"{len(names)} different sounds -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
