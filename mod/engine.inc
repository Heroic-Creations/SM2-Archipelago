// ----------------------------------------------------------------- engine.inc
//
// Everything the mod knows about the game that the SDK does not provide:
// exe-relative addresses, the handle table, guarded reads, and the two
// component helpers every other file uses.
//
// RVAs were recomputed from the raw E8 / rip-relative bytes at their use
// sites, never taken from a disassembler's printed absolute address (that
// slip crashed the game once). Build 2.810.0.0. Image base is read at
// runtime; nothing here assumes where the exe was loaded.

// --- exe-relative addresses -------------------------------------------------

constexpr uintptr_t kRva_ResolveHandle    = 0x29C0640;  // (u32* handle) -> object*   generational table
constexpr uintptr_t kRva_IsMainThread     = 0x308A0F0;  // () -> bool                 the engine's own check
constexpr uintptr_t kRva_ReleaseOut       = 0x34B9830;  // (out*)                     releases a store call's out-handle
constexpr uintptr_t kRva_Equip            = 0x14B0B80;  // (equip_store, item_hash, slot, flag) -> bool
constexpr uintptr_t kRva_HealthRecompute  = 0x157E4F0;  // (HealthPerkHelper*)         re-sums perks -> SetMaxHealth
constexpr uintptr_t kRva_HealthDelta      = 0x347A680;  // (HeroHealth*, float delta)  current + delta -> vt[0x70]; the path damage and healing use

// Objective / mission state write. Nine callers, all mission-node code (one
// tagged "Mission Started Auto" / "Mission Complete Auto"). key64 is the
// node's 64-bit key (mission id in the high dword, objective id in the low),
// val the new state. Hooked log-only: this is where location detection starts.
//     ObjectiveSystem::SetState(sys, key64, val):
//         table = 0x2D7A6D0(sys, 0); e = hashmap_find_or_insert(table+0xC0, key64); set(e+0x30, val)
constexpr uintptr_t kRva_ObjectiveSetState = 0x358D7D0;

// The shared store REMOVE implementation, twin of the add at 0x34BC5E0.
// Tracing both shows a whole transaction: crafting adds the item AND removes
// the currency, and whatever else it touches.
constexpr uintptr_t kRva_StoreRemoveImpl = 0x34BD4C0;

// Event-level candidates: ProgressionRewardSystem dispatches these virtuals
// once per completed objective / collected collectible (no direct callers --
// vtable only). `this` is the reward entry, whose config carries the
// objective's crc64 name key; the entry's first qwords are logged so the key
// can be matched offline against crc64(save-schema name).
constexpr uintptr_t kRva_ProgRewardObjective_On   = 0x15F4CF0;   // ProgRewardObjective   vt+0x38 (this, ctx)
constexpr uintptr_t kRva_ProgRewardCollectible_On = 0x15F4110;   // ProgRewardCollectible vt+0x40 (this, ctx)

// The HUD's district-notification data fetch (15 callers). It fires when the
// game tells the player which district they are in -- i.e. the game's OWN
// answer to the containment question that five offline heuristics failed to
// reconstruct. Hooked to dump its arguments alongside the hero position, so
// the district identifier can be found in the raw bytes.
constexpr uintptr_t kRva_DistrictNotifyData = 0xCC49E0;

// GameUISettings -- a DDL type descriptor with 35 fields, one byte of type
// code each (0x0F bool, 0x0A string). Field 22 is AllowDebugMenuInPauseMenu,
// confirmed by its name pointer and by h32("AllowDebugMenuInPauseMenu") =
// 0x8B6F67A7 sitting at index 22 of the field-hash array.
//
// The retail PC binary still ships the whole DevMenu/DebugMenu system --
// DevMenuList at exe+0xCFD250 has 15 callers -- so the menu looks gated rather
// than stripped. These values live in .data and are populated at runtime.
constexpr uintptr_t kRva_GameUISettingsValues = 0xBCA4A30;
constexpr uintptr_t kRva_GameUISettingsNames  = 0x5C48E40;   // 35 x char*
constexpr uintptr_t kRva_GameUISettingsTypes  = 0x5C492C0;   // 35 x u8

// The GameUISettings accessor: reads the live instance from [rcx+0x40],
// creating it via exe+0x30AAFD0 if null, then hands it and the descriptor
// (exe+0xA0B3170) to the deserializer. exe+0xC363520 turned out to be the
// TYPE registry -- descriptors keyed by hash, not instances -- so hooking this
// is how the live object's address is obtained.
constexpr uintptr_t kRva_GameUISettingsGet = 0x2643D20;

// --- process state ----------------------------------------------------------

uintptr_t g_exe_base = 0;
size_t    g_exe_size = 0;

template <typename Fn>
Fn Rva(uintptr_t rva) { return reinterpret_cast<Fn>(g_exe_base + rva); }

// --- faults -------------------------------------------------------------------
//
// Every engine call is wrapped in SEH. When one faults we record where,
// exe-relative, so the log says which engine function tripped and on what.

struct FaultInfo { uint32_t code; uintptr_t addr_rva; };
FaultInfo g_last_fault{};

