// game/meta/MetaProgression.h — unlocks, loadout modifiers, save/load.
// FROZEN CONTRACT. Owner: Wave 4B, extended by Wave 5A (DESIGN.md §7.2/§7.3's
// antibody-memory currency loop: earn-per-run, permanent-upgrade purchases,
// loadout-modifier aggregation, and the real save/load bodies). Nothing
// outside src/app/App.cpp consumes this header yet (verified at the start of
// Wave 5A), so the extensions below are additive and do not break any other
// wave's build.
//
// Save data is versioned JSON. Loading an older version must migrate, not fail;
// loading a NEWER version must fail loudly rather than silently drop fields.
#pragma once

#include "core/Types.h"

#include <string>
#include <vector>

namespace immune::game {

/// Which of DESIGN.md §7.2's three permanent-upgrade categories a purchasable
/// AntibodyMemory belongs to. Order matches the design doc's dependable-first
/// ordering (cheap/broad -> mid/gating -> expensive/specialized) but nothing
/// in code depends on the numeric order beyond storage as a u8 in JSON.
enum class MemoryTier : u8 {
    GlobalBaseline = 0,  ///< Small universal income/damage/build-speed bumps.
    RosterUnlock = 1,    ///< Unlocks a new tower archetype (see unlocks_tower).
    Specialized = 2,     ///< Archetype/family-specific, highest cost.
};

/// Passive pre-run loadout modifier ("antibody memory", DESIGN.md §7.2/§7.3).
struct AntibodyMemory {
    std::string id;
    std::string display_name;
    /// Multiplicative damage bonus against one family (1.0 = none).
    f32 damage_vs_family[kFamilyCount] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    f32 starting_atp_bonus = 0.0f;
    f32 tower_cost_multiplier = 1.0f;
    f32 income_multiplier = 1.0f;
    bool unlocked = false;

    // ---- Wave 5A additions -------------------------------------------------
    /// Which permanent-upgrade category this entry belongs to (DESIGN.md
    /// §7.2). Purely a UI/shop grouping + doesn't affect combination math.
    MemoryTier tier = MemoryTier::GlobalBaseline;
    /// Cost in antibody_points() to purchase() this entry. Ignored once
    /// `unlocked` is already true.
    u32 cost = 0;
    /// For MemoryTier::RosterUnlock entries: the tower this purchase also
    /// unlocks, in addition to setting `unlocked` (DESIGN.md §7.2 point 2,
    /// "new cell types become available"). TowerType::Count is the sentinel
    /// for "purchasing this does not unlock a tower" and is the correct
    /// default for GlobalBaseline/Specialized entries.
    TowerType unlocks_tower = TowerType::Count;
};

/// Combined effect of the currently selected_loadout() (DESIGN.md §7.3),
/// ready to apply to Economy/TowerSystem/damage calculations at run start.
/// Combination rule (see MetaProgression::combine_loadout_modifiers):
/// multiplier fields (damage_vs_family, tower_cost_multiplier,
/// income_multiplier) multiply across the selected memories; the flat
/// starting_atp_bonus fields sum. Multiplying flat ATP bonuses together
/// would be dimensionally meaningless (a "+50 ATP" and a "+30 ATP" memory
/// together should read as "+80 ATP", not "+1500 ATP"), whereas the
/// multiplier fields are already expressed as ratios around 1.0 and DESIGN.md
/// §7.3 explicitly frames loadout picks as independent percentage biases
/// ("+X% Macrophage damage"), which compose multiplicatively by convention
/// everywhere else in this codebase's damage/economy math.
struct LoadoutModifiers {
    f32 damage_vs_family[kFamilyCount] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    f32 starting_atp_bonus = 0.0f;
    f32 tower_cost_multiplier = 1.0f;
    f32 income_multiplier = 1.0f;
};

/// Outcome of a single completed or failed run, as input to earn_for_run().
/// Deliberately independent of SimWorld/WaveDirector types so the earn
/// formula is testable without constructing a real sim. The caller (app/)
/// is responsible for filling this in from SimSnapshot / WaveDirector's
/// status() / named-agent kill tracking at run end -- see MetaProgression.cpp
/// for the exact formula and the Wave 5A report for which fields app/
/// currently has data for.
struct RunResult {
    u32 waves_cleared = 0;
    u64 chaff_killed_total = 0;
    u32 elites_killed = 0;
    u32 bosses_killed = 0;
    /// True only on a full campaign-level clear. Per DESIGN.md §7.2, currency
    /// is earned on every run regardless of this flag -- `won` only adds a
    /// bonus on top, it never gates earning to zero.
    bool won = false;
};

struct CampaignProgress {
    u32 levels_completed = 0;
    std::vector<std::string> completed_level_ids;
    std::vector<std::string> unlocked_regions;
};

class MetaProgression {
public:
    static constexpr i32 kSaveVersion = 1;

