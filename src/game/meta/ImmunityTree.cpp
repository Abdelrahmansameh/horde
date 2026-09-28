// game/meta/ImmunityTree.cpp — the Strengthen Immunity catalog and what each
// node does to a run. See ImmunityTree.h for the model.
//
// THE NUMBERS
// PROGRESSION.md §8 defers every per-level magnitude to a balance pass, so
// the per-level effects below are a first cut chosen for shape, not balance:
// each tower's lines, fully bought, land it a little past where its old tier 3
// sat (the tree's stat nodes ARE the old tier deltas, §7), the hub's global
// lines stay small so they complement per-tower investment rather than
// replace it (§6, Systemic Potency), and every capstone is a new behaviour
// rather than a bigger number. They sit in one table per branch below so a
// balance pass edits one place.
#include "game/meta/ImmunityTree.h"

#include "core/Math.h"
#include "game/config/GameConfig.h"

#include <cmath>
#include <string_view>

namespace immune::game {
namespace {

using K = TreeNodeKind;
using B = TreeBranch;

constexpr TreeNodeDef node(const char* key, const char* name, const char* effect, K kind, B branch,
                           u8 max_level, u16 cost_weight = 100,
                           AbilityId ability = AbilityId::Count) {
    TreeNodeDef d;
    d.key = key;
    d.name = name;
    d.effect = effect;
    d.kind = kind;
    d.branch = branch;
    d.ability = ability;
    d.max_level = max_level;
    d.cost_weight = cost_weight;
    return d;
}

constexpr AbilityId kCascade = AbilityId::ComplementCascadeBurst;
constexpr AbilityId kHistamine = AbilityId::HistamineFlare;
constexpr AbilityId kFever = AbilityId::FeverResponse;
constexpr AbilityId kClot = AbilityId::FibrinClot;

// In TreeNode order. The static_assert below and a unit test
// (tests/test_immunity_tree.cpp) keep the two in step.
constexpr TreeNodeDef kNodes[] = {
    // ---- Hub: economy ----
    node("hub.bone_marrow_reserve", "Bone Marrow Reserve", "+40 starting ATP", K::Economy, B::Hub, 5),
    node("hub.rapid_metabolism", "Rapid Metabolism", "+10% passive ATP/sec", K::Economy, B::Hub, 5),
    node("hub.efficient_clearance", "Efficient Clearance", "+10% ATP per density killed", K::Economy, B::Hub, 5),
    node("hub.field_requisition", "Field Requisition", "-5% tower build cost", K::Economy, B::Hub, 4, 125),
    node("hub.systemic_potency", "Systemic Potency", "+4% damage, every tower", K::Economy, B::Hub, 3, 150),
    node("hub.cellular_resilience", "Cellular Resilience", "+8% tower integrity, every tower", K::Economy, B::Hub, 4),
    node("hub.rapid_deployment", "Rapid Deployment", "+5% of cost refunded on sell", K::Economy, B::Hub, 3),
    node("hub.elite_response", "Elite Response", "+10% damage vs. elites and bosses", K::Economy, B::Hub, 4),
    node("hub.homeostasis", "Homeostasis", "-10% integrity lost per leak", K::Economy, B::Hub, 4, 125),
    node("hub.membrane_resilience", "Membrane Resilience", "-8% damage taken from pathogen attacks", K::Economy, B::Hub, 4, 125),
    // ---- Hub: abilities ----
    node("ability.complement.unlock", "Complement Cascade Burst", "Unlock: a chain of kills jumping target to target", K::AbilityRoot, B::Hub, 1, 100, kCascade),
    node("ability.complement.cooldown", "Cooldown Reduction", "-10% cooldown", K::AbilityStat, B::Hub, 3, 120, kCascade),
    node("ability.complement.potency", "Blast Potency", "+20% kill rate", K::AbilityStat, B::Hub, 3, 120, kCascade),
    node("ability.complement.chain", "Chain Links", "+2 chain hops", K::AbilityStat, B::Hub, 3, 120, kCascade),
    node("ability.histamine.unlock", "Histamine Flare", "Unlock: a soft-edged nova at a point", K::AbilityRoot, B::Hub, 1, 100, kHistamine),
    node("ability.histamine.cooldown", "Cooldown Reduction", "-10% cooldown", K::AbilityStat, B::Hub, 3, 120, kHistamine),
    node("ability.histamine.radius", "Radius", "+12% nova radius", K::AbilityStat, B::Hub, 3, 120, kHistamine),
    node("ability.histamine.potency", "Potency / Duration", "+15% kill rate and nova duration", K::AbilityStat, B::Hub, 3, 120, kHistamine),
    node("ability.fever.unlock", "Fever Response", "Unlock: every tower's reload jumps ahead", K::AbilityRoot, B::Hub, 1, 100, kFever),
    node("ability.fever.cooldown", "Cooldown Reduction", "-10% cooldown", K::AbilityStat, B::Hub, 3, 120, kFever),
    node("ability.fever.magnitude", "Buff Magnitude", "+25% reload relief", K::AbilityStat, B::Hub, 3, 120, kFever),
    node("ability.fever.duration", "Buff Duration", "+2 s of 50% faster reloads after the burst", K::AbilityStat, B::Hub, 3, 120, kFever),
    node("ability.clot.unlock", "Fibrin Clot", "Unlock: a temporary bar across the lane", K::AbilityRoot, B::Hub, 1, 100, kClot),
    node("ability.clot.cooldown", "Cooldown Reduction", "-10% cooldown", K::AbilityStat, B::Hub, 3, 120, kClot),
    node("ability.clot.duration", "Barrier Duration", "+20% seconds the clot stands", K::AbilityStat, B::Hub, 3, 120, kClot),
    node("ability.clot.width", "Barrier Width", "+6% span, +10% thickness", K::AbilityStat, B::Hub, 3, 120, kClot),
    // ---- Neutrophil ----
    node("neutrophil.unlock", "Neutrophil", "Owned from the start: the innate first responder", K::TowerRoot, B::Neutrophil, 1),
    node("neutrophil.round_damage", "Round Damage", "+15% per-shot damage", K::Stat, B::Neutrophil, 5),
    node("neutrophil.volley_cadence", "Volley Cadence", "-8% time between squad releases", K::Stat, B::Neutrophil, 4),
    node("neutrophil.trigger_rate", "Trigger Rate", "-8% swarmer volley and reload time", K::Stat, B::Neutrophil, 4),
    node("neutrophil.aggro_range", "Aggro Range", "+10% swarmer search radius", K::Stat, B::Neutrophil, 3),
    node("neutrophil.squad_size", "Squad Size", "+1 swarmer per volley", K::Stat, B::Neutrophil, 3, 130),
    node("neutrophil.accuracy", "Accuracy", "-20% shot spread", K::Stat, B::Neutrophil, 3),
    node("neutrophil.vitality", "Swarmer Vitality", "+20% swarmer health, +10% lifetime", K::Stat, B::Neutrophil, 4),
    node("neutrophil.tower_health", "Tower Health", "+20% tower integrity", K::Stat, B::Neutrophil, 4),
    node("neutrophil.capstone", "Incendiary Rounds", "Impacts leave a brief burning patch", K::Capstone, B::Neutrophil, 1),
    // ---- Cytotoxic T ----
    node("cytotoxic.unlock", "Cytotoxic T", "Unlock: latchers that ride a host and drain it", K::TowerRoot, B::CytotoxicT, 1),
    node("cytotoxic.drain", "Drain DPS", "+15% drain per second", K::Stat, B::CytotoxicT, 5),
    node("cytotoxic.attach_speed", "Attach Speed", "-25% time to lock on", K::Stat, B::CytotoxicT, 3),
    node("cytotoxic.cadence", "Deploy Cadence", "-8% time between volleys", K::Stat, B::CytotoxicT, 4),
    node("cytotoxic.search", "Search Radius", "+10% hunt radius", K::Stat, B::CytotoxicT, 3),
    node("cytotoxic.squad_size", "Squad Size", "+2 latchers per volley", K::Stat, B::CytotoxicT, 3, 130),
    node("cytotoxic.stamina", "Swarmer Speed / Lifetime", "+10% speed, +15% lifetime", K::Stat, B::CytotoxicT, 3),
    node("cytotoxic.tower_health", "Tower Health", "+20% tower integrity", K::Stat, B::CytotoxicT, 4),
    node("cytotoxic.capstone", "Apoptosis Trigger", "A kill bursts on nearby chaff; +50% drain vs. elites and bosses", K::Capstone, B::CytotoxicT, 1),
    // ---- Macrophage ----
    node("macrophage.unlock", "Macrophage", "Unlock: pseudopods that swallow the horde, a wall across the lane", K::TowerRoot, B::Macrophage, 1),
    node("macrophage.arms", "Arm Count", "+1 pseudopod tree", K::Stat, B::Macrophage, 1, 200),
    node("macrophage.grab_speed", "Extend / Latch / Pull / Recover", "-10% grab cycle time", K::Stat, B::Macrophage, 4),
    node("macrophage.captives", "Captive Capacity", "+1 enemy per pull, +0.4 cluster radius", K::Stat, B::Macrophage, 3),
    node("macrophage.cadence", "Deploy Cadence", "-8% time between releases", K::Stat, B::Macrophage, 4),
    node("macrophage.search", "Search Radius", "+10% reach for a target", K::Stat, B::Macrophage, 3),
    node("macrophage.body_count", "Body Count", "+1 body per release", K::Stat, B::Macrophage, 2, 150),
    node("macrophage.body_mass", "Body Mass", "Bodies yield less to the crowd", K::Stat, B::Macrophage, 3),
    node("macrophage.health", "Body / Tower Health", "+20% body and tower integrity", K::Stat, B::Macrophage, 4),
    node("macrophage.wall", "Wall Spacing / Body Block", "-10% gap in the lane wall", K::Stat, B::Macrophage, 3),
    node("macrophage.capstone", "Phagocytic Sustain", "Each enemy swallowed heals the tower", K::Capstone, B::Macrophage, 1),
    // ---- Goblet Cell ----
    node("goblet.unlock", "Goblet Cell", "Unlock: mucus bombs that slow what they soak", K::TowerRoot, B::GobletCell, 1),
    node("goblet.slow_strength", "Slow Strength", "Soaked targets keep 15% less speed", K::Stat, B::GobletCell, 4),
    node("goblet.splash_radius", "Splash Radius", "+15% splash size", K::Stat, B::GobletCell, 3),
    node("goblet.droplets", "Droplet Count", "+20% droplets per splash", K::Stat, B::GobletCell, 3),
    node("goblet.slow_duration", "Slow Duration", "+20% slow after leaving mucus", K::Stat, B::GobletCell, 4),
    node("goblet.weakness", "Weakening Mucus", "Slowed targets take +10% damage from every source", K::Stat, B::GobletCell, 3, 150),
    node("goblet.cadence", "Deploy Cadence", "-8% time between shots", K::Stat, B::GobletCell, 4),
    node("goblet.tower_health", "Tower Health", "+20% tower integrity", K::Stat, B::GobletCell, 4),
    node("goblet.capstone", "Anaphylactic Shock", "A slowed target's death spreads the slow to nearby chaff", K::Capstone, B::GobletCell, 1),
    // ---- Fibroblast ----
    node("fibroblast.unlock", "Fibroblast", "Unlock: builders that lay collagen scars across the lane", K::TowerRoot, B::Fibroblast, 1),
    node("fibroblast.scar_health", "Scar Health", "+20% scar integrity", K::Stat, B::Fibroblast, 5),
    node("fibroblast.reinforce", "Reinforce Rate", "+30% repair per builder", K::Stat, B::Fibroblast, 3),
    node("fibroblast.max_scars", "Max Scars", "+1 standing scar", K::Stat, B::Fibroblast, 3, 130),
    node("fibroblast.scar_size", "Scar Size", "+10% scar length and width", K::Stat, B::Fibroblast, 3),
    node("fibroblast.build_radius", "Build Radius", "+12% reach for a build site", K::Stat, B::Fibroblast, 3),
    node("fibroblast.inflammation", "Inflammation", "Scars inflame the tissue around them: allied swarmers there deal +10% damage, towers reload 10% faster", K::Stat, B::Fibroblast, 3, 150),
    node("fibroblast.cadence", "Build Cadence", "-8% time between builders", K::Stat, B::Fibroblast, 4),
    node("fibroblast.tower_health", "Tower Health", "+20% tower integrity", K::Stat, B::Fibroblast, 4),
    node("fibroblast.capstone", "Inflammatory Scarring", "Scars burn whatever presses against them", K::Capstone, B::Fibroblast, 1),
};
static_assert(sizeof(kNodes) / sizeof(kNodes[0]) == kTreeNodeCount,
              "kNodes must list every TreeNode, in TreeNode order");

// ---------------------------------------------------------------------------
// Effects. Per-level magnitudes, grouped as the catalog is.
// ---------------------------------------------------------------------------

// Hub.
constexpr u32 kStartingAtpPerLevel = 40;
constexpr f32 kIncomePerLevel = 0.10f;
constexpr f32 kBuildCostCutPerLevel = 0.05f;
constexpr f32 kPotencyPerLevel = 0.04f;
constexpr f32 kResiliencePerLevel = 0.08f;
constexpr f32 kRefundPerLevel = 0.05f;
constexpr f32 kRefundCeiling = 0.95f;
constexpr f32 kElitePerLevel = 0.10f;
constexpr f32 kLeakCutPerLevel = 0.10f;
constexpr f32 kMembranePerLevel = 0.08f;

// Abilities.
constexpr f32 kCooldownCutPerLevel = 0.10f;
constexpr f32 kCascadePotencyPerLevel = 0.20f;
constexpr u32 kCascadeBaseLinks = 8;          // the damage system's own default
constexpr u32 kCascadeLinksPerLevel = 2;
constexpr f32 kHistamineRadiusPerLevel = 0.12f;
constexpr f32 kHistaminePotencyPerLevel = 0.15f;
constexpr f32 kFeverMagnitudePerLevel = 0.25f;
constexpr f32 kFeverLingerSecondsPerLevel = 2.0f;
constexpr f32 kFeverLingerRate = 0.5f;
constexpr f32 kClotDurationPerLevel = 0.20f;
constexpr f32 kClotSpanPerLevel = 0.06f;
constexpr f32 kClotThicknessPerLevel = 0.10f;

// Shared tower lines.
constexpr f32 kCadencePerLevel = 0.08f;
constexpr f32 kTowerHealthPerLevel = 0.20f;
constexpr f32 kSearchPerLevel = 0.10f;

// Capstones. The per-unit ones land on CapstoneParams (-> SwarmerProfile), the
// world-wide ones on sim::ImmunityTuning.
constexpr f32 kIncendiaryRadius = 1.6f;
constexpr f32 kIncendiaryDamage = 3.0f;
constexpr f32 kIncendiarySeconds = 1.2f;
constexpr f32 kKillPulseRadius = 2.5f;
constexpr f32 kKillPulseDamage = 3.0f;
constexpr f32 kApoptosisNamedMult = 1.5f;
constexpr f32 kSustainHealPerKill = 4.0f;
constexpr f32 kContagionRadius = 3.5f;
constexpr f32 kContagionSeconds = 2.5f;
constexpr f32 kScarContactRate = 8.0f;
constexpr f32 kScarContactReach = 0.8f;

// World-wide lines (sim::ImmunityTuning), not capstones.
constexpr f32 kWeaknessPerLevel = 0.10f;
constexpr f32 kInflammationRadius = 8.0f;
constexpr f32 kInflammationDamagePerLevel = 0.10f;
constexpr f32 kInflammationReloadPerLevel = 0.10f;

/// 1 + per_level * level: the usual "grows" line.
f32 grow(u8 level, f32 per_level) { return 1.0f + per_level * static_cast<f32>(level); }
/// 1 - per_level * level, floored so a long line can never zero or invert a
/// value: the usual "shrinks" line (cooldowns, spreads, costs).
f32 shrink(u8 level, f32 per_level) {
    return math::max(0.1f, 1.0f - per_level * static_cast<f32>(level));
}
u32 scale_u32(u32 v, f32 m) {
    return static_cast<u32>(std::lround(static_cast<f64>(v) * static_cast<f64>(m)));
}

void apply_tower_lines(const TreeLevels& lv, TowerType type, TowerStats& st, TowerMechanics& m) {
    using N = TreeNode;
    switch (type) {
    case TowerType::Neutrophil:
        m.shooter.round_damage *= grow(lv[N::NeutrophilRoundDamage], 0.15f);
        st.fire_interval *= shrink(lv[N::NeutrophilVolleyCadence], kCadencePerLevel);
        // A swarmer's cadence is its whole magazine cycle now, so the line
        // shortens every part of it, not just the gap inside a volley.
        m.shooter.fire_interval *= shrink(lv[N::NeutrophilTriggerRate], kCadencePerLevel);
        m.shooter.gather_seconds *= shrink(lv[N::NeutrophilTriggerRate], kCadencePerLevel);
        m.shooter.reload_seconds *= shrink(lv[N::NeutrophilTriggerRate], kCadencePerLevel);
        m.swarm.search_radius *= grow(lv[N::NeutrophilAggroRange], kSearchPerLevel);
        m.swarm.release_per_shot += lv[N::NeutrophilSquadSize];
        m.shooter.round_spread *= shrink(lv[N::NeutrophilAccuracy], 0.20f);
        m.shooter.volley_cone *= shrink(lv[N::NeutrophilAccuracy], 0.20f);
        m.swarm.max_health *= grow(lv[N::NeutrophilVitality], 0.20f);
        m.swarm.lifetime *= grow(lv[N::NeutrophilVitality], 0.10f);
        st.max_health *= grow(lv[N::NeutrophilHealth], kTowerHealthPerLevel);
        break;
    case TowerType::CytotoxicT:
        m.latch.dps *= grow(lv[N::CytotoxicDrain], 0.15f);
        m.latch.attach_seconds *= shrink(lv[N::CytotoxicAttachSpeed], 0.25f);
        st.fire_interval *= shrink(lv[N::CytotoxicCadence], kCadencePerLevel);
        m.swarm.search_radius *= grow(lv[N::CytotoxicSearch], kSearchPerLevel);
        m.swarm.release_per_shot += 2u * lv[N::CytotoxicSquadSize];
        m.swarm.speed *= grow(lv[N::CytotoxicStamina], 0.10f);
        m.swarm.lifetime *= grow(lv[N::CytotoxicStamina], 0.15f);
        st.max_health *= grow(lv[N::CytotoxicHealth], kTowerHealthPerLevel);
        if (lv.owned(N::CytotoxicCapstone)) {
            m.capstone.kill_pulse_radius = kKillPulseRadius;
            m.capstone.kill_pulse_damage = kKillPulseDamage;
            m.capstone.named_damage_mult = kApoptosisNamedMult;
        }
        break;
    case TowerType::Macrophage: {
        m.arbor_grabber.arm_count += lv[N::MacrophageArms];
        const f32 cycle = shrink(lv[N::MacrophageGrabSpeed], 0.10f);
        m.arbor_grabber.extend_seconds *= cycle;
        m.arbor_grabber.latch_seconds *= cycle;
        m.arbor_grabber.pull_seconds *= cycle;
        m.arbor_grabber.recover_seconds *= cycle;
        m.arbor_grabber.max_captives += lv[N::MacrophageCaptives];
        m.arbor_grabber.cluster_radius += 0.4f * static_cast<f32>(lv[N::MacrophageCaptives]);
        st.fire_interval *= shrink(lv[N::MacrophageCadence], kCadencePerLevel);
        m.swarm.search_radius *= grow(lv[N::MacrophageSearch], kSearchPerLevel);
        m.swarm.release_per_shot += lv[N::MacrophageBodyCount];
        // Body Mass closes a third of the remaining gap to an immovable body
        // per level, so it approaches 1 without ever passing it.
        for (u8 i = 0; i < lv[N::MacrophageBodyMass]; ++i) {
            m.arbor_grabber.body_block += (1.0f - m.arbor_grabber.body_block) / 3.0f;
        }
        m.swarm.max_health *= grow(lv[N::MacrophageHealth], kTowerHealthPerLevel);
        st.max_health *= grow(lv[N::MacrophageHealth], kTowerHealthPerLevel);
        m.arbor_grabber.wall_spacing *= shrink(lv[N::MacrophageWall], 0.10f);
        if (lv.owned(N::MacrophageCapstone)) m.capstone.heal_per_kill = kSustainHealPerKill;
        break;
    }
    case TowerType::GobletCell:
        m.mucus_bomber.slow_factor *= shrink(lv[N::GobletSlowStrength], 0.15f);
        m.mucus_bomber.splash_radius *= grow(lv[N::GobletSplashRadius], 0.15f);
        m.mucus_bomber.droplets = scale_u32(m.mucus_bomber.droplets, grow(lv[N::GobletDroplets], 0.20f));
        m.mucus_bomber.slow_duration *= grow(lv[N::GobletSlowDuration], 0.20f);
        st.fire_interval *= shrink(lv[N::GobletCadence], kCadencePerLevel);
        st.max_health *= grow(lv[N::GobletHealth], kTowerHealthPerLevel);
        break;
    case TowerType::Fibroblast:
        m.builder.scar_health *= grow(lv[N::FibroblastScarHealth], 0.20f);
        m.builder.scar_reinforce *= grow(lv[N::FibroblastReinforce], 0.30f);
        m.builder.max_scars += lv[N::FibroblastMaxScars];
        m.builder.scar_half_length *= grow(lv[N::FibroblastScarSize], 0.10f);
        m.builder.scar_half_width *= grow(lv[N::FibroblastScarSize], 0.10f);
        m.builder.build_radius *= grow(lv[N::FibroblastBuildRadius], 0.12f);
        // search_radius doubles as the builder's reach (TowerSystem.cpp), so
        // the ring the HUD draws grows with the site annulus.
        m.swarm.search_radius *= grow(lv[N::FibroblastBuildRadius], 0.12f);
        st.fire_interval *= shrink(lv[N::FibroblastCadence], kCadencePerLevel);
        st.max_health *= grow(lv[N::FibroblastHealth], kTowerHealthPerLevel);
        break;
    case TowerType::Count:
        break;
    }
}

void apply_ability_lines(const TreeLevels& lv, AbilityConfig& abilities) {
    using N = TreeNode;
    AbilityTuning& cascade = abilities.ability[static_cast<u32>(kCascade)];
    cascade.cooldown_seconds *= shrink(lv[N::ComplementCooldown], kCooldownCutPerLevel);
    cascade.kill_rate *= grow(lv[N::ComplementPotency], kCascadePotencyPerLevel);
    if (lv[N::ComplementChain] > 0) {
        cascade.chain_links = kCascadeBaseLinks + kCascadeLinksPerLevel * lv[N::ComplementChain];
    }

    AbilityTuning& histamine = abilities.ability[static_cast<u32>(kHistamine)];
    histamine.cooldown_seconds *= shrink(lv[N::HistamineCooldown], kCooldownCutPerLevel);
    histamine.radius *= grow(lv[N::HistamineRadius], kHistamineRadiusPerLevel);
    histamine.kill_rate *= grow(lv[N::HistaminePotency], kHistaminePotencyPerLevel);
    histamine.field_duration *= grow(lv[N::HistaminePotency], kHistaminePotencyPerLevel);

    AbilityTuning& fever = abilities.ability[static_cast<u32>(kFever)];
    fever.cooldown_seconds *= shrink(lv[N::FeverCooldown], kCooldownCutPerLevel);
    const f32 magnitude = grow(lv[N::FeverMagnitude], kFeverMagnitudePerLevel);
    fever.fever_cooldown_relief *= magnitude;
    fever.fever_linger_seconds = kFeverLingerSecondsPerLevel * static_cast<f32>(lv[N::FeverDuration]);
    fever.fever_linger_rate = fever.fever_linger_seconds > 0.0f ? kFeverLingerRate * magnitude : 0.0f;

    AbilityTuning& clot = abilities.ability[static_cast<u32>(kClot)];
    clot.cooldown_seconds *= shrink(lv[N::ClotCooldown], kCooldownCutPerLevel);
    clot.field_duration *= grow(lv[N::ClotDuration], kClotDurationPerLevel);
    clot.barrier_half_length *= grow(lv[N::ClotWidth], kClotSpanPerLevel);
    clot.barrier_half_width *= grow(lv[N::ClotWidth], kClotThicknessPerLevel);
}

} // namespace

const TreeNodeDef& tree_node(TreeNode n) {
    const u32 i = static_cast<u32>(n);
    return kNodes[i < kTreeNodeCount ? i : 0];
}

bool find_tree_node(std::string_view key, TreeNode& out) {
    for (u32 i = 0; i < kTreeNodeCount; ++i) {
        if (key == kNodes[i].key) {
            out = static_cast<TreeNode>(i);
            return true;
        }
    }
    return false;
}

TowerType branch_tower(TreeBranch b) {
    switch (b) {
    case TreeBranch::Neutrophil: return TowerType::Neutrophil;
    case TreeBranch::CytotoxicT: return TowerType::CytotoxicT;
    case TreeBranch::Macrophage: return TowerType::Macrophage;
    case TreeBranch::GobletCell: return TowerType::GobletCell;
    case TreeBranch::Fibroblast: return TowerType::Fibroblast;
    case TreeBranch::Hub:
    case TreeBranch::Count: break;
    }
    return TowerType::Count;
}

TreeBranch tower_branch(TowerType t) {
    switch (t) {
    case TowerType::Neutrophil: return TreeBranch::Neutrophil;
    case TowerType::CytotoxicT: return TreeBranch::CytotoxicT;
    case TowerType::Macrophage: return TreeBranch::Macrophage;
    case TowerType::GobletCell: return TreeBranch::GobletCell;
    case TowerType::Fibroblast: return TreeBranch::Fibroblast;
    case TowerType::Count: break;
    }
    return TreeBranch::Hub;
}

TreeNode tower_root(TowerType t) {
    switch (t) {
    case TowerType::Neutrophil: return TreeNode::NeutrophilRoot;
    case TowerType::CytotoxicT: return TreeNode::CytotoxicRoot;
    case TowerType::Macrophage: return TreeNode::MacrophageRoot;
    case TowerType::GobletCell: return TreeNode::GobletRoot;
    case TowerType::Fibroblast: return TreeNode::FibroblastRoot;
    case TowerType::Count: break;
    }
    return TreeNode::Count;
}

TreeNode ability_root(AbilityId id) {
    switch (id) {
    case AbilityId::ComplementCascadeBurst: return TreeNode::ComplementUnlock;
    case AbilityId::HistamineFlare: return TreeNode::HistamineUnlock;
    case AbilityId::FeverResponse: return TreeNode::FeverUnlock;
    case AbilityId::FibrinClot: return TreeNode::ClotUnlock;
    case AbilityId::Count: break;
    }
    return TreeNode::Count;
}

TreeNode branch_capstone(TreeBranch b) {
    switch (b) {
    case TreeBranch::Neutrophil: return TreeNode::NeutrophilCapstone;
    case TreeBranch::CytotoxicT: return TreeNode::CytotoxicCapstone;
    case TreeBranch::Macrophage: return TreeNode::MacrophageCapstone;
    case TreeBranch::GobletCell: return TreeNode::GobletCapstone;
    case TreeBranch::Fibroblast: return TreeNode::FibroblastCapstone;
    case TreeBranch::Hub:
    case TreeBranch::Count: break;
    }
    return TreeNode::Count;
}

const char* branch_name(TreeBranch b) {
    switch (b) {
    case TreeBranch::Hub: return "Hub";
    case TreeBranch::Neutrophil: return "Neutrophil";
    case TreeBranch::CytotoxicT: return "Cytotoxic T";
    case TreeBranch::Macrophage: return "Macrophage";
    case TreeBranch::GobletCell: return "Goblet Cell";
    case TreeBranch::Fibroblast: return "Fibroblast";
    case TreeBranch::Count: break;
    }
    return "?";
}

TreeCost tree_node_cost(const MetaConfig& cfg, TreeNode n, u8 current) {
    const TreeNodeDef& d = tree_node(n);
    TreeCost c;
    switch (d.kind) {
    case TreeNodeKind::TowerRoot:
        c.antibodies = cfg.tower_unlock_antibodies;
        break;
    case TreeNodeKind::AbilityRoot:
        c.antibodies = cfg.ability_unlock_antibodies;
        break;
    case TreeNodeKind::Capstone:
        c.antibodies = cfg.capstone_antibodies;
        c.memory_cells = cfg.capstone_memory_cells;
        break;
    case TreeNodeKind::Stat:
    case TreeNodeKind::Economy:
    case TreeNodeKind::AbilityStat: {
        const u64 base = static_cast<u64>(cfg.stat_base_cost) +
                         static_cast<u64>(cfg.stat_cost_step) * current;
        c.memory_cells = static_cast<u32>((base * d.cost_weight + 50) / 100);
        break;
    }
    }
    return c;
}

u32 branch_points(const TreeLevels& levels, TreeBranch branch) {
    u32 points = 0;
    for (u32 i = 0; i < kTreeNodeCount; ++i) {
        if (kNodes[i].branch == branch && kNodes[i].kind == TreeNodeKind::Stat) {
            points += levels.level[i];
        }
    }
    return points;
}

u32 unlocked_tower_mask(const TreeLevels& levels) {
    u32 mask = 0;
    for (u32 t = 0; t < kTowerTypeCount; ++t) {
        if (levels.owned(tower_root(static_cast<TowerType>(t)))) mask |= 1u << t;
    }
    return mask;
}

u32 unlocked_ability_mask(const TreeLevels& levels) {
    u32 mask = 0;
    for (u32 a = 0; a < kAbilityCount; ++a) {
        if (levels.owned(ability_root(static_cast<AbilityId>(a)))) mask |= 1u << a;
    }
    return mask;
}

TreeEffects apply_immunity_tree(const TreeLevels& lv, GameConfig& cfg) {
    using N = TreeNode;
    TreeEffects fx;

    // ---- Towers: per-branch lines on the tier-1 row, then the hub's global
    // lines on top, then the collapse.
    const f32 cost_mult = shrink(lv[N::FieldRequisition], kBuildCostCutPerLevel);
    const f32 potency = grow(lv[N::SystemicPotency], kPotencyPerLevel);
    const f32 resilience = grow(lv[N::CellularResilience], kResiliencePerLevel);
    for (u32 t = 0; t < kTowerTypeCount; ++t) {
        TowerStats& st = cfg.towers.stats[t][0];
        TowerMechanics& m = cfg.towers.mechanics[t][0];
        apply_tower_lines(lv, static_cast<TowerType>(t), st, m);

        st.build_cost = math::max(1u, scale_u32(st.build_cost, cost_mult));
        st.max_health *= resilience;
        m.shooter.round_damage *= potency;
        m.latch.dps *= potency;
        m.bomber.burst_damage *= potency;
        m.bomber.named_damage *= potency;

        // No tiers (PROGRESSION.md §7): the boosted baseline is the only row,
        // and nothing can pay to leave it.
        st.upgrade_cost = 0;
        for (u32 tier = 1; tier < 3; ++tier) {
            cfg.towers.stats[t][tier] = st;
            cfg.towers.mechanics[t][tier] = m;
        }
    }

    // ---- Economy.
    cfg.economy.starting_atp += kStartingAtpPerLevel * lv[N::BoneMarrowReserve];
    cfg.economy.passive_income_per_second *= grow(lv[N::RapidMetabolism], kIncomePerLevel);
    cfg.economy.atp_per_density *= grow(lv[N::EfficientClearance], kIncomePerLevel);
    cfg.economy.refund_fraction =
        math::min(kRefundCeiling, cfg.economy.refund_fraction +
                                      kRefundPerLevel * static_cast<f32>(lv[N::RapidDeployment]));
    // The sell path reads the tower copy; parse_game_config keeps the two in
    // step and so must this.
    cfg.towers.globals.refund_fraction = cfg.economy.refund_fraction;

    // ---- Abilities.
    apply_ability_lines(lv, cfg.abilities);

    // ---- The rest: world-wide sim modifiers, the hostile pass, the masks.
    fx.immunity.named_damage_mult = grow(lv[N::EliteResponse], kElitePerLevel);
    fx.immunity.leak_damage =
        cfg.sim.globals.objective_damage_per_leak * shrink(lv[N::Homeostasis], kLeakCutPerLevel);
    if (lv.owned(N::NeutrophilCapstone)) {
        fx.immunity.incendiary_radius = kIncendiaryRadius;
        fx.immunity.incendiary_damage = kIncendiaryDamage * potency;
        fx.immunity.incendiary_seconds = kIncendiarySeconds;
    }
    if (lv.owned(N::GobletCapstone)) {
        fx.immunity.contagion_radius = kContagionRadius;
        fx.immunity.contagion_seconds = kContagionSeconds;
    }
    if (lv.owned(N::FibroblastCapstone)) {
        fx.immunity.scar_contact_rate = kScarContactRate * potency;
        fx.immunity.scar_contact_reach = kScarContactReach;
    }
    fx.immunity.slowed_damage_mult = grow(lv[N::GobletWeakness], kWeaknessPerLevel);
    if (lv[N::FibroblastInflammation] > 0) {
        fx.immunity.inflammation_radius = kInflammationRadius;
        fx.immunity.inflammation_damage_mult =
            grow(lv[N::FibroblastInflammation], kInflammationDamagePerLevel);
        fx.immunity.inflammation_reload_mult =
            grow(lv[N::FibroblastInflammation], kInflammationReloadPerLevel);
    }
    fx.hostile_damage_taken_mult = shrink(lv[N::MembraneResilience], kMembranePerLevel);
    fx.tower_mask = unlocked_tower_mask(lv);
    fx.ability_mask = unlocked_ability_mask(lv);
    return fx;
}

} // namespace immune::game
