#include "app/App.h"

#include "core/Log.h"
#include "game/level/Level.h"
#include "platform/FileIO.h"
#include "render/Screenshot.h"

namespace immune::app {

bool App::init(const Options& options) {
    options_ = options;

    if (options.threads == 1) jobs_ = std::make_unique<JobSystem>(0u);
    else if (options.threads > 1) jobs_ = std::make_unique<JobSystem>(static_cast<u32>(options.threads - 1));
    else jobs_ = std::make_unique<JobSystem>();

    platform::WindowDesc wd;
    wd.title = "IMMUNE";
    wd.width = options.width;
    wd.height = options.height;
    wd.vsync = options.vsync;
    wd.gl_debug = options.verbose;
    if (!window_.create(wd)) {
        IMMUNE_LOG_ERROR("window creation failed: %s", window_.error().c_str());
        return false;
    }

    render::RendererDesc rd;
    rd.framebuffer_width = window_.width();
    rd.framebuffer_height = window_.height();
    rd.hot_reload_shaders = true;
    if (!renderer_.init(rd)) {
        IMMUNE_LOG_ERROR("renderer init failed: %s", renderer_.error().c_str());
        return false;
    }

    audio::AudioConfig ac;
    audio_.init(ac);
    hud_.init();
    input_.bind_defaults();

    enemies_.load_defaults();
    meta_.reset_to_new_game();
    economy_.configure(game::EconomyConfig{});

    if (!load_level(options.level)) return false;

    camera_.set_viewport(window_.width(), window_.height());
    camera_.set_bounds(sim_.desc().world_bounds);
    camera_.set_center(sim_.desc().world_bounds.center());
    camera_.set_view_height(sim_.desc().world_bounds.size().y);
    camera_.clamp_to_bounds();

    profiler_.reserve(4096);
    state_.request(GameStateId::InLevel);
    state_.apply_pending();
    running_ = true;
    return true;
}

bool App::load_level(const std::string& path) {
    game::LevelLoader loader;
    game::LevelDef level;
    if (!path.empty() && platform::file_exists(path)) {
        const auto res = loader.load_file(path, level);
        if (!res.ok) {
            IMMUNE_LOG_WARN("level load failed (%s); using the built-in test level",
                            res.error.c_str());
            level = game::LevelLoader::default_test_level();
        }
    } else {
        level = game::LevelLoader::default_test_level();
    }

    sim::SimDesc desc;
    desc.seed = options_.seed;
    desc.world_bounds = level.world_bounds;
    enemies_.apply_to_tuning(desc.chaff_tuning);
    sim_.init(desc, jobs_.get());

    const auto res = loader.instantiate(level, sim_);
    if (!res.ok) {
        IMMUNE_LOG_ERROR("level instantiation failed: %s", res.error.c_str());
        return false;
    }

    towers_.register_systems(sim_);
    enemies_.register_systems(sim_);
    waves_.start(sim_);
    state_.set_current_level_id(level.name);
    return true;
}

void App::shutdown() {
    hud_.shutdown();
    audio_.shutdown();
    renderer_.shutdown();
    window_.destroy();
    jobs_.reset();
    running_ = false;
}

void App::handle_input() {
    input_.poll(window_);
    if (input_.quit_requested() || window_.close_requested()) {
        state_.request(GameStateId::Quitting);
        running_ = false;
    }
    if (input_.action_pressed(platform::Action::Pause)) {
        clock_.set_time_scale(clock_.time_scale() > 0.0f ? 0.0f : 1.0f);
    }
    if (input_.action_pressed(platform::Action::SpeedUp)) {
        clock_.set_time_scale(clock_.time_scale() >= 2.0f ? 4.0f : 2.0f);
    }
    if (input_.action_pressed(platform::Action::SpeedDown)) {
        clock_.set_time_scale(1.0f);
    }
    if (input_.action_pressed(platform::Action::ToggleDebugOverlay)) {
        hud_.set_debug_overlay_visible(!hud_.debug_overlay_visible());
    }
    if (input_.action_pressed(platform::Action::Screenshot)) {
        render::capture_framebuffer_png("shot.png", window_.width(), window_.height());
        IMMUNE_LOG_INFO("wrote shot.png");
    }
}

void App::apply_intents(const std::vector<ui::Intent>& intents) {
    for (const auto& in : intents) {
        switch (in.kind) {
            case ui::IntentKind::PlaceTower: {
                const auto q = towers_.validate(sim_, in.tower_type, in.world_position,
                                                economy_.atp());
                if (q.valid()) {
                    const EntityId e = towers_.place(sim_, in.tower_type, q.snapped_position);
                    if (e.valid()) {
                        economy_.spend(towers_.stats(in.tower_type, 1).build_cost);
                        audio_.post(audio::AudioEvent{audio::SoundId::TowerPlace, q.snapped_position});
                    }
                } else {
                    audio_.post(audio::AudioEvent{audio::SoundId::UiInvalid, in.world_position});
                }
                break;
            }
            case ui::IntentKind::UpgradeTower: towers_.upgrade(sim_, in.entity); break;
            case ui::IntentKind::SellTower:
                economy_.credit_bounty(towers_.sell(sim_, in.entity));
                break;
            case ui::IntentKind::TriggerAbility: towers_.trigger_ability(sim_, in.entity); break;
            case ui::IntentKind::SetTimeScale: clock_.set_time_scale(in.value); break;
            case ui::IntentKind::StartWaveEarly: waves_.request_early_start(); break;
            case ui::IntentKind::QuitToMenu: state_.request(GameStateId::MainMenu); break;
            default: break;
        }
    }
}

void App::tick_sim() {
    waves_.tick(sim_, sim_.rng(), kFixedDt);
    sim_.tick(&profiler_);
    economy_.tick(kFixedDt);
}

void App::render_frame() {
    WallClock submit;
    renderer_.poll_shader_reload();
    renderer_.begin_frame(camera_, clock_.alpha());
    renderer_.submit_tissue(sim_.tissue(), sim_.sdf(), 0.0f);
    renderer_.submit_chaff(sim_.chaff(), sim_.spatial());
    renderer_.submit_entities(sim_.ecs());
    renderer_.submit_fields(sim_.damage().fields().data(), sim_.damage().fields().size());
    if (hud_.debug_overlay_visible()) renderer_.submit_flow_debug(sim_.flow());
    renderer_.end_frame();
    profiler_.record(prof_key::kRenderSubmit, submit.elapsed_ms());

    hud_.begin_frame(input_);
    intents_.clear();
    hud_.build(sim_, economy_, waves_, towers_, camera_, input_, intents_);
    hud_.render();
    apply_intents(intents_);

    window_.swap();
}

void App::enter_state(GameStateId id) {
    state_.request(id);
    state_.apply_pending();
}

int App::run() {
    while (running_) {
        WallClock frame;
        clock_.begin_frame();
        handle_input();
        state_.apply_pending();

        if (state_.sim_running()) {
            while (clock_.consume_tick()) tick_sim();
        }

        camera_.set_viewport(window_.width(), window_.height());
        renderer_.resize(window_.width(), window_.height());
        render_frame();

        const sim::SimSnapshot snap = sim_.snapshot();
        audio_.update(camera_.center(),
                      snap.total_density > 0.0f ? 1.0f : 0.0f,
                      static_cast<f32>(clock_.frame_delta()));

        profiler_.record(prof_key::kFrameTotal, frame.elapsed_ms());
    }
    return 0;
}

} // namespace immune::app
