"""Spider-Man 2 location detector.

Method (proven 2026-09-04, see notes.md "LOCATION DETECTION SOLVED"): every
objective name appears TWICE in save section 0x65f6fa6e while it is live --
once in the string table, once in its active record -- and completing it drops
the count to ONE. Diff two snapshots and anything going 2 -> 1 is a check the
player just completed. Confirmed blind on OW_SAND_MEMORY_05.

Names are hashed with the game's crc64 (the same hash as toc asset ids and
inventory keys) and counted at 4-byte alignment.

    python detector.py counts  <save>              # what is live right now
    python detector.py diff    <before> <after>    # what was completed between
    python detector.py watch                       # poll the live save folder
"""
import os
import sys
import glob
import time
import json
import struct
import collections

import crc64 as c64          # vendored; the engine's own hash
import paths

PROGRESS_SECTION = 0x65F6FA6E
# The names we detect are exactly our own locations. All 61 named ones were
# verified present in the game's save schema on 2026-09-06, so no schema file
# ships and no wider list is needed.
LOCATIONS = os.path.join(paths.HERE, "ap-locations-final.json")
SCHEMA = None


def save_folder():
    return paths.save_folder()


def sections(path):
    """DAT1 sections of a .save. Offsets are relative to the file here."""
    d = open(path, "rb").read()
    magic, size, n = struct.unpack_from("<III", d, 4)
    out = {}
    for i in range(n):
        sid, off, ln = struct.unpack_from("<III", d, 0x10 + i * 12)
        out[sid] = d[off:off + ln]
    return out


def names():
    """Every candidate check name: the AP location list plus the save schema."""
    out = set()
    if os.path.exists(LOCATIONS):
        for row in json.load(open(LOCATIONS, encoding="utf-8")):
            # The save is keyed by the engine name; "name" is the friendly
            # Archipelago name and would hash to nothing.
            out.add(row.get("engine", row["name"]))
    if SCHEMA and os.path.exists(SCHEMA):
        for line in open(SCHEMA, encoding="utf-8", errors="ignore"):
            s = line.strip()
            if s:
                out.add(s)
    return sorted(out)


_HASHES = None


def hashes():
    global _HASHES
    if _HASHES is None:
        _HASHES = {}
        for n in names():
            _HASHES[c64.hash(n)] = n
    return _HASHES


def counts(path):
    """name -> how many times its hash appears in the progression section."""
    buf = sections(path).get(PROGRESS_SECTION)
    if buf is None:
        raise ValueError("no progression section in %s" % path)
    want = hashes()
    found = collections.Counter()
    for off in range(0, len(buf) - 8 + 1, 4):
        v = struct.unpack_from("<Q", buf, off)[0]
        n = want.get(v)
        if n is not None:
            found[n] += 1
    return found


def completed(before, after):
    """Names that went 2 -> 1: the checks completed between the snapshots."""
    a, b = counts(before), counts(after)
    done = []
    for n, was in a.items():
        now = b.get(n, 0)
        if was == 2 and now == 1:
            done.append(n)
    return sorted(done), a, b


def _report(done, a, b):
    print("names live in the baseline (count 2): %d" % sum(1 for v in a.values() if v == 2))
    print("names live afterwards:                %d" % sum(1 for v in b.values() if v == 2))
    if not done:
        print("\nno completions detected")
        return
    print("\nCOMPLETED (%d):" % len(done))
    locs = {}
    if os.path.exists(LOCATIONS):
        locs = {r["name"]: r for r in json.load(open(LOCATIONS))}
    for n in done:
        r = locs.get(n)
        tag = ("  [%s%s]" % (r["category"], ", opt-in story" if r.get("pool") == "story" else "")) if r else "  (not in the AP list)"
        print("   %-48s%s" % (n, tag))


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "watch"

    if cmd == "counts":
        c = counts(sys.argv[2])
        live = sorted(n for n, v in c.items() if v == 2)
        print("%s\n  names matched: %d   live (count 2): %d" % (sys.argv[2], len(c), len(live)))
        for n in live[:40]:
            print("     " + n)
        return

    if cmd == "diff":
        _report(*completed(sys.argv[2], sys.argv[3]))
        return

    folder = save_folder()
    if not folder:
        print("no save folder found under %s" % SAVE_DIR)
        return
    print("watching %s  (Ctrl-C to stop)" % folder)
    seen = {}
    baseline = {}
    while True:
        for f in glob.glob(os.path.join(folder, "*.save")):
            if "prefs" in os.path.basename(f):
                continue
            m = os.path.getmtime(f)
            if seen.get(f) == m:
                continue
            seen[f] = m
            try:
                c = counts(f)
            except Exception as e:
                print("  %s: %s" % (os.path.basename(f), e))
                continue
            if f in baseline:
                done = [n for n, was in baseline[f].items()
                        if was == 2 and c.get(n, 0) == 1]
                for n in sorted(done):
                    print("CHECK  %s" % n)
            baseline[f] = c
        time.sleep(5)


if __name__ == "__main__":
    main()
