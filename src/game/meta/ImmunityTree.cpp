// game/meta/ImmunityTree.cpp — the Strengthen Immunity catalog and what each
// node does to a run. See ImmunityTree.h for the model.
//
// Each purchased rank adds the same amount relative to the unmodified
// baseline. Long lines use separate stages and distinct effects have their
// own nodes; upgrades to the same stat add rather than compound.
#include "game/meta/ImmunityTree.h"

#include "core/Math.h"
#include "game/config/GameConfig.h"

#include <array>
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
constexpr AbilityId kFever = AbilityId::FeverResponse;
constexpr AbilityId kClot = AbilityId::FibrinClot;

// In TreeNode order. The static_assert below and a unit test
// (tests/test_immunity_tree.cpp) keep the two in step.
constexpr TreeNodeDef kCatalog[] = {
    // ---- Hub: economy ----
    node("hub.bone_marrow_reserve", "Bone Marrow Reserve", "+40 starting ATP", K::Economy, B::Hub, 5),
    node("hub.efficient_clearance", "Efficient Clearance", "+10% ATP from kills", K::Economy, B::Hub, 5),
    node("hub.field_requisition", "Hematopoiesis", "-7% cell placement cost", K::Economy, B::Hub, 3, 125),
    node("hub.systemic_potency", "Cytokine Signaling", "+6% work rate (except Goblet Cells)", K::Economy, B::Hub, 3, 150),
    node("hub.elite_response", "Emergency Reserves", "+15% health for every cell", K::Economy, B::Hub, 4),
    node("hub.homeostasis", "Homeostasis", "-10% integrity lost per leak", K::Economy, B::Hub, 4, 125),
    node("hub.membrane_resilience", "Membrane Resilience", "-8% damage from pathogens", K::Economy, B::Hub, 4, 125),
    // ---- Hub: abilities ----
    node("ability.complement.unlock", "Complement Cascade Burst", "Unlock: ruptures a target, then chains to others", K::AbilityRoot, B::Hub, 1, 100, kCascade),
    node("ability.complement.cooldown", "Cooldown Reduction", "-8% base cooldown", K::AbilityStat, B::Hub, 5, 120, kCascade),
    node("ability.complement.chain", "Extended Cascade", "+2 cascade targets, up to 16", K::AbilityStat, B::Hub, 4, 120, kCascade),
    node("ability.fever.unlock", "Fever Response", "Unlock: heals all deployed cells", K::AbilityRoot, B::Hub, 1, 100, kFever),
    node("ability.fever.cooldown", "Cooldown Reduction", "-8% base cooldown", K::AbilityStat, B::Hub, 5, 120, kFever),
    node("ability.fever.magnitude", "Healing Strength", "+25% base healing", K::AbilityStat, B::Hub, 5, 120, kFever),
    node("ability.clot.unlock", "Fibrin Clot", "Unlock: creates a temporary barrier", K::AbilityRoot, B::Hub, 1, 100, kClot),
    node("ability.clot.cooldown", "Cooldown Reduction", "-8% base cooldown", K::AbilityStat, B::Hub, 5, 120, kClot),
    node("ability.clot.duration", "Barrier Duration", "+20% base barrier duration", K::AbilityStat, B::Hub, 5, 120, kClot),
    // ---- Neutrophil ----
    node("neutrophil.round_damage", "Degranulation Cycle", "+10% granule regrowth rate", K::Stat, B::Neutrophil, 5, 150),
    node("neutrophil.aggro_range", "Chemotaxis", "+10% base sensing range", K::Stat, B::Neutrophil, 5),
    node("neutrophil.squad_size", "Granule Capacity", "+1 granule", K::Stat, B::Neutrophil, 5, 130),
    node("neutrophil.accuracy", "Directed Release", "-10% base spread", K::Stat, B::Neutrophil, 5),
    node("neutrophil.vitality", "Cell Vitality", "+9 base health", K::Stat, B::Neutrophil, 5),
    node("neutrophil.capstone", "Granule Priming", "+25% base granule damage", K::Capstone, B::Neutrophil, 1),
    // ---- Cytotoxic T ----
    node("cytotoxic.unlock", "Cytotoxic T", "Unlock: binds to and damages pathogens", K::TowerRoot, B::CytotoxicT, 1),
    node("cytotoxic.drain", "Perforin Potency", "+15% base bound damage", K::Stat, B::CytotoxicT, 5),
    node("cytotoxic.attach_speed", "Receptor Affinity", "+25% binding rate", K::Stat, B::CytotoxicT, 3),
    node("cytotoxic.search", "Search Radius", "+10% base hunt radius", K::Stat, B::CytotoxicT, 5),
    node("cytotoxic.stamina", "Cell Stamina", "+10% base movement speed", K::Stat, B::CytotoxicT, 5),
    node("cytotoxic.tower_health", "Cell Resilience", "+10 base health", K::Stat, B::CytotoxicT, 5),
    node("cytotoxic.capstone", "Rapid Pursuit", "+100% base cell movement speed", K::Capstone, B::CytotoxicT, 1),
    // ---- Macrophage ----
    node("macrophage.unlock", "Macrophage", "Unlock: engulfs enemies and blocks the lane", K::TowerRoot, B::Macrophage, 1),
    node("macrophage.arms", "Cluster Intake", "+2 enemies per pull", K::Stat, B::Macrophage, 1, 200),
    node("macrophage.grab_speed", "Pseudopod Speed", "+10% pseudopod work rate", K::Stat, B::Macrophage, 5),
    node("macrophage.captives", "Phagosome Capacity", "+1 enemy per pull", K::Stat, B::Macrophage, 3),
    node("macrophage.search", "Search Radius", "+10% base target reach", K::Stat, B::Macrophage, 5),
    node("macrophage.health", "Body Health", "+20% base health", K::Stat, B::Macrophage, 5),
    node("macrophage.wall", "Defensive Membrane", "+1 percentage point of blocking", K::Stat, B::Macrophage, 5),
    node("macrophage.capstone", "Phagocytic Nourishment", "Engulfing enemies heals the cell", K::Capstone, B::Macrophage, 1),
    // ---- Goblet Cell ----
    node("goblet.unlock", "Goblet Cell", "Unlock: coats enemies in slowing mucus", K::TowerRoot, B::GobletCell, 1),
    node("goblet.slow_strength", "Slow Strength", "-10% base speed after slowing", K::Stat, B::GobletCell, 5),
    node("goblet.splash_radius", "Splash Radius", "+15% base splash radius", K::Stat, B::GobletCell, 5),
    node("goblet.slow_duration", "Slow Duration", "+20% base slow duration", K::Stat, B::GobletCell, 5),
    node("goblet.weakness", "Weakening Mucus", "+10% damage to slowed enemies", K::Stat, B::GobletCell, 5, 150),
    node("goblet.tower_health", "Cell Resilience", "+10 base health", K::Stat, B::GobletCell, 5),
    node("goblet.capstone", "Clinging Mucus", "Slowed enemies spread mucus when they die", K::Capstone, B::GobletCell, 1),
    // ---- Fibroblast ----
    node("fibroblast.unlock", "Fibroblast", "Unlock: builds walls to block the horde", K::TowerRoot, B::Fibroblast, 1),
    node("fibroblast.scar_health", "Scar Health", "+20% base scar integrity", K::Stat, B::Fibroblast, 5),
    node("fibroblast.reinforce", "Reinforce Rate", "+30% base repair", K::Stat, B::Fibroblast, 5),
    node("fibroblast.scar_size", "Scar Size", "+10% base scar dimensions", K::Stat, B::Fibroblast, 5),
    node("fibroblast.build_radius", "Territory", "+12% base build reach", K::Stat, B::Fibroblast, 5),
    node("fibroblast.inflammation", "Inflammation", "+10% ally damage near scars", K::Stat, B::Fibroblast, 5, 150),
    node("fibroblast.tower_health", "Cell Resilience", "+20% base health", K::Stat, B::Fibroblast, 5),
    node("fibroblast.build_rate", "Rapid Fibrosis", "+12% construction rate", K::Stat, B::Fibroblast, 5),
    node("fibroblast.scar_count", "Scar Network", "+1 standing wall per Fibroblast", K::Stat, B::Fibroblast, 4, 150),
    node("fibroblast.capstone", "Fibrotic Bulwark", "+100% base scar length and width", K::Capstone, B::Fibroblast, 1),
    node("hub.adaptive_memory", "Adaptive Memory", "+6% base cell health", K::Economy, B::Hub, 5, 200),
    // Continuation stages retain the same effects and price progression.
    node("hub.bone_marrow_reserve.stage2", "Bone Marrow Reserve II", "+40 starting ATP", K::Economy, B::Hub, 3),
    node("hub.efficient_clearance.stage2", "Efficient Clearance II", "+10% ATP from kills", K::Economy, B::Hub, 3),
    node("ability.complement.cooldown.stage2", "Cooldown Reduction II", "-8% base cooldown", K::AbilityStat, B::Hub, 1, 120, kCascade),
    node("ability.fever.cooldown.stage2", "Cooldown Reduction II", "-8% base cooldown", K::AbilityStat, B::Hub, 1, 120, kFever),
    node("ability.fever.magnitude.stage2", "Healing Strength II", "+25% base healing", K::AbilityStat, B::Hub, 1, 120, kFever),
    node("ability.clot.cooldown.stage2", "Cooldown Reduction II", "-8% base cooldown", K::AbilityStat, B::Hub, 1, 120, kClot),
    node("ability.clot.duration.stage2", "Barrier Duration II", "+20% base barrier duration", K::AbilityStat, B::Hub, 1, 120, kClot),
    node("neutrophil.round_damage.stage2", "Degranulation Cycle II", "+10% granule regrowth rate", K::Stat, B::Neutrophil, 5, 150),
    node("neutrophil.round_damage.stage3", "Degranulation Cycle III", "+10% granule regrowth rate", K::Stat, B::Neutrophil, 5, 150),
    node("neutrophil.round_damage.stage4", "Degranulation Cycle IV", "+10% granule regrowth rate", K::Stat, B::Neutrophil, 5, 150),
    node("neutrophil.aggro_range.stage2", "Chemotaxis II", "+10% base sensing range", K::Stat, B::Neutrophil, 1),
    node("neutrophil.squad_size.stage2", "Granule Capacity II", "+1 granule", K::Stat, B::Neutrophil, 1, 130),
    node("neutrophil.accuracy.stage2", "Directed Release II", "-10% base spread", K::Stat, B::Neutrophil, 1),
    node("neutrophil.vitality.stage2", "Cell Vitality II", "+9 base health", K::Stat, B::Neutrophil, 5),
    node("cytotoxic.drain.stage2", "Perforin Potency II", "+15% base bound damage", K::Stat, B::CytotoxicT, 5),
    node("cytotoxic.drain.stage3", "Perforin Potency III", "+15% base bound damage", K::Stat, B::CytotoxicT, 5),
    node("cytotoxic.drain.stage4", "Perforin Potency IV", "+15% base bound damage", K::Stat, B::CytotoxicT, 5),
    node("cytotoxic.search.stage2", "Search Radius II", "+10% base hunt radius", K::Stat, B::CytotoxicT, 1),
    node("cytotoxic.stamina.stage2", "Cell Stamina II", "+10% base movement speed", K::Stat, B::CytotoxicT, 3),
    node("cytotoxic.tower_health.stage2", "Cell Resilience II", "+10 base health", K::Stat, B::CytotoxicT, 5),
    node("macrophage.grab_speed.stage2", "Pseudopod Speed II", "+10% pseudopod work rate", K::Stat, B::Macrophage, 5),
    node("macrophage.grab_speed.stage3", "Pseudopod Speed III", "+10% pseudopod work rate", K::Stat, B::Macrophage, 5),
    node("macrophage.grab_speed.stage4", "Pseudopod Speed IV", "+10% pseudopod work rate", K::Stat, B::Macrophage, 1),
    node("macrophage.search.stage2", "Search Radius II", "+10% base target reach", K::Stat, B::Macrophage, 1),
    node("macrophage.health.stage2", "Body Health II", "+20% base health", K::Stat, B::Macrophage, 5),
    node("macrophage.wall.stage2", "Defensive Membrane II", "+1 percentage point of blocking", K::Stat, B::Macrophage, 1),
    node("goblet.slow_strength.stage2", "Slow Strength II", "-10% base speed after slowing", K::Stat, B::GobletCell, 1),
    node("goblet.splash_radius.stage2", "Splash Radius II", "+15% base splash radius", K::Stat, B::GobletCell, 1),
    node("goblet.slow_duration.stage2", "Slow Duration II", "+20% base slow duration", K::Stat, B::GobletCell, 3),
    node("goblet.weakness.stage2", "Weakening Mucus II", "+10% damage to slowed enemies", K::Stat, B::GobletCell, 3, 150),
    node("goblet.tower_health.stage2", "Cell Resilience II", "+10 base health", K::Stat, B::GobletCell, 5),
    node("fibroblast.scar_health.stage2", "Scar Health II", "+20% base scar integrity", K::Stat, B::Fibroblast, 5),
    node("fibroblast.reinforce.stage2", "Reinforce Rate II", "+30% base repair", K::Stat, B::Fibroblast, 1),
    node("fibroblast.scar_size.stage2", "Scar Size II", "+10% base scar dimensions", K::Stat, B::Fibroblast, 1),
    node("fibroblast.build_radius.stage2", "Territory II", "+12% base build reach", K::Stat, B::Fibroblast, 1),
    node("fibroblast.inflammation.stage2", "Inflammation II", "+10% ally damage near scars", K::Stat, B::Fibroblast, 1, 150),
    node("fibroblast.tower_health.stage2", "Cell Resilience II", "+20% base health", K::Stat, B::Fibroblast, 5),
    node("fibroblast.build_rate.stage2", "Rapid Fibrosis II", "+12% construction rate", K::Stat, B::Fibroblast, 3),
    node("hub.adaptive_memory.stage2", "Adaptive Memory II", "+6% base cell health", K::Economy, B::Hub, 5, 200),
    node("hub.adaptive_memory.stage3", "Adaptive Memory III", "+6% base cell health", K::Economy, B::Hub, 5, 200),
    node("hub.adaptive_memory.stage4", "Adaptive Memory IV", "+6% base cell health", K::Economy, B::Hub, 5, 200),
    // Independent effects split from the original combined upgrades.
    node("neutrophil.release", "Rapid Release", "+5% release rate", K::Stat, B::Neutrophil, 5, 150),
    node("neutrophil.release.stage2", "Rapid Release II", "+5% release rate", K::Stat, B::Neutrophil, 5, 150),
    node("neutrophil.release.stage3", "Rapid Release III", "+5% release rate", K::Stat, B::Neutrophil, 5, 150),
    node("neutrophil.release.stage4", "Rapid Release IV", "+5% release rate", K::Stat, B::Neutrophil, 5, 150),
    node("neutrophil.granule_damage", "Granule Damage", "+5% granule damage", K::Stat, B::Neutrophil, 5, 150),
    node("neutrophil.granule_damage.stage2", "Granule Damage II", "+5% granule damage", K::Stat, B::Neutrophil, 3, 150),
    node("neutrophil.speed", "Cell Speed", "+10% movement speed", K::Stat, B::Neutrophil, 5, 100),
    node("neutrophil.speed.stage2", "Cell Speed II", "+10% movement speed", K::Stat, B::Neutrophil, 5, 100),
    node("cytotoxic.stamina_health", "Cell Vitality", "+9 base health", K::Stat, B::CytotoxicT, 5, 100),
    node("cytotoxic.stamina_health.stage2", "Cell Vitality II", "+9 base health", K::Stat, B::CytotoxicT, 3, 100),
    node("macrophage.intake_radius", "Cluster Radius", "+0.6 capture radius", K::Stat, B::Macrophage, 1, 200),
    node("macrophage.captives_radius", "Capture Radius", "+0.4 capture radius", K::Stat, B::Macrophage, 3, 100),
    node("macrophage.grab_reach", "Pseudopod Reach", "+3% reach", K::Stat, B::Macrophage, 5, 100),
    node("macrophage.grab_reach.stage2", "Pseudopod Reach II", "+3% reach", K::Stat, B::Macrophage, 5, 100),
    node("macrophage.grab_reach.stage3", "Pseudopod Reach III", "+3% reach", K::Stat, B::Macrophage, 2, 100),
    node("macrophage.mobility", "Cell Mobility", "+3% movement speed", K::Stat, B::Macrophage, 5, 100),
    node("macrophage.mobility.stage2", "Cell Mobility II", "+3% movement speed", K::Stat, B::Macrophage, 5, 100),
    node("macrophage.mobility.stage3", "Cell Mobility III", "+3% movement speed", K::Stat, B::Macrophage, 2, 100),
    node("macrophage.wall_health", "Reinforced Body", "+20% health", K::Stat, B::Macrophage, 5, 100),
    node("macrophage.wall_health.stage2", "Reinforced Body II", "+20% health", K::Stat, B::Macrophage, 1, 100),
    node("fibroblast.regrowth", "Inflammatory Regrowth", "+10% ally granule regrowth near scars", K::Stat, B::Fibroblast, 5, 150),
    node("fibroblast.regrowth.stage2", "Inflammatory Regrowth II", "+10% ally granule regrowth near scars", K::Stat, B::Fibroblast, 1, 150),
    node("hub.adaptive_work", "Adaptive Work", "+4% work rate (except Goblet Cells)", K::Economy, B::Hub, 5, 200),
    node("hub.adaptive_work.stage2", "Adaptive Work II", "+4% work rate (except Goblet Cells)", K::Economy, B::Hub, 5, 200),
    node("hub.adaptive_work.stage3", "Adaptive Work III", "+4% work rate (except Goblet Cells)", K::Economy, B::Hub, 5, 200),
    node("hub.adaptive_work.stage4", "Adaptive Work IV", "+4% work rate (except Goblet Cells)", K::Economy, B::Hub, 5, 200),
    node("cytotoxic.cytokine_drain", "Cytokine Potency", "+6% bound damage", K::Stat, B::CytotoxicT, 3, 150),
    node("goblet.cytokine_speed", "Cytokine Mobility", "+6% movement speed", K::Stat, B::GobletCell, 3, 150),
    node("cytotoxic.adaptive_drain", "Adaptive Potency", "+4% bound damage", K::Stat, B::CytotoxicT, 5, 200),
    node("cytotoxic.adaptive_drain.stage2", "Adaptive Potency II", "+4% bound damage", K::Stat, B::CytotoxicT, 5, 200),
    node("cytotoxic.adaptive_drain.stage3", "Adaptive Potency III", "+4% bound damage", K::Stat, B::CytotoxicT, 5, 200),
    node("cytotoxic.adaptive_drain.stage4", "Adaptive Potency IV", "+4% bound damage", K::Stat, B::CytotoxicT, 5, 200),
    node("goblet.adaptive_speed", "Adaptive Mobility", "+4% movement speed", K::Stat, B::GobletCell, 5, 200),
    node("goblet.adaptive_speed.stage2", "Adaptive Mobility II", "+4% movement speed", K::Stat, B::GobletCell, 5, 200),
    node("goblet.adaptive_speed.stage3", "Adaptive Mobility III", "+4% movement speed", K::Stat, B::GobletCell, 5, 200),
    node("goblet.adaptive_speed.stage4", "Adaptive Mobility IV", "+4% movement speed", K::Stat, B::GobletCell, 5, 200),
};
static_assert(sizeof(kCatalog) / sizeof(kCatalog[0]) == kTreeNodeCount,
              "kCatalog must list every TreeNode, in TreeNode order");

