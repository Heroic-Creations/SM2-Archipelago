// -------------------------------------------------------------- inventory.inc
//
// The item primitives, proven live on 2026-09-04:
//
//   abilities   remove(-1) on the store that claims the item locks it and
//               empties its slot, live. Restore = add(+1) THEN equip into the
//               item's own slot (equip without add makes a ghost).
//   suit tech   remove(-1) on GameEquipInventory un-owns the record (menu),
//               and the game's own unequip walk calls the PerkItem's
//               re-activate with count 0, tearing the perk components down;
//               its pipeline refreshes max health and the HUD. add(+1) applies
//               a tier and persists across reload. We chain a recompute + HUD
//               nudge after either as belt-and-braces.
//   skills      the same item mechanism (perk items in the progression tree);
//               untested at the time of writing.
//
// Every engine call runs on the game thread, from a request queue drained by
// the AddComponent and GetActor hooks and gated by IsGameThread(). Calls made
// off-thread mutate and then fault in the frame-context getter.
//
// Store layout (all stores share one base implementation):
//   GameInventoryManager (hero component) holds nine 4-byte handles at +0x2108:
//     [0] GameGeneralInventory  [4] GameEquipInventory  [5] GameVanityInventory
//     [6] GameMissionInventory  [8] GameVanityEquippedInventory
//   mgr->vt[0x3C0](mgr, hash)              -> the store that claims this item type
//   store->vt[0x60](store, hash)           -> owned entry (catalogue record), null if none
//   store->vt[0x198](store, hash)          -> equipped record (table at store+0x2140)
//   store->vt[0x1C8](store, hash)          -> int count of records for the hash
//   store->vt[0x80](store, &out, hash, +n) -> add        store->vt[0x98](..., -n) -> remove
//   out is an out-handle the game releases straight after; we do the same.

// --- slots and kinds ----------------------------------------------------------------

constexpr size_t kSlotFindOwned    = 0x60;
constexpr size_t kSlotAdd          = 0x80;
constexpr size_t kSlotRemove       = 0x98;
constexpr size_t kSlotFindEquipped = 0x198;
constexpr size_t kSlotGetCount     = 0x1C8;
constexpr size_t kSlotStoreForHash = 0x3C0;
constexpr size_t kHandleTable      = 0x2108;
constexpr int    kEquipStoreIndex  = 4;

// Suits and styles need a second write. A plain add to GameVanityInventory
// only makes a suit LISTED -- the menu still shows it as a purchase
// (UNAFFORDABLE), and with currency zeroed it can never be bought. Adding the
// same hash to GameVanityEquippedInventory as well is what makes it wearable;
// Confirmed in game on Miles Morales 2020. GameEquipInventory refuses these
// outright (give-to 4 reported 0 -> 0), which is why crafting them destroyed
// the wardrobe.
constexpr int    kVanityEquippedStoreIndex = 8;

constexpr int32_t kNoEquip = -2;   // item slot value meaning "perk: not slotted"

// --- guarded store calls ----------------------------------------------------------

using StoreOpFn      = void* (*)(void* store, void* out, uint64_t hash, int32_t amount);
using StoreFindFn    = void* (*)(void* store, uint64_t hash);
using StoreCountFn   = int32_t (*)(void* store, uint64_t hash);
using StoreForHashFn = void* (*)(void* mgr, uint64_t hash);
using EquipFn        = bool (*)(void* equip_store, uint64_t hash, int32_t slot, uint8_t flag);

bool SafeStoreForHash(void* mgr, uint64_t hash, void** out) {
    __try {
        void** vt = *reinterpret_cast<void***>(mgr);
        *out = reinterpret_cast<StoreForHashFn>(vt[kSlotStoreForHash / 8])(mgr, hash);
        return true;
    } __except (RecordFault(GetExceptionInformation())) { return false; }
}

bool SafeStoreFind(void* store, size_t slot, uint64_t hash, void** entry) {
    __try {
        void** vt = *reinterpret_cast<void***>(store);
        *entry = reinterpret_cast<StoreFindFn>(vt[slot / 8])(store, hash);
        return true;
    } __except (RecordFault(GetExceptionInformation())) { return false; }
}

bool SafeStoreCount(void* store, uint64_t hash, int32_t* out) {
    __try {
        void** vt = *reinterpret_cast<void***>(store);
        *out = reinterpret_cast<StoreCountFn>(vt[kSlotGetCount / 8])(store, hash);
        return true;
    } __except (RecordFault(GetExceptionInformation())) { return false; }
}

// ---- lockdown: only Archipelago may grant ------------------------------------
//
// Design rule: keep currency at 0; only Archipelago grants.
// Every store shares one add implementation (exe+0x34BC5E0), so hooking that
// single function refuses every game-initiated grant -- mission rewards,
// pickups, level-ups. Our own adds set g_ap_granting first, so they pass.
//
// Off by default: blanket refusal could stall a story beat that waits on
// receiving something. `lockdown on` arms it.
constexpr uintptr_t kRva_StoreAddImpl = 0x34BC5E0;
inline const char* kStateFileStr() { static const std::string p = RuntimeFile("ap-state.txt"); return p.c_str(); }
#define kStateFile kStateFileStr()
std::atomic<bool> g_lockdown{false};
// Store-call tracing: logs each add's caller so the game shows us its own
// purchase path instead of us hunting it in the disassembly.
std::atomic<bool> g_trace{false};

// Objective state fires every frame per live objective. Left on by default it
// wrote a 1.3 GB log in one session, so it is opt-in now.
std::atomic<bool> g_objwatch{false};

// The purchase-path probe: off until we are about to trigger a choice.
std::atomic<bool> g_probe{false};

// Sampling (hero position -> district) to build a labelled point cloud.
std::atomic<bool> g_district_log{false};

// Collect Vol_Dist_* actors as the game resolves them.
std::atomic<bool> g_collect_vols{false};
// Suits are deliberately out of scope (2026-09-05): granting them in bulk left
// both heroes invisible twice, and their vanilla unlock path works fine.

// Lockdown has to survive a restart, or the first thing a reloaded game does
// is start paying the player again.
void SaveLockdownState() {
    if (FILE* f = nullptr; fopen_s(&f, kStateFile, "w") == 0 && f) {
        fprintf(f, "lockdown=%d\n", g_lockdown.load() ? 1 : 0);
        fclose(f);
    }
}
void LoadLockdownState() {
    if (FILE* f = nullptr; fopen_s(&f, kStateFile, "r") == 0 && f) {
        char line[64] = "";
        if (fgets(line, sizeof(line), f)) g_lockdown.store(strstr(line, "lockdown=1") != nullptr);
        fclose(f);
    }
}
thread_local bool g_ap_granting = false;
std::atomic<int>  g_refused{0};

