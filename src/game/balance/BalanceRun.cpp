#include "game/balance/BalanceRun.h"
#include "game/balance/BalanceController.h"
#include "game/session/RunSetup.h"
#include "game/session/LevelSession.h"
#include "game/level/Level.h"
#include "game/level/LevelWriter.h"
#include "game/economy/Economy.h"
#include "game/towers/TowerSystem.h"
#include "game/enemies/EnemyRoster.h"
#include "game/enemies/EnemyConfigApply.h"
#include "game/abilities/ActiveAbilities.h"
#include "platform/FileIO.h"
#include "config/Registry.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace immune::game::balance {
using json = config::Json;
u64 fingerprint(std::string_view text) {
    u64 h = 14695981039346656037ULL;
    for (const unsigned char c : text) { h ^= c; h *= 1099511628211ULL; }
    return h;
}
namespace {
constexpr const char* families[] = {"virus", "bacteria", "parasite"};
constexpr const char* ability_keys[] = {"complement", "histamine", "fever", "clot"};
void require(bool ok, const std::string& error) {
    if (!ok) throw std::runtime_error(error);
}
void keys(const json& j, std::initializer_list<const char*> allowed, const char* where) {
    require(j.is_object(), std::string(where) + " must be an object");
    for (auto it = j.begin(); it != j.end(); ++it) {
        bool found = false;
        for (auto k : allowed) if (it.key() == k) found = true;
        require(found, std::string(where) + ": unknown field '" + it.key() + "'");
    }
}
u64 integer(const json& j, const char* key, u64 fallback, u64 maximum = 1000000000ULL) {
    if (!j.contains(key)) return fallback;
    const auto& v = j.at(key);
    require(v.is_number_integer() && (v.is_number_unsigned() || v.get<i64>() >= 0),
            std::string(key) + " must be a nonnegative integer");
    const u64 n = v.get<u64>();
    require(n <= maximum, std::string(key) + " exceeds supported limit");
    return n;
}
Vec2 position(const json& j) {
    require(j.is_array() && j.size() == 2, "position must contain two numbers");
    const Vec2 p{j.at(0).get<f32>(), j.at(1).get<f32>()};
    require(std::isfinite(p.x) && std::isfinite(p.y), "position must be finite");
    return p;
}
json economy_json(const Economy& economy) {
    const auto e = economy.snapshot();
    return {{"atp",e.atp},{"opening_atp",e.opening_atp},{"damage_income",e.damage_income},
            {"wave_income",e.bounty_income},{"refunds",e.refunds},{"spent",e.total_spent},
            {"fractional_income",e.fractional_income}};
}
json state_json(const sim::SimWorld& world) {
    const auto s = world.snapshot();
    json enemy = json::object();
    u64 kills = 0, leaks = 0, despawns = 0, spawned = 0;
    for (u32 f = 0; f < kFamilyCount; ++f) {
        enemy[families[f]] = {{"spawned",s.chaff_spawned_by_family[f]},
            {"killed",s.chaff_killed_by_family[f]},{"leaked",s.chaff_leaked_by_family[f]},
            {"despawned",s.chaff_despawned_by_family[f]},{"alive",s.chaff_by_family[f]}};
        kills += s.chaff_killed_by_family[f]; leaks += s.chaff_leaked_by_family[f];
        despawns += s.chaff_despawned_by_family[f]; spawned += s.chaff_spawned_by_family[f];
    }
    u64 friendly_alive = 0;
    const auto& sw = world.swarmers();
    for (usize i = 0; i < sw.count(); ++i)
        if (sw.health[i] > 0 && !(sw.flags[i]&sim::swarmer_flags::kPendingKill)) ++friendly_alive;
    return {{"tick",s.tick},{"alive",s.chaff_count},{"density",s.total_density},
        {"integrity",s.objective_integrity},{"killed",kills},{"leaked",leaks},
        {"despawned",despawns},{"spawned",spawned},{"families",enemy},
        {"friendly_alive",friendly_alive},{"friendly_killed",s.swarmers_killed_total},
        {"scars_alive",s.scars_live},{"scars_built",s.scars_built_total},
        {"scars_lost",s.scars_lost_total},{"burrowed",s.chaff_burrowed},
        {"projectiles_live",world.projectiles().count()},{"fluid_live",world.fluid().count()}};
}
json damage_json(const sim::DensityMetrics& m) {
    return {{"attempted_density",m.attempted_density},{"density_removed",m.density_removed},
        {"overkill_density",m.overkill_density},{"immune_density",m.immune_density},
        {"damage_kills",m.damage_kills},{"damage_calls",m.damage_calls}};
}
json emission_json(const sim::EmissionMetrics& m) {
    return {{"requested",m.requested},{"spawned",m.spawned},{"denied",m.denied}};
}
json pool_json(const sim::SpawnPoolMetrics& m, usize capacity) {
    auto result = emission_json(m);
    result["highwater"] = m.highwater;
    result["capacity"] = capacity;
    return result;
}
json projectile_json(const sim::ProjectileOutcomeMetrics& m) {
    return {{"agent_hits",m.agent_hits},{"projectiles_hit",m.projectiles_hit},
        {"wall_impacts",m.wall_impacts},{"expired",m.expired},{"retired_on_hit",m.retired_on_hit}};
}
void add_emissions(json& target,const sim::EmissionMetrics& m) {
    target["requested"] = target.at("requested").get<u64>()+m.requested;
    target["spawned"] = target.at("spawned").get<u64>()+m.spawned;
    target["denied"] = target.at("denied").get<u64>()+m.denied;
}
void add_projectile_outcomes(json& target,const sim::ProjectileOutcomeMetrics& m) {
    target["agent_hits"] = target.at("agent_hits").get<u64>()+m.agent_hits;
    target["projectiles_hit"] = target.at("projectiles_hit").get<u64>()+m.projectiles_hit;
    target["wall_impacts"] = target.at("wall_impacts").get<u64>()+m.wall_impacts;
    target["expired"] = target.at("expired").get<u64>()+m.expired;
    target["retired_on_hit"] = target.at("retired_on_hit").get<u64>()+m.retired_on_hit;
}
json activity_json(const sim::ActivityMetrics& m) {
    json result = {{"sample_hz",60},{"overlapping",true},{"exact_subtick_durations",false},
        {"sampling","end of behavior update before body collision"}};
    const auto add = [&](const char* name,u64 count) {
        result[std::string(name)+"_ticks"] = count;
        result[std::string(name)+"_seconds"] = count*kFixedDtSeconds;
    };
    add("observed",m.observed_ticks); add("targetless",m.targetless_ticks);
    add("chasing",m.chasing_ticks); add("engaged",m.engaged_ticks);
    add("attaching",m.attaching_ticks); add("attacking",m.attacking_ticks);
    add("reloading",m.reloading_ticks); add("gathering",m.gathering_ticks);
    add("firing",m.firing_ticks); add("loaded",m.loaded_ticks);
    add("arbor_work",m.arbor_work_ticks); add("builder_travel",m.builder_travel_ticks);
    add("builder_rest",m.builder_rest_ticks); add("builder_secreting",m.builder_secreting_ticks);
    add("builder_waiting",m.builder_waiting_ticks); add("builder_paused",m.builder_paused_ticks);
    add("retired",m.retired_ticks);
    return result;
}
json distribution_json(const sim::TickDistribution& d) {
    json result = {{"observations",d.observations},{"sum_ticks",d.sum_ticks},
        {"min_ticks",d.min_ticks},{"max_ticks",d.max_ticks},{"bins",d.bins},
        {"mean_seconds",nullptr},{"p50_lower_seconds",nullptr},{"p50_upper_seconds",nullptr},
        {"p90_lower_seconds",nullptr},{"p90_upper_seconds",nullptr}};
    if (!d.observations) return result;
    result["mean_seconds"] = d.sum_ticks/d.observations*kFixedDtSeconds;
    const auto quantile = [&](f64 fraction,const char* lower,const char* upper) {
        const u64 rank = static_cast<u64>(std::ceil(d.observations*fraction));
        u64 cumulative = 0;
        for (usize bin = 0; bin < d.bins.size(); ++bin) {
            cumulative += d.bins[bin];
            if (cumulative < rank) continue;
            const f64 lo = bin == 0 ? 0 : std::ldexp(1.0,static_cast<int>(bin)-1);
            const f64 hi = bin == 0 ? 0 : std::ldexp(1.0,static_cast<int>(bin))-1;
            result[lower] = std::max(lo,static_cast<f64>(d.min_ticks))*kFixedDtSeconds;
            result[upper] = std::min(hi,static_cast<f64>(d.max_ticks))*kFixedDtSeconds;
            break;
        }
    };
    quantile(0.5,"p50_lower_seconds","p50_upper_seconds");
    quantile(0.9,"p90_lower_seconds","p90_upper_seconds");
    return result;
}
json lifecycle_json(const sim::LifecycleMetrics& m) {
    return {{"spawned",m.spawned},{"entered_alive",m.entered_alive},{"alive",m.alive},
        {"hit_alive",m.hit_alive},{"first_hits",m.first_hits},{"damage_deaths",m.damage_deaths},{"leaked",m.leaked},
        {"out_of_bounds",m.out_of_bounds},{"consumed",m.consumed},{"expired",m.expired},
        {"unknown",m.unknown},{"damage_taken",m.damage_taken},
        {"damage_ttk",distribution_json(m.damage_ttk)},
        {"damage_lifetime",distribution_json(m.damage_lifetime)},
        {"retired_lifetime",distribution_json(m.retired_lifetime)}};
}
void merge_distribution(sim::TickDistribution& a,const sim::TickDistribution& b) {
    if (!b.observations) return;
    if (!a.observations) a.min_ticks = b.min_ticks;
    else a.min_ticks = std::min(a.min_ticks,b.min_ticks);
    a.max_ticks = std::max(a.max_ticks,b.max_ticks);
    a.observations += b.observations; a.sum_ticks += b.sum_ticks;
    for (usize i = 0; i < a.bins.size(); ++i) a.bins[i] += b.bins[i];
}
void merge_lifecycle(sim::LifecycleMetrics& a,const sim::LifecycleMetrics& b) {
    a.spawned += b.spawned; a.entered_alive += b.entered_alive; a.alive += b.alive;
    a.hit_alive += b.hit_alive; a.first_hits += b.first_hits; a.damage_deaths += b.damage_deaths; a.leaked += b.leaked;
    a.out_of_bounds += b.out_of_bounds; a.consumed += b.consumed; a.expired += b.expired;
    a.unknown += b.unknown; a.damage_taken += b.damage_taken;
    merge_distribution(a.damage_ttk,b.damage_ttk);
    merge_distribution(a.damage_lifetime,b.damage_lifetime);
    merge_distribution(a.retired_lifetime,b.retired_lifetime);
}
json metrics_json(const sim::CombatMetrics& metrics, const sim::SimWorld& world) {
    if (!metrics.enabled()) return {{"enabled",false},{"damage",nullptr},{"families",json::object()},
        {"pools",nullptr},{"projectile_outcomes",nullptr},{"activity",nullptr},{"overflow",nullptr},
        {"buckets",json::array()},{"enemy_lifetimes",json::object()},{"friendly_lifetimes",json::array()},
        {"unavailable_reason","combat telemetry disabled"}};
    constexpr const char* sources[] = {"unknown","field","projectile","drain","swallow","burst","fluid","scar_contact"};
    json result = {{"enabled",metrics.enabled()},{"damage",damage_json(metrics.totals().damage)},
        {"overflow",metrics.overflow_count()},{"buckets",json::array()}};
    for (const auto& b : metrics.buckets()) {
        json item = {{"owner",b.owner.value},{"type",tower_type_name(b.type)},
            {"source",sources[static_cast<u32>(b.source)]},{"damage",damage_json(b.totals)},
            {"emissions",emission_json(b.emissions)},{"projectile_outcomes",projectile_json(b.projectile_outcomes)},
            {"activity",activity_json(b.activity)}};
        for (u32 f = 0; f < kFamilyCount; ++f)
            item["families"][families[f]] = damage_json(b.by_family[f]);
        result["buckets"].push_back(std::move(item));
    }
    for (u32 f = 0; f < kFamilyCount; ++f)
        result["families"][families[f]] = damage_json(metrics.totals().by_family[f]);
    const auto& t = metrics.totals();
    result["pools"] = {{"cells",pool_json(t.pools.cells,world.swarmers().capacity())},
        {"projectiles",pool_json(t.pools.projectiles,world.projectiles().capacity())},
        {"fluid",pool_json(t.pools.fluid,world.fluid().capacity())}};
    result["pools"]["cells"]["request_scope"] = "kernel spawn attempts; player validation rejections are separate";
    result["projectile_outcomes"] = projectile_json(t.projectile_outcomes);
    result["projectile_outcomes"]["live_at_end"] = world.projectiles().count();
    result["projectile_outcomes"]["scope"] = "observed chaff hits; distinct hit projectiles and live rounds are separate";
    result["fluid_live_at_end"] = world.fluid().count();
    for (u32 type = 0; type < kTowerTypeCount; ++type)
        result["activity"][tower_type_name(static_cast<TowerType>(type))] = activity_json(metrics.activity()[type]);
    result["hostile_requested_cell_damage"] = t.hostile_requested_cell_damage;
    result["hostile_actual_cell_damage"] = t.hostile_actual_cell_damage;
    result["hostile_requested_structure_damage"] = t.hostile_requested_structure_damage;
    result["cells_killed_by_hostiles"] = t.cells_killed_by_hostiles;
    result["toxin_fired"] = t.toxin_fired;
    result["toxin_hits"] = t.toxin_hits;
    result["new_latches"] = t.new_latches;
    result["replications_denied_capacity"] = t.replications_denied_capacity;
    result["replications_denied_tick_budget"] = t.replications_denied_tick_budget;
    for (u32 f = 0; f < kFamilyCount; ++f)
        result["enemy_lifetimes"][families[f]] = lifecycle_json(metrics.enemy_lifetimes()[f]);
    result["friendly_lifetimes"] = json::array();
    for (const auto& b : metrics.friendly_lifetimes())
        result["friendly_lifetimes"].push_back({{"owner",b.owner.value},
            {"type",tower_type_name(b.type)},{"stats",lifecycle_json(b.stats)}});
    return result;
}
json combat_conservation(const sim::SimWorld& world, f64 world_density, json& errors) {
    const auto& metrics = world.combat_metrics();
    if (!metrics.enabled()) return {{"available",false},{"reason","combat telemetry disabled"}};
    const auto& totals = metrics.totals();
    json result = {{"available",true}};
    const auto fail = [&](const std::string& message) { errors.push_back(message); };
    const auto pool = [&](const char* name,const sim::SpawnPoolMetrics& counts) {
        const bool reconciled = counts.requested == counts.spawned+counts.denied;
        result["pool_requests"][name] = reconciled;
        if (!reconciled) fail(std::string(name)+" requested/spawned/denied counters do not reconcile");
    };
    pool("cells",totals.pools.cells); pool("projectiles",totals.pools.projectiles); pool("fluid",totals.pools.fluid);
    if (totals.pools.projectiles.denied) fail("friendly projectile output was suppressed by projectile capacity");
    if (totals.pools.fluid.denied) fail("friendly fluid output was suppressed by fluid capacity");
    if (totals.pools.cells.denied) fail("friendly kernel spawn output was suppressed by cell capacity");
    const auto& projectiles = totals.projectile_outcomes;
    const bool projectile_conserved = totals.pools.projectiles.spawned == world.projectiles().count()+
        projectiles.retired_on_hit+projectiles.wall_impacts+projectiles.expired;
    result["projectile_population"] = projectile_conserved;
    if (!projectile_conserved) fail("spawned projectiles do not reconcile with live and retired rounds");
    const f64 world_tolerance = std::max(0.01,std::abs(world_density)*1e-4);
    result["world_density_removed"] = world_density;
    result["observed_density_removed"] = totals.damage.density_removed;
    result["density_difference"] = totals.damage.density_removed-world_density;
    result["density_tolerance"] = world_tolerance;
    result["world_damage"] = std::abs(totals.damage.density_removed-world_density) <= world_tolerance;
    if (!result["world_damage"].get<bool>()) fail("applied damage telemetry does not reconcile with gameplay accounting");
    f64 attributed_damage = 0;
    std::array<sim::EmissionMetrics,3> emissions{};
    for (const auto& bucket : metrics.buckets()) {
        attributed_damage += bucket.totals.density_removed;
        const usize pool_index = bucket.source == sim::CombatDamageSource::Projectile ? 1 :
                                 bucket.source == sim::CombatDamageSource::Fluid ? 2 : 0;
        emissions[pool_index].requested += bucket.emissions.requested;
        emissions[pool_index].spawned += bucket.emissions.spawned;
        emissions[pool_index].denied += bucket.emissions.denied;
    }
    if (metrics.overflow_count() == 0) {
        result["attributed_density_removed"] = attributed_damage;
        result["attributed_damage"] = std::abs(attributed_damage-totals.damage.density_removed) <=
            std::max(1e-5,std::abs(totals.damage.density_removed)*1e-5);
        if (!result["attributed_damage"].get<bool>()) fail("source-attributed damage does not reconcile with applied damage");
        const sim::SpawnPoolMetrics* pools[] = {&totals.pools.cells,&totals.pools.projectiles,&totals.pools.fluid};
        bool attributed_emissions = true;
        for (usize i = 0; i < emissions.size(); ++i)
            attributed_emissions = attributed_emissions && emissions[i].requested == pools[i]->requested &&
                emissions[i].spawned == pools[i]->spawned && emissions[i].denied == pools[i]->denied;
        result["attributed_emissions"] = attributed_emissions;
        if (!attributed_emissions) fail("source-attributed emissions do not reconcile with requested output");
    } else {
        result["attributed_damage"] = nullptr;
        result["attributed_emissions"] = nullptr;
    }
    const auto snapshot = world.snapshot();
    u64 killed = 0;
    for (u32 family = 0; family < kFamilyCount; ++family) {
        killed += snapshot.chaff_killed_by_family[family];
        const auto& life = metrics.enemy_lifetimes()[family];
        const bool conserved = life.spawned+life.entered_alive == life.alive+life.damage_deaths+
            life.leaked+life.out_of_bounds+life.consumed+life.expired+life.unknown;
        result["enemy_population"][families[family]] = conserved;
        if (!conserved) fail(std::string(families[family])+" lifecycle population does not reconcile");
    }
    result["damage_kills"] = std::abs(totals.damage.damage_kills-static_cast<f64>(killed)) <= 1e-6;
    if (!result["damage_kills"].get<bool>()) fail("damage deaths do not reconcile with gameplay kill accounting");
    return result;
}
json gameplay_conservation(const sim::SimWorld& world,const Economy& economy,json& errors) {
    json result;
    const auto snapshot = world.snapshot();
    for (u32 family = 0; family < kFamilyCount; ++family) {
        const bool conserved = snapshot.chaff_spawned_by_family[family] ==
            snapshot.chaff_killed_by_family[family]+snapshot.chaff_leaked_by_family[family]+
            snapshot.chaff_despawned_by_family[family]+snapshot.chaff_by_family[family];
        result["enemies"][families[family]] = conserved;
        if (!conserved) errors.push_back(std::string(families[family])+" spawned population does not reconcile with deaths exits and survivors");
    }
    const auto ledger = economy.snapshot();
    const u64 credited = static_cast<u64>(ledger.opening_atp)+ledger.damage_income+ledger.bounty_income+ledger.refunds;
    const u64 accounted = static_cast<u64>(ledger.atp)+ledger.total_spent;
    result["economy"] = {{"credited_atp",credited},{"wallet_plus_spent",accounted},{"reconciled",credited == accounted}};
    if (credited != accounted) errors.push_back("ATP source ledger does not reconcile with wallet and spending");
    return result;
}
ControllerParams controller_params(const json& spec) {
    ControllerParams p;
    const json c = spec.value("controller",json::object());
    keys(c,{"policy","reaction_ticks","prep_ticks","reserve_atp","weights","placement_seed","use_abilities"},"controller");
    require(parse_policy(c.value("policy",std::string("competent")),p.policy),"unknown controller policy");
    p.reaction_ticks = static_cast<u32>(integer(c,"reaction_ticks",0,36000));
    p.prep_ticks = static_cast<u32>(integer(c,"prep_ticks",0,36000));
    p.reserve_atp = static_cast<u32>(integer(c,"reserve_atp",0));
    p.use_abilities = c.value("use_abilities",true);
    p.placement_seed = integer(c,"placement_seed",1,std::numeric_limits<u64>::max());
    if (c.contains("weights")) {
        const auto& w = c.at("weights");
        if (w.is_array()) {
            require(w.size() == kTowerTypeCount,"weights array must contain five values");
            for (u32 i = 0; i < kTowerTypeCount; ++i) p.weights[i] = w.at(i).get<f32>();
        } else {
            require(w.is_object(),"weights must be an array or cell-name object");
            p.weights.fill(0);
            for (auto it = w.begin(); it != w.end(); ++it) {
                TowerType type;
                require(parse_tower_type(it.key(),type),"unknown weight cell '"+it.key()+"'");
                p.weights[static_cast<u32>(type)] = it.value().get<f32>();
            }
        }
        for (const auto weight : p.weights)
            require(std::isfinite(weight) && weight >= 0,"weights must be finite and nonnegative");
    }
    return p;
}
Action parse_action(const json& j) {
    keys(j,{"tick","kind","tower","ability","position","quantity","required"},"action");
    Action a;
    a.tick = integer(j,"tick",0);
    const std::string kind = j.at("kind").get<std::string>();
    if (kind == "deploy") {
        a.kind = ActionKind::Deploy;
        require(parse_tower_type(j.at("tower").get<std::string>(),a.tower),"unknown action cell");
        a.quantity = static_cast<u32>(integer(j,"quantity",1,10));
        require(a.quantity > 0,"quantity must be positive");
        a.position = position(j.at("position"));
    } else if (kind == "cast" || kind == "cast_ability") {
        a.kind = ActionKind::CastAbility;
        const std::string ability = j.at("ability").get<std::string>();
        bool found = false;
        for (u32 i = 0; i < kAbilityCount; ++i) {
            if (ability == ability_keys[i] || ability == ability_name(static_cast<AbilityId>(i))) {
                a.ability = static_cast<AbilityId>(i); found = true;
            }
        }
        require(found,"unknown action ability");
        if (j.contains("position")) a.position = position(j.at("position"));
    } else {
        require(kind == "start_wave","unknown action kind '"+kind+"'");
        a.kind = ActionKind::StartWave;
    }
    return a;
}
void initialize_progression(const json& spec, const GameConfig& base, MetaProgression& meta) {
    meta.reset_to_new_game();
    const auto p = spec.value("progression",json::object());
    keys(p,{"save","memory_cells","antibodies","purchases","profile","grant_tree"},"progression");
    if (p.contains("save")) {
        const auto text = platform::read_text_file(p.at("save").get<std::string>());
        require(text.has_value(),"cannot read progression snapshot");
        std::string error;
        require(meta.from_json(*text,error),"invalid progression snapshot: "+error);
    }
    meta.credit(integer(p,"memory_cells",0),static_cast<u32>(integer(p,"antibodies",0)));
    const std::string profile = p.value("profile",std::string("campaign"));
    require(profile == "campaign" || profile == "all_unlocked","unknown progression profile");
    const auto purchases = p.value("purchases",json::array());
    require(purchases.is_array(),"progression purchases must be an array");
    require(profile != "all_unlocked" || purchases.empty(),"all_unlocked baseline cannot apply purchased bonuses");
    for (const auto& item : purchases) {
        keys(item,{"key","levels"},"purchase");
        TreeNode node;
        require(find_tree_node(item.at("key").get<std::string>(),node),"unknown progression purchase");
        const u64 n = integer(item,"levels",1,255);
        require(n > 0,"purchase levels must be positive");
        for (u64 i = 0; i < n; ++i)
            require(meta.purchase(node,base.meta) == MetaProgression::PurchaseResult::Ok,
                    std::string("illegal progression purchase: ")+tree_node(node).key);
    }
    if (p.contains("grant_tree")) {
        require(p.at("grant_tree").is_object(),"grant_tree must be a node-name object");
        require(profile != "all_unlocked","grant_tree bonuses cannot be used with all_unlocked baseline");
        for (auto it = p.at("grant_tree").begin(); it != p.at("grant_tree").end(); ++it) {
            TreeNode n;
            require(find_tree_node(it.key(),n),"unknown grant_tree node '"+it.key()+"'");
            require(it.value().is_number_integer(),"grant_tree ranks must be integers");
            const auto v = it.value().get<i64>();
            require(v >= 0 && v <= tree_node(n).max_level,"grant_tree rank exceeds node maximum");
            meta.debug_set_skill(n,static_cast<u32>(v));
        }
    }
}
struct Deployment {
    EntityId owner;
    TowerType type;
    Tick bought = 0;
    Tick last_seen = 0;
    u32 cost = 0;
    f32 health = 0;
};

json run_level(const json& spec, const GameConfig& base, MetaProgression& meta,
               const CaptureCallback& capture) {
    const auto text = spec.contains("level_document")
        ? std::optional<std::string>(spec.at("level_document").dump())
        : platform::read_text_file(spec.at("level").get<std::string>());
    require(text.has_value(),"cannot read requested level");
    LevelDef level;
    LevelLoader loader;
    const auto loaded = loader.load_string(*text,level);
    require(loaded.ok,"invalid level: "+loaded.error);
    const auto params = controller_params(spec);
    BalanceController controller(params);
    const Tick max_ticks = integer(spec,"max_ticks",54000,2160000);
    const Tick sample_ticks = integer(spec,"sample_ticks",60,36000);
    require(max_ticks > 0 && sample_ticks > 0,"max_ticks and sample_ticks must be positive");
    const bool scripted = spec.contains("actions");
    const bool income_enabled = spec.value("income_enabled",true);
    const u64 spend_limit = integer(spec,"spend_limit",std::numeric_limits<u32>::max(),std::numeric_limits<u32>::max());
    std::vector<std::pair<Action,bool>> scripts;
    if (scripted) {
        require(spec.at("actions").is_array(),"actions must be an array");
        for (const auto& item : spec.at("actions")) {
            auto a = parse_action(item);
            require(a.tick < max_ticks,"action lies outside run duration");
            scripts.emplace_back(a,item.value("required",true));
        }
        std::stable_sort(scripts.begin(),scripts.end(),[](const auto& a,const auto& b) {return a.first.tick < b.first.tick;});
    }
    bool sandbox = spec.value("progression",json::object()).value("profile",std::string("campaign")) == "all_unlocked";
    const auto run = make_run_tuning(base,meta,sandbox);
    sim::SimWorld world;
    TowerSystem towers;
    EnemyRoster enemies;
    Economy economy;
    ActiveAbilitySystem abilities;
    WaveDirector waves;
    apply_enemy_config(enemies,base.enemies);
    auto desc = make_level_sim_desc(base,enemies,level,integer(spec,"seed",1,std::numeric_limits<u64>::max()));
    desc.deterministic_rebakes = spec.value("deterministic_rebakes",true);
    world.init(desc,nullptr);
    const auto instantiated = loader.instantiate(level,world);
    require(instantiated.ok,"level geometry setup failed: "+instantiated.error);
    towers.register_systems(world);
    enemies.register_systems(world);
    abilities.register_systems(world);
    apply_run_tuning(base,run,level,world,towers,enemies,economy,abilities,sandbox);
    if (spec.contains("opening_atp")) {
        auto ec = run.config.economy;
        ec.starting_atp = static_cast<u32>(integer(spec,"opening_atp",0));
        ec.atp_per_density *= level.economy.income_multiplier;
        economy.configure(ec);
    }
    if (spec.contains("allowed_cells")) {
        require(spec.at("allowed_cells").is_array(),"allowed_cells must be an array");
        u32 mask = 0;
        for (const auto& name : spec.at("allowed_cells")) {
            TowerType type;
            require(parse_tower_type(name.get<std::string>(),type),"unknown allowed cell");
            if (towers.tower_allowed(type)) mask |= 1u << static_cast<u32>(type);
        }
        // Use unlock mask for an empty intersection: allowed mask 0 means all.
        towers.set_unlocked_towers(towers.unlocked_towers() & mask);
    }
    if (spec.contains("allowed_abilities")) {
        require(spec.at("allowed_abilities").is_array(),"allowed_abilities must be an array");
        u32 mask = 0;
        for (const auto& name : spec.at("allowed_abilities")) {
            bool found = false;
            for (u32 i = 0; i < kAbilityCount; ++i) {
                if (name == ability_keys[i] || name == ability_name(static_cast<AbilityId>(i))) {
                    mask |= 1u << i; found = true;
                }
            }
            require(found,"unknown allowed ability");
        }
        abilities.set_unlocked(abilities.unlocked_mask() & mask);
    }
    waves.set_waves(level.waves);
    waves.set_auto_start(spec.value("auto_start",false));
    waves.start(world);
    const bool measure = spec.value("combat_metrics",true);
    world.enable_combat_metrics(measure,32768);
    LevelSystems systems;
    systems.world = &world; systems.towers = &towers; systems.enemies = &enemies;
    systems.waves = &waves; systems.abilities = &abilities;
    systems.economy = income_enabled ? &economy : nullptr;
    systems.survive_seconds = level.win.survive_seconds;
    json report = {{"schema",1},{"kind","run"},{"spec",spec},
        {"actions",json::array()},{"timeline",json::array()},{"waves",json::array()},
        {"diagnostics",{{"errors",json::array()},{"warnings",json::array()}}}};
    report["provenance"] = {{"level_hash",fingerprint(*text)},
        {"base_config_hash",fingerprint(json(dump_game_config(base)).dump())},
        {"effective_config_hash",fingerprint(json(dump_game_config(run.config)).dump()+meta.to_json())},
        {"seed",desc.seed},{"threads",1},{"deterministic_rebakes",desc.deterministic_rebakes},
        {"base_config",dump_game_config(base)},{"effective_config",dump_game_config(run.config)},
        {"progression",json::parse(meta.to_json())},
        {"synthetic_progression",spec.value("progression",json::object()).contains("grant_tree") || sandbox},
        {"experimental_rules",spec.contains("opening_atp") || !income_enabled || spec.contains("spend_limit") || spec.contains("allowed_cells") || spec.contains("allowed_abilities")},
        {"immunity",{{"leak_damage",run.tree.immunity.leak_damage},
            {"slowed_damage_mult",run.tree.immunity.slowed_damage_mult},
            {"hostile_damage_taken_mult",run.tree.hostile_damage_taken_mult}}}};
    report["replay_spec"] = spec;
    report["replay_spec"]["level_document"] = json::parse(*text);
    report["replay_spec"]["progression"] = {{"profile",sandbox ? "all_unlocked" : "campaign"}};
    report["replay_spec"]["progression_snapshot"] = json::parse(meta.to_json());
    report["replay_spec"].erase("overrides");
    std::vector<Deployment> deployments;
    std::unordered_map<u32,usize> owner_index;
    f64 density_removed = 0, slow_enemy_seconds = 0, danger_seconds = 0, atp_area = 0;
    Tick unaffordable_ticks = 0, waiting_ticks = 0;
    u64 peak = 0, rejected = 0, deploy_capacity_rejected = 0;
    u32 casts = 0;
    std::array<u64,kTowerTypeCount> bought{}, spent{};
    std::array<f64,kTowerTypeCount> cell_seconds{};
    std::vector<json> intervals(level.waves.size());
    for (usize i = 0; i < intervals.size(); ++i)
        intervals[i] = {{"index",i},{"name",level.waves[i].name},{"ticks",0},
            {"density_removed",0.0},{"leaks",0},{"kills",0},{"peak_alive",0}};
    auto sample = [&]() {
        auto s = state_json(world);
        s["economy"] = economy_json(economy);
        s["wave_index"] = waves.status().wave_index;
        s["phase"] = static_cast<u32>(waves.status().phase);
        u64 slow = 0, danger = 0;
        const auto& chaff = world.chaff();
        for (usize i = 0; i < chaff.count(); ++i) {
            if (chaff.slow_remaining[i] > 0 && chaff.slow_factor[i] < 1) ++slow;
            const Vec2 p{chaff.pos_x[i],chaff.pos_y[i]};
            for (const auto& objective : level.objectives) {
                const Vec2 d = p-objective.position;
                if (d.x*d.x+d.y*d.y < 1600) { ++danger; break; }
            }
        }
        s["slowed_enemies"] = slow; s["enemies_near_objective"] = danger;
        report["timeline"].push_back(std::move(s));
    };
    sample();
    auto act = [&](const Action& a,bool required) {
        json entry = {{"tick",world.tick_index()},{"kind",action_kind_name(a.kind)},
            {"position",{a.position.x,a.position.y}},{"accepted",false},{"required",required}};
        bool ok = false;
        if (a.kind == ActionKind::Deploy) {
            const u32 cost = towers.stats(a.tower).build_cost;
            const u64 available = spend_limit > economy.snapshot().total_spent
                ? spend_limit-economy.snapshot().total_spent : 0;
            const auto quantity = static_cast<u32>(std::min<u64>(a.quantity,cost ? available/cost : 0));
            entry["tower"] = tower_type_name(a.tower);
            entry["quantity"] = a.quantity;
            entry["owner_ids"] = json::array();
            u32 placed = 0;
            const auto validation = towers.validate_deploy(world,a.tower,a.position,economy.atp());
            for (u32 i = 0; i < quantity; ++i) {
                if (deploy_cells(towers,world,economy,a.tower,a.position,1) == 0) break;
                const auto owner = towers.last_deployment_id();
                owner_index[owner.value] = deployments.size();
                deployments.push_back({owner,a.tower,world.tick_index(),world.tick_index(),cost,0});
                world.combat_metrics().register_owner(owner,a.tower);
                entry["owner_ids"].push_back(owner.value);
                ++placed; ++bought[static_cast<u32>(a.tower)]; spent[static_cast<u32>(a.tower)] += cost;
            }
            entry["deployed"] = placed; entry["atp_spent"] = placed*cost;
            entry["placement_result"] = static_cast<u32>(validation.result);
            const u32 capacity_refused = placed < quantity && world.swarmers().full() ? quantity-placed : 0;
            deploy_capacity_rejected += capacity_refused;
            entry["capacity_rejected"] = capacity_refused;
            entry["reason"] = quantity < a.quantity ? "spend_limit" : placed == a.quantity ? "ok" :
                capacity_refused ? "cell_capacity" : "deployment_refused";
            ok = placed == a.quantity;
        } else if (a.kind == ActionKind::CastAbility) {
            entry["ability"] = ability_keys[static_cast<u32>(a.ability)];
            ok = abilities.cast(world,a.ability,a.position);
            entry["reason"] = ok ? "ok" : "ability_refused";
            if (ok) ++casts;
        } else {
            ok = waves.status().phase == WavePhase::Prep && !waves.status().all_waves_complete;
            if (ok) waves.request_early_start();
            entry["reason"] = ok ? "ok" : "wave_not_in_prep";
        }
        entry["accepted"] = ok;
        report["actions"].push_back(entry);
        if (!ok) {
            ++rejected;
            if (required) report["diagnostics"]["errors"].push_back("required action refused at tick "+std::to_string(world.tick_index()));
        }
    };
    SessionOutcome outcome = SessionOutcome::InProgress;
    usize script_index = 0;
    auto before = world.snapshot();
    for (Tick tick = 0; tick < max_ticks; ++tick) {
        if (scripted) {
            while (script_index < scripts.size() && scripts[script_index].first.tick == tick) {
                act(scripts[script_index].first,scripts[script_index].second); ++script_index;
            }
        } else {
            const u64 remaining = spend_limit > economy.snapshot().total_spent
                ? spend_limit-economy.snapshot().total_spent : 0;
            for (const auto& action : controller.decide({world,level,towers,economy,waves,abilities,remaining})) act(action,false);
        }
        if (!report["diagnostics"]["errors"].empty()) break;
        const auto status = waves.status();
        if (status.phase == WavePhase::Prep) ++waiting_ticks;
        u32 cheapest = std::numeric_limits<u32>::max();
        for (u32 t = 0; t < kTowerTypeCount; ++t) {
            const auto type = static_cast<TowerType>(t);
            if (towers.tower_unlocked(type) && towers.tower_allowed(type) && params.weights[t] > 0)
                cheapest = std::min(cheapest,towers.stats(type).build_cost);
        }
        if (cheapest != std::numeric_limits<u32>::max() && economy.atp() < cheapest) ++unaffordable_ticks;
        atp_area += economy.atp()*kFixedDtSeconds;
        const auto& starting_friends = world.swarmers();
        for (usize i = 0; i < starting_friends.count(); ++i) {
            if (starting_friends.health[i] <= 0 || (starting_friends.flags[i]&sim::swarmer_flags::kPendingKill)) continue;
            const auto found = owner_index.find(starting_friends.owner[i].value);
            if (found != owner_index.end()) cell_seconds[static_cast<u32>(deployments[found->second].type)] += kFixedDtSeconds;
        }
        outcome = step_level(systems);
        // Fixed-wallet experiments still drain rewards so nothing queues up.
        if (!income_enabled) (void)waves.take_pending_atp_reward();
        const auto after = world.snapshot();
        density_removed += world.last_damage_stats().density_removed;
        peak = std::max(peak,after.chaff_count);
        if (status.wave_index < intervals.size()) {
            auto& row = intervals[status.wave_index];
            row["ticks"] = row["ticks"].get<u64>()+1;
            row["density_removed"] = row["density_removed"].get<f64>()+world.last_damage_stats().density_removed;
            row["leaks"] = row["leaks"].get<u64>()+after.chaff_leaked_total-before.chaff_leaked_total;
            row["peak_alive"] = std::max(row["peak_alive"].get<u64>(),after.chaff_count);
            u64 deaths = 0;
            for (u32 f = 0; f < kFamilyCount; ++f) deaths += after.chaff_killed_by_family[f]-before.chaff_killed_by_family[f];
            row["kills"] = row["kills"].get<u64>()+deaths;
        }
        const auto& sw = world.swarmers();
        for (usize i = 0; i < sw.count(); ++i) {
            if (sw.health[i] <= 0 || (sw.flags[i]&sim::swarmer_flags::kPendingKill)) continue;
            const auto it = owner_index.find(sw.owner[i].value);
            if (it != owner_index.end()) {
                auto& d = deployments[it->second]; d.last_seen = world.tick_index(); d.health = sw.health[i];
            }
        }
        // Cosmetic buffers must never accumulate indefinitely in a headless run.
        world.combat_events().clear();
        if (world.tick_index()%sample_ticks == 0 || outcome != SessionOutcome::InProgress) sample();
        before = after;
        if (outcome != SessionOutcome::InProgress) break;
    }
    if (report["timeline"].back().at("tick").get<Tick>() != world.tick_index()) sample();
    else if (!report["diagnostics"]["errors"].empty()) { report["timeline"].erase(report["timeline"].end()-1); sample(); }
    for (usize i = 1; i < report["timeline"].size(); ++i) {
        const auto& a = report["timeline"][i-1]; const auto& b = report["timeline"][i];
        const f64 dt = (b.at("tick").get<Tick>()-a.at("tick").get<Tick>())*kFixedDtSeconds;
        slow_enemy_seconds += a.at("slowed_enemies").get<f64>()*dt;
        if (a.at("enemies_near_objective").get<u64>() > 0) danger_seconds += dt;
    }
    u64 denied = 0, timeout_advances = 0;
    Tick interval_start = 0;
    for (usize i = 0; i < waves.metrics().size(); ++i) {
        const auto& m = waves.metrics()[i];
        auto row = intervals[i];
        row["start_tick"] = interval_start;
        interval_start += row.at("ticks").get<Tick>();
        row["end_tick"] = interval_start;
        row["requested"] = m.requested; row["spawned"] = m.spawned; row["denied"] = m.denied;
        row["completed"] = m.completed; row["clearing_timeout"] = m.clearing_timeout;
        row["survivors_on_completion"] = m.survivors_on_completion;
        row["accounting_scope"] = "activity during wave interval; carryover may belong to earlier waves";
        report["waves"].push_back(std::move(row));
        denied += m.denied; timeout_advances += m.clearing_timeout ? 1 : 0;
    }
    if (denied) report["diagnostics"]["errors"].push_back("authored enemy spawns were denied");
    if (measure && world.combat_metrics().overflow_count()) report["diagnostics"]["errors"].push_back("combat attribution capacity exceeded");
    if (world.combat_metrics().totals().replications_denied_capacity)
        report["diagnostics"]["errors"].push_back("replication was suppressed by enemy capacity");
    report["combat_conservation"] = combat_conservation(world,density_removed,report["diagnostics"]["errors"]);
    report["conservation"] = gameplay_conservation(world,economy,report["diagnostics"]["errors"]);
    if (!measure) report["diagnostics"]["warnings"].push_back("combat telemetry disabled; contribution and pool-capacity evidence unavailable");
    if (deploy_capacity_rejected) report["diagnostics"]["warnings"].push_back("player deployment requests were rejected by cell capacity");
    if (timeout_advances) report["diagnostics"]["warnings"].push_back("wave director advanced with surviving enemies after clearing grace period");
    if (script_index < scripts.size()) report["diagnostics"]["warnings"].push_back("run ended before all scheduled actions");
    const bool valid = report["diagnostics"]["errors"].empty();
    const char* status = !valid ? "invalid" : outcome == SessionOutcome::Cleared ? "cleared"
        : outcome == SessionOutcome::ObjectiveDestroyed ? "defeated" : "timeout";
    const auto state = state_json(world);
    const auto result = collect_run_result(world,waves,outcome == SessionOutcome::Cleared);
    RunReward reward;
    if (valid && !sandbox && outcome != SessionOutcome::InProgress)
        reward = meta.record_run(level.name,result,base.meta);
    report["summary"] = {{"status",status},{"valid",valid},{"duration_ticks",world.tick_index()},
        {"duration_seconds",world.tick_index()*kFixedDtSeconds},{"objective_integrity",state.at("integrity")},
        {"waves_cleared",result.waves_cleared},{"wave_reached",waves.status().wave_index},
        {"kills",state.at("killed")},{"leaks",state.at("leaked")},{"peak_enemies",peak},
        {"atp_spent",economy.snapshot().total_spent},{"atp_remaining",economy.atp()},
        {"memory_cells",reward.memory_cells},{"antibodies",reward.antibodies},
        {"density_removed",density_removed},{"friendly_killed",state.at("friendly_killed")},
        {"friendly_alive",state.at("friendly_alive")},{"abilities_used",casts},{"actions_rejected",rejected},
        {"deploy_capacity_rejected",deploy_capacity_rejected},{"combat_metrics_available",measure},
        {"clearing_timeout_advances",timeout_advances},{"denied_spawns",denied},
        {"unable_to_afford_seconds",unaffordable_ticks*kFixedDtSeconds},
        {"prep_seconds",waiting_ticks*kFixedDtSeconds},{"atp_seconds_unspent",atp_area},
        {"sampled_slow_enemy_seconds",slow_enemy_seconds},{"sampled_danger_seconds",danger_seconds}};
    report["state"] = state; report["state_hash"] = world.state_hash();
    const auto wave_status = waves.status();
    report["wave_status"] = {{"wave_index",wave_status.wave_index},
        {"phase",static_cast<u32>(wave_status.phase)},{"phase_time_remaining",wave_status.phase_time_remaining},
        {"remaining_to_spawn",wave_status.remaining_to_spawn},{"all_waves_complete",wave_status.all_waves_complete}};
    report["economy"] = economy_json(economy);
    report["combat"] = metrics_json(world.combat_metrics(),world);
    report["units"] = json::object();
    for (u32 t = 0; t < kTowerTypeCount; ++t)
        report["units"][tower_type_name(static_cast<TowerType>(t))] =
            {{"deployed",bought[t]},{"atp_spent",spent[t]},{"alive",0},{"remaining_hp",0.0},{"maximum_hp_alive",0.0},{"observed_cell_seconds",cell_seconds[t]},
             {"telemetry_available",measure},{"density_removed",measure ? json(0.0) : json(nullptr)},
             {"damage_kills",measure ? json(0.0) : json(nullptr)},
             {"projectiles",measure ? projectile_json({}) : json(nullptr)},
             {"fluid_emissions",measure ? emission_json({}) : json(nullptr)},
             {"cell_emissions",measure ? emission_json({}) : json(nullptr)}};
    if (measure) for (u32 t = 0; t < kTowerTypeCount; ++t) {
        auto& row = report["units"][tower_type_name(static_cast<TowerType>(t))];
        row["projectiles"].update(emission_json({}));
        row["projectiles"]["live_at_end"] = 0;
        row["activity"] = activity_json(world.combat_metrics().activity()[t]);
    }
    const auto& remaining_friends = world.swarmers();
    for (usize i = 0; i < remaining_friends.count(); ++i) {
        if (remaining_friends.health[i] <= 0 || (remaining_friends.flags[i]&sim::swarmer_flags::kPendingKill)) continue;
        const auto found = owner_index.find(remaining_friends.owner[i].value);
        if (found != owner_index.end()) {
            auto& row = report["units"][tower_type_name(deployments[found->second].type)];
            row["alive"] = row["alive"].get<u64>()+1;
            row["remaining_hp"] = row["remaining_hp"].get<f64>()+remaining_friends.health[i];
            row["maximum_hp_alive"] = row["maximum_hp_alive"].get<f64>()+remaining_friends.profile_of(i).max_health;
        }
    }
    for (const auto& bucket : world.combat_metrics().buckets()) {
        if (static_cast<u32>(bucket.type) >= kTowerTypeCount) continue;
        auto& row = report["units"][tower_type_name(bucket.type)];
        row["density_removed"] = row["density_removed"].get<f64>()+bucket.totals.density_removed;
        row["damage_kills"] = row["damage_kills"].get<f64>()+bucket.totals.damage_kills;
        if (bucket.source == sim::CombatDamageSource::Projectile) {
            add_emissions(row["projectiles"],bucket.emissions);
            add_projectile_outcomes(row["projectiles"],bucket.projectile_outcomes);
        } else if (bucket.source == sim::CombatDamageSource::Fluid) add_emissions(row["fluid_emissions"],bucket.emissions);
        else if (bucket.source == sim::CombatDamageSource::Unknown) add_emissions(row["cell_emissions"],bucket.emissions);
    }
    if (measure) for (usize i = 0; i < world.projectiles().count(); ++i) {
        auto type = world.projectiles().source[i];
        if (type == TowerType::Count) type = world.combat_metrics().owner_type(world.projectiles().owner[i]);
        if (static_cast<u32>(type) >= kTowerTypeCount) continue;
        auto& shots = report["units"][tower_type_name(type)]["projectiles"];
        shots["live_at_end"] = shots["live_at_end"].get<u64>()+1;
    }
    std::array<sim::LifecycleMetrics,kTowerTypeCount> friendly{};
    for (const auto& bucket : world.combat_metrics().friendly_lifetimes()) {
        if (static_cast<u32>(bucket.type) < kTowerTypeCount)
            merge_lifecycle(friendly[static_cast<u32>(bucket.type)],bucket.stats);
    }
    if (measure) for (u32 t = 0; t < kTowerTypeCount; ++t)
        report["units"][tower_type_name(static_cast<TowerType>(t))]["lifecycle"] = lifecycle_json(friendly[t]);
    for (u32 f = 0; f < kFamilyCount; ++f) {
        report["enemies"][families[f]] = state.at("families").at(families[f]);
        report["enemies"][families[f]]["telemetry_available"] = measure;
        if (measure) report["enemies"][families[f]]["lifecycle"] = lifecycle_json(world.combat_metrics().enemy_lifetimes()[f]);
    }
    report["reward"] = {{"memory_cells",reward.memory_cells},{"antibodies",reward.antibodies},{"first_clear",reward.first_clear}};
    report["discovery"] = {{"available_choices",0},{"offers",json::array()}};
    for (const auto& offer : meta.discovery_offers(base.meta)) {
        json prerequisite = nullptr;
        if (offer.prerequisite != TreeNode::Count)
            prerequisite = {{"key",tree_node(offer.prerequisite).key},
                            {"cost",{{"memory_cells",offer.prerequisite_cost.memory_cells},
                                      {"antibodies",offer.prerequisite_cost.antibodies}}}};
        report["discovery"]["offers"].push_back({{"key",tree_node(offer.node).key},
            {"cost",{{"memory_cells",offer.cost.memory_cells},{"antibodies",offer.cost.antibodies}}},
            {"prerequisite",std::move(prerequisite)}});
    }
    report["discovery"]["available_choices"] = report["discovery"]["offers"].size();
    report["progression_after"] = json::parse(meta.to_json());
    report["replay_spec"]["actions"] = json::array();
    for (const auto& a : report["actions"]) {
        if (!a.at("accepted").get<bool>() && a.value("deployed",0u) == 0) continue;
        json replay = {{"tick",a.at("tick")},{"kind",a.at("kind")},{"position",a.at("position")}};
        if (a.contains("tower")) { replay["tower"] = a.at("tower"); replay["quantity"] = a.at("deployed"); }
        if (a.contains("ability")) replay["ability"] = a.at("ability");
        report["replay_spec"]["actions"].push_back(std::move(replay));
    }
    if (capture) capture({world,level,economy,waves,towers,abilities});
    return report;
}

json buy_priorities(MetaProgression& meta, const GameConfig& base, const json& policy) {
    keys(policy,{"priority","repeat"},"purchase_policy");
    json purchases = json::array();
    const auto priority = policy.value("priority",json::array({"neutrophil.round_damage","hub.bone_marrow_reserve","hub.efficient_clearance"}));
    require(priority.is_array(),"purchase priority must be an array of node keys");
    std::vector<TreeNode> nodes;
    for (const auto& key : priority) {
        TreeNode node;
        require(find_tree_node(key.get<std::string>(),node),"unknown campaign purchase key: "+key.get<std::string>());
        nodes.push_back(node);
    }
    for (u32 pass = 0; pass < 256; ++pass) {
        bool bought = false;
        for (const auto node : nodes) {
            const auto cost = meta.next_cost(node,base.meta);
            if (meta.purchase(node,base.meta) == MetaProgression::PurchaseResult::Ok) {
                purchases.push_back({{"key",tree_node(node).key},{"level",meta.level(node)},
                    {"memory_cells",cost.memory_cells},{"antibodies",cost.antibodies}});
                bought = true;
            }
        }
        if (!bought || !policy.value("repeat",true)) break;
    }
    return purchases;
}
} // namespace

