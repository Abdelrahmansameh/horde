#include "app/App.h"

#include "app/Modes.h"
#include "game/abilities/AbilityConfigApply.h"
#include "game/enemies/EnemyConfigApply.h"
#include "game/towers/TowerMechanics.h"

#include "core/Log.h"
#include "game/level/Level.h"
#include "game/session/LevelSession.h"
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

    // Tuning first: every system below is configured from it. A config that
    // will not load is fatal rather than papered over -- the files are
    // authoritative, so running on a half-applied table would be worse than
    // not running.
    if (!load_tuning_config()) return false;

    // apply_enemy_config() calls load_defaults() itself, then overrides from
    // the file -- so this replaces the bare load_defaults() that used to be
    // here rather than following it.
    game::apply_enemy_config(enemies_, config_.enemies);
    meta_.reset_to_new_game();
    apply_tuning_config();

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

    // --exec at launch. It already worked for --screenshot; doing it here too
    // is what makes "watch the balance bot play" a command line rather than a
    // sequence of keystrokes into the gym panel:
    //     immune --level <f> --exec "autoplay on; time 8"
    // Runs after the level is loaded and the state has settled, so a command
    // that needs a world (which is most of them) has one.
    if (!options_.exec.empty()) {
        game::GymContext ctx = make_gym_context();
        const game::GymResult r = game::gym_execute_script(ctx, options_.exec);
        if (!r.ok) {
            IMMUNE_LOG_WARN("--exec failed: %s", r.message.c_str());
        } else if (!r.message.empty()) {
            IMMUNE_LOG_INFO("--exec: %s", r.message.c_str());
        }
    }

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

bool App::load_tuning_config() {
    const std::string dir = resolve_config_dir(options_);
    std::string err;
    if (!game::load_game_config(config_store_, dir, config_, err)) {
        IMMUNE_LOG_ERROR("config load failed: %s", err.c_str());
        return false;
    }
    game::bind_game_config(config_store_.registry(), config_);
    IMMUNE_LOG_INFO("tuning config loaded from %s (hash %016llx)", dir.c_str(),
                    static_cast<unsigned long long>(config_store_.hash()));
    return true;
}

void App::apply_tuning_config() {
    // Everything here is safe to re-apply at any time, which is what makes the
    // hot reload possible: each call fully overwrites the system's tuning
    // rather than adjusting it incrementally.
    game::apply_tower_config(towers_, config_.towers);
    game::apply_enemy_config(enemies_, config_.enemies);
    economy_.configure(config_.economy);
    game::apply_ability_config(abilities_, config_.abilities);
}

