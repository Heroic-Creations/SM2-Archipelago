// SM2-Archipelago -- the Marvel's Spider-Man 2 Archipelago mod.
//
// State on 2026-09-04: a live command channel over the proven item
// primitives. the client appends a line to ap-commands.txt in the runtime folder,
// the mod runs it on the game thread and prints the result on its console.
// Location detection and the Archipelago client itself are not built yet.
//
//   engine.inc     exe-relative addresses, guarded reads, hero/component helpers
//   inventory.inc  the store primitives, health recompute, the request queue
//   items.inc      every known item (generated): abilities, suit tech, skills
//   ap.inc         the command file: parsing, item lookup, dispatch
//
// Rules this build was shaped by, the hard way:
//   * engine store calls only on the game thread (gated by the engine's own
//     is-main-thread check; drained from the AddComponent and GetActor hooks)
//   * never bind F12 (Steam screenshot); the poll thread only reads a file
//   * an RVA comes from the raw call bytes, never a disassembler's printout

#include "game/Native.h"
#include "game/Actor.h"
#include "game/Component/Component.h"
#include "game/HeroSystem.h"

#include "logging.h"
#include "overlay.h"
#include "runtime.h"
#include "include/MinHook.h"

#include <Windows.h>
#include <psapi.h>
#include <intrin.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <unordered_set>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

#include "engine.inc"
#include "items.inc"       // before inventory.inc: seeding walks the item table
#include "inventory.inc"
#include "doubleperks.inc"
#include "gate.inc"

// Defined below, but the command parser in ap.inc calls it.
void DumpCollectibles();

#include "ap.inc"

// --- hooks: where queued requests get to run on the game thread ------------------
//
// AddComponent fires while things spawn; GetActor fires on every handle
// resolve, so a request is picked up within a frame even when the player is
// standing still. Both drain at most one request per call, outside any lock
// of ours, with a re-entrancy guard because engine code inside a request can
// itself add components or resolve handles.

using AddComponentFn = Component* (*)(Actor*, ComponentInfo*, void*);
using GetActorFn     = Actor* (*)(ActorHandle*);
AddComponentFn g_original_add_component = nullptr;
GetActorFn     g_original_get_actor     = nullptr;

Component* Hooked_AddComponent(Actor* actor, ComponentInfo* info, void* prius) {
    Component* result = g_original_add_component(actor, info, prius);
    thread_local bool draining = false;
    if (!draining) { draining = true; DrainStoreRequests(); draining = false; }
    return result;
}

// Every actor the game resolves passes through here. Collecting the district
// volumes as they go by is the only way to reach them: they are not on the
// hero, the DistrictTrackerComponent holds no actor handles, and there is no
// district grouping in the activity config -- the game does containment
// against these volumes at runtime, so their shapes are what we need.
std::mutex              g_vol_lock;
std::vector<void*>      g_district_vols;

void NoteDistrictVolume(Actor* a) {
    if (!a) return;
    char name[160] = "";
    if (!SafeReadName(a, name, sizeof(name))) return;
    if (_strnicmp(name, "Vol_Dist", 8) != 0) return;
    std::lock_guard<std::mutex> g(g_vol_lock);
    for (void* v : g_district_vols) if (v == a) return;
    if (g_district_vols.size() < 64) g_district_vols.push_back(a);
}

// Log any actor that looks like a CHECK, with its own world position.
//
// This is the piece the manual method cannot give: standing at a crystal and
// reading the hero position says "a memory in Central Park", but not WHICH
// memory -- and the internal id is what the save reports on completion. The
// actor carries both, so one fly-around captures name and coordinates together.
//
// Positions come from the ACTOR, never the hero: using the player's position
// is what made all 44 spider-bots share one coordinate in an earlier pass.
const char* const kCheckPrefixes[] = {
    "collectible_", "OW_SAND", "SAND_MEMORY", "SYMBIOTE_NEST", "symbiotenest",
    "BLIND_HUNTER", "Blind_hunters", "MYSTERIO_CHALLENGE", "OW_PROWLERTECH",
    "MemoryCam_", "Prompt_EMF", "OW_EMF", "FNSM_", "BVA_", "OW_PHOTO",
    "OW_EMILYMAY", "FALCON", "hunter_cloaking",
};

std::mutex                     g_check_lock;
std::unordered_set<std::string> g_checks_seen;

void NoteCheckActor(Actor* a) {
    if (!a) return;
    char name[160] = "";
    if (!SafeReadName(a, name, sizeof(name))) return;

    bool interesting = false;
    for (const char* pre : kCheckPrefixes)
        if (_strnicmp(name, pre, strlen(pre)) == 0) { interesting = true; break; }
    if (!interesting) return;

    {
        std::lock_guard<std::mutex> g(g_check_lock);
        if (!g_checks_seen.insert(name).second) return;   // already logged
    }

    Vector3 p{};
    if (!SafeActorPos(a, &p)) return;
    if (p.x == 0.0f && p.z == 0.0f) return;               // template actors sit at the origin

    if (FILE* f = nullptr; fopen_s(&f, RuntimeFile("check-actors.jsonl").c_str(), "a") == 0 && f) {
        fprintf(f, "{\"actor\":\"%s\",\"pos\":[%.2f,%.2f,%.2f]}\n", name, p.x, p.y, p.z);
        fclose(f);
    }
    INFO("CHECK-ACTOR %s [%.1f %.1f %.1f]", name, p.x, p.y, p.z);
}

