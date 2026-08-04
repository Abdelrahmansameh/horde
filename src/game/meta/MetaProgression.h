// game/meta/MetaProgression.h — unlocks, loadout modifiers, save/load.
// FROZEN CONTRACT. Owner: Wave 4B.
//
// Save data is versioned JSON. Loading an older version must migrate, not fail;
// loading a NEWER version must fail loudly rather than silently drop fields.
#pragma once

#include "core/Types.h"

#include <string>
#include <vector>

namespace immune::game {

/// Passive pre-run loadout modifier ("antibody memory", DESIGN.md §9).
struct AntibodyMemory {
    std::string id;
    std::string display_name;
    /// Multiplicative damage bonus against one family (1.0 = none).
    f32 damage_vs_family[kFamilyCount] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    f32 starting_atp_bonus = 0.0f;
    f32 tower_cost_multiplier = 1.0f;
    f32 income_multiplier = 1.0f;
    bool unlocked = false;
};

struct CampaignProgress {
    u32 levels_completed = 0;
    std::vector<std::string> completed_level_ids;
    std::vector<std::string> unlocked_regions;
};

class MetaProgression {
public:
    static constexpr i32 kSaveVersion = 1;

    void reset_to_new_game();

    bool tower_unlocked(TowerType type) const;
    void unlock_tower(TowerType type);

    const std::vector<AntibodyMemory>& memories() const { return memories_; }
    /// Loadout selected for the next run. At most `max_loadout_slots()` entries.
    const std::vector<std::string>& selected_loadout() const { return loadout_; }
    bool select_memory(const std::string& id);
    void clear_loadout();
    u32 max_loadout_slots() const { return loadout_slots_; }

    const CampaignProgress& progress() const { return progress_; }
    void record_level_complete(const std::string& level_id);

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
};

} // namespace immune::game