bool SafeStoreOp(void* store, size_t slot, uint64_t hash, int32_t amount) {
    __try {
        void** vt = *reinterpret_cast<void***>(store);
        StoreOpFn fn = reinterpret_cast<StoreOpFn>(vt[slot / 8]);
        if (!fn) return false;
        alignas(16) unsigned char out[64] = {};
        g_ap_granting = true;             // this add is ours; the lockdown hook lets it through
        fn(store, out, hash, amount);
        g_ap_granting = false;
        Rva<void (*)(void*)>(kRva_ReleaseOut)(out);
        return true;
    } __except (RecordFault(GetExceptionInformation())) { return false; }
}

bool SafeEquip(void* equip_store, uint64_t hash, int32_t slot, bool* out) {
    __try { *out = Rva<EquipFn>(kRva_Equip)(equip_store, hash, slot, 0); return true; }
    __except (RecordFault(GetExceptionInformation())) { return false; }
}

// --- stores -----------------------------------------------------------------------

// Defined below; used by seeding and by the currency sweep.
bool DriveToZero(void* store, uint64_t hash, const char* label, int32_t* out_before);


// Suits are never blocked: the game grants loadout pieces to build the hero.
bool HashIsSuit(uint64_t hash) {
    for (int i = 0; i < kApItemCount; ++i)
        if (kApItems[i].hash == hash) return strcmp(kApItems[i].kind, "suit") == 0;
    return false;
}
void ApplyAllDoublePerks(uint32_t seed);   // doubleperks.inc

void* InventoryManager() { return FindHeroComponent("GameInventoryManager"); }

void* StoreByIndex(int index) {
    void* mgr = InventoryManager();
    if (!mgr) return nullptr;
    uint32_t h = 0;
    if (!SafeRead(reinterpret_cast<char*>(mgr) + kHandleTable + index * 4, &h, 4) || !h) return nullptr;
    void* store = nullptr;
    return SafeResolveHandle(&h, &store) ? store : nullptr;
}

// The store the manager says owns this item type, or null.
void* ClaimingStore(uint64_t hash) {
    void* mgr = InventoryManager();
    void* store = nullptr;
    if (!mgr || !SafeStoreForHash(mgr, hash, &store)) return nullptr;
    return store;
}

// Craft an item the way the game does. Comparing a perk crafted in the
// menu against one I injected showed the difference exactly:
//     crafted   General=0  Equip=1     owned and active
//     available General=1  Equip=0     visible in the menu, not owned
//     injected  General=0  Equip=3     nonsense: owned yet "UNAFFORDABLE"
// So a craft is a MOVE, not an add: clear the claiming store, leave exactly
// one in the Equip store. Doing it by hand is the same end state the game's
// own purchase produces, without needing the currency.
bool CraftItem(uint64_t hash, const char* label) {
    void* from = ClaimingStore(hash);
    void* to   = StoreByIndex(kEquipStoreIndex);
    if (!to) { WARN("%s: no equip store", label); return false; }

    if (from && from != to) DriveToZero(from, hash, label, nullptr);

    int32_t have = 0;
    SafeStoreCount(to, hash, &have);
    if (have > 1) { DriveToZero(to, hash, label, nullptr); have = 0; }   // undo any stacking
    if (have < 1) {
        if (!SafeStoreOp(to, kSlotAdd, hash, 1)) { WARN("%s: equip-store add faulted", label); return false; }
        SafeStoreCount(to, hash, &have);
    }
    // >= 1, not == 1: the count read straight after an add sometimes lags,
    // and reporting "(failed)" on a craft that plainly worked -- the caller's
    // own line said equip=1 -- is worse than tolerating a stack here.
    return have >= 1;
}

// The inverse of CraftItem: return an item to "available" -- out of the Equip
// store, exactly one in the claiming store so the menu still offers it.
// Deleting it from Equip alone leaves it in neither, which is what made
// Confidence Boost disappear from the Health pair entirely.
bool UncraftItem(uint64_t hash, const char* label) {
    void* eq   = StoreByIndex(kEquipStoreIndex);
    void* home = ClaimingStore(hash);
    if (eq) DriveToZero(eq, hash, label, nullptr);
    if (!home) return false;
    int32_t have = 0;
    SafeStoreCount(home, hash, &have);
    if (have > 1) { DriveToZero(home, hash, label, nullptr); have = 0; }
    if (have < 1) {
        if (!SafeStoreOp(home, kSlotAdd, hash, 1)) { WARN("%s: could not return it to the menu", label); return false; }
        SafeStoreCount(home, hash, &have);
    }
    return have == 1;
}

// Own a cosmetic: present in its claiming store AND in VanityEquipped.
// Both sides are guarded so re-seeding cannot stack them.
bool GrantCosmetic(uint64_t hash, const char* label) {
    void* claim = ClaimingStore(hash);
    if (!claim) { WARN("%s: no store claims it", label); return false; }
    int32_t have = 0;
    SafeStoreCount(claim, hash, &have);
    if (have <= 0) SafeStoreOp(claim, kSlotAdd, hash, +1);

    // The wearable write is the one that matters, and it is READ BACK. A seed
    // once reported 443 granted while every suit sat at [8] count=0 -- listed
    // but not wearable -- because this returned true without checking. If the
    // store is missing or the add does not stick, say so; `wearable` can sweep
    // it again later without a full re-seed.
    void* worn = StoreByIndex(kVanityEquippedStoreIndex);
    if (!worn) { WARN("%s: no VanityEquipped store yet", label); return false; }
    int32_t on = 0;
    SafeStoreCount(worn, hash, &on);
    if (on <= 0) {
        SafeStoreOp(worn, kSlotAdd, hash, +1);
        SafeStoreCount(worn, hash, &on);
    }
    return on > 0;
}

// --- query --------------------------------------------------------------------------