Actor* Hooked_GetActor(ActorHandle* handle) {
    thread_local bool draining = false;
    if (!draining) { draining = true; DrainStoreRequests(); draining = false; }
    Actor* a = g_original_get_actor(handle);
    if (g_collect_vols.load(std::memory_order_relaxed)) { NoteDistrictVolume(a); NoteCheckActor(a); }
    return a;
}

// --- location detection, first step: watch objective state changes ------------
//
// Log-only. Every objective / mission state write comes through here with the
// node's 64-bit key (mission id high, objective id low) and the new value.
// The ids are matched against the 1,411 save-schema names offline
// (the client's match-objectives.py) until the hashing is pinned down.

using ObjectiveSetStateFn = void (*)(void* sys, uint64_t key, uint32_t val);
ObjectiveSetStateFn g_original_objective_set_state = nullptr;

// The setter is called far more often than states change (the first build
// printed every write and flooded the console). Only a value that differs
// from the last one seen for that key is reported, and console lines are
// capped per second; the file gets every change.
std::mutex g_obj_lock;
std::unordered_map<uint64_t, uint32_t> g_obj_last;

void Hooked_ObjectiveSetState(void* sys, uint64_t key, uint32_t val) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> g(g_obj_lock);
        auto it = g_obj_last.find(key);
        changed = (it == g_obj_last.end() || it->second != val);
        g_obj_last[key] = val;
    }
    // Never on the console: the value for an active objective is a rotating
    // number (timer / distance) that changes every frame, so "only on change"
    // still floods. The file keeps everything for offline work; the reward
    // hooks are the ones that fire once per actual completion.
    if (changed && g_objwatch.load(std::memory_order_relaxed)) {
        const uint32_t mission = static_cast<uint32_t>(key >> 32), objective = static_cast<uint32_t>(key);
        DEBUG("OBJECTIVE sys=%p key=%016llX val=%u", sys, static_cast<unsigned long long>(key), val);
        if (FILE* f = nullptr; fopen_s(&f, RuntimeFile("objectives.log").c_str(), "a") == 0 && f) {
            SYSTEMTIME t; GetLocalTime(&t);
            fprintf(f, "%02d:%02d:%02d %08X %08X %u\n", t.wHour, t.wMinute, t.wSecond, mission, objective, val);
            fclose(f);
        }
    }
    g_original_objective_set_state(sys, key, val);
}

// --- location detection, second step: the reward system's per-event virtuals -
//
// ProgressionRewardSystem calls ProgRewardObjective::On (vt+0x38) once per
// completed objective and ProgRewardCollectible::On (vt+0x40) once per
// collectible, both through the vtable. Log-only: the reward entry's first
// qwords (its config key is the objective's crc64 name) and the context's go
// to rewards.log for offline matching against crc64(save-schema name).

using RewardOnFn = uint64_t (*)(void* self, void* ctx);
RewardOnFn g_original_reward_objective   = nullptr;
RewardOnFn g_original_reward_collectible = nullptr;

void LogRewardEvent(const char* kind, void* self, void* ctx) {
    uint64_t s[16] = {}, c[8] = {};
    SafeRead(self, s, sizeof(s));
    if (ctx) SafeRead(ctx, c, sizeof(c));
    SAY("* reward event: %s  entry=%p ctx=%p  (details in rewards.log)", kind, self, ctx);
    if (FILE* f = nullptr; fopen_s(&f, RuntimeFile("rewards.log").c_str(), "a") == 0 && f) {
        SYSTEMTIME t; GetLocalTime(&t);
        fprintf(f, "%02d:%02d:%02d %s self=%p", t.wHour, t.wMinute, t.wSecond, kind, self);
        for (uint64_t v : s) fprintf(f, " %016llX", static_cast<unsigned long long>(v));
        fprintf(f, " | ctx=%p", ctx);
        for (uint64_t v : c) fprintf(f, " %016llX", static_cast<unsigned long long>(v));
        fprintf(f, "\n");
        fclose(f);
    }
}

// The collectible table, learned from the game rather than guessed.
//
// ProgRewardCollectible::On fires once per collectible with a stable 32-bit id
// in the ctx slot, and entry+0x08 points at the collectible's own object. That
// is everything needed to watch them: a name we can trust and a place to look.
struct CollectibleEntry {
    uint32_t id;        // the ctx value -- stable across runs
    void*    entry;     // the reward entry
    void*    object;    // entry+0x08, the collectible itself
    uint32_t state;     // last value read from the object
};
constexpr int kMaxCollectibles = 256;
CollectibleEntry  g_collectibles[kMaxCollectibles]{};
std::atomic<int>  g_collectible_count{0};
std::mutex        g_collectible_lock;

void RecordCollectible(void* self, void* ctx) {
    const uint32_t id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ctx));
    if (!id) return;
    std::lock_guard<std::mutex> g(g_collectible_lock);
    const int n = g_collectible_count.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i)
        if (g_collectibles[i].id == id) { g_collectibles[i].entry = self; return; }
    if (n >= kMaxCollectibles) return;
    void* obj = nullptr;
    SafeRead(static_cast<char*>(self) + 8, &obj, sizeof(obj));
    g_collectibles[n] = { id, self, obj, 0 };
    g_collectible_count.store(n + 1, std::memory_order_release);
}

uint64_t Hooked_RewardObjective(void* self, void* ctx)   { LogRewardEvent("objective",   self, ctx); return g_original_reward_objective(self, ctx); }
uint64_t Hooked_RewardCollectible(void* self, void* ctx) {
    RecordCollectible(self, ctx);
    LogRewardEvent("collectible", self, ctx);
    return g_original_reward_collectible(self, ctx);
}

