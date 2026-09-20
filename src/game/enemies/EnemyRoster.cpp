// game/enemies/EnemyRoster.cpp — implementation. Owner: Wave 2C.
// EnemyRoster.h (declared contract) is frozen; see its header for rationale.
//
// DESIGN RULE (DESIGN.md §8.2, enforced here structurally)
// No per-family subclass, ever. Chaff behaviour is FamilyDef data + chaff flag
// bits consumed by ChaffSystem's one shared kernel (sim/chaff/ChaffSystem.cpp,
// Wave 1B). Only the three named elites below get real per-archetype ECS logic,
// and even that logic is *data* (an ArchetypeBehavior transition table plus a
// couple of small registered systems) rather than a class hierarchy.
#include "game/enemies/EnemyRoster.h"

#include "game/enemies/EnemyConfigApply.h"

#include "core/Math.h"
#include "render/ChaffBatcher.h"
#include "render/Renderer.h"
#include "sim/SimWorld.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/chaff/ChaffSystem.h"
#include "sim/chaff/HitFlash.h"
#include "sim/damage/DamageField.h"
#include "sim/ecs/AiStateMachine.h"
#include "sim/ecs/EcsWorld.h"
#include "sim/ecs/NamedAgents.h"
#include "sim/flowfield/FlowField.h"
#include "sim/spatial/SpatialHash.h"
#include "vfx/DeathVfx.h"

#include <entt/entity/registry.hpp>

#include <vector>

