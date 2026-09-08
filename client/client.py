"""Archipelago client for Marvel's Spider-Man 2.

    python client.py --offline
        Runs the whole loop with no server: watches your save, and when a check
        completes, grants a random item from the pool -- the full loop with no
        server, for testing detection and granting.

    python client.py
        Normal use: waits for the connect window (F8 in game), then sends
        completed checks to the server and applies whatever comes back --
        grants in Standard, removals in Longest Night.

    python client.py --host archipelago.gg:38281 --slot YourName
        Same, with the connection given on the command line.

Both directions were built and verified separately before this existed:
detection reads the save (validated blind on OW_SAND_MEMORY_05), and granting
writes to the mod's command channel (used all session).
"""
import argparse
import asyncio
import glob
import os
import json
import queue
import random
import sys
import threading
import time

import paths
import sm2_bridge as bridge

GAME = "Marvel's Spider-Man 2"


class Session:
    """Shared logic: what to do when a check completes or an item arrives.

    Two shapes:
      zero_to_hero   -- start stripped; a check hands something back
      longest_night  -- start with everything; a check TAKES something away
    """

    def __init__(self, mod, watcher, locations, pool, mode="standard"):
        self.mod = mod
        self.watcher = watcher
        self.locations = {r["name"]: r for r in locations}
        self.pool = pool
        self.mode = mode
        self.granted = []
        self.stripped = []
        self.begun = False
        self.received = 0
        self.slot_label = ""
        self.gate = "none"
        self.goal_locations = set()
        self.checked = set()            # AP location ids known checked (server + ours)
        self.goal_sent = False
        self.homed = False
        self.districts = set()

        # Every mod command is slow by design -- the poll loop is 200 ms and a
        # seed walks the whole item table (20 s). Run them on their own thread,
        # in order, so the websocket keepalive is never held up.
        self.work = queue.Queue()
        self.worker = threading.Thread(target=self._drain, daemon=True)
        self.worker.start()

    def _drain(self):
        while True:
            fn = self.work.get()
            try:
                fn()
            except Exception as e:
                print("  mod command failed: %s" % e)

    def defer(self, fn):
        self.work.put(fn)

    def begin(self):
        """Put the save into the shape this mode needs. Once per run.

        Guarded because a reconnect re-runs the Connected handler, and seeding
        twice would strip a Longest Night save a second time.
        """
        if self.begun:
            print("  (already seeded this run -- not doing it again)")
            return
        self.begun = True
        mode = "full" if self.mode == "longest_night" else "empty"
        if self.mode == "longest_night":
            print("LONGEST NIGHT: granting everything, then the night takes it back")
            banner = "LONGEST NIGHT -- pace yourself; this is going to be a long one"
        else:
            print("ZERO TO HERO: stripping everything; checks hand it back")
            banner = "ZERO TO HERO -- start with nothing; earn it back"

        def seed_when_ready():
            # Seeding at the main menu grants into a hero that does not exist
            # yet: it reports success and nothing actually lands. Wait for the
            # mod to say we are in gameplay. This runs on the worker thread, so
            # the websocket is unaffected by the wait.
            def waiting():
                print("  waiting for the game to reach gameplay before seeding...")
                bridge.set_status("connected -- waiting for the game to load")
            if not bridge.wait_for_gameplay(on_wait=waiting):
                print("  !! game never reached gameplay -- NOT seeding")
                bridge.set_status("connected but the game never loaded -- not seeded")
                return
            self.mod.notify("update", banner)
            self.mod.seed(mode)
            bridge.set_status("connected as %s" % self.slot_label)

            # If the game goes through a load in the next two minutes, the
            # stores we just seeded may have been replaced by the save that
            # loaded. Seeding is idempotent, so do it once more when gameplay
            # returns, and re-apply everything the night has already taken.
            # On its OWN thread: the first version sat on the worker for two
            # minutes and held the district unlocks behind it (16:52-16:54).
            def watch_reload():
                end = time.time() + 120
                saw_menu = False
                while time.time() < end:
                    time.sleep(1.0)
                    st = bridge.game_state()
                    if st == "menu":
                        saw_menu = True
                    elif st == "gameplay" and saw_menu:
                        print("  game reloaded after seeding -- seeding again")
                        self.defer(lambda: self.mod.notify("update", banner))
                        self.defer(lambda: self.mod.seed(mode))
                        for item in list(self.stripped):
                            self.defer(lambda i=item: self.mod.send("remove %s" % i))
                        break
            threading.Thread(target=watch_reload, daemon=True).start()

        self.defer(seed_when_ready)

    def on_night_toll(self):
        """Offline only: with no server to send an item, pick a victim at random.

        Online, the loss comes from `on_item` instead, so the seed decides.
        """
        holding = [i for i in self.pool if i not in self.stripped]
        if not holding:
            print("  (nothing left to lose)")
            return
        victim = random.choice(holding)
        self.stripped.append(victim)
        print("  LOST   %s" % victim)
        self.defer(lambda: self.mod.send("remove %s" % victim))
        self.defer(lambda: self.mod.notify("item", "LOST: " + victim))

    def goal_met(self, loc_ids):
        """True the first time every goal location is in the checked set."""
        if self.goal_sent or not self.goal_locations:
            return False
        need = {loc_ids[n] for n in self.goal_locations if n in loc_ids}
        if need and need <= self.checked:
            self.goal_sent = True
            return True
        return False

    def on_check(self, name):
        row = self.locations.get(name, {})
        cat = row.get("category", "?")
        district = row.get("district") or "-"
        print("  CHECK  %-40s %-12s %s" % (name, cat, district))
        self.defer(lambda: self.mod.notify("check", name))
        return row

    def on_item(self, item, sender=None):
        """An item arrived from Archipelago.

        Standard hands it over. Longest Night takes it instead -- the seed, not
        a coin flip, decides what the night costs you.
        """
        if self.mode == "longest_night":
            if item in self.stripped:
                return                      # a resync can replay the same item
            self.stripped.append(item)
            print("  LOST   %s%s" % (item, (" (via %s)" % sender) if sender else ""))
            self.defer(lambda: self.mod.send("remove %s" % item))
            self.defer(lambda: self.mod.notify("item", "LOST: " + item))
            return
        label = ("%s from %s" % (item, sender)) if sender else item
        print("  ITEM   %s" % label)
        self.defer(lambda: self.mod.grant(item))
        self.defer(lambda: self.mod.notify("item", label))
        self.granted.append(item)