// ---------------------------------------------------------------------------
// The tree's shape: each node's parent. The Strengthen Immunity screen lays
// it out radially (assets/ui/tree_layout.json, written by
// tools/gen_tree_layout.py): Degranulation Cycle is the root and centre.
// Nine root spokes keep economy, ordinary starter upgrades, cell discovery
// and the Neutrophil capstone directly accessible. Abilities and the other
// capstones grow from their related cell root. Children are clockwise.
// ---------------------------------------------------------------------------

struct Edge {
    TreeNode child;
    TreeNode parent;
};

using N = TreeNode;
constexpr Edge kEdges[] = {
    // Degranulation Cycle is the first purchase; one rank opens the spokes.
    {N::BoneMarrowReserve, N::NeutrophilRoundDamage},
    {N::CytotoxicRoot, N::NeutrophilRoundDamage},
    {N::MacrophageRoot, N::NeutrophilRoundDamage},
    {N::NeutrophilSquadSize, N::NeutrophilRoundDamage},
    {N::NeutrophilAccuracy, N::NeutrophilRoundDamage},
    {N::NeutrophilAggroRange, N::NeutrophilRoundDamage},
    {N::GobletRoot, N::NeutrophilRoundDamage},
    {N::FibroblastRoot, N::NeutrophilRoundDamage},

    // A single economy branch, without cell or ability unlocks mixed into it.
    {N::EfficientClearance, N::BoneMarrowReserve},
    {N::FieldRequisition, N::BoneMarrowReserve},
    {N::SystemicPotency, N::EfficientClearance},
    {N::Homeostasis, N::EfficientClearance},
    {N::EliteResponse, N::FieldRequisition},
    {N::MembraneResilience, N::FieldRequisition},
    {N::AdaptiveMemory, N::SystemicPotency},
    // Macrophage.
    {N::MacrophageGrabSpeed, N::MacrophageRoot},
    {N::MacrophageCaptives, N::MacrophageRoot},
    {N::MacrophageHealth, N::MacrophageRoot},
    {N::MacrophageArms, N::MacrophageGrabSpeed},
    {N::MacrophageSearch, N::MacrophageGrabSpeed},
    {N::MacrophageCapstone, N::MacrophageRoot},
    {N::MacrophageWall, N::MacrophageHealth},
    // The removed cadence node's children now grow directly from Degranulation Cycle.
    {N::NeutrophilCapstone, N::NeutrophilRoundDamage},
    {N::NeutrophilVitality, N::NeutrophilAggroRange},
    // Cytotoxic T.
    {N::CytotoxicAttachSpeed, N::CytotoxicRoot},
    {N::CytotoxicDrain, N::CytotoxicRoot},
    {N::CytotoxicSearch, N::CytotoxicRoot},
    {N::CytotoxicStamina, N::CytotoxicAttachSpeed},
    {N::CytotoxicCapstone, N::CytotoxicRoot},
    {N::CytotoxicHealth, N::CytotoxicSearch},

    // Abilities are discoveries along related cell paths, in three sectors.
    {N::ComplementUnlock, N::CytotoxicRoot},
    {N::FeverUnlock, N::MacrophageRoot},
    {N::ClotUnlock, N::FibroblastRoot},
    // Each ability's own upgrades still grow directly from its unlock.
    {N::ComplementCooldown, N::ComplementUnlock},
    {N::ComplementChain, N::ComplementUnlock},
    {N::FeverCooldown, N::FeverUnlock},
    {N::FeverMagnitude, N::FeverUnlock},
    {N::ClotCooldown, N::ClotUnlock},
    {N::ClotDuration, N::ClotUnlock},

    // Fibroblast.
    {N::FibroblastBuildRadius, N::FibroblastRoot},
    {N::FibroblastScarHealth, N::FibroblastRoot},
    {N::FibroblastHealth, N::FibroblastRoot},
    {N::FibroblastScarSize, N::FibroblastBuildRadius},
    {N::FibroblastScarNetwork, N::FibroblastScarSize},
    {N::FibroblastReinforce, N::FibroblastScarHealth},
    {N::FibroblastInflammation, N::FibroblastScarHealth},
    {N::FibroblastCapstone, N::FibroblastRoot},
    {N::FibroblastBuildRate, N::FibroblastHealth},
    // Goblet Cell.
    {N::GobletSplashRadius, N::GobletRoot},
    {N::GobletSlowStrength, N::GobletRoot},
    {N::GobletHealth, N::GobletRoot},
    {N::GobletWeakness, N::GobletSplashRadius},
    {N::GobletSlowDuration, N::GobletSlowStrength},
    {N::GobletCapstone, N::GobletRoot},
    // Continuation stages require completing the preceding stage.
    {N::BoneMarrowReserveStage2, N::BoneMarrowReserve},
    {N::EfficientClearanceStage2, N::EfficientClearance},
    {N::ComplementCooldownStage2, N::ComplementCooldown},
    {N::FeverCooldownStage2, N::FeverCooldown},
    {N::FeverMagnitudeStage2, N::FeverMagnitude},
    {N::ClotCooldownStage2, N::ClotCooldown},
    {N::ClotDurationStage2, N::ClotDuration},
    {N::NeutrophilRoundDamageStage2, N::NeutrophilSquadSize},
    {N::NeutrophilRoundDamageStage3, N::NeutrophilRoundDamageStage2},
    {N::NeutrophilRoundDamageStage4, N::NeutrophilRoundDamageStage3},
    {N::NeutrophilAggroRangeStage2, N::NeutrophilAggroRange},
    {N::NeutrophilSquadSizeStage2, N::NeutrophilSquadSize},
    {N::NeutrophilAccuracyStage2, N::NeutrophilAccuracy},
    {N::NeutrophilVitalityStage2, N::NeutrophilVitality},
    {N::CytotoxicDrainStage2, N::CytotoxicDrain},
    {N::CytotoxicDrainStage3, N::CytotoxicDrainStage2},
    {N::CytotoxicDrainStage4, N::CytotoxicDrainStage3},
    {N::CytotoxicSearchStage2, N::CytotoxicSearch},
    {N::CytotoxicStaminaStage2, N::CytotoxicStamina},
    {N::CytotoxicHealthStage2, N::CytotoxicHealth},
    {N::MacrophageGrabSpeedStage2, N::MacrophageGrabSpeed},
    {N::MacrophageGrabSpeedStage3, N::MacrophageGrabSpeedStage2},
    {N::MacrophageGrabSpeedStage4, N::MacrophageGrabSpeedStage3},
    {N::MacrophageSearchStage2, N::MacrophageSearch},
    {N::MacrophageHealthStage2, N::MacrophageHealth},
    {N::MacrophageWallStage2, N::MacrophageWall},
    {N::GobletSlowStrengthStage2, N::GobletSlowStrength},
    {N::GobletSplashRadiusStage2, N::GobletSplashRadius},
    {N::GobletSlowDurationStage2, N::GobletSlowDuration},
    {N::GobletWeaknessStage2, N::GobletWeakness},
    {N::GobletHealthStage2, N::GobletHealth},
    {N::FibroblastScarHealthStage2, N::FibroblastScarHealth},
    {N::FibroblastReinforceStage2, N::FibroblastReinforce},
    {N::FibroblastScarSizeStage2, N::FibroblastScarSize},
    {N::FibroblastBuildRadiusStage2, N::FibroblastBuildRadius},
    {N::FibroblastInflammationStage2, N::FibroblastInflammation},
    {N::FibroblastHealthStage2, N::FibroblastHealth},
    {N::FibroblastBuildRateStage2, N::FibroblastBuildRate},
    {N::AdaptiveMemoryStage2, N::AdaptiveMemory},
    {N::AdaptiveMemoryStage3, N::AdaptiveMemoryStage2},
    {N::AdaptiveMemoryStage4, N::AdaptiveMemoryStage3},
    // Independent effects on their related branches.
    {N::NeutrophilRelease, N::NeutrophilRoundDamageStage2},
    {N::NeutrophilReleaseStage2, N::NeutrophilRelease},
    {N::NeutrophilReleaseStage3, N::NeutrophilReleaseStage2},
    {N::NeutrophilReleaseStage4, N::NeutrophilReleaseStage3},
    {N::NeutrophilGranuleDamage, N::NeutrophilRoundDamageStage3},
    {N::NeutrophilGranuleDamageStage2, N::NeutrophilGranuleDamage},
    {N::NeutrophilSpeed, N::NeutrophilVitality},
    {N::NeutrophilSpeedStage2, N::NeutrophilSpeed},
    {N::CytotoxicStaminaHealth, N::CytotoxicStamina},
    {N::CytotoxicStaminaHealthStage2, N::CytotoxicStaminaHealth},
    {N::MacrophageIntakeRadius, N::MacrophageArms},
    {N::MacrophageCaptivesRadius, N::MacrophageCaptives},
    {N::MacrophageGrabReach, N::MacrophageGrabSpeed},
    {N::MacrophageGrabReachStage2, N::MacrophageGrabReach},
    {N::MacrophageGrabReachStage3, N::MacrophageGrabReachStage2},
    {N::MacrophageMobility, N::MacrophageGrabSpeedStage2},
    {N::MacrophageMobilityStage2, N::MacrophageMobility},
    {N::MacrophageMobilityStage3, N::MacrophageMobilityStage2},
    {N::MacrophageWallHealth, N::MacrophageWall},
    {N::MacrophageWallHealthStage2, N::MacrophageWallHealth},
    {N::FibroblastRegrowth, N::FibroblastInflammation},
    {N::FibroblastRegrowthStage2, N::FibroblastRegrowth},
    {N::AdaptiveWork, N::AdaptiveMemory},
    {N::AdaptiveWorkStage2, N::AdaptiveWork},
    {N::AdaptiveWorkStage3, N::AdaptiveWorkStage2},
    {N::AdaptiveWorkStage4, N::AdaptiveWorkStage3},
    {N::CytokineDrain, N::CytotoxicDrain},
    {N::CytokineGobletSpeed, N::GobletSlowStrength},
    {N::AdaptiveDrain, N::CytokineDrain},
    {N::AdaptiveDrainStage2, N::AdaptiveDrain},
    {N::AdaptiveDrainStage3, N::AdaptiveDrainStage2},
    {N::AdaptiveDrainStage4, N::AdaptiveDrainStage3},
    {N::AdaptiveGobletSpeed, N::CytokineGobletSpeed},
    {N::AdaptiveGobletSpeedStage2, N::AdaptiveGobletSpeed},
    {N::AdaptiveGobletSpeedStage3, N::AdaptiveGobletSpeedStage2},
    {N::AdaptiveGobletSpeedStage4, N::AdaptiveGobletSpeedStage3},
};
static_assert(sizeof(kEdges) / sizeof(kEdges[0]) == kTreeNodeCount - 1,
              "every node but the root has exactly one parent");

