// Wave 0 stub. Implementation is owned by Wave 2C.
#include "game/enemies/EnemyRoster.h"

#include "render/Renderer.h"
#include "sim/SimWorld.h"

namespace immune::game {

void EnemyRoster::load_defaults() {
    auto set = [this](PathogenFamily f, const char* name, SpeedTier speed, f32 density) {
        FamilyDef& d = families_[static_cast<u32>(f)];
        d.family = f;
        d.name = name;
        d.color = render::family_color(f);
        d.speed_tier = speed;
        d.base_density = density;
    };
    set(PathogenFamily::Virus, "virus", SpeedTier::Fast, 0.6f);
    set(PathogenFamily::Bacteria, "bacteria", SpeedTier::Normal, 1.8f);
    set(PathogenFamily::FungalSpore, "fungal_spore", SpeedTier::Slow, 1.2f);
    set(PathogenFamily::Parasite, "parasite", SpeedTier::Erratic, 2.5f);
    set(PathogenFamily::CancerCell, "cancer_cell", SpeedTier::Slow, 8.0f);
    set(PathogenFamily::Allergen, "allergen", SpeedTier::Normal, 1.0f);

    families_[static_cast<u32>(PathogenFamily::Virus)].replicates = true;
    families_[static_cast<u32>(PathogenFamily::Bacteria)].clumps = true;
    families_[static_cast<u32>(PathogenFamily::FungalSpore)].drifts = true;
    families_[static_cast<u32>(PathogenFamily::FungalSpore)].leaves_hazard = true;
    families_[static_cast<u32>(PathogenFamily::Parasite)].can_hide = true;

    elites_.clear();
}

const FamilyDef& EnemyRoster::family(PathogenFamily f) const {
    const u32 i = static_cast<u32>(f) < kFamilyCount ? static_cast<u32>(f) : 0u;
    return families_[i];
}

const EliteDef* EnemyRoster::find_elite(std::string_view name) const {
    for (const auto& e : elites_) {
        if (name == e.name) return &e;
    }
    return nullptr;
}

void EnemyRoster::apply_to_tuning(sim::ChaffTuning& tuning) const {
    for (u32 i = 0; i < kFamilyCount; ++i) {
        tuning.family[i].base_density = families_[i].base_density;
    }
}

EntityId EnemyRoster::spawn_elite(sim::SimWorld&, u16, Vec2) const { return EntityId{}; }
void EnemyRoster::register_systems(sim::SimWorld&) {}

} // namespace immune::game
