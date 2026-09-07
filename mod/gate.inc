// --- district gates ----------------------------------------------------------------
//
// The game cannot be made to hide a side mission from a mod (see notes: every
// reference to its ActivityBlockers machinery is descriptor plumbing, and
// destroying the mission's world actor does not gate it). What a mod CAN do is
// make a locked district unplayable, using only writes already proven live:
// the hero's position, the hero's current health, and the item stores.
//
// Three gates, chosen in the YAML:
//
//   boundary     cross into a locked district and you are put back where you
//                came from. A wall.
//   nights_grip  a warning, a few seconds of grace, then your health drains
//                to 1 and stays there until you leave. It never kills. You
//                can dash in for a bot; you cannot fight a Hunter Blind on
//                one hit point. Longest Night's default.
//   powers_fade  abilities and gadgets are stripped on entry and handed back
//                on exit. Combat missions become impossible; traversal stays.
//
// Which district the hero is in comes from the game's own DistrictTracker
// component, which names one of sixteen Vol_Dist_* volumes. Our fourteen AP
// districts map onto those; a volume is open when ANY district mapped to it
// is unlocked. Manhattan (the outer catch-all) and Coney Island are always
// open -- there is nothing to gate there.
//
// Thread rule, same as everywhere in this mod: the poll thread only reads and
// decides; every write is queued and runs on the game thread.

