"""Where things live, on any machine.

Until 2026-09-06 every path in the client was a literal folder that only
existed on the machine it was written on. These are the rules that replace it:

  RUNTIME   %ProgramData%\SM2-Archipelago -- every file the mod and client
            exchange (commands, status, game state, logs). The mod derives the
            same folder (runtime.h), so nothing is configured.
            Not AppData\Local: the Microsoft Store build of Python redirects
            its writes there into a private per-package copy, and the mod
            (native code) never sees them. ProgramData is not redirected.
  HERE      the folder this client was unzipped into. Registered into RUNTIME
            on start so the mod's F8 can open the connect window.
  saves     the game writes to <Documents>\Marvel's Spider-Man 2\<steam id>\,
            where <Documents> is whatever Windows says it is -- redirected to
            OneDrive on some machines, plain on others.
"""
import ctypes
import glob
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RUNTIME = os.path.join(os.environ.get("ProgramData") or r"C:\ProgramData", "SM2-Archipelago")
os.makedirs(RUNTIME, exist_ok=True)


def runtime(name):
    return os.path.join(RUNTIME, name)


def register_client():
    """Tell the mod where this client is, so F8 in game can launch the window."""
    try:
        with open(runtime("client-path.txt"), "w", encoding="utf-8") as f:
            f.write(HERE)
    except OSError:
        pass


def _known_folder(fid_guid):
    """SHGetKnownFolderPath, or None. FOLDERID_Documents = FDD39AD0-..."""
    try:
        from ctypes import wintypes

        class GUID(ctypes.Structure):
            _fields_ = [("Data1", wintypes.DWORD), ("Data2", wintypes.WORD),
                        ("Data3", wintypes.WORD), ("Data4", wintypes.BYTE * 8)]

        parts = fid_guid.replace("{", "").replace("}", "").split("-")
        g = GUID(int(parts[0], 16), int(parts[1], 16), int(parts[2], 16),
                 (wintypes.BYTE * 8)(*bytes.fromhex(parts[3] + parts[4])))
        out = ctypes.c_wchar_p()
        if ctypes.windll.shell32.SHGetKnownFolderPath(ctypes.byref(g), 0, None, ctypes.byref(out)) == 0:
            path = out.value
            ctypes.windll.ole32.CoTaskMemFree(out)
            return path
    except Exception:
        return None
    return None


FOLDERID_DOCUMENTS = "{FDD39AD0-238F-46AF-ADB4-6C85480369C7}"


def documents_candidates():
    home = os.path.expanduser("~")
    out = []
    known = _known_folder(FOLDERID_DOCUMENTS)
    if known:
        out.append(known)
    out += [os.path.join(home, "OneDrive", "Documents"), os.path.join(home, "Documents")]
    seen, uniq = set(), []
    for d in out:
        k = os.path.normcase(d)
        if k not in seen:
            seen.add(k)
            uniq.append(d)
    return uniq


def save_folder():
    """The folder holding the .save files, or None if the game has never saved."""
    for docs in documents_candidates():
        hits = glob.glob(os.path.join(docs, "Marvel's Spider-Man 2", "*", "*.save"))
        if hits:
            return os.path.dirname(hits[0])
    return None


if __name__ == "__main__":
    print("HERE    ", HERE)
    print("RUNTIME ", RUNTIME)
    print("docs    ", documents_candidates())
    print("saves   ", save_folder())