// --- lockdown: refuse every grant that isn't ours ------------------------------
//
// All inventory stores share this add implementation, so one hook covers
// abilities, suit tech, skills, gadgets, currency and XP. Ours pass because
// SafeStoreOp sets g_ap_granting around the call.

using StoreAddImplFn = void* (*)(void* store, void* out, uint64_t hash, int32_t amount);
StoreAddImplFn g_original_store_add = nullptr;

// Refuse ONLY what the AP manages. The first version refused every grant, and
// that included the loadout pieces the game hands itself to assemble the hero:
// suit models, masks, damage variants. The result was an invisible Spider-Man
// that followed the player across saves, because it was never in the save at all --
// it was the running process being denied its own body.
//
// So: an item we do not know about is the game's business, and passes.
// Who calls the store? Rather than hunt the purchase routine by reading
// disassembly, ask the game: with tracing on, every add records its caller
// (exe-relative). Craft one perk in the menu and the caller IS the purchase
// path -- no guessing, and it works the same for any category later.
// The return address is passed in, never taken here: this function stopped
// being inlined into the detours when it grew, and every caller was then
// reported as our own DLL -- which made the game's save-time adds look like
// ours for an hour.
// Name the module a return address belongs to. Kept separate so both the
// trace and the probe report callers the same way.
void DescribeCaller(uintptr_t ret, char* out, size_t n) {
    if (g_exe_base && ret >= g_exe_base && ret < g_exe_base + g_exe_size) {
        sprintf_s(out, n, "exe+0x%llX", static_cast<unsigned long long>(ret - g_exe_base));
        return;
    }
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(ret), &mod) && mod) {
        char full[MAX_PATH] = "?";
        GetModuleFileNameA(mod, full, MAX_PATH);
        const char* base = strrchr(full, '\\');
        sprintf_s(out, n, "%s+0x%llX", base ? base + 1 : full,
                  static_cast<unsigned long long>(ret - reinterpret_cast<uintptr_t>(mod)));
    } else {
        sprintf_s(out, n, "0x%llX (no module)", static_cast<unsigned long long>(ret));
    }
}

void TraceCallerAt(uintptr_t ret, const char* what, uint64_t hash, int32_t amount) {
    if (!g_trace.load(std::memory_order_relaxed)) return;
    const char* label = "unknown";
    for (int i = 0; i < kApItemCount; ++i) if (kApItems[i].hash == hash) { label = kApItems[i].label; break; }

    // "caller exe+0x0" hid the answer once: everything outside the image
    // collapsed to zero, so our own calls and any other mod's looked alike.
    // Name the module instead.
    char where[192];
    DescribeCaller(ret, where, sizeof(where));
    SAY("T %s %+d %-28s caller %s", what, amount, label, where);
    INFO("TRACE %s %+d 0x%016llX caller %s", what, amount, static_cast<unsigned long long>(hash), where);
}

using StoreRemoveImplFn = void* (*)(void* store, void* out, uint64_t hash, int32_t amount);
StoreRemoveImplFn g_original_store_remove = nullptr;

void* Hooked_StoreRemove(void* store, void* out, uint64_t hash, int32_t amount) {
    TraceCallerAt(reinterpret_cast<uintptr_t>(_ReturnAddress()), "remove", hash, amount);
    return g_original_store_remove(store, out, hash, amount);
}

void* Hooked_StoreAdd(void* store, void* out, uint64_t hash, int32_t amount) {
    TraceCallerAt(reinterpret_cast<uintptr_t>(_ReturnAddress()), "add", hash, amount);
    if (g_lockdown.load(std::memory_order_relaxed) && !g_ap_granting) {
        const char* label = nullptr;
        for (int i = 0; i < kApItemCount; ++i)
            if (kApItems[i].hash == hash) { label = kApItems[i].label; break; }
        // Suits are cosmetic and the game manages their loadouts; never block them.
        const bool ours = label && !HashIsSuit(hash);
        if (ours) {
            const int n = ++g_refused;
            if (n <= 20 || n % 50 == 0) SAY("x refused game grant: %+d %s", amount, label);
            INFO("LOCKDOWN refused %+d 0x%016llX (%s)", amount, static_cast<unsigned long long>(hash), label);
            return out;   // same shape as a no-op add; the caller releases this
        }
    }
    return g_original_store_add(store, out, hash, amount);
}

// --- position + district trail --------------------------------------------------
//
// Detection runs in Python off the save, which says *what* was completed but
// not where. This writes a timestamped position/district trail so a completion
// can be matched to the place it happened -- and the check -> district table
// builds itself as the player plays.

void WriteTrail() {
    Actor* hero = HeroActor();
    if (!hero) return;
    Vector3 p{};
    if (!SafeActorPos(hero, &p)) return;
    if (p.x < -1e5f || p.x > 1e5f) return;

    static float lx = 1e9f, ly = 1e9f, lz = 1e9f;
    const float d = (p.x-lx)*(p.x-lx) + (p.y-ly)*(p.y-ly) + (p.z-lz)*(p.z-lz);
    if (d < 25.0f) return;                    // only log real movement (>5 units)
    lx = p.x; ly = p.y; lz = p.z;

    if (FILE* f = nullptr; fopen_s(&f, RuntimeFile("trail.log").c_str(), "a") == 0 && f) {
        SYSTEMTIME t; GetLocalTime(&t);
        fprintf(f, "%02d:%02d:%02d %.1f %.1f %.1f\n", t.wHour, t.wMinute, t.wSecond, p.x, p.y, p.z);
        fclose(f);
    }
}

