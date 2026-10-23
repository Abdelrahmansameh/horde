#include "game/balance/BalanceCatalog.h"

#include "config/Registry.h"
#include "game/balance/BalanceRun.h"
#include "game/meta/ImmunityTree.h"
#include "game/towers/TowerMechanics.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace immune::game::balance {
namespace {
using json = config::Json;

const char* ability_key(AbilityId id) {
    switch (id) {
        case AbilityId::ComplementCascadeBurst: return "complement";
        case AbilityId::HistamineFlare: return "histamine";
        case AbilityId::FeverResponse: return "fever";
        case AbilityId::FibrinClot: return "clot";
        case AbilityId::Count: break;
    }
    return "unknown";
}

const char* branch_key(TreeBranch branch) {
    const TowerType type = branch_tower(branch);
    return type == TowerType::Count ? "hub" : tower_type_name(type);
}

const char* node_kind(TreeNodeKind kind) {
    switch (kind) {
        case TreeNodeKind::TowerRoot: return "tower_root";
        case TreeNodeKind::Stat: return "stat";
        case TreeNodeKind::Capstone: return "capstone";
        case TreeNodeKind::Economy: return "economy";
        case TreeNodeKind::AbilityRoot: return "ability_root";
        case TreeNodeKind::AbilityStat: return "ability_stat";
    }
    return "unknown";
}

json node_key(TreeNode node) {
    return node == TreeNode::Count ? json(nullptr) : json(tree_node(node).key);
}

json rules(const TreeEffects& effects, const GameConfig& config) {
    const auto& t = effects.immunity;
    json result = {{"tower_mask", effects.tower_mask}, {"ability_mask", effects.ability_mask},
        {"hostile_damage_taken_mult", effects.hostile_damage_taken_mult},
        {"immunity", {{"named_damage_mult", t.named_damage_mult}, {"leak_damage", t.leak_damage},
            {"contagion_radius", t.contagion_radius}, {"contagion_seconds", t.contagion_seconds},
            {"max_contagion_per_tick", t.max_contagion_per_tick},
            {"scar_contact_rate", t.scar_contact_rate}, {"scar_contact_reach", t.scar_contact_reach},
            {"slowed_damage_mult", t.slowed_damage_mult}, {"inflammation_radius", t.inflammation_radius},
            {"inflammation_damage_mult", t.inflammation_damage_mult},
            {"inflammation_reload_mult", t.inflammation_reload_mult}}}};
    for (u32 i = 0; i < kTowerTypeCount; ++i) {
        const auto& c = config.towers.mechanics[i].capstone;
        result["unit_capstones"][tower_type_name(static_cast<TowerType>(i))] = {
            {"kill_pulse_radius", c.kill_pulse_radius}, {"kill_pulse_damage", c.kill_pulse_damage},
            {"named_damage_mult", c.named_damage_mult}, {"heal_per_kill", c.heal_per_kill}};
    }
    return result;
}

std::string read(const config::Registry& registry, const std::string& path) {
    std::string value, error;
    if (!registry.get(path, value, error)) throw std::runtime_error(error);
    return value;
}
} // namespace

