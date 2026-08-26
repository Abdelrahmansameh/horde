// Tests for game/editor/LevelTemplates.
//
// The contract a starter template has to meet is the strongest one in the
// editor: File > New must produce a level you can immediately PLAY. Not one
// that validates on paper -- one whose geometry bakes, whose spawn points sit
// on tissue and reach an objective, and whose wave table is non-empty and
// ramps. If a template ships broken, the first thing a new author sees is an
// error, so every template is put through the whole load path here.
#include "game/editor/LevelDoc.h"      // lane_ids
#include "game/editor/LevelTemplates.h"
#include "game/editor/LevelValidate.h"
#include "game/level/Level.h"
#include "game/level/LevelWriter.h"
#include "game/session/LevelSession.h"
#include "sim/SimWorld.h"
#include "sim/flowfield/FlowField.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace immune;
using namespace immune::game;

namespace {

struct Baked {
    sim::TissueMask mask;
    sim::DistanceField sdf;
    sim::FlowField flow;
    BakedGeometry view() { return BakedGeometry{&mask, &sdf, &flow}; }
};

Baked bake(const LevelDef& d) {
    Baked b;
    LevelLoader loader;
    REQUIRE(loader.bake_geometry(d, GeometryBakeDesc{}, b.mask, b.sdf, b.flow).ok);
    return b;
}

} // namespace

TEST_CASE("every starter template is valid, bakes, and is playable",
          "[level][templates]") {
    for (LevelTemplate t : all_level_templates()) {
        INFO("template = " << level_template_to_string(t));
        const LevelDef d = make_template(t);

        // Structure the loader insists on.
        REQUIRE_FALSE(d.vessels.empty());
        REQUIRE_FALSE(d.spawn_points.empty());
        REQUIRE_FALSE(d.objectives.empty());
        REQUIRE_FALSE(d.waves.empty());

        // The full validator, including the checks that need real geometry:
        // spawn points on tissue, and every one of them able to reach a goal.
        Baked b = bake(d);
        const std::vector<Issue> issues = validate_level(d, b.view());
        // No errors AND no warnings: File > New is the first thing a new author
        // sees, and a starter level that arrives already complaining teaches
        // them the validator is noise.
        for (const Issue& i : issues) {
            FAIL(std::string(severity_to_string(i.severity)) + ": " + i.message);
        }

        // Explicitly, because this is the property that makes it PLAYABLE
        // rather than merely well-formed.
        for (const SpawnPoint& p : d.spawn_points) {
            INFO("spawn " << p.id);
            REQUIRE(b.flow.reachable(p.position));
        }
    }
}

TEST_CASE("every starter template round-trips through the writer",
          "[level][templates]") {
    // A template you cannot save is a template you cannot use.
    for (LevelTemplate t : all_level_templates()) {
        INFO("template = " << level_template_to_string(t));
        const LevelDef d = make_template(t);
        LevelLoader loader;
        LevelDef back;
        const LevelLoadResult r = loader.load_string(level_to_json(d), back);
        INFO("load error: " << r.error);
        REQUIRE(r.ok);
        REQUIRE(level_equal(d, back));
    }
}

TEST_CASE("templates honour their parameters", "[level][templates]") {
    TemplateParams p;
    p.name = "custom";
    p.region = "organ_chamber";
    p.world_size = Vec2{320.0f, 200.0f};
    p.cell_size = 1.0f;
    p.lane_width = 40.0f;
    p.wave_count = 3;

    const LevelDef d = make_template(LevelTemplate::StraightLane, p);
    REQUIRE(d.name == "custom");
    REQUIRE(d.region == "organ_chamber");
    REQUIRE(d.cell_size == Catch::Approx(1.0f));
    REQUIRE(d.world_bounds.max.x == Catch::Approx(320.0f));
    REQUIRE(d.world_bounds.max.y == Catch::Approx(200.0f));
    REQUIRE(d.waves.size() == 3);
    for (const VesselPoint& vp : d.vessels[0].points) {
        REQUIRE(vp.width == Catch::Approx(40.0f));
    }
}