json execute(const json& input, const GameConfig& loaded_base, const CaptureCallback& capture,
             const AttemptCallback& attempt_completed) {
    json output;
    try {
        keys(input,{"schema","name","description","level","level_document","seed","max_ticks","sample_ticks",
            "controller","progression","progression_snapshot","overrides","opening_atp","income_enabled","spend_limit",
            "allowed_cells","allowed_abilities","actions","auto_start","combat_metrics","deterministic_rebakes","campaign","config_dir"},"run specification");
        require(integer(input,"schema",0) == 1,"run specification schema must be 1");
        GameConfig base = loaded_base;
        config::Registry registry;
        bind_game_config(registry,base);
        const auto overrides = input.value("overrides",json::object());
        require(overrides.is_object(),"overrides must be a registry-path object");
        for (auto it = overrides.begin(); it != overrides.end(); ++it) {
            std::string error;
            const auto v = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
            require(registry.set(it.key(),v,error),"config override failed: "+error);
        }
        for (u32 t = 0; t < kTowerTypeCount; ++t)
            require(base.towers.stats[t].build_cost > 0,"cell build cost must be positive");
        MetaProgression meta;
        initialize_progression(input,base,meta);
        if (input.contains("progression_snapshot")) {
            std::string error;
            require(meta.from_json(input.at("progression_snapshot").dump(),error),"invalid replay progression: "+error);
        }
        if (!input.contains("campaign")) return run_level(input,base,meta,capture);
        require(!capture,"campaign capture requires choosing an individual attempt replay");
        const auto& campaign = input.at("campaign");
        keys(campaign,{"levels","purchase_policy","max_attempts_per_level","max_total_attempts"},"campaign");
        const auto levels = campaign.at("levels");
        require(levels.is_array() && !levels.empty(),"campaign needs an ordered nonempty levels array");
        const u64 per_level = integer(campaign,"max_attempts_per_level",5,1000);
        const u64 total_limit = integer(campaign,"max_total_attempts",50,10000);
        require(per_level > 0 && total_limit > 0,"campaign attempt limits must be positive");
        const auto policy = campaign.value("purchase_policy",json::object());
        output = {{"schema",1},{"kind","campaign"},{"spec",input},{"attempts",json::array()},
            {"purchases",json::array()},{"diagnostics",{{"errors",json::array()}}}};
        json level_documents = json::array(), level_hashes = json::array();
        for (const auto& path : levels) {
            const auto text = platform::read_text_file(path.get<std::string>());
            require(text.has_value(),"cannot read campaign level: "+path.get<std::string>());
            const auto document = json::parse(*text);
            LevelDef check;
            const auto parsed = LevelLoader{}.load_string(*text,check);
            require(parsed.ok,"invalid campaign level: "+parsed.error);
            level_documents.push_back(document); level_hashes.push_back(fingerprint(*text));
        }
        u64 ticks = 0, cleared = 0;
        std::string status = "completed";
        for (usize l = 0; l < levels.size(); ++l) {
            bool won = false;
            for (u64 a = 0; a < per_level; ++a) {
                if (output["attempts"].size() >= total_limit) { status = "stalled"; break; }
                const auto purchases = buy_priorities(meta,base,policy);
                for (auto purchase : purchases) {
                    purchase["before_attempt"] = output["attempts"].size(); output["purchases"].push_back(purchase);
                }
                auto spec = input; spec.erase("campaign"); spec.erase("overrides"); spec.erase("level_document");
                spec["level"] = levels.at(l); spec["level_document"] = level_documents.at(l);
                const u64 seed = integer(input,"seed",1,std::numeric_limits<u64>::max());
                require(seed <= std::numeric_limits<u64>::max()-output["attempts"].size(),"campaign seed overflow");
                spec["seed"] = seed + output["attempts"].size();
                const auto before = json::parse(meta.to_json());
                auto attempt = run_level(spec,base,meta,{});
                attempt["attempt_index"] = output["attempts"].size(); attempt["level_index"] = l;
                attempt["level_attempt"] = a+1; attempt["progression_before"] = before;
                attempt["purchases"] = purchases;
                ticks += attempt.at("summary").at("duration_ticks").get<u64>();
                const auto outcome = attempt.at("summary").at("status").get<std::string>();
                output["attempts"].push_back(std::move(attempt));
                if (attempt_completed) attempt_completed(output["attempts"].back());
                if (outcome == "cleared") { won = true; ++cleared; break; }
                if (outcome == "invalid" || outcome == "timeout") { status = outcome; break; }
            }
            if (!won) { if (status == "completed") status = "stalled"; break; }
        }
        output["summary"] = {{"status",status},{"valid",status != "invalid"},
            {"levels_cleared",cleared},{"levels_total",levels.size()},{"attempts",output["attempts"].size()},
            {"duration_ticks",ticks},{"duration_seconds",ticks*kFixedDtSeconds},
            {"memory_cells",meta.memory_cells()},{"antibodies",meta.antibodies()}};
        output["final_progression"] = json::parse(meta.to_json());
        output["provenance"] = {{"base_config_hash",fingerprint(json(dump_game_config(base)).dump())},
            {"initial_progression",input.value("progression",json::object())},
            {"level_documents",level_documents},{"level_hashes",level_hashes},
            {"seed",integer(input,"seed",1,std::numeric_limits<u64>::max())}};
        return output;
    } catch (const std::exception& error) {
        output = {{"schema",1},{"kind",input.contains("campaign") ? "campaign" : "run"},
            {"summary",{{"status","invalid"},{"valid",false}}},
            {"diagnostics",{{"errors",json::array({error.what()})}}},{"spec",input}};
        return output;
    }
}
} // namespace immune::game::balance