constexpr u32 idx(TreeNode n) { return static_cast<u32>(n); }

/// One parent per node but the root, small branches around a wider radial hub,
/// and every node's parents lead back to the root (no cycles, no islands).
constexpr bool well_formed_tree() {
    TreeNode parent[kTreeNodeCount] = {};
    u32 edges_in[kTreeNodeCount] = {};
    u32 children[kTreeNodeCount] = {};
    for (u32 i = 0; i < kTreeNodeCount; ++i) parent[i] = TreeNode::Count;
    for (const Edge& e : kEdges) {
        if (e.child == kTreeRoot || e.child == e.parent) return false;
        if (e.child >= TreeNode::Count || e.parent >= TreeNode::Count) return false;
        parent[idx(e.child)] = e.parent;
        ++edges_in[idx(e.child)];
        ++children[idx(e.parent)];
    }
    for (u32 i = 0; i < kTreeNodeCount; ++i) {
        if (static_cast<TreeNode>(i) != kTreeRoot && edges_in[i] != 1) return false;
        const u32 limit = static_cast<TreeNode>(i) == kTreeHub ? kTreeHubMaxChildren : kTreeMaxChildren;
        if (children[i] > limit) return false;
        TreeNode n = static_cast<TreeNode>(i);
        u32 steps = 0;
        while (n != kTreeRoot) {
            if (n == TreeNode::Count || ++steps > kTreeNodeCount) return false;
            n = parent[idx(n)];
        }
    }
    return true;
}
static_assert(well_formed_tree(), "kEdges must make one tree rooted at Round Damage");