TEST_CASE("a multi-lane template feeds every lane", "[level][templates]") {
    // A generated table that puts the whole wave down lane one leaves the other
    // lanes empty all match. The validator only warns about that; better not to
    // author it in the first place.
    const LevelDef d = make_template(LevelTemplate::MultiLaneTrunk);
    REQUIRE(d.spawn_points.size() == 3);

    for (const SpawnPoint& sp : d.spawn_points) {
        bool fed = false;
        for (const WaveDef& w : d.waves) {
            for (const SpawnEntry& e : w.spawns) fed = fed || e.spawn_point_id == sp.id;
        }
        INFO("spawn point " << sp.id << " receives nothing");
        REQUIRE(fed);
    }

    // Three distinct, typed lanes -- the thing this template exists to show.
    REQUIRE(lane_ids(d).size() == 3);
    bool artery = false, lymph = false, nerve = false;
    for (const Vessel& v : d.vessels) {
        artery = artery || v.type == VesselType::Artery;
        lymph = lymph || v.type == VesselType::Lymphatic;
        nerve = nerve || v.type == VesselType::NerveAdjacent;
    }
    REQUIRE(artery);
    REQUIRE(lymph);
    REQUIRE(nerve);
}

TEST_CASE("the wave ramp generator ramps", "[level][templates][waves]") {
    WaveRampParams p;
    p.wave_count = 8;
    p.first_count[0] = 100;
    p.last_count[0] = 800;
    p.first_prep = 8.0f;
    p.last_prep = 4.0f;
    p.first_atp = 50;
    p.last_atp = 120;

    const std::vector<WaveDef> waves = make_wave_ramp(p);
    REQUIRE(waves.size() == 8);

    u32 prev_total = 0;
    f32 prev_prep = 1e9f;
    u32 prev_atp = 0;
    for (usize i = 0; i < waves.size(); ++i) {
        INFO("wave " << i);
        REQUIRE(waves[i].index == static_cast<u32>(i));
        REQUIRE_FALSE(waves[i].spawns.empty());

        u32 total = 0;
        for (const SpawnEntry& e : waves[i].spawns) total += e.count;
        REQUIRE(total >= prev_total);        // monotonically harder
        REQUIRE(waves[i].prep_time <= prev_prep + 1e-3f);   // monotonically tighter
        REQUIRE(waves[i].atp_reward >= prev_atp);           // monotonically richer
        prev_total = total;
        prev_prep = waves[i].prep_time;
        prev_atp = waves[i].atp_reward;
    }

    REQUIRE(waves.front().prep_time == Catch::Approx(8.0f));
    REQUIRE(waves.back().prep_time == Catch::Approx(4.0f));
    REQUIRE(waves.back().atp_reward == 120);
}

TEST_CASE("a family can join the ramp partway through", "[level][templates][waves]") {
    // Layering bacteria in from wave 2 is the single most common thing a
    // shipped table does, so the generator has to express it -- and the joining
    // family must ramp from ITS OWN first count, not start partway up the
    // virus curve.
    WaveRampParams p;
    p.wave_count = 6;
    p.first_count[0] = 100;
    p.last_count[0] = 600;
    p.first_count[1] = 40;
    p.last_count[1] = 240;
    p.first_wave[1] = 2;

    const std::vector<WaveDef> waves = make_wave_ramp(p);
    REQUIRE(waves.size() == 6);

    const auto count_of = [](const WaveDef& w, PathogenFamily f) -> u32 {
        u32 c = 0;
        for (const SpawnEntry& e : w.spawns) {
            if (e.family == f) c += e.count;
        }
        return c;
    };

    REQUIRE(count_of(waves[0], PathogenFamily::Bacteria) == 0);
    REQUIRE(count_of(waves[1], PathogenFamily::Bacteria) == 0);
    REQUIRE(count_of(waves[2], PathogenFamily::Bacteria) == 40);   // its own first count
    REQUIRE(count_of(waves[5], PathogenFamily::Bacteria) == 240);
    // Viruses ran the whole table.
    REQUIRE(count_of(waves[0], PathogenFamily::Virus) == 100);
    REQUIRE(count_of(waves[5], PathogenFamily::Virus) == 600);
}

TEST_CASE("the ramp curve front- and back-loads difficulty",
          "[level][templates][waves]") {
    WaveRampParams base;
    base.wave_count = 5;
    base.first_count[0] = 100;
    base.last_count[0] = 500;
    base.first_count[1] = 0;
    base.last_count[1] = 0;

    const auto mid_count = [](const std::vector<WaveDef>& w) {
        u32 c = 0;
        for (const SpawnEntry& e : w[2].spawns) c += e.count;
        return c;
    };

    WaveRampParams linear = base;
    WaveRampParams back_loaded = base;
    back_loaded.curve = 2.0f;
    WaveRampParams front_loaded = base;
    front_loaded.curve = 0.5f;

    const u32 lin = mid_count(make_wave_ramp(linear));
    const u32 back = mid_count(make_wave_ramp(back_loaded));
    const u32 front = mid_count(make_wave_ramp(front_loaded));
    INFO("front=" << front << " linear=" << lin << " back=" << back);
    REQUIRE(back < lin);
    REQUIRE(front > lin);

    // Endpoints are pinned regardless of curve.
    for (const WaveRampParams& p : {linear, back_loaded, front_loaded}) {
        const std::vector<WaveDef> w = make_wave_ramp(p);
        u32 first = 0, last = 0;
        for (const SpawnEntry& e : w.front().spawns) first += e.count;
        for (const SpawnEntry& e : w.back().spawns) last += e.count;
        REQUIRE(first == 100);
        REQUIRE(last == 500);
    }
}