bool InstallHook(void* target, void* detour, void** original, const char* name) {
    if (!target) { WARN("%s did not resolve", name); return false; }
    if (MH_CreateHook(target, detour, original) != MH_OK || MH_EnableHook(target) != MH_OK) {
        WARN("failed to hook %s", name);
        return false;
    }
    INFO("hooked %s", name);
    return true;
}

// --- the purchase-path probe -------------------------------------------------------
//
// The StatChoiceGroupItemPrius record is what a real purchase writes and our
// injections never create. Statically it is a dead end: every code reference to
// the type is reflection plumbing with no direct callers, and the two functions
// that name its descriptor (exe+0x1BC20B0, and exe+0x1BC1BC0 for PerkItemPrius)
// tail-call the generic serializer at exe+0x30AA0F0 -- 7,249 callers, reading
// fields off a stream. Serialization, not gameplay.
//
// So let the game name the caller instead. Choosing a side of a suit-tech pair
// is FREE, so it can be triggered on demand. Whichever of these fires, its
// return address is the code that performs the choice.
constexpr uintptr_t kRva_StatChoicePrius = 0x1BC20B0;   // StatChoiceGroupItemPrius serialize
constexpr uintptr_t kRva_PerkItemPrius   = 0x1BC1BC0;   // PerkItemPrius serialize, for contrast

using PriusFn = void* (*)(void* self, void* stream);
PriusFn g_original_statchoice_prius = nullptr;
PriusFn g_original_perkitem_prius   = nullptr;

using EquipProbeFn = bool (*)(void* equip_store, uint64_t hash, int32_t slot, uint8_t flag);
EquipProbeFn g_original_equip = nullptr;

void ProbeHit(uintptr_t ret, const char* what, const char* detail) {
    if (!g_probe.load(std::memory_order_relaxed)) return;
    char where[192];
    DescribeCaller(ret, where, sizeof(where));
    SAY("P %-22s %-34s caller %s", what, detail, where);
    INFO("PROBE %s %s caller %s", what, detail, where);
}

void* Hooked_StatChoicePrius(void* self, void* stream) {
    char d[64]; sprintf_s(d, "self=%p stream=%p", self, stream);
    ProbeHit(reinterpret_cast<uintptr_t>(_ReturnAddress()), "StatChoicePrius", d);
    return g_original_statchoice_prius(self, stream);
}

void* Hooked_PerkItemPrius(void* self, void* stream) {
    char d[64]; sprintf_s(d, "self=%p stream=%p", self, stream);
    ProbeHit(reinterpret_cast<uintptr_t>(_ReturnAddress()), "PerkItemPrius", d);
    return g_original_perkitem_prius(self, stream);
}

bool Hooked_Equip(void* equip_store, uint64_t hash, int32_t slot, uint8_t flag) {
    const char* label = "unknown";
    for (int i = 0; i < kApItemCount; ++i)
        if (kApItems[i].hash == hash) { label = kApItems[i].label; break; }
    char d[160];
    sprintf_s(d, "%s slot=%d flag=%u", label, slot, static_cast<unsigned>(flag));
    ProbeHit(reinterpret_cast<uintptr_t>(_ReturnAddress()), "Equip", d);
    return g_original_equip(equip_store, hash, slot, flag);
}

// --- hooking what the vtable actually names ---------------------------------------
//
// The scanned "store remove" address never fired once while the game deducted
// 735 Tech Parts for a craft, and the craft's own add never appeared either.
// A signature can match the wrong function; slot 0x98 of a live store object
// cannot. So read the pointers out of the object and hook those.

void* g_vt_add_target = nullptr;
void* g_vt_rem_target = nullptr;
StoreAddImplFn    g_original_vt_add = nullptr;
StoreRemoveImplFn g_original_vt_rem = nullptr;

void* Hooked_VtAdd(void* store, void* out, uint64_t hash, int32_t amount) {
    TraceCallerAt(reinterpret_cast<uintptr_t>(_ReturnAddress()), "add", hash, amount);
    return g_original_vt_add(store, out, hash, amount);
}

void* Hooked_VtRemove(void* store, void* out, uint64_t hash, int32_t amount) {
    TraceCallerAt(reinterpret_cast<uintptr_t>(_ReturnAddress()), "remove", hash, amount);
    return g_original_vt_rem(store, out, hash, amount);
}

// Report one slot and hook it if it is not already covered.
void HookSlot(void* store, size_t slot, const char* what, void* detour,
              void** original, void** target_slot, uintptr_t scanned_rva) {
    uintptr_t fn = 0;
    if (!SafeVtSlot(store, slot, &fn)) { SAY("  +0x%03X %s: unreadable", (unsigned)slot, what); return; }
    const bool in_exe = g_exe_base && fn >= g_exe_base && fn < g_exe_base + g_exe_size;
    const unsigned long long rva = static_cast<unsigned long long>(in_exe ? fn - g_exe_base : fn);

    if (in_exe && rva == scanned_rva) {
        SAY("  +0x%03X %-7s exe+0x%llX  (matches the scan -- already hooked)", (unsigned)slot, what, rva);
        return;
    }
    SAY("  +0x%03X %-7s %s0x%llX  <-- the scan said 0x%llX", (unsigned)slot, what,
        in_exe ? "exe+" : "abs ", rva, static_cast<unsigned long long>(scanned_rva));

    if (*target_slot == reinterpret_cast<void*>(fn)) { SAY("      already hooked here"); return; }
    if (*target_slot) { MH_DisableHook(*target_slot); MH_RemoveHook(*target_slot); *target_slot = nullptr; }
    if (InstallHook(reinterpret_cast<void*>(fn), detour, original, what)) {
        *target_slot = reinterpret_cast<void*>(fn);
        SAY("      hooked");
    }
}