constexpr std::array<TreeNodeDef, kTreeNodeCount> with_parents() {
    std::array<TreeNodeDef, kTreeNodeCount> out{};
    for (u32 i = 0; i < kTreeNodeCount; ++i) out[i] = kCatalog[i];
    for (const Edge& e : kEdges) out[idx(e.child)].parent = e.parent;
    out[idx(TreeNode::BoneMarrowReserveStage2)].effect_source = TreeNode::BoneMarrowReserve;
    out[idx(TreeNode::BoneMarrowReserveStage2)].rank_offset = 5;
    out[idx(TreeNode::EfficientClearanceStage2)].effect_source = TreeNode::EfficientClearance;
    out[idx(TreeNode::EfficientClearanceStage2)].rank_offset = 5;
    out[idx(TreeNode::ComplementCooldownStage2)].effect_source = TreeNode::ComplementCooldown;
    out[idx(TreeNode::ComplementCooldownStage2)].rank_offset = 5;
    out[idx(TreeNode::FeverCooldownStage2)].effect_source = TreeNode::FeverCooldown;
    out[idx(TreeNode::FeverCooldownStage2)].rank_offset = 5;
    out[idx(TreeNode::FeverMagnitudeStage2)].effect_source = TreeNode::FeverMagnitude;
    out[idx(TreeNode::FeverMagnitudeStage2)].rank_offset = 5;
    out[idx(TreeNode::ClotCooldownStage2)].effect_source = TreeNode::ClotCooldown;
    out[idx(TreeNode::ClotCooldownStage2)].rank_offset = 5;
    out[idx(TreeNode::ClotDurationStage2)].effect_source = TreeNode::ClotDuration;
    out[idx(TreeNode::ClotDurationStage2)].rank_offset = 5;
    out[idx(TreeNode::NeutrophilRoundDamageStage2)].effect_source = TreeNode::NeutrophilRoundDamage;
    out[idx(TreeNode::NeutrophilRoundDamageStage2)].rank_offset = 5;
    out[idx(TreeNode::NeutrophilRoundDamageStage3)].effect_source = TreeNode::NeutrophilRoundDamage;
    out[idx(TreeNode::NeutrophilRoundDamageStage3)].rank_offset = 10;
    out[idx(TreeNode::NeutrophilRoundDamageStage4)].effect_source = TreeNode::NeutrophilRoundDamage;
    out[idx(TreeNode::NeutrophilRoundDamageStage4)].rank_offset = 15;
    out[idx(TreeNode::NeutrophilAggroRangeStage2)].effect_source = TreeNode::NeutrophilAggroRange;
    out[idx(TreeNode::NeutrophilAggroRangeStage2)].rank_offset = 5;
    out[idx(TreeNode::NeutrophilSquadSizeStage2)].effect_source = TreeNode::NeutrophilSquadSize;
    out[idx(TreeNode::NeutrophilSquadSizeStage2)].rank_offset = 5;
    out[idx(TreeNode::NeutrophilAccuracyStage2)].effect_source = TreeNode::NeutrophilAccuracy;
    out[idx(TreeNode::NeutrophilAccuracyStage2)].rank_offset = 5;
    out[idx(TreeNode::NeutrophilVitalityStage2)].effect_source = TreeNode::NeutrophilVitality;
    out[idx(TreeNode::NeutrophilVitalityStage2)].rank_offset = 5;
    out[idx(TreeNode::CytotoxicDrainStage2)].effect_source = TreeNode::CytotoxicDrain;
    out[idx(TreeNode::CytotoxicDrainStage2)].rank_offset = 5;
    out[idx(TreeNode::CytotoxicDrainStage3)].effect_source = TreeNode::CytotoxicDrain;
    out[idx(TreeNode::CytotoxicDrainStage3)].rank_offset = 10;
    out[idx(TreeNode::CytotoxicDrainStage4)].effect_source = TreeNode::CytotoxicDrain;
    out[idx(TreeNode::CytotoxicDrainStage4)].rank_offset = 15;
    out[idx(TreeNode::CytotoxicSearchStage2)].effect_source = TreeNode::CytotoxicSearch;
    out[idx(TreeNode::CytotoxicSearchStage2)].rank_offset = 5;
    out[idx(TreeNode::CytotoxicStaminaStage2)].effect_source = TreeNode::CytotoxicStamina;
    out[idx(TreeNode::CytotoxicStaminaStage2)].rank_offset = 5;
    out[idx(TreeNode::CytotoxicHealthStage2)].effect_source = TreeNode::CytotoxicHealth;
    out[idx(TreeNode::CytotoxicHealthStage2)].rank_offset = 5;
    out[idx(TreeNode::MacrophageGrabSpeedStage2)].effect_source = TreeNode::MacrophageGrabSpeed;
    out[idx(TreeNode::MacrophageGrabSpeedStage2)].rank_offset = 5;
    out[idx(TreeNode::MacrophageGrabSpeedStage3)].effect_source = TreeNode::MacrophageGrabSpeed;
    out[idx(TreeNode::MacrophageGrabSpeedStage3)].rank_offset = 10;
    out[idx(TreeNode::MacrophageGrabSpeedStage4)].effect_source = TreeNode::MacrophageGrabSpeed;
    out[idx(TreeNode::MacrophageGrabSpeedStage4)].rank_offset = 15;
    out[idx(TreeNode::MacrophageSearchStage2)].effect_source = TreeNode::MacrophageSearch;
    out[idx(TreeNode::MacrophageSearchStage2)].rank_offset = 5;
    out[idx(TreeNode::MacrophageHealthStage2)].effect_source = TreeNode::MacrophageHealth;
    out[idx(TreeNode::MacrophageHealthStage2)].rank_offset = 5;
    out[idx(TreeNode::MacrophageWallStage2)].effect_source = TreeNode::MacrophageWall;
    out[idx(TreeNode::MacrophageWallStage2)].rank_offset = 5;
    out[idx(TreeNode::GobletSlowStrengthStage2)].effect_source = TreeNode::GobletSlowStrength;
    out[idx(TreeNode::GobletSlowStrengthStage2)].rank_offset = 5;
    out[idx(TreeNode::GobletSplashRadiusStage2)].effect_source = TreeNode::GobletSplashRadius;
    out[idx(TreeNode::GobletSplashRadiusStage2)].rank_offset = 5;
    out[idx(TreeNode::GobletSlowDurationStage2)].effect_source = TreeNode::GobletSlowDuration;
    out[idx(TreeNode::GobletSlowDurationStage2)].rank_offset = 5;
    out[idx(TreeNode::GobletWeaknessStage2)].effect_source = TreeNode::GobletWeakness;
    out[idx(TreeNode::GobletWeaknessStage2)].rank_offset = 5;
    out[idx(TreeNode::GobletHealthStage2)].effect_source = TreeNode::GobletHealth;
    out[idx(TreeNode::GobletHealthStage2)].rank_offset = 5;
    out[idx(TreeNode::FibroblastScarHealthStage2)].effect_source = TreeNode::FibroblastScarHealth;
    out[idx(TreeNode::FibroblastScarHealthStage2)].rank_offset = 5;
    out[idx(TreeNode::FibroblastReinforceStage2)].effect_source = TreeNode::FibroblastReinforce;
    out[idx(TreeNode::FibroblastReinforceStage2)].rank_offset = 5;
    out[idx(TreeNode::FibroblastScarSizeStage2)].effect_source = TreeNode::FibroblastScarSize;
    out[idx(TreeNode::FibroblastScarSizeStage2)].rank_offset = 5;
    out[idx(TreeNode::FibroblastBuildRadiusStage2)].effect_source = TreeNode::FibroblastBuildRadius;
    out[idx(TreeNode::FibroblastBuildRadiusStage2)].rank_offset = 5;
    out[idx(TreeNode::FibroblastInflammationStage2)].effect_source = TreeNode::FibroblastInflammation;
    out[idx(TreeNode::FibroblastInflammationStage2)].rank_offset = 5;
    out[idx(TreeNode::FibroblastHealthStage2)].effect_source = TreeNode::FibroblastHealth;
    out[idx(TreeNode::FibroblastHealthStage2)].rank_offset = 5;
    out[idx(TreeNode::FibroblastBuildRateStage2)].effect_source = TreeNode::FibroblastBuildRate;
    out[idx(TreeNode::FibroblastBuildRateStage2)].rank_offset = 5;
    out[idx(TreeNode::AdaptiveMemoryStage2)].effect_source = TreeNode::AdaptiveMemory;
    out[idx(TreeNode::AdaptiveMemoryStage2)].rank_offset = 5;
    out[idx(TreeNode::AdaptiveMemoryStage3)].effect_source = TreeNode::AdaptiveMemory;
    out[idx(TreeNode::AdaptiveMemoryStage3)].rank_offset = 10;
    out[idx(TreeNode::AdaptiveMemoryStage4)].effect_source = TreeNode::AdaptiveMemory;
    out[idx(TreeNode::AdaptiveMemoryStage4)].rank_offset = 15;
    out[idx(TreeNode::NeutrophilReleaseStage2)].effect_source = TreeNode::NeutrophilRelease;
    out[idx(TreeNode::NeutrophilReleaseStage2)].rank_offset = 5;
    out[idx(TreeNode::NeutrophilReleaseStage3)].effect_source = TreeNode::NeutrophilRelease;
    out[idx(TreeNode::NeutrophilReleaseStage3)].rank_offset = 10;
    out[idx(TreeNode::NeutrophilReleaseStage4)].effect_source = TreeNode::NeutrophilRelease;
    out[idx(TreeNode::NeutrophilReleaseStage4)].rank_offset = 15;
    out[idx(TreeNode::NeutrophilGranuleDamageStage2)].effect_source = TreeNode::NeutrophilGranuleDamage;
    out[idx(TreeNode::NeutrophilGranuleDamageStage2)].rank_offset = 5;
    out[idx(TreeNode::NeutrophilSpeedStage2)].effect_source = TreeNode::NeutrophilSpeed;
    out[idx(TreeNode::NeutrophilSpeedStage2)].rank_offset = 5;
    out[idx(TreeNode::CytotoxicStaminaHealthStage2)].effect_source = TreeNode::CytotoxicStaminaHealth;
    out[idx(TreeNode::CytotoxicStaminaHealthStage2)].rank_offset = 5;
    out[idx(TreeNode::MacrophageGrabReachStage2)].effect_source = TreeNode::MacrophageGrabReach;
    out[idx(TreeNode::MacrophageGrabReachStage2)].rank_offset = 5;
    out[idx(TreeNode::MacrophageGrabReachStage3)].effect_source = TreeNode::MacrophageGrabReach;
    out[idx(TreeNode::MacrophageGrabReachStage3)].rank_offset = 10;
    out[idx(TreeNode::MacrophageMobilityStage2)].effect_source = TreeNode::MacrophageMobility;
    out[idx(TreeNode::MacrophageMobilityStage2)].rank_offset = 5;
    out[idx(TreeNode::MacrophageMobilityStage3)].effect_source = TreeNode::MacrophageMobility;
    out[idx(TreeNode::MacrophageMobilityStage3)].rank_offset = 10;
    out[idx(TreeNode::MacrophageWallHealthStage2)].effect_source = TreeNode::MacrophageWallHealth;
    out[idx(TreeNode::MacrophageWallHealthStage2)].rank_offset = 5;
    out[idx(TreeNode::FibroblastRegrowthStage2)].effect_source = TreeNode::FibroblastRegrowth;
    out[idx(TreeNode::FibroblastRegrowthStage2)].rank_offset = 5;
    out[idx(TreeNode::AdaptiveWorkStage2)].effect_source = TreeNode::AdaptiveWork;
    out[idx(TreeNode::AdaptiveWorkStage2)].rank_offset = 5;
    out[idx(TreeNode::AdaptiveWorkStage3)].effect_source = TreeNode::AdaptiveWork;
    out[idx(TreeNode::AdaptiveWorkStage3)].rank_offset = 10;
    out[idx(TreeNode::AdaptiveWorkStage4)].effect_source = TreeNode::AdaptiveWork;
    out[idx(TreeNode::AdaptiveWorkStage4)].rank_offset = 15;
    out[idx(TreeNode::AdaptiveDrainStage2)].effect_source = TreeNode::AdaptiveDrain;
    out[idx(TreeNode::AdaptiveDrainStage2)].rank_offset = 5;
    out[idx(TreeNode::AdaptiveDrainStage3)].effect_source = TreeNode::AdaptiveDrain;
    out[idx(TreeNode::AdaptiveDrainStage3)].rank_offset = 10;
    out[idx(TreeNode::AdaptiveDrainStage4)].effect_source = TreeNode::AdaptiveDrain;
    out[idx(TreeNode::AdaptiveDrainStage4)].rank_offset = 15;
    out[idx(TreeNode::AdaptiveGobletSpeedStage2)].effect_source = TreeNode::AdaptiveGobletSpeed;
    out[idx(TreeNode::AdaptiveGobletSpeedStage2)].rank_offset = 5;
    out[idx(TreeNode::AdaptiveGobletSpeedStage3)].effect_source = TreeNode::AdaptiveGobletSpeed;
    out[idx(TreeNode::AdaptiveGobletSpeedStage3)].rank_offset = 10;
    out[idx(TreeNode::AdaptiveGobletSpeedStage4)].effect_source = TreeNode::AdaptiveGobletSpeed;
    out[idx(TreeNode::AdaptiveGobletSpeedStage4)].rank_offset = 15;
    return out;
}
constexpr std::array<TreeNodeDef, kTreeNodeCount> kNodes = with_parents();

