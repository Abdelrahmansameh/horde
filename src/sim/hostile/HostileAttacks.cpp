// sim/hostile/HostileAttacks.cpp — the pathogen-vs-friendly pass.
// HostileAttacks.h states the contract; this file is the four walks it makes:
//
//   1. PASSENGERS  one serial walk of the chaff store. Every kLatched agent
//                  re-validates its host, is planted on the host's membrane
//                  (carrying the host's velocity so the renderer's motion
//                  cues agree), and feeds on it. A host that is gone drops
//                  its passenger back into the lane where it stood.
//   2. SWARMERS    every live unit queries the chaff hash once around itself:
//                  aura damage from every bacterium in reach, and a new
//                  passenger from every virus touching it while it has room.
//   3. TOWERS      the same, for every tower SimWorld listed -- and every
//                  scar, which is a BAR host: walked in segments along its
//                  length, measured to its nearest face (bar_* below).
//   4. DEATHS      units at or below zero health are flagged for compaction
//                  and announced to the VFX sink.
//
// Order matters twice. 1 before 2/3 so this tick's passenger count is known
// before the caps are tested; 4 last so a unit that both took aura damage
// and gained a passenger this tick dies exactly once.
#include "sim/hostile/HostileAttacks.h"

#include "core/Math.h"
#include "sim/CombatEvents.h"
#include "sim/flowfield/RuntimeBlock.h"

#include <algorithm>
#include <cmath>

namespace immune::sim {

namespace {

/// Where on a host's membrane a passenger sits, as a unit vector from the
/// host's centre. Hashed off the passenger's own generation so it is stable
/// for the passenger's life and scatters a clump of them round the host
/// rather than stacking them on one side.
Vec2 ring_dir(u32 generation) {
    const u32 h = generation * 2654435761u;
    const f32 a = static_cast<f32>(h >> 8) * (math::kTwoPi / 16777216.0f);
    return Vec2{std::cos(a), std::sin(a)};
}

/// How far a passenger's centre sits from its host's centre: on the
/// membrane, sunk in by a fraction of its own body so it reads as gripping
/// the host rather than hovering beside it.
f32 ring_radius(f32 host_body, f32 passenger_radius) {
    return math::max(host_body + passenger_radius * 0.35f, 0.0f);
}

// ---------------------------------------------------------------------------
// Bar hosts (a scar, sim/scar). Everything is done in the bar's own frame:
// +x along the bar, +y across it, origin at its centre.
// ---------------------------------------------------------------------------

struct BarFrame {
    Vec2 c{0.0f, 0.0f};
    Vec2 he{1.0f, 1.0f};
    f32 cs = 1.0f;
    f32 sn = 0.0f;

    explicit BarFrame(const FriendlyTower& t)
        : c(t.position), he(t.half_extents), cs(std::cos(t.rotation)), sn(std::sin(t.rotation)) {}

