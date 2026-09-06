"""Spider-bot and photo-op state, read from the save's collectible block.

Collectibles are not stored under their names. They live in
CollectibleSystemGameSaveBlock -> TypeSaveData -> six per-type nodes, each
{InstanceSaveData: array of records, NumCompleted}. A record is a DDL node
with five fields and no name:

    Completed, Discovered, ProximityPinged : bool (1 byte each)
    CollectedOrderIndex, ...RootType       : int  (4 bytes each)

A record's identity is its position in its type's array. That position is
stable across saves, sessions and fresh files (verified 2026-09-06 across an
old 100% file, a mid-run file and a fresh one: 348 records every time, same
indices completed). The Collections panel's "4/42 FOUND" is the count of
Completed flags in type 0.

Types are identified by record count against the panel's own totals:
    type 0  42  Spider-Bots
    type 4  23  Photo Ops
The others (250 / 9 / 14 / 10) are things the name rule already covers.

    import collectibles
    state = collectibles.parse(path)          # {(type, index): completed}
    done  = collectibles.newly_completed(a, b)
"""
import json
import os
import struct
import sys

import detector

MAGIC = 0x03150044
RECORD_STRIDE = 88          # magic + 5 descriptors + 5 name offsets + 11 value bytes + pad

# Field-name hashes (the game's 32-bit name hash, h32). Resolved once against
# the 259,575-name table on 2026-09-06 and pinned here so nothing that big has
# to ship with the client.
H_COMPLETED = 0xB805C362      # Completed
H_DISCOVERED = 0x0E194CFD     # Discovered
H_PINGED = 0x2092E6A2         # ProximityPinged
H_ORDER = 0xBA80F700          # CollectedOrderIndex
H_ROOT = 0x77BB1406           # CollectedOrderIndexRootType
H_INSTANCE = 0xC1B22EC3       # InstanceSaveData
H_TYPESAVE = 0xAC814FB5       # TypeSaveData
SIGNATURE = [H_COMPLETED, H_DISCOVERED, H_PINGED, H_ORDER, H_ROOT]

# type index -> (category, expected record count)
TYPES = {
    0: ("spiderbot", 42),
    4: ("photo", 23),
}


def _u32(sec, o):
    return struct.unpack_from("<I", sec, o)[0]


def _records(sec):
    """Every five-field collectible record in the section, in file order."""
    out = []
    n = len(sec)
    off = 0
    while off + 12 <= n:
        if _u32(sec, off) != MAGIC or _u32(sec, off + 4) != 5:
            off += 4
            continue
        if [_u32(sec, off + 12 + 8 * i) for i in range(5)] != SIGNATURE:
            off += 4
            continue
        vals = off + 12 + 40 + 20
        out.append((off, sec[vals] != 0))
        off = (vals + 11 + 3) & ~3
    return out


def _type_spans(sec):
    """Byte range of each per-type node, in type order."""
    tsd = None
    for m in range(0, len(sec) - 16, 4):
        if _u32(sec, m) == MAGIC and _u32(sec, m + 4) == 2 and _u32(sec, m + 12) == H_TYPESAVE:
            tsd = m
            break
    if tsd is None:
        return []
    six = None
    for m in range(tsd, min(len(sec) - 12, tsd + 0x400), 4):
        if _u32(sec, m) == MAGIC and _u32(sec, m + 4) == 6:
            six = m
            break
    if six is None:
        return []
    starts = []
    m = six + 12 + 8 * 6 + 4 * 6
    while len(starts) < 6 and m < len(sec) - 12:
        if _u32(sec, m) == MAGIC and _u32(sec, m + 4) == 2 and _u32(sec, m + 12) == H_INSTANCE:
            size = _u32(sec, m + 8)
            starts.append(m)
            m += size if size > 12 else 4
            continue
        m += 4
    return [(s, starts[i + 1] if i + 1 < len(starts) else len(sec)) for i, s in enumerate(starts)]


def parse(path):
    """-> {(type_index, record_index): completed} for the types we care about.

    Raises if the layout does not match, so a caller never mistakes a parse
    failure for 'nothing completed'.
    """
    sec = detector.sections(path)[detector.PROGRESS_SECTION]
    spans = _type_spans(sec)
    if len(spans) != 6:
        raise ValueError("expected 6 collectible types, found %d" % len(spans))
    recs = _records(sec)
    state = {}
    for t, (lo, hi) in enumerate(spans):
        if t not in TYPES:
            continue
        mine = [done for off, done in recs if lo <= off < hi]
        cat, expect = TYPES[t]
        if len(mine) != expect:
            raise ValueError("%s: expected %d records, found %d" % (cat, expect, len(mine)))
        for i, done in enumerate(mine):
            state[(t, i)] = done
    return state


def newly_completed(before, after):
    """Keys that went not-completed -> completed."""
    return sorted(k for k, v in after.items() if v and not before.get(k, False))


def location_name(key):
    """The AP location for a record: 'Spider-Bot 07', 'Photo Op 12'."""
    t, i = key
    cat = TYPES[t][0]
    return ("Spider-Bot %02d" if cat == "spiderbot" else "Photo Op %02d") % (i + 1)


def counts(state):
    """Per category: (completed, total) -- what the Collections panel shows."""
    out = {}
    for t, (cat, total) in TYPES.items():
        out[cat] = (sum(1 for (tt, _), v in state.items() if tt == t and v), total)
    return out


if __name__ == "__main__":
    folder = detector.save_folder()
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(folder, "slot2-autosave.save")
    st = parse(path)
    for cat, (done, total) in counts(st).items():
        print("%-10s %d/%d" % (cat, done, total))
    done = [location_name(k) for k, v in sorted(st.items()) if v]
    print("completed:", done)