// ---------------------------------------------------------------------------
// Effects. Per-level magnitudes, grouped as the catalog is.
// ---------------------------------------------------------------------------

// Hub.
constexpr u32 kStartingAtpPerLevel = 40;
constexpr f32 kKillIncomePerLevel = 0.10f;
constexpr f32 kBuildCostCutPerLevel = 0.07f;
constexpr f32 kReserveHealthPerLevel = 0.15f;
constexpr f32 kLeakCutPerLevel = 0.10f;
constexpr f32 kMembranePerLevel = 0.08f;

// Abilities.
constexpr f32 kCooldownCutPerLevel = 0.08f;
constexpr u32 kCascadeBaseLinks = 8;          // the damage system's own default
constexpr u32 kCascadeLinksPerLevel = 2;
constexpr f32 kFeverMagnitudePerLevel = 0.25f;
constexpr f32 kClotDurationPerLevel = 0.20f;

// Capstones. The per-unit ones land on CapstoneParams (-> SwarmerProfile), the
// world-wide ones on sim::ImmunityTuning.
constexpr f32 kNeutrophilGranuleDamage = 1.25f;
constexpr f32 kPursuitSpeedMult = 2.0f;
constexpr f32 kSustainHealPerKill = 4.0f;
constexpr f32 kContagionRadius = 3.5f;
constexpr f32 kContagionSeconds = 2.5f;
constexpr f32 kBulwarkScarSizeMult = 2.0f;

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