// One line on the console; the detail goes to the file.
void* StoreFor(uint64_t hash, const char* label) {
    void* store = ClaimingStore(hash);
    if (!store) { SAY("  %-24s no store claims it", label); return nullptr; }

    char tn[128] = "?"; SafeTypeName(store, tn, sizeof(tn));
    void* entry = nullptr; SafeStoreFind(store, kSlotFindOwned, hash, &entry);
    int32_t gen = -1, eq = -1, active = -1;
    SafeStoreCount(store, hash, &gen);
    if (void* equip = StoreByIndex(kEquipStoreIndex)) {
        SafeStoreCount(equip, hash, &eq);
        void* rec = nullptr;
        if (SafeStoreFind(equip, kSlotFindEquipped, hash, &rec) && rec)
            SafeRead(reinterpret_cast<char*>(rec) + 0x1B8, &active, 4);   // PerkItem::m_ActivePerkComponents count
    }
    SAY("  %-24s %-26s held=%-3s  count: general=%d equip=%d  live perk components=%d",
        label, tn, entry ? "yes" : "no", gen, eq, active);
    INFO("query %s: store=%p entry=%p", label, store, entry);
    return store;
}

// --- max health -----------------------------------------------------------------------
//
// HeroHealth+0xA0 = max, +0xD0 = current. HealthPerkHelper keeps the list of
// HealthPerk components; exe+0x157E4F0(helper) re-sums it into SetMaxHealth.
// The HUD only redraws through the health pipeline, whose entry is the
// current-health setter HeroHealth->vt[0x70](health, value).

float ReadHealth(size_t off) {
    void* hh = FindHeroComponent("HeroHealth");
    float v = -1.f;
    if (hh) SafeRead(reinterpret_cast<char*>(hh) + off, &v, sizeof(v));
    return v;
}

bool SafeSetCurrentHealth(void* hh, float v) {
    __try {
        void** vt = *reinterpret_cast<void***>(hh);
        reinterpret_cast<void (*)(void*, float)>(vt[0x70 / 8])(hh, v);
        return true;
    } __except (RecordFault(GetExceptionInformation())) { return false; }
}

bool SafeRecomputeHealth(void* helper) {
    __try { Rva<void (*)(void*)>(kRva_HealthRecompute)(helper); return true; }
    __except (RecordFault(GetExceptionInformation())) { return false; }
}

void RecomputeHealthNow() {
    void* helper = FindHeroComponent("HealthPerkHelper");
    void* hh     = FindHeroComponent("HeroHealth");
    if (!helper || !hh) { WARN("no HealthPerkHelper / HeroHealth on the hero"); return; }
    const float before = ReadHealth(0xA0);
    if (!SafeRecomputeHealth(helper)) { WARN("health recompute FAULTED 0x%08X @ exe+0x%llX", FAULT_DETAIL); return; }
    const float mx = ReadHealth(0xA0), cur = ReadHealth(0xD0);
    if (!SafeSetCurrentHealth(hh, (mx > 0.f && cur > mx) ? mx : cur)) WARN("HUD nudge FAULTED 0x%08X @ exe+0x%llX", FAULT_DETAIL);
    SAY("  max health %.0f -> %.0f", before, mx);
}

// --- suit tech ------------------------------------------------------------------------
//
// configs/equipment/perk_hero_stat_<stat>_<n>.config, n = 1..6, "+10 Health"
// per tier. Health 1 is the tutorial grant -- the vanilla floor.

struct SuitTechItem { const char* label; uint64_t hash; bool keep_on_reset; };
const SuitTechItem kSuitTech[] = {
    { "SuitTech Health 1", 0xB23F702B5A5B4448ull, true  },
    { "SuitTech Health 2", 0x8D79B45DA3BABB99ull, false },
    { "SuitTech Health 3", 0xBBD61136525B6C49ull, false },
    { "SuitTech Health 4", 0x9742177ABBBAC39Aull, false },
    { "SuitTech Health 5", 0xA1EDB2114A5B144Aull, false },
    { "SuitTech Health 6", 0x9EAB7667B3BAEB9Bull, false },
    { "SuitTech Damage 1", 0x94C010B8E66A132Full, false },
    { "SuitTech Damage 2", 0xAB86D4CE1F8BECFEull, false },
    { "SuitTech Damage 3", 0x9D2971A5EE6A3B2Eull, false },
    { "SuitTech Damage 4", 0xB1BD77E9078B94FDull, false },
    { "SuitTech Damage 5", 0x8712D282F66A432Dull, false },
    { "SuitTech Damage 6", 0xB85416F40F8BBCFCull, false },
    { "SuitTech Focus 1",  0x8709213FD05BD89Cull, false },
    { "SuitTech Focus 2",  0xB84FE54929BA274Dull, false },
    { "SuitTech Focus 3",  0x8EE04022D85BF09Dull, false },
    { "SuitTech Focus 4",  0xA274466E31BA5F4Eull, false },
    { "SuitTech Focus 5",  0x94DBE305C05B889Eull, false },
    { "SuitTech Focus 6",  0xAB9D277339BA774Full, false },
};
constexpr int kSuitTechCount = static_cast<int>(sizeof(kSuitTech) / sizeof(*kSuitTech));

// --- the request queue ------------------------------------------------------------------

enum class StoreOp { Remove, Add, Equip, Restore, ResetSuitTech, GrantAllSuitTech, RecomputeHealth, Zero, SeedEmpty, SeedFull, ZeroCurrency, Wearable, Craft, Uncraft, GiveTo, RawHash, GrantSuits, DoublePerks,
                     GateTeleport, GateDrain, GateStrip, GateRestore };

// District-gate effects run on the game thread like every other write. They
// live in gate.inc, which is compiled after this file, hence the declarations.
namespace gate { void TeleportNow(); void DrainNow(); void StripNow(); void RestoreNow(); }

const char* OpName(StoreOp op) {
    switch (op) {
        case StoreOp::Zero:             return "zero (remove the whole count)";
        case StoreOp::Remove:           return "remove";
        case StoreOp::Add:              return "add";
        case StoreOp::Equip:            return "equip";
        case StoreOp::Restore:          return "restore (add, then equip)";
        case StoreOp::ResetSuitTech:    return "reset suit tech (keeps Health 1)";
        case StoreOp::GrantAllSuitTech: return "grant all suit tech";
        case StoreOp::RecomputeHealth:  return "recompute max health";
        case StoreOp::SeedEmpty:        return "SEED EMPTY (strip everything, lock the game out)";
        case StoreOp::SeedFull:         return "SEED FULL (grant everything, game unlocked)";
        case StoreOp::Wearable:         return "make every suit and style wearable";
        case StoreOp::Craft:            return "craft";
        case StoreOp::Uncraft:          return "uncraft";
        case StoreOp::GiveTo:           return "give to store";
        case StoreOp::RawHash:          return "raw hash";
        case StoreOp::GrantSuits:       return "grant the missing suits";
        case StoreOp::DoublePerks:      return "apply the paired perks";
        case StoreOp::GateTeleport:     return "gate: boundary teleport";
        case StoreOp::GateDrain:        return "gate: night's grip";
        case StoreOp::GateStrip:        return "gate: powers fade";
        case StoreOp::GateRestore:      return "gate: powers return";
    }
    return "?";
}

