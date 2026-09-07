// ------------------------------------------------------------ command channel
//
// The point of this mod, for now: the client says "remove VenomPunch",
// Claude appends a line to a file, the game does it while he watches. The
// hotkey thread polls the file, truncates it, and enqueues one request per
// line; requests execute on the game thread through the same drains as before
// and print results on the live console.
//
//     <runtime>\ap-commands.txt        one command per line
//
//     remove <item>          equip inventory remove(-1); perks: Equip store, +recompute
//     add <item>             add(+1)
//     equip <item>           slotted ability: equip into its own slot
//     restore <item>         add then equip
//     query <item>           read-only: store, held, counts, live components
//     list [text]            print item labels containing <text>
//     reset-suittech         remove every owned Health/Damage/Focus tier except Health 1
//     grant-suittech         add every tier not owned
//     health-recompute       recompute max health + HUD nudge
//     send-location <name>   STUB: logs the check; detection is not built yet
//
// Item names are matched case-insensitively: exact label, then unique prefix,
// then unique substring. Ambiguity prints the candidates and does nothing.

inline const char* kCommandFileStr() { static const std::string p = RuntimeFile("ap-commands.txt"); return p.c_str(); }
#define kCommandFile kCommandFileStr()

int FindItemIndex(const char* text, char* why, size_t why_size) {
    auto eq  = [](const char* a, const char* b) { return _stricmp(a, b) == 0; };
    auto has = [](const char* hay, const char* needle) {
        const size_t n = strlen(needle);
        for (const char* p = hay; *p; ++p) if (_strnicmp(p, needle, n) == 0) return true;
        return false;
    };
    for (int i = 0; i < kApItemCount; ++i) if (eq(kApItems[i].label, text)) return i;

    int hit = -1, hits = 0; char cands[512] = ""; size_t cl = 0;
    const size_t n = strlen(text);
    for (int i = 0; i < kApItemCount; ++i) {
        if (_strnicmp(kApItems[i].label, text, n) == 0) { hit = i; ++hits; if (cl < 400) cl += sprintf_s(cands + cl, sizeof(cands) - cl, "%s, ", kApItems[i].label); }
    }
    if (hits == 1) return hit;
    if (hits > 1) { sprintf_s(why, why_size, "ambiguous prefix: %s", cands); return -1; }

    hit = -1; hits = 0; cl = 0; cands[0] = 0;
    for (int i = 0; i < kApItemCount; ++i) {
        if (has(kApItems[i].label, text)) { hit = i; ++hits; if (cl < 400) cl += sprintf_s(cands + cl, sizeof(cands) - cl, "%s, ", kApItems[i].label); }
    }
    if (hits == 1) return hit;
    sprintf_s(why, why_size, hits ? "ambiguous: %s" : "no item matches", cands);
    return -1;
}

void ListItems(const char* text) {
    SAY("");
    SAY("items%s%s:", text && *text ? " matching " : "", text ? text : "");
    int shown = 0;
    for (int i = 0; i < kApItemCount && shown < 60; ++i) {
        bool ok = !text || !*text;
        if (!ok) { const size_t n = strlen(text); for (const char* p = kApItems[i].label; *p && !ok; ++p) ok = (_strnicmp(p, text, n) == 0); }
        if (ok) { SAY("  %-32s %s", kApItems[i].label, kApItems[i].kind); ++shown; }
    }
    if (shown == 60) SAY("  ... (more; narrow the text)");
}

