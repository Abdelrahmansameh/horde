// sim/swarm/Swarmers.cpp — the per-tick seek / engage / detonate kernel.
// Swarmers.h states the contract and the cost model; this file implements it.
//
// SHAPE OF THE TICK, and why it is one branchy pass instead of three clean ones
// ProjectileSystem splits integration (vectorizable, branch-free) from
// collision (branchy) precisely so the compiler can vectorize the first half.
// That split does not pay here and is not attempted: a swarmer's velocity for
// this tick depends on whether it has a target, where that target is, and what
// kind of swarmer it is, so there is no position update that can run ahead of
// the decision. One pass, stated plainly, is the honest structure.
//
// THE STATES a swarmer moves through, in the order this file handles them:
//   1. RETIRED   — lifetime exhausted, or out of world. Bombers detonate in
//                  place on expiry; everything else dissolves.
//   2. TARGETLESS— no valid target. Pays one spatial query (chaff) plus a walk
//                  of the named list, then steers at the nearest.
//   3. SEEKING   — has a target, not yet in reach. Steers toward it with a
//                  wander term; this is where the cloud gets its shape.
//   4. ENGAGED   — inside attach_radius. What happens now is the KIND:
//                  Latch rides and drains, Shooter holds and fires, a bomber
//                  detonates and is gone.
// A swarmer can traverse 2 -> 3 -> 4 in a single tick, and drops from 4 back
// to 2 the tick after its target dies. After the pass, two positional
// corrections run over everyone that moved: BODIES (resolve_bodies, contact
// against other swarmers and against pathogens) and then WALLS.
//
// WHY THE WANDER IS NOT FROM THE SHARED Rng
// Swarmers.h explains the determinism reason. The visual reason is just as
// binding: a shared draw would give every swarmer alive on the same tick a
// correlated nudge, and a cloud that jitters in unison reads as one object
// vibrating rather than as many small things moving independently. The private
// per-swarmer stream is what makes it a swarm.
#include "sim/swarm/Swarmers.h"

#include "core/Math.h"
#include "core/Rng.h"
#include "sim/CombatEvents.h"
#include "sim/chaff/ChaffBuffers.h"
#include "sim/flowfield/FlowField.h"
#include "sim/spatial/SpatialHash.h"

#include <algorithm>
#include <cmath>

