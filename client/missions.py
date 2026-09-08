"""Mission completion straight from the save's mission records.

Why this exists
---------------
The first detector counted how often a mission's name-hash appears in the
progression section: twice while live, once when done. That is true only for
activities that have no mission record of their own (Marko's memories,
prowler stashes). Everything with a real record -- hunter blinds, symbiote
nests, mysteriums, FNSM requests, Brooklyn Visions, EMF, the drone chases --
keeps its count at 2 forever, or climbs to 3-4 while active. On 2026-09-07 a
full Longest Night run finished ten of those and sent none.

What the save actually says
---------------------------
Each mission has a DDL node whose first two fields are:

    SaveHash      h32 4C5AB740   u32: the game's 32-bit hash of the mission name
    MissionState  h32 D5BB196B   string record: u32 len, u32 h32, u64 h64, bytes

States seen: kInactive, kAvailable, kActive, kCompleteCleaning,
kCompleteFinished. Done means kCompleteCleaning or kCompleteFinished.
Validated live: three +1000 XP blind payouts in the mod log matched exactly
the three blind records that read kCompleteFinished.

The 32-bit hash is a plain reflected CRC-32 (the standard table for
polynomial 0xEDB88320), seeded with 0xEDB88320 instead of 0xFFFFFFFF and
with no final XOR. The table is generated here from the polynomial.
"""
import os
import struct

MAGIC = 0x03150044
F_SAVEHASH = 0x4C5AB740
F_MISSIONSTATE = 0xD5BB196B
DONE = frozenset(("kCompleteCleaning", "kCompleteFinished", "kComplete"))

_TABLE = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ (0xEDB88320 if _c & 1 else 0)
    _TABLE.append(_c)


def h32(text, seed=0xEDB88320):
    """The game's 32-bit name hash."""
    crc = seed
    for b in text.encode("utf-8"):
        crc = (crc >> 8) ^ _TABLE[(crc & 0xFF) ^ b]
    return crc & 0xFFFFFFFF


def _progression(path):
    """The progression section's bytes, or the whole file if the DAT1 header
    does not parse (a mid-write read). Callers treat a failure as 'retry'."""
    d = open(path, "rb").read()
    try:
        magic, size, n = struct.unpack_from("<III", d, 4)
        for i in range(n):
            sid, off, ln = struct.unpack_from("<III", d, 0x10 + i * 12)
            if sid == 0x65F6FA6E:
                return d[off:off + ln]
    except struct.error:
        pass
    raise ValueError("no progression section in %s" % os.path.basename(path))


def parse(path):
    """-> {h32(mission name): state string} for every mission record."""
    buf = _progression(path)
    magic = struct.pack("<I", MAGIC)
    out = {}
    o = 0
    while True:
        o = buf.find(magic, o)
        if o < 0:
            break
        try:
            nf, size = struct.unpack_from("<II", buf, o + 4)
            if 2 <= nf <= 64 and size < 0x20000:
                f0 = struct.unpack_from("<I", buf, o + 12)[0]
                f1 = struct.unpack_from("<I", buf, o + 20)[0]
                if f0 == F_SAVEHASH and f1 == F_MISSIONSTATE:
                    vals = o + 12 + 12 * nf
                    save_hash = struct.unpack_from("<I", buf, vals)[0]
                    ln = struct.unpack_from("<I", buf, vals + 4)[0]
                    if 0 < ln < 64:
                        out[save_hash] = buf[vals + 20:vals + 20 + ln].decode("ascii", "replace")
        except struct.error:
            pass
        o += 4
    return out


def states(state_map, engine_names):
    """-> {engine name: state or None} for the names given."""
    return {n: state_map.get(h32(n)) for n in engine_names}


def newly_done(prev_states, now_states):
    """Engine names that are done now and were not done before."""
    out = []
    for n, st in now_states.items():
        if st in DONE and prev_states.get(n) not in DONE:
            out.append(n)
    return out


def done_names(path, engine_names):
    """Engine names the save at `path` records as finished."""
    st = states(parse(path), engine_names)
    return [n for n, s in st.items() if s in DONE]


if __name__ == "__main__":
    import sys, collections
    p = sys.argv[1]
    m = parse(p)
    print("%d mission records" % len(m))
    print(collections.Counter(m.values()).most_common())