// Read one live store's add/remove and put the trace hooks on them.
void RehookStoreFromVtable(uint64_t hash, const char* label) {
    void* store = ClaimingStore(hash);
    if (!store) { SAY("  %s: no store claims it", label); return; }
    char tn[128] = "?"; SafeTypeName(store, tn, sizeof(tn));
    SAY("claiming store for %s -> %s", label, tn);
    HookSlot(store, kSlotAdd,    "add",    reinterpret_cast<void*>(&Hooked_VtAdd),
             reinterpret_cast<void**>(&g_original_vt_add), &g_vt_add_target, kRva_StoreAddImpl);
    HookSlot(store, kSlotRemove, "remove", reinterpret_cast<void*>(&Hooked_VtRemove),
             reinterpret_cast<void**>(&g_original_vt_rem), &g_vt_rem_target, kRva_StoreRemoveImpl);

    // Crafted items land in the Equip store; if it uses a different
    // implementation the trace would miss the half that matters.
    void* eq = StoreByIndex(kEquipStoreIndex);
    if (!eq || eq == store) return;
    uintptr_t ea = 0, er = 0;
    SafeVtSlot(eq, kSlotAdd, &ea); SafeVtSlot(eq, kSlotRemove, &er);
    const bool same = (g_vt_add_target && reinterpret_cast<void*>(ea) == g_vt_add_target) ||
                      (ea && g_exe_base && ea - g_exe_base == kRva_StoreAddImpl);
    char en[128] = "?"; SafeTypeName(eq, en, sizeof(en));
    SAY("equip store -> %s   add exe+0x%llX  remove exe+0x%llX%s", en,
        static_cast<unsigned long long>(ea ? ea - g_exe_base : 0),
        static_cast<unsigned long long>(er ? er - g_exe_base : 0),
        same ? "   (same implementation -- covered)" : "   (DIFFERENT -- not covered)");
}



// Dump each district volume: its position and the raw bytes of its
// DistrictComponent. Somewhere in there is the volume's extent -- the thing
// that decides which district a point is in. Three offline methods failed
// against the game's own counts (nearest volume origin, the tracker sweep,
// nearest labelled anchor at 5/15) precisely because that extent was missing.
void DumpDistrictVolumes() {
    std::vector<void*> vols;
    { std::lock_guard<std::mutex> g(g_vol_lock); vols = g_district_vols; }
    SAY("district volumes collected: %zu", vols.size());
    if (vols.empty()) { SAY("  (run `districts on`, then fly/fast-travel around so the game resolves them)"); return; }

    for (void* v : vols) {
        Actor* a = reinterpret_cast<Actor*>(v);
        char name[160] = "?";
        SafeReadName(a, name, sizeof(name));
        Vector3 p{};
        SafeActorPos(a, &p);
        SAY("  %-28s at [%.0f, %.0f, %.0f]", name, p.x, p.y, p.z);
        INFO("VOL %s pos %.2f %.2f %.2f actor=%p", name, p.x, p.y, p.z, v);

        uint32_t flags = 0; int count = 0; ComponentEntry* entries = nullptr;
        if (!SafeActorInfo(a, &flags, &count, &entries) || !entries || count <= 0 || count > 512) continue;
        for (int i = 0; i < count; ++i) {
            const char* cn = nullptr; void* comp = nullptr;
            if (!SafeEntry(entries, i, &cn, &comp) || !cn || !comp) continue;
            INFO("VOL   %s component[%d] %s @ %p", name, i, cn, comp);
            if (strcmp(cn, "DistrictComponent") != 0) continue;
            // 0x100 bytes is plenty to spot a min/max pair of floats.
            unsigned char b[0x100];
            if (!SafeRead(comp, b, sizeof(b))) { SAY("    DistrictComponent unreadable"); continue; }
            SAY("    DistrictComponent %p -> log", comp);
            for (size_t off = 0; off < sizeof(b); off += 16) {
                char line[128];
                int n = sprintf_s(line, sizeof(line), "%03X ", static_cast<unsigned>(off));
                for (int k = 0; k < 16; ++k) n += sprintf_s(line + n, sizeof(line) - n, "%02X ", b[off + k]);
                INFO("VOL   %s dc %s", name, line);
            }
            // Floats too: extents are far easier to spot as numbers.
            for (size_t off = 0; off + 16 <= sizeof(b); off += 16) {
                float f[4];
                memcpy(f, b + off, 16);
                INFO("VOL   %s df %03X  %12.2f %12.2f %12.2f %12.2f",
                     name, static_cast<unsigned>(off), f[0], f[1], f[2], f[3]);
            }
        }
    }
}


// --- district notification probe --------------------------------------------------
//
// The lesson: instrument broadly and sift, rather than guess which structure
// holds the answer. This is that. The game announces the district; we log the
// announcement with the hero position, plus raw bytes of both arguments so the
// identifier can be located offline.
using DistrictNotifyFn = void* (*)(void*, void*, void*, void*);
DistrictNotifyFn g_original_district_notify = nullptr;