    Vec2 to_local(Vec2 p) const {
        const Vec2 d = p - c;
        return Vec2{d.x * cs + d.y * sn, -d.x * sn + d.y * cs};
    }
    Vec2 to_world(Vec2 l) const { return c + Vec2{l.x * cs - l.y * sn, l.x * sn + l.y * cs}; }
    Vec2 dir_to_world(Vec2 l) const { return Vec2{l.x * cs - l.y * sn, l.x * sn + l.y * cs}; }
    f32 perimeter() const { return 4.0f * (he.x + he.y); }
};

/// Squared distance from a local point to the bar's solid; 0 inside.
f32 bar_dist_sq(const BarFrame& b, Vec2 l) {
    const f32 qx = math::max(std::fabs(l.x) - b.he.x, 0.0f);
    const f32 qy = math::max(std::fabs(l.y) - b.he.y, 0.0f);
    return qx * qx + qy * qy;
}

/// The point on the bar's boundary nearest a local point, inside or out.
Vec2 bar_surface_point(const BarFrame& b, Vec2 l) {
    const f32 ax = std::fabs(l.x);
    const f32 ay = std::fabs(l.y);
    if (ax > b.he.x || ay > b.he.y) {
        return Vec2{math::clamp(l.x, -b.he.x, b.he.x), math::clamp(l.y, -b.he.y, b.he.y)};
    }
    // Inside: out through the nearest face. The long faces win ties, so a
    // passenger buried dead centre surfaces on a face and not an end.
    if ((b.he.y - ay) <= (b.he.x - ax)) return Vec2{l.x, l.y >= 0.0f ? b.he.y : -b.he.y};
    return Vec2{l.x >= 0.0f ? b.he.x : -b.he.x, l.y};
}

/// Perimeter parameter in [0,1) of a boundary point. Edge order: the +y face
/// left to right, the +x end top to bottom, the -y face right to left, the
/// -x end bottom to top. A corner belongs to the face it ends.
f32 bar_perimeter_t(const BarFrame& b, Vec2 on) {
    const f32 P = math::max(b.perimeter(), 1e-4f);
    const f32 hx = b.he.x;
    const f32 hy = b.he.y;
    // Snap to whichever face the point actually lies on (it is a boundary
    // point, so at least one coordinate is at its extent).
    const bool on_x_face = std::fabs(std::fabs(on.x) - hx) <= std::fabs(std::fabs(on.y) - hy);
    f32 d = 0.0f;
    if (!on_x_face) {
        d = on.y >= 0.0f ? (on.x + hx)                          // +y face
                         : (2.0f * hx + 2.0f * hy + (hx - on.x)); // -y face
    } else {
        d = on.x >= 0.0f ? (2.0f * hx + (hy - on.y))                        // +x end
                         : (4.0f * hx + 2.0f * hy + (on.y + hy));           // -x end
    }
    f32 t = d / P;
    t -= std::floor(t);
    return t;
}

/// Inverse of bar_perimeter_t: the boundary point at `t` and the outward
/// normal of the face it sits on, both local.
Vec2 bar_point_at(const BarFrame& b, f32 t, Vec2& normal) {
    const f32 P = b.perimeter();
    const f32 hx = b.he.x;
    const f32 hy = b.he.y;
    f32 d = (t - std::floor(t)) * P;
    if (d < 2.0f * hx) {
        normal = Vec2{0.0f, 1.0f};
        return Vec2{-hx + d, hy};
    }
    d -= 2.0f * hx;
    if (d < 2.0f * hy) {
        normal = Vec2{1.0f, 0.0f};
        return Vec2{hx, hy - d};
    }
    d -= 2.0f * hy;
    if (d < 2.0f * hx) {
        normal = Vec2{0.0f, -1.0f};
        return Vec2{hx - d, -hy};
    }
    d -= 2.0f * hx;
    normal = Vec2{-1.0f, 0.0f};
    return Vec2{-hx, -hy + d};
}

/// The perimeter fraction packed into a passenger's host_generation word.
u32 pack_spot(f32 t) {
    t -= std::floor(t);
    return static_cast<u32>(t * 4294967296.0);
}
f32 unpack_spot(u32 code) { return static_cast<f32>(code) * (1.0f / 4294967296.0f); }

/// Fraction of the remaining gap a lunging passenger closes per tick no
/// matter how hard the ease-out has throttled it. The ease curve alone
/// (HostileFamilyParams::latch_ease_power) goes to zero speed at zero
/// distance, so without a floor the last hair of the approach would take
/// forever and the passenger would sit visibly off its host; 0.1 a tick
/// closes 99% of whatever is left in under a second while staying far below
/// the lunge itself, so the ease-out is what the eye sees.
constexpr f32 kLatchSettle = 0.1f;

/// One tick of a passenger's lunge from `p` toward `goal`: full speed until
/// the ease zone, then the power ease-out, then the settle floor. Returns the
/// new position; never overshoots.
Vec2 lunge(Vec2 p, Vec2 goal, const HostileFamilyParams& fp, f32 dt) {
    const Vec2 to = goal - p;
    const f32 d = math::length(to);
    if (d <= 1e-5f) return goal;
    f32 factor = 1.0f;
    if (fp.latch_ease_distance > 0.0f && d < fp.latch_ease_distance) {
        factor = std::pow(d / fp.latch_ease_distance, math::max(fp.latch_ease_power, 0.0f));
    }
    const f32 step = math::max(math::max(fp.latch_speed, 0.0f) * factor * dt, d * kLatchSettle);
    if (step >= d) return goal;
    return p + to * (step / d);
}

/// One capped walk of every hash cell the circle (p, radius) overlaps,
/// calling `visit(j)` for each index until `budget` have been seen. Same job
/// as walk_cells in Swarmers.cpp but for an arbitrary radius: an aura reaches
/// further than one 3x3 neighbourhood of 4-unit cells does.
///
/// CELL ORDER IS NEAREST-FIRST, and that is load-bearing. Cells are visited
/// in rings around the friendly's own cell (ring 0, then the 8 around it,
/// then the 16 around those), CSR order inside a cell. A plain row-major
/// sweep spent the whole budget on the far corner of the query rectangle
/// whenever the crowd was dense -- measured: 600 viruses dropped on a tower
/// produced FOUR latches, because the first 64 agents in row-major order all
/// sat in the top-left cell, out of reach -- so truncation must drop the far
/// cells, never the near ones. Deterministic either way: ring order is a
/// pure function of the query, CSR order of positions.
template <typename Visit>
void walk_circle(const SpatialHash& hash, Vec2 p, f32 radius, u32 budget, Visit&& visit) {
    const IVec2 dims = hash.grid_dims();
    if (dims.x <= 0 || dims.y <= 0) return;
    const IVec2 c = hash.cell_coord(p);
    const IVec2 lo = hash.cell_coord(p - Vec2{radius, radius});
    const IVec2 hi = hash.cell_coord(p + Vec2{radius, radius});
    const i32 reach = math::max(math::max(c.x - lo.x, hi.x - c.x), math::max(c.y - lo.y, hi.y - c.y));
    const u32* indices = hash.indices();
    const u32 indexed = static_cast<u32>(hash.indexed_count());
    u32 visited = 0;
    const auto walk_cell = [&](i32 cx, i32 cy) {
        if (cx < lo.x || cx > hi.x || cy < lo.y || cy > hi.y) return true;
        if (cx < 0 || cy < 0 || cx >= dims.x || cy >= dims.y) return true;
        u32 begin, end;
        hash.cell_range(static_cast<u32>(cy) * static_cast<u32>(dims.x) + static_cast<u32>(cx), begin, end);
        if (end > indexed) end = indexed;
        for (u32 k = begin; k < end; ++k) {
            if (visited++ >= budget) return false;
            visit(indices[k]);
        }
        return true;
    };
    if (!walk_cell(c.x, c.y)) return;
    for (i32 r = 1; r <= reach; ++r) {
        // The ring's top and bottom rows, then its left and right columns
        // (corners belong to the rows).
        for (i32 cx = c.x - r; cx <= c.x + r; ++cx) {
            if (!walk_cell(cx, c.y - r)) return;
            if (!walk_cell(cx, c.y + r)) return;
        }
        for (i32 cy = c.y - r + 1; cy <= c.y + r - 1; ++cy) {
            if (!walk_cell(c.x - r, cy)) return;
            if (!walk_cell(c.x + r, cy)) return;
        }
    }
}

/// A pathogen this pass will consider at all: alive, not already dying, not
/// burrowed (under the tissue: it can neither bite nor burn), and not already
/// riding something.
bool attacker(const ChaffBuffers& chaff, u32 j) {
    if (j >= chaff.count()) return false;
    const u8 f = chaff.flags[j];
    if ((f & chaff_flags::kAlive) == 0) return false;
    if ((f & chaff_flags::kPendingKill) != 0) return false;
    if ((f & (chaff_flags::kHidden | chaff_flags::kLatched)) != 0) return false;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Tower list
// ---------------------------------------------------------------------------

void FriendlyTowerList::sort() {
    std::vector<usize> order(items.size());
    for (usize i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [this](usize a, usize b) { return items[a].id.value < items[b].id.value; });
    std::vector<FriendlyTower> sorted_items;
    std::vector<f32> sorted_damage;
    std::vector<u32> sorted_passengers;
    sorted_items.reserve(items.size());
    sorted_damage.reserve(items.size());
    sorted_passengers.reserve(items.size());
    for (const usize i : order) {
        sorted_items.push_back(items[i]);
        sorted_damage.push_back(damage[i]);
        sorted_passengers.push_back(passengers[i]);
    }
    items.swap(sorted_items);
    damage.swap(sorted_damage);
    passengers.swap(sorted_passengers);
}

usize FriendlyTowerList::find(EntityId id) const {
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

// ---------------------------------------------------------------------------
// The pass
// ---------------------------------------------------------------------------

void HostileSystem::set_chaff_radii(const f32* radii, usize count) {
    if (radii == nullptr) return;
    const usize n = count < kFamilyCount ? count : kFamilyCount;
    for (usize f = 0; f < n; ++f) chaff_radius_[f] = math::max(radii[f], 0.0f);
}

HostileStats HostileSystem::update(ChaffBuffers& chaff, const SpatialHash& hash,
                                   SwarmerBuffers& sw, FriendlyTowerList& towers,
                                   const TissueMask& mask, f32 dt, CombatEventSink* events) {
    HostileStats stats;
    const usize chaff_n = chaff.count();
    const usize sw_n = sw.count();

    if (swarmer_passengers_.size() < sw.capacity()) swarmer_passengers_.resize(sw.capacity());
    for (usize i = 0; i < sw_n; ++i) swarmer_passengers_[i] = 0u;
    for (usize k = 0; k < towers.items.size(); ++k) towers.passengers[k] = 0u;

    // Drops a passenger back into the lane where it stands. Velocity is
    // zeroed too: it was carrying its host's, and its host is gone.
    const auto release = [&](usize i) {
        chaff.flags[i] &= static_cast<u8>(~chaff_flags::kLatched);
        chaff.host_kind[i] = host_kind::kNone;
        chaff.host_generation[i] = 0u;
        chaff.host_index[i] = 0u;
        chaff.latch_heading[i] = 0.0f;
        chaff.vel_x[i] = 0.0f;
        chaff.vel_y[i] = 0.0f;
        ++stats.released;
    };

    // Switched off (or dt 0) means nobody is latched: anything still holding
    // a host from before the switch is let go, so a gym toggle cannot leave
    // a virus frozen on a tower for the rest of the run.
    if (!tuning_.enabled || dt <= 0.0f) {
        for (usize i = 0; i < chaff_n; ++i) {
            if ((chaff.flags[i] & chaff_flags::kLatched) != 0) release(i);
        }
        last_ = stats;
        return last_;
    }

    // ---- 1. PASSENGERS ----------------------------------------------------
    for (usize i = 0; i < chaff_n; ++i) {
        const u8 flags = chaff.flags[i];
        if ((flags & chaff_flags::kLatched) == 0) continue;
        // Dying this tick: leave it where it is for the death pop, and do not
        // let it feed. Compaction takes it after this pass.
        if ((flags & chaff_flags::kPendingKill) != 0) continue;

        const u32 fam = chaff.family[i] < kFamilyCount ? chaff.family[i] : 0u;
        const HostileFamilyParams& fp = tuning_.family[fam];
        // A family that no longer latches (a hot reload zeroed it) lets go.
        if (fp.latch_dps <= 0.0f) { release(i); continue; }
        const f32 bite = fp.latch_dps * dt;

        Vec2 host_pos{0.0f, 0.0f};
        Vec2 host_vel{0.0f, 0.0f};
        f32 host_body = 0.0f;
        const FriendlyTower* bar = nullptr;
        switch (chaff.host_kind[i]) {
        case host_kind::kSwarmer: {
            const u32 idx = chaff.host_index[i];
            if (!sw.alive_at(idx, chaff.host_generation[i]) ||
                (sw.flags[idx] & swarmer_flags::kPendingKill) != 0) {
                release(i);
                continue;
            }
            host_pos = Vec2{sw.pos_x[idx], sw.pos_y[idx]};
            host_vel = Vec2{sw.vel_x[idx], sw.vel_y[idx]};
            host_body = sw.profile_of(idx).size * kWallContactFraction;
            ++swarmer_passengers_[idx];
            sw.health[idx] -= bite;
            stats.swarmer_damage += bite;
            break;
        }
        case host_kind::kTower: {
            const usize k = towers.find(EntityId{chaff.host_index[i]});
            if (k == FriendlyTowerList::npos) {
                release(i);
                continue;
            }
            host_pos = towers.items[k].position;
            host_body = towers.items[k].radius;
            if (towers.items[k].is_bar()) bar = &towers.items[k];
            ++towers.passengers[k];
            towers.damage[k] += bite;
            stats.tower_damage += bite;
            break;
        }
        default:
            release(i);
            continue;
        }

        // The spot on the membrane. A disc's is a direction from its centre
        // hashed off the passenger's generation; a bar's is the perimeter
        // fraction the latch stored, on the face it grabbed, sunk in by the
        // same fraction of its own body.
        //
        // `inward` is the membrane normal there, pointing into the host: the
        // cosmetic heading the renderer turns the sprite by so its latch
        // throb pumps toward the cell it is eating (ChaffBuffers::latch_heading).
        Vec2 goal;
        Vec2 inward;
        if (bar != nullptr) {
            const BarFrame frame(*bar);
            Vec2 n;
            const Vec2 on = bar_point_at(frame, unpack_spot(chaff.host_generation[i]), n);
            goal = frame.to_world(on + n * (chaff_radius_[fam] * 0.35f));
            inward = frame.dir_to_world(n * -1.0f);
        } else {
            const Vec2 dir = ring_dir(chaff.generation[i]);
            const f32 ring = ring_radius(host_body, chaff_radius_[fam]);
            goal = host_pos + dir * ring;
            inward = dir * -1.0f;
        }
        // The membrane spot is a fixed offset from the host and knows nothing
        // about the tissue/vessel border; a host parked near that border can
        // put it on the wrong side. The host's own position is always
        // walkable ground (it would not have been placed otherwise), so pull
        // the spot back onto it rather than let a passenger ride out past
        // the edge of the map.
        contain_to_walkable(mask, goal, host_pos);
        chaff.latch_heading[i] = std::atan2(inward.y, inward.x);
        // Carry, then lunge (RIDING in the header). The stored velocity is
        // the host's from last tick, so p + v*dt is where the host's motion
        // took the passenger's spot; the lunge closes what is left. The
        // kernel zeroed nothing here: a latched agent's velocity is ours.
        const Vec2 carried{chaff.pos_x[i] + chaff.vel_x[i] * dt, chaff.pos_y[i] + chaff.vel_y[i] * dt};
        const Vec2 next = lunge(carried, goal, fp, dt);
        chaff.pos_x[i] = next.x;
        chaff.pos_y[i] = next.y;
        chaff.vel_x[i] = host_vel.x;
        chaff.vel_y[i] = host_vel.y;
        ++stats.latched;
    }

    // The furthest any family reaches from a friendly's membrane, so one
    // query per friendly covers every attack. Computed once per tick: the
    // table is hot-reloadable.
    f32 reach_past_body = 0.0f;
    for (u32 f = 0; f < kFamilyCount; ++f) {
        const HostileFamilyParams& fp = tuning_.family[f];
        if (fp.aura_dps > 0.0f) reach_past_body = math::max(reach_past_body, fp.aura_radius);
        if (fp.latch_dps > 0.0f) {
            reach_past_body = math::max(reach_past_body, chaff_radius_[f] + fp.latch_reach);
        }
    }
    if (reach_past_body <= 0.0f) {
        last_ = stats;
        return last_;
    }

    u32 latch_events = 0;
    // Grabs chaff j onto a host at `host_pos` with body `host_body`, and
    // plants it there on the spot so it never spends a tick at the point of
    // contact with its velocity frozen mid-stride.
    // `contact` / `outward` are where on the host the grab happened and the
    // membrane normal there, for the event; a disc host derives both from the
    // passenger's ring direction, a bar host from its nearest face.
    const auto latch = [&](u32 j, u8 kind, u32 index, u32 generation, Vec2 host_pos, Vec2 host_vel,
                           Vec2 contact, Vec2 outward, TowerType source, u16 visual_id) {
        chaff.flags[j] |= chaff_flags::kLatched;
        chaff.host_kind[j] = kind;
        chaff.host_index[j] = index;
        chaff.host_generation[j] = generation;
        const u32 fam = chaff.family[j] < kFamilyCount ? chaff.family[j] : 0u;
        const Vec2 dir = outward;
        // Not moved: the grab happens where the touch happened, and the
        // lunge to the spot on the membrane starts next tick (pass 1). The
        // velocity becomes the host's so the carry there is right from the
        // first tick, and the sprite faces its host from the first frame.
        chaff.vel_x[j] = host_vel.x;
        chaff.vel_y[j] = host_vel.y;
        chaff.latch_heading[j] = std::atan2(-outward.y, -outward.x);
        ++stats.latches_new;
        ++stats.latched;
        if (events != nullptr && latch_events < tuning_.max_latch_events) {
            ++latch_events;
            CombatEvent e;
            e.type = CombatEventType::PathogenLatch;
            e.source = source;
            e.visual_id = visual_id;
            e.target_family = static_cast<PathogenFamily>(fam);
            e.origin = contact;
            e.secondary = host_pos;
            e.direction = dir;
            e.radius = chaff_radius_[fam];
            e.magnitude = 1.0f;
            events->push(e);
        }
    };

    // ---- 2. SWARMERS ------------------------------------------------------
    for (usize i = 0; i < sw_n; ++i) {
        if ((sw.flags[i] & swarmer_flags::kPendingKill) != 0) continue;
        const SwarmerProfile& pr = sw.profile_of(i);
        const Vec2 p{sw.pos_x[i], sw.pos_y[i]};
        const Vec2 v{sw.vel_x[i], sw.vel_y[i]};
        const f32 body = pr.size * kWallContactFraction;
        // A Latch unit is a granule already sitting on a pathogen; there is
        // nothing there for a virus to climb onto, and a granule that is both
        // riding and ridden would drag its pair across the map (each snaps to
        // the other every tick). Latch units still burn in an aura.
        const bool latchable = pr.kind != SwarmerKind::Latch;

        f32 incoming = 0.0f;
        walk_circle(hash, p, reach_past_body + body, tuning_.max_attackers, [&](u32 j) {
            if (!attacker(chaff, j)) return;
            const u32 fam = chaff.family[j] < kFamilyCount ? chaff.family[j] : 0u;
            const HostileFamilyParams& fp = tuning_.family[fam];
            const f32 dx = chaff.pos_x[j] - p.x;
            const f32 dy = chaff.pos_y[j] - p.y;
            const f32 d2 = dx * dx + dy * dy;
            if (fp.aura_dps > 0.0f) {
                const f32 r = fp.aura_radius + body;
                if (d2 < r * r) {
                    incoming += fp.aura_dps * dt;
                    ++stats.aura_hits;
                }
            }
            if (fp.latch_dps > 0.0f && latchable && swarmer_passengers_[i] < fp.latch_cap_swarmer) {
                const f32 r = chaff_radius_[fam] + body + fp.latch_reach;
                if (d2 < r * r) {
                    ++swarmer_passengers_[i];
                    const Vec2 dir = ring_dir(chaff.generation[j]);
                    latch(j, host_kind::kSwarmer, static_cast<u32>(i), sw.generation[i], p, v,
                          p + dir * body, dir, pr.source,
                          static_cast<u16>(sw.visual_id[i] | kSwarmerEventBit));
                }
            }
        });
        if (incoming > 0.0f) {
            sw.health[i] -= incoming;
            stats.swarmer_damage += incoming;
        }
    }

    // ---- 3. TOWERS --------------------------------------------------------
    for (usize k = 0; k < towers.items.size(); ++k) {
        const FriendlyTower& t = towers.items[k];
        const Vec2 p = t.position;
        const f32 body = t.radius;
        f32 incoming = 0.0f;

        if (t.is_bar()) {
            // A scar. One circle around the centre would spend the whole walk
            // budget on the crowd pressed against the middle of the wall and
            // never see its ends, so the bar is walked in segments along its
            // length, each with the full budget, and a pathogen inside two
            // overlapping segments is stamped so it counts once.
            const BarFrame frame(t);
            const f32 cell = math::max(hash.cell_size(), 1e-3f);
            const u32 segments = math::max(1u, static_cast<u32>(std::ceil(2.0f * frame.he.x / cell)));
            const f32 seg_half = frame.he.x / static_cast<f32>(segments);
            if (visit_stamp_.size() < chaff.capacity()) visit_stamp_.assign(chaff.capacity(), 0u);
            if (++visit_gen_ == 0u) {   // wrapped: every stamp is stale
                std::fill(visit_stamp_.begin(), visit_stamp_.end(), 0u);
                visit_gen_ = 1u;
            }
            for (u32 sgi = 0; sgi < segments; ++sgi) {
                const f32 lx = -frame.he.x + (static_cast<f32>(sgi) + 0.5f) * 2.0f * seg_half;
                const Vec2 centre = frame.to_world(Vec2{lx, 0.0f});
                const f32 radius = seg_half + frame.he.y + reach_past_body;
                walk_circle(hash, centre, radius, tuning_.max_attackers, [&](u32 j) {
                    if (j >= visit_stamp_.size() || visit_stamp_[j] == visit_gen_) return;
                    visit_stamp_[j] = visit_gen_;
                    if (!attacker(chaff, j)) return;
                    const u32 fam = chaff.family[j] < kFamilyCount ? chaff.family[j] : 0u;
                    const HostileFamilyParams& fp = tuning_.family[fam];
                    const Vec2 l = frame.to_local(Vec2{chaff.pos_x[j], chaff.pos_y[j]});
                    const f32 d2 = bar_dist_sq(frame, l);
                    if (fp.aura_dps > 0.0f) {
                        if (d2 < fp.aura_radius * fp.aura_radius) {
                            incoming += fp.aura_dps * dt;
                            ++stats.aura_hits;
                        }
                    }
                    if (fp.latch_dps > 0.0f && towers.passengers[k] < fp.latch_cap_scar) {
                        const f32 r = chaff_radius_[fam] + fp.latch_reach;
                        if (d2 < r * r) {
                            // Grab the face it touched, a little to one side
                            // so a queue of passengers spreads along the wall
                            // rather than stacking on one point.
                            const Vec2 on = bar_surface_point(frame, l);
                            const f32 jitter = (static_cast<f32>((chaff.generation[j] * 2654435761u) >> 8) *
                                                    (2.0f / 16777216.0f) - 1.0f) *
                                               chaff_radius_[fam];
                            const f32 spot = bar_perimeter_t(frame, on) + jitter / frame.perimeter();
                            Vec2 n;
                            const Vec2 at = bar_point_at(frame, spot, n);
                            ++towers.passengers[k];
                            latch(j, host_kind::kTower, t.id.value, pack_spot(spot), p, Vec2{0.0f, 0.0f},
                                  frame.to_world(at), frame.dir_to_world(n), t.type, t.visual_id);
                        }
                    }
                });
            }
            if (incoming > 0.0f) {
                towers.damage[k] += incoming;
                stats.tower_damage += incoming;
            }
            continue;
        }

        walk_circle(hash, p, reach_past_body + body, tuning_.max_attackers, [&](u32 j) {
            if (!attacker(chaff, j)) return;
            const u32 fam = chaff.family[j] < kFamilyCount ? chaff.family[j] : 0u;
            const HostileFamilyParams& fp = tuning_.family[fam];
            const f32 dx = chaff.pos_x[j] - p.x;
            const f32 dy = chaff.pos_y[j] - p.y;
            const f32 d2 = dx * dx + dy * dy;
            if (fp.aura_dps > 0.0f) {
                const f32 r = fp.aura_radius + body;
                if (d2 < r * r) {
                    incoming += fp.aura_dps * dt;
                    ++stats.aura_hits;
                }
            }
            if (fp.latch_dps > 0.0f && towers.passengers[k] < fp.latch_cap_tower) {
                const f32 r = chaff_radius_[fam] + body + fp.latch_reach;
                if (d2 < r * r) {
                    ++towers.passengers[k];
                    const Vec2 dir = ring_dir(chaff.generation[j]);
                    latch(j, host_kind::kTower, t.id.value, 0u, p, Vec2{0.0f, 0.0f}, p + dir * body, dir,
                          t.type, t.visual_id);
                }
            }
        });
        if (incoming > 0.0f) {
            towers.damage[k] += incoming;
            stats.tower_damage += incoming;
        }
    }

    // ---- 4. DEATHS --------------------------------------------------------
    for (usize i = 0; i < sw_n; ++i) {
        if ((sw.flags[i] & swarmer_flags::kPendingKill) != 0) continue;
        if (sw.health[i] > 0.0f) continue;
        sw.kill(i);
        ++stats.swarmers_killed;
        if (events != nullptr) {
            const SwarmerProfile& pr = sw.profile_of(i);
            CombatEvent e;
            e.type = CombatEventType::SwarmerDeath;
            e.source = pr.source;
            e.visual_id = static_cast<u16>(sw.visual_id[i] | kSwarmerEventBit);
            e.target_family = PathogenFamily::Count;
            e.origin = Vec2{sw.pos_x[i], sw.pos_y[i]};
            e.secondary = e.origin;
            e.direction = math::normalize_safe(Vec2{sw.vel_x[i], sw.vel_y[i]});
            if (e.direction.x == 0.0f && e.direction.y == 0.0f) e.direction = Vec2{1.0f, 0.0f};
            e.radius = pr.size;
            e.magnitude = pr.max_health;
            events->push(e);
        }
    }

    last_ = stats;
    return last_;
}

} // namespace immune::sim