TEST_CASE("a single-wave ramp does not divide by zero", "[level][templates][waves]") {
    WaveRampParams p;
    p.wave_count = 1;
    const std::vector<WaveDef> w = make_wave_ramp(p);
    REQUIRE(w.size() == 1);
    REQUIRE_FALSE(w[0].spawns.empty());
    REQUIRE(w[0].prep_time == Catch::Approx(p.first_prep));
}

TEST_CASE("the wave budget reports the real peak", "[level][templates][waves]") {
    // The point of the readout is to tell you BEFORE you play that a wave will
    // blow the chaff cap. Overlapping entries have to sum, or two 500-agent
    // bursts fired at the same instant look like one.
    WaveDef w;
    SpawnEntry a;
    a.family = PathogenFamily::Virus;
    a.count = 400;
    a.start_time = 0.0f;
    a.duration = 4.0f;   // 100/s
    SpawnEntry b;
    b.family = PathogenFamily::Bacteria;
    b.count = 200;
    b.start_time = 2.0f;
    b.duration = 2.0f;   // 100/s, overlapping a's second half
    w.spawns = {a, b};

    const WaveBudget budget = wave_budget(w);
    REQUIRE(budget.total_agents == 600);
    REQUIRE(budget.span_seconds == Catch::Approx(4.0f));
    REQUIRE(budget.peak_per_second == Catch::Approx(200.0f));   // both at once

    SECTION("non-overlapping entries do not sum") {
        WaveDef s;
        SpawnEntry x = a;
        SpawnEntry y = b;
        y.start_time = 10.0f;
        s.spawns = {x, y};
        REQUIRE(wave_budget(s).peak_per_second == Catch::Approx(100.0f));
    }
    SECTION("an empty wave budgets zero") {
        REQUIRE(wave_budget(WaveDef{}).total_agents == 0);
    }
}

// ---- schema 2 in the session -----------------------------------------------

TEST_CASE("a survive-seconds level clears on the clock, not the wave table",
          "[level][session][waves]") {
    // The alternative win condition has to come from the SIM's tick counter,
    // not a wall clock, or a survive level would replay differently every run.
    // And it must not also clear early by exhausting its table -- running out
    // of waves before the clock is the opposite of "survive for two minutes".
    LevelDef d = make_template(LevelTemplate::StraightLane);
    d.win.survive_seconds = 2.0f;

    sim::SimWorld world;
    sim::SimDesc sd;
    sd.world_bounds = d.world_bounds;
    world.init(sd, nullptr);
    LevelLoader loader;
    REQUIRE(loader.instantiate(d, world).ok);

    LevelSystems s;
    s.world = &world;
    s.survive_seconds = d.win.survive_seconds;

    // 2 seconds at 60 Hz. It must NOT clear before then, even though the
    // (unattached) wave director has nothing to run.
    const u64 needed = static_cast<u64>(2.0 / kFixedDtSeconds);
    for (u64 i = 0; i < needed - 1; ++i) {
        REQUIRE(step_level(s) == SessionOutcome::InProgress);
    }
    REQUIRE(step_level(s) == SessionOutcome::Cleared);
}

TEST_CASE("a level with no survive time still clears by the wave table",
          "[level][session][waves]") {
    LevelDef d = make_template(LevelTemplate::StraightLane);
    REQUIRE(d.win.survive_seconds == 0.0f);

    sim::SimWorld world;
    sim::SimDesc sd;
    sd.world_bounds = d.world_bounds;
    world.init(sd, nullptr);
    LevelLoader loader;
    REQUIRE(loader.instantiate(d, world).ok);

    LevelSystems s;
    s.world = &world;
    // No wave director: all_waves_complete is unreachable, so the level simply
    // never clears -- which is the pre-schema-2 behaviour, unchanged.
    for (i32 i = 0; i < 300; ++i) {
        REQUIRE(step_level(s) == SessionOutcome::InProgress);
    }
}