    // ---- Wave 5A earn-rate constants ---------------------------------------
    // DESIGN.md §14 flags the exact antibody-memory earn-rate curve and
    // permanent-upgrade cost table as an explicit open question pending a
    // balance pass; these are placeholder values chosen only to satisfy
    // §7.2's *structural* requirements (every run earns something; density,
    // waves, and named kills all contribute) and are public/constexpr so a
    // future balance pass can retune them without touching earn_for_run()'s
    // logic.
    static constexpr u32 kBaseRunReward = 10;      ///< Flat, every run, even a wipe on wave 0.
    static constexpr u32 kPerWaveReward = 15;       ///< Per wave fully cleared.
    static constexpr u32 kPerEliteReward = 8;       ///< Per elite defeated.
    static constexpr u32 kPerBossReward = 50;       ///< Per boss defeated.
    static constexpr u32 kChaffPerPoint = 200;      ///< Chaff density killed, coarse-grained.
    static constexpr u32 kWinBonus = 40;            ///< Flat bonus for a full clear.

    /// Pure function: currency earned from a run's outcome (DESIGN.md §7.2).
    /// Earns something on every run (kBaseRunReward alone guarantees > 0),
    /// scaled up by performance signals, never gated by `won`. Static and
    /// side-effect-free so it's unit-testable without a MetaProgression
    /// instance or a real SimWorld.
    static u32 earn_for_run(const RunResult& result);

    /// Combination rule for a loadout's aggregate modifiers -- see
    /// LoadoutModifiers's doc comment for the multiply-vs-sum rationale.
    /// Unknown ids in `selected_ids` (shouldn't happen via select_memory(),
    /// but this is deliberately tolerant for direct testing) are skipped.
    static LoadoutModifiers combine_loadout_modifiers(
        const std::vector<AntibodyMemory>& all_memories,
        const std::vector<std::string>& selected_ids);

    void reset_to_new_game();

    bool tower_unlocked(TowerType type) const;
    void unlock_tower(TowerType type);

    const std::vector<AntibodyMemory>& memories() const { return memories_; }
    /// Loadout selected for the next run. At most `max_loadout_slots()` entries.
    const std::vector<std::string>& selected_loadout() const { return loadout_; }
    bool select_memory(const std::string& id);
    void clear_loadout();
    u32 max_loadout_slots() const { return loadout_slots_; }

    /// combine_loadout_modifiers() applied to the current memories_/loadout_.
    /// This is what the orchestrator calls at run start.
    LoadoutModifiers compute_loadout_modifiers() const {
        return combine_loadout_modifiers(memories_, loadout_);
    }

    const CampaignProgress& progress() const { return progress_; }
    void record_level_complete(const std::string& level_id);

    // ---- Wave 5A currency + purchases --------------------------------------
    u64 antibody_points() const { return antibody_points_; }
    /// Adds to the persistent balance. Typically called with earn_for_run()'s
    /// result at the end of a run.
    void credit(u32 amount) { antibody_points_ += amount; }

    enum class PurchaseResult : u8 { Ok, NotFound, AlreadyUnlocked, InsufficientFunds };
    /// Spends antibody_points() to unlock the named memory (DESIGN.md §7.2's
    /// permanent-upgrade purchases). Sets `unlocked = true` and, for a
    /// MemoryTier::RosterUnlock entry with `unlocks_tower != TowerType::Count`,
    /// also unlocks that tower. Balance is untouched on any non-Ok result.
    PurchaseResult purchase(const std::string& id);

    /// Versioned JSON save/load. Returns false and leaves state untouched on a
    /// malformed or future-versioned file.
    bool save(const std::string& path) const;
    bool load(const std::string& path);

    /// Serialized form, exposed for tests without touching the filesystem.
    std::string to_json() const;
    bool from_json(const std::string& json, std::string& out_error);

private:
    bool tower_unlocked_[kTowerTypeCount] = {};
    std::vector<AntibodyMemory> memories_;
    std::vector<std::string> loadout_;
    CampaignProgress progress_;
    u32 loadout_slots_ = 2;
    u64 antibody_points_ = 0;
};

} // namespace immune::game