struct StoreRequest { StoreOp op; uint64_t hash; const char* label; int32_t slot; uint64_t queued_ms; int32_t amount; };
std::mutex                g_req_lock;
std::vector<StoreRequest> g_requests;

void EnqueueStoreRequest(StoreOp op, uint64_t hash, const char* label, int32_t slot = -1, int32_t amount = 1) {
    { std::lock_guard<std::mutex> g(g_req_lock); g_requests.push_back({op, hash, label, slot, GetTickCount64(), amount}); }
    if (op != StoreOp::GateDrain) INFO("queued %s %s", OpName(op), label);
}

bool PopStoreRequest(StoreRequest* out) {
    std::lock_guard<std::mutex> g(g_req_lock);
    if (g_requests.empty()) return false;
    *out = g_requests.front();
    g_requests.erase(g_requests.begin());
    return true;
}

// --- executing a request (game thread only) ------------------------------------------

void DoEquip(void* equip, uint64_t hash, const char* label, int32_t slot) {
    // The item's own slot first (DefaultSlot - 1); the others only if refused.
    int32_t order[4] = {0, 1, 2, 3};
    if (slot >= 0 && slot < 4) { order[0] = slot; for (int32_t s = 0, k = 1; s < 4; ++s) if (s != slot) order[k++] = s; }
    for (int32_t s : order) {
        bool ok = false;
        if (!SafeEquip(equip, hash, s, &ok)) { WARN("equip %s slot %d FAULTED 0x%08X @ exe+0x%llX", label, s, FAULT_DETAIL); return; }
        if (ok) { SAY("* equipped %s into slot %d", label, s); return; }
    }
    SAY("  %s: no slot accepted it", label);
}

void DoSuitTechColumn(bool reset) {
    void* mgr = InventoryManager(); void* equip = StoreByIndex(kEquipStoreIndex);
    if (!mgr || !equip) { WARN("no inventory manager / equip store"); return; }
    int changed = 0;
    for (int i = 0; i < kSuitTechCount; ++i) {
        const SuitTechItem& it = kSuitTech[i];
        int32_t count = 0;
        if (!SafeStoreCount(equip, it.hash, &count)) continue;
        if (reset) {
            if (it.keep_on_reset || count <= 0) continue;
            for (int k = 0; k < 10 && count > 0; ++k) {
                if (!SafeStoreOp(equip, kSlotRemove, it.hash, -1)) { WARN("%s remove FAULTED 0x%08X @ exe+0x%llX", it.label, FAULT_DETAIL); break; }
                int32_t after = count;
                if (!SafeStoreCount(equip, it.hash, &after) || after >= count) { WARN("%s: count did not drop (%d)", it.label, after); break; }
                count = after;
            }
            SAY("- removed %s", it.label); ++changed;
        } else {
            if (count > 0) continue;
            void* claimed = ClaimingStore(it.hash);
            if (!claimed) { WARN("%s: no store claims it", it.label); continue; }
            if (!SafeStoreOp(claimed, kSlotAdd, it.hash, +1)) { WARN("%s add FAULTED 0x%08X @ exe+0x%llX", it.label, FAULT_DETAIL); continue; }
            SAY("+ added %s", it.label); ++changed;
        }
    }
    SAY("  %s: %d changed", reset ? "suit tech reset" : "suit tech granted", changed);
    if (changed) RecomputeHealthNow();
}

// ---- seeding: the two shapes of the randomizer -------------------------------
//
// Both shapes exist, chosen in the YAML:
//   empty : the player starts with nothing; Archipelago grants everything, and
//           lockdown stops the game handing anything out on its own.
//   full  : the player starts with everything; checks strip it away, and the
//           game is left free to grant normally.
// One request each, run on the game thread, so a whole seed is one command.

// Design note: "what does it matter if I am level 1 or not? Grant max level and then
// hard set skill points and all of the other things to zero." Exactly right --
// level is only a gate-key (it decides which suits and upgrades are *allowed*),
// while skill points and currency are the actual power. So max the level to
// open every gate, and hold the spendables at zero.
//
// Order matters: lockdown is armed BEFORE the XP grant, so the level-ups' own
// skill-point awards are refused as they happen rather than swept up after.
constexpr int32_t kMaxLevelXP = 999999;   // past the level-60 curve; the game caps it