config::Json describe_catalog(const GameConfig& input) {
    GameConfig base = input;
    config::Registry registry;
    bind_game_config(registry, base);
    const auto paths = registry.field_paths();
    json result = {{"schema", 1}, {"kind", "balance_catalog"},
        {"base_config", dump_game_config(base)},
        {"config_files", config_file_names()},
        {"config_hash", fingerprint(json(dump_game_config(base)).dump())},
        {"tree", {{"root", tree_node(kTreeRoot).key},
            {"capstone_threshold", 0},
            {"branch_points_rule", "stat ranks are descriptive; discoveries require their parent and one Antibody, without a branch-point gate"},
            {"rank_effects_scope", "target node granted alone against the zero tree; eligibility prerequisites are supplied separately in experiments"},
            {"nodes", json::array()}}},
        {"registry", {{"value_encoding", "Registry::get text, accepted verbatim by Registry::set; vectors are comma-separated"},
            {"fields", json::array()}}}, {"cells", json::array()}, {"abilities", json::array()}};
    for (const auto& path : paths) {
        result["registry"]["fields"].push_back({{"path", path}, {"value", read(registry, path)},
            {"documentation", std::string(registry.doc(path))}});
    }

    GameConfig zero_config = base;
    const TreeEffects zero_effects = apply_immunity_tree(TreeLevels{}, zero_config);
    config::Registry zero_registry;
    bind_game_config(zero_registry, zero_config);
    result["tree"]["zero_rules"] = rules(zero_effects, zero_config);
    for (u32 i = 0; i < kTreeNodeCount; ++i) {
        const auto node = static_cast<TreeNode>(i);
        const auto& def = tree_node(node);
        const TowerType type = branch_tower(def.branch);
        json item = {{"id", i}, {"key", def.key}, {"name", def.name}, {"effect", def.effect},
            {"parent", node_key(def.parent)}, {"kind", node_kind(def.kind)},
            {"kind_id", static_cast<u32>(def.kind)}, {"branch", branch_key(def.branch)},
            {"branch_id", static_cast<u32>(def.branch)}, {"branch_name", branch_name(def.branch)},
            {"max_rank", def.max_level}, {"cost_weight", def.cost_weight},
            {"cell", type == TowerType::Count ? json(nullptr) : json(tower_type_name(type))},
            {"ability", def.ability == AbilityId::Count ? json(nullptr) : json(ability_key(def.ability))},
            {"rank_prices", json::array()}, {"rank_effects", json::array()},
            {"applicability", {{"status", "available"},
                {"reason", "requires an encounter and composition that exercise the documented effect"}}}};
        if (node == TreeNode::MacrophageArms) {
            const auto configured = base.towers.mechanics[static_cast<u32>(TowerType::Macrophage)].arbor_grabber.max_captives;
            item["runtime_constraint"] = {{"field","towers.macrophage.payload.max_captives"},
                {"configured_baseline",configured},{"engine_ceiling",sim::kArborMaxCaptives},
                {"runtime_baseline",std::min(configured,sim::kArborMaxCaptives)},
                {"scope","Captive capacity is capped by the runtime; this discovery also increases the collection radius"}};
        }
        u64 cumulative_mc = 0, cumulative_ab = 0;
        for (u32 rank = 1; rank <= def.max_level; ++rank) {
            const TreeCost price = tree_node_cost(base.meta, node, static_cast<u8>(rank - 1));
            cumulative_mc += price.memory_cells;
            cumulative_ab += price.antibodies;
            item["rank_prices"].push_back({{"rank", rank}, {"memory_cells", price.memory_cells},
                {"antibodies", price.antibodies}, {"cumulative_memory_cells", cumulative_mc},
                {"cumulative_antibodies", cumulative_ab}});
            TreeLevels levels;
            levels[node] = static_cast<u8>(rank);
            GameConfig upgraded = base;
            const TreeEffects effects = apply_immunity_tree(levels, upgraded);
            config::Registry upgraded_registry;
            bind_game_config(upgraded_registry, upgraded);
            json changes = json::array();
            for (const auto& path : paths) {
                const auto before = read(zero_registry, path);
                const auto after = read(upgraded_registry, path);
                if (before != after) changes.push_back({{"path", path}, {"from", before}, {"value", after}});
            }
            json rank_effect = {{"rank", rank}, {"config_changes", changes},
                {"rules", rules(effects, upgraded)}};
            if (node == TreeNode::MacrophageArms) {
                const auto configured = upgraded.towers.mechanics[static_cast<u32>(TowerType::Macrophage)].arbor_grabber.max_captives;
                rank_effect["runtime_max_captives"] = std::min(configured,sim::kArborMaxCaptives);
            }
            item["rank_effects"].push_back(std::move(rank_effect));
        }
        result["tree"]["nodes"].push_back(std::move(item));
    }
    for (u32 i = 0; i < kTowerTypeCount; ++i) {
        const auto type = static_cast<TowerType>(i);
        result["cells"].push_back({{"id", i}, {"key", tower_type_name(type)},
            {"name", branch_name(tower_branch(type))}, {"branch", branch_key(tower_branch(type))},
            {"root", node_key(tower_root(type))}, {"kind", tower_kind_name(tower_kind(type))},
            {"build_cost", base.towers.stats[i].build_cost}});
    }
    for (u32 i = 0; i < kAbilityCount; ++i) {
        const auto ability = static_cast<AbilityId>(i);
        result["abilities"].push_back({{"id", i}, {"key", ability_key(ability)},
            {"name", ability_name(ability)}, {"root", node_key(ability_root(ability))},
            {"cooldown_seconds", base.abilities.ability[i].cooldown_seconds}});
    }
    result["enemy_roster"] = {{"named_count", base.enemies.elites.size()},
        {"families", json::array()}, {"named", json::array()}};
    constexpr const char* family_keys[] = {"virus", "bacteria", "parasite"};
    for (u32 i = 0; i < kFamilyCount; ++i) {
        result["enemy_roster"]["families"].push_back({{"id", i}, {"key", family_keys[i]},
            {"base_density", base.enemies.families[i].behavior.base_density}});
    }
    for (const auto& enemy : base.enemies.elites) {
        result["enemy_roster"]["named"].push_back({{"id", enemy.id}, {"name", enemy.name},
            {"family_id", static_cast<u32>(enemy.family)}, {"tier_id", static_cast<u32>(enemy.tier)}});
    }
    return result;
}

} // namespace immune::game::balance
