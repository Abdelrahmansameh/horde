// Wave 0 stub. Implementation is owned by Wave 4B.
#include "game/meta/MetaProgression.h"

namespace immune::game {

void MetaProgression::reset_to_new_game() {
    for (bool& b : tower_unlocked_) b = false;
    tower_unlocked_[static_cast<u32>(TowerType::Macrophage)] = true;
    tower_unlocked_[static_cast<u32>(TowerType::Neutrophil)] = true;
    memories_.clear();
    loadout_.clear();
    progress_ = CampaignProgress{};
}

bool MetaProgression::tower_unlocked(TowerType type) const {
    const u32 i = static_cast<u32>(type);
    return i < kTowerTypeCount && tower_unlocked_[i];
}

void MetaProgression::unlock_tower(TowerType type) {
    const u32 i = static_cast<u32>(type);
    if (i < kTowerTypeCount) tower_unlocked_[i] = true;
}

bool MetaProgression::select_memory(const std::string& id) {
    if (loadout_.size() >= loadout_slots_) return false;
    for (const auto& m : memories_) {
        if (m.id == id && m.unlocked) {
            loadout_.push_back(id);
            return true;
        }
    }
    return false;
}

void MetaProgression::clear_loadout() { loadout_.clear(); }

void MetaProgression::record_level_complete(const std::string& level_id) {
    for (const auto& id : progress_.completed_level_ids) {
        if (id == level_id) return;
    }
    progress_.completed_level_ids.push_back(level_id);
    ++progress_.levels_completed;
}

bool MetaProgression::save(const std::string&) const { return false; }
bool MetaProgression::load(const std::string&) { return false; }
std::string MetaProgression::to_json() const { return "{\"version\":1}"; }

bool MetaProgression::from_json(const std::string&, std::string& out_error) {
    out_error = "MetaProgression::from_json not implemented (Wave 4B)";
    return false;
}

} // namespace immune::game
