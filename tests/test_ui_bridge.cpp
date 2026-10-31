#include "app/UiBridge.h"
#include "game/abilities/ActiveAbilities.h"
#include "game/economy/Economy.h"
#include "game/level/Level.h"
#include "game/towers/TowerSystem.h"
#include "game/wave/WaveDirector.h"
#include "gui/core/Gui.h"
#include "platform/FileIO.h"
#include "sim/SimWorld.h"
#include "ui/hud/HudScreen.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>

using namespace immune;

namespace {

struct BridgeFixture {
    game::LevelDef level;
    sim::SimWorld world;
    game::TowerSystem towers;
    game::Economy economy;
    game::WaveDirector waves;
    game::ActiveAbilitySystem abilities;
    gui::Gui gui;
    std::unique_ptr<ui::HudScreen> hud;

    BridgeFixture() {
        const auto loaded = game::LevelLoader{}.load_string(R"JSON({
          "schema":2,"name":"bridge_fixture","region":"capillary",
          "world":{"min":[0,0],"max":[128,80],"cell_size":1},
          "vessels":[
            {"id":"collector","points":[{"p":[116,4],"w":12},{"p":[116,76],"w":12}]},
            {"id":"main","points":[{"p":[4,40],"w":20},{"p":[120,40],"w":20}]},
            {"id":"upper","points":[{"p":[60,40],"w":12},{"p":[60,76],"w":12}]}
          ],
          "spawn_points":[{"id":"p0","pos":[4,40],"radius":3,"lane_id":"main"}],
          "objectives":[
            {"id":"right","pos":[116,40],"half_extents":[4,8]},
            {"id":"top","pos":[60,76],"half_extents":[4,4]}
          ],
          "squad_paths":[
            {"id":"right_path","lane_id":"main","points":[[4,40],[116,40],[120,40],[120,40]]},
            {"id":"top_path","lane_id":"main","points":[[4,40],[60,40],[60,76]]}
          ],
          "waves":[
            {"name":"first","prep_time":20,"spawns":[{"family":"virus","count":3,"duration":1,"spawn_point_id":"p0"}]},
            {"name":"second","prep_time":20,"spawns":[{"family":"bacteria","count":7,"duration":1,"spawn_point_id":"p0"}]}
          ]
        })JSON", level);
        INFO(loaded.error);
        REQUIRE(loaded.ok);
        sim::SimDesc desc;
        desc.max_chaff = 16;
        desc.world_bounds = level.world_bounds;
        desc.sim_bounds = game::level_sim_bounds(level);
        world.init(desc, nullptr);
        const auto instantiated = game::LevelLoader{}.instantiate(level, world);
        INFO(instantiated.error);
        REQUIRE(instantiated.ok);
        towers.register_systems(world);
        economy.configure(game::EconomyConfig{});
        abilities.load_defaults();
        waves.set_waves(level.waves);
        waves.set_auto_start(false);
        waves.start(world);
        REQUIRE(gui.init(gui::Gui::Assets{platform::asset_path("fonts"), platform::asset_path("ui/icons"),
                                          platform::asset_path("config/ui_theme.json")}));
        hud = std::make_unique<ui::HudScreen>(gui);
    }

    ui::HudModel model() const {
        const app::HudSources sources{&world, &economy, &waves, &towers, &abilities, &level};
        return app::make_hud_model(sources, *hud, Vec2{0, 0});
    }
};

} // namespace

TEST_CASE("HUD bridge: organ arrows follow incoming routes past a crossing collector", "[ui][hud][organ][bridge]") {
    BridgeFixture fixture;
    SECTION("authored routes, including a repeated terminal point") {}
    SECTION("vessel endpoints when authored routes are absent") {
        fixture.level.squad_paths.clear();
    }
    const auto model = fixture.model();
    REQUIRE(model.organ_points.size() == 2);
    const auto right = std::find_if(model.organ_points.begin(), model.organ_points.end(),
                                   [](const ui::HudOrganPoint& point) { return point.world.x == 116.0f; });
    const auto top = std::find_if(model.organ_points.begin(), model.organ_points.end(),
                                 [](const ui::HudOrganPoint& point) { return point.world.y == 76.0f; });
    REQUIRE(right != model.organ_points.end());
    REQUIRE(top != model.organ_points.end());
    CHECK(right->direction.x > 0.99f);
    CHECK(right->direction.y == 0.0f);
    CHECK(top->direction.x == 0.0f);
    CHECK(top->direction.y > 0.99f);
}

TEST_CASE("HUD bridge: a later wave does not supply a preview during combat", "[ui][hud][bridge]") {
    BridgeFixture fixture;
    const auto prep = fixture.model();
    REQUIRE(prep.phase == ui::HudWavePhase::Prep);
    REQUIRE(prep.preview.valid);
    CHECK(prep.preview.wave_number == 1);
    CHECK(prep.preview.total == 3);

    fixture.waves.request_early_start();
    fixture.waves.tick(fixture.world, fixture.world.rng(), kFixedDt);
    REQUIRE(fixture.waves.next_wave() != nullptr);
    const auto spawning = fixture.model();
    REQUIRE(spawning.phase == ui::HudWavePhase::Spawning);
    CHECK_FALSE(spawning.preview.valid);

    fixture.waves.tick(fixture.world, fixture.world.rng(), 2.0f);
    const auto clearing = fixture.model();
    REQUIRE(clearing.phase == ui::HudWavePhase::Clearing);
    CHECK_FALSE(clearing.preview.valid);
}
