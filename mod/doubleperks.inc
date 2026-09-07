// ------------------------------------------------------------ doubleperks.inc
//
// Suit Tech's paired perks: eight DoublePerk items, each offering a choice of
// two, two pairs per column. The game lets the player hold exactly one of each
// pair, so the AP must too -- granting both is not a state the game produces.
//
// Design note: "we can't have all of those on as they are the dual selections, but we
// should turn on at least one of them for each at random every seed" and "maybe
// make a spot in the YAML where you can pick which one you want on."
//
// So each pair has a YAML choice: A, B, or random. Seeding applies the choice
// and removes the loser; `pick <perk>` does the same for one pair at runtime.
//
// Extracted from the DoublePerk items' PerkConfig1/PerkConfig2 fields; every
// path verified present in the toc. See the client's double-perks.json.

struct DoublePerk {
    const char* column;
    const char* a_label;  uint64_t a_hash;
    const char* b_label;  uint64_t b_hash;
};

const DoublePerk kDoublePerks[] = {
    { "Health",    "Confidence Boost",   0x9F452A3D66013461ull, "Rejuvenating Parry", 0xBEE32D05C33C2BB9ull },
    { "Health",    "The Best Defense",   0x95F005EBF04D3689ull, "Life Link",          0xBD35072D7E0114F0ull },
    { "Damage",    "Air Marshal",        0x813381135A2CC078ull, "Focused Parry",      0x89BAEECAC067F8F6ull },
    { "Damage",    "Combo King",         0xB9DA25DBE23218ABull, "Floor Is Lava",      0x97C2410B72FD16EAull },
    { "Focus",     "Focus Strike",       0xA78413848917688Cull, "Target Acquisition", 0xBA485D99C372ADDAull },
    { "Focus",     "Perfect Sight",      0xABC88C9B082C5DF5ull, "Eyes On Target",     0xB6F44F0B9775EEDAull },
    { "Traversal", "Acrobat",            0x84370D4B4C4D113Eull, "Charged",            0xB1ED2CB9CBC64292ull },
    { "Traversal", "All Seeing",         0x9292304A53CD8BB9ull, "Active Spider",      0x91E56BE841272859ull },
};
constexpr int kDoublePerkCount = static_cast<int>(sizeof(kDoublePerks) / sizeof(*kDoublePerks));

// The YAML's choice per pair: -1 random, 0 = A, 1 = B. Defaults to random.
int8_t g_dp_choice[kDoublePerkCount] = { -1, -1, -1, -1, -1, -1, -1, -1 };

// In the game, buying one of a pair buys both. So a pair is not
// an ownership split -- purchasing either side unlocks both, and the choice is
// only which one is ACTIVE. Owning one and handing the other back (what this
// did before) is a state the game never produces, and it left the unchosen side
// reading UNAFFORDABLE with currency zeroed, so it could never be recovered.
//
// The active side is the StatChoiceGroupItemPrius record in the save -- the one
// piece we still cannot write. Until we can, own both and let the game's own
// selection stand; the player can switch sides in the menu for free.
bool ApplyDoublePerk(int i, int side) {
    const DoublePerk& d = kDoublePerks[i];
    const bool ok_a = CraftItem(d.a_hash, d.a_label);
    const bool ok_b = CraftItem(d.b_hash, d.b_label);
    SAY("  %-10s %s + %s%s", d.column, d.a_label, d.b_label,
        (ok_a && ok_b) ? "" : "   (one did not take)");
    return ok_a && ok_b;
}

// Called by seeding: both sides of every pair, which is what a real purchase
// produces. The YAML choice is kept for when the active-side record is writable.
void ApplyAllDoublePerks(uint32_t seed) {
    SAY("paired suit-tech perks (buying one buys both -- granting both):");
    for (int i = 0; i < kDoublePerkCount; ++i) {
        int side = g_dp_choice[i];
        if (side < 0) { seed = seed * 1664525u + 1013904223u; side = (seed >> 16) & 1; }
        ApplyDoublePerk(i, side);
    }
}