void DumpBytesTo(const char* tag, void* p, size_t n) {
    if (!p) return;
    unsigned char b[0x80];
    if (n > sizeof(b)) n = sizeof(b);
    if (!SafeRead(p, b, n)) return;
    for (size_t off = 0; off < n; off += 16) {
        char line[128];
        int k = sprintf_s(line, sizeof(line), "%03X ", static_cast<unsigned>(off));
        for (int i = 0; i < 16; ++i) k += sprintf_s(line + k, sizeof(line) - k, "%02X ", b[off + i]);
        INFO("DN %s %s", tag, line);
    }
}

void* Hooked_DistrictNotify(void* a, void* b, void* c, void* d) {
    void* r = g_original_district_notify(a, b, c, d);
    if (g_district_log.load(std::memory_order_relaxed)) {
        Actor* hero = HeroActor();
        Vector3 p{};
        if (hero) SafeActorPos(hero, &p);
        INFO("DN fire this=%p arg=%p ret=%p hero %.1f %.1f %.1f", a, b, r, p.x, p.y, p.z);
        SAY("D district notification at [%.0f, %.0f]", p.x, p.z);
        DumpBytesTo("this", a, 0x80);
        DumpBytesTo("arg", b, 0x40);
        DumpBytesTo("ret", r, 0x80);
    }
    return r;
}


// --- district sampling ------------------------------------------------------------
//
// Reads only (resolve a handle, read a name), so it is safe from the poll
// thread under the rule the crash taught us: the parser and pollers may READ,
// never WRITE. Writes go to a file, not the game.
//
// Purpose: nearest-Vol_Dist on volume ORIGINS was proven wrong against the
// game's own district counts, and the capture has no volume extents. A cloud
// of (position, district) samples sidesteps the geometry entirely -- classify
// each check by its nearest labelled sample.
void SampleDistrict() {
    if (!g_district_log.load(std::memory_order_relaxed)) return;
    static uint64_t last = 0;
    const uint64_t now = GetTickCount64();
    if (now - last < 500) return;
    last = now;

    void* dt = FindHeroComponent("DistrictTrackerComponent");
    if (!dt) return;
    unsigned char b[0x400];
    if (!SafeRead(dt, b, sizeof(b))) return;

    char district[160] = "";
    for (size_t off = 0; off + 4 <= sizeof(b); off += 4) {
        uint32_t h; memcpy(&h, b + off, 4);
        if (!h || (h & 0xFFFFF) == 0 || (h >> 20) == 0) continue;
        void* obj = nullptr;
        if (!SafeResolveHandle(&h, &obj) || !obj) continue;
        char an[160] = "";
        if (!SafeReadName(reinterpret_cast<Actor*>(obj), an, sizeof(an))) continue;
        if (_strnicmp(an, "Vol_Dist", 8) == 0) { strncpy_s(district, an, _TRUNCATE); break; }
    }
    if (!district[0]) return;

    Actor* hero = HeroActor();
    if (!hero) return;
    Vector3 p{};
    // SafeActorPos, not an inline __try: this function now builds a std::string
    // for the runtime path, and SEH cannot share a frame with an object that
    // needs unwinding (C2712).
    if (!SafeActorPos(hero, &p)) return;
    if (FILE* f = nullptr; fopen_s(&f, RuntimeFile("district-samples.jsonl").c_str(), "a") == 0 && f) {
        fprintf(f, "{\"district\":\"%s\",\"pos\":[%.1f,%.1f,%.1f]}\n", district, p.x, p.y, p.z);
        fclose(f);
    }
}


// --- connection status ------------------------------------------------
//
// The mod does not take the connection details any more: an overlay that can
// be typed into needs keyboard focus, and giving it focus dropped the game out
// of exclusive fullscreen. connect_ui.py is a separate window and cannot do
// that. All this does now is mirror the client's status onto the dot.
// Dump every collectible object we know about. Run it once, collect a bot,
// run it again: the field that changed for exactly that one is the "collected"
// flag, and a field shared by 42 of them is the spider-bot category.
void DumpCollectibles() {
    const int n = g_collectible_count.load(std::memory_order_acquire);
    if (n == 0) { SAY("no collectibles recorded yet -- open the Collections menu once"); return; }
    FILE* f = nullptr;
    if (fopen_s(&f, RuntimeFile("collectibles.jsonl").c_str(), "a") != 0 || !f) {
        SAY("could not open collectibles.jsonl");
        return;
    }
    SYSTEMTIME t; GetLocalTime(&t);
    int written = 0;
    for (int i = 0; i < n; ++i) {
        const CollectibleEntry& c = g_collectibles[i];
        if (!c.object) continue;
        uint64_t body[32]{};
        if (!SafeRead(c.object, body, sizeof(body))) continue;
        fprintf(f, "{\"t\":\"%02d:%02d:%02d\",\"id\":\"0x%08X\",\"obj\":\"%p\",\"w\":[",
                t.wHour, t.wMinute, t.wSecond, c.id, c.object);
        for (int k = 0; k < 32; ++k)
            fprintf(f, "%s\"%016llX\"", k ? "," : "", static_cast<unsigned long long>(body[k]));
        fprintf(f, "]}\n");
        ++written;
    }
    fclose(f);
    SAY("dumped %d of %d collectibles to collectibles.jsonl", written, n);
}

