"""The bridge between the game and Archipelago.

Two directions, both already proven separately:

  game -> AP : `detector.py` reads the save and reports checks that went 2 -> 1
  AP -> game : write a line to `ap-commands.txt`; the mod's poll loop reads it

This module owns those two channels and nothing else, so it can be tested
without a server (see `--offline` in client.py).
"""
import json
import os
import re
import time
import collections

import paths

CAP = paths.RUNTIME                       # every file the mod and client exchange
COMMANDS = paths.runtime("ap-commands.txt")
LOG = paths.runtime("APMod.log")
LOCATIONS = os.path.join(paths.HERE, "ap-locations-final.json")
ITEMS_JSON = os.path.join(paths.HERE, "items.json")


# --- game <- AP ---------------------------------------------------------------

class ModBridge:
    """Sends commands to the running mod.

    The mod reads ap-commands.txt and clears it, so commands must be written
    one batch at a time and not overwritten before the poll loop (200 ms) picks
    them up -- writing again too quickly silently loses the previous batch,
    which cost us a map-collection run during development.
    """

    def __init__(self, path=COMMANDS, settle=0.6):
        self.path = path
        self.settle = settle

    def send(self, *lines):
        with open(self.path, "w") as f:
            f.write("\n".join(lines) + "\n")
        time.sleep(self.settle)

    def grant(self, item, count=1):
        self.send("give %s %d" % (item, count))

    def notify(self, kind, text):
        """kind: 'check' | 'item' | '' -- drives the on-screen overlay colour."""
        self.send(("notify %s %s" % (kind, text)).strip())

    def seed(self, mode):
        assert mode in ("full", "empty")
        self.send("seed " + mode)
        time.sleep(20)          # seeding walks the whole item table


# --- game -> AP ---------------------------------------------------------------

class SaveWatcher:
    """Reports mission-type checks completed since the last poll.

    Wraps detector.py: an objective name present twice in save section
    0x65f6fa6e is live, once means completed, so a name going 2 -> 1 between
    two reads is a check the player just finished.

    Spider-bots and photo ops are not stored by name; CollectibleWatcher
    reads those out of the collectible block instead. The two share `seen`.
    """

    def __init__(self, names=None, alias=None):
        """names: the ENGINE names to watch. alias: engine -> Archipelago name."""
        import detector
        self.detector = detector
        self.folder = detector.save_folder()
        self.baseline = {}
        self.seen = set()
        self.valid = set(names or [])
        self.alias = dict(alias or {})

    def ap_name(self, engine):
        return self.alias.get(engine, engine)

    def poll(self):
        """-> list of check names newly completed."""
        import glob
        found = []
        if not self.folder:
            return found
        for path in glob.glob(os.path.join(self.folder, "*.save")):
            if "prefs" in os.path.basename(path):
                continue
            try:
                mtime = os.path.getmtime(path)
            except OSError:
                continue
            key = os.path.basename(path)
            if self.baseline.get(key, (0, None))[0] == mtime:
                continue
            try:
                counts = self.detector.counts(path)
            except Exception:
                continue
            prev = self.baseline.get(key, (0, None))[1]
            self.baseline[key] = (mtime, counts)

            if prev is None:
                continue                      # first sighting only sets a baseline
            for engine, was in prev.items():
                if was == 2 and counts.get(engine, 0) == 1:
                    if self.valid and engine not in self.valid:
                        continue              # not one of our AP locations
                    name = self.ap_name(engine)
                    if name in self.seen:
                        continue
                    self.seen.add(name)
                    found.append(name)

        return found


# --- tables -------------------------------------------------------------------

def load_locations():
    return json.load(open(LOCATIONS))


def load_items():
    """Grantable items, from items.json -- generated from the mod's own
    items.inc at package time so the two cannot drift."""
    out = collections.defaultdict(list)
    for row in json.load(open(ITEMS_JSON, encoding="utf-8")):
        out[row["kind"]].append(row["label"])
    return out