void DoSeed(bool full) {
    void* equip = StoreByIndex(kEquipStoreIndex);
    if (!InventoryManager() || !equip) { WARN("no inventory manager / equip store"); return; }
    SAY("=== SEED %s: %d items ===", full ? "FULL" : "EMPTY", kApItemCount);

    g_lockdown.store(true);
    SaveLockdownState();
    for (int i = 0; i < kApItemCount; ++i) {
        if (strcmp(kApItems[i].label, "XP") != 0) continue;
        if (void* xs = ClaimingStore(kApItems[i].hash)) {
            int32_t have = 0; SafeStoreCount(xs, kApItems[i].hash, &have);
            if (have < kMaxLevelXP && SafeStoreOp(xs, kSlotAdd, kApItems[i].hash, kMaxLevelXP - have))
                SAY("^ XP raised to %d (max level: every level gate now passes)", kMaxLevelXP);
        }
        break;
    }

    int granted = 0, removed = 0, zeroed = 0, equipped = 0;
    for (int i = 0; i < kApItemCount; ++i) {
        const ApItem& it = kApItems[i];
        const bool currency = (strcmp(it.kind, "currency") == 0);
        void* store = ClaimingStore(it.hash);
        if (!store) continue;

        // Currency is zeroed in BOTH modes. In "full" the player owns
        // everything, so money is not just useless -- it is a leak: a check
        // removes Venom Punch and the player simply re-buys it, and the
        // removal means nothing. Same reason lockdown is armed either way.
        if (currency) {
            if (strcmp(it.label, "XP") == 0) continue;   // held at max, not zero
            int32_t before = 0;
            if (DriveToZero(store, it.hash, it.label, &before) && before > 0) ++zeroed;
            continue;
        }

        // Suits and styles are granted in BOTH modes. Design rule: "both versions on
        // the YAML should grant all Suits." They are never part of the item
        // pool and never stripped; Suit Sanity would be the only exception,
        // when it exists. GrantCosmetic writes [5] Vanity + [8] VanityEquipped
        // and guards each against stacking.
        if (strcmp(it.kind, "suit") == 0 || strcmp(it.kind, "suitstyle") == 0) {
            if (GrantCosmetic(it.hash, it.label)) ++granted;
            continue;
        }

        if (full) {
            // Never stack a grant. The guard has to look in the store that
            // CLAIMS the item, not just Equip: suits and styles live in
            // GameVanityInventory, so an equip-only check always read 0 and
            // re-seeding stacked them (a style reached general=2).
            int32_t have = 0;
            SafeStoreCount(equip, it.hash, &have);
            if (have <= 0) {
                void* claim = ClaimingStore(it.hash);
                if (claim && claim != equip) SafeStoreCount(claim, it.hash, &have);
            }
            if (have > 0) continue;
            // Anything with no equip slot has to be CRAFTED, not added: a plain
            // add leaves it at general=1/equip=0, which is the game's
            // "available to buy" state, not ownership. That is why a seeded
            // save showed every suit-tech tier unbought and max health never
            // moved, while the paired perks -- which go through CraftItem --
            // were right. Skills are exempt: their claiming store already IS
            // the equip store, so they land equipped on a plain add.
            // NOT suits or styles. They are claimed by GameVanityInventory, and
            // CraftItem moves an item into GameEquipInventory -- crafting them
            // pulls every suit out of the vanity store and the add lands in a
            // store that does not want it, leaving them in NEITHER
            // (general=0 equip=0, the whole wardrobe gone). A plain add to
            // their own claiming store is what always worked.
            const bool must_craft = (strcmp(it.kind, "suittech")   == 0 ||
                                     strcmp(it.kind, "doubleperk") == 0 ||
                                     strcmp(it.kind, "modslot")    == 0);
            if (must_craft) { if (CraftItem(it.hash, it.label)) ++granted; continue; }

            if (SafeStoreOp(store, kSlotAdd, it.hash, +1)) {
                ++granted;
                if (it.slot >= 0) { bool ok = false; if (SafeEquip(equip, it.hash, it.slot, &ok) && ok) ++equipped; }
            }
        } else {
            // Health 1 is the tutorial floor: removing the last HealthPerk
            // takes HealthPerkHelper with it.
            if (strcmp(it.label, "SuitTech Health 1") == 0) continue;

            void* from = store;
            int32_t ec = 0;
            if (SafeStoreCount(equip, it.hash, &ec) && ec > 0) from = equip;
            else if (!SafeStoreCount(store, it.hash, &ec) || ec <= 0) continue;

            // DriveToZero, never a raw remove with a negative amount: remove
            // negates internally, so passing -ec ADDS -- that is the bug that
            // turned currency into 115 million Tech Parts. DriveToZero measures,
            // corrects the sign itself, and confirms the count reached zero, so
            // `removed` counts verified removals rather than calls that returned.
            if (DriveToZero(from, it.hash, it.label, nullptr)) ++removed;
        }
    }
    if (full) ApplyAllDoublePerks(static_cast<uint32_t>(GetTickCount64()));
    RecomputeHealthNow();
    g_lockdown.store(true);   // both modes: the game must not re-supply what the AP controls
    SaveLockdownState();
    SAY("  %s: granted %d (equipped %d), removed %d, zeroed %d", full ? "seed full" : "seed empty", granted, equipped, removed, zeroed);
    SAY("  lockdown ON -- only Archipelago may grant; the game cannot pay you or re-sell what a check removed");
}

// Ensure every suit and style sits in BOTH vanity stores.
//
// MUST run on the game thread. The first version of this ran inline from the
// command parser -- 210 store writes off-thread -- and killed the game on the
// spot. Reads (query/stores) are safe there; writes are not.
//
// Needed because stripping makes the game re-grant suits itself (lockdown
// exempts suits on purpose, to avoid the invisible-hero bug) and that resets
// the wearable store.
void MakeCosmeticsWearable() {
    void* worn = StoreByIndex(kVanityEquippedStoreIndex);
    if (!worn) { SAY("  no VanityEquipped store -- load a save first"); return; }
    int ok = 0, fixed = 0, failed = 0;
    for (int i = 0; i < kApItemCount; ++i) {
        const ApItem& it = kApItems[i];
        if (strcmp(it.kind, "suit") != 0 && strcmp(it.kind, "suitstyle") != 0) continue;
        int32_t on = 0;
        SafeStoreCount(worn, it.hash, &on);
        if (on > 0) { ++ok; continue; }
        if (GrantCosmetic(it.hash, it.label)) ++fixed;
        else { ++failed; if (failed <= 6) SAY("   ! %s", it.label); }
    }
    SAY("  wearable: %d already, %d fixed, %d failed", ok, fixed, failed);
}

// Grant any suit the player does not have. Suits go to their claiming store
// (GameVanityInventory) plus VanityEquipped -- see GrantCosmetic.
void GrantMissingSuits() {
    int owned = 0, added = 0, refused = 0;
    for (int i = 0; i < kApItemCount; ++i) {
        const ApItem& it = kApItems[i];
        if (strcmp(it.kind, "suit") != 0) continue;
        void* st = ClaimingStore(it.hash);
        if (!st) { SAY("   %-32s no store claims it", it.label); ++refused; continue; }
        int32_t have = 0;
        if (SafeStoreCount(st, it.hash, &have) && have > 0) { ++owned; continue; }
        if (GrantCosmetic(it.hash, it.label)) ++added;
        else { ++refused; if (refused <= 6) SAY("   ! %s", it.label); }
    }
    SAY("  suits: %d already owned, %d granted, %d refused", owned, added, refused);
}

// give-to <store#>: add straight into one store, reported before -> after.
void GiveToStoreNow(int index, uint64_t hash, const char* label, int32_t n) {
    void* st = StoreByIndex(index);
    if (!st) { SAY("  store %d is empty", index); return; }
    char tn[128] = "?"; SafeTypeName(st, tn, sizeof(tn));
    int32_t before = 0; SafeStoreCount(st, hash, &before);
    const bool ok = SafeStoreOp(st, kSlotAdd, hash, n);
    int32_t after = before; SafeStoreCount(st, hash, &after);
    SAY("  %s into %s: %d -> %d %s", label, tn, before, after, ok ? "" : "(faulted)");
}