// Additive bonuses are all measured against the fresh configuration baseline.
// Rates are increased linearly; a duration is its reciprocal.
f32 rate_time(u8 rank, f32 per_rank) {
    return 1.0f / grow(rank, per_rank);
}

void apply_tower_lines(const TreeLevels& lv, TowerType type, TowerMechanics& m) {
    using N = TreeNode;
    const f32 base_health = m.swarm.max_health;
    switch (type) {
    case TowerType::Neutrophil:
        m.shooter.reload_seconds *= rate_time(lv[N::NeutrophilRoundDamage], 0.10f);
        m.shooter.fire_interval *= rate_time(lv[N::NeutrophilRelease], 0.05f);
        m.shooter.gather_seconds *= rate_time(lv[N::NeutrophilRelease], 0.05f);
        m.shooter.round_damage *= 1.0f + 0.05f * lv[N::NeutrophilGranuleDamage] +
            (lv.owned(N::NeutrophilCapstone) ? kNeutrophilGranuleDamage - 1.0f : 0.0f);
        m.swarm.search_radius *= grow(lv[N::NeutrophilAggroRange], 0.10f);
        m.shooter.magazine_size += lv[N::NeutrophilSquadSize];
        m.shooter.round_spread *= shrink(lv[N::NeutrophilAccuracy], 0.10f);
        m.shooter.volley_cone *= shrink(lv[N::NeutrophilAccuracy], 0.10f);
        m.swarm.max_health += 9.0f * lv[N::NeutrophilVitality];
        m.swarm.speed *= grow(lv[N::NeutrophilSpeed], 0.10f);
        break;
    case TowerType::CytotoxicT:
        m.latch.dps *= 1.0f + 0.15f * lv[N::CytotoxicDrain] +
            0.06f * lv[N::CytokineDrain] + 0.04f * lv[N::AdaptiveDrain];
        m.latch.attach_seconds *= rate_time(lv[N::CytotoxicAttachSpeed], 0.25f);
        m.swarm.search_radius *= grow(lv[N::CytotoxicSearch], 0.10f);
        m.swarm.speed *= 1.0f + 0.10f * lv[N::CytotoxicStamina] +
            (lv.owned(N::CytotoxicCapstone) ? kPursuitSpeedMult - 1.0f : 0.0f);
        m.swarm.max_health += 10.0f * lv[N::CytotoxicHealth] +
            9.0f * lv[N::CytotoxicStaminaHealth];
        break;
    case TowerType::Macrophage: {
        m.arbor_grabber.max_captives += 2u * lv[N::MacrophageArms] + lv[N::MacrophageCaptives];
        m.arbor_grabber.cluster_radius += 0.6f * lv[N::MacrophageIntakeRadius] +
            0.4f * lv[N::MacrophageCaptivesRadius];
        const f32 cycle = rate_time(lv[N::MacrophageGrabSpeed], 0.10f);
        m.arbor_grabber.extend_seconds *= cycle;
        m.arbor_grabber.latch_seconds *= cycle;
        m.arbor_grabber.pull_seconds *= cycle;
        m.arbor_grabber.recover_seconds *= cycle;
        m.swarm.search_radius *= 1.0f + 0.10f * lv[N::MacrophageSearch] + 0.03f * lv[N::MacrophageGrabReach];
        m.swarm.attach_radius *= grow(lv[N::MacrophageGrabReach], 0.03f);
        m.swarm.speed *= grow(lv[N::MacrophageMobility], 0.03f);
        m.swarm.max_health += base_health * 0.20f * (lv[N::MacrophageHealth] + lv[N::MacrophageWallHealth]);
        m.arbor_grabber.body_block += 0.01f * lv[N::MacrophageWall];
        if (lv.owned(N::MacrophageCapstone)) m.capstone.heal_per_kill = kSustainHealPerKill;
        break;
    }
    case TowerType::GobletCell:
        m.mucus_bomber.slow_factor *= shrink(lv[N::GobletSlowStrength], 0.10f);
        m.mucus_bomber.splash_radius *= grow(lv[N::GobletSplashRadius], 0.15f);
        m.mucus_bomber.slow_duration *= grow(lv[N::GobletSlowDuration], 0.20f);
        m.swarm.max_health += 10.0f * lv[N::GobletHealth];
        m.swarm.speed *= 1.0f + 0.06f * lv[N::CytokineGobletSpeed] + 0.04f * lv[N::AdaptiveGobletSpeed];
        break;
    case TowerType::Fibroblast: {
        m.builder.scar_health *= grow(lv[N::FibroblastScarHealth], 0.20f);
        m.builder.scar_reinforce *= grow(lv[N::FibroblastReinforce], 0.30f);
        const f32 scar_size = 1.0f + 0.10f * lv[N::FibroblastScarSize] +
            (lv.owned(N::FibroblastCapstone) ? kBulwarkScarSizeMult - 1.0f : 0.0f);
        m.builder.scar_half_length *= scar_size;
        m.builder.scar_half_width *= scar_size;
        m.builder.build_radius *= grow(lv[N::FibroblastBuildRadius], 0.12f);
        m.builder.leash_radius *= grow(lv[N::FibroblastBuildRadius], 0.12f);
        m.swarm.search_radius *= grow(lv[N::FibroblastBuildRadius], 0.12f);
        m.swarm.max_health += base_health * 0.20f * lv[N::FibroblastHealth];
        m.builder.build_cooldown *= rate_time(lv[N::FibroblastBuildRate], 0.12f);
        if (m.builder.max_scars > 0) m.builder.max_scars += lv[N::FibroblastScarNetwork];
        break;
    }
    case TowerType::Count:
        break;
    }
}