def item_pool():
    """What Archipelago hands out. Deliberately excludes cosmetics and currency:
    suits are granted up front in both YAML modes by design, and currency
    is held at zero by design -- a check that gave money would let the player
    re-buy something a check had removed."""
    items = load_items()
    pool = []
    for kind in ("ability", "gadget", "suittech", "doubleperk", "skill", "modslot"):
        pool.extend(items.get(kind, []))
    return pool

# --- the connect window ---------------------------------------------------------
#
# connect_ui.py (opened with F8 in game) writes what the player typed to a
# file and this side picks it up. Status goes back the same way, which drives
# the dot in both the window and the in-game overlay.

CONNECT = os.path.join(CAP, "ap-connect.json")
STATUS = os.path.join(CAP, "ap-status.txt")


def read_connect(clear=True):
    """Settings the player entered in the connect window, or None."""
    if not os.path.exists(CONNECT):
        return None
    try:
        with open(CONNECT) as f:
            cfg = json.load(f)
    except Exception:
        return None
    if clear:
        try:
            os.remove(CONNECT)
        except OSError:
            pass
    host = (cfg.get("host") or "").strip()
    slot = (cfg.get("slot") or "").strip()
    if not host or not slot:
        return None
    return {"host": host, "slot": slot, "password": cfg.get("password", "")}


def set_status(text):
    """One line the mod polls once a second; 'connected...' turns the dot green."""
    try:
        with open(STATUS, "w") as f:
            f.write(text.strip() + "\n")
    except OSError:
        pass

ALIVE = os.path.join(CAP, "ap-client-alive.txt")


def heartbeat():
    """Touched every couple of seconds while the client runs.

    connect_ui checks this file's age to decide whether a client is already up.
    The previous check shelled out to wmic, which flashed a console window on
    screen every time and does not even list Store Python correctly.
    """
    try:
        with open(ALIVE, "w") as f:
            f.write(str(time.time()))
    except OSError:
        pass


STATE = os.path.join(CAP, "mod-state.txt")


def game_state():
    """'gameplay', 'menu', or None if the mod has not said yet.

    Written by the mod's poll loop whenever it changes: gameplay means the hero
    and its inventory stores exist, which is the only time granting works.
    """
    try:
        with open(STATE) as f:
            return f.read().strip().lower() or None
    except OSError:
        return None


def wait_for_gameplay(timeout=600, on_wait=None):
    """Block until the game is in gameplay. -> True if it got there.

    Returns True when the mod has not reported at all, so an older build that
    does not publish the state still seeds rather than hanging forever.
    """
    first = game_state()
    if first is None or first == "gameplay":
        return True
    if on_wait:
        on_wait()
    deadline = time.time() + timeout
    while time.time() < deadline:
        time.sleep(1.0)
        if game_state() == "gameplay":
            return True
    return False


def completed_in_save(watcher, names):
    """Which of `names` (ENGINE names) the save says are already finished,
    returned as Archipelago names.

    Same rule as live detection: a name present twice is still live, once means
    done. A name that never appears is a collectible, which this cannot judge.
    """
    import glob
    if not watcher.folder:
        return []
    saves = [p for p in glob.glob(os.path.join(watcher.folder, "*.save"))
             if "prefs" not in os.path.basename(p)]
    if not saves:
        return []
    newest = max(saves, key=os.path.getmtime)
    try:
        counts = watcher.detector.counts(newest)
    except Exception:
        return []
    return [watcher.ap_name(n) for n in names if counts.get(n, 0) == 1]


# --- spider-bots and photo ops, from the save's collectible block ---------------