namespace immune::game {

namespace {

// ---------------------------------------------------------------------------
// Chaff tuning derivation helpers
// ---------------------------------------------------------------------------

/// Movement feel by speed tier (DESIGN.md §6's "tempo = speed tier" rule,
/// applied to the physics side rather than the render tempo table). A single
/// small table, not a per-family special case, so every family's tuning comes
/// from exactly two inputs: its speed tier and its behaviour flags.
struct SpeedProfile {
    f32 max_speed;
    f32 acceleration;
    f32 jitter;
};

/// The four tiers, now read from assets/config/enemies.json. Erratic
/// deliberately reads through jitter rather than raw top speed: sudden,
/// unpredictable impulses rather than a flat-out sprint. No family uses it
/// today; it stays because it is a tier, not a family trait.
SpeedProfile speed_profile(SpeedTier tier) {
    const SpeedProfileParams& p = enemy_speed_profile(tier);
    return SpeedProfile{p.max_speed, p.acceleration, p.jitter};
}

// ---------------------------------------------------------------------------
// Per-world elite registration state, cached in the ECS registry's context so
// spawn_elite()/register_systems() are idempotent no matter how many times
// they are called against the same world. Per-WORLD, not per-run: SimWorld::init()
// resets the registry context along with the systems (see sim/SimWorld.cpp), so a
// new level re-installs from scratch, which is the point. Mirrors the
// PlaceholderEliteState pattern in sim/ecs/NamedAgents.cpp.
// ---------------------------------------------------------------------------

struct RosterState {
    bool installed = false;
};

RosterState& roster_state(entt::registry& registry) {
    return registry.ctx().emplace<RosterState>();
}

// ---------------------------------------------------------------------------
// Idempotent world install, shared by register_systems() and spawn_elite().
// ---------------------------------------------------------------------------

void ensure_roster_installed(const EnemyRoster&, sim::SimWorld& world) {
    entt::registry& registry = world.ecs().registry();
    RosterState& state = roster_state(registry);
    if (state.installed) return;

    // The named-agent layer (Wave 1D) is built but nothing in app/ wires it
    // up; enemies are what own "the named-agent tier exists", so this is
    // where it gets installed.
    sim::named::install(world);

    // No elite archetypes are registered today: the roster's elite table is
    // empty pending a redesign. The archetype registry and the systems above
    // stay wired so registering one is a local change here, nothing more.
    state.installed = true;
}

} // namespace

// ---------------------------------------------------------------------------
// EnemyRoster
// ---------------------------------------------------------------------------

void EnemyRoster::load_defaults() {
    auto set = [this](PathogenFamily f, const char* name, SpeedTier speed, f32 density) {
        FamilyDef& d = families_[static_cast<u32>(f)];
        d.family = f;
        d.name = name;
        d.color = render::family_color(f);
        d.speed_tier = speed;
        d.base_density = density;
    };
    // DESIGN.md §6 table. Names mirror the sim-test schema's family strings
    // (docs/AGENT_BRIEF.md): "virus" "bacteria".
    set(PathogenFamily::Virus, "virus", SpeedTier::Fast, 0.6f);
    set(PathogenFamily::Bacteria, "bacteria", SpeedTier::Normal, 1.8f);

    // Behaviour switches, straight off the frozen header's table: Virus
    // replicates. Bacteria is a plain chaff family with no behaviour of its
    // own.
    families_[static_cast<u32>(PathogenFamily::Virus)].replicates = true;

    // The elite table is empty: every elite this roster used to ship has been
    // removed pending a redesign. spawn_elite() below fails cleanly on any id
    // while it stays that way.
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
    const EnemyConfig& cfg = enemy_config();
    for (u32 i = 0; i < kFamilyCount; ++i) {
        const FamilyDef& d = families_[i];
        const FamilyChaffParams& fc = cfg.families[i].chaff;
        sim::ChaffFamilyParams& p = tuning.family[i];
        const SpeedProfile sp = speed_profile(d.speed_tier);

        p.max_speed = sp.max_speed;
        p.acceleration = sp.acceleration;
        p.jitter = sp.jitter;
        p.base_density = d.base_density;

        // Physical footprint mirrors the renderer's silhouette so a chaff
        // agent's collision size can never disagree with what is drawn -- the
        // same "single source of truth" reasoning FamilyDef::color's comment
        // states for colour, applied to size instead. Both multipliers are
        // authorable now, so the relationship stays visible and adjustable
        // rather than buried in this function.
        const f32 silhouette = render::family_visual(d.family).silhouette;
        p.radius = silhouette * fc.radius_from_silhouette;
        p.separation_radius = p.radius * fc.separation_radius_mul;
        // Until a config is applied, the three values enemies.json now owns are
        // still DERIVED here, exactly as they always were. Without this a bare
        // EnemyRoster -- a unit test, and more importantly default_game_config()
        // generating the shipped files -- would read the empty seed and produce
        // a virus that does not replicate.
        const bool from_config = enemy_config_applied();
        p.separation_strength = from_config
                                    ? fc.separation_strength
                                    : math::min(6.0f + d.base_density * 1.5f, 12.0f);

        // The fluid-feel block. Every one of these was unreachable from any
        // data path before the config existed: struct defaults nothing wrote.
        p.alignment_radius = fc.alignment_radius;
        p.alignment_strength = fc.alignment_strength;
        p.pressure_threshold = fc.pressure_threshold;
        p.pressure_gain = fc.pressure_gain;
        p.pressure_max = fc.pressure_max;
        p.wall_restitution = fc.wall_restitution;
        p.wall_splash = fc.wall_splash;
        p.contact_spacing = fc.contact_spacing;
        p.contact_stiffness = fc.contact_stiffness;
        p.crowd_relief = fc.crowd_relief;

        p.drift_bias = from_config ? fc.drift_bias : 0.0f;
        p.replication_rate = from_config ? fc.replication_rate : (d.replicates ? 0.2f : 0.0f);
    }

    // NOTE: ambient_drift is deliberately NOT set here any more. This function
    // used to stamp a hardcoded Vec2{0.5, 0.28} over the whole tuning, which
    // silently overrode LevelDef::ambient_drift -- a field the level schema has
    // parsed and then discarded since it was written. sim.json now supplies the
    // default and the level file overrides it, both applied by the caller.
}

EntityId EnemyRoster::spawn_elite(sim::SimWorld& world, u16 elite_id, Vec2 pos) const {
    const EliteDef* def = nullptr;
    for (const auto& e : elites_) {
        if (e.id == elite_id) { def = &e; break; }
    }
    if (def == nullptr) return EntityId{};

    ensure_roster_installed(*this, world);
    (void)pos;

    // No EliteDef maps to a registered archetype today (the roster's elite
    // table is empty). Returning an invalid id is the same failure the old
    // code produced for an unknown elite name, so every caller already
    // handles it.
    return EntityId{};
}

void EnemyRoster::register_systems(sim::SimWorld& world) {
    ensure_roster_installed(*this, world);
}

// ---------------------------------------------------------------------------
// Config application (game/enemies/EnemyConfigApply.h)
// ---------------------------------------------------------------------------

namespace {

/// The live tuning apply_to_tuning() and speed_profile() read.
///
/// It starts out holding exactly the values this file used to hardcode, so a
/// roster that never sees enemies.json behaves as it always did -- and so
/// default_game_config() can read the shipped defaults back out of here
/// instead of restating them.
EnemyConfig& mutable_enemy_config() {
    static EnemyConfig cfg = [] {
        EnemyConfig seed;
        seed.speed_tiers[static_cast<u32>(SpeedTier::Slow)] =
            SpeedProfileParams{5.625f, 20.25f, 0.12f};
        seed.speed_tiers[static_cast<u32>(SpeedTier::Normal)] =
            SpeedProfileParams{10.125f, 36.0f, 0.30f};
        seed.speed_tiers[static_cast<u32>(SpeedTier::Fast)] =
            SpeedProfileParams{16.875f, 63.0f, 0.45f};
        seed.speed_tiers[static_cast<u32>(SpeedTier::Erratic)] =
            SpeedProfileParams{12.375f, 45.0f, 1.00f};

        // How each family fights back (sim/hostile). The virus is the
        // grappler: cheap, replicating, and every one that touches a cell
        // hangs on and feeds -- weak alone, lethal by the dozen, which is what
        // a replicating family should be. The bacterium is the big slow one,
        // and it burns: nothing has to touch it, standing near it is enough,
        // so a lane full of them is a lane the player's units cannot
        // loiter in. Neither is meant to erase a tower on its own; both are
        // meant to make "in the lane" a bet.
        {
            sim::HostileFamilyParams& virus = seed.families[static_cast<u32>(PathogenFamily::Virus)].attack;
            virus.latch_dps = 2.0f;
            virus.latch_reach = 0.4f;
            virus.latch_cap_swarmer = 3u;
            virus.latch_cap_tower = 16u;
            virus.latch_cap_scar = 40u;
            virus.latch_speed = 30.0f;
            virus.latch_ease_distance = 1.2f;
            virus.latch_ease_power = 3.0f;
            virus.aura_dps = 0.0f;
            virus.aura_radius = 0.0f;
            sim::HostileFamilyParams& bacteria = seed.families[static_cast<u32>(PathogenFamily::Bacteria)].attack;
            bacteria.latch_dps = 0.0f;
            bacteria.latch_reach = 0.0f;
            bacteria.latch_cap_swarmer = 0u;
            bacteria.latch_cap_tower = 0u;
            bacteria.latch_cap_scar = 0u;
            bacteria.latch_speed = 0.0f;
            bacteria.latch_ease_distance = 0.0f;
            bacteria.latch_ease_power = 0.0f;
            bacteria.aura_dps = 3.0f;
            bacteria.aura_radius = 4.0f;
        }
        return seed;
    }();
    return cfg;
}

bool g_enemy_config_applied = false;

} // namespace

const EnemyConfig& enemy_config() { return mutable_enemy_config(); }

bool enemy_config_applied() { return g_enemy_config_applied; }

sim::HostileTuning hostile_tuning(const HostileGlobals& globals) {
    sim::HostileTuning t;
    const EnemyConfig& cfg = mutable_enemy_config();
    for (u32 i = 0; i < kFamilyCount; ++i) t.family[i] = cfg.families[i].attack;
    t.enabled = globals.enabled;
    t.max_attackers = globals.max_attackers;
    t.max_latch_events = globals.max_latch_events;
    return t;
}

const SpeedProfileParams& enemy_speed_profile(SpeedTier tier) {
    const u32 i = static_cast<u32>(tier);
    return mutable_enemy_config().speed_tiers[i < 4u ? i : 1u];
}

void apply_enemy_config(EnemyRoster& roster, const EnemyConfig& cfg) {
    mutable_enemy_config() = cfg;
    g_enemy_config_applied = true;

    // render/ cannot see game/, so the family look has to be pushed down.
    // Do this BEFORE load_defaults(), which reads render::family_color back
    // out into FamilyDef::color.
    for (u32 i = 0; i < kFamilyCount; ++i) {
        const auto family = static_cast<PathogenFamily>(i);
        const FamilyVisualParams& v = cfg.families[i].visual;
        render::set_family_visual(family, render::FamilyVisual{v.silhouette, v.tempo, v.wobble});
        render::set_family_color(family, v.color);
        // Same push-down, one layer further out: vfx/ cannot see game/ either,
        // and the death burst is authored per family alongside the look it
        // comes out of. See vfx/DeathVfx.h.
        vfx::set_family_death_vfx(family, cfg.families[i].death_vfx);
        // And once more, one layer DOWN rather than up: the hit flash is
        // per-agent state, so its table lives at the bottom of sim/ where both
        // the damage path that writes it and the batcher that draws it can
        // reach it. See sim/chaff/HitFlash.h.
        sim::set_family_hit_flash(family, cfg.families[i].hit_flash);
        sim::set_family_replication_split(family, cfg.families[i].replication_split);
        // Back up to render/ for the feeding animation, which nothing in sim
        // consumes. See render/LatchThrob.h.
        render::set_family_latch_throb(family, cfg.families[i].latch_throb);
    }

    roster.load_defaults();

    for (u32 i = 0; i < kFamilyCount; ++i) {
        const FamilyConfig& fc = cfg.families[i];
        FamilyDef& d = roster.families_[i];
        d.speed_tier = fc.speed_tier;
        d.base_density = fc.behavior.base_density;
        d.replicates = fc.behavior.replicates;
    }

    // Elites are matched by id, not by position: reordering the JSON array
    // must not silently reassign one archetype's health to another. An id the
    // roster does not know is ignored rather than fatal -- the behaviour code
    // for it would not exist either.
    for (const EliteConfig& ec : cfg.elites) {
        for (EliteDef& def : roster.elites_) {
            if (def.id != ec.id) continue;
            def.max_health = ec.stats.max_health;
            def.armor = ec.stats.armor;
            def.speed = ec.stats.speed;
            def.sprite_size = ec.stats.sprite_size;
            def.ability_cooldown = ec.stats.ability_cooldown;
            def.telegraph_duration = ec.stats.telegraph_duration;
            def.atp_bounty = ec.stats.atp_bounty;
            break;
        }
    }
}

} // namespace immune::game