// hash <hex> <count>: act on any item, in the table or not. Negative removes.
void RawHashNow(uint64_t hash, int32_t n) {
    void* store = ClaimingStore(hash);
    if (!store) { SAY("  no store claims 0x%016llX", static_cast<unsigned long long>(hash)); return; }
    char tn[128] = "?"; SafeTypeName(store, tn, sizeof(tn));
    int32_t before = 0; SafeStoreCount(store, hash, &before);
    bool ok;
    if (n < 0) ok = DriveToZero(store, hash, "hash", nullptr);
    else       ok = SafeStoreOp(store, kSlotAdd, hash, n);
    int32_t after = before; SafeStoreCount(store, hash, &after);
    SAY("  0x%016llX in %s: %d -> %d %s", static_cast<unsigned long long>(hash), tn, before, after, ok ? "" : "(faulted)");
}

// craft, reported with both counts read back.
void CraftNow(uint64_t hash, const char* label) {
    const bool ok = CraftItem(hash, label);
    void* eq = StoreByIndex(kEquipStoreIndex);
    void* cl = ClaimingStore(hash);
    int32_t e = -1, c = -1;
    if (eq) SafeStoreCount(eq, hash, &e);
    if (cl) SafeStoreCount(cl, hash, &c);
    SAY("  %s: claiming=%d equip=%d %s", label, c, e, ok ? "OK" : "(failed)");
}

// Sweep: whatever the lockdown hook did not catch gets zeroed anyway.
// Design rule: "any time it is awarded it is reset to zero or it's zero awarded."
// Silent unless something was actually found, so it can run on a timer.
// Drive a count to zero and PROVE it. The remove slot negates its argument
// internally, so the sign that works is not obvious -- and getting it wrong is
// not a no-op, it *adds*. A sweep that announced success without checking
// doubled the player's tech parts every 20 seconds (225280 -> 450560 -> 901120 ...).
// So: try a sign, re-read, and if the count did not fall, undo and try the
// other. Never claim a result that was not measured.
bool DriveToZero(void* store, uint64_t hash, const char* label, int32_t* out_before) {
    int32_t before = 0;
    if (!SafeStoreCount(store, hash, &before)) return false;
    if (out_before) *out_before = before;
    if (before <= 0) return true;

    for (int attempt = 0; attempt < 2; ++attempt) {
        const int32_t amount = (attempt == 0) ? -before : before;
        int32_t now = before;
        if (!SafeStoreOp(store, kSlotRemove, hash, amount)) return false;
        if (!SafeStoreCount(store, hash, &now)) return false;
        if (now <= 0) return true;
        if (now < before) {                       // right sign, just needs repeating
            for (int k = 0; k < 64 && now > 0; ++k) {
                const int32_t step = now;
                if (!SafeStoreOp(store, kSlotRemove, hash, (attempt == 0) ? -step : step)) break;
                int32_t after = now;
                if (!SafeStoreCount(store, hash, &after) || after >= now) break;
                now = after;
            }
            return now <= 0;
        }
        // wrong sign: it grew. Put it back before trying the other way.
        WARN("%s: %+d grew the count %d -> %d, correcting", label, amount, before, now);
        SafeStoreOp(store, kSlotRemove, hash, (attempt == 0) ? (now - before) : -(now - before));
        SafeStoreCount(store, hash, &before);
    }
    return false;
}

void ZeroCurrencyNow(bool announce) {
    int swept = 0;
    for (int i = 0; i < kApItemCount; ++i) {
        const ApItem& it = kApItems[i];
        if (strcmp(it.kind, "currency") != 0) continue;
        if (strcmp(it.label, "XP") == 0) continue;      // XP is pinned at max, not zero
        void* store = ClaimingStore(it.hash);
        if (!store) continue;
        int32_t before = 0;
        const bool ok = DriveToZero(store, it.hash, it.label, &before);
        if (before <= 0) continue;
        int32_t after = -1; SafeStoreCount(store, it.hash, &after);
        if (ok && after <= 0) { SAY("0 %s: %d -> 0", it.label, before); ++swept; }
        else                  { WARN("%s: could not zero (%d -> %d)", it.label, before, after); }
    }
    if (announce && !swept) SAY("  all currency already at zero");
}

