// sim/squad/Squads.cpp — implementation of the squad layer declared in Squads.h.
#include "sim/squad/Squads.h"

#include "core/Math.h"
#include "sim/chaff/ChaffBuffers.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace immune::sim {

namespace {

/// Golden-ratio low-discrepancy sequence, the same trick spawn_burst() uses for
/// its phyllotaxis disc. Consecutive squads get lateral offsets that are spread
/// across the path width rather than clustered, without any RNG draw -- which
/// keeps squad placement a pure function of creation order and therefore
/// reproducible under --sim-test.
f32 golden_offset(u32 serial) {
    constexpr f32 kInvGolden = 0.61803398875f;
    const f32 t = std::fmod(static_cast<f32>(serial) * kInvGolden, 1.0f);
    return t - 0.5f;   // [-0.5, 0.5]
}

/// Squared distance from `p` to segment [a,b], plus the parameter of the
/// closest point along it.
f32 seg_dist_sq(Vec2 p, Vec2 a, Vec2 b, f32& out_t) {
    const Vec2 ab = b - a;
    const f32 len_sq = math::length_sq(ab);
    if (len_sq < math::kEpsilon) {
        out_t = 0.0f;
        return math::length_sq(p - a);
    }
    const f32 t = math::clamp(((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / len_sq, 0.0f, 1.0f);
    out_t = t;
    return math::length_sq(p - (a + ab * t));
}

u64 hash_mix(u64 h, u64 v) {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

u64 hash_f32(u64 h, f32 v) {
    // Hash the bit pattern, not the value: two runs must agree exactly, and
    // -0.0f vs +0.0f is a real divergence worth catching.
    u32 bits = 0;
    static_assert(sizeof(bits) == sizeof(v), "f32 must be 4 bytes");
    std::memcpy(&bits, &v, sizeof(bits));
    return hash_mix(h, static_cast<u64>(bits));
}

} // namespace

// ---- SquadPath -------------------------------------------------------------

void SquadPath::rebuild_arc() {
    arc.resize(points.size());
    if (points.empty()) return;
    arc[0] = 0.0f;
    for (usize i = 1; i < points.size(); ++i)
        arc[i] = arc[i - 1] + math::length(points[i] - points[i - 1]);
}

Vec2 SquadPath::point_at(f32 s) const {
    if (points.empty()) return Vec2{0.0f, 0.0f};
    if (points.size() == 1 || s <= 0.0f) return points.front();
    if (s >= length()) return points.back();

    // arc is non-decreasing, so upper_bound finds the segment containing s.
    const auto it = std::upper_bound(arc.begin(), arc.end(), s);
    const usize hi = static_cast<usize>(it - arc.begin());
    const usize lo = hi - 1;
    const f32 span = arc[hi] - arc[lo];
    const f32 t = span > math::kEpsilon ? (s - arc[lo]) / span : 0.0f;
    return math::lerp(points[lo], points[hi], t);
}

Vec2 SquadPath::tangent_at(f32 s) const {
    if (points.size() < 2) return Vec2{0.0f, 0.0f};
    const f32 total = length();
    usize lo = 0;
    if (s >= total) {
        lo = points.size() - 2;
    } else if (s > 0.0f) {
        const auto it = std::upper_bound(arc.begin(), arc.end(), s);
        lo = static_cast<usize>(it - arc.begin()) - 1;
        if (lo + 1 >= points.size()) lo = points.size() - 2;
    }
    return math::normalize_safe(points[lo + 1] - points[lo]);
}

f32 SquadPath::project_near(Vec2 p, f32 hint, f32 window) const {
    if (points.size() < 2) return 0.0f;
    const f32 total = length();
    const f32 lo_s = math::max(0.0f, hint - window);
    const f32 hi_s = math::min(total, hint + window);

    f32 best_s = math::clamp(hint, 0.0f, total);
    f32 best_d2 = math::length_sq(p - point_at(best_s));

    for (usize i = 0; i + 1 < points.size(); ++i) {
        // Skip segments entirely outside the search window. This is what keeps
        // a hairpin's far limb from capturing the projection.
        if (arc[i + 1] < lo_s || arc[i] > hi_s) continue;
        f32 t = 0.0f;
        const f32 d2 = seg_dist_sq(p, points[i], points[i + 1], t);
        if (d2 < best_d2) {
            best_d2 = d2;
            best_s = math::lerp(arc[i], arc[i + 1], t);
        }
    }
    return math::clamp(best_s, lo_s, hi_s);
}

// ---- SquadRegistry ---------------------------------------------------------

void SquadRegistry::set_tuning(const SquadTuning& tuning) {
    tuning_ = tuning;
    // Guard the neighbour-gather invariant at the point of entry rather than
    // trusting every caller: a foreign separation radius past the spatial hash
    // cell size does not error, it silently drops neighbours outside the 3x3
    // scan, which would read as squads randomly failing to repel each other.
    tuning_.foreign_radius_mult = math::max(1.0f, tuning_.foreign_radius_mult);
    tuning_.follow_weight_max = math::clamp(tuning_.follow_weight_max, 0.0f, 1.0f);
    tuning_.follow_ramp = math::max(tuning_.follow_ramp, math::kEpsilon);
    tuning_.min_radius = math::max(tuning_.min_radius, 0.0f);
    if (tuning_.max_squads > kNoSquad) tuning_.max_squads = kNoSquad;
    // A cap below the spawner's own intake size is self-contradictory: the
    // spawner would fill a squad to target_squad_size through accepting(), and
    // every one of them would be born already over the replication cap.
    tuning_.max_squad_size = math::max(tuning_.max_squad_size, tuning_.target_squad_size);

    if (squads_.size() > tuning_.max_squads) squads_.resize(tuning_.max_squads);
}

void SquadRegistry::set_paths(std::vector<SquadPath> paths) {
    paths_ = std::move(paths);
    for (SquadPath& p : paths_)
        if (p.arc.size() != p.points.size()) p.rebuild_arc();

    // Group by lane so a lane's paths are a contiguous range. Stable sort keeps
    // authored order within a lane, which is what makes round-robin assignment
    // reproducible from the level file alone.
    std::stable_sort(paths_.begin(), paths_.end(),
                     [](const SquadPath& a, const SquadPath& b) { return a.lane_id < b.lane_id; });

    lane_ids_.clear();
    lane_first_.clear();
    lane_count_.clear();
    lane_cursor_.clear();
    for (u32 i = 0; i < static_cast<u32>(paths_.size()); ++i) {
        if (lane_ids_.empty() || lane_ids_.back() != paths_[i].lane_id) {
            lane_ids_.push_back(paths_[i].lane_id);
            lane_first_.push_back(i);
            lane_count_.push_back(0);
            lane_cursor_.push_back(0);
        }
        ++lane_count_.back();
    }
    clear();
}

i32 SquadRegistry::lane_index(const std::string& lane_id) const {
    for (usize i = 0; i < lane_ids_.size(); ++i)
        if (lane_ids_[i] == lane_id) return static_cast<i32>(i);
    return -1;
}

void SquadRegistry::lane_path_range(const std::string& lane_id, u32& out_first,
                                    u32& out_count) const {
    const i32 li = lane_index(lane_id);
    if (li < 0) {
        // Unknown lane falls back to the whole path list rather than to nothing,
        // so a spawn point whose lane authored no paths still gets squads
        // instead of silently reverting to the un-grouped horde.
        out_first = 0;
        out_count = static_cast<u32>(paths_.size());
        return;
    }
    out_first = lane_first_[static_cast<usize>(li)];
    out_count = lane_count_[static_cast<usize>(li)];
}

bool SquadRegistry::next_path_for_lane(const std::string& lane_id, u16& out_path_index) {
    if (paths_.empty()) return false;
    const i32 li = lane_index(lane_id);
    if (li < 0) {
        out_path_index = 0;
        return true;
    }
    const usize l = static_cast<usize>(li);
    if (lane_count_[l] == 0) return false;
    out_path_index = static_cast<u16>(lane_first_[l] + (lane_cursor_[l] % lane_count_[l]));
    lane_cursor_[l] = (lane_cursor_[l] + 1) % lane_count_[l];
    return true;
}

u16 SquadRegistry::create_squad(u16 path_index, Vec2 spawn_pos) {
    if (path_index >= paths_.size()) return kNoSquad;

    u16 slot = kNoSquad;
    for (u16 i = 0; i < static_cast<u16>(squads_.size()); ++i) {
        if (!squads_[i].active) { slot = i; break; }
    }
    if (slot == kNoSquad) {
        if (squads_.size() >= tuning_.max_squads) return kNoSquad;   // graceful: caller uses kNoSquad
        slot = static_cast<u16>(squads_.size());
        squads_.emplace_back();
    }

    const SquadPath& path = paths_[path_index];
    Squad& sq = squads_[slot];
    sq = Squad{};
    sq.active = true;
    sq.path_index = path_index;
    // Seed from the spawn point over the whole path: there is no previous
    // position to hint with, so the window is the full length here by design.
    f32 arc = path.project_near(spawn_pos, path.length() * 0.5f, path.length());

    // Then push forward past anything already sitting there, so two squads on
    // one path form a column instead of spawning inside each other. Repeated
    // because pushing clear of one squad can land on the next; bounded by the
    // squad count, and squads on this path are few.
    for (u32 pass = 0; pass < static_cast<u32>(squads_.size()); ++pass) {
        bool moved = false;
        for (u16 i = 0; i < static_cast<u16>(squads_.size()); ++i) {
            if (i == slot || !squads_[i].active || squads_[i].path_index != path_index) continue;
            const f32 gap = squads_[i].arc_pos - arc;
            if (gap < tuning_.spawn_spacing && gap > -tuning_.spawn_spacing) {
                arc = squads_[i].arc_pos + tuning_.spawn_spacing;
                moved = true;
            }
        }
        if (!moved) break;
    }
    sq.arc_pos = math::min(arc, path.length());
    sq.birth_arc = sq.arc_pos;
    sq.lateral = golden_offset(next_serial_) * 2.0f * path.half_width * tuning_.path_lateral_jitter;
    sq.radius = tuning_.min_radius;
    sq.centroid = spawn_pos;
    sq.anchor = path.point_at(sq.arc_pos);
    ++next_serial_;
    ++active_count_;
    return slot;
}

bool SquadRegistry::accepting(u16 id) const {
    if (!alive(id)) return false;
    const Squad& sq = squads_[id];
    if (sq.member_count >= tuning_.target_squad_size) return false;
    return (sq.arc_pos - sq.birth_arc) < tuning_.intake_distance;
}

bool SquadRegistry::can_absorb(u16 id, u32 pending) const {
    if (!alive(id)) return false;
    return squads_[id].member_count + pending < tuning_.max_squad_size;
}

void SquadRegistry::update(const ChaffBuffers& chaff, f32 dt) {
    if (squads_.empty()) return;

    sum_x_.assign(squads_.size(), 0.0f);
    sum_y_.assign(squads_.size(), 0.0f);
    sum_d2_.assign(squads_.size(), 0.0f);
    sum_n_.assign(squads_.size(), 0u);

    // SERIAL and in index order -- see the determinism note in Squads.h.
    const usize n = chaff.count();
    const f32* px = chaff.pos_x.data();
    const f32* py = chaff.pos_y.data();
    const u16* sid = chaff.squad_id.data();
    for (usize i = 0; i < n; ++i) {
        const u16 s = sid[i];
        if (s >= squads_.size()) continue;   // covers kNoSquad
        sum_x_[s] += px[i];
        sum_y_[s] += py[i];
        // Sum of |p|^2, so spread comes out of the same single pass:
        // RMS^2 = mean(|p|^2) - |mean(p)|^2.
        sum_d2_[s] += px[i] * px[i] + py[i] * py[i];
        ++sum_n_[s];
    }

    for (usize i = 0; i < squads_.size(); ++i) {
        Squad& sq = squads_[i];
        if (!sq.active) continue;

        sq.member_count = sum_n_[i];
        if (sq.member_count == 0) {
            if (++sq.empty_ticks >= tuning_.retire_ticks) {
                sq.active = false;
                if (active_count_ > 0) --active_count_;
            }
            continue;
        }
        sq.empty_ticks = 0;

        const f32 inv = 1.0f / static_cast<f32>(sq.member_count);
        sq.centroid = Vec2{sum_x_[i] * inv, sum_y_[i] * inv};
        sq.spread = std::sqrt(math::max(0.0f, sum_d2_[i] * inv -
                                                  math::length_sq(sq.centroid)));

        if (sq.path_index >= paths_.size()) continue;
        const SquadPath& path = paths_[sq.path_index];
        if (!path.valid()) continue;

        sq.radius = math::clamp(tuning_.squad_radius_scale *
                                    std::sqrt(static_cast<f32>(sq.member_count)),
                                tuning_.min_radius, path.half_width);

        // The leash. `max` = never reverses; `min` = cannot teleport forward
        // when a squad is culled down to one leading straggler. Both matter.
        const f32 s = path.project_near(sq.centroid, sq.arc_pos, tuning_.project_window);
        const f32 target = s + tuning_.anchor_lookahead;
        const f32 capped = sq.arc_pos + tuning_.anchor_max_speed * dt;
        sq.arc_pos = math::max(sq.arc_pos, math::min(target, capped));

        const Vec2 tangent = path.tangent_at(sq.arc_pos);
        const Vec2 normal{-tangent.y, tangent.x};
        sq.anchor = path.point_at(sq.arc_pos) + normal * sq.lateral;
    }
}

u64 SquadRegistry::state_hash() const {
    u64 h = 0xcbf29ce484222325ULL;
    for (const Squad& sq : squads_) {
        if (!sq.active) continue;
        h = hash_mix(h, sq.path_index);
        h = hash_f32(h, sq.arc_pos);
        h = hash_f32(h, sq.lateral);
    }
    return h;
}

void SquadRegistry::clear() {
    squads_.clear();
    sum_x_.clear();
    sum_y_.clear();
    sum_d2_.clear();
    sum_n_.clear();
    next_serial_ = 0;
    active_count_ = 0;
    for (u32& c : lane_cursor_) c = 0;
}

} // namespace immune::sim