void RunCommand(char* line) {
    // trim
    while (*line == ' ' || *line == '\t') ++line;
    size_t len = strlen(line);
    while (len && (line[len-1] == '\r' || line[len-1] == '\n' || line[len-1] == ' ')) line[--len] = 0;
    if (!len || line[0] == '#') return;

    char verb[32] = ""; const char* arg = "";
    {
        size_t v = 0;
        while (line[v] && line[v] != ' ' && v < sizeof(verb) - 1) { verb[v] = line[v]; ++v; }
        verb[v] = 0;
        arg = line + v; while (*arg == ' ') ++arg;
    }
    SAY("");
    SAY("$ %s", line);

    if (_stricmp(verb, "list") == 0)             { ListItems(arg); return; }
    if (_stricmp(verb, "seed") == 0) {
        // The YAML's mode, applied in one command at seed start.
        if (_stricmp(arg, "empty") == 0) { EnqueueStoreRequest(StoreOp::SeedEmpty, 0, "seed"); return; }
        if (_stricmp(arg, "full")  == 0) { EnqueueStoreRequest(StoreOp::SeedFull,  0, "seed"); return; }
        SAY("  usage: seed empty   (AP grants everything, game locked out)");
        SAY("         seed full    (start with everything, checks strip it away)");
        return;
    }

    // give <item> <count> -- bulk grant. Archipelago needs this for currency
    // and XP, and it is the only way to put a level back after a zeroing.
    if (_stricmp(verb, "give") == 0) {
        const char* sp = strrchr(arg, ' ');
        int32_t n = 1; char item[192] = "";
        if (sp && sp[1]) { n = atoi(sp + 1); strncpy_s(item, arg, (size_t)(sp - arg)); }
        else strncpy_s(item, arg, _TRUNCATE);
        if (n <= 0) { SAY("  usage: give <item> <count>"); return; }
        char why[600];
        const int idx = FindItemIndex(item, why, sizeof(why));
        if (idx < 0) { SAY("  %s", why); return; }
        EnqueueStoreRequest(StoreOp::Add, kApItems[idx].hash, kApItems[idx].label, kApItems[idx].slot, n);
        return;
    }

    // pick <perk> -- hold that side of its pair, drop the other.
    // hash <hex> <count>  -- act on any item, in the table or not.
    if (_stricmp(verb, "hash") == 0) {
        char hex[32] = ""; int32_t n = 1;
        const char* sp = strrchr(arg, ' ');
        if (sp && sp[1]) { n = atoi(sp + 1); strncpy_s(hex, arg, (size_t)(sp - arg)); }
        else strncpy_s(hex, arg, _TRUNCATE);
        uint64_t h = _strtoui64(hex, nullptr, 16);
        if (!h) { SAY("  usage: hash <hex> <count>   (negative count removes)"); return; }
        // "hash" is a literal: the queue holds the label pointer until the game
        // thread drains it, so it must outlive this call.
        EnqueueStoreRequest(StoreOp::RawHash, h, "hash", -1, n);
        return;
    }

    // suits            -- list the ones not owned
    // grant-suits       -- grant only those, leaving the equipped loadout alone
    if (_stricmp(verb, "grant-suits") == 0) {
        EnqueueStoreRequest(StoreOp::GrantSuits, 0, "suits");
        return;
    }

    if (_stricmp(verb, "suits") == 0) {
        // Read-only listing; safe to run here.
        int owned = 0, missing = 0;
        SAY("suits not owned:");
        for (int i = 0; i < kApItemCount; ++i) {
            const ApItem& it = kApItems[i];
            if (strcmp(it.kind, "suit") != 0) continue;
            void* st = ClaimingStore(it.hash);
            if (!st) { SAY("   %-34s no store claims it", it.label); ++missing; continue; }
            int32_t have = 0;
            if (!SafeStoreCount(st, it.hash, &have)) continue;
            if (have > 0) { ++owned; continue; }
            ++missing;
            SAY("   - %s", it.label);
        }
        SAY("  %d owned, %d missing", owned, missing);
        return;
    }

    // give-to <store#> <item> [count] -- bypass the manager and add directly
    // to one store. Items that work live in GameEquipInventory [4]; the locked
    // ones stopped at their claiming store, so this tests moving them.
    // craft <item> -- the game's own end state: cleared from the claiming
    // store, exactly one in the Equip store. No currency needed.
    if (_stricmp(verb, "craft") == 0) {
        char why[600];
        const int idx = FindItemIndex(arg, why, sizeof(why));
        if (idx < 0) { SAY("  %s", why); return; }
        EnqueueStoreRequest(StoreOp::Craft, kApItems[idx].hash, kApItems[idx].label);
        return;
    }

    if (_stricmp(verb, "uncraft") == 0) {
        char why[600];
        const int idx = FindItemIndex(arg, why, sizeof(why));
        if (idx < 0) { SAY("  %s", why); return; }
        EnqueueStoreRequest(StoreOp::Uncraft, kApItems[idx].hash, kApItems[idx].label);
        return;
    }

    if (_stricmp(verb, "trace") == 0) {
        const bool on = (_stricmp(arg, "on") == 0);
        g_trace.store(on);
        SAY("  store tracing %s -- every add logs its caller (exe-relative)", on ? "ON" : "off");
        return;
    }

    if (_stricmp(verb, "give-to") == 0) {
        int si = atoi(arg);
        const char* rest = strchr(arg, ' ');
        if (!rest) { SAY("  usage: give-to <store#> <item> [count]"); return; }
        while (*rest == ' ') ++rest;
        int32_t n = 1; char item[192] = "";
        const char* sp = strrchr(rest, ' ');
        // Only treat a trailing number as a count if what precedes it names an
        // item: suit names end in numbers ("Suit Miles 2020").
        bool took_count = false;
        if (sp && sp[1] && atoi(sp + 1) != 0) {
            char head[192] = ""; char ignore[600];
            strncpy_s(head, rest, (size_t)(sp - rest));
            if (FindItemIndex(head, ignore, sizeof(ignore)) >= 0) {
                n = atoi(sp + 1); strncpy_s(item, head, _TRUNCATE); took_count = true;
            }
        }
        if (!took_count) strncpy_s(item, rest, _TRUNCATE);
        char why[600];
        const int idx = FindItemIndex(item, why, sizeof(why));
        if (idx < 0) { SAY("  %s", why); return; }
        EnqueueStoreRequest(StoreOp::GiveTo, kApItems[idx].hash, kApItems[idx].label, si, n);
        return;
    }

    if (_stricmp(verb, "stores") == 0) {
        char why[600];
        const int idx = FindItemIndex(arg, why, sizeof(why));
        if (idx < 0) { SAY("  %s", why); return; }
        const ApItem& it = kApItems[idx];
        SAY("  %s (0x%016llX) across all nine stores:", it.label, (unsigned long long)it.hash);
        static const char* kNames[9] = { "General", "?", "?", "?", "Equip", "Vanity", "Mission", "?", "VanityEquipped" };
        for (int si = 0; si < 9; ++si) {
            void* st = StoreByIndex(si);
            if (!st) continue;
            char tn[128] = "?"; SafeTypeName(st, tn, sizeof(tn));
            int32_t cnt = -1; SafeStoreCount(st, it.hash, &cnt);
            void* owned = nullptr;   SafeStoreFind(st, kSlotFindOwned, it.hash, &owned);
            void* equipped = nullptr; SafeStoreFind(st, kSlotFindEquipped, it.hash, &equipped);
            SAY("   [%d] %-28s count=%-4d owned=%-3s equipped=%s", si, tn, cnt,
                owned ? "yes" : "no", equipped ? "yes" : "no");
        }
        void* claim = ClaimingStore(it.hash);
        char cn[128] = "none";
        if (claim) SafeTypeName(claim, cn, sizeof(cn));
        SAY("   manager says this item belongs to: %s", cn);
        return;
    }

    if (_stricmp(verb, "pick") == 0) {
        for (int i = 0; i < kDoublePerkCount; ++i) {
            if (_stricmp(arg, kDoublePerks[i].a_label) == 0 || _stricmp(arg, kDoublePerks[i].b_label) == 0) {
                const bool b = (_stricmp(arg, kDoublePerks[i].b_label) == 0);
                g_dp_choice[i] = b ? 1 : 0;
                // Both sides are owned either way; this only records the YAML
                // preference and re-applies the pair on the game thread.
                EnqueueStoreRequest(StoreOp::DoublePerks, 0, "paired perks");
                return;
            }
        }
        SAY("  no paired perk called '%s'. The eight pairs are:", arg);
        for (int i = 0; i < kDoublePerkCount; ++i)
            SAY("    %-10s %-20s or  %s", kDoublePerks[i].column, kDoublePerks[i].a_label, kDoublePerks[i].b_label);
        return;
    }

    if (_stricmp(verb, "double-perks") == 0) { EnqueueStoreRequest(StoreOp::DoublePerks, 0, "paired perks"); return; }

    if (_stricmp(verb, "zero-currency") == 0) { EnqueueStoreRequest(StoreOp::ZeroCurrency, 0, "currency", 0); return; }

    if (_stricmp(verb, "lockdown") == 0) {
        const bool on = (_stricmp(arg, "on") == 0);
        if (!on && _stricmp(arg, "off") != 0) { SAY("  usage: lockdown on | lockdown off   (currently %s, %d refused)", g_lockdown.load() ? "ON" : "off", g_refused.load()); return; }
        g_lockdown.store(on);
        SaveLockdownState();
        SAY("  lockdown %s -- %s", on ? "ON" : "off",
            on ? "the game can no longer grant anything; only these commands can" : "the game may grant normally again");
        return;
    }

    if (_stricmp(verb, "where") == 0) {
        Actor* hero = HeroActor();
        if (!hero) { SAY("  no hero"); return; }
        Vector3 p{};
        __try { p = hero->GetPosition(); } __except (EXCEPTION_EXECUTE_HANDLER) { SAY("  position unreadable"); return; }
        SAY("  hero at  x=%.1f  y=%.1f  z=%.1f", p.x, p.y, p.z);
        return;
    }

    if (_stricmp(verb, "districts") == 0) {
        // `districts on`  -- start collecting Vol_Dist_* actors from GetActor
        // `districts`     -- dump what has been collected, with the component bytes
        if (_stricmp(arg, "on") == 0 || _stricmp(arg, "off") == 0) {
            const bool on = (_stricmp(arg, "on") == 0);
            g_collect_vols.store(on, std::memory_order_relaxed);
            SAY("district volume collection %s%s", on ? "ON" : "off",
                on ? " -- fast-travel around a few districts, then run `districts`" : "");
            return;
        }
        DumpDistrictVolumes();
        return;
    }

    if (_stricmp(verb, "district") == 0) {
        // Find which handle in DistrictTrackerComponent is the CURRENT district.
        //
        // The earlier attempt swept 0x38..0x120 and reported whatever resolved,
        // which was HeroLocal and WetnessTracker -- no way to tell right from
        // wrong. Now there is one: the game's district actors are named
        // `Vol_Dist_*` (Vol_Dist_Harlem, Vol_Dist_ChinaTown ...), seen in the
        // capture with a DistrictComponent on them. So sweep wider and flag
        // anything whose actor name starts with Vol_Dist -- that is the answer,
        // and anything else is noise.
        void* dt = FindHeroComponent("DistrictTrackerComponent");
        if (!dt) { SAY("  no DistrictTrackerComponent on the hero"); return; }
        SAY("  DistrictTrackerComponent %p  (sweeping for Vol_Dist_* actors)", dt);
        unsigned char b[0x400];
        if (!SafeRead(dt, b, sizeof(b))) { SAY("  unreadable"); return; }
        int hits = 0, other = 0;
        for (size_t off = 0; off + 4 <= sizeof(b); off += 4) {
            uint32_t h; memcpy(&h, b + off, 4);
            if (!h || (h & 0xFFFFF) == 0 || (h >> 20) == 0) continue;
            void* obj = nullptr;
            if (!SafeResolveHandle(&h, &obj) || !obj) continue;
            char an[160] = "";
            if (!SafeReadName(reinterpret_cast<Actor*>(obj), an, sizeof(an))) { ++other; continue; }
            if (_strnicmp(an, "Vol_Dist", 8) == 0) {
                char tn[128] = "?"; SafeTypeName(obj, tn, sizeof(tn));
                SAY("  ** +0x%03zX -> %-30s (%s)", off, an, tn);
                ++hits;
            } else {
                ++other;
                if (other <= 8) SAY("     +0x%03zX -> %s", off, an);
            }
        }
        SAY("  %d Vol_Dist actor(s), %d other resolvable handles", hits, other);
        if (!hits) SAY("  none found -- the current district may be stored as an index, not a handle");
        return;
    }

    if (_stricmp(verb, "district-log") == 0) {
        // Sample (hero position -> district) while flying around. Enough
        // samples give a labelled point cloud, and every check position can
        // then be classified by nearest sample -- no volume bounds needed.
        const bool on = (_stricmp(arg, "on") == 0);
        g_district_log.store(on, std::memory_order_relaxed);
        SAY("district sampling %s%s", on ? "ON" : "off",
            on ? " -- fly around; samples go to the runtime folder" : "");
        return;
    }


    if (_stricmp(verb, "count") == 0) {
        // Read-only: how many components of this name sit on the hero right
        // now. "Is Damage 6 applied?" = count LevelDamageMultiplierPerk.
        Actor* hero = HeroActor();
        uint32_t flags = 0; int count = 0; ComponentEntry* entries = nullptr;
        if (!hero || !SafeActorInfo(hero, &flags, &count, &entries) || !entries) { SAY("  no hero"); return; }
        int n = 0;
        for (int i = 0; i < count; ++i) {
            const char* name = nullptr; void* comp = nullptr;
            if (SafeEntry(entries, i, &name, &comp) && _stricmp(name, arg) == 0) ++n;
        }
        SAY("  %d x %s on the hero (%d components total)", n, arg, count);
        return;
    }
    if (_stricmp(verb, "reset-suittech") == 0)   { EnqueueStoreRequest(StoreOp::ResetSuitTech,    0, "suit tech"); return; }
    if (_stricmp(verb, "grant-suittech") == 0)   { EnqueueStoreRequest(StoreOp::GrantAllSuitTech, 0, "suit tech"); return; }
    if (_stricmp(verb, "health-recompute") == 0) { EnqueueStoreRequest(StoreOp::RecomputeHealth,  0, "HealthPerkHelper"); return; }
    if (_stricmp(verb, "send-location") == 0) {
        // Detection itself works (detector.py, validated on OW_SAND_MEMORY_05);
        // what is missing is the AP client that would forward this. Until then
        // this records the check and shows it, so the whole path is visible.
        INFO("LOCATION %s", arg);
        if (overlay::Running()) overlay::Push(0, arg);
        SAY("  location: %s   (logged and shown; no AP client to send it yet)", arg);
        return;
    }

    if (_stricmp(verb, "wearable") == 0) {
        // Queued, never inline: this does store WRITES, and writes off the
        // game thread crash the process (210 of them did exactly that).
        EnqueueStoreRequest(StoreOp::Wearable, 0, "wearable");
        return;
    }

    // --- district gates: decide here, write on the game thread (gate.inc) -------
    if (_stricmp(verb, "gate") == 0)            { gate::SetMode(arg); return; }
    if (_stricmp(verb, "unlock-district") == 0) { if (*arg) gate::Unlock(arg, true);  else SAY("usage: unlock-district <name>"); return; }
    if (_stricmp(verb, "lock-district") == 0)   { if (*arg) gate::Unlock(arg, false); else SAY("usage: lock-district <name>");   return; }
    if (_stricmp(verb, "gate-status") == 0)     { gate::Status(); return; }
    if (_stricmp(verb, "home") == 0)            { gate::Home(); return; }
    if (_stricmp(verb, "spawn-district") == 0)  { gate::SetSpawnPolicy(arg); return; }
    if (_stricmp(verb, "district-dump") == 0)   { gate::DumpTracker(); return; }
    if (_stricmp(verb, "health") == 0)          { gate::SayHealth(); return; }

    if (_stricmp(verb, "collectibles") == 0) {
        // Reads only -- the parser must never write a store, but a dump of
        // objects the game already handed us is safe from here.
        DumpCollectibles();
        return;
    }

    if (_stricmp(verb, "overlay") == 0) {
        // On-screen feed. Pure Win32 on its own thread -- it never touches the
        // renderer or the game thread, so the worst case is that it does not
        // appear (true exclusive fullscreen will not composite a layered
        // window; flip-model presentation, which most DX12 titles use, will).
        if (_stricmp(arg, "off") == 0) { overlay::Stop(); SAY("overlay off"); return; }
        // overlay pos <x> <y>  -- x from the left edge, y up from the bottom.
        // Placement takes a few tries to find a spot that does not fight the
        // HUD, and a rebuild per try costs a full game restart.
        if (_strnicmp(arg, "pos", 3) == 0) {
            int px = 40, py = 140;
            if (sscanf_s(arg + 3, "%d %d", &px, &py) == 2) {
                overlay::SetOffsets(px, py);
                SAY("overlay at x=%d, y=%d up from the bottom", px, py);
            } else {
                SAY("usage: overlay pos <x from left> <y up from bottom>");
            }
            return;
        }
        const bool ok = overlay::Start();
        SAY("overlay %s", ok ? "ON -- lower left of the game window" : "FAILED to create its window");
        if (ok) {
            overlay::Push(2, "Archipelago overlay ready");
            overlay::Push(0, "Prowler Tech 07");
            overlay::Push(1, "Venom Punch from Fossil");
        }
        return;
    }

    if (_stricmp(verb, "notify") == 0) {
        // notify [check|item] <text>  -- an explicit kind picks the colour;
        // without one it is a plain notice.
        if (!overlay::Running() && !overlay::Start()) { SAY("  overlay is not running"); return; }
        int kind = 2;
        const char* body = arg;
        if (_strnicmp(arg, "check ", 6) == 0)       { kind = 0; body = arg + 6; }
        else if (_strnicmp(arg, "item ", 5) == 0)   { kind = 1; body = arg + 5; }
        else if (_strnicmp(arg, "update ", 7) == 0) { kind = 3; body = arg + 7; }
        overlay::Push(kind, body);
        SAY("  pushed [%s]: %s", kind == 0 ? "check" : (kind == 1 ? "item" : (kind == 3 ? "update" : "link")), body);
        return;
    }

    if (_stricmp(verb, "prius") == 0) {
        // Read the item's prius: type and quantity. Read-only.
        char item[192] = ""; size_t nb = 0;
        const char* sp = strrchr(arg, ' ');
        if (sp && sp[1] && isdigit((unsigned char)sp[1])) {
            nb = (size_t)atoi(sp + 1);
            strncpy_s(item, arg, (size_t)(sp - arg));
        } else {
            strncpy_s(item, arg, _TRUNCATE);
        }
        char why[600];
        const int pi = FindItemIndex(item, why, sizeof(why));
        if (pi < 0) { SAY("  %s", why); return; }
        DumpPrius(kApItems[pi].hash, kApItems[pi].label, nb);
        return;
    }

    if (_stricmp(verb, "probe") == 0) {
        const bool on = (_stricmp(arg, "on") == 0);
        g_probe.store(on, std::memory_order_relaxed);
        SAY("purchase probe %s -- now pick a side of a suit-tech pair", on ? "ON" : "OFF");
        return;
    }

    if (_stricmp(verb, "objwatch") == 0) {
        const bool on = (_stricmp(arg, "on") == 0);
        g_objwatch.store(on, std::memory_order_relaxed);
        SAY("objective watch %s%s", on ? "ON" : "OFF",
            on ? " -- writes every frame, only turn this on to capture a check" : "");
        return;
    }

    if (_stricmp(verb, "dump") == 0) {
        // dump <item> [bytes] -- raw store records, for diffing an item before
        // and after a real craft. Read-only.
        char item[192] = ""; size_t n = 0x80;
        const char* sp = strrchr(arg, ' ');
        if (sp && sp[1] && isdigit((unsigned char)sp[1])) {
            n = (size_t)atoi(sp + 1);
            strncpy_s(item, arg, (size_t)(sp - arg));
        } else {
            strncpy_s(item, arg, _TRUNCATE);
        }
        if (n < 0x10) n = 0x10;
        char why[600];
        const int di = FindItemIndex(item, why, sizeof(why));
        if (di < 0) { SAY("  %s", why); return; }
        DumpItemRecords(kApItems[di].hash, kApItems[di].label, n);
        return;
    }

    if (_stricmp(verb, "hook-store") == 0) {
        // Dump the store's real add/remove addresses and move the trace hooks
        // onto them. Read-only until it hooks; safe to run any time.
        char why[600];
        const int hi = FindItemIndex(arg, why, sizeof(why));
        if (hi < 0) { SAY("  %s", why); return; }
        RehookStoreFromVtable(kApItems[hi].hash, kApItems[hi].label);
        return;
    }

    if (_stricmp(verb, "vtable") == 0) {
        // Read-only. Prints the store's real add/remove/count addresses so a
        // hook can go on the function the game actually calls.
        char why[600];
        const int vi = FindItemIndex(arg, why, sizeof(why));
        if (vi < 0) { SAY("  %s", why); return; }
        DumpStoreVtable(kApItems[vi].hash, kApItems[vi].label);
        return;
    }

    StoreOp op; bool known = true;
    if      (_stricmp(verb, "remove")  == 0) op = StoreOp::Remove;
    else if (_stricmp(verb, "zero")    == 0) op = StoreOp::Zero;      // remove the whole count (currency, stacks)
    else if (_stricmp(verb, "add")     == 0) op = StoreOp::Add;
    else if (_stricmp(verb, "equip")   == 0) op = StoreOp::Equip;
    else if (_stricmp(verb, "restore") == 0) op = StoreOp::Restore;
    else if (_stricmp(verb, "query")   == 0) op = StoreOp::Remove, known = false;
    else { SAY("  unknown command '%s'", verb); return; }

    char why[600];
    const int idx = FindItemIndex(arg, why, sizeof(why));
    if (idx < 0) { SAY("  %s", why); return; }
    const ApItem& it = kApItems[idx];

    if (!known) { StoreFor(it.hash, it.label); return; }   // query, read-only, runs here (reads only)

    if ((op == StoreOp::Equip || op == StoreOp::Restore) && it.slot == kNoEquip) {
        if (op == StoreOp::Equip) { SAY("  %s is not a slotted ability -- use add/remove", it.label); return; }
        op = StoreOp::Add;   // restore of a perk = add
    }
    EnqueueStoreRequest(op, it.hash, it.label, it.slot);
}

// Called from the hotkey thread every poll. Reads the whole file, truncates
// it, runs each line. Truncating first means a command is never run twice
// even if the game thread is slow to drain.
void PollCommandFile() {
    FILE* f = nullptr;
    if (fopen_s(&f, kCommandFile, "r+b") != 0 || !f) return;
    char buf[8192];
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    if (n == 0) { fclose(f); return; }
    buf[n] = 0;
    // truncate
    fclose(f);
    if (fopen_s(&f, kCommandFile, "wb") == 0 && f) fclose(f);

    char* p = buf;
    while (p && *p) {
        char* nl = strchr(p, '\n');
        if (nl) *nl = 0;
        RunCommand(p);
        p = nl ? nl + 1 : nullptr;
    }
}