void apply_ability_lines(const TreeLevels& lv, AbilityConfig& abilities) {
    using N = TreeNode;
    AbilityTuning& cascade = abilities.ability[static_cast<u32>(kCascade)];
    cascade.cooldown_seconds *= shrink(lv[N::ComplementCooldown], kCooldownCutPerLevel);
    if (lv[N::ComplementChain] > 0) {
        cascade.chain_links = kCascadeBaseLinks + kCascadeLinksPerLevel * lv[N::ComplementChain];
    }

    AbilityTuning& fever = abilities.ability[static_cast<u32>(kFever)];
    fever.cooldown_seconds *= shrink(lv[N::FeverCooldown], kCooldownCutPerLevel);
    // Healing upgrades affect the direct-cell heal only, not the legacy
    // emplacement cooldown relief or its optional lingering effect.
    fever.fever_healing_mult *= grow(lv[N::FeverMagnitude], kFeverMagnitudePerLevel);

    AbilityTuning& clot = abilities.ability[static_cast<u32>(kClot)];
    clot.cooldown_seconds *= shrink(lv[N::ClotCooldown], kCooldownCutPerLevel);
    clot.field_duration *= grow(lv[N::ClotDuration], kClotDurationPerLevel);
}

} // namespace

const TreeNodeDef& tree_node(TreeNode n) {
    const u32 i = static_cast<u32>(n);
    return kNodes[i < kTreeNodeCount ? i : 0];
}