namespace gate {

enum class Mode : int { None = 0, Boundary = 1, Grip = 2, Fade = 3 };

std::atomic<int>  g_mode{0};
std::mutex        g_lock;
std::set<std::string> g_unlocked;            // our district names, as AP sends them
std::set<std::string> g_free_volumes;        // game volumes open regardless (the spawn district)
std::atomic<bool>     g_spawn_free{true};    // random_start off: where you wake up is yours

// Game volume -> the AP districts that open it.
struct VolumeMap { const char* volume; const char* districts[3]; };
const VolumeMap kVolumes[] = {
    { "Vol_Dist_Brooklyn",         { "Downtown Brooklyn", "Williamsburg", nullptr } },
    { "Vol_Dist_Queens",           { "Astoria", "Downtown Queens", nullptr } },
    { "Vol_Dist_CentralPark",      { "Central Park", nullptr, nullptr } },
    { "Vol_Dist_ChinaTown",        { "Chinatown", nullptr, nullptr } },
    { "Vol_Dist_FinancialDist",    { "Financial District", nullptr, nullptr } },
    { "Vol_Dist_GreenwichVillage", { "Greenwich", nullptr, nullptr } },
    { "Vol_Dist_Harlem",           { "Harlem", nullptr, nullptr } },
    { "Vol_Dist_EastHarlem",       { "Harlem", nullptr, nullptr } },
    { "Vol_Dist_HellsKitchen",     { "Hell's Kitchen", nullptr, nullptr } },
    { "Vol_Dist_LittleOdessa",     { "Little Odessa", nullptr, nullptr } },
    { "Vol_Dist_Midtown",          { "Midtown", nullptr, nullptr } },
    { "Vol_Dist_TimesSquare",      { "Midtown", nullptr, nullptr } },
    { "Vol_Dist_UpperEastSide",    { "Upper East Side", nullptr, nullptr } },
    { "Vol_Dist_UpperWestSide",    { "Upper West Side", nullptr, nullptr } },
    // always open: nothing lives there that we gate
    { "Vol_Dist_Manhattan",        { nullptr, nullptr, nullptr } },
    { "Vol_Dist_ConeyIsland",      { nullptr, nullptr, nullptr } },
};

const char* ModeName(Mode m) {
    switch (m) {
        case Mode::Boundary: return "boundary";
        case Mode::Grip:     return "nights_grip";
        case Mode::Fade:     return "powers_fade";
        default:             return "none";
    }
}

bool VolumeOpen(const char* volume, char* why, size_t n) {
    {
        std::lock_guard<std::mutex> g(g_lock);
        if (g_free_volumes.count(volume)) { if (why) sprintf_s(why, n, "yours -- you woke up here"); return true; }
    }
    for (const VolumeMap& v : kVolumes) {
        if (_stricmp(v.volume, volume) != 0) continue;
        if (!v.districts[0]) { if (why) sprintf_s(why, n, "always open"); return true; }
        std::lock_guard<std::mutex> g(g_lock);
        for (const char* d : v.districts) {
            if (d && g_unlocked.count(d)) { if (why) sprintf_s(why, n, "%s unlocked", d); return true; }
        }
        if (why) sprintf_s(why, n, "%s", v.districts[0]);      // the name to show
        return false;
    }
    if (why) sprintf_s(why, n, "unknown volume");
    return true;                                     // never gate what we cannot name
}

// The hero's current district volume, from the game's own tracker. Same walk
// SampleDistrict uses: the component holds handles, one of which resolves to
// a Vol_Dist_* actor.
bool CurrentVolume(char* out, size_t n) {
    void* dt = FindHeroComponent("DistrictTrackerComponent");
    if (!dt) return false;
    unsigned char b[0x400];
    if (!SafeRead(dt, b, sizeof(b))) return false;
    for (size_t off = 0; off + 4 <= sizeof(b); off += 4) {
        uint32_t h; memcpy(&h, b + off, 4);
        if (!h || (h & 0xFFFFF) == 0 || (h >> 20) == 0) continue;
        void* obj = nullptr;
        if (!SafeResolveHandle(&h, &obj) || !obj) continue;
        char an[160] = "";
        if (!SafeReadName(reinterpret_cast<Actor*>(obj), an, sizeof(an))) continue;
        if (_strnicmp(an, "Vol_Dist", 8) == 0) { strncpy_s(out, n, an, _TRUNCATE); return true; }
    }
    return false;
}

// Where to set the player down in a district: hand-placed centre
// points (district-anchors.json), lifted 40 m so the landing is a fall onto a
// roof or street rather than a spawn inside geometry. There is no fall damage.
struct Anchor { const char* district; float x, y, z; };
const Anchor kAnchors[] = {
    { "Hell's Kitchen",     -789.3f,  27.0f,   -18.9f },
    { "Midtown",             -57.0f, 161.0f,   -63.2f },
    { "Greenwich",          -111.0f,  51.8f,   952.7f },
    { "Chinatown",           963.4f,  35.3f,  1429.7f },
    { "Financial District",  120.7f, 182.8f,  2283.2f },
    { "Downtown Brooklyn",  2084.1f, 105.0f,  2989.1f },
    { "Williamsburg",       2783.5f,  87.9f,  1691.7f },
    { "Little Odessa",      2411.8f,  60.7f,   560.9f },
    { "Downtown Queens",    2072.0f, 121.3f,  -467.1f },
    { "Astoria",            2245.4f,  25.4f, -1428.9f },
    { "Harlem",             -235.8f,  20.5f, -2122.8f },
    { "Upper West Side",    -795.0f,  58.2f, -1348.1f },
    { "Central Park",       -220.1f,  -3.9f, -1247.4f },
    { "Upper East Side",     272.2f,   1.0f, -1232.2f },
};
constexpr float kDropHeight = 40.0f;

// Which district the hero is in, from position alone.
//
// The DistrictTrackerComponent walk (CurrentVolume above) reads no Vol_Dist_*
// handles -- "0 Vol_Dist actor(s), 18 other resolvable handles" live -- so it
// is kept for reference and not used. Position is classified against a point
// cloud instead: the fourteen hand-placed centres PLUS the game's own district
// volume positions (district-volumes.txt, 33 usable). Fourteen centres alone
// were too coarse at borders: standing in Greenwich at (392, 1229) the nearest
// centre was Chinatown by a hair; with the volume points Greenwich wins by
// 101 m to 425 m.
//
// Flicker control is a settle, not a distance bias: a new district has to be
// the nearest for three ticks in a row (1.5 s) before it is adopted.
struct Pt { const char* district; float x, y, z; };
const Pt kVolumePoints[] = {
    { "Downtown Brooklyn",   1393.0f,  400.0f,   4518.5f },   // Vol_Dist_Brooklyn
    { "Downtown Brooklyn",   2560.0f,  400.0f,   2579.0f },   // Vol_Dist_Brooklyn
    { "Central Park",   -256.0f,  400.0f,  -1263.2f },   // Vol_Dist_CentralPark
    { "Chinatown",   1019.9f,  400.0f,   1759.8f },   // Vol_Dist_ChinaTown
    { "Chinatown",   1037.0f,  400.0f,   1047.7f },   // Vol_Dist_ChinaTown
    { "Chinatown",   1094.3f,  400.0f,   1473.9f },   // Vol_Dist_ChinaTown
    { "Chinatown",   1172.9f,  400.0f,   1130.7f },   // Vol_Dist_ChinaTown
    { "Chinatown",   1353.9f,  400.0f,   1366.7f },   // Vol_Dist_ChinaTown
    { "Chinatown",    486.3f,  400.0f,    794.0f },   // Vol_Dist_ChinaTown
    { "Chinatown",    504.5f,  400.0f,    808.1f },   // Vol_Dist_ChinaTown
    { "Chinatown",    780.9f,  400.0f,   1058.0f },   // Vol_Dist_ChinaTown
    { "Chinatown",    990.6f,  400.0f,   1251.5f },   // Vol_Dist_ChinaTown
    { "Harlem",    442.0f,  400.0f,  -2300.4f },   // Vol_Dist_EastHarlem
    { "Financial District",  -1225.5f,  400.0f,   3628.2f },   // Vol_Dist_FinancialDist
    { "Financial District",    124.0f,  400.0f,   3392.0f },   // Vol_Dist_FinancialDist
    { "Financial District",    637.6f,  400.0f,   2565.6f },   // Vol_Dist_FinancialDist
    { "Financial District",    637.6f,  400.0f,   2565.6f },   // Vol_Dist_FinancialDist
    { "Financial District",    795.8f,  400.0f,   3646.9f },   // Vol_Dist_FinancialDist
    { "Greenwich",  -1574.8f,  400.0f,   1057.0f },   // Vol_Dist_GreenwichVillage
    { "Greenwich",  -2092.5f,  400.0f,   2074.1f },   // Vol_Dist_GreenwichVillage
    { "Greenwich",  -3042.6f,  400.0f,   1772.5f },   // Vol_Dist_GreenwichVillage
    { "Greenwich",    486.3f,  400.0f,   1193.5f },   // Vol_Dist_GreenwichVillage
    { "Harlem",  -1511.0f,  400.0f,  -3849.0f },   // Vol_Dist_Harlem
    { "Harlem",   -256.0f,  400.0f,  -1955.9f },   // Vol_Dist_Harlem
    { "Harlem",   -551.7f,  400.0f,  -2373.6f },   // Vol_Dist_Harlem
    { "Hell's Kitchen",  -1872.0f,  400.0f,      9.0f },   // Vol_Dist_HellsKitchen
    { "Little Odessa",   2548.0f,  400.0f,    784.5f },   // Vol_Dist_LittleOdessa
    { "Midtown",    305.0f,  400.0f,     11.0f },   // Vol_Dist_Midtown
    { "Downtown Queens",   2332.0f,  400.0f,  -2866.0f },   // Vol_Dist_Queens
    { "Midtown",   -260.0f,  400.0f,    -56.0f },   // Vol_Dist_TimesSquare
    { "Upper East Side",    374.0f,  400.0f,  -1274.0f },   // Vol_Dist_UpperEastSide
    { "Upper East Side",    543.0f,  400.0f,  -1274.0f },   // Vol_Dist_UpperEastSide
    { "Upper West Side",  -2066.0f,  400.0f,  -1270.0f },   // Vol_Dist_UpperWestSide
};

const char* NearestDistrict(const Vector3& p) {
    const char* best = nullptr; float bestd = 0.0f;
    for (const Anchor& a : kAnchors) {
        const float dx = p.x - a.x, dz = p.z - a.z, d = dx * dx + dz * dz;
        if (!best || d < bestd) { best = a.district; bestd = d; }
    }
    for (const Pt& a : kVolumePoints) {
        const float dx = p.x - a.x, dz = p.z - a.z, d = dx * dx + dz * dz;
        if (d < bestd) { best = a.district; bestd = d; }
    }
    return best;
}

// Raw bytes of the tracker component, so the real "current district" field
// can be found by diffing two dumps taken in two districts. Read-only.
void DumpTracker() {
    void* dt = FindHeroComponent("DistrictTrackerComponent");
    if (!dt) { SAY("district-dump: no DistrictTrackerComponent on the hero"); return; }
    unsigned char b[0x400];
    if (!SafeRead(dt, b, sizeof(b))) { SAY("district-dump: unreadable"); return; }
    Actor* hero = HeroActor(); Vector3 pos{};
    if (hero) SafeActorPos(hero, &pos);
    if (FILE* f = nullptr; fopen_s(&f, RuntimeFile("district-dump.log").c_str(), "a") == 0 && f) {
        SYSTEMTIME t; GetLocalTime(&t);
        fprintf(f, "%02d:%02d:%02d pos %.1f %.1f %.1f comp %p\n", t.wHour, t.wMinute, t.wSecond, pos.x, pos.y, pos.z, dt);
        for (size_t o = 0; o < sizeof(b); o += 32) {
            fprintf(f, "  %04zX ", o);
            for (size_t k = 0; k < 32; ++k) fprintf(f, "%02X", b[o + k]);
            fputc('\n', f);
        }
        fclose(f);
    }
    SAY("district-dump: 0x400 bytes written (hero at %.1f %.1f %.1f)", pos.x, pos.y, pos.z);
}

// The game's own answer. Two dumps of DistrictTrackerComponent, one per
// district, differed in exactly two non-position words: +0x48 (2 -> 3, an
// index) and +0x4C (a hash id). This reads them; the index -> name table is
// filled in from crossings (see notes) and until then the position classifier
// names the district while this logs what the game says.
bool TrackerDistrict(uint32_t* idx, uint32_t* id) {
    void* dt = FindHeroComponent("DistrictTrackerComponent");
    if (!dt) return false;
    uint32_t v[2] = {0, 0};
    if (!SafeRead(static_cast<char*>(dt) + 0x48, v, sizeof(v))) return false;
    *idx = v[0]; *id = v[1];
    return true;
}
uint32_t g_tracker_idx = 0xFFFFFFFF, g_tracker_id = 0;

// Index -> district, from crossings the player named while the log recorded the
// index. Greenwich read 2 in two separate sessions; +0x4C changes within a
// district (three ids inside Financial District) so it is a sub-area, not the
// district. Unknown indices fall back to the position classifier and are
// logged with its guess, so the table fills itself in as he plays.
struct TrackerName { uint32_t idx; const char* district; };
const TrackerName kTrackerNames[] = {
    { 1, "Hell's Kitchen" },
    { 2, "Greenwich" },
    { 3, "Chinatown" },
    { 4, "Financial District" },
};
const char* TrackerDistrictName(uint32_t idx) {
    for (const TrackerName& t : kTrackerNames) if (t.idx == idx) return t.district;
    return nullptr;
}

bool DistrictOpen(const char* district) {
    std::lock_guard<std::mutex> g(g_lock);
    return g_free_volumes.count(district) > 0 || g_unlocked.count(district) > 0;
}

// --- state the poll thread keeps -------------------------------------------------

char      g_volume[160] = "";        // where the hero is now
bool      g_locked_in = false;       // currently inside a locked volume
uint64_t  g_entered_ms = 0;          // when they crossed in
Vector3   g_safe_pos{};              // last position seen in an open volume
bool      g_have_safe = false;
uint64_t  g_last_drain_ms = 0;
uint64_t  g_last_nag_ms = 0;
char      g_locked_name[96] = "";
char      g_grace_volume[160] = "";  // spawn volume treated as open until first exit

constexpr uint64_t kGraceMs      = 5000;   // grip: warning before the drain starts
constexpr uint64_t kDrainEveryMs = 500;
constexpr float    kDrainFrac    = 0.02f;  // of max health per tick -> ~25 s down to 1
constexpr uint64_t kNagEveryMs   = 8000;

// Pending requests for the game thread, read by the executors below.
std::atomic<bool>  g_want_teleport{false};
Vector3            g_teleport_to{};
std::atomic<int>   g_want_drain{0};       // count of ticks owed
float              g_grip_ceiling = 0.0f;  // grip: health may not exceed this; falls each tick

// --- game-thread executors (called from ExecuteStoreRequest) ----------------------

void TeleportNow() {
    Actor* hero = HeroActor();
    if (!hero) return;
    __try {
        hero->SetPosition(g_teleport_to);
    } __except (RecordFault(GetExceptionInformation())) {
        WARN("gate: SetPosition faulted");
    }
}

// Change health by a delta through the game\'s own routine. Setting the value
// directly (vt[0x70]) moves the number but the HUD bar never wakes up -- it
// shows on damage events, and a raw set is not one. A player, watching their health
// fall with no bar: "sneaky". This is the path real damage takes.
bool SafeHealthDelta(void* hh, float delta) {
    __try {
        Rva<float (*)(void*, float)>(kRva_HealthDelta)(hh, delta);
        return true;
    } __except (RecordFault(GetExceptionInformation())) { return false; }
}

void DrainNow() {
    void* hh = FindHeroComponent("HeroHealth");
    if (!hh) return;
    const float max = ReadHealth(0xA0);
    const float cur = ReadHealth(0xD0);
    if (max <= 0.0f || cur <= 0.0f) return;
    // The grip bleeds you to the bone and no further: it never kills. Dying
    // would be a checkpoint reload, which is a worse gate than being left at 1.
    // A falling ceiling rather than a subtraction: auto-recovery was healing
    // between ticks and the bleed stalled. Each tick lowers the ceiling by 2 %
    // of max and clamps health to it; regen can only refill up to the ceiling.
    if (g_grip_ceiling <= 0.0f || g_grip_ceiling > cur) g_grip_ceiling = cur;
    g_grip_ceiling -= max * kDrainFrac;
    if (g_grip_ceiling < 1.0f) g_grip_ceiling = 1.0f;
    bool wrote = false; const char* how = "-";
    if (cur > g_grip_ceiling) {
        // Prefer the game\'s delta path so the HUD bar shows; fall back to the
        // direct setter if it faults or does not take.
        wrote = SafeHealthDelta(hh, g_grip_ceiling - cur); how = "delta";
        const float chk = ReadHealth(0xD0);
        if (!wrote || chk > g_grip_ceiling + 0.5f) { wrote = SafeSetCurrentHealth(hh, g_grip_ceiling); how = "set"; }
    }
    // Proof in the log, every ~4 s: what we read, what we set, and what the
    // game reads back a moment later.
    static uint64_t last_say = 0;
    const uint64_t now = GetTickCount64();
    if (now - last_say >= 4000) {
        last_say = now;
        const float after = ReadHealth(0xD0);
        SAY("grip: health %.1f -> ceiling %.1f (max %.0f)  via %s write=%s  readback=%.1f",
            cur, g_grip_ceiling, max, how, wrote ? "ok" : "no", after);
    }
}

// health: what the hero has right now, for checking the bleed by hand.
void SayHealth() {
    SAY("health: current %.1f / max %.1f   grip ceiling %.1f", ReadHealth(0xD0), ReadHealth(0xA0), g_grip_ceiling);
}

// Powers Fade: what was taken on entry, so exactly that comes back on exit.
std::vector<int> g_faded;                 // indexes into kApItems

void StripNow() {
    g_faded.clear();
    int taken = 0;
    for (int i = 0; i < kApItemCount; ++i) {
        const ApItem& it = kApItems[i];
        if (strcmp(it.kind, "ability") != 0 && strcmp(it.kind, "gadget") != 0) continue;
        void* store = ClaimingStore(it.hash);
        void* equip = StoreByIndex(kEquipStoreIndex);
        bool had = false;
        for (void* s : { equip, store }) {
            if (!s) continue;
            int32_t c = 0;
            if (SafeStoreCount(s, it.hash, &c) && c > 0) { had = true; DriveToZero(s, it.hash, it.label, nullptr); }
        }
        if (had) { g_faded.push_back(i); ++taken; }
    }
    SAY("gate: powers fade -- %d taken", taken);
}

void RestoreNow() {
    int back = 0;
    g_ap_granting = true;
    for (int i : g_faded) {
        const ApItem& it = kApItems[i];
        void* store = ClaimingStore(it.hash);
        if (store && SafeStoreOp(store, kSlotAdd, it.hash, +1)) ++back;
    }
    g_ap_granting = false;
    SAY("gate: powers return -- %d restored", back);
    g_faded.clear();
}

void Announce(int kind, const char* text) { overlay::Push(kind, text); SAY("gate: %s", text); }

// --- home: set the player down in a district they hold --------------------------
//
// A fresh save spawns wherever the story left you, which can be a locked
// district; without this the gate would fire on the first frame. Called by the
// client once the starting districts have arrived, and by Tick as a backstop
// when the hero is found in a locked volume with no safe position known yet.
bool Home() {
    // DISABLED 2026-09-06 16:21. The first live call teleported the hero ~3 km
    // to a district centre and the game crashed on the very next frame: the
    // world there was not streamed in. A cross-map SetPosition is not a safe
    // write; the game's own fast travel does far more than move the transform.
    // Spawning inside a locked district is handled by the grace rule in Tick
    // instead. Re-enable only with a teleport that goes through the game.
    SAY("gate: home is disabled (a long teleport crashed the game); the spawn district is treated as open instead");
    return false;
}

// --- the poll-thread tick ---------------------------------------------------------


void Tick() {
    const Mode mode = static_cast<Mode>(g_mode.load(std::memory_order_relaxed));
    if (mode == Mode::None) return;
    {
        // Not armed until at least one district is unlocked. On connect the
        // client sends `gate` first, then seeds (20 s), and the District items
        // -- hence the unlocks -- only land after that. With no unlocks known,
        // every volume would read as locked and the grip would start bleeding
        // the player on their spawn point during seeding.
        std::lock_guard<std::mutex> g(g_lock);
        if (g_unlocked.empty()) return;
    }

    static uint64_t last = 0;
    const uint64_t now = GetTickCount64();
    if (now - last < 500) return;
    last = now;

    Actor* hero = HeroActor();
    Vector3 pos{};
    const bool have_pos = hero && SafeActorPos(hero, &pos);
    if (!have_pos) return;                              // menu, loading
    if (pos.x < -1e5f || pos.x > 1e5f) return;          // not a world position yet

    static const char* current = nullptr;
    static const char* candidate = nullptr;
    static int candidate_ticks = 0;
    const char* nearest = NearestDistrict(pos);
    // Compare by NAME: the same district comes back as different pointers
    // from the anchor table and the volume table, and a pointer compare kept
    // resetting the settle so it could never switch (stuck on Greenwich while
    // standing in Chinatown, 16:56).
    if (!current) current = nearest;
    if (strcmp(nearest, current) != 0) {
        if (candidate && strcmp(nearest, candidate) == 0) { if (++candidate_ticks >= 3) { current = nearest; candidate = nullptr; candidate_ticks = 0; } }
        else { candidate = nearest; candidate_ticks = 1; }
    } else { candidate = nullptr; candidate_ticks = 0; }
    const char* named = current;                      // the classifier's answer
    {
        uint32_t idx = 0, id = 0;
        if (TrackerDistrict(&idx, &id)) {
            if (idx != g_tracker_idx || id != g_tracker_id) {
                g_tracker_idx = idx; g_tracker_id = id;
                SAY("district change: tracker idx=%u id=%08X   classifier=%s   pos=%.0f %.0f %.0f",
                    idx, id, current, pos.x, pos.y, pos.z);
            }
            const char* t = TrackerDistrictName(idx);
            if (t) named = t;                          // the game\'s own answer wins
        }
    }
    char vol[160] = "";
    strncpy_s(vol, named, _TRUNCATE);
    char why[96] = "";
    strncpy_s(why, named, _TRUNCATE);
    const bool open = DistrictOpen(vol);

    if (open) {
        if (have_pos) { g_safe_pos = pos; g_have_safe = true; }
        if (g_locked_in) {
            g_locked_in = false;
            g_grip_ceiling = 0.0f;
            overlay::SetEnergy(-1);
            if (mode == Mode::Fade) EnqueueStoreRequest(StoreOp::GateRestore, 0, "powers return");
            char m[160]; sprintf_s(m, "clear of %s -- catch your breath", g_locked_name);
            Announce(3, m);
        }
        strncpy_s(g_volume, vol, _TRUNCATE);
        return;
    }

    // Spawn grace: a fresh save (or a reload) can put the hero inside a locked
    // district before any safe position is known. Rather than teleport them
    // out -- which crashed the game -- that one volume counts as open until the
    // hero is seen somewhere else. You cannot be gated where you spawn.
    if (!g_have_safe && !g_grace_volume[0]) {
        if (g_spawn_free.load()) {
            // random_start off: the district you wake up in is your first free
            // one, for good. AP's own logic still treats it as gated -- doing
            // its checks early is merely out of logic, which Archipelago accepts.
            { std::lock_guard<std::mutex> g(g_lock); g_free_volumes.insert(vol); }
            char m[200]; sprintf_s(m, "%s -- home ground for tonight; you can work here", why);
            Announce(3, m);
            if (have_pos) { g_safe_pos = pos; g_have_safe = true; }
            strncpy_s(g_volume, vol, _TRUNCATE);
            return;
        }
        strncpy_s(g_grace_volume, vol, _TRUNCATE);
        char m[200]; sprintf_s(m, "%s -- you can start here, but once you leave, it is a long way back", why);
        Announce(3, m);
    }
    if (g_grace_volume[0] && _stricmp(g_grace_volume, vol) == 0) {
        if (have_pos) { g_safe_pos = pos; g_have_safe = true; }
        strncpy_s(g_volume, vol, _TRUNCATE);
        return;
    }
    if (g_grace_volume[0]) g_grace_volume[0] = 0;       // they left it: grace over

    // inside a locked volume
    if (!g_locked_in) {
        g_locked_in = true;
        g_entered_ms = now;
        g_last_nag_ms = now;
        strncpy_s(g_locked_name, why, _TRUNCATE);
        char m[200];
        switch (mode) {
            case Mode::Boundary:
                sprintf_s(m, "%s -- not tonight; you turn back", why);
                Announce(3, m);
                if (g_have_safe) { g_teleport_to = g_safe_pos; EnqueueStoreRequest(StoreOp::GateTeleport, 0, "boundary"); }
                g_locked_in = false;        // the teleport puts them back; re-arm
                break;
            case Mode::Grip:
                sprintf_s(m, "%s -- long night ahead; best not to waste your energy here", why);
                Announce(3, m);
                break;
            case Mode::Fade:
                sprintf_s(m, "%s -- you are too spent for this; your powers fade until you fall back", why);
                Announce(3, m);
                EnqueueStoreRequest(StoreOp::GateStrip, 0, "powers fade");
                break;
            default: break;
        }
        strncpy_s(g_volume, vol, _TRUNCATE);
        return;
    }

    if (g_locked_in && _stricmp(g_locked_name, why) != 0) {
        // Crossed from one locked district straight into another: the warning
        // was still naming the first one ("get out of Greenwich" in FD).
        strncpy_s(g_locked_name, why, _TRUNCATE);
        g_entered_ms = now; g_last_nag_ms = now;
        char m[200]; sprintf_s(m, "%s -- still nowhere to rest; keep moving", why);
        Announce(3, m);
    }

    if (mode == Mode::Grip && now - g_entered_ms >= kGraceMs) {
        if (now - g_last_drain_ms >= kDrainEveryMs) {
            g_last_drain_ms = now;
            EnqueueStoreRequest(StoreOp::GateDrain, 0, "night's grip", -1);
        }
        // The meter: what is left, as the game's HUD will not show it.
        {
            const float mx = ReadHealth(0xA0), cur = ReadHealth(0xD0);
            if (mx > 0.0f) overlay::SetEnergy(static_cast<int>(cur / mx * 100.0f + 0.5f));
        }
        if (now - g_last_nag_ms >= kNagEveryMs) {
            g_last_nag_ms = now;
            char m[160]; sprintf_s(m, "you are running on empty -- get clear of %s", g_locked_name);
            Announce(3, m);
        }
    }
    strncpy_s(g_volume, vol, _TRUNCATE);
}

// --- commands (parser: read + decide only; never write a store here) ---------------

void SetMode(const char* name) {
    Mode m = Mode::None;
    if (_stricmp(name, "boundary") == 0)    m = Mode::Boundary;
    else if (_stricmp(name, "nights_grip") == 0 || _stricmp(name, "grip") == 0) m = Mode::Grip;
    else if (_stricmp(name, "powers_fade") == 0 || _stricmp(name, "fade") == 0) m = Mode::Fade;
    const Mode before = static_cast<Mode>(g_mode.load());
    if (before == Mode::Fade && m != Mode::Fade && g_locked_in)
        EnqueueStoreRequest(StoreOp::GateRestore, 0, "powers return");
    g_mode.store(static_cast<int>(m));
    g_locked_in = false;
    g_grace_volume[0] = 0;
    g_grip_ceiling = 0.0f;
    overlay::SetEnergy(-1);
    SAY("district gate: %s", ModeName(m));
}

void Unlock(const char* district, bool on) {
    {
        std::lock_guard<std::mutex> g(g_lock);
        if (on) g_unlocked.insert(district); else g_unlocked.erase(district);
    }
    SAY("district %s: %s", on ? "unlocked" : "locked", district);
    if (on) { char m[160]; sprintf_s(m, "%s is open -- you have earned some ground", district); overlay::Push(3, m); }
    if (on && g_locked_in && _stricmp(g_locked_name, district) == 0) {
        // They are standing in the district that just opened: end the effect now.
        g_locked_in = false;
        overlay::SetEnergy(-1);
        if (static_cast<Mode>(g_mode.load()) == Mode::Fade) EnqueueStoreRequest(StoreOp::GateRestore, 0, "powers return");
    }
}

void SetSpawnPolicy(const char* arg) {
    const bool free = (_stricmp(arg, "free") == 0);
    g_spawn_free.store(free);
    SAY("spawn district: %s", free ? "free (where you wake up is yours)" : "grace (open only until you leave it)");
}

void Status() {
    std::lock_guard<std::mutex> g(g_lock);
    SAY("district gate: %s   in: %s%s", ModeName(static_cast<Mode>(g_mode.load())),
        g_volume[0] ? g_volume : "? (no position yet)", g_locked_in ? "  (LOCKED)" : "");
    std::string list;
    for (const std::string& d : g_unlocked) { if (!list.empty()) list += ", "; list += d; }
    SAY("  unlocked (%d): %s", static_cast<int>(g_unlocked.size()), list.empty() ? "none" : list.c_str());
    std::string fv; for (const std::string& v : g_free_volumes) { if (!fv.empty()) fv += ", "; fv += v; }
    SAY("  free volumes: %s   spawn policy: %s", fv.empty() ? "none" : fv.c_str(), g_spawn_free.load() ? "free" : "grace");
    SAY("  tracker: idx=%u id=%08X", g_tracker_idx, g_tracker_id);
}

}  // namespace gate