namespace immune::sim {

namespace {

bool family_matches(u8 mask, u8 family) {
    return (mask & static_cast<u8>(1u << family)) != 0;
}

/// A swarmer may go for anything alive, of a matching family, not already
/// dying this tick, and NOT burrowed: a hidden pathogen is invisible to every
/// tower's units, which is what makes burrowing a real escape.
bool targetable(const ChaffBuffers& chaff, u32 idx, u8 mask) {
    if (idx >= chaff.count()) return false;
    const u8 f = chaff.flags[idx];
    if ((f & chaff_flags::kAlive) == 0) return false;
    if ((f & chaff_flags::kPendingKill) != 0) return false;
    if ((f & chaff_flags::kHidden) != 0) return false;
    return family_matches(mask, chaff.family[idx]);
}

/// PCG-flavoured step on a per-swarmer state word. Same algorithm family as
/// core/Rng.h so the quality is known, but a private stream per swarmer — see
/// the note at the head of this file for why that matters twice over.
u32 next_rand(u32& state) {
    state = state * 1664525u + 1013904223u;
    return state;
}

f32 signed_unit(u32& state) {
    return static_cast<f32>(next_rand(state) >> 8) * (1.0f / 8388608.0f) - 1.0f;
}

/// Where a round of speed `speed` fired now from `from` meets a target that is
/// at `at` and keeps walking at `vel`: the first-order intercept, i.e. the
/// smallest positive t with |at + vel*t - from| == speed*t, evaluated on the
/// target's line. When no such t exists (the target outruns the round, or is
/// already on the muzzle) the answer is `at` itself and the shot goes straight
/// at it, which is also exactly what a standing target gets.
///
/// Solved rather than approximated because the numbers no longer let the
/// miss hide: a Neutrophil holds ~16 out and fires at 45 u/s, so a round is
/// in the air for a third of a second, in which a horde-pace target has
/// walked ten hit radii sideways. A round aimed at where the target IS lands
/// where it WAS, every time.
Vec2 intercept_point(Vec2 from, Vec2 at, Vec2 vel, f32 speed) {
    const Vec2 d = at - from;
    const f32 a = math::length_sq(vel) - speed * speed;
    const f32 b = 2.0f * (d.x * vel.x + d.y * vel.y);
    const f32 c = math::length_sq(d);
    f32 t = -1.0f;
    if (std::fabs(a) < math::kEpsilon) {
        // Round and target equally fast: the quadratic collapses to b*t + c.
        if (std::fabs(b) > math::kEpsilon) t = -c / b;
    } else {
        const f32 disc = b * b - 4.0f * a * c;
        if (disc >= 0.0f) {
            const f32 root = std::sqrt(disc);
            const f32 t0 = (-b - root) / (2.0f * a);
            const f32 t1 = (-b + root) / (2.0f * a);
            // The earliest meeting that is still ahead of us.
            if (t0 > 0.0f && t1 > 0.0f) t = math::min(t0, t1);
            else t = math::max(t0, t1);
        }
    }
    if (!(t > 0.0f)) return at;
    return at + vel * t;
}

CombatEvent make_event(CombatEventType type, TowerType source, Vec2 pos, Vec2 dir, u16 visual_id) {
    CombatEvent e;
    e.type = type;
    e.visual_id = static_cast<u16>(visual_id | kSwarmerEventBit);
    e.source = source;
    e.target_family = PathogenFamily::Count;
    e.origin = pos;
    e.secondary = pos;
    e.direction = math::normalize_safe(dir);
    return e;
}

} // namespace

const char* swarmer_kind_name(SwarmerKind kind) {
    switch (kind) {
        case SwarmerKind::Latch:       return "latch";
        case SwarmerKind::Shooter:     return "shooter";
        case SwarmerKind::Bomber:      return "bomber";
        case SwarmerKind::SlowBomber:  return "slow_bomber";
        case SwarmerKind::MucusBomber: return "mucus_bomber";
        case SwarmerKind::Builder:     return "builder";
        case SwarmerKind::ArborGrabber:return "arbor_grabber";
        case SwarmerKind::Count:       break;
    }
    return "latch";
}

// ---------------------------------------------------------------------------
// Named-target list
// ---------------------------------------------------------------------------

void NamedTargetList::sort() {
    // Stable sort on id, carrying the damage slots along. The list is tiny
    // (elites and bosses only), so an index sort is not worth its own code.
    std::vector<usize> order(items.size());
    for (usize i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [this](usize a, usize b) { return items[a].id.value < items[b].id.value; });
    std::vector<NamedTarget> sorted_items;
    std::vector<f32> sorted_damage;
    std::vector<f32> sorted_health;
    sorted_items.reserve(items.size());
    sorted_damage.reserve(items.size());
    sorted_health.reserve(items.size());
    for (const usize i : order) {
        sorted_items.push_back(items[i]);
        sorted_damage.push_back(damage[i]);
        sorted_health.push_back(health_left[i]);
    }
    items.swap(sorted_items);
    damage.swap(sorted_damage);
    health_left.swap(sorted_health);
}

usize NamedTargetList::find(EntityId id) const {
    if (!id.valid()) return npos;
    usize lo = 0;
    usize hi = items.size();
    while (lo < hi) {
        const usize mid = lo + (hi - lo) / 2;
        const u32 v = items[mid].id.value;
        if (v == id.value) return mid;
        if (v < id.value) lo = mid + 1;
        else hi = mid;
    }
    return npos;
}

void SwarmerEffects::reserve(usize n) {
    bursts.reserve(n);
    zones.reserve(n);
    splashes.reserve(n);
    shots.reserve(n);
    builds.reserve(n);
}

void SwarmerEffects::clear() {
    bursts.clear();
    zones.clear();
    splashes.clear();
    shots.clear();
    builds.clear();
}

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------

void SwarmerBuffers::reserve(usize max_swarmers) {
    capacity_ = max_swarmers;
    pos_x.assign(max_swarmers, 0.0f);
    pos_y.assign(max_swarmers, 0.0f);
    vel_x.assign(max_swarmers, 0.0f);
    vel_y.assign(max_swarmers, 0.0f);
    life.assign(max_swarmers, 0.0f);
    cooldown.assign(max_swarmers, 0.0f);
    chase.assign(max_swarmers, 0.0f);
    attach.assign(max_swarmers, 0.0f);
    target_index.assign(max_swarmers, 0u);
    target_generation.assign(max_swarmers, 0u);
    target_named.assign(max_swarmers, EntityId{});
    profile.assign(max_swarmers, 0u);
    family_mask.assign(max_swarmers, 0u);
    flags.assign(max_swarmers, 0u);
    visual_id.assign(max_swarmers, 0u);
    seed.assign(max_swarmers, 0u);
    owner.assign(max_swarmers, EntityId{});
    group.assign(max_swarmers, 0u);
    slot.assign(max_swarmers, 0u);
    goal_x.assign(max_swarmers, 0.0f);
    goal_y.assign(max_swarmers, 0.0f);
    health.assign(max_swarmers, 0.0f);
    generation.assign(max_swarmers, 0u);
    arbor_grabber.assign(max_swarmers, ArborGrabberState{});
    next_generation_ = 1u;
    clear();
}

void SwarmerBuffers::clear() {
    count_ = 0;
    for (usize i = 0; i < capacity_; ++i) {
        flags[i] = 0;
        generation[i] = 0u;
    }
    // next_generation_ deliberately survives a clear(), like the chaff store's:
    // a reference taken before the clear must not resolve to a new unit.
}

void SwarmerBuffers::set_profile(u16 slot, const SwarmerProfile& p) {
    if (slot >= kSwarmerProfileSlots) return;
    profiles_[slot] = p;
}

bool SwarmerBuffers::spawn(const SwarmerSpawnParams& p) {
    if (count_ >= capacity_) return false;   // S3: never grows.
    const usize i = count_++;
    pos_x[i] = p.position.x;
    pos_y[i] = p.position.y;
    vel_x[i] = p.velocity.x;
    vel_y[i] = p.velocity.y;
    life[i] = profile_at(p.profile).lifetime;
    cooldown[i] = 0.0f;              // a shooter fires the moment it arrives
    chase[i] = 0.0f;
    attach[i] = 0.0f;
    target_index[i] = 0u;
    target_generation[i] = 0u;      // invalid handle => starts targetless
    target_named[i] = EntityId{};
    profile[i] = p.profile < kSwarmerProfileSlots ? p.profile : u16{0};
    family_mask[i] = p.family_mask;
    flags[i] = swarmer_flags::kAlive;
    if (p.has_goal) flags[i] |= swarmer_flags::kHasGoal;
    goal_x[i] = p.goal.x;
    goal_y[i] = p.goal.y;
    visual_id[i] = p.visual_id;
    // Never zero: a zero state word makes the LCG produce a fixed sequence
    // shared by every swarmer that happened to get it.
    seed[i] = p.seed | 1u;
    owner[i] = p.owner;
    group[i] = p.group;
    slot[i] = p.slot;
    health[i] = profile_at(p.profile).max_health;
    generation[i] = next_generation_++;
    arbor_grabber[i] = ArborGrabberState{};
    if (next_generation_ == 0u) next_generation_ = 1u;   // never hand out 0
    return true;
}

void SwarmerBuffers::kill(usize index) {
    if (index >= count_) return;
    flags[index] |= swarmer_flags::kPendingKill;
}

usize SwarmerBuffers::compact() {
    usize removed = 0;
    usize i = 0;
    while (i < count_) {
        if ((flags[i] & swarmer_flags::kPendingKill) == 0) { ++i; continue; }
        const usize last = count_ - 1;
        if (i != last) {
            pos_x[i] = pos_x[last];
            pos_y[i] = pos_y[last];
            vel_x[i] = vel_x[last];
            vel_y[i] = vel_y[last];
            life[i] = life[last];
            cooldown[i] = cooldown[last];
            chase[i] = chase[last];
            attach[i] = attach[last];
            target_index[i] = target_index[last];
            target_generation[i] = target_generation[last];
            target_named[i] = target_named[last];
            profile[i] = profile[last];
            family_mask[i] = family_mask[last];
            flags[i] = flags[last];
            visual_id[i] = visual_id[last];
            seed[i] = seed[last];
            owner[i] = owner[last];
            group[i] = group[last];
            slot[i] = slot[last];
            goal_x[i] = goal_x[last];
            goal_y[i] = goal_y[last];
            health[i] = health[last];
            generation[i] = generation[last];
            arbor_grabber[i] = arbor_grabber[last];
        }
        flags[last] = 0;
        generation[last] = 0u;   // the retired id is never reissued
        --count_;
        ++removed;
        // The survivor swapped down into slot i has not been tested yet, so i
        // deliberately does not advance. Same pattern as ChaffBuffers::compact.
    }
    return removed;
}

// ---------------------------------------------------------------------------
// The tick
// ---------------------------------------------------------------------------

namespace {

/// Result of one "what is nearest" search from `p`: at most one of the two
/// handles is set.
struct SearchHit {
    bool found = false;
    u32 chaff_index = 0;
    usize named_index = NamedTargetList::npos;
    f32 d2 = 0.0f;
};

/// Nearest targetable thing to `p` inside `radius`: chaff via the hash, named
/// agents by walking the (tiny) list. `skip_chaff` / `skip_named` exclude the
/// caller's current target so a "is there anything closer" question cannot
/// answer itself. Ties break by lowest chaff index, then named-list order,
/// which keeps the choice reproducible.
/// `ignore_latched` drops chaff riding a friendly host (chaff_flags::kLatched)
/// from the answer. Off for target acquisition -- a virus clinging to a tower
/// is precisely what a unit should go and kill -- and on for a shooter's
/// kite-threat search, where a passenger on its own membrane would otherwise
/// read as a threat it can never outrun.
SearchHit search_nearest(const ChaffBuffers& chaff, const SpatialHash& hash,
                         const NamedTargetList& named, std::vector<u32>& scratch, Vec2 p,
                         f32 radius, u8 mask, usize skip_chaff, usize skip_named,
                         bool ignore_latched = false) {
    SearchHit best;
    scratch.clear();
    hash.query_circle(p, radius, scratch);
    const f32 r2 = radius * radius;
    for (const u32 idx : scratch) {
        if (idx == skip_chaff) continue;
        if (!targetable(chaff, idx, mask)) continue;
        if (ignore_latched && (chaff.flags[idx] & chaff_flags::kLatched) != 0) continue;
        const f32 dx = chaff.pos_x[idx] - p.x;
        const f32 dy = chaff.pos_y[idx] - p.y;
        const f32 d2 = dx * dx + dy * dy;
        if (d2 > r2) continue;
        if (!best.found || d2 < best.d2) {
            best.found = true;
            best.chaff_index = idx;
            best.named_index = NamedTargetList::npos;
            best.d2 = d2;
        }
    }
    // Named agents compete on the same distance. Their body radius counts
    // toward reach, so a boss is "in range" at its rim, not its centre.
    for (usize k = 0; k < named.items.size(); ++k) {
        if (k == skip_named) continue;
        const NamedTarget& t = named.items[k];
        if (!family_matches(mask, t.family)) continue;
        const f32 reach = radius + t.radius;
        const f32 d2 = math::length_sq(t.position - p);
        if (d2 > reach * reach) continue;
        if (!best.found || d2 < best.d2) {
            best.found = true;
            best.named_index = k;
            best.d2 = d2;
        }
    }
    return best;
}

} // namespace

SwarmerSystem::SwarmerSystem() {
    for (u32 f = 0; f < kFamilyCount; ++f) chaff_radius_[f] = 0.5f;
}

void SwarmerSystem::set_chaff_radii(const f32* radii, usize count) {
    if (radii == nullptr) return;
    const usize n = count < kFamilyCount ? count : kFamilyCount;
    for (usize f = 0; f < n; ++f) chaff_radius_[f] = math::max(radii[f], 0.0f);
}

SwarmerStats SwarmerSystem::update(SwarmerBuffers& sw,
                                   ChaffBuffers& chaff,
                                   const SpatialHash& hash,
                                   NamedTargetList& named,
                                   const DistanceField* sdf,
                                   const FlowField* flow,
                                   const Rect& world_bounds,
                                   Rng& rng,
                                   f32 dt,
                                   CombatEventSink* events) {
    // Threaded through for signature symmetry with ProjectileSystem, and
    // deliberately unused: every stochastic choice a swarmer makes comes from
    // its own seed stream (see the file header).
    (void)rng;

    SwarmerStats stats;
    effects_.clear();

    // Queues hit points against a named agent and credits the owner. The kill
    // flag is decided here, off the running total, because SimWorld lands the
    // damage after this loop and the owner is gone from view by then.
    const auto hit_named = [&](usize i, usize k, f32 hp) {
        if (hp <= 0.0f) return;
        named.damage[k] += hp;
        stats.named_damage += hp;
        const bool was_alive = named.health_left[k] > 0.0f;
        named.health_left[k] -= hp;
        if (attribution_ != nullptr && sw.owner[i].valid()) {
            attribution_->record_named(sw.owner[i], hp, was_alive && named.health_left[k] <= 0.0f);
        }
    };

    const usize entry_count = sw.count();
    const usize prev_live = static_cast<usize>(last_.live);
    stats.spawned_this_tick =
        static_cast<u32>(entry_count > prev_live ? entry_count - prev_live : 0);

    if (entry_count == 0 || dt <= 0.0f) {
        stats.live = static_cast<u32>(entry_count);
        last_ = stats;
        return last_;
    }

    // ---- SQUADS. Shooters released together (same owner, same group) march
    // and hold as one rank, and so do arbor grabbers (the LANE WALL). This
    // pre-pass gives every member its squad's centroid, its rank within the
    // squad (by slot, over the members still alive, so a squad that has lost
    // units closes up) and the squad size. The line itself is built per
    // member in the main loop: perpendicular to the squad's approach to the
    // member's target for a shooter (formation_slot), across the flow for a
    // wall (wall_slot). Sorted, so the ranks are a pure function of the
    // store's contents and not of compaction order.
    squad_scratch_.clear();
    for (usize i = 0; i < entry_count; ++i) {
        const SwarmerKind k = sw.profile_of(i).kind;
        if (k == SwarmerKind::Shooter) {
            squad_scratch_.push_back(SquadKey{sw.owner[i].value, sw.group[i], 0u, sw.slot[i],
                                              static_cast<u32>(i)});
        } else if (k == SwarmerKind::ArborGrabber) {
            // A wall is EVERY live arbor grabber a tower owns, not one
            // volley: four volleys each holding their own two-unit rank on
            // the same spot is a clump. The release tick still orders the
            // rank, so a newcomer takes the outer slot and the line re-spreads.
            squad_scratch_.push_back(SquadKey{sw.owner[i].value, 0u, sw.group[i], sw.slot[i],
                                              static_cast<u32>(i)});
        }
    }
    std::sort(squad_scratch_.begin(), squad_scratch_.end(), [](const SquadKey& a, const SquadKey& b) {
        if (a.owner != b.owner) return a.owner < b.owner;
        if (a.group != b.group) return a.group < b.group;
        if (a.order != b.order) return a.order < b.order;
        if (a.slot != b.slot) return a.slot < b.slot;
        return a.index < b.index;
    });
    if (squad_rank_.size() < entry_count) {
        squad_rank_.resize(entry_count);
        squad_size_.resize(entry_count);
        squad_cx_.resize(entry_count);
        squad_cy_.resize(entry_count);
    }
    for (usize a = 0; a < squad_scratch_.size();) {
        usize b = a;
        Vec2 sum{0.0f, 0.0f};
        while (b < squad_scratch_.size() && squad_scratch_[b].owner == squad_scratch_[a].owner &&
               squad_scratch_[b].group == squad_scratch_[a].group) {
            const u32 i = squad_scratch_[b].index;
            sum += Vec2{sw.pos_x[i], sw.pos_y[i]};
            ++b;
        }
        const u32 n = static_cast<u32>(b - a);
        const Vec2 centroid = sum / static_cast<f32>(n);
        for (usize k = a; k < b; ++k) {
            const u32 i = squad_scratch_[k].index;
            squad_rank_[i] = static_cast<u32>(k - a);
            squad_size_[i] = n;
            squad_cx_[i] = centroid.x;
            squad_cy_[i] = centroid.y;
        }
        a = b;
    }

    // What a bomber leaves behind. Shared by the contact and the expiry paths
    // so the two can never disagree about what a detonation is.
    const auto detonate = [&](usize i, const SwarmerProfile& pr, Vec2 p, Vec2 dir) {
        switch (pr.kind) {
        case SwarmerKind::Bomber: {
            SwarmerBurst b;
            b.origin = p;
            b.radius = pr.burst_radius;
            b.damage = pr.burst_damage;
            b.seconds = pr.burst_seconds;
            b.falloff = pr.burst_falloff;
            b.named_damage = pr.burst_named_damage;
            b.family_mask = sw.family_mask[i];
            b.owner = sw.owner[i];
            b.source = pr.source;
            b.visual_id = sw.visual_id[i];
            effects_.bursts.push_back(b);
            break;
        }
        case SwarmerKind::SlowBomber: {
            SwarmerSlowZone z;
            z.origin = p;
            z.radius = pr.zone_radius;
            z.duration = pr.zone_duration;
            z.slow_duration = pr.slow_duration;
            z.slow_factor = pr.slow_factor;
            z.family_mask = sw.family_mask[i];
            z.owner = sw.owner[i];
            z.source = pr.source;
            z.visual_id = sw.visual_id[i];
            effects_.zones.push_back(z);
            break;
        }
        case SwarmerKind::MucusBomber: {
            SwarmerSplash s;
            s.origin = p;
            s.droplets = pr.splash_droplets;
            s.radius = pr.splash_radius;
            s.speed = pr.splash_speed;
            s.lifetime = pr.splash_lifetime;
            s.dps = pr.splash_dps;
            s.mark_seconds = pr.mark_seconds;
            s.family_mask = sw.family_mask[i];
            s.owner = sw.owner[i];
            s.source = pr.source;
            s.visual_id = sw.visual_id[i];
            s.seed = sw.seed[i];
            effects_.splashes.push_back(s);
            break;
        }
        default:
            return;
        }
        ++stats.detonated;
        sw.flags[i] |= swarmer_flags::kPendingKill;
        if (events) {
            CombatEvent e = make_event(CombatEventType::Explosion, pr.source, p, dir, sw.visual_id[i]);
            e.radius = pr.kind == SwarmerKind::Bomber      ? pr.burst_radius
                     : pr.kind == SwarmerKind::SlowBomber  ? pr.zone_radius
                                                           : pr.splash_radius;
            e.magnitude = 1.0f;
            events->push(e);
        }
    };

    // Adopt a search hit as swarmer i's target. A new target is a new chase.
    const auto adopt = [&](usize i, const SearchHit& hit) {
        sw.chase[i] = 0.0f;
        if (hit.named_index != NamedTargetList::npos) {
            sw.target_generation[i] = 0u;
            sw.target_named[i] = named.items[hit.named_index].id;
        } else {
            sw.target_index[i] = hit.chaff_index;
            sw.target_generation[i] = chaff.generation[hit.chaff_index];
            sw.target_named[i] = EntityId{};
        }
    };

    // KITING (file header), shared by the shooter and the grabber body. If
    // anything targetable is inside the unit's kite radius, the velocity to
    // retreat with this tick (wander already folded in); otherwise nothing,
    // and the caller holds whatever ground it holds. The search is measured
    // to the rim, like every other reach here, so a boss is "too close"
    // where its body starts.
    struct Kite {
        Vec2 desired{0.0f, 0.0f};
        bool kiting = false;
    };
    const auto kite_steer = [&](Vec2 p, Vec2 dir, const SwarmerProfile& pr, u8 mask, Vec2 wander) {
        Kite out;
        if (pr.kite_fraction <= 0.0f) return out;
        const f32 kite_r = pr.attach_radius * pr.kite_fraction;
        const SearchHit threat = search_nearest(chaff, hash, named, scratch_, p, kite_r, mask,
                                                ChaffBuffers::npos, NamedTargetList::npos,
                                                /*ignore_latched=*/true);
        if (!threat.found) return out;
        const bool threat_named = threat.named_index != NamedTargetList::npos;
        const Vec2 threat_pos =
            threat_named ? named.items[threat.named_index].position
                         : Vec2{chaff.pos_x[threat.chaff_index], chaff.pos_y[threat.chaff_index]};
        const f32 threat_rim = threat_named ? named.items[threat.named_index].radius : 0.0f;
        const f32 gap = math::max(std::sqrt(threat.d2) - threat_rim, 0.0f);

        // Away from the threat, bent toward where the horde is heading. The
        // flow is zero off the field (a pocket, a wall), which simply leaves
        // "away" -- and if even that is degenerate (standing on the threat),
        // away from the target it is engaging.
        //
        // The flow may only BEND the retreat, never point it back at the
        // threat: a unit that has ended up upstream of the crowd (behind it,
        // looking down the lane) reads a flow that runs straight into the
        // thing it is fleeing, and summing that with "away" cancelled to a
        // sideways shuffle into the nearest wall. So the component of the
        // flow along -away is dropped and only what is left -- across or
        // ahead -- gets a vote.
        Vec2 away = math::normalize_safe(p - threat_pos);
        if (away.x == 0.0f && away.y == 0.0f) away = -dir;
        Vec2 downstream = flow != nullptr ? flow->sample(p) : Vec2{0.0f, 0.0f};
        const f32 into = downstream.x * away.x + downstream.y * away.y;
        if (into < 0.0f) downstream -= away * into;
        Vec2 retreat = math::normalize_safe(away + downstream * pr.kite_flow_weight);
        if (retreat.x == 0.0f && retreat.y == 0.0f) retreat = away;

        // Full retreat speed with the threat two thirds of the way in, fading
        // to a stop at the edge: the unit settles on the kite circle instead
        // of bouncing off it, and a horde walking at it meets a wall of
        // ground being given up faster than it can take it.
        const f32 urgency = math::saturate(3.0f * (1.0f - gap / math::max(kite_r, 0.01f)));
        out.desired = retreat * (pr.speed * pr.kite_speed_mult * urgency) + wander * (pr.speed * 0.15f);
        out.kiting = true;
        ++stats.kiting;
        return out;
    };

    // A macrophage's body turns to face what it is going at, so the maw and
    // the resting pseudopods point at the prey. Rate-limited, so a unit whose
    // target hops from one side to the other swings round instead of
    // snapping.
    const auto face_arbor = [&](usize i, Vec2 dir, f32 dt) {
        Vec2& h = sw.arbor_grabber[i].heading;
        const f32 turn = math::saturate(8.0f * dt);
        Vec2 next = math::normalize_safe(h + (dir - h) * turn);
        if (next.x == 0.0f && next.y == 0.0f) next = dir;
        h = next;
    };

    // A macrophage owns one captive per independent branch. Releasing the
    // unit (death, expiry, or leaving the arena) must make every live captive
    // ordinary chaff again before the fixed arm state is cleared.
    const auto release_arbor_captives = [&](ArborGrabberState& arbor) {
        for (ArborArmState& arm : arbor.arms) {
            const usize host = chaff.resolve(arm.captive.chaff);
            if (host != ChaffBuffers::npos) {
                chaff.flags[host] &= static_cast<u8>(~chaff_flags::kHidden);
            }
        }
        const Vec2 heading = arbor.heading;
        arbor = ArborGrabberState{};
        arbor.heading = heading;
    };

    for (usize i = 0; i < entry_count; ++i) {
        // Killed by the horde since the last compaction (sim/hostile runs
        // after this update, so its kills wait a tick here). A dead unit does
        // not act: no chase, no shot, no detonation.
        if ((sw.flags[i] & swarmer_flags::kPendingKill) != 0) {
            if (sw.profile_of(i).kind == SwarmerKind::ArborGrabber) {
                release_arbor_captives(sw.arbor_grabber[i]);
            }
            continue;
        }

        sw.life[i] -= dt;

        const SwarmerProfile& pr = sw.profile_of(i);
        Vec2 p{sw.pos_x[i], sw.pos_y[i]};
        const Vec2 v{sw.vel_x[i], sw.vel_y[i]};

        // ---- 1. Retirement, checked first so a spent swarmer never gets to
        // act on the tick it dies. Leaving the world is a plain dissolve even
        // for a bomber: nothing off the map is worth a circle.
        if (!world_bounds.contains(p)) {
            if (pr.kind == SwarmerKind::ArborGrabber) release_arbor_captives(sw.arbor_grabber[i]);
            sw.flags[i] |= swarmer_flags::kPendingKill;
            ++stats.expired;
            continue;
        }
        if (sw.life[i] <= 0.0f) {
            if (pr.kind == SwarmerKind::ArborGrabber) release_arbor_captives(sw.arbor_grabber[i]);
            if (swarmer_kind_detonates(pr.kind)) {
                detonate(i, pr, p, v);
            } else {
                sw.flags[i] |= swarmer_flags::kPendingKill;
                ++stats.expired;
                if (events) {
                    events->push(make_event(CombatEventType::ProjectileExpired, pr.source, p, v,
                                            sw.visual_id[i]));
                }
            }
            continue;
        }

        const u8 mask = sw.family_mask[i];

        // ---- BUILDER. Hunts nothing: it was released with a site to walk to
        // and that is the whole job. Steer there with the same wander every
        // seeking unit has, and on arrival ask for the scar and be spent --
        // the request names the SITE, not where the unit actually stopped,
        // so the wall lands where the tower's site picker already checked
        // it could. A builder released with no site (nowhere to build) just
        // drifts and dissolves like a targetless latcher does.
        if (pr.kind == SwarmerKind::Builder) {
            if ((sw.flags[i] & swarmer_flags::kHasGoal) == 0) {
                const f32 damp = 1.0f / (1.0f + 1.5f * dt);
                sw.vel_x[i] *= damp;
                sw.vel_y[i] *= damp;
                sw.pos_x[i] = p.x + sw.vel_x[i] * dt;
                sw.pos_y[i] = p.y + sw.vel_y[i] * dt;
                continue;
            }
            const Vec2 goal{sw.goal_x[i], sw.goal_y[i]};
            const Vec2 to_goal = goal - p;
            const f32 goal_dist = math::length(to_goal);
            if (goal_dist <= pr.attach_radius) {
                SwarmerBuild b;
                b.origin = goal;
                b.profile = sw.profile[i];
                b.owner = sw.owner[i];
                b.source = pr.source;
                b.visual_id = sw.visual_id[i];
                effects_.builds.push_back(b);
                ++stats.built;
                sw.flags[i] |= swarmer_flags::kPendingKill;
                if (events) {
                    CombatEvent e = make_event(CombatEventType::ProjectileImpact, pr.source, goal, v,
                                               sw.visual_id[i]);
                    e.radius = pr.scar_half_width;
                    e.magnitude = 0.0f;
                    events->push(e);
                }
                continue;
            }
            u32 s = sw.seed[i];
            const Vec2 wander{signed_unit(s), signed_unit(s)};
            sw.seed[i] = s;
            const Vec2 goal_dir = to_goal / goal_dist;
            const f32 wander_gain =
                0.45f * math::saturate((goal_dist - pr.attach_radius) /
                                       math::max(pr.attach_radius * 6.0f, 0.01f));
            Vec2 want = math::normalize_safe(goal_dir + wander * wander_gain);
            if (want.x == 0.0f && want.y == 0.0f) want = goal_dir;
            // Arrive rather than overshoot: ease off inside a few body
            // lengths so the unit settles on the site instead of orbiting it.
            const f32 arrive = math::min(1.0f, goal_dist / math::max(pr.size * 3.0f, 0.01f));
            const Vec2 desired = want * (pr.speed * math::max(arrive, 0.35f));
            const f32 turn = math::saturate(9.0f * dt);
            sw.vel_x[i] += (desired.x - sw.vel_x[i]) * turn;
            sw.vel_y[i] += (desired.y - sw.vel_y[i]) * turn;
            sw.pos_x[i] = p.x + sw.vel_x[i] * dt;
            sw.pos_y[i] = p.y + sw.vel_y[i] * dt;
            continue;
        }

        // ---- BRANCHING PSEUDOPODS. Macrophage_2 owns three fixed arm slots
        // that acquire different targets and cycle independently. The body
        // braces while any branch is active, but a recovered slot may launch
        // again immediately while its siblings are still pulling.
        if (pr.kind == SwarmerKind::ArborGrabber) {
            ArborGrabberState& arbor = sw.arbor_grabber[i];
            const u32 arm_count = math::min<u32>(pr.arbor_arm_count, kArborMaxArms);
            const f32 max_reach = math::max(pr.attach_radius, pr.size);
            const f32 body_rim = pr.size * kWallContactFraction;

            const auto target_is_held = [&](u32 chaff_index, EntityId named_id) {
                for (u32 a = 0; a < arm_count; ++a) {
                    const ArborArmState& other = arbor.arms[a];
                    if (other.phase == ArborArmPhase::Idle ||
                        other.phase == ArborArmPhase::Recovering) continue;
                    if (named_id.valid() && other.captive.named == named_id) return true;
                    if (!named_id.valid() && other.captive.chaff.valid() &&
                        other.captive.chaff.index == chaff_index) return true;
                }
                return false;
            };

            const auto begin_recovery = [&](ArborArmState& arm) {
                const usize host = chaff.resolve(arm.captive.chaff);
                if (host != ChaffBuffers::npos) {
                    chaff.flags[host] &= static_cast<u8>(~chaff_flags::kHidden);
                }
                arm.captive = GrabberCaptive{};
                arm.phase = ArborArmPhase::Recovering;
                arm.time = 0.0f;
                arm.grip = 0.0f;
            };

            // Advance every live tree before filling free slots. Target
            // headings are refreshed during extension so the fine tips track
            // moving prey instead of behaving like rigid ballistic spears.
            for (u32 a = 0; a < arm_count; ++a) {
                ArborArmState& arm = arbor.arms[a];
                if (arm.phase == ArborArmPhase::Idle) continue;
                arm.time += dt;

                if (arm.phase == ArborArmPhase::Recovering) {
                    const f32 t = math::saturate(
                        arm.time / math::max(pr.arbor_recover_seconds, 0.001f));
                    arm.reach = body_rim * (1.0f - math::smoothstep01(t));
                    if (t >= 1.0f) arm = ArborArmState{};
                    continue;
                }

                usize host = ChaffBuffers::npos;
                usize named_idx = NamedTargetList::npos;
                Vec2 target{};
                f32 target_radius = 0.0f;
                bool valid = false;
                if (arm.captive.named.valid()) {
                    named_idx = named.find(arm.captive.named);
                    if (named_idx != NamedTargetList::npos &&
                        named.health_left[named_idx] > 0.0f &&
                        family_matches(mask, named.items[named_idx].family)) {
                        target = named.items[named_idx].position;
                        target_radius = named.items[named_idx].radius;
                        valid = true;
                    }
                } else {
                    host = chaff.resolve(arm.captive.chaff);
                    if (host != ChaffBuffers::npos) {
                        const bool pulling = arm.phase == ArborArmPhase::Pulling;
                        valid = pulling || targetable(chaff, static_cast<u32>(host), mask);
                        if (valid) {
                            target = Vec2{chaff.pos_x[host], chaff.pos_y[host]};
                            target_radius = chaff_radius_[chaff.family[host] < kFamilyCount
                                                            ? chaff.family[host]
                                                            : 0u];
                        }
                    }
                }
                if (!valid) {
                    begin_recovery(arm);
                    continue;
                }

                Vec2 to_target = target - p;
                const f32 target_distance = math::length(to_target);
                Vec2 target_dir = math::normalize_safe(to_target);
                if (target_dir.x == 0.0f && target_dir.y == 0.0f) target_dir = arm.heading;
                if (arm.phase != ArborArmPhase::Pulling) {
                    const f32 turn = math::saturate(18.0f * dt);
                    arm.heading = math::normalize_safe(arm.heading + (target_dir - arm.heading) * turn);
                }

                if (arm.phase == ArborArmPhase::Extending) {
                    const f32 t = math::saturate(
                        arm.time / math::max(pr.arbor_extend_seconds, 0.001f));
                    const f32 tip = math::clamp(target_distance - target_radius * 0.15f,
                                                body_rim, max_reach);
                    arm.reach = body_rim + (tip - body_rim) * math::smoothstep01(t);
                    arm.grip = 0.0f;
                    if (t >= 1.0f) {
                        arm.phase = ArborArmPhase::Latching;
                        arm.time = 0.0f;
                    }
                } else if (arm.phase == ArborArmPhase::Latching) {
                    const f32 t = math::saturate(
                        arm.time / math::max(pr.arbor_latch_seconds, 0.001f));
                    arm.reach = math::clamp(target_distance - target_radius * 0.15f,
                                            body_rim, max_reach);
                    arm.grip = math::smoothstep01(t);
                    if (t >= 1.0f) {
                        arm.captive.offset = target - p;
                        if (host != ChaffBuffers::npos) {
                            chaff.flags[host] |= chaff_flags::kHidden;
                            chaff.vel_x[host] = 0.0f;
                            chaff.vel_y[host] = 0.0f;
                        }
                        arm.phase = ArborArmPhase::Pulling;
                        arm.time = 0.0f;
                        arm.grip = 1.0f;
                    }
                } else if (arm.phase == ArborArmPhase::Pulling) {
                    const f32 t = math::saturate(
                        arm.time / math::max(pr.arbor_pull_seconds, 0.001f));
                    const f32 eased = math::smoothstep01(t);
                    arm.reach = math::max(body_rim * 0.25f,
                                          math::length(arm.captive.offset) * (1.0f - eased));
                    arm.grip = 1.0f;
                    if (host != ChaffBuffers::npos) {
                        const Vec2 swallowed = arm.heading * (body_rim * 0.08f);
                        const Vec2 q = p + arm.captive.offset * (1.0f - eased) + swallowed * eased;
                        chaff.pos_x[host] = q.x;
                        chaff.pos_y[host] = q.y;
                        chaff.vel_x[host] = 0.0f;
                        chaff.vel_y[host] = 0.0f;
                    }
                    if (t >= 1.0f) {
                        PathogenFamily family = PathogenFamily::Count;
                        f32 radius = math::max(target_radius, 0.6f);
                        bool killed = false;
                        if (named_idx != NamedTargetList::npos) {
                            family = static_cast<PathogenFamily>(named.items[named_idx].family);
                            hit_named(i, named_idx, named.health_left[named_idx]);
                            killed = true;
                        } else if (host != ChaffBuffers::npos) {
                            family = static_cast<PathogenFamily>(chaff.family[host]);
                            chaff.flags[host] &= static_cast<u8>(~chaff_flags::kHidden);
                            const f32 before = chaff.density[host];
                            chaff.apply_density_loss(host, before);
                            stats.density_removed += before;
                            killed = true;
                            if (attribution_ != nullptr && sw.owner[i].valid()) {
                                attribution_->record_chaff(sw.owner[i], chaff.family[host], before, true);
                            }
                        }
                        if (killed) {
                            ++stats.hosts_finished;
                            if (events) {
                                CombatEvent e = make_event(CombatEventType::ProjectileImpact,
                                                           pr.source, p, arm.heading,
                                                           sw.visual_id[i]);
                                e.target_family = family;
                                e.radius = radius;
                                e.magnitude = 1.0f;
                                events->push(e);
                            }
                        }
                        arm.captive = GrabberCaptive{};
                        arm.phase = ArborArmPhase::Recovering;
                        arm.time = 0.0f;
                        arm.grip = 0.0f;
                    }
                }
            }

            // Fill every idle slot from one deterministic nearest-target walk
            // per arm. Targets already claimed by a sibling are skipped, so
            // a three-arm unit visibly fans out instead of drawing three trees
            // on top of the same pathogen.
            for (u32 a = 0; a < arm_count; ++a) {
                ArborArmState& arm = arbor.arms[a];
                if (arm.phase != ArborArmPhase::Idle) continue;

                SearchHit best;
                scratch_.clear();
                hash.query_circle(p, max_reach, scratch_);
                const f32 max_reach2 = max_reach * max_reach;
                for (const u32 idx : scratch_) {
                    if (!targetable(chaff, idx, mask) || target_is_held(idx, EntityId{})) continue;
                    const f32 dx = chaff.pos_x[idx] - p.x;
                    const f32 dy = chaff.pos_y[idx] - p.y;
                    const f32 d2 = dx * dx + dy * dy;
                    if (d2 > max_reach2) continue;
                    if (!best.found || d2 < best.d2 ||
                        (d2 == best.d2 && idx < best.chaff_index)) {
                        best = SearchHit{true, idx, NamedTargetList::npos, d2};
                    }
                }
                for (usize k = 0; k < named.items.size(); ++k) {
                    const NamedTarget& candidate = named.items[k];
                    if (named.health_left[k] <= 0.0f ||
                        !family_matches(mask, candidate.family) ||
                        target_is_held(0u, candidate.id)) continue;
                    const Vec2 delta = candidate.position - p;
                    const f32 rim_distance = math::max(math::length(delta) - candidate.radius, 0.0f);
                    const f32 d2 = rim_distance * rim_distance;
                    if (d2 > max_reach2) continue;
                    if (!best.found || d2 < best.d2) {
                        best = SearchHit{true, 0u, k, d2};
                    }
                }
                if (!best.found) continue;

                const Vec2 target = best.named_index != NamedTargetList::npos
                                        ? named.items[best.named_index].position
                                        : Vec2{chaff.pos_x[best.chaff_index],
                                               chaff.pos_y[best.chaff_index]};
                Vec2 heading = math::normalize_safe(target - p);
                if (heading.x == 0.0f && heading.y == 0.0f) heading = arbor.heading;
                arm.heading = heading;
                arm.phase = ArborArmPhase::Extending;
                arm.time = 0.0f;
                arm.reach = body_rim;
                arm.grip = 0.0f;
                if (best.named_index != NamedTargetList::npos) {
                    arm.captive.named = named.items[best.named_index].id;
                } else {
                    arm.captive.chaff = ChaffHandle{best.chaff_index,
                                                    chaff.generation[best.chaff_index]};
                }
                ++stats.arms_launched;
            }

            u32 active = 0;
            u32 gripping = 0;
            Vec2 heading_sum{0.0f, 0.0f};
            for (u32 a = 0; a < arm_count; ++a) {
                const ArborArmState& arm = arbor.arms[a];
                if (arm.phase == ArborArmPhase::Idle) continue;
                ++active;
                heading_sum += arm.heading;
                if (arm.phase == ArborArmPhase::Latching ||
                    arm.phase == ArborArmPhase::Pulling) ++gripping;
            }
            if (active > 0) {
                Vec2 facing = math::normalize_safe(heading_sum);
                if (facing.x != 0.0f || facing.y != 0.0f) {
                    const f32 turn = math::saturate(10.0f * dt);
                    arbor.heading = math::normalize_safe(
                        arbor.heading + (facing - arbor.heading) * turn);
                }
                stats.arms_attached += gripping;
                ++stats.attached;
                // The body does not brace while its arms work -- inside a
                // horde the arms never stop, and a body that froze for them
                // would never reach its place in the LANE WALL. It creeps to
                // its slot at half speed instead, with no wander; the arms
                // read the body's position every tick, so they follow. The
                // slot is taken about the squad's own centroid (no target,
                // no shift along the flow): a unit with arms busy is where
                // the horde is, and the line has nowhere better to be.
                const Vec2 slot_pos = wall_slot(sw, i, pr, Vec2{squad_cx_[i], squad_cy_[i]},
                                                pr.attach_radius * 0.7f, flow, sdf, dt);
                const Vec2 to_slot = slot_pos - p;
                const f32 slot_dist = math::length(to_slot);
                const Vec2 desired = slot_dist > math::kEpsilon
                                         ? to_slot * (math::min(slot_dist * 3.0f, pr.speed * 0.5f) / slot_dist)
                                         : Vec2{0.0f, 0.0f};
                const f32 turn = math::saturate(6.0f * dt);
                sw.vel_x[i] += (desired.x - sw.vel_x[i]) * turn;
                sw.vel_y[i] += (desired.y - sw.vel_y[i]) * turn;
                sw.pos_x[i] = p.x + sw.vel_x[i] * dt;
                sw.pos_y[i] = p.y + sw.vel_y[i] * dt;
                continue;
            }
        }

        // ---- Resolve the current target. A handle that no longer resolves,
        // or resolves to something no longer targetable, means the target
        // died (or burrowed) — the ordinary end of a successful engagement,
        // not an error.
        usize host = ChaffBuffers::npos;
        usize named_idx = NamedTargetList::npos;
        {
            const ChaffHandle held = sw.target(i);
            if (held.valid()) {
                host = chaff.resolve(held);
                if (host != ChaffBuffers::npos && !targetable(chaff, static_cast<u32>(host), mask)) {
                    host = ChaffBuffers::npos;
                }
            } else if (sw.target_named[i].valid()) {
                named_idx = named.find(sw.target_named[i]);
                if (named_idx != NamedTargetList::npos &&
                    !family_matches(mask, named.items[named_idx].family)) {
                    named_idx = NamedTargetList::npos;
                }
            }
            const bool had_target = held.valid() || sw.target_named[i].valid();
            if (had_target && host == ChaffBuffers::npos && named_idx == NamedTargetList::npos) {
                // Only count a finished target for a swarmer that was actually
                // engaged: a seeking swarmer whose quarry died to something
                // else has not finished anything.
                if ((sw.flags[i] & swarmer_flags::kAttached) != 0 &&
                    pr.kind != SwarmerKind::ArborGrabber) {
                    ++stats.hosts_finished;
                }
                sw.target_generation[i] = 0u;
                sw.target_named[i] = EntityId{};
                sw.chase[i] = 0.0f;
                sw.flags[i] &= static_cast<u8>(~swarmer_flags::kAttached);
                sw.attach[i] = 0.0f;
            }
        }

        // ---- 2. TARGETLESS: pay for a search. The expensive path.
        if (host == ChaffBuffers::npos && named_idx == NamedTargetList::npos) {
            ++stats.searching;
            const SearchHit hit = search_nearest(chaff, hash, named, scratch_, p, pr.search_radius,
                                                 mask, ChaffBuffers::npos, NamedTargetList::npos);
            if (hit.found) {
                adopt(i, hit);
                if (hit.named_index != NamedTargetList::npos) named_idx = hit.named_index;
                else host = hit.chaff_index;
            }
        }

        // ---- Nothing anywhere in reach: drift on, slowing, and let the
        // lifetime run out. Deliberately not killed early — a unit that
        // vanishes the instant its lane is clear makes the cloud pop out of
        // existence between waves instead of dispersing.
        if (host == ChaffBuffers::npos && named_idx == NamedTargetList::npos) {
            const f32 damp = 1.0f / (1.0f + 1.5f * dt);
            sw.vel_x[i] *= damp;
            sw.vel_y[i] *= damp;
            sw.pos_x[i] = p.x + sw.vel_x[i] * dt;
            sw.pos_y[i] = p.y + sw.vel_y[i] * dt;
            if (pr.kind == SwarmerKind::Shooter) sw.cooldown[i] = math::max(0.0f, sw.cooldown[i] - dt);
            continue;
        }

        bool on_named = named_idx != NamedTargetList::npos;
        Vec2 target_pos = on_named ? named.items[named_idx].position
                                   : Vec2{chaff.pos_x[host], chaff.pos_y[host]};
        f32 target_radius = on_named ? named.items[named_idx].radius : 0.0f;
        f32 dist = math::length(target_pos - p);
        // Reach is measured to the target's RIM: a swarmer engages a boss
        // where its body starts, not at a point buried inside it.
        f32 reach = pr.attach_radius + target_radius;

        // ---- STAND YOUR GROUND. A shooter whose target has walked out of
        // its standoff, or a bomber whose target has left its aggro radius,
        // does not chase it if anything else is closer: it re-picks the
        // nearest thing inside its search radius and only follows the old
        // target when there is nothing else. Latchers keep chasing — the
        // hunt is their whole identity. Costs one search per tick while the
        // condition holds, the same price a targetless swarmer pays.
        if (pr.kind != SwarmerKind::Latch) {
            const bool standoff_kind = pr.kind == SwarmerKind::Shooter ||
                                       pr.kind == SwarmerKind::ArborGrabber;
            const f32 leash = standoff_kind ? reach : pr.search_radius + target_radius;
            if (dist > leash) {
                const SearchHit hit = search_nearest(chaff, hash, named, scratch_, p, pr.search_radius,
                                                     mask, on_named ? ChaffBuffers::npos : host,
                                                     on_named ? named_idx : NamedTargetList::npos);
                if (hit.found && hit.d2 < dist * dist) {
                    ++stats.retargeted;
                    adopt(i, hit);
                    sw.flags[i] &= static_cast<u8>(~swarmer_flags::kAttached);
                    sw.attach[i] = 0.0f;
                    on_named = hit.named_index != NamedTargetList::npos;
                    if (on_named) named_idx = hit.named_index;
                    else host = hit.chaff_index;
                    target_pos = on_named ? named.items[named_idx].position
                                          : Vec2{chaff.pos_x[host], chaff.pos_y[host]};
                    target_radius = on_named ? named.items[named_idx].radius : 0.0f;
                    dist = math::length(target_pos - p);
                    reach = pr.attach_radius + target_radius;
                }
            }
        }

        const Vec2 target_vel = on_named ? named.items[named_idx].velocity
                                         : Vec2{chaff.vel_x[host], chaff.vel_y[host]};
        const Vec2 to_target = target_pos - p;
        const Vec2 dir = dist > math::kEpsilon ? to_target / dist : Vec2{1.0f, 0.0f};

        // ---- 4. ENGAGED: inside reach. What happens now is the kind.
        // A latcher that is already riding stays engaged whatever the gap
        // this tick: it is IN the host, and a host that lurches further than
        // attach_radius in one tick must carry it rather than shake it off.
        // Only the host dying or a retarget (both above) let it go.
        const bool was_engaged = (sw.flags[i] & swarmer_flags::kAttached) != 0;
        if (dist <= reach || (was_engaged && pr.kind == SwarmerKind::Latch)) {

            switch (pr.kind) {
            case SwarmerKind::Latch: {
                sw.flags[i] |= swarmer_flags::kAttached;
                ++stats.attached;

                // ENTRY. On the tick it latches the clock starts at zero; it
                // runs up to the profile's attach_seconds and stops there.
                // Progress in [0,1] drives everything below: the position
                // lurches from the clump spot to the host's centre in
                // attach_steps discrete jumps, the renderer shrinks the body
                // by (1 - progress), and the drain waits for progress == 1.
                // The clock lives on the unit, not the host, so a re-latch
                // after the host dies starts the entry over.
                if (!was_engaged) sw.attach[i] = 0.0f;
                else sw.attach[i] = math::min(sw.attach[i] + dt, math::max(pr.attach_seconds, 0.0f));
                const f32 progress = pr.attach_seconds > 0.0f
                                         ? math::saturate(sw.attach[i] / pr.attach_seconds)
                                         : 1.0f;
                const f32 stepped = pr.attach_steps > 1u
                                        ? std::floor(progress * static_cast<f32>(pr.attach_steps)) /
                                              static_cast<f32>(pr.attach_steps)
                                        : progress;

                // Sit just off the host's centre rather than exactly on it, so
                // a dozen swarmers on one agent form a visible clump instead of
                // stacking into a single brighter dot. The offset is derived
                // from the private seed, so it is stable for this swarmer. The
                // entry then pulls it from that spot into the centre. Both are
                // measured off the host's position THIS tick, never integrated,
                // so a latched unit tracks its host exactly and cannot lag.
                const f32 ang = static_cast<f32>(sw.seed[i] & 0xFFFFu) * (math::kTwoPi / 65536.0f);
                const f32 ring = (target_radius + pr.attach_radius * 0.6f) * (1.0f - stepped);
                sw.pos_x[i] = target_pos.x + std::cos(ang) * ring;
                sw.pos_y[i] = target_pos.y + std::sin(ang) * ring;
                // Carry the host's motion so the clump travels with it.
                sw.vel_x[i] = target_vel.x;
                sw.vel_y[i] = target_vel.y;

                if (events && !was_engaged) {
                    CombatEvent e = make_event(CombatEventType::ProjectileImpact, pr.source,
                                               target_pos, to_target, sw.visual_id[i]);
                    e.target_family = on_named
                                          ? static_cast<PathogenFamily>(named.items[named_idx].family)
                                          : static_cast<PathogenFamily>(chaff.family[host]);
                    e.magnitude = 0.0f;
                    events->push(e);
                }

                // Still on its way in: no drain yet. A latched unit rides its
                // host; the host is on tissue.
                if (progress < 1.0f) continue;

                f32 removed = 0.0f;
                if (on_named) {
                    // Armor is a flat reduction per damage event; for a
                    // continuous drain the event is "one second of feeding".
                    const NamedTarget& t = named.items[named_idx];
                    const f32 hp = math::max(0.0f, pr.dps - t.armor) * dt * t.damage_multiplier;
                    hit_named(i, named_idx, hp);
                    removed = hp;
                } else {
                    // A unit feeding on an agent the Goblet Cell has already
                    // weakened (chaff_flags::kMarked) drains it faster, same
                    // flag every other damage path in the sim reads.
                    const f32 drain = (chaff.flags[host] & chaff_flags::kMarked) != 0
                                          ? pr.dps * dt * chaff_flags::kMarkedDamageMultiplier
                                          : pr.dps * dt;
                    const f32 before = chaff.density[host];
                    chaff.apply_density_loss(host, drain);
                    removed = before - chaff.density[host];
                    stats.density_removed += removed;

                    // Off by default; see sim/Attribution.h.
                    if (attribution_ != nullptr && sw.owner[i].valid() && removed > 0.0f) {
                        const bool killed = (chaff.flags[host] & chaff_flags::kPendingKill) != 0;
                        attribution_->record_chaff(sw.owner[i], chaff.family[host], removed, killed);
                    }
                }

                continue;
            }

            case SwarmerKind::Shooter: {
                sw.flags[i] |= swarmer_flags::kAttached;
                ++stats.attached;

                u32 s = sw.seed[i];
                const Vec2 wander{signed_unit(s), signed_unit(s)};
                sw.seed[i] = s;

                // KITING (file header): anything inside the kite radius --
                // the target or a stranger walking through the rank -- and
                // the shooter backs off, still firing.
                const Kite kite = kite_steer(p, dir, pr, mask, wander);
                Vec2 desired = kite.desired;
                const bool kiting = kite.kiting;

                // Otherwise hold the rank: steer to the squad line's slot for
                // this unit (see the pre-pass), which sits at ~80% of reach
                // from the target, with the wander on top so the rank reads
                // as cells jostling rather than a frozen grid.
                if (!kiting) {
                    const Vec2 slot_pos = formation_slot(sw, i, pr, target_pos, reach * 0.8f);
                    const Vec2 to_slot = slot_pos - p;
                    const f32 slot_dist = math::length(to_slot);
                    const Vec2 arrive = slot_dist > math::kEpsilon
                                            ? to_slot * (math::min(slot_dist * 3.0f, pr.speed) / slot_dist)
                                            : Vec2{0.0f, 0.0f};
                    desired = arrive + wander * (pr.speed * 0.15f);
                }
                const f32 turn = math::saturate(6.0f * dt);
                sw.vel_x[i] += (desired.x - sw.vel_x[i]) * turn;
                sw.vel_y[i] += (desired.y - sw.vel_y[i]) * turn;
                sw.pos_x[i] = p.x + sw.vel_x[i] * dt;
                sw.pos_y[i] = p.y + sw.vel_y[i] * dt;

                sw.cooldown[i] -= dt;
                if (sw.cooldown[i] > 0.0f) break;
                sw.cooldown[i] = pr.fire_interval;

                // The round leaves from where the shooter now stands (it has
                // just moved this tick, and while kiting it moves fast) and is
                // aimed at where the target will be when the round gets there
                // (intercept_point), not where it is now. The spread on top is
                // the "spray of cells" read and stays: it scatters a stream
                // AROUND a point that would otherwise land, rather than
                // decorating a miss.
                const Vec2 muzzle{sw.pos_x[i], sw.pos_y[i]};
                const Vec2 lead = intercept_point(muzzle, target_pos, target_vel, pr.round_speed);
                Vec2 line = math::normalize_safe(lead - muzzle);
                if (line.x == 0.0f && line.y == 0.0f) line = dir;
                const f32 jitter = signed_unit(s) * pr.round_spread;
                sw.seed[i] = s;
                const f32 cj = std::cos(jitter);
                const f32 sj = std::sin(jitter);
                const Vec2 aim{line.x * cj - line.y * sj, line.x * sj + line.y * cj};

                SwarmerShot shot;
                shot.origin = muzzle;
                shot.velocity = aim * pr.round_speed;
                shot.damage = pr.round_damage;
                shot.hit_radius = pr.round_hit_radius;
                // Twice the standoff and a little more. A shooter fires from
                // its kite band at a target that may be out near the standoff
                // edge and still closing, so a round that could only just
                // cross the standoff fell short of the crowd behind the target;
                // one that outlives its usefulness is merely a store slot
                // another wanted.
                shot.lifetime = (reach * 2.0f + 2.0f) / math::max(pr.round_speed, 1.0f);
                shot.family_mask = mask;
                shot.owner = sw.owner[i];
                shot.source = pr.source;
                shot.visual_id = sw.visual_id[i];
                effects_.shots.push_back(shot);
                ++stats.shots_fired;

                // Rounds only ever DAMAGE chaff (Projectiles.h), so a shot at
                // a named agent lands as a direct hit here and the spawned
                // round is its tracer. That tracer may still die on a wall.
                if (on_named) {
                    const NamedTarget& t = named.items[named_idx];
                    hit_named(i, named_idx,
                              math::max(0.0f, pr.round_damage - t.armor) * t.damage_multiplier);
                }

                if (events) {
                    CombatEvent e = make_event(CombatEventType::MuzzleFlash, pr.source, muzzle, aim,
                                               sw.visual_id[i]);
                    e.radius = pr.round_hit_radius;
                    e.magnitude = pr.round_damage;
                    events->push(e);
                }
                break;
            }

            case SwarmerKind::Bomber:
            case SwarmerKind::SlowBomber:
            case SwarmerKind::MucusBomber:
                detonate(i, pr, p, dir);
                continue;

            case SwarmerKind::ArborGrabber: {
                // The arms above do the work. The body holds a standoff and
                // kites exactly as a shooter does (KITING in the file
                // header), parked at ~70% of reach so every arm has slack to
                // ride a target that shuffles about and a body shoved off a
                // pathogen's rim does not lose its target out of reach.
                u32 s = sw.seed[i];
                const Vec2 wander{signed_unit(s), signed_unit(s)};
                sw.seed[i] = s;
                const Kite kite = kite_steer(p, dir, pr, mask, wander);
                Vec2 desired = kite.desired;
                // A pathogen riding a friendly host (its own membrane, most
                // likely) is not something to hold a standoff from: backing
                // off from a passenger carries the passenger along and the
                // body walks off forever. It stands, and the arms eat it.
                const bool passenger = !on_named && (chaff.flags[host] & chaff_flags::kLatched) != 0;
                if (!kite.kiting && passenger) {
                    desired = wander * (pr.speed * 0.15f);
                } else if (!kite.kiting) {
                    // An arbor grabber parks in its slot of the LANE WALL
                    // (file header), which is anchored on the squad and does
                    // not give ground to a target pressing into it.
                    const Vec2 hold = wall_slot(sw, i, pr, target_pos, reach * 0.7f, flow, sdf, dt);
                    const Vec2 to_hold = hold - p;
                    const f32 hold_dist = math::length(to_hold);
                    const Vec2 arrive = hold_dist > math::kEpsilon
                                            ? to_hold * (math::min(hold_dist * 3.0f, pr.speed) / hold_dist)
                                            : Vec2{0.0f, 0.0f};
                    desired = arrive + wander * (pr.speed * 0.15f);
                }
                const f32 turn = math::saturate(6.0f * dt);
                sw.vel_x[i] += (desired.x - sw.vel_x[i]) * turn;
                sw.vel_y[i] += (desired.y - sw.vel_y[i]) * turn;
                sw.pos_x[i] = p.x + sw.vel_x[i] * dt;
                sw.pos_y[i] = p.y + sw.vel_y[i] * dt;
                face_arbor(i, dir, dt);
                continue;
            }

            case SwarmerKind::Builder:   // handled above; never reaches here
            case SwarmerKind::Count:
                break;
            }
        } else {
            // ---- 3. SEEKING: steer at the target, with a wander term.
            sw.flags[i] &= static_cast<u8>(~swarmer_flags::kAttached);
            sw.attach[i] = 0.0f;
            if (pr.kind == SwarmerKind::Shooter) sw.cooldown[i] = math::max(0.0f, sw.cooldown[i] - dt);

            // A bomber gives a chase only so long. Past the profile's limit it
            // goes off where it is rather than trailing a target it is not
            // catching: the effect still lands somewhere useful (the target
            // is in reach of the search radius, so the crowd is near), and the
            // unit stops being a shell that never lands.
            if (swarmer_kind_detonates(pr.kind) && pr.chase_seconds > 0.0f) {
                sw.chase[i] += dt;
                if (sw.chase[i] >= pr.chase_seconds) {
                    ++stats.chase_timeouts;
                    detonate(i, pr, p, dir);
                    continue;
                }
            }

            u32 s = sw.seed[i];
            const Vec2 wander{signed_unit(s), signed_unit(s)};
            sw.seed[i] = s;

            // Where to go: straight at the target, or -- for a shooter -- at
            // its slot in the squad's rank, which is what makes a volley of
            // shooters arrive as a line rather than a string.
            Vec2 goal_dir = dir;
            f32 goal_dist = dist - reach;
            if (pr.kind == SwarmerKind::Shooter || pr.kind == SwarmerKind::ArborGrabber) {
                const Vec2 slot_pos = pr.kind == SwarmerKind::Shooter
                                          ? formation_slot(sw, i, pr, target_pos, reach * 0.8f)
                                          : wall_slot(sw, i, pr, target_pos, reach * 0.7f, flow, sdf, dt);
                const Vec2 to_slot = slot_pos - p;
                goal_dist = math::length(to_slot);
                if (goal_dist > math::kEpsilon) goal_dir = to_slot / goal_dist;
            }

            // Wander fades out as the swarmer closes, so the cloud is loose and
            // organic on approach but converges cleanly instead of orbiting its
            // target forever.
            const f32 wander_gain =
                0.45f * math::saturate(goal_dist / math::max(pr.attach_radius * 6.0f, 0.01f));
            Vec2 want = goal_dir + wander * wander_gain;
            want = math::normalize_safe(want);
            if (want.x == 0.0f && want.y == 0.0f) want = goal_dir;

            const Vec2 desired = want * pr.speed;

            // Steer rather than snap: the turn rate is what makes a swarmer arc in
            // and overshoot slightly, and the overshoot is most of the "alive" read.
            const f32 turn = math::saturate(9.0f * dt);
            sw.vel_x[i] += (desired.x - sw.vel_x[i]) * turn;
            sw.vel_y[i] += (desired.y - sw.vel_y[i]) * turn;

            sw.pos_x[i] = p.x + sw.vel_x[i] * dt;
            sw.pos_y[i] = p.y + sw.vel_y[i] * dt;
            if (pr.kind == SwarmerKind::ArborGrabber) face_arbor(i, dir, dt);
        }
    }

    // ---- Arbor grabbers are engaged during any part of their reach cycle.
    for (usize i = 0; i < entry_count; ++i) {
        if ((sw.flags[i] & swarmer_flags::kPendingKill) != 0) continue;
        const SwarmerKind kind = sw.profile_of(i).kind;
        if (kind != SwarmerKind::ArborGrabber) continue;
        bool busy = false;
        for (const ArborArmState& arm : sw.arbor_grabber[i].arms) {
            busy = busy || arm.phase != ArborArmPhase::Idle;
        }
        if (busy) sw.flags[i] |= swarmer_flags::kAttached;
        else sw.flags[i] &= static_cast<u8>(~swarmer_flags::kAttached);
    }

    // ---- BODIES. Contact resolution against other swarmers (symmetric) and
    // against pathogens (one-way), off the post-steering positions, so the
    // wall projection below is the last word on where a unit ends up. A
    // bomber the pass finds touching an enemy goes off here, at the spot it
    // touched -- it has not been pushed anywhere first.
    if (collision_.enabled) {
        resolve_bodies(sw, chaff, hash, named, stats);
        for (const ContactBoom& boom : contact_booms_) {
            const usize i = boom.index;
            const SwarmerProfile& pr = sw.profile_of(i);
            const Vec2 p{sw.pos_x[i], sw.pos_y[i]};
            Vec2 dir = math::normalize_safe(boom.at - p);
            if (dir.x == 0.0f && dir.y == 0.0f) dir = math::normalize_safe(Vec2{sw.vel_x[i], sw.vel_y[i]});
            if (dir.x == 0.0f && dir.y == 0.0f) dir = Vec2{1.0f, 0.0f};
            ++stats.contact_detonations;
            detonate(i, pr, boom.at, dir);
        }
    }

    // ---- WALLS. Every unit that moved under its own steering is projected
    // back onto the tissue: a swarmer is a cell, not a ghost, and a volley
    // that clipped through the vessel wall would land in the next lane over.
    // Same projection the fluid uses (sim/fluid/Fluid.cpp): push out along
    // the clearance gradient to the contact surface and drop the velocity
    // component that was heading into the wall, so units run along walls
    // rather than sticking to them. Latched units are exempt -- they ride a
    // host that is on tissue already -- and so are the units killed above.
    if (sdf != nullptr && sdf->width() > 0) {
        for (usize i = 0; i < entry_count; ++i) {
            if ((sw.flags[i] & swarmer_flags::kPendingKill) != 0) continue;
            const SwarmerProfile& pr = sw.profile_of(i);
            if (pr.kind == SwarmerKind::Latch && (sw.flags[i] & swarmer_flags::kAttached) != 0) continue;
            const f32 contact = pr.size * kWallContactFraction;
            const Vec2 p{sw.pos_x[i], sw.pos_y[i]};
            const f32 clearance = sdf->sample(p);
            if (clearance >= contact) continue;
            const Vec2 nrm = sdf->gradient(p);
            const f32 len2 = math::length_sq(nrm);
            if (len2 <= 1e-8f) continue;   // flat field: nowhere to push
            const Vec2 n = nrm / std::sqrt(len2);
            const f32 push = contact - clearance;
            sw.pos_x[i] = p.x + n.x * push;
            sw.pos_y[i] = p.y + n.y * push;
            const f32 into = sw.vel_x[i] * n.x + sw.vel_y[i] * n.y;
            if (into < 0.0f) {
                sw.vel_x[i] -= n.x * into;
                sw.vel_y[i] -= n.y * into;
            }
            ++stats.wall_contacts;
        }
    }

    // Chaff compaction is the CALLER's job, once, after every damage source.
    sw.compact();

    stats.live = static_cast<u32>(sw.count());
    last_ = stats;
    return last_;
}

/// One 3x3 cell walk of `hash` around `p`, calling `visit(j)` for every index
/// found, own cell first (see gather_neighbours in ChaffSystem.cpp for why
/// own-cell-first matters when the walk is capped). Stops after `budget`
/// visits. Returns false if the walk was cut short.
template <typename Visit>
static void walk_cells(const SpatialHash& hash, Vec2 p, u32 budget, Visit&& visit) {
    static constexpr i32 kCellOrder[9][2] = {
        {0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1},
    };
    const IVec2 c = hash.cell_coord(p);
    const IVec2 dims = hash.grid_dims();
    const u32* indices = hash.indices();
    u32 visited = 0;
    for (const auto& off : kCellOrder) {
        const i32 cx = c.x + off[0];
        const i32 cy = c.y + off[1];
        if (cx < 0 || cy < 0 || cx >= dims.x || cy >= dims.y) continue;
        u32 begin, end;
        hash.cell_range(static_cast<u32>(cy) * static_cast<u32>(dims.x) + static_cast<u32>(cx),
                        begin, end);
        for (u32 k = begin; k < end; ++k) {
            if (visited++ >= budget) return;
            visit(indices[k]);
        }
    }
}

/// The BODIES pass. Two Jacobi half-steps: every swarmer reads the positions
/// as they stand after steering and writes ONLY its own displacement, then
/// all displacements land at once. A friendly pair therefore sees each other
/// at the same distance and takes the same half-overlap each, whichever order
/// the store happens to hold them in.
///
/// Both walks are capped (SwarmerCollisionTuning::max_neighbours). Truncating
/// in CSR order is a pure function of positions, so a capped walk is still
/// deterministic; and contact is self-limiting anyway -- geometry bounds how
/// many bodies can overlap one unit once they are no longer allowed to
/// interpenetrate.
void SwarmerSystem::resolve_bodies(SwarmerBuffers& sw, ChaffBuffers& chaff,
                                   const SpatialHash& hash, const NamedTargetList& named,
                                   SwarmerStats& stats) {
    const usize n = sw.count();
    contact_booms_.clear();
    if (n == 0) return;

    // The swarmer-only broadphase mirrors the chaff hash's grid, so one 3x3
    // walk reaches the same distance in both. Reconfigured only when the
    // chaff hash changed shape (a level load), never per tick.
    {
        const Rect& b = hash.bounds();
        const Rect& mine = swarmer_hash_.bounds();
        if (mine.min.x != b.min.x || mine.min.y != b.min.y || mine.max.x != b.max.x ||
            mine.max.y != b.max.y || swarmer_hash_.cell_size() != hash.cell_size() ||
            swarmer_hash_.grid_dims().x == 0) {
            SpatialHashDesc d;
            d.bounds = b;
            d.cell_size = hash.cell_size();
            swarmer_hash_.configure(d);
        }
    }
    swarmer_hash_.rebuild(sw.pos_x.data(), sw.pos_y.data(), n, nullptr);

    if (push_x_.size() < n) {
        push_x_.resize(n);
        push_y_.resize(n);
    }

    const SwarmerCollisionTuning& ct = collision_;
    const f32* px = sw.pos_x.data();
    const f32* py = sw.pos_y.data();
    constexpr f32 kEpsSq = 1e-8f;

    // Which swarmers have a body at all. Latch units never do (file header),
    // and a unit already retiring this tick is not solid to anyone.
    const auto solid = [&](usize j) {
        if ((sw.flags[j] & swarmer_flags::kPendingKill) != 0) return false;
        return sw.profile_of(j).kind != SwarmerKind::Latch;
    };
    // A pathogen with a body: alive, not already dying, not burrowed -- a
    // burrowed agent is under the tissue, so nothing can stand on it -- and
    // not latched onto a friendly host, which puts it inside that host's
    // membrane by construction. Pushing a host out of its own passenger every
    // tick would walk the pair across the map together, and a bomber going
    // off on a passenger it has already been carrying for a tick is not a
    // contact.
    const auto solid_chaff = [&](u32 j) {
        const u8 f = chaff.flags[j];
        return (f & chaff_flags::kAlive) != 0 && (f & chaff_flags::kPendingKill) == 0 &&
               (f & (chaff_flags::kHidden | chaff_flags::kLatched)) == 0;
    };

    for (usize i = 0; i < n; ++i) {
        push_x_[i] = 0.0f;
        push_y_[i] = 0.0f;
        if (!solid(i)) continue;

        const SwarmerProfile& pr = sw.profile_of(i);
        const Vec2 p{px[i], py[i]};
        const f32 size = pr.size * kWallContactFraction;
        const bool bomber = swarmer_kind_detonates(pr.kind);
        const f32 boom_mult = bomber ? ct.bomber_contact_mult : 0.0f;
        const u8 mask = sw.family_mask[i];
        // A builder takes only its profile's share of the crowd's shove
        // (Swarmers.h, builder_crowd_push); everyone else takes all of it --
        // less the part a body-blocking unit hands back to the pathogen
        // (SwarmerProfile::body_block, BODIES in the file header).
        const f32 block = math::saturate(pr.body_block);
        const f32 enemy_share =
            (pr.kind == SwarmerKind::Builder ? math::saturate(pr.builder_crowd_push) : 1.0f) *
            (1.0f - block);

        Vec2 correction{0.0f, 0.0f};
        u32 contacts = 0;
        bool boom = false;
        Vec2 boom_at = p;
        // The touch point on this unit's membrane, toward an enemy at `to_me`
        // away (a vector from the enemy to us). A bomber sitting exactly on
        // an enemy has no direction to offer; it goes off where it is.
        const auto membrane_toward = [&](Vec2 to_me, f32 d2) {
            if (d2 < kEpsSq) return p;
            return p - to_me * (size / std::sqrt(d2));
        };

        // ---- Friendlies: half the overlap each. The neighbour computes and
        // applies the other half on its own turn.
        walk_cells(swarmer_hash_, p, ct.max_neighbours, [&](u32 j) {
            if (j == i || !solid(j)) return;
            const f32 dx = p.x - px[j];
            const f32 dy = p.y - py[j];
            const f32 d2 = dx * dx + dy * dy;
            const f32 contact =
                (size + sw.profile_of(j).size * kWallContactFraction) * ct.friendly_spacing_mult;
            if (d2 >= contact * contact || d2 < kEpsSq) return;
            const f32 d = std::sqrt(d2);
            const f32 c = (contact - d) * 0.5f * ct.friendly_stiffness / d;
            correction.x += dx * c;
            correction.y += dy * c;
            ++contacts;
        });
        stats.friendly_contacts += contacts;

        // ---- Pathogens: the whole overlap, ours to resolve. A bomber inside
        // any enemy's contact circle is done -- record it and skip the push,
        // it is detonating where it stands. A body-blocking unit hands its
        // `block` share of the overlap to the pathogen instead, written into
        // the chaff store on the spot: serial, index order, so two walls
        // shoving one pathogen land in a fixed order and the result is a
        // pure function of positions. Position only -- the pathogen keeps
        // its velocity into the wall so it stays pressed against it.
        u32 enemy_contacts = 0;
        u32 blocked = 0;
        walk_cells(hash, p, ct.max_neighbours, [&](u32 j) {
            if (boom || !solid_chaff(j)) return;
            const f32 dx = p.x - chaff.pos_x[j];
            const f32 dy = p.y - chaff.pos_y[j];
            const f32 d2 = dx * dx + dy * dy;
            const f32 body = size + chaff_radius_[chaff.family[j] < kFamilyCount ? chaff.family[j] : 0u];
            if (bomber && family_matches(mask, chaff.family[j])) {
                const f32 fuse = body * boom_mult;
                if (d2 < fuse * fuse) {
                    boom = true;
                    boom_at = membrane_toward(Vec2{dx, dy}, d2);
                    return;
                }
            }
            if (enemy_share <= 0.0f && block <= 0.0f) return;
            const f32 contact = body * ct.enemy_spacing_mult;
            if (d2 >= contact * contact || d2 < kEpsSq) return;
            const f32 d = std::sqrt(d2);
            const f32 overlap = (contact - d) * ct.enemy_stiffness / d;
            if (block > 0.0f) {
                chaff.pos_x[j] -= dx * overlap * block;
                chaff.pos_y[j] -= dy * overlap * block;
                ++blocked;
            }
            if (enemy_share <= 0.0f) return;
            const f32 c = overlap * enemy_share;
            correction.x += dx * c;
            correction.y += dy * c;
            ++enemy_contacts;
        });
        for (usize k = 0; k < named.items.size() && !boom; ++k) {
            const NamedTarget& t = named.items[k];
            const Vec2 to_me = p - t.position;
            const f32 d2 = math::length_sq(to_me);
            const f32 body = size + t.radius;
            if (bomber && family_matches(mask, t.family)) {
                const f32 fuse = body * boom_mult;
                if (d2 < fuse * fuse) {
                    boom = true;
                    boom_at = membrane_toward(to_me, d2);
                    break;
                }
            }
            if (enemy_share <= 0.0f) continue;
            const f32 contact = body * ct.enemy_spacing_mult;
            if (d2 >= contact * contact || d2 < kEpsSq) continue;
            const f32 d = std::sqrt(d2);
            const f32 c = (contact - d) * ct.enemy_stiffness * enemy_share / d;
            correction += to_me * c;
            ++enemy_contacts;
        }
        if (boom) {
            contact_booms_.push_back(ContactBoom{static_cast<u32>(i), boom_at});
            continue;
        }
        stats.body_blocks += blocked;
        stats.enemy_contacts += enemy_contacts;
        contacts += enemy_contacts;
        if (contacts == 0) continue;

        // Averaged over the contacts, not summed, so a unit buried in a jam
        // takes one sensible step and not n of them (the argument is spelled
        // out on NeighbourSample::contact_push in ChaffSystem.cpp), and capped
        // so a unit can never be launched.
        Vec2 total = correction / static_cast<f32>(contacts);
        const f32 cap = size * ct.max_push_mult;
        const f32 len2 = math::length_sq(total);
        if (cap > 0.0f && len2 > cap * cap) total *= cap / std::sqrt(len2);
        push_x_[i] = total.x;
        push_y_[i] = total.y;
    }

    for (usize i = 0; i < n; ++i) {
        sw.pos_x[i] += push_x_[i];
        sw.pos_y[i] += push_y_[i];
    }
}

/// The point in the squad's rank that shooter `i` should occupy, given the
/// target it is holding against: the rank is a line perpendicular to the
/// squad's approach (centroid -> target), centred `hold` out from the target,
/// with the members spread along it by rank at the profile's spacing. A
/// shooter with no squad-mates (size 1) simply holds `hold` out.
Vec2 SwarmerSystem::formation_slot(const SwarmerBuffers& sw, usize i, const SwarmerProfile& pr,
                                   Vec2 target_pos, f32 hold) const {
    const Vec2 centroid{squad_cx_[i], squad_cy_[i]};
    Vec2 approach = math::normalize_safe(target_pos - centroid);
    if (approach.x == 0.0f && approach.y == 0.0f) {
        approach = math::normalize_safe(target_pos - Vec2{sw.pos_x[i], sw.pos_y[i]});
        if (approach.x == 0.0f && approach.y == 0.0f) approach = Vec2{1.0f, 0.0f};
    }
    const Vec2 across{-approach.y, approach.x};
    // The rank has to fit inside the standoff or its wings can never engage:
    // squeeze the spacing so the outermost slot sits within 0.9 * hold, and
    // bend the line onto the hold circle so every slot is the same distance
    // from the target rather than the wings hanging further out.
    const f32 wings = static_cast<f32>(squad_size_[i] - 1u);
    const f32 spacing = wings > 0.0f ? math::min(pr.formation_spacing, 1.8f * hold / wings)
                                     : pr.formation_spacing;
    const f32 lateral = (static_cast<f32>(squad_rank_[i]) - wings * 0.5f) * spacing;
    const f32 along = std::sqrt(math::max(hold * hold - lateral * lateral, 0.0f));
    return target_pos - approach * along + across * lateral;
}

/// The LANE WALL (file header). The rank runs across the flow sampled at the
/// squad's centroid -- the way the horde walks through that spot -- and falls
/// back to the approach to the target where the field is silent (a pocket,
/// no field at all), which degrades to formation_slot's line.
///
/// Along the flow the line is anchored on the centroid. It moves only to
/// bring a target that is farther than `hold` out (measured along the flow,
/// either side) back to `hold`; a target closer than that, pressing into the
/// rank, moves nothing. Members correct laterally about the centroid and the
/// lateral offsets sum to zero, so the centroid -- and with it the wall -- is
/// a fixed point of the members chasing their slots.
///
/// Across the flow the rank is spaced to the lane: the clearance at the
/// centroid is the distance to the nearest bank, the outermost slot must sit
/// a body inside it, and the profile's formation_spacing is the ceiling. The
/// whole line is also nudged toward the bank that is farther away (one SDF
/// probe either side, a hill-climb step per tick, bounded by the unit's own
/// speed), which walks a volley released against one bank to the middle
/// where it blocks the most.
Vec2 SwarmerSystem::wall_slot(const SwarmerBuffers& sw, usize i, const SwarmerProfile& pr,
                              Vec2 target_pos, f32 hold, const FlowField* flow,
                              const DistanceField* sdf, f32 dt) const {
    Vec2 centroid{squad_cx_[i], squad_cy_[i]};
    Vec2 along = flow != nullptr ? math::normalize_safe(flow->sample(centroid)) : Vec2{0.0f, 0.0f};
    if (along.x == 0.0f && along.y == 0.0f) {
        along = math::normalize_safe(target_pos - centroid);
        if (along.x == 0.0f && along.y == 0.0f) {
            along = math::normalize_safe(target_pos - Vec2{sw.pos_x[i], sw.pos_y[i]});
            if (along.x == 0.0f && along.y == 0.0f) along = Vec2{1.0f, 0.0f};
        }
    }
    const Vec2 across{-along.y, along.x};

    // Anchor: advance (or fall back) only as far as brings the target to
    // `hold` along the flow. Sign-agnostic, so a wall that has ended up
    // downstream of a horde walking away from it turns and follows.
    const Vec2 to_target = target_pos - centroid;
    const f32 target_along = to_target.x * along.x + to_target.y * along.y;
    const f32 shift = target_along - math::clamp(target_along, -hold, hold);
    centroid += along * shift;

    const f32 body = pr.size * kWallContactFraction;
    const f32 wings = static_cast<f32>(squad_size_[i] - 1u);
    f32 spacing = pr.formation_spacing;
    if (sdf != nullptr && sdf->width() > 0) {
        const f32 clearance = sdf->sample(centroid);
        // Fit the rank inside the nearest bank ...
        if (wings > 0.0f && clearance > body) {
            spacing = math::min(spacing, 2.0f * (clearance - body) / wings);
        }
        // ... and slide it toward the farther one.
        const f32 probe = math::max(body, 0.5f);
        const f32 left = sdf->sample(centroid + across * probe);
        const f32 right = sdf->sample(centroid - across * probe);
        const f32 slope = (left - right) * 0.5f;
        if (slope != 0.0f) {
            const f32 step = math::min(std::fabs(slope), pr.speed * dt);
            centroid += across * (slope > 0.0f ? step : -step);
        }
    }
    // Never closer than touching: a rank crushed into one blob is not a wall.
    spacing = math::max(spacing, 2.0f * body);

    const f32 lateral = (static_cast<f32>(squad_rank_[i]) - wings * 0.5f) * spacing;
    return centroid + across * lateral;
}

} // namespace immune::sim
