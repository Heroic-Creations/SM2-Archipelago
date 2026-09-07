"""CRC-64 as Insomniac's engine keys asset ids, inventory hashes and save names.

Standard CRC-64/ECMA-182 in reflected form -- the lookup table is generated
here from the polynomial, not copied from anywhere. The two things specific to
the game are format facts observed from its files: the initial value, and the
final shift-and-set-top-bit. Verified against the save: crc64(objective name)
is the value stored beside that name.

    crc64.hash("OW_SAND_MEMORY_05")   -> the u64 that appears in the save
"""

POLY = 0xC96C5795D7870F42          # CRC-64/ECMA-182, reflected
INIT = 0xC96C5795D7870F42          # what the engine starts from


def _make_table(poly=POLY):
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ poly if c & 1 else c >> 1
        table.append(c)
    return table


TABLE = _make_table()


def normalize(text):
    """Lower-case, backslashes to slashes, runs of slashes collapsed -- the
    normalisation the engine applies before hashing a path."""
    text = text.lower().replace("\\", "/")
    out = []
    prev_slash = False
    for c in text:
        if c == "/":
            if prev_slash:
                continue
            prev_slash = True
        else:
            prev_slash = False
        out.append(c)
    return "".join(out)


def hash(text):
    crc = INIT
    for ch in normalize(text):
        crc = 0xFFFFFFFFFFFFFFFF & ((crc >> 8) ^ TABLE[0xFF & (crc ^ ord(ch))])
    return (crc >> 2) | 0x8000000000000000
