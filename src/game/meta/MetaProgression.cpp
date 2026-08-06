// game/meta/MetaProgression.cpp — Wave 4B stub, implemented by Wave 5A
// (currency earn/spend loop, purchase tiers, versioned JSON save/load,
// loadout-modifier combination). See MetaProgression.h for the contract and
// rationale of every field this file reads or writes.
#include "game/meta/MetaProgression.h"

#include "platform/FileIO.h"

#include <nlohmann/json.hpp>

#include <utility>

namespace immune::game {

namespace {
using json = nlohmann::json;
} // namespace

void MetaProgression::reset_to_new_game() {
    for (bool& b : tower_unlocked_) b = false;
    tower_unlocked_[static_cast<u32>(TowerType::Macrophage)] = true;
    tower_unlocked_[static_cast<u32>(TowerType::Neutrophil)] = true;
    memories_.clear();
    loadout_.clear();
    progress_ = CampaignProgress{};
    antibody_points_ = 0;
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

// ---- Wave 5A: earn / spend loop -------------------------------------------

u32 MetaProgression::earn_for_run(const RunResult& result) {
    // DESIGN.md §7.2: earned on EVERY run, cleared or failed, scaled by
    // performance signals -- never gated to zero by `won`. See the header's
    // kBaseRunReward et al. doc comment: numbers are placeholders pending the
    // balance pass DESIGN.md §14 explicitly defers.
    u32 total = kBaseRunReward;
    total += result.waves_cleared * kPerWaveReward;
    total += static_cast<u32>(result.chaff_killed_total / kChaffPerPoint);
    total += result.elites_killed * kPerEliteReward;
    total += result.bosses_killed * kPerBossReward;
    if (result.won) total += kWinBonus;
    return total;
}

LoadoutModifiers MetaProgression::combine_loadout_modifiers(
    const std::vector<AntibodyMemory>& all_memories,
    const std::vector<std::string>& selected_ids) {
    LoadoutModifiers out;
    for (const std::string& id : selected_ids) {
        for (const AntibodyMemory& m : all_memories) {
            if (m.id != id) continue;
            for (u32 i = 0; i < kFamilyCount; ++i) {
                out.damage_vs_family[i] *= m.damage_vs_family[i];
            }
            out.starting_atp_bonus += m.starting_atp_bonus;
            out.tower_cost_multiplier *= m.tower_cost_multiplier;
            out.income_multiplier *= m.income_multiplier;
            break;
        }
    }
    return out;
}

MetaProgression::PurchaseResult MetaProgression::purchase(const std::string& id) {
    for (AntibodyMemory& m : memories_) {
        if (m.id != id) continue;
        if (m.unlocked) return PurchaseResult::AlreadyUnlocked;
        if (antibody_points_ < m.cost) return PurchaseResult::InsufficientFunds;
        antibody_points_ -= m.cost;
        m.unlocked = true;
        if (m.unlocks_tower != TowerType::Count) unlock_tower(m.unlocks_tower);
        return PurchaseResult::Ok;
    }
    return PurchaseResult::NotFound;
}

// ---- Wave 5A: versioned JSON save/load -------------------------------------

std::string MetaProgression::to_json() const {
    json j;
    j["version"] = kSaveVersion;
    j["antibody_points"] = antibody_points_;
    j["loadout_slots"] = loadout_slots_;

    json towers = json::array();
    for (u32 i = 0; i < kTowerTypeCount; ++i) {
        if (tower_unlocked_[i]) towers.push_back(i);
    }
    j["unlocked_towers"] = std::move(towers);

    json mem_arr = json::array();
    for (const AntibodyMemory& m : memories_) {
        json mj;
        mj["id"] = m.id;
        mj["display_name"] = m.display_name;
        json dvf = json::array();
        for (u32 i = 0; i < kFamilyCount; ++i) dvf.push_back(m.damage_vs_family[i]);
        mj["damage_vs_family"] = std::move(dvf);
        mj["starting_atp_bonus"] = m.starting_atp_bonus;
        mj["tower_cost_multiplier"] = m.tower_cost_multiplier;
        mj["income_multiplier"] = m.income_multiplier;
        mj["unlocked"] = m.unlocked;
        mj["tier"] = static_cast<u32>(m.tier);
        mj["cost"] = m.cost;
        mj["unlocks_tower"] = static_cast<u32>(m.unlocks_tower);
        mem_arr.push_back(std::move(mj));
    }
    j["memories"] = std::move(mem_arr);

    j["loadout"] = loadout_;

    json prog;
    prog["levels_completed"] = progress_.levels_completed;
    prog["completed_level_ids"] = progress_.completed_level_ids;
    prog["unlocked_regions"] = progress_.unlocked_regions;
    j["progress"] = std::move(prog);

    return j.dump();
}

bool MetaProgression::from_json(const std::string& text, std::string& out_error) {
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        out_error = std::string("MetaProgression JSON parse error: ") + e.what();
        return false;
    }

    if (!j.is_object()) {
        out_error = "MetaProgression JSON must be an object";
        return false;
    }
    // A missing "version" is an error, not a default (mirrors Level.h's
    // "schema" rule) -- a save file without a version is not a v1 save, it's
    // not a save file this code understands at all.
    if (!j.contains("version")) {
        out_error = "MetaProgression JSON missing required field 'version'";
        return false;
    }

    i32 version = 0;
    try {
        version = j.at("version").get<i32>();
    } catch (const std::exception& e) {
        out_error = std::string("MetaProgression JSON malformed 'version': ") + e.what();
        return false;
    }
    // Newer-than-supported must fail loudly, never silently drop fields.
    if (version > kSaveVersion) {
        out_error = "MetaProgression save version " + std::to_string(version) +
                     " is newer than supported version " + std::to_string(kSaveVersion) +
                     " -- refusing to load and silently drop fields";
        return false;
    }

    // version <= kSaveVersion: parse leniently (j.value() defaults) so an
    // older save missing fields this version added still loads -- that's the
    // whole of v1's migration story per the header comment. Everything is
    // built into locals first and only committed to members at the very end,
    // so a parse failure partway through leaves *this* completely untouched.
    try {
        const u64 antibody_points = j.value("antibody_points", u64{0});
        const u32 loadout_slots = j.value("loadout_slots", u32{2});

        bool towers[kTowerTypeCount] = {};
        if (j.contains("unlocked_towers")) {
            const json& arr = j.at("unlocked_towers");
            if (!arr.is_array()) throw std::runtime_error("'unlocked_towers' must be an array");
            for (const auto& idx_j : arr) {
                const u32 idx = idx_j.get<u32>();
                if (idx < kTowerTypeCount) towers[idx] = true;
            }
        }

        std::vector<AntibodyMemory> memories;
        if (j.contains("memories")) {
            const json& arr = j.at("memories");
            if (!arr.is_array()) throw std::runtime_error("'memories' must be an array");
            memories.reserve(arr.size());
            for (const auto& mj : arr) {
                if (!mj.contains("id")) throw std::runtime_error("memory entry missing 'id'");
                AntibodyMemory m;
                m.id = mj.at("id").get<std::string>();
                m.display_name = mj.value("display_name", std::string{});
                if (mj.contains("damage_vs_family")) {
                    const json& dvf = mj.at("damage_vs_family");
                    if (!dvf.is_array()) throw std::runtime_error("'damage_vs_family' must be an array");
                    for (u32 i = 0; i < kFamilyCount && i < dvf.size(); ++i) {
                        m.damage_vs_family[i] = dvf.at(i).get<f32>();
                    }
                }
                m.starting_atp_bonus = mj.value("starting_atp_bonus", 0.0f);
                m.tower_cost_multiplier = mj.value("tower_cost_multiplier", 1.0f);
                m.income_multiplier = mj.value("income_multiplier", 1.0f);
                m.unlocked = mj.value("unlocked", false);
                m.tier = static_cast<MemoryTier>(
                    mj.value("tier", static_cast<u32>(MemoryTier::GlobalBaseline)));
                m.cost = mj.value("cost", u32{0});
                m.unlocks_tower = static_cast<TowerType>(
                    mj.value("unlocks_tower", static_cast<u32>(TowerType::Count)));
                memories.push_back(std::move(m));
            }
        }

        std::vector<std::string> loadout;
        if (j.contains("loadout")) {
            const json& arr = j.at("loadout");
            if (!arr.is_array()) throw std::runtime_error("'loadout' must be an array");
            for (const auto& e : arr) loadout.push_back(e.get<std::string>());
        }

        CampaignProgress progress;
        if (j.contains("progress")) {
            const json& pj = j.at("progress");
            progress.levels_completed = pj.value("levels_completed", u32{0});
            if (pj.contains("completed_level_ids")) {
                for (const auto& e : pj.at("completed_level_ids")) {
                    progress.completed_level_ids.push_back(e.get<std::string>());
                }
            }
            if (pj.contains("unlocked_regions")) {
                for (const auto& e : pj.at("unlocked_regions")) {
                    progress.unlocked_regions.push_back(e.get<std::string>());
                }
            }
        }

        // Every field parsed successfully -- commit atomically.
        antibody_points_ = antibody_points;
        loadout_slots_ = loadout_slots;
        for (u32 i = 0; i < kTowerTypeCount; ++i) tower_unlocked_[i] = towers[i];
        memories_ = std::move(memories);
        loadout_ = std::move(loadout);
        progress_ = std::move(progress);
    } catch (const std::exception& e) {
        out_error = std::string("MetaProgression JSON malformed: ") + e.what();
        return false;
    }

    out_error.clear();
    return true;
}

bool MetaProgression::save(const std::string& path) const {
    return platform::write_text_file(path, to_json());
}

bool MetaProgression::load(const std::string& path) {
    const auto text = platform::read_text_file(path);
    if (!text) return false;
    std::string err;
    return from_json(*text, err);
}

} // namespace immune::game