// --- F8: open the connect window ------------------------------------------------
//
// Launching a process is all this does. The earlier F8 drew an input panel in
// the overlay, which needed focus, and focus cost us exclusive fullscreen --
// so nothing here ever calls SetForegroundWindow on our own windows.
const char* const kConnectWindowTitle = "Spider-Man 2 - Archipelago";

bool ConnectWindowOpen() {
    return FindWindowA(nullptr, kConnectWindowTitle) != nullptr;
}

void LaunchConnectWindow() {
    if (HWND w = FindWindowA(nullptr, kConnectWindowTitle)) {
        // Already running: raise it rather than starting a second copy. This
        // is the connect window's own HWND, not ours, so bringing it forward
        // is what the player asked for by pressing the key.
        ShowWindow(w, SW_SHOW);
        SetForegroundWindow(w);
        SAY("connect window raised");
        return;
    }

    // Where is the client? It writes its own folder into the runtime dir when
    // it starts (paths.register_client), so a tester can unzip it anywhere.
    std::string client_dir;
    if (FILE* f = nullptr; fopen_s(&f, RuntimeFile("client-path.txt").c_str(), "r") == 0 && f) {
        char buf[MAX_PATH * 2] = "";
        if (fgets(buf, sizeof(buf), f)) {
            for (char* c = buf; *c; ++c) if (*c == '\n' || *c == '\r') { *c = 0; break; }
            client_dir = buf;
        }
        fclose(f);
    }
    if (client_dir.empty()) {
        WARN("no client-path.txt in %s -- start the client once so F8 can find it", RuntimeDir().string().c_str());
        overlay::Push(3, "start the Archipelago client once, then F8 works");
        return;
    }
    const std::string script = client_dir + "\\connect_ui.py";

    // pythonw so no console flashes up: PATH first, then the Store alias, then
    // the usual per-user and machine installs.
    std::vector<std::string> candidates = { "pythonw.exe" };
    if (const char* la = getenv("LOCALAPPDATA")) {
        candidates.push_back(std::string(la) + "\\Microsoft\\WindowsApps\\pythonw.exe");
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(std::string(la) + "\\Programs\\Python", ec))
            candidates.push_back((e.path() / "pythonw.exe").string());
    }
    for (int v = 14; v >= 9; --v) candidates.push_back("C:\\Python3" + std::to_string(v) + "\\pythonw.exe");
    candidates.push_back("python.exe");
    for (const std::string& exe : candidates) {
        std::string cmd = "\"" + exe + "\" \"" + script + "\"";
        STARTUPINFOA si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::vector<char> cmdbuf(cmd.begin(), cmd.end()); cmdbuf.push_back(0);
        if (CreateProcessA(nullptr, cmdbuf.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, client_dir.c_str(), &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            SAY("connect window opening (%s)...", exe.c_str());
            overlay::Push(3, "Archipelago -- connect window opening");
            return;
        }
    }
    WARN("could not start connect_ui.py -- is Python on PATH?");
    overlay::Push(2, "could not open the connect window");
}

void PollConnectHotkey() {
    static bool was_down = false;
    const bool down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (down && !was_down) LaunchConnectWindow();
    was_down = down;
}

void PollConnectStatus() {
    // The overlay used to wait for an `overlay on` command, which meant F8
    // had no window to post to and silently did nothing. It is the connection
    // UI now, so it comes up on its own -- lazily, off the load path, because
    // creating it during DllMain has bitten us before.
    static bool tried = false;
    if (!tried && !overlay::Running()) {
        tried = true;
        if (overlay::Start()) {
            INFO("overlay auto-started -- feed + connection dot");
            overlay::Push(2, "Archipelago ready");
        } else {
            WARN("overlay failed to start; no feed or dot");
        }
    }

    // Seeding into the main menu silently does nothing useful: the hero and
    // its inventory stores do not exist yet, so grants land nowhere. Publish
    // whether we are actually in gameplay and let the client wait for it.
    {
        // "Ready" flickers true during the menu -> save hand-off: on 2026-09-06
        // it read gameplay for 20 s before the real load, the seed landed on
        // that transient hero, and the save then loaded over it. So the hero
        // must have a real world position too, and must stay ready for a few
        // seconds before we say so. Menu is reported immediately.
        bool ready = (HeroActor() != nullptr) && (InventoryManager() != nullptr);
        if (ready) {
            Vector3 p{};
            ready = SafeActorPos(HeroActor(), &p) && p.x > -1e5f && p.x < 1e5f && (p.x != 0.0f || p.z != 0.0f);
        }
        static uint64_t ready_since = 0;
        const uint64_t now_ms = GetTickCount64();
        if (!ready) ready_since = 0;
        else if (!ready_since) ready_since = now_ms;
        const bool stable = ready && (now_ms - ready_since >= 6000);
        static int last_state = -1;
        if (static_cast<int>(stable) != last_state) {
            last_state = static_cast<int>(stable);
            if (FILE* f = nullptr;
                fopen_s(&f, RuntimeFile("mod-state.txt").c_str(), "w") == 0 && f) {
                fputs(stable ? "gameplay" : "menu", f);
                fclose(f);
            }
            INFO("game state: %s", stable ? "gameplay" : "menu/loading");
        }
    }

    // The Python client owns the socket, so it reports state through a file.
    static uint64_t last = 0;
    const uint64_t now = GetTickCount64();
    if (now - last < 1000) return;
    last = now;
    if (FILE* f = nullptr; fopen_s(&f, RuntimeFile("ap-status.txt").c_str(), "r") == 0 && f) {
        char buf[160] = "";
        if (fgets(buf, sizeof(buf), f)) {
            for (char* c = buf; *c; ++c) if (*c == '\n' || *c == 13) { *c = 0; break; }
            static char shown[160] = "";
            if (strcmp(shown, buf) != 0) {
                strncpy_s(shown, buf, _TRUNCATE);
                overlay::SetConnected(_strnicmp(buf, "connected", 9) == 0, buf);
            }
        }
        fclose(f);
    }
}

