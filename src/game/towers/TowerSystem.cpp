// Wave 0 stub. Implementation is owned by Wave 2B.
#include "game/towers/TowerSystem.h"

#include "sim/SimWorld.h"

namespace immune::game {
namespace {
constexpr const char* kTowerNames[kTowerTypeCount] = {
    "macrophage", "neutrophil", "dendritic", "cytotoxic_t",
    "b_cell", "nk_cell", "mast_cell", "complement_cascade"};
} // namespace

const char* tower_type_name(TowerType type) {
    const u32 i = static_cast<u32>(type);
    return i < kTowerTypeCount ? kTowerNames[i] : "unknown";
}

bool parse_tower_type(std::string_view name, TowerType& out) {
    for (u32 i = 0; i < kTowerTypeCount; ++i) {
        if (name == kTowerNames[i]) {
            out = static_cast<TowerType>(i);
            return true;
        }
    }
    return false;
}

void TowerSystem::register_systems(sim::SimWorld&) {}

const TowerStats& TowerSystem::stats(TowerType type, u8 tier) const {
    const u32 t = static_cast<u32>(type) < kTowerTypeCount ? static_cast<u32>(type) : 0u;
    const u32 k = tier >= 1 && tier <= 3 ? static_cast<u32>(tier - 1) : 0u;
    return stats_[t][k];
}

void TowerSystem::set_stats(TowerType type, u8 tier, const TowerStats& s) {
    if (static_cast<u32>(type) >= kTowerTypeCount || tier < 1 || tier > 3) return;
    stats_[static_cast<u32>(type)][tier - 1] = s;
}

PlacementQuery TowerSystem::validate(const sim::SimWorld&, TowerType, Vec2 pos, u32) const {
    PlacementQuery q;
    q.snapped_position = pos;
    q.result = PlacementResult::NotOnTissue; // no tissue baked in Wave 0
    return q;
}

EntityId TowerSystem::place(sim::SimWorld&, TowerType, Vec2) { return EntityId{}; }
u8 TowerSystem::upgrade(sim::SimWorld&, EntityId) { return 0; }
u32 TowerSystem::sell(sim::SimWorld&, EntityId) { return 0; }
bool TowerSystem::trigger_ability(sim::SimWorld&, EntityId) { return false; }
EntityId TowerSystem::find_target(const sim::SimWorld&, Vec2, f32, u8, bool) const {
    return EntityId{};
}

} // namespace immune::game
