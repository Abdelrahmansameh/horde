// game/editor/LevelTemplates.h — starter geometries and the wave ramp
// generator. NEW MODULE.
//
// RATIONALE
// Both generators exist to kill the tedium that stops people building levels.
// Neither is a runtime system: they MATERIALISE real data into the document,
// which you then hand-edit.
//
// That distinction is the important one. This project deliberately deleted a
// procedural wave generator (see Level.h's AUTHORED WAVES note) because a table
// derived from a level's region string cannot express two levels in the same
// region meant to play differently. Nothing here reintroduces that: the ramp
// generator is a typist for the wave table, exactly as ui/GymPanel is a typist
// for the gym language, and once it has run there is no generator left in the
// picture -- just the WaveDefs it wrote, which the author owns.
//
// Headless: no ImGui, no GL. Every template must produce a level that passes
// validate_level() and instantiates, which tests/test_level_templates asserts.
#pragma once

#include "core/Types.h"
#include "game/level/Level.h"

#include <string>
#include <vector>

namespace immune::game {

/// The starter geometries offered by File > New.
enum class LevelTemplate : u8 {
    StraightLane = 0,   ///< One lane, end to end. The tutorial shape.
    Fork,               ///< A trunk that splits into two branches on one lane.
    Switchback,         ///< A hairpin: the shape that makes tower range matter.
    ConvergentChamber,  ///< Two lanes meeting at a shared objective.
    MultiLaneTrunk,     ///< Three typed lanes, each with its own spawn point.
};

const char* level_template_to_string(LevelTemplate t);
/// Every template, in menu order.
std::vector<LevelTemplate> all_level_templates();

struct TemplateParams {
    std::string name = "new_level";
    std::string region = "capillary";
    /// World size in world units. The default is sized so the default lane
    /// width leaves room for towers either side.
    Vec2 world_size{480.0f, 260.0f};
    f32 cell_size = 0.5f;
    /// Lane lumen width. Defaults to the ~68 units a lane needs to hold two
    /// squads abreast (ARCHITECTURE.md 4.7); anything much narrower reads as a
    /// single column however well the steering works.
    f32 lane_width = 68.0f;
    u32 wave_count = 6;
};

/// A complete, valid, playable level: geometry, spawn points, an objective,
/// placement zones and a wave table. Never returns a document that fails
/// validate_level().
LevelDef make_template(LevelTemplate t, const TemplateParams& params = {});

/// Inputs for the wave ramp generator. Every shipped table is visibly a linear
/// ramp (counts 135/210/285..., prep 8.0->4.0, ATP 50->120), so this is the
/// shape of the thing an author actually types out by hand.
struct WaveRampParams {
    u32 wave_count = 8;
    /// Per-family first/last wave counts. A family with both zero is skipped,
    /// which is how you ramp only viruses and layer bacteria in later.
    u32 first_count[kFamilyCount] = {135, 0};
    u32 last_count[kFamilyCount] = {600, 240};
    /// Wave at which each family first appears (0-based). Lets bacteria start
    /// at wave 2 while viruses run the whole table.
    u32 first_wave[kFamilyCount] = {0, 1};
    f32 first_prep = 8.0f;
    f32 last_prep = 4.0f;
    u32 first_atp = 50;
    u32 last_atp = 120;
    f32 duration = 4.0f;
    /// Curve exponent on the count ramp: 1 is linear, >1 back-loads the
    /// difficulty, <1 front-loads it.
    f32 curve = 1.0f;
    /// Which spawn point the generated entries name. Empty means "any", which
    /// is the documented meaning and what a single-spawn level wants.
    std::string spawn_point_id;
    std::string name_prefix = "wave";
};

/// Materialises the ramp as real WaveDefs. Pure; hand these to the document.
std::vector<WaveDef> make_wave_ramp(const WaveRampParams& params);

/// Peak concurrent agents an entry is responsible for, and the total it
/// releases, so the wave timeline can show a budget against
/// sim.capacities.max_chaff BEFORE the wave is played rather than during it.
struct WaveBudget {
    u32 total_agents = 0;      ///< Sum of counts across the wave.
    f32 peak_per_second = 0.0f;///< Highest release rate at any instant.
    f32 span_seconds = 0.0f;   ///< Last entry end minus first entry start.
};
WaveBudget wave_budget(const WaveDef& w);

} // namespace immune::game
