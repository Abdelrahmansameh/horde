#include "app/App.h"

#include "core/Log.h"
#include "game/level/Level.h"
#include "platform/FileIO.h"
#include "render/Screenshot.h"

#include <algorithm>

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
    input_.bind_defaults();
    if (!hud_.init(window_, input_)) {
        IMMUNE_LOG_ERROR("HUD init failed (ImGui/SDL2/GL3 backend setup)");
        return false;
    }

    enemies_.load_defaults();
    meta_.reset_to_new_game();
    economy_.configure(game::EconomyConfig{});
    abilities_.load_defaults();

    // Seeded from the run seed so a replay looks the same, but stepped on its
    // own stream -- vfx never draws from the sim's Rng (see vfx/Particles.h).
    particles_.init(vfx::ParticleSystem::kDefaultCapacity, options.seed ^ 0xA5A5'5A5AULL);
    particle_scratch_.reserve(vfx::ParticleSystem::kDefaultCapacity);

    discover_levels();

    profiler_.reserve(4096);

    // An explicit --level means "play this now" (that is what the headless
    // modes and every existing launch script expect), so it skips the front
    // end. A bare launch goes to the menu.
    if (!options.level.empty()) {
        if (!load_level(options.level)) return false;
        state_.request(GameStateId::InLevel);
    } else {
        state_.request(GameStateId::MainMenu);
    }
    state_.apply_pending();
    running_ = true;
    return true;
}

void App::discover_levels() {
    levels_.clear();
    const std::string dir = platform::asset_path("levels");
    for (const std::string& path : platform::list_files(dir, ".json")) {
        game::LevelLoader loader;
        game::LevelDef def;
        if (!loader.load_file(path, def).ok) {
            IMMUNE_LOG_WARN("skipping unreadable level '%s'", path.c_str());
            continue;
        }
        ui::LevelEntry e;
        e.path = path;
        e.display_name = def.name.empty() ? path : def.name;
        e.region = def.region.empty() ? std::string("unknown") : def.region;
        // Distinct lane ids, not vessel count: a lane can be authored as
        // several chained vessel segments, and the player cares how many ways
        // in there are, not how the spline was cut up.
        std::vector<std::string> seen;
        for (const game::Vessel& v : def.vessels) {
            const std::string& id = v.lane_id.empty() ? v.id : v.lane_id;
            if (std::find(seen.begin(), seen.end(), id) == seen.end()) seen.push_back(id);
        }
        e.lane_count = static_cast<u32>(seen.size());
        levels_.push_back(std::move(e));
    }
    IMMUNE_LOG_INFO("discovered %zu level(s) in %s", levels_.size(), dir.c_str());
}