int RecordFault(EXCEPTION_POINTERS* ep) {
    g_last_fault.code = ep->ExceptionRecord->ExceptionCode;
    const uintptr_t a = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
    g_last_fault.addr_rva = (g_exe_base && a >= g_exe_base && a < g_exe_base + g_exe_size) ? a - g_exe_base : a;
    return EXCEPTION_EXECUTE_HANDLER;
}

#define FAULT_DETAIL g_last_fault.code, static_cast<unsigned long long>(g_last_fault.addr_rva)

// --- guarded reads --------------------------------------------------------------
//
// SEH cannot share a function with C++ objects that need unwinding, so each
// guarded read is its own object-free function.

bool SafeRead(const void* src, void* dst, size_t n) {
    __try { memcpy(dst, src, n); return true; }
    __except (RecordFault(GetExceptionInformation())) { return false; }
}

bool SafeReadName(Actor* a, char* out, size_t size) {
    __try {
        const char* n = a->GetName();
        if (!n) return false;
        size_t i = 0;
        for (; i < size - 1; ++i) {
            const unsigned char c = static_cast<unsigned char>(n[i]);
            if (c == 0) break;
            if (c < 0x20 || c > 0x7E) return false;
            out[i] = static_cast<char>(c);
        }
        out[i] = 0;
        return i > 0;
    } __except (RecordFault(GetExceptionInformation())) { return false; }
}

bool SafeActorInfo(Actor* a, uint32_t* flags, int* count, ComponentEntry** entries) {
    __try {
        *flags   = a->GetFlags();
        *count   = static_cast<int>(a->GetComponentCount());
        *entries = a->GetComponentEntries();
        return true;
    } __except (RecordFault(GetExceptionInformation())) { return false; }
}

bool SafeEntry(ComponentEntry* entries, int i, const char** name, void** comp) {
    __try {
        ComponentInfo* info = entries[i].info;
        if (!info || !info->name) return false;
        *name = info->name;
        *comp = entries[i].component;
        return true;
    } __except (RecordFault(GetExceptionInformation())) { return false; }
}

// --- the engine's own checks and lookups ------------------------------------------

// Engine store calls fault off the main thread: the frame-context getter
// (0x2AF76E0) branches on this and dereferences a per-thread context our
// threads do not have. So this gates every queued request.
bool IsGameThread() {
    __try { return Rva<bool (*)()>(kRva_IsMainThread)(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Generational handle -> object. idx = h & 0xFFFFF, gen = h >> 20; pure reads.
using ResolveFn = void* (*)(void* handle_ptr);
bool SafeResolveHandle(void* handle_ptr, void** out) {
    __try { *out = Rva<ResolveFn>(kRva_ResolveHandle)(handle_ptr); return true; }
    __except (RecordFault(GetExceptionInformation())) { return false; }
}

// A component's vtable[0x48]() returns its ComponentInfo*; the name is at +0x60.
// Only called on objects whose vtable pointer lies inside the exe image.
bool SafeTypeName(void* obj, char* out, size_t size) {
    __try {
        void** vt = *reinterpret_cast<void***>(obj);
        const uintptr_t v = reinterpret_cast<uintptr_t>(vt);
        if (!g_exe_base || v < g_exe_base || v >= g_exe_base + g_exe_size) return false;
        void* info = reinterpret_cast<void* (*)(void*)>(vt[0x48 / 8])(obj);
        if (!info) return false;
        const char* n = *reinterpret_cast<const char**>(reinterpret_cast<char*>(info) + 0x60);
        if (!n) return false;
        strncpy_s(out, size, n, _TRUNCATE);
        return true;
    } __except (RecordFault(GetExceptionInformation())) { return false; }
}


// An actor's world position, guarded. Its own function because SEH cannot
// share one with C++ objects that need unwinding (error C2712) -- the same
// rule that shaped every other Safe* helper here.
bool SafeActorPos(Actor* a, Vector3* out) {
    __try { *out = a->GetPosition(); return true; }
    __except (RecordFault(GetExceptionInformation())) { return false; }
}

// --- the hero ----------------------------------------------------------------------

Actor* HeroActor() {
    __try {
        HeroSystem* system = GetHeroSystem();
        if (!system) return nullptr;
        return system->GetHero();
    } __except (RecordFault(GetExceptionInformation())) { return nullptr; }
}

// Walk the hero's component list for one by name. The SDK declares
// Actor::GetComponent but leaves it out, so we do it here.
void* FindHeroComponent(const char* wanted) {
    Actor* hero = HeroActor();
    if (!hero) return nullptr;
    uint32_t flags = 0; int count = 0; ComponentEntry* entries = nullptr;
    if (!SafeActorInfo(hero, &flags, &count, &entries) || !entries || count <= 0 || count > 2048) return nullptr;
    for (int i = 0; i < count; ++i) {
        const char* name = nullptr; void* comp = nullptr;
        if (SafeEntry(entries, i, &name, &comp) && strcmp(name, wanted) == 0) return comp;
    }
    return nullptr;
}
