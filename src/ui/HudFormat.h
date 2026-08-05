// ui/HudFormat.h — pure, ImGui-free helpers extracted out of Hud.cpp so they
// can be unit-tested directly (see tests/test_hud_format.cpp). Owner: Wave 4E.
//
// Deliberately header-only: Hud.h is a frozen contract owned by a different
// wave and CMakeLists.txt's per-module source lists are shared across every
// agent working this tree right now, so adding a new immune_ui .cpp (and thus
// touching the root CMakeLists.txt) is out of scope for this wave. A
// header-only file needs neither -- it's included directly by Hud.cpp and by
// the test file, and both already link everything its inline bodies touch
// (game::WaveDef, EntityId, Vec2) via immune_app.
#pragma once

#include "core/Math.h"
#include "core/Types.h"
#include "game/abilities/ActiveAbilities.h"
#include "game/wave/WaveDirector.h"

#include <array>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace immune::ui::fmt {

/// Human-readable pathogen family name. No global equivalent exists elsewhere
/// in the codebase (Renderer.cpp only maps family -> colour).
inline const char* pathogen_family_name(PathogenFamily f) {
    switch (f) {
        case PathogenFamily::Virus:       return "Virus";
        case PathogenFamily::Bacteria:    return "Bacteria";
        case PathogenFamily::FungalSpore: return "Fungal Spore";
        case PathogenFamily::Parasite:    return "Parasite";
        case PathogenFamily::CancerCell:  return "Cancer Cell";
        case PathogenFamily::Allergen:    return "Allergen";
        default: break;
    }
    return "?";
}

/// One family's aggregated presence within a wave.
struct WaveFamilyCount {
    PathogenFamily family = PathogenFamily::Virus;
    u32 count = 0;
    bool has_elite = false;
};

/// Aggregates WaveDef::spawns by family (summing counts, OR-ing the elite
/// flag), skipping families with zero spawns. Order follows PathogenFamily's
/// declared order, matching the renderer's own batch order convention.
inline std::vector<WaveFamilyCount> summarize_wave(const game::WaveDef& wave) {
    std::array<u32, kFamilyCount> counts{};
    std::array<bool, kFamilyCount> elite{};
    for (const game::SpawnEntry& spawn : wave.spawns) {
        const u32 idx = static_cast<u32>(spawn.family);
        if (idx >= kFamilyCount) continue;
        counts[idx] += spawn.count;
        if (spawn.elite_id != 0) elite[idx] = true;
    }
    std::vector<WaveFamilyCount> out;
    for (u32 i = 0; i < kFamilyCount; ++i) {
        if (counts[i] == 0) continue;
        out.push_back(WaveFamilyCount{static_cast<PathogenFamily>(i), counts[i], elite[i]});
    }
    return out;
}

/// True if any SpawnEntry in the wave spawns a named elite/boss.
inline bool wave_has_elite(const game::WaveDef& wave) {
    for (const game::SpawnEntry& spawn : wave.spawns) {
        if (spawn.elite_id != 0) return true;
    }
    return false;
}

/// Rounds a countdown to whole seconds for display, clamping negative
/// straggler values (a wave director tick can leave phase_time_remaining
/// slightly negative the instant it crosses zero) to "0s".
inline std::string format_countdown_seconds(f32 seconds_remaining) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0fs", static_cast<double>(math::max(seconds_remaining, 0.0f)));
    return std::string(buf);
}

/// "Ready" or a countdown, for the ability bar / any cooldown-gated button.
inline std::string format_ability_cooldown(const game::AbilityStatus& status) {
    if (status.ready) return "Ready";
    return format_countdown_seconds(status.cooldown_remaining);
}

/// Nearest entry to `point` within `pick_radius` world units, or an invalid
/// EntityId if nothing qualifies. Pulled out of the click-to-select handler
/// so the selection rule (nearest-within-radius, not just first-hit) is
/// testable without a live ECS/registry.
inline EntityId pick_nearest(const std::vector<std::pair<EntityId, Vec2>>& entities, Vec2 point,
                             f32 pick_radius) {
    EntityId best{};
    f32 best_dist_sq = pick_radius * pick_radius;
    for (const auto& [id, pos] : entities) {
        const f32 d2 = math::length_sq(pos - point);
        if (d2 <= best_dist_sq) {
            best_dist_sq = d2;
            best = id;
        }
    }
    return best;
}

} // namespace immune::ui::fmt