void App::build_menus() {
    const i32 w = window_.width();
    const i32 h = window_.height();
    ui::MenuResult r;

    switch (state_.current()) {
    case GameStateId::MainMenu:     r = menu_.build_main_menu(w, h); break;
    case GameStateId::LevelSelect:  r = menu_.build_level_select(levels_, w, h); break;
    case GameStateId::LevelFailed:  r = menu_.build_level_failed_screen(w, h); break;
    case GameStateId::LevelComplete: r = menu_.build_level_complete_screen(w, h); break;
    case GameStateId::Paused:        r = menu_.build_pause_menu(w, h); break;
    default: return;
    }

    switch (r.action) {
    case ui::MenuAction::OpenLevelSelect:
        state_.request(GameStateId::LevelSelect);
        break;
    case ui::MenuAction::Resume:
        state_.request(GameStateId::InLevel);
        break;
    case ui::MenuAction::Back:
        // Leaving a live/paused run from the pause menu abandons it, same as
        // Escape does from InLevel directly.
        if (state_.current() == GameStateId::Paused) {
            state_.set_outcome(LevelOutcome::Aborted);
        }
        state_.request(GameStateId::MainMenu);
        break;
    case ui::MenuAction::Quit:
        state_.request(GameStateId::Quitting);
        running_ = false;
        break;
    case ui::MenuAction::StartLevel:
        if (r.level_index < levels_.size()) {
            if (load_level(levels_[r.level_index].path)) {
                state_.request(GameStateId::InLevel);
            } else {
                // Loading failed and load_level() already logged why. Stay on
                // the level list rather than dropping into a half-built world.
                IMMUNE_LOG_ERROR("could not start level '%s'",
                                 levels_[r.level_index].path.c_str());
            }
        }
        break;
    case ui::MenuAction::RestartLevel:
        if (load_level(current_level_path_)) {
            state_.request(GameStateId::InLevel);
        } else {
            IMMUNE_LOG_ERROR("could not restart level '%s'", current_level_path_.c_str());
            state_.request(GameStateId::MainMenu);
        }
        break;
    case ui::MenuAction::None:
        break;
    }
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
    // Per-cell "which lane is this" attribution -- pure function of `level`,
    // independent of instantiate(). No renderer/HUD consumes this yet; stored
    // now so whichever wave implements per-lane hue/threat readout doesn't
    // need to re-derive it.
    lane_map_ = loader.build_lane_ownership_map(level);

    towers_.register_systems(sim_);
    enemies_.register_systems(sim_);
    // A fresh, deterministic wave table per level -- generate() only needs
    // the region name and a wave count today; per-region tuning is Wave 3A's
    // remaining scope, not something this minimal wiring blocks on.
    waves_.set_waves(game::WaveDirector::generate(level.region, 8, sim_.rng()));
    waves_.start(sim_);
    state_.set_current_level_id(level.name);

    // Camera framing moved here from init(): with a front end, a level can be
    // loaded long after startup, and each one has its own world bounds.
    camera_.set_viewport(window_.width(), window_.height());
    camera_.set_bounds(sim_.desc().world_bounds);
    camera_.set_center(sim_.desc().world_bounds.center());
    camera_.set_view_height(sim_.desc().world_bounds.size().y);
    camera_.clamp_to_bounds();

    // A fresh level must not inherit the previous one's sparks, nor a stale
    // economy/ability state from a run that already ended.
    particles_.clear();
    economy_.configure(game::EconomyConfig{});
    abilities_.load_defaults();

    level_loaded_ = true;
    current_level_path_ = path;
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
    // Escape backs out one level of the front end. From inside a level it
    // opens the pause menu rather than immediately abandoning the run; the
    // pause menu itself offers resume/restart/main-menu.
    if (input_.action_pressed(platform::Action::CancelPlacement)) {
        switch (state_.current()) {
        case GameStateId::LevelSelect:
            state_.request(GameStateId::MainMenu);
            break;
        case GameStateId::InLevel:
            state_.request(GameStateId::Paused);
            break;
        case GameStateId::Paused:
            state_.request(GameStateId::InLevel);
            break;
        case GameStateId::LevelComplete:
        case GameStateId::LevelFailed:
            state_.set_outcome(LevelOutcome::Aborted);
            state_.request(GameStateId::MainMenu);
            break;
        default:
            break;
        }
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
            case ui::IntentKind::CastAbility: {
                if (!abilities_.cast(sim_, in.ability_id, in.world_position)) {
                    audio_.post(audio::AudioEvent{audio::SoundId::UiInvalid, in.world_position});
                }
                break;
            }
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
    economy_.credit_kills(sim_.last_damage_stats().density_removed);
    economy_.credit_bounty(waves_.take_pending_atp_reward());
    abilities_.tick(kFixedDt);

    // Win/lose: checked every tick so the transition fires the moment either
    // condition becomes true, not on some later poll. Fail takes priority --
    // an integrity breach on the same tick the last wave clears is still a
    // loss, not a photo-finish win.
    const sim::SimSnapshot snap = sim_.snapshot();
    if (state_.current() == GameStateId::InLevel && snap.objective_integrity <= 0.0f) {
        IMMUNE_LOG_INFO("level failed: objective integrity depleted at tick %llu",
                        static_cast<unsigned long long>(snap.tick));
        state_.set_outcome(LevelOutcome::ObjectiveDestroyed);
        state_.request(GameStateId::LevelFailed);
    } else if (state_.current() == GameStateId::InLevel && waves_.status().all_waves_complete &&
              snap.chaff_count == 0) {
        IMMUNE_LOG_INFO("level cleared at tick %llu", static_cast<unsigned long long>(snap.tick));
        state_.set_outcome(LevelOutcome::Cleared);
        state_.request(GameStateId::LevelComplete);
    }
}

void App::render_frame() {
    // Menu states run before any level exists, so every pass below would be
    // reading an uninitialised SimWorld. Draw a bare frame plus the front end
    // and return.
    if (!level_loaded_) {
        renderer_.poll_shader_reload();
        renderer_.begin_frame(camera_, 0.0f);
        renderer_.end_frame();
        hud_.begin_frame(input_);
        build_menus();
        hud_.render();
        window_.swap();
        return;
    }

    // Drain the tick's combat events into particles, then advance them on the
    // RENDER clock. Draining here (not in tick_sim) means one drain per frame
    // regardless of how many ticks the frame consumed, which is what the sink's
    // once-per-frame contract asks for.
    const auto& events = sim_.combat_events().events();
    particles_.emit_for_events(events.data(), events.size());
    sim_.combat_events().clear();
    particles_.update(static_cast<f32>(clock_.frame_delta()), jobs_.get());

    WallClock submit;
    renderer_.poll_shader_reload();
    renderer_.begin_frame(camera_, clock_.alpha());
    // Lane identity (DESIGN.md §9.2) plus the flow field the plasma streamlines
    // follow. lane_map_ is a pure function of the level file, so this is just
    // unpacking it into render/'s game/-free view; the renderer caches the
    // texture it bakes from `owner` on the pointer.
    render::TissueDecor decor;
    decor.flow = &sim_.flow();
    if (!lane_map_.owner.empty() && !lane_map_.lane_types.empty()) {
        decor.lane_owner = lane_map_.owner.data();
        decor.lane_type = reinterpret_cast<const u8*>(lane_map_.lane_types.data());
        decor.lane_count = static_cast<u32>(lane_map_.lane_types.size());
        decor.lane_width = lane_map_.width;
        decor.lane_height = lane_map_.height;
    }
    renderer_.submit_tissue(sim_.tissue(), sim_.sdf(), 0.0f, &decor);
    renderer_.submit_chaff(sim_.chaff(), sim_.spatial());
    renderer_.submit_entities(sim_.ecs());
    renderer_.submit_fields(sim_.damage().fields().data(), sim_.damage().fields().size());
    renderer_.submit_projectiles(sim_.projectiles());
    // Additive first so the alpha-blended mist composites OVER the glow rather
    // than under it (vfx/Particles.h documents this ordering requirement).
    particles_.build_instances(vfx::BlendMode::Additive, particle_scratch_);
    renderer_.submit_particles(particle_scratch_.data(), particle_scratch_.size(),
                               vfx::BlendMode::Additive);
    particles_.build_instances(vfx::BlendMode::AlphaBlend, particle_scratch_);
    renderer_.submit_particles(particle_scratch_.data(), particle_scratch_.size(),
                               vfx::BlendMode::AlphaBlend);
    if (hud_.debug_overlay_visible()) renderer_.submit_flow_debug(sim_.flow());
    renderer_.end_frame();
    profiler_.record(prof_key::kRenderSubmit, submit.elapsed_ms());

    hud_.begin_frame(input_);
    intents_.clear();

    // For LevelFailed, LevelComplete, and Paused, show a front-end screen
    // instead of the interactive HUD over the (frozen, for Paused) game frame.
    if (state_.current() == GameStateId::LevelFailed ||
        state_.current() == GameStateId::LevelComplete ||
        state_.current() == GameStateId::Paused) {
        build_menus();
    } else {
        hud_.build(sim_, economy_, waves_, towers_, abilities_, camera_, input_, intents_);
        apply_intents(intents_);
    }

    hud_.render();
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
        if (state_.apply_pending()) {
            // Returning to the front end tears the world down so the menu
            // renders over a clean frame and a re-entered level starts fresh
            // rather than inheriting the previous run's state.
            const GameStateId now = state_.current();
            if (now == GameStateId::MainMenu || now == GameStateId::LevelSelect) {
                level_loaded_ = false;
                particles_.clear();
                clock_.set_time_scale(1.0f);
            }
        }

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