void ExecuteStoreRequest(const StoreRequest& r) {
    if (r.op == StoreOp::ZeroCurrency)     { ZeroCurrencyNow(r.slot >= 0); return; }
    if (r.op == StoreOp::SeedEmpty)        { DoSeed(false);          return; }
    if (r.op == StoreOp::SeedFull)         { DoSeed(true);           return; }
    if (r.op == StoreOp::Wearable)         { MakeCosmeticsWearable(); return; }
    if (r.op == StoreOp::Craft)            { CraftNow(r.hash, r.label);  return; }
    if (r.op == StoreOp::Uncraft)          { const bool ok = UncraftItem(r.hash, r.label);
                                             SAY("  %s returned to the menu %s", r.label, ok ? "" : "(failed)"); return; }
    if (r.op == StoreOp::GiveTo)           { GiveToStoreNow(r.slot, r.hash, r.label, r.amount); return; }
    if (r.op == StoreOp::RawHash)          { RawHashNow(r.hash, r.amount); return; }
    if (r.op == StoreOp::GrantSuits)       { GrantMissingSuits(); return; }
    if (r.op == StoreOp::DoublePerks)      { ApplyAllDoublePerks(static_cast<uint32_t>(GetTickCount64())); return; }
    if (r.op == StoreOp::ResetSuitTech)    { DoSuitTechColumn(true);  return; }
    if (r.op == StoreOp::GrantAllSuitTech) { DoSuitTechColumn(false); return; }
    if (r.op == StoreOp::RecomputeHealth)  { RecomputeHealthNow();    return; }
    if (r.op == StoreOp::GateTeleport)     { gate::TeleportNow();      return; }
    if (r.op == StoreOp::GateDrain)        { gate::DrainNow();         return; }
    if (r.op == StoreOp::GateStrip)        { gate::StripNow();         return; }
    if (r.op == StoreOp::GateRestore)      { gate::RestoreNow();       return; }

    void* store = ClaimingStore(r.hash);
    if (!store) { SAY("  %s: no store claims it", r.label); return; }
    void* equip = StoreByIndex(kEquipStoreIndex);
    const bool perk = (r.slot == kNoEquip);

    if (r.op == StoreOp::Zero) {
        // Remove the whole count in one call, from whichever store holds it
        // (Equip first: applied perks / gadgets live there; currency sits in
        // the claiming store). Falls back to -1 steps if the count refuses
        // to drop in one go.
        void* targets[2] = { equip, store }; const char* names[2] = { "GameEquipInventory", "claimed store" };
        bool did = false;
        for (int t = 0; t < 2; ++t) {
            void* s = targets[t]; if (!s) continue;
            int32_t count = 0;
            if (!SafeStoreCount(s, r.hash, &count) || count <= 0) continue;
            const int32_t before = count;
            DriveToZero(s, r.hash, r.label, nullptr);
            SafeStoreCount(s, r.hash, &count);
            for (int k = 0; k < 200 && count > 0; ++k) {
                if (!SafeStoreOp(s, kSlotRemove, r.hash, -1)) break;
                int32_t after = count; SafeStoreCount(s, r.hash, &after);
                if (after >= count) break;
                count = after;
            }
            SAY("- zeroed %s  (%s: %d -> %d)", r.label, names[t], before, count);
            did = true;
        }
        if (!did) SAY("  %s: nothing held", r.label);
        if (perk) RecomputeHealthNow();
        StoreFor(r.hash, r.label);
        return;
    }

    if (r.op == StoreOp::Remove) {
        // Perks live and are counted in the Equip store; the claimed store
        // only holds their catalogue record.
        const char* which = "claimed store";
        if (perk && equip) { int32_t ec = 0; if (SafeStoreCount(equip, r.hash, &ec) && ec > 0) { store = equip; which = "GameEquipInventory"; } }
        if (!SafeStoreOp(store, kSlotRemove, r.hash, -1)) { WARN("remove %s FAULTED 0x%08X @ exe+0x%llX", r.label, FAULT_DETAIL); return; }
        SAY("- removed %s  (%s)", r.label, which);
        if (perk) RecomputeHealthNow();
        StoreFor(r.hash, r.label);
        return;
    }

    if (r.op == StoreOp::Add || r.op == StoreOp::Restore) {
        const int32_t n = (r.amount > 0) ? r.amount : 1;
        if (!SafeStoreOp(store, kSlotAdd, r.hash, n)) { WARN("add %s FAULTED 0x%08X @ exe+0x%llX", r.label, FAULT_DETAIL); return; }
        SAY("+ added %d x %s", n, r.label);
        if (perk) { RecomputeHealthNow(); StoreFor(r.hash, r.label); return; }
        if (r.op == StoreOp::Restore && equip) DoEquip(equip, r.hash, r.label, r.slot);
        StoreFor(r.hash, r.label);
        return;
    }

    if (r.op == StoreOp::Equip) {
        if (perk) { SAY("  %s is not a slotted ability", r.label); return; }
        if (!equip) { WARN("no equip store"); return; }
        DoEquip(equip, r.hash, r.label, r.slot);
        StoreFor(r.hash, r.label);
        return;
    }
}

// Called from the AddComponent and GetActor hooks. Runs at most one request
// per call, and only on the game thread. Must not be called under g_lock.
void DrainStoreRequests() {
    if (!IsGameThread()) return;
    StoreRequest r{};
    if (PopStoreRequest(&r)) ExecuteStoreRequest(r);
}

// From the polling thread: never runs engine code; only says when a request
// has been waiting, which means the game thread is not reaching our hooks.
void DrainStaleStoreRequests(uint64_t min_age_ms) {
    static uint64_t last_warn = 0;
    std::lock_guard<std::mutex> g(g_req_lock);
    if (g_requests.empty()) return;
    const uint64_t now = GetTickCount64();
    if (now - g_requests.front().queued_ms < min_age_ms || now - last_warn < 2000) return;
    last_warn = now;
    WARN("'%s' has waited %llu ms for the game thread -- move or look around",
         g_requests.front().label, static_cast<unsigned long long>(now - g_requests.front().queued_ms));
}

// --- reading the store's own vtable -------------------------------------------------
//
// Signature scanning put the remove hook on the wrong function: the game
// deducted 735 Tech Parts for a craft and the hook never fired once, while my
// own grants through slot 0x80 traced fine. So stop guessing at addresses --
// read the function pointers out of the live object. Whatever slot 0x98
// actually points at IS remove, by definition.

bool SafeVtSlot(void* obj, size_t slot, uintptr_t* out) {
    __try {
        void** vt = *reinterpret_cast<void***>(obj);
        void* fn = vt[slot / 8];
        if (!fn) return false;
        *out = reinterpret_cast<uintptr_t>(fn);
        return true;
    } __except (RecordFault(GetExceptionInformation())) { return false; }
}

// Print one address exe-relative, flagging anything that is not in the image.
void SayFnSlot(void* obj, size_t slot, const char* name) {
    uintptr_t fn = 0;
    if (!SafeVtSlot(obj, slot, &fn)) { SAY("    +0x%03X %-13s unreadable", (unsigned)slot, name); return; }
    const bool in_exe = g_exe_base && fn >= g_exe_base && fn < g_exe_base + g_exe_size;
    SAY("    +0x%03X %-13s %s0x%llX", (unsigned)slot, name, in_exe ? "exe+" : "abs ",
        static_cast<unsigned long long>(in_exe ? fn - g_exe_base : fn));
}

// Defined in dllmain.cpp, next to the hooks it installs.
void RehookStoreFromVtable(uint64_t hash, const char* label);
void DumpDistrictVolumes();               // dllmain.cpp

void DumpStoreVtable(uint64_t hash, const char* label) {
    void* store = ClaimingStore(hash);
    if (!store) { SAY("  %s: no store claims it", label); return; }
    char tn[128] = "?"; SafeTypeName(store, tn, sizeof(tn));
    SAY("  %s -> %s", label, tn);
    SayFnSlot(store, kSlotFindOwned,    "findOwned");
    SayFnSlot(store, kSlotAdd,          "add");
    SayFnSlot(store, kSlotRemove,       "remove");
    SayFnSlot(store, kSlotFindEquipped, "findEquipped");
    SayFnSlot(store, kSlotGetCount,     "getCount");

    void* eq = StoreByIndex(kEquipStoreIndex);
    if (eq && eq != store) {
        char en[128] = "?"; SafeTypeName(eq, en, sizeof(en));
        SAY("  equip store -> %s", en);
        SayFnSlot(eq, kSlotAdd,    "add");
        SayFnSlot(eq, kSlotRemove, "remove");
    }
}

