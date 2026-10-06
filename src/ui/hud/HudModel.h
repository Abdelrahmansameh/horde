// ui/hud/HudModel.h — everything the in-match HUD shows, as plain data.
//
// app/UiBridge.cpp fills this from the live game each frame; HudScreen only
// reads it. Keeping the screen off SimWorld/TowerSystem/WaveDirector is what
// lets every HUD state (wave, placing, inspect, prep, critical) be built,
// tested and screenshotted from a hand-written fixture.
#pragma once

#include "core/Types.h"
#include "game/abilities/ActiveAbilities.h"

#include <array>
#include <string>
#include <vector>

namespace immune::ui {

/// Mirrors game::WavePhase without making the screen depend on the director.
enum class HudWavePhase : u8 { Prep, Spawning, Clearing, Complete };

struct HudTowerCard {
    bool unlocked = false;
    /// The level's allowed_towers list permits it.
    bool allowed = true;
    u32 cost = 0;
    /// Baseline swarmer aggro radius, world units — the placement ring.
    f32 range = 0.0f;
};

struct HudAbility {
    bool unlocked = false;
    bool ready = false;
    f32 cooldown_remaining = 0.0f;
    f32 cooldown_total = 1.0f;
    /// World radius of the effect, for the aim reticle.
    f32 radius = 0.0f;
    /// False for Fever Response, which fires without a target.
    bool needs_target = true;
};

struct HudFamilyCount {
    PathogenFamily family = PathogenFamily::Virus;
    u32 count = 0;
};

struct HudWavePreview {
    bool valid = false;
    u32 wave_number = 0;  ///< 1-based.
    std::vector<HudFamilyCount> families;
    u32 elites = 0;
    u32 total = 0;
};

/// A tower on the board.
struct HudTower {
    EntityId id{};
    TowerType type = TowerType::Neutrophil;
    Vec2 world{0.0f, 0.0f};
    f32 range = 0.0f;
    f32 health = 0.0f;
    f32 health_max = 1.0f;
    /// ATP returned if sold now.
    u32 refund = 0;
};

/// Where the armed tower would go, from TowerSystem::validate.
struct HudPlacement {
    bool active = false;
    bool valid = false;
    Vec2 world{0.0f, 0.0f};  ///< Snapped position.
};

struct HudModel {
    // ---- Organ ----
    f32 integrity01 = 1.0f;

    // ---- Economy ----
    u32 atp = 0;
    f32 income_per_second = 0.0f;

    // ---- Waves ----
    HudWavePhase phase = HudWavePhase::Prep;
    u32 wave_number = 1;          ///< 1-based, the current (or upcoming in Prep) wave.
    u32 wave_count = 0;
    f32 phase_time_remaining = 0.0f;
    f32 prep_total = 0.0f;        ///< Length of this prep, for the countdown ring.
    bool all_waves_complete = false;
    bool auto_start = false;      ///< Prep ends by itself when its timer runs out.
    /// The wave the player should prepare for: the upcoming one in Prep, the
    /// next one while a wave is on.
    HudWavePreview preview;
    /// Where the horde enters, for the prep composition marker.
    bool has_spawn = false;
    Vec2 spawn_world{0.0f, 0.0f};

    // ---- Board ----
    std::array<HudTowerCard, kTowerTypeCount> cards{};
    std::vector<HudTower> towers;
    std::array<HudAbility, game::kAbilityCount> abilities{};
    HudPlacement placement;

    // ---- Clock ----
    f32 time_scale = 1.0f;
};

/// The canvas names (DESIGN.md §5; code identifiers are tower_type_name()).
const char* tower_display_name(TowerType t);
/// Icon file stem in assets/ui/icons.
const char* tower_icon(TowerType t);
/// Widget id ("neutrophil", "cytotoxic_t"): the dock card is "hud/dock/<id>".
const char* tower_id(TowerType t);
/// Widget id ("cascade", "histamine", "fever", "clot").
const char* ability_id_name(game::AbilityId id);
/// Short name under an ability cell ("Cascade", "Histamine", "Fever", "Clot").
const char* ability_short_name(game::AbilityId id);
const char* ability_icon(game::AbilityId id);
/// Theme colour name for an ability's tint.
const char* ability_color(game::AbilityId id);
const char* family_display_name(PathogenFamily f, bool plural);
const char* family_icon(PathogenFamily f);

/// The build dock's order, a flat list top to bottom. Hotkeys 1-5 follow it.
const std::vector<TowerType>& dock_order();
/// Position of `t` in dock order (0-based), i.e. its hotkey minus one.
u32 dock_slot(TowerType t);
TowerType dock_tower(u32 slot);

} // namespace immune::ui