TreeNode tree_previous_stage(TreeNode n) {
    const auto& d = tree_node(n);
    if (d.effect_source == TreeNode::Count) return TreeNode::Count;
    const auto& first = tree_node(d.effect_source);
    if (first.max_level == d.rank_offset) return d.effect_source;
    for (u32 i = 0; i < kTreeNodeCount; ++i) {
        const auto& candidate = kNodes[i];
        if (candidate.effect_source == d.effect_source &&
            candidate.rank_offset + candidate.max_level == d.rank_offset) {
            return static_cast<TreeNode>(i);
        }
    }
    return TreeNode::Count;
}

bool find_tree_node(std::string_view key, TreeNode& out) {
    // Legacy console and experiment keys resolve to the merged upgrade.
    if (key == "neutrophil.trigger_rate") {
        out = TreeNode::NeutrophilRoundDamage;
        return true;
    }
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
    case TowerType::Neutrophil: return TreeNode::Count;
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
    case TreeNodeKind::AbilityRoot:
    case TreeNodeKind::Capstone:
        // Each first clear funds one new usable tool. Ordinary stats remain
        // Memory Cell purchases, independently of this discovery currency.
        c.antibodies = 1;
        break;
    case TreeNodeKind::Stat:
    case TreeNodeKind::Economy:
    case TreeNodeKind::AbilityStat: {
        const u64 base = static_cast<u64>(cfg.stat_base_cost) +
                         static_cast<u64>(cfg.stat_cost_step) * (current + d.rank_offset);
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
        const TowerType type = static_cast<TowerType>(t);
        const TreeNode root = tower_root(type);
        if (type == TowerType::Neutrophil || (root != TreeNode::Count && levels.owned(root))) mask |= 1u << t;
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

TreeEffects apply_immunity_tree(const TreeLevels& purchased, GameConfig& cfg) {
    using N = TreeNode;
    // The effects of a line depend on its total purchased ranks, even when
    // those ranks are presented as separate nodes in the tree.
    TreeLevels lv = purchased;
    for (u32 i = 0; i < kTreeNodeCount; ++i) {
        const auto& d = kNodes[i];
        if (d.effect_source != TreeNode::Count)
            lv[d.effect_source] += purchased.level[i];
    }
    TreeEffects fx;

    // All rate and health bonuses are measured from the fresh baseline.
    // Summing rate bonuses before converting to a duration keeps an extra
    // rank equally valuable regardless of the other purchased upgrades.
    const f32 cost_mult = shrink(lv[N::FieldRequisition], kBuildCostCutPerLevel);
    const f32 work_bonus = 0.06f * lv[N::SystemicPotency] + 0.04f * lv[N::AdaptiveWork];
    for (u32 t = 0; t < kTowerTypeCount; ++t) {
        TowerStats& st = cfg.towers.stats[t];
        TowerMechanics& m = cfg.towers.mechanics[t];
        const TowerMechanics base = m;
        apply_tower_lines(lv, static_cast<TowerType>(t), m);

        st.build_cost = math::max(1u, scale_u32(st.build_cost, cost_mult));
        switch (static_cast<TowerType>(t)) {
        case TowerType::Neutrophil: {
            const f32 reload_rate = 1.0f + 0.10f * lv[N::NeutrophilRoundDamage] + work_bonus;
            const f32 release_rate = 1.0f + 0.05f * lv[N::NeutrophilRelease] + work_bonus;
            m.shooter.reload_seconds = base.shooter.reload_seconds / reload_rate;
            m.shooter.fire_interval = base.shooter.fire_interval / release_rate;
            m.shooter.gather_seconds = base.shooter.gather_seconds / release_rate;
            break;
        }
        case TowerType::CytotoxicT:
            m.latch.attach_seconds = base.latch.attach_seconds /
                (1.0f + 0.25f * lv[N::CytotoxicAttachSpeed] + work_bonus);
            break;
        case TowerType::Macrophage: {
            const f32 rate = 1.0f + 0.10f * lv[N::MacrophageGrabSpeed] + work_bonus;
            m.arbor_grabber.extend_seconds = base.arbor_grabber.extend_seconds / rate;
            m.arbor_grabber.latch_seconds = base.arbor_grabber.latch_seconds / rate;
            m.arbor_grabber.pull_seconds = base.arbor_grabber.pull_seconds / rate;
            m.arbor_grabber.recover_seconds = base.arbor_grabber.recover_seconds / rate;
            break;
        }
        case TowerType::GobletCell:
            break;
        case TowerType::Fibroblast:
            m.builder.build_cooldown = base.builder.build_cooldown /
                (1.0f + 0.12f * lv[N::FibroblastBuildRate] + work_bonus);
            m.builder.secrete_seconds = base.builder.secrete_seconds / (1.0f + work_bonus);
            break;
        case TowerType::Count: break;
        }
        m.swarm.max_health += base.swarm.max_health *
            (kReserveHealthPerLevel * lv[N::EliteResponse] + 0.06f * lv[N::AdaptiveMemory]);
    }

    // ---- Economy.
    cfg.economy.starting_atp += kStartingAtpPerLevel * lv[N::BoneMarrowReserve];
    cfg.economy.atp_per_density *= grow(lv[N::EfficientClearance], kKillIncomePerLevel);
    // Legacy tower economics still mirror the economy's refund field.
    cfg.towers.globals.refund_fraction = cfg.economy.refund_fraction;

    // ---- Abilities.
    apply_ability_lines(lv, cfg.abilities);

    // ---- The rest: world-wide sim modifiers, the hostile pass, the masks.
    fx.immunity.leak_damage =
        cfg.sim.globals.objective_damage_per_leak * shrink(lv[N::Homeostasis], kLeakCutPerLevel);
    if (lv.owned(N::GobletCapstone)) {
        fx.immunity.contagion_radius = kContagionRadius;
        fx.immunity.contagion_seconds = kContagionSeconds;
    }
    fx.immunity.slowed_damage_mult = grow(lv[N::GobletWeakness], kWeaknessPerLevel);
    if (lv[N::FibroblastInflammation] > 0 || lv[N::FibroblastRegrowth] > 0) {
        fx.immunity.inflammation_radius = kInflammationRadius;
        fx.immunity.inflammation_damage_mult = grow(lv[N::FibroblastInflammation], kInflammationDamagePerLevel);
        fx.immunity.inflammation_reload_mult = grow(lv[N::FibroblastRegrowth], kInflammationReloadPerLevel);
    }
    fx.hostile_damage_taken_mult = shrink(lv[N::MembraneResilience], kMembranePerLevel);
    fx.tower_mask = unlocked_tower_mask(lv);
    fx.ability_mask = unlocked_ability_mask(lv);
    return fx;
}

} // namespace immune::game