void App::poll_config_reload(f32 dt) {
    // Off in every deterministic mode and whenever --config pinned the
    // directory: a run that can be retuned underneath itself is not a run
    // anyone can reproduce.
    if (options_.config_pinned) return;

    constexpr f32 kPollInterval = 0.5f;
    config_poll_timer_ -= dt;
    if (config_poll_timer_ > 0.0f) return;
    config_poll_timer_ = kPollInterval;

    std::string err;
    if (!config_store_.poll_changed(err)) {
        if (!err.empty()) IMMUNE_LOG_WARN("config reload failed, keeping the last good one: %s", err.c_str());
        return;
    }
    game::GameConfig fresh;
    if (!game::parse_game_config(config_store_, fresh, err)) {
        IMMUNE_LOG_WARN("config reload failed, keeping the last good one: %s", err.c_str());
        return;
    }
    const bool capacities_changed =
        fresh.sim.capacities.max_chaff != config_.sim.capacities.max_chaff ||
        fresh.sim.capacities.max_projectiles != config_.sim.capacities.max_projectiles ||
        fresh.sim.capacities.max_swarmers != config_.sim.capacities.max_swarmers ||
        fresh.sim.globals.spatial_cell_size != config_.sim.globals.spatial_cell_size;

    config_ = std::move(fresh);
    game::bind_game_config(config_store_.registry(), config_);
    apply_tuning_config();
    IMMUNE_LOG_INFO("tuning config reloaded (hash %016llx)",
                    static_cast<unsigned long long>(config_store_.hash()));
    if (capacities_changed) {
        IMMUNE_LOG_WARN("sim capacities/cell size changed; restart the level to apply them");
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
    // Capacities and the spatial grid are fixed at init() and cannot change
    // without rebuilding the world, which is exactly why they are read here
    // rather than applied by a hot reload.
    desc.max_chaff = config_.sim.capacities.max_chaff;
    desc.max_damage_fields = config_.sim.capacities.max_damage_fields;
    desc.max_projectiles = config_.sim.capacities.max_projectiles;
    desc.max_swarmers = config_.sim.capacities.max_swarmers;
    desc.max_fluid_particles = config_.sim.capacities.max_fluid_particles;
    desc.max_combat_events = config_.sim.capacities.max_combat_events;
    desc.fluid_tuning = config_.sim.fluid;
    desc.squad_tuning = config_.sim.squads;
    desc.spatial_cell_size = config_.sim.globals.spatial_cell_size;
    desc.flow_rebake_budget_ms = config_.sim.globals.flow_rebake_budget_ms;
    desc.flow_smoothing_radius = config_.sim.globals.flow_smoothing_radius;
    desc.flow_wall_cost = config_.sim.globals.flow_wall_cost;
    desc.flow_wall_falloff = config_.sim.globals.flow_wall_falloff;
    desc.flow_wall_exponent = config_.sim.globals.flow_wall_exponent;
    enemies_.apply_to_tuning(desc.chaff_tuning);
    desc.chaff_tuning.max_replications_per_tick = config_.sim.globals.max_replications_per_tick;
    desc.chaff_tuning.max_neighbors_sampled = config_.sim.globals.max_neighbors_sampled;
    desc.chaff_tuning.ambient_drift = config_.sim.globals.ambient_drift;
    // The level gets the last word on drift: sim.json supplies the default and
    // a level that authors `ambient_drift` overrides it. That field has been
    // parsed and then ignored since the schema was written, because
    // apply_to_tuning() unconditionally stamped a global value over it.
    if (level.ambient_drift != Vec2{0.0f, 0.0f}) {
        desc.chaff_tuning.ambient_drift = level.ambient_drift;
    }
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
    // The bot plans against geometry, so the level it planned for has to
    // outlive load_level(). Any bot running for the previous level stops here.
    current_level_def_ = level;
    autoplay_enabled_ = false;

    towers_.register_systems(sim_);
    enemies_.register_systems(sim_);
    // The level owns its pressure curve outright (Level.h, AUTHORED WAVES);
    // the loader guarantees the table is non-empty, so there is nothing to
    // fall back to and no choice to make here.
    IMMUNE_LOG_INFO("level '%s' wave table: %zu waves", level.name.c_str(),
                    level.waves.size());
    waves_.set_waves(level.waves);
    waves_.start(sim_);
    state_.set_current_level_id(level.name);

    // The gym level defends its own objective by default: a run that ends
    // because a lane leaked while you were three commands into setting up an
    // experiment is pure friction, and the leak counters still record what
    // happened. Every other level gets the real loss condition back.
    gym_toggles_ = game::GymToggles{};
    gym_toggles_.hold_integrity = sim_.snapshot().objective_integrity;
    gym_toggles_.objective_invulnerable = (level.name == "gym");

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
    apply_tuning_config();
    gym_spawns_.clear();

    // The gym level is the one that exists to be driven from the panel, so it
    // brings the panel up with it; every other level leaves it as the player
    // left it.
    gym_panel_.set_level(level.name);

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
    // The gym panel eats Escape first: with it open, Escape means "close the
    // panel", not "abandon the run". Nothing else in the app can express that,
    // since the panel is not a GameStateId.
    if (gym_panel_.visible() && input_.action_pressed(platform::Action::CancelPlacement)) {
        gym_panel_.set_visible(false);
        return;
    }
    // Screenshot is checked before the UI-capture guard below: capturing the
    // frame is never a gameplay action, and the one frame you most want to
    // capture is often the one with a console or panel open on top of it.
    if (input_.action_pressed(platform::Action::Screenshot)) {
        render::capture_framebuffer_png("shot.png", window_.width(), window_.height());
        IMMUNE_LOG_INFO("wrote shot.png");
    }
    // Typing a command must not also drive the game. ui_capture_keyboard is set
    // from the previous frame's ImGui state (Hud::begin_frame), which is exactly
    // the frame whose keystrokes are being classified here.
    if (input_.ui_capture_keyboard()) return;

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
            case ui::IntentKind::UpgradeTower: {
                // Upgrades were free from Wave 3A until the balance harness
                // went looking for why upgrade_cost never showed up in any
                // spend total: upgrade() charges nothing by design and this
                // was its only caller.
                const u32 cost = towers_.upgrade_cost(sim_, in.entity);
                if (cost == 0 || !economy_.can_afford(cost)) {
                    audio_.post(audio::AudioEvent{audio::SoundId::UiInvalid, in.world_position});
                    break;
                }
                if (towers_.upgrade(sim_, in.entity) != 0) economy_.spend(cost);
                break;
            }
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
    // The order of operations lives in game/session/LevelSession.h so the
    // balance harness runs the same game this does rather than a headless
    // imitation of it. Everything App-specific stays here: the state machine,
    // and the logging that names the tick a run ended on.
    game::LevelSystems systems;
    systems.world = &sim_;
    systems.towers = &towers_;
    systems.enemies = &enemies_;
    systems.waves = &waves_;
    systems.economy = &economy_;
    systems.abilities = &abilities_;
    systems.spawns = &gym_spawns_;
    systems.toggles = &gym_toggles_;

    // The bot buys between ticks, in the same place apply_intents() puts a
    // player's clicks -- it has no path into the sim that a player lacks.
    if (autoplay_enabled_) {
        bot_.tick(sim_, towers_, economy_, sim_.snapshot().tick);
    }

    const game::SessionOutcome outcome = game::step_level(systems, &profiler_);
    // A transition is applied at the top of the NEXT frame, so the remaining
    // ticks of this frame still see InLevel. Without the pending check, a
    // fast-forwarded frame (say `time 12`, twelve ticks deep) logs "level
    // cleared" once per tick.
    if (state_.current() != GameStateId::InLevel || state_.has_pending()) return;

    if (outcome == game::SessionOutcome::ObjectiveDestroyed) {
        IMMUNE_LOG_INFO("level failed: objective integrity depleted at tick %llu",
                        static_cast<unsigned long long>(sim_.snapshot().tick));
        state_.set_outcome(LevelOutcome::ObjectiveDestroyed);
        state_.request(GameStateId::LevelFailed);
    } else if (outcome == game::SessionOutcome::Cleared) {
        IMMUNE_LOG_INFO("level cleared at tick %llu",
                        static_cast<unsigned long long>(sim_.snapshot().tick));
        state_.set_outcome(LevelOutcome::Cleared);
        state_.request(GameStateId::LevelComplete);
    }
}

game::GymContext App::make_gym_context() {
    game::GymContext ctx;
    ctx.world = level_loaded_ ? &sim_ : nullptr;
    ctx.towers = &towers_;
    ctx.enemies = &enemies_;
    ctx.waves = &waves_;
    ctx.economy = &economy_;
    ctx.abilities = &abilities_;
    ctx.spawns = &gym_spawns_;
    ctx.toggles = &gym_toggles_;
    ctx.clock = &clock_;
    ctx.camera = &camera_;
    if (level_loaded_) {
        ctx.cursor = camera_.screen_to_world(input_.mouse_pos());
        ctx.has_cursor = true;
    }

    // The tuning surface. `config set` writes through the same registry the
    // JSON loader fills and then re-applies, so a value changed in the console
    // and a value changed in the file take effect identically.
    ctx.config_get = [this](const std::string& path, std::string& out) {
        std::string err;
        if (config_store_.registry().get(path, out, err)) return true;
        out = err;
        return false;
    };
    ctx.config_set = [this](const std::string& path, const std::string& value, std::string& err) {
        if (!config_store_.registry().set(path, value, err)) return false;
        apply_tuning_config();
        return true;
    };
    ctx.config_reload = [this](std::string& err) {
        const std::string dir = resolve_config_dir(options_);
        game::GameConfig fresh;
        config::ConfigStore store;
        if (!game::load_game_config(store, dir, fresh, err)) return false;
        config_ = std::move(fresh);
        config_store_ = std::move(store);
        game::bind_game_config(config_store_.registry(), config_);
        apply_tuning_config();
        return true;
    };
    ctx.config_dump = [this](std::string& err) {
        return game::write_game_config(config_, resolve_config_dir(options_), err);
    };
    ctx.config_paths = [this]() { return config_store_.registry().field_paths(); };

    // `level <name>`: accepts a path, a file stem, or the level's display name,
    // resolved against the same list the level-select screen renders, so the
    // console and the menu can never disagree about what levels exist.
    ctx.load_level = [this](const std::string& wanted) {
        std::string path = wanted;
        if (!platform::file_exists(path)) {
            path.clear();
            for (const ui::LevelEntry& e : levels_) {
                const usize slash = e.path.find_last_of("/\\");
                const std::string file = slash == std::string::npos ? e.path : e.path.substr(slash + 1);
                const std::string stem = file.substr(0, file.rfind(".json"));
                if (wanted == e.display_name || wanted == stem || wanted == file) {
                    path = e.path;
                    break;
                }
            }
        }
        if (path.empty() || !load_level(path)) return false;
        enter_state(GameStateId::InLevel);
        return true;
    };
    ctx.restart_level = [this]() {
        if (current_level_path_.empty() || !load_level(current_level_path_)) return false;
        enter_state(GameStateId::InLevel);
        return true;
    };
    ctx.set_autoplay = [this](bool enable, const std::string& profile, std::string& err) {
        if (!enable) {
            autoplay_enabled_ = false;
            return true;
        }
        if (!level_loaded_) {
            err = "no level loaded";
            return false;
        }
        game::AutoPlayConfig cfg;
        if (!game::parse_autoplay_profile(profile, cfg.profile, cfg.single_type)) {
            err = "unknown profile '" + profile +
                  "' (greedy-cheapest | spread-coverage | save-for-tier3 | single-type:<tower>)";
            return false;
        }
        bot_.configure(cfg);
        bot_.plan(current_level_def_, lane_map_, sim_, towers_, waves_);
        if (bot_.sites().empty()) {
            err = "the planner found nowhere to build on this level";
            return false;
        }
        autoplay_enabled_ = true;
        return true;
    };
    ctx.set_overlay = [this](const std::string& name, bool on) {
        if (name == "debug") { hud_.set_debug_overlay_visible(on); return true; }
        if (name == "threat") { hud_.set_threat_overlay_visible(on); return true; }
        if (name == "squads") { hud_.set_squad_overlay_visible(on); return true; }
        return false;
    };
    return ctx;
}

void App::render_frame() {
    // Menu states run before any level exists, so every pass below would be
    // reading an uninitialised SimWorld. Draw a bare frame plus the front end
    // and return.
    if (!level_loaded_) {
        renderer_.poll_shader_reload();
        poll_config_reload(static_cast<f32>(kFixedDtSeconds));
        renderer_.begin_frame(camera_, 0.0f);
        renderer_.end_frame();
        hud_.begin_frame(input_);
        build_menus();
        // Available from the front end too: `level gym` is the fastest way in,
        // and a panel that vanished with the world would be useless exactly
        // when a level failed to load.
        game::GymContext gym = make_gym_context();
        gym_panel_.build(gym);
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
    poll_config_reload(static_cast<f32>(clock_.frame_delta()));
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
    // rendered_fields(), not fields(): the persistent cone/beam/rotor AoEs
    // have already been culled from the submission buffer by this point.
    // See DamageSystem::rendered_fields().
    renderer_.submit_fields(sim_.damage().rendered_fields().data(),
                            sim_.damage().rendered_fields().size());
    renderer_.submit_projectiles(sim_.projectiles());
    renderer_.submit_swarmers(sim_.swarmers());
    // After the other matter passes and before the additive particle layer.
    // The fluid is opaque-ish stuff that has to occlude the agents it has
    // buried, and the cosmetic spray thrown off a splash has to composite on
    // top of the surface that threw it.
    renderer_.submit_fluid(sim_.fluid(), sim_.fluid_system().draw_radius());
    // Additive first so the alpha-blended mist composites OVER the glow rather
    // than under it (vfx/Particles.h documents this ordering requirement).
    particles_.build_instances(vfx::BlendMode::Additive, particle_scratch_);
    renderer_.submit_particles(particle_scratch_.data(), particle_scratch_.size(),
                               vfx::BlendMode::Additive);
    particles_.build_instances(vfx::BlendMode::AlphaBlend, particle_scratch_);
    renderer_.submit_particles(particle_scratch_.data(), particle_scratch_.size(),
                               vfx::BlendMode::AlphaBlend);
    if (hud_.debug_overlay_visible()) renderer_.submit_flow_debug(sim_.flow());
    if (hud_.squad_overlay_visible()) renderer_.submit_squad_debug(sim_.squads());
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

    // Drawn last so it sits above the HUD and the pause/results screens, and
    // outside the state switch above so it stays usable while paused.
    game::GymContext gym = make_gym_context();
    gym_panel_.build(gym);

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