// --- the polling thread ------------------------------------------------------------

void Poll() {
    for (;;) {
        Sleep(200);
        SampleDistrict();
        PollConnectStatus();
        PollConnectHotkey();
        gate::Tick();
        PollCommandFile();
        DrainStaleStoreRequests(1500);
        static int tick = 0;
        if (++tick % 10 == 0) WriteTrail();                       // ~ every 2 s
        // Belt and braces for "any time it is awarded it is reset to zero":
        // the hook refuses grants, this catches anything that gets past it.
        if (g_lockdown.load() && tick % 100 == 0)
            EnqueueStoreRequest(StoreOp::ZeroCurrency, 0, "currency", -1);   // silent
    }
}

void Run() {
    MODULEINFO mi{};
    if (GetModuleInformation(GetCurrentProcess(), GetModuleHandleA(nullptr), &mi, sizeof(mi))) {
        g_exe_base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
        g_exe_size = mi.SizeOfImage;
    }

    const bool a = InstallHook(reinterpret_cast<void*>(Native::Actor::AddComponent),
                               reinterpret_cast<void*>(&Hooked_AddComponent),
                               reinterpret_cast<void**>(&g_original_add_component), "Actor::AddComponent");
    const bool b = InstallHook(reinterpret_cast<void*>(Native::Actor::GetActor),
                               reinterpret_cast<void*>(&Hooked_GetActor),
                               reinterpret_cast<void**>(&g_original_get_actor), "Actor::GetActor");
    if (!a && !b) { FATAL("no drain hook installed -- requests would never run"); return; }

    // Log-only objective watch. Not fatal if it fails; the item side still works.
    InstallHook(reinterpret_cast<void*>(g_exe_base + kRva_ObjectiveSetState),
                reinterpret_cast<void*>(&Hooked_ObjectiveSetState),
                reinterpret_cast<void**>(&g_original_objective_set_state), "ObjectiveSystem::SetState (watch)");
    InstallHook(reinterpret_cast<void*>(g_exe_base + kRva_StoreAddImpl),
                reinterpret_cast<void*>(&Hooked_StoreAdd),
                reinterpret_cast<void**>(&g_original_store_add), "store add (lockdown)");
    InstallHook(reinterpret_cast<void*>(g_exe_base + kRva_StoreRemoveImpl),
                reinterpret_cast<void*>(&Hooked_StoreRemove),
                reinterpret_cast<void**>(&g_original_store_remove), "store remove (trace)");
    InstallHook(reinterpret_cast<void*>(g_exe_base + kRva_StatChoicePrius),
                reinterpret_cast<void*>(&Hooked_StatChoicePrius),
                reinterpret_cast<void**>(&g_original_statchoice_prius), "StatChoiceGroupItemPrius (probe)");
    InstallHook(reinterpret_cast<void*>(g_exe_base + kRva_PerkItemPrius),
                reinterpret_cast<void*>(&Hooked_PerkItemPrius),
                reinterpret_cast<void**>(&g_original_perkitem_prius), "PerkItemPrius (probe)");
    // The district-notification hook fired ZERO times across four district
    // crossings and coincided with the opening screen misbehaving, so it is
    // removed: it sat on the HUD data path for no benefit.
    InstallHook(reinterpret_cast<void*>(g_exe_base + kRva_Equip),
                reinterpret_cast<void*>(&Hooked_Equip),
                reinterpret_cast<void**>(&g_original_equip), "Equip (probe)");
    InstallHook(reinterpret_cast<void*>(g_exe_base + kRva_ProgRewardObjective_On),
                reinterpret_cast<void*>(&Hooked_RewardObjective),
                reinterpret_cast<void**>(&g_original_reward_objective), "ProgRewardObjective::On (watch)");
    InstallHook(reinterpret_cast<void*>(g_exe_base + kRva_ProgRewardCollectible_On),
                reinterpret_cast<void*>(&Hooked_RewardCollectible),
                reinterpret_cast<void**>(&g_original_reward_collectible), "ProgRewardCollectible::On (watch)");

    LoadLockdownState();
    if (g_lockdown.load()) EnqueueStoreRequest(StoreOp::ZeroCurrency, 0, "currency", -1);
    SAY("SM2-Archipelago ready.  %d items known (22 abilities, 18 suit tech, 128 skills / gadget upgrades).", kApItemCount);
    SAY("  commands are read from %s", kCommandFile);
    SAY("  remove <item>  add <item>  equip <item>  restore <item>  query <item>  list [text]");
    SAY("  seed empty|full   reset-suittech  grant-suittech  health-recompute  zero-currency");
    SAY("  district  where  lockdown on|off%s", g_lockdown.load() ? "   [LOCKDOWN IS ON]" : "");
    SAY("  '$' echoes a command, '!!' is a warning, everything else is a result.");

    std::thread(Poll).detach();
}

} // namespace

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { MH_Initialize(); Native::Init(); }
    return TRUE;
}

extern "C" __declspec(dllexport) void script_enable() {
    INFO("=== SM2-Archipelago script_enable ===");
    std::thread(Run).detach();
}