# --- offline ------------------------------------------------------------------

def run_offline(session, interval):
    print("offline mode: every check grants a random item from the pool")
    print("waiting for the game to save... (autosave, or save manually)\n")
    while True:
        for name in session.watcher.poll() + session.collectibles.poll() + session.missions.poll():
            session.on_check(name)
            if session.mode == "longest_night":
                session.on_night_toll()
            else:
                remaining = [i for i in session.pool if i not in session.granted]
                if remaining:
                    session.on_item(random.choice(remaining))
                else:
                    print("  (pool exhausted)")
        time.sleep(interval)


# --- archipelago --------------------------------------------------------------

async def run_online(session, host, slot, password, interval):
    import websockets
    bare = host.split(":")[0].lower()
    local = bare in ("localhost", "127.0.0.1", "::1") or bare.startswith("192.168.")
    uri = ("ws://" if local else "wss://") + host
    print("connecting to %s as %s" % (uri, slot))
    bridge.set_status("connecting to %s" % host)

    # Nothing blocks the loop any more, but a slow save read still
    # deserves room before the connection is called dead.
    async with websockets.connect(uri, max_size=None,
                                  ping_interval=30, ping_timeout=60) as ws:
        # AP handshake: the server greets with RoomInfo, we answer with Connect.
        info = json.loads(await ws.recv())
        print("  server: %s" % [m.get("cmd") for m in info])

        await ws.send(json.dumps([{
            "cmd": "Connect", "game": GAME, "name": slot,
            "password": password, "uuid": "sm2-client",
            "version": {"major": 0, "minor": 5, "build": 0, "class": "Version"},
            "items_handling": 0b111, "tags": [],
            # must be True: the mode (standard / longest_night) rides in slot data
            "slot_data": True,
        }]))

        loc_ids = {}          # name -> AP id
        id_to_item = {}       # AP id -> item name

        # The apworld generates the ids from data.json's tables; the same file
        # ships with the client, so the mapping is rebuilt locally and never
        # waits on a DataPackage. Both sides sort the same names.
        try:
            D = json.load(open(os.path.join(paths.HERE, "data.json"), encoding="utf-8"))
            base = D["base_id"]
            for i, n in enumerate(sorted(D["items"])):
                id_to_item[base + i] = n
            for i, d in enumerate(D["districts"]):
                id_to_item[base + 10000 + i] = "District: " + d
            for i, n in enumerate(sorted(D["locations"])):
                loc_ids[n] = base + 20000 + i
            print("  id tables: %d items, %d locations" % (len(id_to_item), len(loc_ids)))
        except Exception as e:
            print("  could not load id tables: %s" % e)
        async def pump_checks():
            """Poll the save and forward completed checks."""
            while True:
                found = await asyncio.to_thread(session.watcher.poll)
                found += await asyncio.to_thread(session.collectibles.poll)
                found += await asyncio.to_thread(session.missions.poll)
                ids = []
                for name in found:
                    session.on_check(name)
                    # No strip here: in Longest Night the item Archipelago sends
                    # back is the thing that goes, handled in on_item.
                    if name in loc_ids:
                        ids.append(loc_ids[name])
                    else:
                        print("     (no AP id for %s -- not in this seed?)" % name)
                if ids:
                    await ws.send(json.dumps([{"cmd": "LocationChecks", "locations": ids}]))
                    session.checked.update(ids)
                    if session.goal_met(loc_ids):
                        print("  GOAL COMPLETE -- telling the server")
                        session.defer(lambda: session.mod.notify("update", "GOAL COMPLETE -- you made it through the night"))
                        await ws.send(json.dumps([{"cmd": "StatusUpdate", "status": 30}]))
                await asyncio.sleep(interval)

        pump = None
        async for raw in ws:
            for msg in json.loads(raw):
                cmd = msg.get("cmd")
                if cmd == "Connected":
                    print("  connected: %d locations in this slot" % len(msg.get("missing_locations", [])))
                    bridge.set_status("connected as %s" % slot)
                    # Keep the player name: this used to reuse `slot` for the
                    # slot data, which then went out on the status line and put
                    # the entire location table on the in-game dot.
                    session.slot_label = slot
                    sdata = msg.get("slot_data") or {}
                    session.mode = sdata.get("game_mode", session.mode)
                    if session.mode == "standard":
                        session.mode = "zero_to_hero"          # seeds from before the rename
                    print("  mode: %s" % session.mode)
                    session.gate = sdata.get("district_gate", "none")
                    print("  district gate: %s" % session.gate)
                    session.goal_locations = set(sdata.get("goal_locations", []))
                    session.checked = set(msg.get("checked_locations", []))
                    print("  goal: %s (%d locations, %d already checked)"
                          % (sdata.get("goal", "?"), len(session.goal_locations), len(session.checked)))
                    # The gate is enforced in game; the mod only needs to know
                    # which one, and which districts are open (sent as the
                    # District items arrive, including on a resync).
                    session.defer(lambda g=session.gate: session.mod.send("gate %s" % g))
                    policy = "grace" if sdata.get("random_start") else "free"
                    session.defer(lambda p=policy: session.mod.send("spawn-district %s" % p))
                    # Gate state is idempotent and lives only in the mod's memory:
                    # a game restart wipes it while this process keeps running,
                    # and a resync skips items we already applied -- so the
                    # District items would never be re-sent. Send every known
                    # district again on each connect.
                    for d in sorted(session.districts):
                        session.defer(lambda d=d: session.mod.send("unlock-district %s" % d))
                    session.homed = False
                    session.begin()

                    # Catch up on anything finished while nothing was
                    # listening -- a closed client, a crash, a reconnect.
                    missing = set(msg.get("missing_locations", []))
                    done = bridge.completed_in_save(session.watcher, list(session.watcher.valid))
                    done += bridge.completed_collectibles(session.watcher)
                    done += bridge.completed_missions(session.watcher, list(session.watcher.valid))
                    catch_up = []
                    for name in done:
                        lid = loc_ids.get(name)
                        if lid in missing and name not in session.watcher.seen:
                            session.watcher.seen.add(name)
                            catch_up.append((name, lid))
                    if catch_up:
                        print("  catching up on %d check(s) done while disconnected:"
                              % len(catch_up))
                        for name, _ in catch_up:
                            session.on_check(name)
                        await ws.send(json.dumps([{
                            "cmd": "LocationChecks",
                            "locations": [lid for _, lid in catch_up],
                        }]))
                        session.checked.update(lid for _, lid in catch_up)
                        if session.goal_met(loc_ids):
                            print("  GOAL COMPLETE -- telling the server")
                            await ws.send(json.dumps([{"cmd": "StatusUpdate", "status": 30}]))

                    pump = asyncio.create_task(pump_checks())
                elif cmd == "ConnectionRefused":
                    print("  refused: %s" % msg.get("errors"))
                    bridge.set_status("refused: %s" % (msg.get("errors") or "check slot/password"))
                    return
                elif cmd == "DataPackage":
                    data = msg.get("data", {}).get("games", {}).get(GAME, {})
                    loc_ids.update(data.get("location_name_to_id", {}))
                    print("  datapackage: %d locations known" % len(loc_ids))
                elif cmd == "ReceivedItems":
                    # A reconnect resends the whole history from index 0. Skip
                    # what we have already applied or Longest Night would strip
                    # the same things twice.
                    start = msg.get("index", 0)
                    items = msg.get("items", [])
                    if start < session.received:
                        items = items[session.received - start:]
                    session.received += len(items)
                    got_district = False
                    for it in items:
                        raw_id = it.get("item")
                        name = id_to_item.get(raw_id, str(raw_id))
                        if name.startswith("District: "):
                            district = name[10:]
                            print("  AREA   %s" % district)
                            session.districts.add(district)
                            session.defer(lambda d=district: session.mod.send("unlock-district %s" % d))
                            got_district = True
                            continue
                        session.on_item(name, it.get("player"))
                    # No `home` teleport: the long jump crashed the game (16:21).
                    # The mod treats the spawn district as open until you leave it.
                elif cmd == "RoomUpdate":
                    if "checked_locations" in msg:
                        session.checked.update(msg["checked_locations"])
                        if session.goal_met(loc_ids):
                            print("  GOAL COMPLETE -- telling the server")
                            await ws.send(json.dumps([{"cmd": "StatusUpdate", "status": 30}]))
                elif cmd == "PrintJSON":
                    text = "".join(p.get("text", "") for p in msg.get("data", []))
                    if text.strip():
                        print("  | %s" % text.strip())
        if pump:
            pump.cancel()


