#include "game/balance/BalanceController.h"

#include "core/Math.h"
#include "game/economy/Economy.h"
#include "game/level/Level.h"
#include "game/towers/TowerMechanics.h"
#include "game/towers/TowerSystem.h"
#include "game/wave/WaveDirector.h"
#include "sim/SimWorld.h"
#include "sim/ecs/Components.h"
#include "sim/flowfield/RuntimeBlock.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace immune::game::balance {
namespace {

u64 mix(u64 value) {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

f32 unit_hash(u64 value) {
    return static_cast<f32>(mix(value) >> 40) / 16777216.0f;
}

u32 default_reaction(Policy policy) {
    if (policy == Policy::Novice) return 90;
    if (policy == Policy::Tactical) return 15;
    return 30;
}

u32 default_prep(Policy policy) {
    if (policy == Policy::Novice) return 540;
    if (policy == Policy::Tactical) return 240;
    return 360;
}

struct RouteBranch {
    std::vector<Vec2> points;
    f32 weight = 0.0f;
};
struct PreviewRoute : RouteBranch {
    std::vector<RouteBranch> branches;
    std::vector<RouteBranch> navigation_branches;
    Vec2 entrance{0.0f, 0.0f};
    Vec2 intercept{0.0f, 0.0f};
    bool shared_intercept = false;
    bool navigation_available = false;
};

struct RouteProjection {
    f32 distance_sq = std::numeric_limits<f32>::infinity();
    f32 along = 0.0f;
    f32 remaining = 0.0f;
};

RouteProjection project_route(Vec2 p, const std::vector<Vec2>& points) {
    RouteProjection projection;
    f32 along = 0.0f;
    for (usize i = 1; i < points.size(); ++i) {
        const Vec2 direction = points[i] - points[i - 1];
        const f32 length_sq = math::length_sq(direction);
        const f32 length = std::sqrt(length_sq);
        const Vec2 offset = p - points[i - 1];
        const f32 t = length_sq > 0.0001f ?
            std::clamp((offset.x * direction.x + offset.y * direction.y) / length_sq, 0.0f, 1.0f) : 0.0f;
        const f32 distance_sq = math::length_sq(p - (points[i - 1] + direction * t));
        if (distance_sq < projection.distance_sq) projection = {distance_sq, along + length * t};
        along += length;
    }
    projection.remaining = std::max(0.0f, along - projection.along);
    return projection;
}

std::vector<Vec2> simplify_navigation(const std::vector<Vec2>& points, f32 tolerance) {
    if (points.size() < 3) return points;
    std::vector<bool> keep(points.size(), false);
    keep.front() = keep.back() = true;
    std::vector<std::pair<usize, usize>> pending{{0, points.size() - 1}};
    while (!pending.empty()) {
        const auto [first, last] = pending.back();
        pending.pop_back();
        const Vec2 delta = points[last] - points[first];
        const f32 length_sq = math::length_sq(delta);
        f32 farthest = tolerance * tolerance;
        usize split = first;
        for (usize i = first + 1; i < last; ++i) {
            const Vec2 offset = points[i] - points[first];
            const f32 t = length_sq > 0.0001f ? std::clamp(
                (offset.x * delta.x + offset.y * delta.y) / length_sq, 0.0f, 1.0f) : 0.0f;
            const f32 distance = math::length_sq(points[i] - (points[first] + delta * t));
            if (distance > farthest) { farthest = distance; split = i; }
        }
        if (split == first) continue;
        keep[split] = true;
        pending.push_back({first, split});
        pending.push_back({split, last});
    }
    std::vector<Vec2> result;
    for (usize i = 0; i < points.size(); ++i) if (keep[i]) result.push_back(points[i]);
    return result;
}

// Public navigation has forward authority; squad paths supply lateral cohesion.
std::vector<Vec2> trace_navigation(const ControllerView& view, Vec2 start) {
    const auto& flow = view.world.flow();
    const f32 step = std::clamp(flow.cell_size() * 1.5f, 1.0f, 3.0f);
    const Rect bounds = view.world.desc().sim_bounds;
    std::vector<Vec2> points{start};
    Vec2 position = start;
    bool arrived = false;
    f32 best_cost = view.world.flow().sample_cost(start);
    u32 stagnant = 0;
    for (u32 iteration = 0; iteration < 2048; ++iteration) {
        const f32 cost = flow.sample_cost(position);
        if (std::isfinite(cost) && cost <= flow.cell_size() * 0.5f) {
            arrived = true;
            break;
        }
        Vec2 direction = flow.sample(position);
        if (math::length_sq(direction) <= math::kEpsilon) direction = view.world.sdf().gradient(position);
        direction = math::normalize_safe(direction);
        if (math::length_sq(direction) <= math::kEpsilon) break;
        Vec2 next = position + direction * step;
        // Hostiles project out of wall contact and retain tangential travel.
        // Simply halving a step stalls a trace at a broad chamber's rim and
        // misses the outgoing vessel that the real horde slides toward.
        const f32 margin = std::max(0.25f, flow.cell_size() * 0.5f);
        for (u32 correction = 0; correction < 4; ++correction) {
            const f32 clearance = view.world.sdf().sample(next);
            if (clearance >= margin) break;
            const Vec2 normal = math::normalize_safe(view.world.sdf().gradient(next));
            if (math::length_sq(normal) <= math::kEpsilon) break;
            next += normal * (margin - clearance);
        }
        sim::contain_to_walkable(view.world.tissue(), next, position);
        if (!bounds.contains(next) || view.world.sdf().sample(next) <= 0.0f ||
            math::length_sq(next - position) < 0.01f) break;
        const f32 next_cost = flow.sample_cost(next);
        if (!std::isfinite(next_cost)) break;
        if (next_cost < best_cost - 0.01f) { best_cost = next_cost; stagnant = 0; }
        else if (++stagnant >= 64) break;
        position = next;
        points.push_back(position);
    }
    // A truncated trace must not promise that an unobserved bypass is covered.
    if (!arrived || points.size() < 2) return {};
    auto simplified = simplify_navigation(points, std::max(1.0f, flow.cell_size()));
    if (simplified.size() > 96) return {};
    return simplified;
}

f32 engagement_reach(const sim::SwarmerProfile& profile) {
    // Short-attach mobile cells acquire and travel to their target. They are
    // useful at the shared cohort even though they do not attack from there.
    return std::max(2.0f, profile.kind == sim::SwarmerKind::Shooter ||
        profile.kind == sim::SwarmerKind::ArborGrabber ? profile.attach_radius : profile.search_radius);
}

void add_branch(PreviewRoute& approach, const RouteBranch& branch) {
    if (branch.points.size() < 2) return;
    for (auto& existing : approach.branches) {
        if (existing.points != branch.points) continue;
        existing.weight += branch.weight;
        return;
    }
    approach.branches.push_back(branch);
}

std::vector<PreviewRoute> preview_routes(const ControllerView& view, const WaveStatus& wave) {
    std::vector<PreviewRoute> routes;
    if (wave.wave_index >= view.waves.waves().size()) return routes;
    const auto& preview = view.waves.waves()[wave.wave_index];
    f32 total_count = 0.0f;
    for (const auto& entry : preview.spawns) total_count += static_cast<f32>(entry.count);
    if (total_count <= 0.0f) return routes;
    for (const auto& entry : preview.spawns) {
        const f32 weight = static_cast<f32>(entry.count) / total_count;
        if (!entry.squad_paths.empty()) {
            for (const auto& path : view.level.squad_paths) {
                if (std::find(entry.squad_paths.begin(), entry.squad_paths.end(), path.id) != entry.squad_paths.end()) {
                    PreviewRoute route;
                    route.points = path.points;
                    route.weight = weight / static_cast<f32>(entry.squad_paths.size());
                    routes.push_back(std::move(route));
                }
            }
        } else {
            std::vector<std::string> lanes;
            for (const auto& spawn : view.level.spawn_points) {
                if (entry.spawn_point_id.empty() || entry.spawn_point_id == spawn.id) {
                    lanes.push_back(LevelLoader{}.resolve_spawn_point_lane_id(view.level, spawn));
                }
            }
            for (const auto& vessel : view.level.vessels) {
                const std::string& lane = vessel.lane_id.empty() ? vessel.id : vessel.lane_id;
                if (std::find(lanes.begin(), lanes.end(), lane) == lanes.end()) continue;
                PreviewRoute route;
                route.weight = weight / static_cast<f32>(std::max<usize>(1, lanes.size()));
                for (const auto& point : vessel.points) route.points.push_back(point.position);
                routes.push_back(std::move(route));
            }
        }
    }
    // Alternative authored paths from the same entrance are one approach at
    // its opening. Allocating a separate cohort to each fork would needlessly
    // split cells that can intercept both paths before that fork.
    std::vector<PreviewRoute> approaches;
    for (auto& route : routes) {
        if (route.points.empty()) continue;
        bool merged = false;
        for (auto& approach : approaches) {
            if (math::length_sq(approach.points.front() - route.points.front()) > 16.0f) continue;
            approach.weight += route.weight;
            add_branch(approach, route);
            merged = true;
            break;
        }
        if (!merged) {
            add_branch(route, route);
            approaches.push_back(std::move(route));
        }
    }
    // Map alternatives are public geometry even when this wave uses only one
    // side of a fork. Preserve them without borrowing future counts or timers.
    for (auto& approach : approaches) for (const auto& path : view.level.squad_paths) {
        if (path.points.empty() || math::length_sq(approach.points.front() - path.points.front()) > 16.0f) continue;
        add_branch(approach, {path.points, 0.0f});
    }
    f32 approach_weight = 0.0f;
    for (const auto& approach : approaches) approach_weight += approach.weight;
    if (approach_weight > 0.0f) {
        for (auto& approach : approaches) approach.weight /= approach_weight;
    }
    for (auto& approach : approaches) approach.entrance = approach.points.front();
    return approaches;
}

/// Coarse observations bound the cost of a decision even with large hordes.
/// Values are accumulated from currently visible agents rather than querying
/// a spatial hash whose indices can precede the last compaction.
struct Board {
    struct Bin {
        std::array<f32, kFamilyCount> enemies{};
        f32 friends = 0.0f;
        f32 missing_health = 0.0f;
        Vec2 weighted_position{0.0f, 0.0f};
    };
    struct Friend { Vec2 position; f32 reach; TowerType type; };

    Rect bounds;
    f32 cell = 10.0f;
    i32 width = 1;
    i32 height = 1;
    std::vector<Bin> bins;
    std::array<f32, kTowerTypeCount> friendly_types{};
    std::vector<Friend> friendly_positions;
    f32 visible_density = 0.0f;
    f32 friends = 0.0f;
    f32 missing_health = 0.0f;
    f32 max_health = 0.0f;

    explicit Board(const ControllerView& view) : bounds(view.world.desc().world_bounds) {
        cell = std::max(10.0f, std::max(bounds.size().x, bounds.size().y) / 96.0f);
        width = std::max(1, static_cast<i32>(std::ceil(bounds.size().x / cell)));
        height = std::max(1, static_cast<i32>(std::ceil(bounds.size().y / cell)));
        bins.resize(static_cast<usize>(width) * static_cast<usize>(height));
        const auto add_enemy = [&](Vec2 p, PathogenFamily family, f32 density) {
            const u32 f = static_cast<u32>(family);
            if (!bounds.contains(p) || f >= kFamilyCount || !(density > 0.0f)) return;
            Bin& b = at(p);
            b.enemies[f] += density;
            b.weighted_position += p * density;
            visible_density += density;
        };
        const auto& chaff = view.world.chaff();
        for (usize i = 0; i < chaff.count(); ++i) {
            if ((chaff.flags[i] & (sim::chaff_flags::kHidden | sim::chaff_flags::kPendingKill)) != 0) continue;
            add_enemy(Vec2{chaff.pos_x[i], chaff.pos_y[i]},
                      static_cast<PathogenFamily>(chaff.family[i]), chaff.density[i]);
        }
        const auto& registry = view.world.ecs().registry();
        const auto named = registry.view<const sim::comp::NamedAgent,
                                         const sim::comp::Transform,
                                         const sim::comp::Health>();
        for (const auto entity : named) {
            const auto* brain = registry.try_get<sim::comp::AiBrain>(entity);
            if (brain != nullptr && (brain->state == sim::comp::AiState::Burrowed ||
                                     brain->state == sim::comp::AiState::Dying)) continue;
            const auto& hp = named.get<const sim::comp::Health>(entity);
            if (hp.dead()) continue;
            // A visible elite has a heavier readable presence. This uses its
            // displayed health bar, not hidden AI timers or future attacks.
            add_enemy(named.get<const sim::comp::Transform>(entity).position,
                      named.get<const sim::comp::NamedAgent>(entity).family,
                      std::max(1.0f, hp.current / 10.0f));
        }
        const auto& swarm = view.world.swarmers();
        for (usize i = 0; i < swarm.count(); ++i) {
            if ((swarm.flags[i] & sim::swarmer_flags::kPendingKill) != 0 || swarm.health[i] <= 0.0f) continue;
            const Vec2 p{swarm.pos_x[i], swarm.pos_y[i]};
            if (!bounds.contains(p)) continue;
            const auto& profile = swarm.profile_of(i);
            friendly_positions.push_back({p, engagement_reach(profile), profile.source});
            const u32 type = static_cast<u32>(profile.source);
            if (type < kTowerTypeCount) friendly_types[type] += 1.0f;
            const f32 missing = std::max(0.0f, profile.max_health - swarm.health[i]);
            Bin& b = at(p);
            b.friends += 1.0f;
            b.missing_health += missing;
            friends += 1.0f;
            missing_health += missing;
            max_health += std::max(0.0f, profile.max_health);
        }
    }

    Bin& at(Vec2 p) {
        const i32 x = std::clamp(static_cast<i32>((p.x - bounds.min.x) / cell), 0, width - 1);
        const i32 y = std::clamp(static_cast<i32>((p.y - bounds.min.y) / cell), 0, height - 1);
        return bins[static_cast<usize>(y) * static_cast<usize>(width) + static_cast<usize>(x)];
    }

    Vec2 center(i32 x, i32 y) const {
        return bounds.min + Vec2{(static_cast<f32>(x) + 0.5f) * cell,
                                  (static_cast<f32>(y) + 0.5f) * cell};
    }

    f32 sample(Vec2 p, f32 radius, bool friendly, u8 mask = 0xff) const {
        // Include half a bin so points near its boundary remain observable.
        const f32 reach = std::max(radius, cell * 0.5f) + cell * 0.5f;
        const i32 x0 = std::clamp(static_cast<i32>(std::floor((p.x - reach - bounds.min.x) / cell)), 0, width - 1);
        const i32 y0 = std::clamp(static_cast<i32>(std::floor((p.y - reach - bounds.min.y) / cell)), 0, height - 1);
        const i32 x1 = std::clamp(static_cast<i32>(std::floor((p.x + reach - bounds.min.x) / cell)), 0, width - 1);
        const i32 y1 = std::clamp(static_cast<i32>(std::floor((p.y + reach - bounds.min.y) / cell)), 0, height - 1);
        f32 sum = 0.0f;
        for (i32 y = y0; y <= y1; ++y) {
            for (i32 x = x0; x <= x1; ++x) {
                const f32 d2 = math::length_sq(center(x, y) - p);
                if (d2 > reach * reach) continue;
                const Bin& b = bins[static_cast<usize>(y) * static_cast<usize>(width) + static_cast<usize>(x)];
                f32 value = b.friends;
                if (!friendly) {
                    value = 0.0f;
                    for (u32 f = 0; f < kFamilyCount; ++f) if ((mask & (1u << f)) != 0) value += b.enemies[f];
                }
                sum += value * (1.0f - 0.65f * d2 / (reach * reach));
            }
        }
        return sum;
    }
};

} // namespace

BalanceController::BalanceController(ControllerParams params) : params_(std::move(params)) {}

void BalanceController::reset() {
    candidates_.clear();
    shared_intercepts_.clear();
    navigation_approaches_.clear();
    next_navigation_refresh_ = 0;
    navigation_wave_ = std::numeric_limits<u32>::max();
    candidate_level_ = nullptr;
    next_decision_ = 0;
    next_ability_ = 0;
    prep_since_ = 0;
    observed_wave_ = 0;
    observed_phase_ = 0;
    observed_ = false;
}

void BalanceController::build_candidates(const ControllerView& view) {
    candidates_.clear();
    shared_intercepts_.clear();
    navigation_approaches_.clear();
    next_navigation_refresh_ = 0;
    navigation_wave_ = std::numeric_limits<u32>::max();
    candidate_level_ = &view.level;
    const Rect bounds = view.world.desc().world_bounds;
    const auto add = [&](Vec2 p) {
        if (!bounds.contains(p) || view.world.sdf().sample(p) < 0.25f) return;
        for (const auto& existing : candidates_) if (math::length_sq(existing.position - p) < 2.25f) return;
        Candidate candidate;
        candidate.position = p;
        for (usize i = 0; i < view.level.placement_zones.size(); ++i) {
            if (!view.level.placement_zones[i].contains(p)) continue;
            if (i < view.level.placement_zone_tags.size()) {
                const auto& tag = view.level.placement_zone_tags[i];
                candidate.priority = std::max(candidate.priority,
                    std::clamp(tag.priority, 0.0f, 4.0f) + (tag.concentrated ? 0.5f : 0.0f));
            }
        }
        candidates_.push_back(candidate);
    };
    const auto segment = [&](Vec2 a, Vec2 b, f32 width_a, f32 width_b, u64 key) {
        const f32 length = math::length(b - a);
        const u32 steps = std::min(160u, std::max(1u, static_cast<u32>(std::ceil(length / 7.0f))));
        const Vec2 direction = math::normalize_safe(b - a);
        const Vec2 side{-direction.y, direction.x};
        for (u32 i = 0; i <= steps; ++i) {
            const f32 t = static_cast<f32>(i) / static_cast<f32>(steps);
            const f32 width = math::lerp(width_a, width_b, t);
            const f32 variation = (unit_hash(params_.placement_seed ^ key ^ static_cast<u64>(i)) - 0.5f) * 1.5f;
            const Vec2 p = math::lerp(a, b, t) + direction * variation;
            for (const f32 fraction : {0.0f, -0.22f, 0.22f, -0.37f, 0.37f}) add(p + side * (width * fraction));
        }
    };
    u64 key = 0;
    for (const auto& vessel : view.level.vessels) {
        for (usize i = 1; i < vessel.points.size(); ++i) {
            segment(vessel.points[i - 1].position, vessel.points[i].position,
                    vessel.points[i - 1].width, vessel.points[i].width, ++key * 104729u);
        }
    }
    for (const auto& path : view.level.squad_paths) {
        for (usize i = 1; i < path.points.size(); ++i) {
            segment(path.points[i - 1], path.points[i], path.half_width * 2.0f,
                    path.half_width * 2.0f, ++key * 104729u);
        }
    }
    // Zone samples cover broad chambers, obstacles and authored positions that
    // the piecewise-linear centerline samples may miss. Every proposed cell
    // still goes through the game's exact type-specific validation below.
    const auto grid = [&](Rect zone) {
        zone.min = Vec2{std::max(zone.min.x, bounds.min.x), std::max(zone.min.y, bounds.min.y)};
        zone.max = Vec2{std::min(zone.max.x, bounds.max.x), std::min(zone.max.y, bounds.max.y)};
        if (zone.max.x <= zone.min.x || zone.max.y <= zone.min.y) return;
        const u32 nx = std::clamp(static_cast<u32>(std::ceil(zone.size().x / 10.0f)), 3u, 24u);
        const u32 ny = std::clamp(static_cast<u32>(std::ceil(zone.size().y / 10.0f)), 3u, 24u);
        for (u32 y = 0; y < ny; ++y) for (u32 x = 0; x < nx; ++x) {
            add(zone.min + Vec2{(static_cast<f32>(x) + 0.5f) * zone.size().x / static_cast<f32>(nx),
                                (static_cast<f32>(y) + 0.5f) * zone.size().y / static_cast<f32>(ny)});
        }
    };
    if (view.level.placement_zones.empty()) grid(bounds);
    else for (const auto& zone : view.level.placement_zones) grid(zone);

    f32 min_cost = std::numeric_limits<f32>::infinity();
    f32 max_cost = 0.0f;
    for (const auto& candidate : candidates_) {
        const f32 cost = view.world.flow().sample_cost(candidate.position);
        if (std::isfinite(cost)) {
            min_cost = std::min(min_cost, cost);
            max_cost = std::max(max_cost, cost);
        }
    }
    for (auto& candidate : candidates_) {
        const f32 cost = view.world.flow().sample_cost(candidate.position);
        if (std::isfinite(cost) && max_cost - min_cost > 0.01f) {
            candidate.progress = 1.0f - (cost - min_cost) / (max_cost - min_cost);
        }
    }
}

std::vector<Action> BalanceController::decide(const ControllerView& view) {
    std::vector<Action> actions;
    const Tick tick = view.world.tick_index();
    const WaveStatus wave = view.waves.status();
    const u8 phase = static_cast<u8>(wave.phase);
    if (!observed_ || observed_wave_ != wave.wave_index || observed_phase_ != phase) {
        if (wave.phase == WavePhase::Prep) prep_since_ = tick;
        observed_ = true;
        observed_wave_ = wave.wave_index;
        observed_phase_ = phase;
    }
    if (tick < next_decision_ || wave.all_waves_complete ||
        view.world.snapshot().objective_integrity <= 0.0f) return actions;
    next_decision_ = tick + std::max(1u, params_.reaction_ticks != 0 ?
        params_.reaction_ticks : default_reaction(params_.policy));

    const u32 prep_ticks = params_.prep_ticks != 0 ? params_.prep_ticks : default_prep(params_.policy);
    if (params_.policy == Policy::None) {
        if (wave.phase == WavePhase::Prep && tick - prep_since_ >= prep_ticks) {
            Action action;
            action.tick = tick;
            action.kind = ActionKind::StartWave;
            actions.push_back(action);
        }
        return actions;
    }
    if (candidate_level_ != &view.level) build_candidates(view);
    const Board board(view);
    auto routes = preview_routes(view, wave);
    if (tick >= next_navigation_refresh_ || navigation_wave_ != wave.wave_index) {
        navigation_approaches_.clear();
        shared_intercepts_.clear();
        next_navigation_refresh_ = tick + 120;
        navigation_wave_ = wave.wave_index;
        const auto usable = [&](Vec2 position) {
            const IVec2 cell = view.world.tissue().world_to_cell(position);
            return view.world.tissue().walkable(cell.x, cell.y) && view.world.sdf().sample(position) > 0.25f &&
                std::isfinite(view.world.flow().sample_cost(position)) &&
                math::length_sq(view.world.flow().sample(position)) > math::kEpsilon;
        };
        for (const auto& route : routes) {
            NavigationApproach navigation;
            navigation.entrance = route.entrance;
            Vec2 start = route.entrance;
            bool found = usable(start);
            // Markers can sit outside the baked tissue. Only use the public
            // inbound geometry to find its first navigable sample in that case.
            for (usize i = 1; i < route.points.size() && !found; ++i) {
                const f32 length = math::length(route.points[i] - route.points[i - 1]);
                const u32 steps = std::min(512u, std::max(1u, static_cast<u32>(std::ceil(length / 3.0f))));
                for (u32 sample = 0; sample <= steps && !found; ++sample) {
                    start = math::lerp(route.points[i - 1], route.points[i], static_cast<f32>(sample) / static_cast<f32>(steps));
                    found = usable(start);
                }
            }
            if (found) {
                f32 radius = 4.0f;
                for (const auto& spawn : view.level.spawn_points)
                    if (math::length_sq(spawn.position - route.entrance) <= 16.0f) radius = spawn.radius;
                const Vec2 forward = math::normalize_safe(view.world.flow().sample(start));
                const Vec2 lateral{-forward.y, forward.x};
                auto centre = trace_navigation(view, start);
                if (centre.size() >= 2) {
                    // Two symmetric side samples carry equal demand. Adding a
                    // third centre sample as population would invent a2:1
                    // split at a symmetric fork. The centre validates guidance
                    // and is the fallback when an offset falls outside tissue.
                    for (f32 offset : {-0.5f, 0.5f}) {
                        const Vec2 seed = start + lateral * (radius * offset);
                        if (!usable(seed)) continue;
                        auto points = trace_navigation(view, seed);
                        if (points.size() >= 2) navigation.branches.push_back(std::move(points));
                    }
                    if (navigation.branches.size() != 2) {
                        navigation.branches.clear();
                        navigation.branches.push_back(std::move(centre));
                    }
                }
            }
            navigation_approaches_.push_back(std::move(navigation));
        }
    }
    for (auto& route : routes) {
        const auto navigation = std::find_if(navigation_approaches_.begin(), navigation_approaches_.end(),
            [&](const NavigationApproach& approach) { return math::length_sq(approach.entrance - route.entrance) <= 16.0f; });
        if (navigation == navigation_approaches_.end() || navigation->branches.empty()) continue;
        route.navigation_available = true;
        const f32 weight = route.weight / static_cast<f32>(navigation->branches.size());
        for (const auto& points : navigation->branches) route.navigation_branches.push_back({points, weight});
    }
    const auto eligible_type = [&](u32 t) {
        return std::isfinite(params_.weights[t]) && params_.weights[t] > 0.0f &&
            view.towers.tower_allowed(static_cast<TowerType>(t)) &&
            view.towers.tower_unlocked(static_cast<TowerType>(t));
    };
    f32 shared_reach = 16.0f;
    bool have_anchor = false;
    for (u32 t = 0; t < kTowerTypeCount; ++t) if (eligible_type(t)) {
        const auto profile = swarmer_profile(static_cast<TowerType>(t));
        if (profile.kind != sim::SwarmerKind::Shooter && profile.kind != sim::SwarmerKind::ArborGrabber) continue;
        shared_reach = std::min(shared_reach, engagement_reach(profile) * 0.75f);
        have_anchor = true;
    }
    // A support unlock must not veto a viable ranged/grabber hold. Mobile-only
    // specialists instead plan around their acquisition/chase distance.
    if (!have_anchor) for (u32 t = 0; t < kTowerTypeCount; ++t) if (eligible_type(t)) {
        shared_reach = std::min(shared_reach, engagement_reach(swarmer_profile(static_cast<TowerType>(t))) * 0.75f);
    }
    shared_reach = std::max(4.0f, shared_reach);
    if (params_.policy != Policy::Novice) {
        std::vector<PreviewRoute> planned;
        for (auto& route : routes) {
            if (route.branches.size() <= 1) { planned.push_back(std::move(route)); continue; }
            const Vec2 entrance = route.entrance;
            auto cached = std::find_if(shared_intercepts_.begin(), shared_intercepts_.end(), [&](const SharedIntercept& site) {
                return math::length_sq(site.entrance - entrance) <= 16.0f;
            });
            if (cached == shared_intercepts_.end()) {
                // Locate the first separation, not a downstream rejoin. An
                // earliest shared legal site before that separation can engage
                // every map alternative when later waves change branches.
                std::vector<f32> prefix_limits(route.branches.size(), std::numeric_limits<f32>::infinity());
                bool has_fork = false;
                for (usize b = 0; b < route.branches.size(); ++b) {
                    const auto& points = route.branches[b].points;
                    f32 along = 0.0f;
                    bool split = false;
                    for (usize i = 1; i < points.size() && !split; ++i) {
                        const f32 length = math::length(points[i] - points[i - 1]);
                        const u32 steps = std::max(1u, static_cast<u32>(std::ceil(length / 4.0f)));
                        for (u32 sample = 0; sample <= steps && !split; ++sample) {
                            const f32 t = static_cast<f32>(sample) / static_cast<f32>(steps);
                            const Vec2 p = math::lerp(points[i - 1], points[i], t);
                            for (usize other = 0; other < route.branches.size(); ++other) {
                                if (other == b) continue;
                                if (project_route(p, route.branches[other].points).distance_sq <= 4.0f * shared_reach * shared_reach) continue;
                                prefix_limits[b] = along + length * t;
                                split = true;
                                has_fork = true;
                                break;
                            }
                        }
                        along += length;
                    }
                }
                SharedIntercept site;
                site.entrance = entrance;
                site.has_fork = has_fork;
                f32 earliest = std::numeric_limits<f32>::infinity();
                for (const auto& candidate : candidates_) {
                    f32 arrival = 0.0f;
                    bool covers = true;
                    for (usize b = 0; b < route.branches.size(); ++b) {
                        const auto projection = project_route(candidate.position, route.branches[b].points);
                        if (projection.distance_sq > shared_reach * shared_reach || projection.along > prefix_limits[b]) {
                            covers = false;
                            break;
                        }
                        arrival = std::max(arrival, projection.along);
                    }
                    if (!covers || arrival >= earliest) continue;
                    bool legal = false;
                    for (u32 t = 0; t < kTowerTypeCount && !legal; ++t) if (eligible_type(t)) {
                        legal = view.towers.validate_deploy(view.world, static_cast<TowerType>(t), candidate.position,
                                                           std::numeric_limits<u32>::max()).valid();
                    }
                    if (!legal) continue;
                    site.position = candidate.position;
                    site.viable = true;
                    earliest = arrival;
                }
                shared_intercepts_.push_back(site);
                cached = shared_intercepts_.end() - 1;
            }
            if (cached->viable) {
                // Parallel authored alternatives also need an early common
                // hold: centreline proximity inside a broad chamber does not
                // cover the full horde as it goes around an island. Synthetic
                // side traces are stored separately and never create this
                // shared-prefix rule for a single authored route.
                route.shared_intercept = true;
                route.intercept = cached->position;
                planned.push_back(std::move(route));
            } else {
                // Disjoint branches may already be separated at the playable
                // boundary. Keep their actual current-wave demand separate.
                f32 active_weight = 0.0f;
                for (const auto& branch : route.branches) active_weight += branch.weight;
                for (const auto& branch : route.branches) if (branch.weight > 0.0f && active_weight > 0.0f) {
                    PreviewRoute separate;
                    separate.points = branch.points;
                    separate.entrance = route.entrance;
                    // Parent traces are not independently assigned to this
                    // authored branch. Cloning them would let one side's cells
                    // satisfy every split demand and starve the other side.
                    separate.weight = route.weight * branch.weight / active_weight;
                    separate.branches.push_back(branch);
                    planned.push_back(std::move(separate));
                }
            }
        }
        routes = std::move(planned);
    }
    std::vector<f32> route_relevance(candidates_.size(), 0.0f);
    std::vector<std::vector<f32>> candidate_distance(candidates_.size(), std::vector<f32>(routes.size(), 0.0f));
    std::vector<std::vector<f32>> active_distance(candidates_.size(), std::vector<f32>(routes.size(), 0.0f));
    std::vector<std::vector<f32>> authored_distance(candidates_.size(), std::vector<f32>(routes.size(), 0.0f));
    std::vector<std::vector<f32>> remaining_distance(candidates_.size(),
        std::vector<f32>(routes.size(), std::numeric_limits<f32>::infinity()));
    for (usize c = 0; c < candidates_.size(); ++c) {
        for (usize r = 0; r < routes.size(); ++r) {
            f32 nearest = std::numeric_limits<f32>::infinity();
            for (const auto& branch : routes[r].branches) {
                const auto projection = project_route(candidates_[c].position, branch.points);
                nearest = std::min(nearest, projection.distance_sq);
                authored_distance[c][r] = std::max(authored_distance[c][r], projection.distance_sq);
            }
            for (const auto& branch : routes[r].navigation_branches) {
                const auto projection = project_route(candidates_[c].position, branch.points);
                nearest = std::min(nearest, projection.distance_sq);
                active_distance[c][r] = std::max(active_distance[c][r], projection.distance_sq);
                remaining_distance[c][r] = std::min(remaining_distance[c][r], projection.remaining);
            }
            candidate_distance[c][r] = nearest;
            route_relevance[c] += routes[r].weight / (1.0f + nearest / 144.0f);
        }
    }
    usize recruiting_approach = 0;
    std::array<f32, kTowerTypeCount> recruiting_types = board.friendly_types;
    if (routes.size() > 1 && params_.policy != Policy::Novice) {
        std::vector<f32> coverage(routes.size(), 0.0f);
        std::vector<std::array<f32, kTowerTypeCount>> route_types(routes.size());
        for (const auto& friendly : board.friendly_positions) {
            std::vector<f32> memberships(routes.size(), 0.0f);
            f32 nearby_weight = 0.0f;
            for (usize r = 0; r < routes.size(); ++r) {
                // Lateral squad guidance can choose public fork alternatives
                // even when a few seed-side flow samples pick only one side.
                // Preserve those safety branches as fractional coverage, not
                // extra demand. A single soft path instead uses actual flow.
                const auto& branches = routes[r].branches.size() > 1 || routes[r].navigation_branches.empty() ?
                    routes[r].branches : routes[r].navigation_branches;
                for (const auto& branch : branches) {
                    if (project_route(friendly.position, branch.points).distance_sq <= friendly.reach * friendly.reach)
                        memberships[r] += 1.0f / static_cast<f32>(branches.size());
                }
                if (memberships[r] > 0.0f) nearby_weight += routes[r].weight;
            }
            // A shared-trunk unit is apportioned, never counted twice. A unit
            // covering only one side of a fork receives only partial credit.
            if (nearby_weight > 0.0f) for (usize r = 0; r < routes.size(); ++r) {
                const f32 credit = memberships[r] * routes[r].weight / nearby_weight;
                coverage[r] += credit;
                const u32 type = static_cast<u32>(friendly.type);
                if (type < kTowerTypeCount) route_types[r][type] += credit;
            }
        }
        f32 greatest_deficit = -std::numeric_limits<f32>::infinity();
        for (usize r = 0; r < routes.size(); ++r) {
            const f32 deficit = routes[r].weight * (board.friends + 1.0f) - coverage[r];
            if (deficit > greatest_deficit) { greatest_deficit = deficit; recruiting_approach = r; }
        }
        // A healthy cohort at another entrance cannot satisfy this front's
        // need for shooters, latchers or support. Reuse the same fractional
        // physical coverage so shared-trunk cells are never counted twice.
        recruiting_types = route_types[recruiting_approach];
    }
    std::array<f32, kTowerTypeCount> weights{};
    if (params_.policy == Policy::Novice) weights = {0.82f, 0.12f, 0.04f, 0.02f, 0.0f};
    else if (params_.policy == Policy::Tactical) weights = {0.52f, 0.28f, 0.10f, 0.06f, 0.04f};
    else if (params_.policy == Policy::Specialist) weights.fill(1.0f);
    else weights = {0.60f, 0.26f, 0.08f, 0.04f, 0.02f};

    f32 total_weight = 0.0f;
    f32 eligible_friends = 0.0f;
    for (u32 t = 0; t < kTowerTypeCount; ++t) {
        const auto type = static_cast<TowerType>(t);
        const f32 multiplier = params_.weights[t];
        weights[t] *= std::isfinite(multiplier) ? std::max(0.0f, multiplier) : 0.0f;
        if (!view.towers.tower_allowed(type) || !view.towers.tower_unlocked(type)) weights[t] = 0.0f;
        total_weight += weights[t];
        if (weights[t] > 0.0f) eligible_friends += recruiting_types[t];
    }

    Action deployment;
    deployment.tick = tick;
    f32 best_purchase = -std::numeric_limits<f32>::infinity();
    f32 deployment_deficit = 0.0f;
    bool have_deployment = false;
    const u32 atp = view.economy.atp();
    const u32 spendable = static_cast<u32>(std::min<u64>(
        atp > params_.reserve_atp ? atp - params_.reserve_atp : 0, view.spending_remaining));
    struct ScoredSite { f32 score; usize index; };
    std::vector<ScoredSite> scored_sites;
    scored_sites.reserve(candidates_.size());
    for (u32 t = 0; t < kTowerTypeCount; ++t) {
        if (weights[t] <= 0.0f || total_weight <= 0.0f) continue;
        const auto type = static_cast<TowerType>(t);
        const auto& stats = view.towers.stats(type);
        if (stats.build_cost > spendable) continue;
        const auto profile = swarmer_profile(type);
        const f32 desired = weights[t] / total_weight * (eligible_friends + 1.0f);
        const f32 deficit = desired - recruiting_types[t];
        f32 best_site_score = -std::numeric_limits<f32>::infinity();
        Vec2 best_site{0.0f, 0.0f};
        scored_sites.clear();
        for (usize c = 0; c < candidates_.size(); ++c) {
            const Candidate& candidate = candidates_[c];
            const f32 clearance = view.world.sdf().sample(candidate.position);
            const Vec2 flow = math::normalize_safe(view.world.flow().sample(candidate.position));
            const f32 standoff = profile.kind == sim::SwarmerKind::Shooter ? profile.attach_radius * 0.8f : 2.0f;
            const f32 pressure = board.sample(candidate.position - flow * standoff,
                                              std::max(8.0f, profile.search_radius), false, stats.family_mask);
            const f32 local_friends = board.sample(candidate.position, 7.0f, true);
            const f32 supporting_friends = board.sample(candidate.position,
                std::clamp(profile.search_radius * 0.45f, 8.0f, 14.0f), true);
            const f32 crowding = board.sample(candidate.position, 3.0f, true);
            // Mobile cells need overlapping engagement ranges. Spreading each
            // purchase to an empty part of the route lets a multiplying horde
            // overwhelm them one at a time. Establish an early cohort, before
            // the horde has crossed most of the playable approach, then recruit
            // into its visible position as combat moves it around the bend.
            const bool novice = params_.policy == Policy::Novice;
            const f32 target_progress = novice ? 0.65f : 0.22f;
            const f32 hold_line = 1.0f - std::abs(candidate.progress - target_progress);
            const f32 priority = novice ? 0.10f : 0.12f;
            f32 score = hold_line * (novice ? 1.8f : 3.0f) + candidate.priority * priority;
            score += route_relevance[c] * (novice ? 0.25f : 1.3f);
            // Each independent entrance needs its own supporting cohort. A
            // single well-defended chamber cannot protect a disjoint route.
            // The public preview supplies expected shares; current living
            // allies supply coverage. Legal alternatives remain available when
            // a particular cell cannot be deployed in the preferred approach.
            if (!novice && !routes.empty()) {
                const auto& recruiting = routes[recruiting_approach];
                const f32 reach = engagement_reach(profile);
                if (candidate_distance[c][recruiting_approach] <= reach * reach) score += 20.0f;
                // Independent entrances can feed a common fighting pocket.
                // Pool their cohort when this role can cover every active
                // traced AND authored branch of both approaches, with room
                // left before the organ. A few successful flow samples are
                // not exhaustive: lateral squad guidance can choose a
                // different branch around a broad obstacle. Public map
                // alternatives constrain safety without adding future demand;
                // pooling must not override their pre-fork protection either.
                // A late convergence at the goal is not a viable hold, and
                // two fork branches from one entrance are not two approaches.
                // The measured outgoing holds have about30–40world units of
                // flow remaining to the actual goal rim; requiring2.5attack
                // ranges excluded these independently paid, effective sites.
                const f32 runway = std::max(30.0f, 1.5f * reach);
                const auto covers = [&](usize r) {
                    return routes[r].navigation_available && active_distance[c][r] <= reach * reach &&
                        authored_distance[c][r] <= reach * reach &&
                        std::isfinite(remaining_distance[c][r]) && remaining_distance[c][r] >= runway;
                };
                bool junction = false;
                if (covers(recruiting_approach)) for (usize r = 0; r < routes.size(); ++r) {
                    if (r == recruiting_approach ||
                        math::length_sq(routes[r].entrance - recruiting.entrance) <= 16.0f) continue;
                    if (covers(r)) { junction = true; break; }
                }
                // This takes precedence over an early per-entrance edge and
                // its idle reserves. Ordinary range, pressure, support and
                // paid placement validation still select the legal local site.
                if (junction) score += 48.0f;
                if (recruiting.shared_intercept) {
                    // Shared coverage outranks attraction to a large cohort
                    // already stranded on one branch. Exact normal validation
                    // still permits every role to use its closest legal site.
                    score += 24.0f / (1.0f + math::length_sq(candidate.position - recruiting.intercept) / 36.0f);
                }
            }
            if (novice) {
                score += 1.5f * std::log1p(pressure) / (1.0f + local_friends * 0.20f);
                score -= 0.22f * local_friends + 0.35f * crowding;
            } else {
                score += 1.5f * std::log1p(pressure);
                score += 2.0f * std::log1p(supporting_friends);
                // Only very large cohorts need another local rank. Do not
                // mistake ordinary mutual support for harmful crowding.
                score -= 0.08f * std::max(0.0f, crowding - 24.0f);
            }
            if (profile.kind == sim::SwarmerKind::Shooter) {
                // Reinforce from a firing position behind current contact,
                // rather than dropping a fragile shooter into enemy bodies.
                score -= (novice ? 0.15f : 1.1f) *
                    std::log1p(board.sample(candidate.position, 3.0f, false));
            }
            if (profile.kind == sim::SwarmerKind::Builder || profile.kind == sim::SwarmerKind::ArborGrabber) {
                score += 0.4f / (1.0f + std::max(0.0f, clearance) * 0.15f);
            }
            // A small stable offset explores nearby viable deployments across
            // placement seeds, without making a tactic random every tick.
            score += unit_hash(params_.placement_seed ^ (static_cast<u64>(t) << 40) ^ c) *
                     (params_.policy == Policy::Novice ? 0.9f : 0.12f);
            if (score > -std::numeric_limits<f32>::infinity()) scored_sites.push_back({score, c});
        }
        // Builder validation includes a full lane-connectivity query. Score
        // first, then validate in stable preference order: lower-ranked sites
        // cannot change the winner. Equal scores retain the original index,
        // and the normal paid deployment API remains the legality authority.
        std::stable_sort(scored_sites.begin(), scored_sites.end(), [](const ScoredSite& a, const ScoredSite& b) {
            return a.score > b.score;
        });
        for (const auto& site : scored_sites) {
            if (!(deficit * 4.0f + site.score * 0.15f > best_purchase)) break;
            const auto query = view.towers.validate_deploy(view.world, type, candidates_[site.index].position, spendable);
            if (!query.valid()) continue;
            best_site_score = site.score;
            best_site = query.snapped_position;
            break;
        }
        if (!std::isfinite(best_site_score)) continue;
        // Deficits set composition; board pressure resolves close choices and
        // site selection. A forbidden weight never becomes a fallback purchase.
        const f32 purchase_score = deficit * 4.0f + best_site_score * 0.15f;
        if (purchase_score > best_purchase) {
            have_deployment = true;
            best_purchase = purchase_score;
            deployment.tower = type;
            deployment.position = best_site;
            deployment_deficit = deficit;
        }
    }
    if (have_deployment && params_.policy != Policy::Novice && deployment_deficit <= 0.0f) {
        // Do not turn each small income payment into another cheap surplus
        // cell while the front needs a slightly more expensive role. Waiting
        // preserves ATP for that legal purchase; exhausted experimental caps
        // never reserve for a role they can no longer fund. Wave preparation
        // still ends normally when no purchase is proposed.
        for (u32 t = 0; t < kTowerTypeCount; ++t) {
            if (weights[t] <= 0.0f || total_weight <= 0.0f) continue;
            const f32 deficit = weights[t] / total_weight * (eligible_friends + 1.0f) - recruiting_types[t];
            const auto type = static_cast<TowerType>(t);
            const u32 cost = view.towers.stats(type).build_cost;
            if (deficit <= 0.0f || cost <= spendable || cost > view.spending_remaining) continue;
            const bool legal = std::any_of(candidates_.begin(), candidates_.end(), [&](const Candidate& candidate) {
                return view.towers.validate_deploy(view.world, type, candidate.position,
                                                  std::numeric_limits<u32>::max()).valid();
            });
            if (legal) { have_deployment = false; break; }
        }
    }
    if (have_deployment) actions.push_back(deployment);

    // One emergency cast per decision. Novice deliberately never spends these
    // resources; competent/tactical policies react to visible concentrations
    // and visible friendly health, without testing a future combat outcome.
    if (params_.use_abilities && params_.policy != Policy::Novice &&
        wave.phase != WavePhase::Prep && tick >= next_ability_) {
        const auto available = [&](AbilityId ability) {
            return view.abilities.unlocked(ability) && view.abilities.ready(ability);
        };
        Action cast;
        cast.tick = tick;
        cast.kind = ActionKind::CastAbility;
        bool have_cast = false;
        if (board.friends >= 3.0f && board.max_health > 0.0f &&
            board.missing_health / board.max_health >= 0.25f && available(AbilityId::FeverResponse)) {
            cast.ability = AbilityId::FeverResponse;
            have_cast = true;
        } else if (board.visible_density >= (params_.policy == Policy::Tactical ? 18.0f : 35.0f)) {
            f32 best_pressure = 0.0f;
            Vec2 target{0.0f, 0.0f};
            for (i32 y = 0; y < board.height; ++y) for (i32 x = 0; x < board.width; ++x) {
                const auto& bin = board.bins[static_cast<usize>(y) * static_cast<usize>(board.width) + static_cast<usize>(x)];
                f32 density = 0.0f;
                for (f32 value : bin.enemies) density += value;
                if (density <= 0.0f) continue;
                const Vec2 point = bin.weighted_position / density;
                const f32 near_friends = board.sample(point, 16.0f, true);
                const f32 pressure = board.sample(point, 13.0f, false) * (1.0f + std::min(near_friends, 8.0f) * 0.08f);
                if (pressure > best_pressure) { best_pressure = pressure; target = point; }
            }
            const f32 threshold = params_.policy == Policy::Tactical ? 12.0f : 24.0f;
            if (best_pressure >= threshold) {
                cast.position = target;
                if (available(AbilityId::HistamineFlare)) { cast.ability = AbilityId::HistamineFlare; have_cast = true; }
                else if (available(AbilityId::ComplementCascadeBurst)) { cast.ability = AbilityId::ComplementCascadeBurst; have_cast = true; }
                else if (params_.policy == Policy::Tactical && available(AbilityId::FibrinClot)) {
                    cast.ability = AbilityId::FibrinClot;
                    cast.position += math::normalize_safe(view.world.flow().sample(target)) * 5.0f;
                    // The normal cast API performs the full no-lane-severing
                    // validation; reject an obviously off-tissue intent here.
                    have_cast = view.world.sdf().sample(cast.position) > 0.0f;
                }
            }
        }
        if (have_cast) {
            actions.push_back(cast);
            next_ability_ = tick + (params_.policy == Policy::Tactical ? 120u : 240u);
        }
    }

    if (wave.phase == WavePhase::Prep && tick - prep_since_ >= prep_ticks && !have_deployment) {
        Action action;
        action.tick = tick;
        action.kind = ActionKind::StartWave;
        actions.push_back(action);
    }
    return actions;
}

bool parse_policy(std::string_view text, Policy& policy) {
    if (text == "none" || text == "no_defense" || text == "no-defense") policy = Policy::None;
    else if (text == "novice") policy = Policy::Novice;
    else if (text == "competent") policy = Policy::Competent;
    else if (text == "tactical") policy = Policy::Tactical;
    else if (text == "specialist") policy = Policy::Specialist;
    else return false;
    return true;
}

const char* policy_name(Policy policy) {
    switch (policy) {
        case Policy::None: return "none";
        case Policy::Novice: return "novice";
        case Policy::Competent: return "competent";
        case Policy::Tactical: return "tactical";
        case Policy::Specialist: return "specialist";
    }
    return "unknown";
}

const char* action_kind_name(ActionKind kind) {
    switch (kind) {
        case ActionKind::Deploy: return "deploy";
        case ActionKind::CastAbility: return "cast";
        case ActionKind::StartWave: return "start_wave";
    }
    return "unknown";
}

} // namespace immune::game::balance