class CollectibleWatcher:
    """Reports spider-bots and photo ops completed since the last poll.

    Reads the collectible block directly (see collectibles.py): one record per
    bot/photo with a Completed flag, keyed by position. Exact, and the same
    identity across saves, sessions and fresh files.
    """

    def __init__(self, watcher):
        import collectibles
        self.c = collectibles
        self.watcher = watcher          # shares folder and the seen-set
        self.baseline = {}              # save basename -> (mtime, state)

    def poll(self):
        import glob
        found = []
        if not self.watcher.folder:
            return found
        for path in glob.glob(os.path.join(self.watcher.folder, "*.save")):
            key = os.path.basename(path)
            if "prefs" in key:
                continue
            try:
                mtime = os.path.getmtime(path)
            except OSError:
                continue
            if self.baseline.get(key, (0, None))[0] == mtime:
                continue
            try:
                state = self.c.parse(path)
            except Exception as e:
                # Never silently absorb a bad read as a baseline: a truncated
                # mid-write file must be retried, not trusted.
                print("  collectibles: %s unreadable (%s) -- will retry" % (key, e))
                continue
            prev = self.baseline.get(key, (0, None))[1]
            self.baseline[key] = (mtime, state)
            if prev is None:
                continue
            for k in self.c.newly_completed(prev, state):
                name = self.c.location_name(k)
                if name not in self.watcher.seen:
                    self.watcher.seen.add(name)
                    found.append(name)
        return found


def completed_collectibles(watcher):
    """Bots and photos the newest save says are done -- an absolute flag, so
    safe to reconcile on, unlike name counts."""
    import glob
    import collectibles
    if not watcher.folder:
        return []
    saves = [p for p in glob.glob(os.path.join(watcher.folder, "*.save"))
             if "prefs" not in os.path.basename(p)]
    if not saves:
        return []
    try:
        state = collectibles.parse(max(saves, key=os.path.getmtime))
    except Exception:
        return []
    return [collectibles.location_name(k) for k, v in state.items() if v]

# --- missions with a record of their own: blinds, nests, mysteriums, FNSM, ... --

class MissionStateWatcher:
    """Reports mission checks by reading each mission's own state record.

    The count rule (SaveWatcher) only works for record-less activities such as
    Marko's memories and prowler stashes. Everything else -- hunter blinds,
    symbiote nests, mysteriums, FNSM requests, Brooklyn Visions, EMF -- keeps
    its objective-hash count at 2 when finished, so it never fired. missions.py
    reads the MissionState string instead: kCompleteCleaning / kCompleteFinished
    means done. Shares the folder, the valid set and the seen-set with the
    SaveWatcher so a check is reported once whichever rule sees it first.
    """

    def __init__(self, watcher):
        import missions
        self.m = missions
        self.watcher = watcher
        self.baseline = {}              # save basename -> (mtime, {engine: state})

    def poll(self):
        import glob
        found = []
        if not self.watcher.folder:
            return found
        names = list(self.watcher.valid)
        for path in glob.glob(os.path.join(self.watcher.folder, "*.save")):
            key = os.path.basename(path)
            if "prefs" in key:
                continue
            try:
                mtime = os.path.getmtime(path)
            except OSError:
                continue
            if self.baseline.get(key, (0, None))[0] == mtime:
                continue
            try:
                state = self.m.states(self.m.parse(path), names)
            except Exception as e:
                print("  missions: %s unreadable (%s) -- will retry" % (key, e))
                continue
            prev = self.baseline.get(key, (0, None))[1]
            self.baseline[key] = (mtime, state)
            if prev is None:
                continue                  # first sighting only sets a baseline
            for engine in self.m.newly_done(prev, state):
                name = self.watcher.ap_name(engine)
                if name not in self.watcher.seen:
                    self.watcher.seen.add(name)
                    found.append(name)
        return found


def completed_missions(watcher, names):
    """Which of `names` (ENGINE names) the newest save records as finished,
    as Archipelago names. The catch-up path for everything the count rule
    cannot judge."""
    import glob
    import missions
    if not watcher.folder:
        return []
    saves = [p for p in glob.glob(os.path.join(watcher.folder, "*.save"))
             if "prefs" not in os.path.basename(p)]
    if not saves:
        return []
    newest = max(saves, key=os.path.getmtime)
    try:
        done = missions.done_names(newest, list(names))
    except Exception:
        return []
    return [watcher.ap_name(n) for n in done]