# --- driven from the in-game panel --------------------------------------------

def serve_panel(session, interval):
    """Sit and wait for the player to hit Connect in the connect window.

    F8 in game opens the window. Runs forever: a failed or dropped connection
    returns here, so a typo can be fixed and retried without restarting anything.
    """
    print("waiting for the connect window -- fill it in and hit Connect")
    bridge.set_status("waiting -- fill in the window and hit Connect")
    while True:
        cfg = bridge.read_connect()
        if not cfg:
            time.sleep(1.0)
            continue
        print("panel: %s as %s" % (cfg["host"], cfg["slot"]))
        try:
            asyncio.run(run_online(session, cfg["host"], cfg["slot"],
                                   cfg["password"], interval))
            bridge.set_status("disconnected -- hit Connect to try again")
        except KeyboardInterrupt:
            raise
        except Exception as e:
            msg = str(e) or type(e).__name__
            print("  connection failed: %s" % msg)
            bridge.set_status("failed: %s" % msg[:80])
        print("back to waiting -- hit Connect to try again")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--offline", action="store_true", help="no server; grant a random item per check")
    ap.add_argument("--host", default=None, help="archipelago host:port")
    ap.add_argument("--slot", default=None)
    ap.add_argument("--password", default="")
    ap.add_argument("--interval", type=float, default=5.0, help="save poll seconds")
    ap.add_argument("--mode", choices=("zero-to-hero", "longest-night"), default="zero-to-hero",
                    help="offline only; online takes it from the seed's slot data")
    ap.add_argument("--no-seed", action="store_true", help="do not seed on start")
    args = ap.parse_args()

    # One client, ever. Two of them race for the connect window's file, and the
    # loser sits idle while the winner -- possibly an old process that already
    # seeded a previous run -- silently skips seeding and knows nothing about
    # newer commands. That cost a test run on 2026-09-06. The heartbeat file
    # already says whether a client is alive; refuse to start on top of one.
    try:
        age = time.time() - os.path.getmtime(bridge.ALIVE)
        if age < 10:
            print("another client is already running (heartbeat %.0fs old) -- not starting a second one" % age)
            return 2
    except OSError:
        pass

    paths.register_client()          # so F8 in game can find connect_ui.py
    locations = bridge.load_locations()
    pool = bridge.item_pool()

    # AP needs at least as many locations as items. 142 items into 120 checks
    # does not fit, so trim the pool -- skills are the most numerous and least
    # individually interesting, so they give way first.
    if len(pool) > len(locations):
        keep = [i for i in pool if i not in bridge.load_items()["skill"]]
        skills = [i for i in pool if i in bridge.load_items()["skill"]]
        room = max(0, len(locations) - len(keep))
        pool = keep + skills[:room]
        print("pool trimmed to %d items for %d locations" % (len(pool), len(locations)))

    # A steady heartbeat so the connect window can tell we are alive without
    # shelling out to wmic, which flashed a console window on screen.
    def _beat():
        while True:
            bridge.heartbeat()
            time.sleep(2)
    threading.Thread(target=_beat, daemon=True).start()

    mod = bridge.ModBridge()
    # The save is keyed by engine names; Archipelago shows friendly ones.
    engine_names = [r.get("engine", r["name"]) for r in locations]
    alias = {r.get("engine", r["name"]): r["name"] for r in locations}
    watcher = bridge.SaveWatcher(names=engine_names, alias=alias)

    session = Session(mod, watcher, locations, pool,
                      mode=args.mode.replace("-", "_"))
    # Spider-bots and photo ops come from the save's collectible block, keyed
    # by position -- see collectibles.py. Exact, so it is allowed to strip.
    session.collectibles = bridge.CollectibleWatcher(watcher)
    # Missions with a record of their own (blinds, nests, mysteriums, FNSM,
    # Brooklyn Visions, EMF) never trip the count rule; their MissionState
    # string says when they are done -- see missions.py.
    session.missions = bridge.MissionStateWatcher(watcher)
    try:
        import collectibles
        panel = collectibles.counts(collectibles.parse(
            max([p for p in glob.glob(os.path.join(watcher.folder, "*.save"))
                 if "prefs" not in p], key=os.path.getmtime)))
        print("collectibles: " + "  ".join("%s %d/%d" % (k, d, t) for k, (d, t) in panel.items()))
    except Exception as e:
        print("collectibles: could not read the save yet (%s)" % e)

    print("%d locations, %d items" % (len(locations), len(pool)))
    if not watcher.folder:
        print("!! no save folder found -- is the game installed for this user?")
        return 1

    try:
        if not args.offline and not args.host:
            serve_panel(session, args.interval)
        elif args.offline or not args.host:
            if not args.no_seed:
                session.begin()
            run_offline(session, args.interval)
        else:
            asyncio.run(run_online(session, args.host, args.slot or "Player",
                                   args.password, args.interval))
    except KeyboardInterrupt:
        bridge.set_status("client stopped")
        print("\nstopped. %d items granted this session." % len(session.granted))
    return 0


if __name__ == "__main__":
    sys.exit(main())