// --- raw record dumps -----------------------------------------------------------
//
// The purchase path does not call the store's add or remove -- the vtable slots
// were read out of a live object and they are exactly the two functions already
// hooked, yet a craft that cost 735 Tech Parts fired neither. So the game writes
// the record directly, and the "owned" state we keep failing to reproduce is a
// field in that record rather than a count.
//
// Dumping the same item before and after one real craft says which bytes a
// purchase writes. Nothing here mutates anything.

constexpr size_t kDumpBytes = 0x200;

void HexDumpRecord(const char* what, void* rec, size_t n) {
    if (!rec) { SAY("  %-5s (none)", what); return; }
    if (n > kDumpBytes) n = kDumpBytes;
    unsigned char buf[kDumpBytes];
    if (!SafeRead(rec, buf, n)) { SAY("  %-5s unreadable at %p", what, rec); return; }
    SAY("  %-5s %p  %u bytes -> log", what, rec, static_cast<unsigned>(n));
    for (size_t off = 0; off < n; off += 16) {
        char line[128];
        int p = sprintf_s(line, sizeof(line), "%03X ", static_cast<unsigned>(off));
        for (int i = 0; i < 16; ++i)
            p += sprintf_s(line + p, sizeof(line) - p, "%02X ", buf[off + i]);
        INFO("DUMP %s %s", what, line);
    }
}

void DumpItemRecords(uint64_t hash, const char* label, size_t n) {
    SAY("dump %s (0x%016llX)", label, static_cast<unsigned long long>(hash));

    if (void* store = ClaimingStore(hash)) {
        char tn[128] = "?"; SafeTypeName(store, tn, sizeof(tn));
        void* owned = nullptr; SafeStoreFind(store, kSlotFindOwned, hash, &owned);
        int32_t c = -1; SafeStoreCount(store, hash, &c);
        SAY("  claiming store %s  count=%d", tn, c);
        INFO("DUMP --- %s claim store=%p count=%d", label, store, c);
        HexDumpRecord("claim", owned, n);
    } else {
        SAY("  no store claims it");
    }

    void* eq = StoreByIndex(kEquipStoreIndex);
    if (eq) {
        void* rec = nullptr; SafeStoreFind(eq, kSlotFindEquipped, hash, &rec);
        int32_t c = -1; SafeStoreCount(eq, hash, &c);
        SAY("  equip store count=%d", c);
        INFO("DUMP --- %s equip store=%p count=%d", label, eq, c);
        HexDumpRecord("equip", rec, n);
    }
}

// --- the item's prius --------------------------------------------------------------
//
// getCount (exe+0x34C9990) gave up the layout. It does not read a scalar; it
// walks the records matching the hash and accumulates:
//
//     quantity += record->vt[0x120]()->[+0x38]
//
// So every item record carries an object reached through vtable slot +0x120 --
// its prius -- and the quantity is a field inside it. The save tells us the
// type of that object is PerkItemPrius for a plain item and
// StatChoiceGroupItemPrius for a suit-tech pair, and that a real purchase
// creates one. This reads it back so a purchased item and an injected one can
// be compared directly.

constexpr size_t kSlotGetPrius = 0x120;
constexpr size_t kPriusQuantity = 0x38;

bool SafePrius(void* record, void** out) {
    __try {
        void** vt = *reinterpret_cast<void***>(record);
        const uintptr_t v = reinterpret_cast<uintptr_t>(vt);
        if (!g_exe_base || v < g_exe_base || v >= g_exe_base + g_exe_size) return false;
        *out = reinterpret_cast<void* (*)(void*)>(vt[kSlotGetPrius / 8])(record);
        return true;
    } __except (RecordFault(GetExceptionInformation())) { return false; }
}

// A prius names its own type through vtable slot +0x40, which returns the DDL
// type descriptor; the descriptor's first field is the name pointer. (The
// generic SafeTypeName uses +0x48 and comes back empty for these.)
bool SafePriusTypeName(void* pr, char* out, size_t n) {
    __try {
        void** vt = *reinterpret_cast<void***>(pr);
        const uintptr_t v = reinterpret_cast<uintptr_t>(vt);
        if (!g_exe_base || v < g_exe_base || v >= g_exe_base + g_exe_size) return false;
        void* desc = reinterpret_cast<void* (*)(void*)>(vt[0x40 / 8])(pr);
        if (!desc) return false;
        const char* nm = *reinterpret_cast<const char**>(desc);
        if (!nm) return false;
        strncpy_s(out, n, nm, _TRUNCATE);
        return true;
    } __except (RecordFault(GetExceptionInformation())) { return false; }
}

void DumpPriusOne(void* store, const char* which, size_t find_slot, uint64_t hash, size_t nbytes) {
    if (!store) { SAY("  %-12s no store", which); return; }
    void* rec = nullptr;
    if (!SafeStoreFind(store, find_slot, hash, &rec) || !rec) { SAY("  %-12s no record", which); return; }

    char rn[128] = "?"; SafeTypeName(rec, rn, sizeof(rn));
    void* pr = nullptr;
    if (!SafePrius(rec, &pr) || !pr) {
        SAY("  %-12s record=%p %-18s prius: NONE", which, rec, rn);
        return;
    }
    char pn[128] = "?";
    if (!SafePriusTypeName(pr, pn, sizeof(pn))) SafeTypeName(pr, pn, sizeof(pn));
    int32_t qty = -1;
    SafeRead(reinterpret_cast<char*>(pr) + kPriusQuantity, &qty, 4);
    SAY("  %-12s record=%p %-18s prius=%p %-26s qty=%d", which, rec, rn, pr, pn, qty);
    INFO("PRIUS %s rec=%p rec_type=%s prius=%p type=%s qty=%d", which, rec, rn, pr, pn, qty);
    if (nbytes) HexDumpRecord(which, pr, nbytes);
}

void DumpPrius(uint64_t hash, const char* label, size_t nbytes) {
    SAY("prius %s (0x%016llX)", label, static_cast<unsigned long long>(hash));
    void* claim = ClaimingStore(hash);
    // getCount sums the table at store+0x2140 of the CLAIMING store, so the
    // quantity record is found there with slot 0x198 -- not in the equip store.
    DumpPriusOne(claim, "claim/owned", kSlotFindOwned,    hash, nbytes);
    DumpPriusOne(claim, "claim/table", kSlotFindEquipped, hash, nbytes);
    void* eq = StoreByIndex(kEquipStoreIndex);
    if (eq && eq != claim) DumpPriusOne(eq, "equip/table", kSlotFindEquipped, hash, nbytes);
}
